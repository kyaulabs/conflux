// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedScratchSource.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace conflux::audio {
namespace {
constexpr std::uint64_t frameBytes = 2 * sizeof(float);
constexpr std::size_t readBlock = 4096;
constexpr unsigned dirtyBit = 4, indexMask = 3;
constexpr std::size_t phases = 1024, speedBanks = 57;
constexpr std::size_t taps = PreparedScratchSource::filterTaps;
constexpr double rateSlew = PreparedScratchSource::maximumSpeed / 240.0;
constexpr unsigned motionEnvelopeFrames = 240;
constexpr auto maxSerial = std::numeric_limits<std::uint64_t>::max() / 2;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);

bool sameFile(const struct stat &a, const struct stat &b) noexcept {
    return S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

// Constructed once OFF callback, shared read-only thereafter. The conservative
// 1/8-speed cutoff bins avoid using a too-wide passband when decimating. Linear
// interpolation between 1024 phases avoids a fractional-phase staircase.
struct Filters {
    std::vector<float> coefficients;
    Filters() : coefficients(speedBanks * (phases + 1) * taps) {
        // The phase/tap window is shared across every speed bank.
        std::vector<double> windows((phases + 1) * taps);
        for (std::size_t phase = 0; phase <= phases; ++phase) {
            for (std::size_t tap = 0; tap < taps; ++tap) {
                const double distance = double(tap) - 31.0 - double(phase) / phases;
                if (std::abs(distance) >= 32.0)
                    continue;
                const double angle = std::numbers::pi * distance / 32.0;
                windows[phase * taps + tap] = .42 + .5 * std::cos(angle) + .08 * std::cos(2 * angle);
            }
        }
        for (std::size_t bank = 0; bank < speedBanks; ++bank) {
            const double cutoff = 1.0 / (1.0 + double(bank) / 8.0);
            for (std::size_t phase = 0; phase <= phases; ++phase) {
                std::array<double, taps> row{};
                double sum = 0;
                for (std::size_t tap = 0; tap < taps; ++tap) {
                    const double distance = double(tap) - 31.0 - double(phase) / phases;
                    if (std::abs(distance) >= 32.0)
                        continue;
                    const double argument = std::numbers::pi * cutoff * distance;
                    row[tap] = cutoff * (argument == 0 ? 1 : std::sin(argument) / argument) * windows[phase * taps + tap];
                    sum += row[tap];
                }
                if (!std::isfinite(sum) || sum <= 0)
                    throw std::runtime_error("Invalid scratch sinc table");
                auto *destination = coefficients.data() + (bank * (phases + 1) + phase) * taps;
                for (std::size_t tap = 0; tap < taps; ++tap)
                    destination[tap] = float(row[tap] / sum);
            }
        }
    }
    const float *row(std::size_t bank, std::size_t phase) const noexcept {
        return coefficients.data() + (bank * (phases + 1) + phase) * taps;
    }
};
const Filters &filters() {
    static const Filters value;
    return value;
}
} // namespace

class PreparedScratchSource::Impl {
  public:
    struct Window {
        std::array<std::vector<float>, 4> leaves;
        std::uint64_t epoch = 0, first = 0, count = 0;
        int error = 0;
    };
    struct Request {
        std::uint64_t serial = 0, epoch = 0, first = 0;
    };

    Impl(std::span<const int> fds, std::uint64_t frames)
        : frames_(frames), leafCount_(fds.size()), filters_(filters()) {
        if ((leafCount_ != 1 && leafCount_ != 4) || !frames_ || frames_ > (std::uint64_t(1) << 53) ||
            frames_ > std::uint64_t(std::numeric_limits<off_t>::max()) / frameBytes)
            throw std::invalid_argument("Scratch requires one or four exact nonempty addressable PCM leaves");
        try {
            for (std::size_t leaf = 0; leaf < leafCount_; ++leaf) {
                fds_[leaf] = fcntl(fds[leaf], F_DUPFD_CLOEXEC, 0);
                if (fds_[leaf] < 0)
                    throw std::system_error(errno, std::generic_category(), "Duplicate scratch PCM");
                if (fstat(fds_[leaf], &identities_[leaf]) != 0)
                    throw std::system_error(errno, std::generic_category(), "Inspect scratch PCM");
                const int flags = fcntl(fds_[leaf], F_GETFL);
                if (flags < 0)
                    throw std::system_error(errno, std::generic_category(), "Inspect scratch FD access");
                if (!S_ISREG(identities_[leaf].st_mode) || identities_[leaf].st_size < 0 ||
                    std::uint64_t(identities_[leaf].st_size) != frames_ * frameBytes ||
                    (flags & O_ACCMODE) == O_WRONLY || (flags & O_PATH))
                    throw std::invalid_argument("Scratch requires readable exact regular stereo F32 geometry");
            }
            for (auto &window : windows_) {
                window = std::make_unique<Window>();
                for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
                    window->leaves[leaf].resize(windowFrames * 2);
            }
            resident_.store(windowCount * leafCount_ * windowFrames * frameBytes, std::memory_order_relaxed);
            publishRequest(0, true);
            worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        } catch (...) {
            closeFiles();
            throw;
        }
    }
    ~Impl() { stop(); }

    bool validFrame(std::uint64_t frame) const noexcept { return frame <= frames_; }
    bool relocate(std::uint64_t frame) noexcept {
        if (stopped_ || !validFrame(frame))
            return false;
        if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            ownerError_ = EOVERFLOW;
            active_ = false;
            return false;
        }
        ++epoch_;
        active_ = false;
        position_ = target_ = double(frame);
        previousDisplacement_ = 0;
        inputDirection_ = 0;
        speed_ = requestedSpeed_ = 0;
        motionEnvelope_ = 0;
        lastMotionSample_ = {};
        missingWindow_ = false;
        ownerError_ = 0;
        return publishRequest(firstFor(position_), true);
    }
    bool follow(std::uint64_t frame) noexcept {
        if (stopped_ || active_ || !validFrame(frame) || ownerError_)
            return false;
        position_ = double(frame);
        missingWindow_ = false;
        poll();
        prefetch();
        return !ownerError_;
    }
    bool begin(std::uint64_t frame) noexcept {
        if (stopped_ || active_ || !validFrame(frame))
            return false;
        poll();
        if (ownerError_)
            return false;
        position_ = target_ = double(frame);
        previousDisplacement_ = 0;
        inputDirection_ = 0;
        speed_ = requestedSpeed_ = 0;
        motionEnvelope_ = 0;
        lastMotionSample_ = {};
        missingWindow_ = false;
        lastMove_ = std::chrono::steady_clock::now();
        active_ = true;
        prefetch();
        return !ownerError_;
    }
    bool move(std::int64_t displacement, double seconds) noexcept {
        if (stopped_ || !active_ || ownerError_ || !std::isfinite(seconds) || seconds <= 0 || seconds > 1)
            return false;
        // Cast BEFORE subtracting: a legal INT64_MIN -> INT64_MAX gesture delta
        // exceeds int64_t. Clamp the wide rate before converting to double.
        const long double delta = static_cast<long double>(displacement) - previousDisplacement_;
        const int direction = (delta > 0) - (delta < 0);
        const bool reversal = direction && inputDirection_ && direction != inputDirection_;
        const long double requested = static_cast<long double>(reversal ? position_ : target_) + delta;
        target_ = double(std::clamp(requested, 0.L, static_cast<long double>(frames_)));
        previousDisplacement_ = displacement;
        if (direction)
            inputDirection_ = direction;
        const auto rate = delta / (static_cast<long double>(sampleRate) * seconds);
        requestedSpeed_ =
            double(std::clamp(rate, -static_cast<long double>(maximumSpeed), static_cast<long double>(maximumSpeed)));
        lastMove_ = std::chrono::steady_clock::now();
        return true;
    }
    void end() noexcept {
        active_ = false;
        speed_ = requestedSpeed_ = 0;
        motionEnvelope_ = 0;
        lastMotionSample_ = {};
        target_ = position_;
        missingWindow_ = false;
    }

    // Only this owner changes front_. A single atomic middle index+dirty bit
    // transfers a whole immutable window. Producer never owns front, consumer
    // never owns back; no refcount/destruction or allocation occurs here.
    void poll() noexcept {
        if (stopped_)
            return;
        if (middle_.load(std::memory_order_acquire) & dirtyBit)
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & indexMask;
        const auto &window = *windows_[front_];
        if (window.epoch == epoch_ && window.error)
            ownerError_ = window.error;
    }
    std::uint64_t firstFor(double position) const noexcept {
        if (frames_ <= windowFrames)
            return 0;
        constexpr std::uint64_t quantum = windowFrames / 4;
        const auto center = std::min(std::uint64_t(position), frames_ - 1);
        const auto before = center > windowFrames / 2 ? center - windowFrames / 2 : 0;
        return std::min((before / quantum) * quantum, frames_ - windowFrames);
    }
    bool covers(double position) const noexcept {
        const auto &window = *windows_[front_];
        if (window.epoch != epoch_ || window.error || !window.count)
            return false;
        const auto center = std::min(std::uint64_t(position), frames_ - 1);
        const auto low = center > 31 ? center - 31 : 0;
        const auto high = std::min(frames_ - 1, center + 32);
        return window.first <= low && window.first + window.count > high;
    }
    void prefetch() noexcept {
        if (ownerError_ || stopped_)
            return;
        const auto &window = *windows_[front_];
        const auto desired = firstFor(position_);
        constexpr std::uint64_t margin = windowFrames / 4;
        const auto frame = std::min(std::uint64_t(position_), frames_ - 1);
        const bool nearEdge = window.epoch != epoch_ || !window.count ||
                              (window.first != 0 && frame < window.first + margin) ||
                              (window.first + window.count != frames_ && frame + margin >= window.first + window.count);
        if (nearEdge)
            publishRequest(desired, false);
    }
    bool publishRequest(std::uint64_t first, bool force) noexcept {
        if (!force && requestedEpoch_ == epoch_ && requestedFirst_ == first)
            return true;
        if (serial_ == maxSerial) {
            ownerError_ = EOVERFLOW;
            active_ = false;
            return false;
        }
        ++serial_;
        requestedEpoch_ = epoch_;
        requestedFirst_ = first;
        requestVersion_.store(serial_ * 2 - 1, std::memory_order_seq_cst);
        requestEpoch_.store(epoch_, std::memory_order_seq_cst);
        requestFirst_.store(first, std::memory_order_seq_cst);
        requestVersion_.store(serial_ * 2, std::memory_order_seq_cst);
        return true;
    }

    Read read(std::size_t frames) noexcept {
        Read result;
        result.nominalPosition = position_;
        if (!frames || frames > maxReadFrames || stopped_ || !active_)
            return result;
        poll();
        prefetch();
        if (ownerError_) {
            result.failed = true;
            return result;
        }
        missingWindow_ = false;
        const bool stale = std::chrono::steady_clock::now() - lastMove_ >= motionTimeout;
        const auto &window = *windows_[front_];
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const double remaining = target_ - position_;
            double desired = stale ? 0 : requestedSpeed_;
            if (remaining == 0)
                desired = 0;
            const double nextSpeed = speed_ + std::clamp(desired - speed_, -rateSlew, rateSlew);
            double step = nextSpeed;
            // A reversal decelerates in place rather than moving away from the
            // authoritative platter target. No overshoot/positional seeking.
            if ((step > 0 && remaining <= 0) || (step < 0 && remaining >= 0))
                step = 0;
            if (std::abs(step) > std::abs(remaining))
                step = remaining;
            if (step == 0) {
                speed_ = nextSpeed;
                // A stopped cursor must not repeat full-amplitude DC or jump
                // from its last track sample directly to silence. Release the
                // shared motion gate without reading or advancing the source.
                if (motionEnvelope_)
                    --motionEnvelope_;
                const double gain = double(motionEnvelope_) / motionEnvelopeFrames;
                for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
                    for (std::size_t channel = 0; channel < 2; ++channel)
                        output_[leaf][frame * 2 + channel] =
                            motionEnvelope_ ? float(double(lastMotionSample_[leaf][channel]) * gain) : 0.F;
            } else {
                const double nextPosition = std::clamp(position_ + step, 0.0, double(frames_));
                const double samplePosition = step < 0 ? nextPosition : position_;
                if (!covers(samplePosition)) {
                    missingWindow_ = true;
                    prefetch();
                    break;
                }
                interpolate(window, samplePosition, std::abs(step), frame);
                if (motionEnvelope_ < motionEnvelopeFrames)
                    ++motionEnvelope_;
                const double gain = double(motionEnvelope_) / motionEnvelopeFrames;
                for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
                    for (std::size_t channel = 0; channel < 2; ++channel) {
                        auto &sample = output_[leaf][frame * 2 + channel];
                        lastMotionSample_[leaf][channel] = sample;
                        // Leave sustained unity-rate PCM bits untouched.
                        if (motionEnvelope_ != motionEnvelopeFrames)
                            sample = float(double(sample) * gain);
                    }
                position_ = nextPosition;
                speed_ = nextSpeed;
            }
            ++result.frames;
        }
        prefetch();
        result.nominalPosition = position_;
        for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
            result.leaves[leaf] = std::span(output_[leaf]).first(result.frames * 2);
        return result;
    }

    void interpolate(const Window &window, double position, double speed, std::size_t outputFrame) noexcept {
        const auto integer = std::uint64_t(position);
        const double fraction = position - double(integer);
        // Exact unit-rate integer samples need no reconstruction/filtering.
        if (speed == 1 && fraction == 0) {
            for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    output_[leaf][outputFrame * 2 + channel] =
                        window.leaves[leaf][std::size_t(integer - window.first) * 2 + channel];
            return;
        }
        const auto bank = std::size_t(std::clamp(std::ceil((std::max(1.0, speed) - 1) * 8), 0.0, 56.0));
        const double phase = fraction * phases;
        const auto phaseIndex = std::min(std::size_t(phase), phases - 1);
        const double blend = phase - double(phaseIndex);
        const auto *a = filters_.row(bank, phaseIndex);
        const auto *b = filters_.row(bank, phaseIndex + 1);
        std::array<std::array<double, 2>, 4> sums{};
        for (std::size_t tap = 0; tap < taps; ++tap) {
            const auto source = static_cast<std::int64_t>(integer) + static_cast<std::int64_t>(tap) - 31;
            if (source < 0 || std::uint64_t(source) >= frames_)
                continue; // Known zero extension at file boundaries.
            const double coefficient = double(a[tap]) + blend * (double(b[tap]) - a[tap]);
            const auto offset = std::size_t(std::uint64_t(source) - window.first) * 2;
            for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    sums[leaf][channel] += coefficient * window.leaves[leaf][offset + channel];
        }
        constexpr double limit = std::numeric_limits<float>::max();
        for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
            for (std::size_t channel = 0; channel < 2; ++channel)
                output_[leaf][outputFrame * 2 + channel] = float(std::clamp(sums[leaf][channel], -limit, limit));
    }

    bool getRequest(Request &request) const noexcept {
        const auto version = requestVersion_.load(std::memory_order_seq_cst);
        if (version & 1)
            return false;
        request.epoch = requestEpoch_.load(std::memory_order_seq_cst);
        request.first = requestFirst_.load(std::memory_order_seq_cst);
        if (requestVersion_.load(std::memory_order_seq_cst) != version)
            return false;
        request.serial = version / 2;
        return true;
    }
    bool obsolete(const Request &request, std::stop_token stop) const noexcept {
        return stop.stop_requested() || requestVersion_.load(std::memory_order_seq_cst) != request.serial * 2;
    }
    int unchanged(std::size_t leaf) const noexcept {
        struct stat now{};
        if (fstat(fds_[leaf], &now) != 0)
            return errno;
        return sameFile(identities_[leaf], now) ? 0 : ESTALE;
    }
    int fill(Window &window, const Request &request, std::stop_token stop) noexcept {
        window.epoch = request.epoch;
        window.first = request.first;
        window.count = std::min<std::uint64_t>(windowFrames, frames_ - request.first);
        window.error = 0;
        for (std::size_t leaf = 0; leaf < leafCount_; ++leaf) {
            if (const int code = unchanged(leaf))
                return code;
            for (std::uint64_t frame = 0; frame < window.count;) {
                if (obsolete(request, stop))
                    return ECANCELED;
                const auto count = std::min<std::uint64_t>(readBlock, window.count - frame);
                const auto bytes = std::size_t(count * frameBytes);
                std::size_t received = 0;
                auto *destination = reinterpret_cast<char *>(window.leaves[leaf].data() + frame * 2);
                while (received < bytes) {
                    if (obsolete(request, stop))
                        return ECANCELED;
                    const auto got = pread(fds_[leaf], destination + received, bytes - received,
                                           off_t((request.first + frame) * frameBytes + received));
                    if (got < 0 && errno == EINTR)
                        continue;
                    if (got <= 0)
                        return got < 0 ? errno : EIO;
                    received += std::size_t(got);
                }
                for (std::size_t sample = 0; sample < count * 2; ++sample) {
                    if (sample % 256 == 0 && obsolete(request, stop))
                        return ECANCELED;
                    if (!std::isfinite(window.leaves[leaf][std::size_t(frame) * 2 + sample]))
                        return EILSEQ;
                }
                frame += count;
            }
        }
        for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
            if (const int code = unchanged(leaf))
                return code;
        return 0;
    }
    void run(std::stop_token stop) noexcept {
        std::uint64_t handled = 0;
        unsigned back = 2;
        while (!stop.stop_requested()) {
            Request request;
            if (!getRequest(request) || request.serial == handled) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            handled = request.serial;
            auto &window = *windows_[back];
            const int code = fill(window, request, stop);
            if (obsolete(request, stop))
                continue;
            window.error = code;
            if (code)
                window.count = 0;
            back = middle_.exchange(back | dirtyBit, std::memory_order_acq_rel) & indexMask;
        }
    }
    void closeFiles() noexcept {
        for (auto &fd : fds_)
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
    }
    void stop() noexcept {
        if (stopped_)
            return;
        stopped_ = true;
        active_ = false;
        worker_.request_stop();
        if (worker_.joinable())
            worker_.join();
        for (auto &window : windows_)
            window.reset();
        resident_.store(0, std::memory_order_relaxed);
        closeFiles();
    }

    const std::uint64_t frames_;
    const std::size_t leafCount_;
    const Filters &filters_;
    std::array<int, 4> fds_{-1, -1, -1, -1};
    std::array<struct stat, 4> identities_{};
    std::array<std::unique_ptr<Window>, windowCount> windows_;
    std::atomic<unsigned> middle_{1};
    unsigned front_ = 0;
    std::atomic<std::uint64_t> requestVersion_{0}, requestEpoch_{0}, requestFirst_{0};
    std::uint64_t epoch_ = 1, serial_ = 0, requestedEpoch_ = 0, requestedFirst_ = 0;
    std::atomic<std::uint64_t> resident_{0};
    std::jthread worker_;
    std::array<std::array<float, maxReadFrames * 2>, 4> output_{};
    std::array<std::array<float, 2>, 4> lastMotionSample_{};
    unsigned motionEnvelope_ = 0;
    double position_ = 0, target_ = 0;
    std::int64_t previousDisplacement_ = 0;
    int inputDirection_ = 0;
    double speed_ = 0, requestedSpeed_ = 0;
    std::chrono::steady_clock::time_point lastMove_{};
    std::atomic<int> ownerError_{0};
    bool active_ = false, stopped_ = false, missingWindow_ = false;
};

PreparedScratchSource::PreparedScratchSource(std::span<const int> fds, std::uint64_t frames)
    : impl_(std::make_unique<Impl>(fds, frames)) {}
PreparedScratchSource::~PreparedScratchSource() = default;
bool PreparedScratchSource::prime(std::uint64_t frame, std::chrono::milliseconds timeout, std::stop_token stop) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (timeout.count() < 0 ||
        timeout > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now) ||
        stop.stop_requested() || !impl_->relocate(frame))
        return false;
    const auto deadline = now + timeout;
    do {
        impl_->poll();
        if (impl_->ownerError_ || stop.stop_requested())
            return false;
        if (impl_->covers(double(frame)))
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (true);
    return false;
}
bool PreparedScratchSource::relocate(std::uint64_t frame) noexcept { return impl_->relocate(frame); }
bool PreparedScratchSource::follow(std::uint64_t frame) noexcept { return impl_->follow(frame); }
bool PreparedScratchSource::begin(std::uint64_t frame) noexcept { return impl_->begin(frame); }
bool PreparedScratchSource::move(std::int64_t displacement, double seconds) noexcept {
    return impl_->move(displacement, seconds);
}
void PreparedScratchSource::end() noexcept { impl_->end(); }
PreparedScratchSource::Read PreparedScratchSource::read(std::size_t frames) noexcept { return impl_->read(frames); }
bool PreparedScratchSource::active() const noexcept { return impl_->active_; }
double PreparedScratchSource::position() const noexcept { return impl_->position_; }
std::uint64_t PreparedScratchSource::epoch() const noexcept { return impl_->epoch_; }
std::size_t PreparedScratchSource::leafCount() const noexcept { return impl_->leafCount_; }
bool PreparedScratchSource::pending() const noexcept {
    return !impl_->stopped_ && !impl_->ownerError_ && (impl_->missingWindow_ || !impl_->covers(impl_->position_));
}
int PreparedScratchSource::errorCode() const noexcept { return impl_->ownerError_.load(std::memory_order_acquire); }
std::uint64_t PreparedScratchSource::residentBytes() const noexcept {
    return impl_->resident_.load(std::memory_order_relaxed);
}
void PreparedScratchSource::stop() noexcept { impl_->stop(); }
} // namespace conflux::audio

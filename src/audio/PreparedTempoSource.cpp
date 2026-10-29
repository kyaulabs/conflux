// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedTempoSource.hpp"
#include "audio/TempoProcessor.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace conflux::audio {
namespace {
constexpr std::uint64_t frameBytes = 2U * sizeof(float);
constexpr auto maximumSerial = std::numeric_limits<std::uint64_t>::max() / 2U;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(PreparedTempoSource::blockFrames == TempoProcessor::maximumBlockFrames);

bool validPercent(double value) noexcept { return std::isfinite(value) && value >= -6 && value <= 6; }
bool sameFile(const struct stat& a, const struct stat& b) noexcept
{
    return S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
} // namespace

class PreparedTempoSource::Impl {
public:
    struct Slot {
        std::array<std::array<float, blockFrames * 2U>, maximumLeaves> leaves{};
        std::uint64_t generation = 0, command = 0;
        std::size_t frames = 0;
        long double nominalStart = 0;
        double nominalStep = 1, requestedPercent = 0, appliedPercent = 0;
        int error = 0;
        bool terminal = false;
    };
    struct Request {
        std::uint64_t serial = 0, generation = 0, frame = 0, initialCommand = 0;
        std::uint64_t loopIn = 0, loopOut = 0;
        double initialPercent = 0, percent = 0;
        bool enabled = false, keyLock = false;
    };

    Impl(std::span<const int> fds, std::uint64_t frames) : frames_(frames), leafCount_(fds.size())
    {
        if ((leafCount_ != 1 && leafCount_ != 4) || !frames_ || frames_ > (std::uint64_t{1} << 53U) ||
            frames_ > std::uint64_t(std::numeric_limits<off_t>::max()) / frameBytes)
            throw std::invalid_argument("Tempo source requires one/four exact nonempty addressable stereo leaves");
        try {
            for (std::size_t leaf = 0; leaf < leafCount_; ++leaf) {
                fds_[leaf] = fcntl(fds[leaf], F_DUPFD_CLOEXEC, 0);
                if (fds_[leaf] < 0)
                    throw std::system_error(errno, std::generic_category(), "Cannot duplicate tempo PCM");
                if (fstat(fds_[leaf], &identities_[leaf]) != 0)
                    throw std::system_error(errno, std::generic_category(), "Cannot inspect tempo PCM");
                const auto& identity = identities_[leaf];
                if (!S_ISREG(identity.st_mode) || identity.st_size < 0 ||
                    std::uint64_t(identity.st_size) != frames_ * frameBytes)
                    throw std::invalid_argument("Tempo PCM requires exact regular stereo geometry");
                const int flags = fcntl(fds_[leaf], F_GETFL);
                if (flags < 0) throw std::system_error(errno, std::generic_category(), "Cannot inspect tempo PCM access");
                if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_PATH))
                    throw std::invalid_argument("Tempo PCM must be readable");
            }
            worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        } catch (...) {
            closeFiles();
            throw;
        }
    }
    ~Impl() { stop(); }

    bool reserveCommand(bool newGeneration) noexcept
    {
        if (stopped_.load(std::memory_order_relaxed) || ownerError_.load(std::memory_order_relaxed)) return false;
        if (ownerSerial_ == maximumSerial || (newGeneration && ownerGeneration_ == maximumSerial)) {
            ownerEnabled_ = false;
            activeGeneration_.store(0, std::memory_order_seq_cst);
            ownerError_.store(EOVERFLOW, std::memory_order_relaxed);
            return false;
        }
        ++ownerSerial_;
        if (newGeneration) ++ownerGeneration_;
        return true;
    }
    void publish() noexcept
    {
        requestVersion_.store(ownerSerial_ * 2U - 1U, std::memory_order_seq_cst);
        requestGeneration_.store(ownerGeneration_, std::memory_order_seq_cst);
        requestFrame_.store(ownerFrame_, std::memory_order_seq_cst);
        requestLoopIn_.store(ownerLoopIn_, std::memory_order_seq_cst);
        requestLoopOut_.store(ownerLoopOut_, std::memory_order_seq_cst);
        requestInitialCommand_.store(ownerInitialCommand_, std::memory_order_seq_cst);
        requestInitialPercent_.store(std::bit_cast<std::uint64_t>(ownerInitialPercent_), std::memory_order_seq_cst);
        requestPercent_.store(std::bit_cast<std::uint64_t>(ownerPercent_), std::memory_order_seq_cst);
        requestKeyLock_.store(ownerKeyLock_, std::memory_order_seq_cst);
        requestEnabled_.store(ownerEnabled_, std::memory_order_seq_cst);
        requestVersion_.store(ownerSerial_ * 2U, std::memory_order_seq_cst);
    }
    bool request(std::uint64_t frame, double percent, bool keyLock,
                 std::uint64_t loopIn, std::uint64_t loopOut) noexcept
    {
        if (loopOut ? (loopOut > frames_ || loopIn >= loopOut || frame < loopIn || frame >= loopOut ||
                       loopOut - loopIn < AudioLoopState::minimumFrames ||
                       loopOut - loopIn > AudioLoopState::maximumFrames) : loopIn != 0)
            return false;
        if (frame > frames_ || !validPercent(percent) || !reserveCommand(true)) return false;
        // The preceding synchronous borrow has ended. Invalidate before the
        // worker can observe a newer generation or recycle stale slots.
        ownerEnabled_ = false;
        offset_ = 0;
        ownerFrame_ = frame;
        ownerLoopIn_ = loopIn;
        ownerLoopOut_ = loopOut;
        ownerWraps_ = 0;
        ownerNominal_ = static_cast<long double>(frame);
        ownerPercent_ = ownerInitialPercent_ = percent;
        ownerInitialCommand_ = ownerSerial_;
        ownerKeyLock_ = keyLock;
        ownerHasDsp_ = percent != 0;
        ownerFinished_ = false;
        ownerAppliedCommand_ = 0;
        ownerAppliedPercent_ = 0;
        activeGeneration_.store(ownerGeneration_, std::memory_order_seq_cst);
        ownerEnabled_ = true;
        publish();
        retireStale();
        return true;
    }
    bool requestSpeed(double percent) noexcept
    {
        if (!validPercent(percent) || !ownerEnabled_ || ownerFinished_ || errorCode() ||
            (!ownerHasDsp_ && percent != 0)) return false;
        if (percent == ownerPercent_) return !stopped_.load(std::memory_order_relaxed);
        if (!reserveCommand(false)) return false;
        ownerPercent_ = percent;
        publish();
        return true;
    }
    bool cancel() noexcept
    {
        if (!reserveCommand(true)) return false;
        ownerEnabled_ = ownerFinished_ = ownerHasDsp_ = false;
        ownerPercent_ = ownerInitialPercent_ = ownerAppliedPercent_ = 0;
        ownerKeyLock_ = false;
        ownerLoopIn_ = ownerLoopOut_ = ownerWraps_ = 0;
        ownerAppliedCommand_ = 0;
        offset_ = 0;
        activeGeneration_.store(ownerGeneration_, std::memory_order_seq_cst);
        publish();
        retireStale();
        return true;
    }

    void retireStale() noexcept
    {
        for (std::size_t examined = 0; examined < slotCount; ++examined) {
            const auto position = consumed_.load(std::memory_order_relaxed);
            if (position == written_.load(std::memory_order_acquire) ||
                (ownerEnabled_ && slots_[position % slotCount].generation == ownerGeneration_)) break;
            offset_ = 0;
            consumed_.store(position + 1U, std::memory_order_release);
        }
    }
    View peek() noexcept
    {
        retireStale();
        if (!ownerEnabled_ || stopped_.load(std::memory_order_relaxed)) return {};
        const auto position = consumed_.load(std::memory_order_relaxed);
        if (position == written_.load(std::memory_order_acquire)) return {};
        const auto& slot = slots_[position % slotCount];
        if (slot.generation != ownerGeneration_) return {};
        View result;
        result.leafCount = leafCount_;
        result.frames = slot.frames - offset_;
        result.generation = slot.generation;
        result.terminal = slot.terminal;
        result.errorCode = slot.error;
        result.nominalStart = reportedPosition(mapPosition(
            slot.nominalStart + static_cast<long double>(offset_) * slot.nominalStep,
            ownerLoopIn_, ownerLoopOut_), ownerLoopIn_, ownerLoopOut_);
        result.nominalStep = slot.nominalStep;
        result.requestedPercent = slot.requestedPercent;
        result.appliedInputPercent = slot.appliedPercent;
        for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
            result.leaves[leaf] = std::span<const float>(slot.leaves[leaf]).subspan(offset_ * 2U, result.frames * 2U);
        return result;
    }
    bool consume(std::size_t frames) noexcept
    {
        const auto view = peek();
        if (!view || frames > view.frames) return false;
        const auto position = consumed_.load(std::memory_order_relaxed);
        const auto& slot = slots_[position % slotCount];
        if (!slot.frames) {
            if (frames) return false;
            if (slot.terminal && !slot.error) {
                ownerFinished_ = true;
                ownerNominal_ = static_cast<long double>(frames_);
            }
            offset_ = 0;
            consumed_.store(position + 1U, std::memory_order_release);
            return true;
        }
        if (!frames) return true;
        const auto next = slot.nominalStart + static_cast<long double>(offset_ + frames) * slot.nominalStep;
        if (ownerLoopOut_) {
            const auto length = static_cast<long double>(ownerLoopOut_ - ownerLoopIn_);
            const auto prior = slot.nominalStart + static_cast<long double>(offset_) * slot.nominalStep;
            const auto wraps = static_cast<std::uint64_t>(std::floor((next - ownerLoopIn_) / length) -
                std::floor((prior - ownerLoopIn_) / length));
            if (ownerWraps_ > std::numeric_limits<std::uint64_t>::max() - wraps) {
                ownerError_.store(EOVERFLOW, std::memory_order_relaxed);
                return false;
            }
            ownerWraps_ += wraps;
        }
        offset_ += frames;
        ownerNominal_ = mapPosition(next, ownerLoopIn_, ownerLoopOut_);
        ownerAppliedPercent_ = slot.appliedPercent;
        ownerAppliedCommand_ = slot.command;
        if (offset_ == slot.frames) {
            offset_ = 0;
            consumed_.store(position + 1U, std::memory_order_release);
        }
        return true;
    }
    AudioTempoState status() noexcept
    {
        const auto view = peek();
        AudioTempoState result;
        result.supported = true;
        result.active = ownerEnabled_ && !stopped_.load(std::memory_order_relaxed);
        result.percent = ownerPercent_;
        result.appliedPercent = ownerAppliedPercent_;
        result.generation = ownerGeneration_;
        result.errorCode = errorCode();
        result.finished = ownerFinished_;
        result.keyLock = ownerKeyLock_;
        result.pending = result.active && !result.finished && !result.errorCode &&
            (!view || ownerAppliedCommand_ != ownerSerial_);
        return result;
    }
    int errorCode() const noexcept
    {
        if (const int code = ownerError_.load(std::memory_order_relaxed)) return code;
        if (stopped_.load(std::memory_order_relaxed)) return 0;
        const auto current = activeGeneration_.load(std::memory_order_seq_cst);
        const auto before = errorGeneration_.load(std::memory_order_seq_cst);
        if (!current || before != current) return 0;
        const int code = workerError_.load(std::memory_order_seq_cst);
        return before == errorGeneration_.load(std::memory_order_seq_cst) &&
            current == activeGeneration_.load(std::memory_order_seq_cst) ? code : 0;
    }
    void stop() noexcept
    {
        if (stopped_.exchange(true, std::memory_order_relaxed)) return;
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
        ownerEnabled_ = false;
        closeFiles();
    }

    const std::uint64_t frames_;
    const std::size_t leafCount_;
    long double ownerNominal_ = 0;
    std::uint64_t ownerGeneration_ = 0, ownerLoopIn_ = 0, ownerLoopOut_ = 0, ownerWraps_ = 0;
    double nominalPosition() const noexcept
    { return reportedPosition(ownerNominal_, ownerLoopIn_, ownerLoopOut_); }

private:
    long double mapPosition(long double value, std::uint64_t in, std::uint64_t out) const noexcept
    {
        return out ? static_cast<long double>(in) + std::fmod(value - in, static_cast<long double>(out - in))
                   : std::min(static_cast<long double>(frames_), value);
    }
    static double reportedPosition(long double value, std::uint64_t in, std::uint64_t out) noexcept
    {
        const double result = static_cast<double>(value);
        // A fractional value immediately below a large integer OUT can round
        // up in double; a looping cursor must still remain strictly below OUT.
        return out && result >= double(out) ? std::nextafter(double(out), double(in)) : result;
    }
    bool readRequest(Request& result) const noexcept
    {
        const auto before = requestVersion_.load(std::memory_order_seq_cst);
        if (before & 1U) return false;
        result.generation = requestGeneration_.load(std::memory_order_seq_cst);
        result.frame = requestFrame_.load(std::memory_order_seq_cst);
        result.loopIn = requestLoopIn_.load(std::memory_order_seq_cst);
        result.loopOut = requestLoopOut_.load(std::memory_order_seq_cst);
        result.initialCommand = requestInitialCommand_.load(std::memory_order_seq_cst);
        result.initialPercent = std::bit_cast<double>(requestInitialPercent_.load(std::memory_order_seq_cst));
        result.percent = std::bit_cast<double>(requestPercent_.load(std::memory_order_seq_cst));
        result.keyLock = requestKeyLock_.load(std::memory_order_seq_cst);
        result.enabled = requestEnabled_.load(std::memory_order_seq_cst);
        if (before != requestVersion_.load(std::memory_order_seq_cst)) return false;
        result.serial = before / 2U;
        return true;
    }
    bool current(std::uint64_t generation, std::stop_token stop) const noexcept
    {
        return !stop.stop_requested() && !stopped_.load(std::memory_order_relaxed) &&
            activeGeneration_.load(std::memory_order_seq_cst) == generation;
    }
    int unchanged(std::size_t leaf) const noexcept
    {
        struct stat now{};
        if (fstat(fds_[leaf], &now) != 0) return errno;
        return sameFile(identities_[leaf], now) ? 0 : ESTALE;
    }
    int allUnchanged() const noexcept
    {
        for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
            if (const int code = unchanged(leaf)) return code;
        return 0;
    }
    int readInput(std::uint64_t generation, std::uint64_t frame, std::size_t count,
        std::span<float> packed, std::span<float> stereo, std::stop_token stop) const noexcept
    {
        const auto channels = leafCount_ * 2U;
        for (std::size_t leaf = 0; leaf < leafCount_; ++leaf) {
            if (!current(generation, stop)) return ECANCELED;
            if (const int code = unchanged(leaf)) return code;
            const auto bytes = count * frameBytes;
            std::size_t received = 0;
            while (received < bytes) {
                if (!current(generation, stop)) return ECANCELED;
                const auto got = pread(fds_[leaf], reinterpret_cast<char*>(stereo.data()) + received,
                    bytes - received, static_cast<off_t>(frame * frameBytes + received));
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) return got < 0 ? errno : EIO;
                received += static_cast<std::size_t>(got);
            }
            if (!current(generation, stop)) return ECANCELED;
            for (std::size_t sample = 0; sample < count * 2U; ++sample) {
                if (sample % 256U == 0 && !current(generation, stop)) return ECANCELED;
                if (!std::isfinite(stereo[sample])) return EILSEQ;
                packed[(sample / 2U) * channels + leaf * 2U + sample % 2U] = stereo[sample];
            }
            if (!current(generation, stop)) return ECANCELED;
            if (const int code = unchanged(leaf)) return code;
        }
        return 0;
    }
    void publishError(std::uint64_t generation, int code) noexcept
    {
        // Zero before replacing errno prevents an external sampler from
        // combining a prior generation with a newly written error value.
        errorGeneration_.store(0, std::memory_order_seq_cst);
        workerError_.store(code > 0 ? code : EIO, std::memory_order_seq_cst);
        errorGeneration_.store(generation, std::memory_order_seq_cst);
    }
    void run(std::stop_token stop) noexcept
    {
        std::unique_ptr<TempoProcessor> processor;
        std::array<float, blockFrames * maximumLeaves * 2U> input{}, output{};
        std::array<float, blockFrames * 2U> stereo{};
        std::uint64_t generation = 0, nextFrame = 0;
        std::uint64_t desiredRevision = 1, appliedRevision = 1, desiredCommand = 0, appliedCommand = 0;
        std::size_t inputFrames = 0, inputOffset = 0;
        double desiredPercent = 0, appliedPercent = 0;
        long double nominal = 0;
        int terminalError = 0;
        bool terminalPending = false, generationDone = true;
        while (!stop.stop_requested() && !stopped_.load(std::memory_order_relaxed)) {
            Request request;
            if (!readRequest(request) || !request.generation) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (request.generation != generation) {
                processor.reset();
                generation = request.generation;
                nextFrame = request.frame;
                inputFrames = inputOffset = 0;
                nominal = static_cast<long double>(request.frame);
                desiredPercent = appliedPercent = request.initialPercent;
                desiredCommand = appliedCommand = request.initialCommand;
                desiredRevision = appliedRevision = 1;
                terminalError = 0;
                terminalPending = false;
                generationDone = !request.enabled;
                if (request.enabled && current(generation, stop)) {
                    try {
                        if (const int code = allUnchanged()) throw std::system_error(code, std::generic_category());
                        if (request.frame == frames_) terminalPending = true;
                        else processor = std::make_unique<TempoProcessor>(TempoProcessor::Config{
                            leafCount_ * 2U, request.initialPercent, request.keyLock});
                    } catch (const std::bad_alloc&) { terminalError = ENOMEM; }
                      catch (const std::system_error& error) { terminalError = error.code().value(); }
                      catch (...) { terminalError = EIO; }
                    if (terminalError) {
                        terminalPending = true;
                        publishError(generation, terminalError);
                    }
                }
            }
            if (generationDone || !current(generation, stop)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            const auto position = written_.load(std::memory_order_relaxed);
            if (position == std::numeric_limits<std::uint64_t>::max()) {
                publishError(generation, EOVERFLOW);
                generationDone = true;
                continue;
            }
            if (position - consumed_.load(std::memory_order_acquire) >= slotCount) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue; // No DSP/input feed while the bounded output ring is full.
            }
            std::size_t produced = 0;
            try {
                if (!terminalPending) {
                    if (processor->counts().inputFramesConsumed && processor->phase() == TempoProcessor::Phase::Running &&
                        (request.percent != desiredPercent || request.serial != desiredCommand)) {
                        desiredRevision = processor->requestSpeed(processor->generation(), request.percent);
                        desiredPercent = request.percent;
                        desiredCommand = request.serial;
                    }
                    if (inputOffset == inputFrames && request.loopOut && nextFrame == request.loopOut)
                        nextFrame = request.loopIn;
                    const auto inputEnd = request.loopOut ? request.loopOut : frames_;
                    if (inputOffset == inputFrames && nextFrame < inputEnd) {
                        inputFrames = static_cast<std::size_t>(std::min<std::uint64_t>(blockFrames, inputEnd - nextFrame));
                        inputOffset = 0;
                        const int code = readInput(generation, nextFrame, inputFrames, input, stereo, stop);
                        if (code) throw std::system_error(code, std::generic_category());
                        nextFrame += inputFrames;
                    }
                    const auto channels = leafCount_ * 2U;
                    const auto offered = std::span<const float>(input).subspan(inputOffset * channels,
                        (inputFrames - inputOffset) * channels);
                    const bool eof = !request.loopOut && nextFrame == frames_ && inputOffset == inputFrames;
                    const auto step = processor->process(processor->generation(), offered,
                        std::span(output).first(blockFrames * channels), eof);
                    inputOffset += step.inputFramesUsed;
                    produced = step.outputFrames;
                    if (step.appliedInputConfiguration == desiredRevision &&
                        (step.appliedInputConfiguration != appliedRevision || step.inputFramesUsed)) {
                        appliedRevision = step.appliedInputConfiguration;
                        appliedPercent = desiredPercent;
                        appliedCommand = desiredCommand;
                    }
                    if (step.phase == TempoProcessor::Phase::Finished) {
                        if (const int code = allUnchanged()) throw std::system_error(code, std::generic_category());
                        terminalPending = true;
                    }
                    if (!step.internalProgress && !step.inputFramesUsed && !step.outputFrames && !terminalPending)
                        throw std::runtime_error("Prepared tempo worker made no progress");
                }
            } catch (const std::bad_alloc&) { terminalError = ENOMEM; produced = 0; terminalPending = true; }
              catch (const std::system_error& error) { terminalError = error.code().value(); produced = 0; terminalPending = true; }
              catch (...) { terminalError = EIO; produced = 0; terminalPending = true; }
            if (!current(generation, stop)) continue;
            if (terminalError) publishError(generation, terminalError);
            if (!produced && !terminalPending) continue;
            auto& slot = slots_[position % slotCount];
            slot.generation = generation;
            slot.command = appliedCommand;
            slot.frames = produced;
            slot.nominalStart = nominal;
            slot.nominalStep = 1.0 + appliedPercent / 100.0;
            slot.requestedPercent = request.percent;
            slot.appliedPercent = appliedPercent;
            slot.terminal = !produced && terminalPending && !terminalError;
            slot.error = !produced ? terminalError : 0;
            const auto channels = leafCount_ * 2U;
            for (std::size_t leaf = 0; leaf < leafCount_; ++leaf)
                for (std::size_t frame = 0; frame < produced; ++frame)
                    for (std::size_t channel = 0; channel < 2U; ++channel)
                        slot.leaves[leaf][frame * 2U + channel] = output[frame * channels + leaf * 2U + channel];
            nominal = mapPosition(nominal + static_cast<long double>(produced) * slot.nominalStep,
                                  request.loopIn, request.loopOut);
            if (!current(generation, stop)) continue;
            written_.store(position + 1U, std::memory_order_release);
            if (!produced && terminalPending) generationDone = true;
        }
    }
    void closeFiles() noexcept
    {
        for (auto& fd : fds_) if (fd >= 0) { close(fd); fd = -1; }
    }

    std::array<int, maximumLeaves> fds_{-1, -1, -1, -1};
    std::array<struct stat, maximumLeaves> identities_{};
    std::array<Slot, slotCount> slots_{};
    std::atomic<std::uint64_t> written_{0}, consumed_{0};
    std::atomic<std::uint64_t> activeGeneration_{0}, errorGeneration_{0};
    std::atomic<int> workerError_{0}, ownerError_{0};
    std::atomic<bool> stopped_{false};
    std::atomic<std::uint64_t> requestVersion_{0}, requestGeneration_{0}, requestFrame_{0}, requestInitialCommand_{0};
    std::atomic<std::uint64_t> requestInitialPercent_{0}, requestPercent_{0};
    std::atomic<std::uint64_t> requestLoopIn_{0}, requestLoopOut_{0};
    std::atomic<bool> requestKeyLock_{false}, requestEnabled_{false};
    std::uint64_t ownerSerial_ = 0, ownerFrame_ = 0, ownerInitialCommand_ = 0, ownerAppliedCommand_ = 0;
    double ownerPercent_ = 0, ownerInitialPercent_ = 0, ownerAppliedPercent_ = 0;
    std::size_t offset_ = 0;
    bool ownerEnabled_ = false, ownerFinished_ = false, ownerKeyLock_ = false, ownerHasDsp_ = false;
    std::jthread worker_;
};

PreparedTempoSource::PreparedTempoSource(std::span<const int> fds, std::uint64_t frames)
    : impl_(std::make_unique<Impl>(fds, frames)) {}
PreparedTempoSource::~PreparedTempoSource() = default;
bool PreparedTempoSource::request(std::uint64_t frame, double percent, bool keyLock,
                                  std::uint64_t loopIn, std::uint64_t loopOut) noexcept
{ return impl_->request(frame, percent, keyLock, loopIn, loopOut); }
bool PreparedTempoSource::requestSpeed(double percent) noexcept { return impl_->requestSpeed(percent); }
bool PreparedTempoSource::cancel() noexcept { return impl_->cancel(); }
PreparedTempoSource::View PreparedTempoSource::peek() noexcept { return impl_->peek(); }
bool PreparedTempoSource::consume(std::size_t frames) noexcept { return impl_->consume(frames); }
double PreparedTempoSource::nominalPosition() const noexcept { return impl_->nominalPosition(); }
std::uint64_t PreparedTempoSource::loopWraps() const noexcept { return impl_->ownerWraps_; }
std::uint64_t PreparedTempoSource::generation() const noexcept { return impl_->ownerGeneration_; }
AudioTempoState PreparedTempoSource::status() noexcept { return impl_->status(); }
int PreparedTempoSource::errorCode() const noexcept { return impl_->errorCode(); }
void PreparedTempoSource::stop() noexcept { impl_->stop(); }

} // namespace conflux::audio

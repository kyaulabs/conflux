// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PcmReadAhead.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace conflux::audio {
namespace {
constexpr std::size_t channels = 2;
constexpr std::size_t frameBytes = channels * sizeof(float);
using Clock = std::chrono::steady_clock;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
} // namespace

class PcmReadAhead::Impl {
public:
    struct Slot {
        std::array<float, blockFrames * channels> samples{};
        std::uint64_t epoch = 0, first = 0;
        std::size_t count = 0;
        int error = 0;
    };

    Impl(int normalizedFd, std::uint64_t frames) : frames_(frames)
    {
        if (!frames || frames > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / frameBytes)
            throw std::invalid_argument("Read-ahead requires a nonempty addressable PCM file");
        fd_ = fcntl(normalizedFd, F_DUPFD_CLOEXEC, 0);
        if (fd_ < 0)
            throw std::system_error(errno, std::generic_category(), "Cannot duplicate prepared PCM file");
        struct stat status{};
        const auto statResult = fstat(fd_, &status);
        if (statResult != 0 || !S_ISREG(status.st_mode)) {
            const int code = statResult != 0 ? errno : EINVAL;
            close(fd_);
            fd_ = -1;
            throw std::system_error(code, std::generic_category(), "Prepared PCM must be a regular file");
        }
        const auto access = fcntl(fd_, F_GETFL);
        if (access < 0 || (access & O_ACCMODE) == O_WRONLY) {
            const int code = access < 0 ? errno : EBADF;
            close(fd_);
            fd_ = -1;
            throw std::system_error(code, std::generic_category(), "Prepared PCM must be readable");
        }
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            close(fd_);
            fd_ = -1;
            throw;
        }
    }

    ~Impl() { stop(); }

    AudioSource sourceRef() noexcept
    {
        return {frames_, nullptr, this,
            [](void* context, std::uint64_t frame, std::span<float> output) noexcept {
                return static_cast<Impl*>(context)->read(frame, output);
            },
            [](void* context, std::uint64_t frame) noexcept {
                static_cast<Impl*>(context)->discontinuity(frame);
            }};
    }

    // This mailbox has one consumer-side writer. Sequentially consistent
    // atomic payload/version stores make the odd/even snapshot unambiguous;
    // the worker retries, while the render thread always performs fixed stores.
    void discontinuity(std::uint64_t frame) noexcept
    {
        if (frame > frames_ || frame == consumerFrame_)
            return;
        consumerFrame_ = frame;
        consumerEpoch_ += 2;
        requestVersion_.store(consumerEpoch_ - 1, std::memory_order_seq_cst);
        requestFrame_.store(frame, std::memory_order_seq_cst);
        requestVersion_.store(consumerEpoch_, std::memory_order_seq_cst);
        controlError_.store(0, std::memory_order_relaxed);
        retireStale();
    }

    bool request(std::uint64_t& epoch, std::uint64_t& frame) const noexcept
    {
        const auto before = requestVersion_.load(std::memory_order_seq_cst);
        if (before & 1U)
            return false;
        frame = requestFrame_.load(std::memory_order_seq_cst);
        const auto after = requestVersion_.load(std::memory_order_seq_cst);
        if (before != after)
            return false;
        epoch = before;
        return true;
    }

    void retireStale() noexcept
    {
        for (std::size_t count = 0; count < slotCount; ++count) {
            const auto position = consumed_.load(std::memory_order_relaxed);
            if (position == written_.load(std::memory_order_acquire) ||
                slots_[position % slotCount].epoch == consumerEpoch_)
                break;
            consumed_.store(position + 1, std::memory_order_release);
        }
    }

    AudioReadResult read(std::uint64_t frame, std::span<float> output) noexcept
    {
        if (output.size() % channels || frame > frames_)
            return {0, true};
        if (output.empty())
            return {};
        if (stopping_.load(std::memory_order_relaxed))
            return {0, true};
        if (frame != consumerFrame_)
            discontinuity(frame);
        if (frame == frames_)
            return {};
        AudioReadResult result;
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(output.size() / channels, frames_ - frame));
        for (std::size_t examined = 0; examined < slotCount && result.frames < wanted; ++examined) {
            const auto position = consumed_.load(std::memory_order_relaxed);
            if (position == written_.load(std::memory_order_acquire))
                break;
            const auto& slot = slots_[position % slotCount];
            if (slot.epoch != consumerEpoch_ || consumerFrame_ >= slot.first + slot.count) {
                if (slot.epoch == consumerEpoch_ && slot.error)
                    result.failed = true;
                consumed_.store(position + 1, std::memory_order_release);
                if (result.failed)
                    break;
                continue;
            }
            if (consumerFrame_ < slot.first)
                break;
            const auto offset = static_cast<std::size_t>(consumerFrame_ - slot.first);
            const auto count = std::min(wanted - result.frames, slot.count - offset);
            std::copy_n(slot.samples.data() + offset * channels, count * channels,
                output.data() + result.frames * channels);
            result.frames += count;
            consumerFrame_ += count;
            result.failed = slot.error != 0;
            if (offset + count == slot.count)
                consumed_.store(position + 1, std::memory_order_release);
            if (result.failed)
                break;
        }
        if (errorEpoch_.load(std::memory_order_acquire) == consumerEpoch_)
            result.failed = true;
        return result;
    }

    bool prime(std::uint64_t frame, std::chrono::milliseconds timeout)
    {
        if (frame > frames_ || timeout.count() < 0) {
            controlError_.store(EINVAL, std::memory_order_relaxed);
            return false;
        }
        if (stopping_.load(std::memory_order_relaxed)) {
            controlError_.store(ECANCELED, std::memory_order_relaxed);
            return false;
        }
        controlError_.store(0, std::memory_order_relaxed);
        discontinuity(frame);
        const auto deadline = Clock::now() + timeout;
        do {
            retireStale();
            if (errorEpoch_.load(std::memory_order_acquire) == consumerEpoch_)
                return false;
            if (frame == frames_)
                return true;
            const auto position = consumed_.load(std::memory_order_relaxed);
            if (position != written_.load(std::memory_order_acquire)) {
                const auto& slot = slots_[position % slotCount];
                if (slot.epoch == consumerEpoch_ && slot.first <= frame &&
                    frame - slot.first < slot.count)
                    return slot.error == 0;
            }
            if (Clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (!stopping_.load(std::memory_order_relaxed));
        controlError_.store(stopping_.load(std::memory_order_relaxed) ? ECANCELED : ETIMEDOUT,
            std::memory_order_relaxed);
        return false;
    }

    int errorCode() const noexcept
    {
        if (const int control = controlError_.load(std::memory_order_relaxed))
            return control;
        if (errorEpoch_.load(std::memory_order_acquire) == requestVersion_.load(std::memory_order_seq_cst))
            return workerError_.load(std::memory_order_relaxed);
        return 0;
    }

    void stop() noexcept
    {
        stopping_.store(true, std::memory_order_relaxed);
        if (worker_.joinable())
            worker_.join();
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    void run() noexcept
    {
        std::uint64_t epoch = 0, next = 0;
        bool finished = false;
        while (!stopping_.load(std::memory_order_relaxed)) {
            std::uint64_t requestedEpoch = 0, requestedFrame = 0;
            if (!request(requestedEpoch, requestedFrame)) {
                std::this_thread::yield();
                continue;
            }
            if (requestedEpoch != epoch) {
                epoch = requestedEpoch;
                next = requestedFrame;
                finished = false;
                errorEpoch_.store(0, std::memory_order_release);
            }
            const auto position = written_.load(std::memory_order_relaxed);
            if (finished || next == frames_ ||
                position - consumed_.load(std::memory_order_acquire) >= slotCount) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            auto& slot = slots_[position % slotCount];
            slot.epoch = epoch;
            slot.first = next;
            slot.count = 0;
            slot.error = 0;
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(blockFrames, frames_ - next));
            const auto wantedBytes = count * frameBytes;
            std::size_t bytes = 0;
            while (bytes < wantedBytes && !stopping_.load(std::memory_order_relaxed)) {
                const auto got = pread(fd_, reinterpret_cast<char*>(slot.samples.data()) + bytes,
                    wantedBytes - bytes, static_cast<off_t>(next * frameBytes + bytes));
                if (got < 0 && errno == EINTR)
                    continue;
                if (got <= 0) {
                    slot.error = got < 0 ? errno : EIO;
                    break;
                }
                bytes += static_cast<std::size_t>(got);
            }
            if (stopping_.load(std::memory_order_relaxed))
                break;
            if (requestVersion_.load(std::memory_order_seq_cst) != epoch)
                continue;
            slot.count = bytes / frameBytes;
            for (std::size_t frame = 0; frame < slot.count; ++frame) {
                if (!std::isfinite(slot.samples[frame * channels]) ||
                    !std::isfinite(slot.samples[frame * channels + 1])) {
                    slot.count = frame;
                    slot.error = EILSEQ;
                    break;
                }
            }
            if (slot.error) {
                workerError_.store(slot.error, std::memory_order_relaxed);
                errorEpoch_.store(epoch, std::memory_order_release);
                finished = true;
            }
            next += slot.count;
            written_.store(position + 1, std::memory_order_release);
        }
    }

    const std::uint64_t frames_;
    std::array<Slot, slotCount> slots_{};
    std::atomic<std::uint64_t> written_{0}, consumed_{0};
    std::atomic<std::uint64_t> requestVersion_{2}, requestFrame_{0};
    std::atomic<std::uint64_t> errorEpoch_{0};
    std::atomic<int> workerError_{0}, controlError_{0};
    std::atomic<bool> stopping_{false};
    std::uint64_t consumerEpoch_ = 2, consumerFrame_ = 0;
    int fd_ = -1;
    std::thread worker_;
};

PcmReadAhead::PcmReadAhead(int normalizedFd, std::uint64_t frames)
    : impl_(std::make_unique<Impl>(normalizedFd, frames)) {}

PcmReadAhead::~PcmReadAhead() = default;

AudioSource PcmReadAhead::sourceRef() noexcept { return impl_->sourceRef(); }

bool PcmReadAhead::prime(std::uint64_t frame, std::chrono::milliseconds timeout)
{
    return impl_->prime(frame, timeout);
}

int PcmReadAhead::errorCode() const noexcept { return impl_->errorCode(); }

std::string PcmReadAhead::error() const
{
    const auto code = errorCode();
    return code ? std::error_code(code, std::generic_category()).message() : std::string{};
}

void PcmReadAhead::stop() noexcept { impl_->stop(); }

} // namespace conflux::audio

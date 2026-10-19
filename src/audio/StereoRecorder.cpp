// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/StereoRecorder.hpp"

#include <sndfile.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace conflux::audio {

class StereoRecorder::Impl {
public:
    enum class Failure : std::uint32_t { None, InvalidPcm, Overflow, FileLimit, Write, Close, DescriptorClose };

    static std::size_t checkedCapacity(std::size_t frames)
    {
        if (!frames || frames > maximumFrames || frames > std::vector<float>{}.max_size() / channels)
            throw std::invalid_argument("Recording ring capacity must hold one or more bounded stereo frames");
        return frames;
    }

    explicit Impl(const std::filesystem::path& path, std::size_t capacityFrames)
        : capacity_(checkedCapacity(capacityFrames)), pcm_(capacity_ * channels)
    {
        descriptor_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor_ < 0)
            throw std::system_error(errno, std::generic_category(), "Cannot create a new recording");
        struct stat info{};
        const int inspected = ::fstat(descriptor_, &info);
        if (inspected < 0 || !S_ISREG(info.st_mode)) {
            const int code = inspected < 0 ? errno : 0;
            ::close(descriptor_);
            descriptor_ = -1;
            if (code)
                throw std::system_error(code, std::generic_category(), "Cannot inspect recording target");
            throw std::invalid_argument("Recording target must be a regular file");
        }
        SF_INFO format{};
        format.samplerate = sampleRate;
        format.channels = channels;
        format.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
        file_ = sf_open_fd(descriptor_, SFM_WRITE, &format, SF_FALSE);
        if (!file_) {
            const std::string message = sf_strerror(nullptr);
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("Cannot initialize float WAV recording: " + message);
        }
        try {
            writer_ = std::thread([this] { write(); });
        } catch (...) {
            sf_close(file_);
            file_ = nullptr;
            ::close(descriptor_);
            descriptor_ = -1;
            throw;
        }
    }

    ~Impl() { stop(); }

    bool push(std::span<const float> interleaved) noexcept
    {
        if (!running())
            return false;
        if (interleaved.size() % channels) {
            fail(Failure::InvalidPcm);
            return false;
        }
        const auto frames = interleaved.size() / channels;
        if (!frames)
            return true;
        const auto position = accepted_.load(std::memory_order_relaxed);
        if (frames > maximumFrames - position) {
            fail(Failure::FileLimit);
            return false;
        }
        const auto occupied = position - written_.load(std::memory_order_acquire);
        if (frames > capacity_ - occupied) {
            overflowCount_.fetch_add(1, std::memory_order_relaxed);
            overflowFrames_.fetch_add(frames, std::memory_order_relaxed);
            fail(Failure::Overflow);
            return false;
        }
        if (std::ranges::any_of(interleaved, [](float value) { return !std::isfinite(value); })) {
            fail(Failure::InvalidPcm);
            return false;
        }
        const auto offset = static_cast<std::size_t>(position % capacity_);
        const auto first = std::min(frames, capacity_ - offset) * channels;
        std::copy_n(interleaved.data(), first, pcm_.data() + offset * channels);
        if (first < interleaved.size())
            std::copy_n(interleaved.data() + first, interleaved.size() - first, pcm_.data());
        accepted_.store(position + frames, std::memory_order_release);
        return true;
    }

    void stop() noexcept
    {
        accepting_.store(false, std::memory_order_release);
        if (writer_.joinable())
            writer_.join();
    }

    bool running() const noexcept
    {
        return accepting_.load(std::memory_order_acquire) &&
               failure_.load(std::memory_order_acquire) == Failure::None;
    }

    void fail(Failure failure) noexcept
    {
        auto expected = Failure::None;
        failure_.compare_exchange_strong(expected, failure, std::memory_order_release, std::memory_order_relaxed);
    }

    void failWrite(Failure failure, int sndfileError, int systemError) noexcept
    {
        // Preserve the first storage failure even when a producer-side fault
        // already stopped acceptance. Only this worker publishes these details.
        if (storageFailure_.load(std::memory_order_relaxed) == Failure::None) {
            sndfileError_.store(sndfileError, std::memory_order_relaxed);
            systemError_.store(systemError, std::memory_order_relaxed);
            storageFailure_.store(failure, std::memory_order_release);
        }
        fail(failure);
    }

    void write() noexcept
    {
        for (;;) {
            // Observe terminal publication before loading the final head so a
            // queue failure/stop cannot hide an earlier accepted prefix.
            const bool ending = !accepting_.load(std::memory_order_acquire) ||
                failure_.load(std::memory_order_acquire) != Failure::None;
            const auto position = written_.load(std::memory_order_relaxed);
            const auto available = accepted_.load(std::memory_order_acquire) - position;
            if (!available) {
                if (ending)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const auto offset = static_cast<std::size_t>(position % capacity_);
            const auto frames = std::min<std::uint64_t>({available, capacity_ - offset, 4096});
            errno = 0;
            const auto count = sf_writef_float(file_, pcm_.data() + offset * channels,
                                               static_cast<sf_count_t>(frames));
            const int systemError = errno;
            const int sndfileError = sf_error(file_);
            if (count != static_cast<sf_count_t>(frames) || sndfileError != SF_ERR_NO_ERROR) {
                failWrite(Failure::Write, sndfileError, systemError);
                if (count > 0 && static_cast<std::uint64_t>(count) <= frames)
                    written_.store(position + static_cast<std::uint64_t>(count), std::memory_order_release);
                break;
            }
            written_.store(position + frames, std::memory_order_release);
        }
        errno = 0;
        const int closeError = sf_close(file_);
        file_ = nullptr;
        if (closeError)
            failWrite(Failure::Close, closeError, errno);
        errno = 0;
        if (::close(descriptor_) < 0)
            failWrite(Failure::DescriptorClose, 0, errno);
        descriptor_ = -1;
        accepting_.store(false, std::memory_order_release);
    }

    std::string error() const
    {
        const auto failure = failure_.load(std::memory_order_acquire);
        std::string message;
        switch (failure) {
        case Failure::None: break;
        case Failure::InvalidPcm: message = "Recording requires finite, complete stereo frames"; break;
        case Failure::Overflow: message = "Recording queue overflowed; recording stopped at its accepted prefix"; break;
        case Failure::FileLimit: message = "Recording reached the WAV size limit; recording stopped before overflow"; break;
        case Failure::Write:
        case Failure::Close:
        case Failure::DescriptorClose: break;
        }
        const auto storageFailure = storageFailure_.load(std::memory_order_acquire);
        if (storageFailure == Failure::None)
            return message;
        if (!message.empty())
            message += "; ";
        message += storageFailure == Failure::Write ? "Cannot write recording" : "Cannot close recording";
        if (const auto code = sndfileError_.load(std::memory_order_relaxed); code) {
            const auto* detail = sf_error_number(code);
            message += ": " + (detail ? std::string(detail) : "libsndfile error " + std::to_string(code));
        }
        if (const auto code = systemError_.load(std::memory_order_relaxed); code)
            message += ": " + std::error_code(code, std::generic_category()).message();
        return message;
    }

    static_assert(std::atomic<Failure>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<int>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    const std::size_t capacity_;
    std::vector<float> pcm_;
    std::atomic<std::uint64_t> accepted_{0}, written_{0}, overflowCount_{0}, overflowFrames_{0};
    std::atomic<bool> accepting_{true};
    std::atomic<Failure> failure_{Failure::None}, storageFailure_{Failure::None};
    std::atomic<int> sndfileError_{0}, systemError_{0};
    SNDFILE* file_ = nullptr;
    int descriptor_ = -1;
    std::thread writer_;
};

StereoRecorder::StereoRecorder(const std::filesystem::path& path, std::size_t capacityFrames)
    : impl_(std::make_unique<Impl>(path, capacityFrames)) {}
StereoRecorder::~StereoRecorder() = default;
bool StereoRecorder::push(std::span<const float> interleaved) noexcept { return impl_->push(interleaved); }
void StereoRecorder::stop() noexcept { impl_->stop(); }
bool StereoRecorder::running() const noexcept { return impl_->running(); }
bool StereoRecorder::failed() const noexcept
{ return impl_->failure_.load(std::memory_order_acquire) != Impl::Failure::None; }
std::string StereoRecorder::error() const { return impl_->error(); }
std::uint64_t StereoRecorder::acceptedFrames() const noexcept
{ return impl_->accepted_.load(std::memory_order_acquire); }
std::uint64_t StereoRecorder::writtenFrames() const noexcept
{ return impl_->written_.load(std::memory_order_acquire); }
std::uint64_t StereoRecorder::overflowCount() const noexcept
{ return impl_->overflowCount_.load(std::memory_order_relaxed); }
std::uint64_t StereoRecorder::overflowFrames() const noexcept
{ return impl_->overflowFrames_.load(std::memory_order_relaxed); }

} // namespace conflux::audio

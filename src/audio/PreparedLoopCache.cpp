// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedLoopCache.hpp"

#include <algorithm>
#include <atomic>
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
constexpr std::size_t readFrames = 4096;
constexpr auto maximumSerial = std::numeric_limits<std::uint64_t>::max() / 2U;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<const PreparedLoopCache::View*>::is_always_lock_free);

bool sameFile(const struct stat& a, const struct stat& b) noexcept
{
    return S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
} // namespace

class PreparedLoopCache::Impl {
public:
    Impl(std::span<const int> fds, std::uint64_t frames) : frames_(frames), leafCount_(fds.size())
    {
        if ((leafCount_ != 1 && leafCount_ != 4) || !frames_ ||
            frames_ > std::uint64_t(std::numeric_limits<off_t>::max()) / frameBytes)
            throw std::invalid_argument("Loop cache requires one or four nonempty addressable stereo PCM leaves");
        try {
            for (std::size_t leaf = 0; leaf < leafCount_; ++leaf) {
                fds_[leaf] = fcntl(fds[leaf], F_DUPFD_CLOEXEC, 0);
                if (fds_[leaf] < 0)
                    throw std::system_error(errno, std::generic_category(), "Cannot duplicate loop-cache PCM");
                if (fstat(fds_[leaf], &identities_[leaf]) != 0)
                    throw std::system_error(errno, std::generic_category(), "Cannot inspect loop-cache PCM");
                const auto& status = identities_[leaf];
                if (!S_ISREG(status.st_mode) || status.st_size < 0 || std::uint64_t(status.st_size) != frames_ * frameBytes)
                    throw std::invalid_argument("Loop cache requires exact regular stereo PCM geometry");
                const int flags = fcntl(fds_[leaf], F_GETFL);
                if (flags < 0)
                    throw std::system_error(errno, std::generic_category(), "Cannot inspect loop-cache PCM access");
                if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_PATH))
                    throw std::invalid_argument("Loop-cache PCM must be readable");
            }
            worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        } catch (...) {
            closeFiles();
            throw;
        }
    }

    ~Impl() { stop(); }

    bool request(std::uint64_t firstFrame) noexcept
    {
        if (firstFrame >= frames_) return false;
        return publish(firstFrame, true);
    }
    bool cancel() noexcept { return publish(0, false); }

    bool publish(std::uint64_t frame, bool enabled) noexcept
    {
        if (stopped_.load(std::memory_order_relaxed)) return false;
        if (ownerSerial_ == maximumSerial) {
            ownerEnabled_ = false;
            ownerError_ = EOVERFLOW;
            return false;
        }
        // The previous synchronous borrow has ended before this function.
        // Invalidate locally BEFORE the worker may acquire the new mailbox.
        ownerEnabled_ = false;
        ++ownerSerial_;
        readySerial_.store(0, std::memory_order_release);
        requestVersion_.store(ownerSerial_ * 2U - 1U, std::memory_order_seq_cst);
        requestFrame_.store(frame, std::memory_order_seq_cst);
        requestEnabled_.store(enabled, std::memory_order_seq_cst);
        requestVersion_.store(ownerSerial_ * 2U, std::memory_order_seq_cst);
        ownerEnabled_ = enabled;
        return true;
    }

    const View* view() const noexcept
    {
        if (!ownerEnabled_ || stopped_.load(std::memory_order_relaxed) ||
            readySerial_.load(std::memory_order_acquire) != ownerSerial_)
            return nullptr;
        return published_.load(std::memory_order_relaxed);
    }
    int errorCode() const noexcept
    {
        if (ownerError_) return ownerError_;
        if (!ownerEnabled_ || stopped_.load(std::memory_order_relaxed) ||
            readySerial_.load(std::memory_order_acquire) != ownerSerial_)
            return 0;
        return workerError_.load(std::memory_order_relaxed);
    }
    std::uint64_t residentBytes() const noexcept { return resident_.load(std::memory_order_relaxed); }

    void stop() noexcept
    {
        if (stopped_.exchange(true, std::memory_order_relaxed)) return;
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
        ownerEnabled_ = false;
        published_.store(nullptr, std::memory_order_relaxed);
        current_.reset();
        resident_.store(0, std::memory_order_relaxed);
        closeFiles();
    }

private:
    struct Request { std::uint64_t serial = 0, frame = 0; bool enabled = false; };

    bool readRequest(Request& request) const noexcept
    {
        const auto before = requestVersion_.load(std::memory_order_seq_cst);
        if (before & 1U) return false;
        request.frame = requestFrame_.load(std::memory_order_seq_cst);
        request.enabled = requestEnabled_.load(std::memory_order_seq_cst);
        if (requestVersion_.load(std::memory_order_seq_cst) != before) return false;
        request.serial = before / 2U;
        return true;
    }
    bool obsolete(const Request& request, std::stop_token stop) const noexcept
    {
        return stop.stop_requested() || requestVersion_.load(std::memory_order_seq_cst) != request.serial * 2U;
    }
    int unchanged(std::size_t leaf) const noexcept
    {
        struct stat now{};
        if (fstat(fds_[leaf], &now) != 0) return errno;
        return sameFile(identities_[leaf], now) ? 0 : ESTALE;
    }
    int build(const Request& request, std::stop_token stop)
    {
        current_ = std::make_unique<View>();
        current_->firstFrame = request.frame;
        current_->frames = std::min(maxFrames, frames_ - request.frame);
        current_->leafCount = leafCount_;
        const auto samples = std::size_t(current_->frames * 2U);
        std::uint64_t allocated = 0;
        for (std::size_t leaf = 0; leaf < leafCount_; ++leaf) {
            if (obsolete(request, stop)) return ECANCELED;
            if (const int code = unchanged(leaf)) return code;
            auto& output = current_->leaves[leaf];
            output.resize(samples);
            allocated += output.capacity() * sizeof(float);
            resident_.store(allocated, std::memory_order_relaxed);
            if (allocated > maxBytes) return EOVERFLOW;
            for (std::uint64_t frame = 0; frame < current_->frames;) {
                if (obsolete(request, stop)) return ECANCELED;
                const auto count = std::size_t(std::min<std::uint64_t>(readFrames, current_->frames - frame));
                const auto bytes = count * frameBytes;
                std::size_t received = 0;
                auto* destination = reinterpret_cast<char*>(output.data() + frame * 2U);
                while (received < bytes) {
                    if (obsolete(request, stop)) return ECANCELED;
                    const auto got = pread(fds_[leaf], destination + received, bytes - received,
                        off_t((request.frame + frame) * frameBytes + received));
                    if (got < 0 && errno == EINTR) continue;
                    if (got <= 0) return got < 0 ? errno : EIO;
                    received += std::size_t(got);
                }
                if (obsolete(request, stop)) return ECANCELED;
                for (std::size_t sample = 0; sample < count * 2U; ++sample) {
                    if (sample % 256U == 0 && obsolete(request, stop)) return ECANCELED;
                    if (!std::isfinite(output[std::size_t(frame) * 2U + sample])) return EILSEQ;
                }
                if (obsolete(request, stop)) return ECANCELED;
                frame += count;
            }
            if (const int code = unchanged(leaf)) return code;
        }
        return 0;
    }
    void run(std::stop_token stop) noexcept
    {
        std::uint64_t handled = 0;
        while (!stop.stop_requested()) {
            Request request;
            if (!readRequest(request) || request.serial == handled) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            // The acquired newer even publication follows the owner's last
            // borrow. Only now may this worker destroy the previous View.
            published_.store(nullptr, std::memory_order_relaxed);
            current_.reset();
            resident_.store(0, std::memory_order_relaxed);
            handled = request.serial;
            int code = 0;
            if (request.enabled) {
                try { code = build(request, stop); }
                catch (const std::bad_alloc&) { code = ENOMEM; }
                catch (...) { code = EIO; }
            }
            if (obsolete(request, stop)) continue;
            if (code) {
                // This failed candidate was never published or borrowed.
                current_.reset();
                resident_.store(0, std::memory_order_relaxed);
            }
            workerError_.store(code, std::memory_order_relaxed);
            published_.store(current_.get(), std::memory_order_relaxed);
            readySerial_.store(request.serial, std::memory_order_release);
        }
    }
    void closeFiles() noexcept
    {
        for (auto& fd : fds_) {
            if (fd >= 0) close(fd);
            fd = -1;
        }
    }

    const std::uint64_t frames_;
    const std::size_t leafCount_;
    std::array<int, 4> fds_{-1, -1, -1, -1};
    std::array<struct stat, 4> identities_{};
    std::atomic<std::uint64_t> requestVersion_{0}, requestFrame_{0}, readySerial_{0}, resident_{0};
    std::atomic<bool> requestEnabled_{false}, stopped_{false};
    std::atomic<int> workerError_{0};
    std::atomic<const View*> published_{nullptr};
    std::uint64_t ownerSerial_ = 0;
    bool ownerEnabled_ = false;
    int ownerError_ = 0;
    std::unique_ptr<View> current_;
    std::jthread worker_;
};

PreparedLoopCache::PreparedLoopCache(std::span<const int> fds, std::uint64_t frames)
    : impl_(std::make_unique<Impl>(fds, frames)) {}
PreparedLoopCache::~PreparedLoopCache() = default;
bool PreparedLoopCache::request(std::uint64_t frame) noexcept { return impl_->request(frame); }
bool PreparedLoopCache::cancel() noexcept { return impl_->cancel(); }
const PreparedLoopCache::View* PreparedLoopCache::view() const noexcept { return impl_->view(); }
int PreparedLoopCache::errorCode() const noexcept { return impl_->errorCode(); }
std::uint64_t PreparedLoopCache::residentBytes() const noexcept { return impl_->residentBytes(); }
void PreparedLoopCache::stop() noexcept { impl_->stop(); }

} // namespace conflux::audio

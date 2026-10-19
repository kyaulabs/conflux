// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/StemBankSource.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>

namespace conflux::audio {
namespace {
using Clock = std::chrono::steady_clock;
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(PreparedPlayback::stemCount == StemBankSource::stemCount);
void validateGeometry(int fd, std::uint64_t frames)
{
    constexpr std::uint64_t frameBytes = 2 * sizeof(float);
    if (!frames || frames > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / frameBytes)
        throw std::invalid_argument("Prepared playback requires nonempty addressable stereo PCM");
    struct stat status{};
    if (fstat(fd, &status) != 0)
        throw std::system_error(errno, std::generic_category(), "Cannot inspect prepared playback PCM");
    if (!S_ISREG(status.st_mode) || status.st_size < 0 || std::uint64_t(status.st_size) != frames * frameBytes)
        throw std::invalid_argument("Prepared playback requires exact regular stereo PCM geometry");
}
}

class PreparedPlayback::LiveState {
public:
    LiveState(std::uint64_t identity, std::uint64_t frames, LiveProducer callback)
        : buffer(identity, frames), producer(std::move(callback)) {}
    void fail(int code) noexcept
    {
        if (code <= 0) code = EINVAL;
        {
            const std::lock_guard lock(mutex);
            int empty = 0;
            (void)health.compare_exchange_strong(empty, code, std::memory_order_relaxed);
        }
        condition.notify_all();
    }
    LiveStemBuffer buffer;
    LiveProducer producer;
    std::atomic<int> health{0};
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable condition;
    // Only the producer/control waiter use these, under the same mutex. Render
    // never locks or signals this condition, including during discontinuity.
    std::uint64_t publishedEpoch = 0, publishedEnd = 0;
    std::jthread thread; // First destroyed, before callable/borrowed buffer state.
};

PreparedPlayback::LiveProducerPort::LiveProducerPort(LiveState& state) noexcept : state_(&state) {}

bool PreparedPlayback::LiveProducerPort::tryRequest(LiveStemBuffer::Request& request) const noexcept
{
    return !state_->stopping.load(std::memory_order_relaxed) &&
        !state_->health.load(std::memory_order_relaxed) && state_->buffer.tryRequest(request);
}

LiveStemBuffer::PublishResult PreparedPlayback::LiveProducerPort::tryPublish(
    const LiveStemBuffer::Packet& packet) noexcept
{
    if (state_->stopping.load(std::memory_order_relaxed) || state_->health.load(std::memory_order_relaxed))
        return LiveStemBuffer::PublishResult::Invalid;
    const auto result = state_->buffer.tryPublish(packet);
    if (result == LiveStemBuffer::PublishResult::Accepted) {
        {
            const std::lock_guard lock(state_->mutex);
            state_->publishedEpoch = packet.epoch;
            state_->publishedEnd = packet.firstFrame + packet.frames;
        }
        state_->condition.notify_all();
    }
    return result;
}

void PreparedPlayback::LiveProducerPort::fail(int errorCode) noexcept { state_->fail(errorCode); }

PreparedPlayback::PreparedPlayback(std::unique_ptr<LiveState> live, std::uint64_t frames)
    : live_(std::move(live)), frames_(frames)
{
    bank_ = std::make_unique<StemBankSource>(live_->buffer.sourceRefs());
}

std::unique_ptr<PreparedPlayback> PreparedPlayback::makeLive(
    std::uint64_t sourceId, std::uint64_t exactFrames, LiveProducer producer)
{
    if (!producer) throw std::invalid_argument("Live playback requires an owned producer callable");
    auto owner = std::unique_ptr<PreparedPlayback>(new PreparedPlayback(
        std::make_unique<LiveState>(sourceId, exactFrames, std::move(producer)), exactFrames));
    auto* state = owner->live_.get();
    state->thread = std::jthread([state](std::stop_token stop) {
        LiveProducerPort port(*state);
        try { state->producer(stop, port); }
        catch (...) { port.fail(EIO); }
        if (!stop.stop_requested() && !state->stopping.load(std::memory_order_relaxed)) port.fail(EPIPE);
    });
    // jthread transfer/unique_ptr return are noexcept; nothing can throw after
    // the live thread has transferred into its completely constructed owner.
    return owner;
}

PreparedPlayback::PreparedPlayback(int normalizedFd, std::uint64_t frames) : frames_(frames)
{
    validateGeometry(normalizedFd, frames);
    readers_[0] = std::make_unique<PcmReadAhead>(normalizedFd, frames);
}

PreparedPlayback::PreparedPlayback(std::array<int, stemCount> normalizedFds, std::uint64_t frames)
    : frames_(frames)
{
    std::array<AudioSource, stemCount> sources{};
    for (std::size_t stem = 0; stem < stemCount; ++stem) {
        validateGeometry(normalizedFds[stem], frames);
        readers_[stem] = std::make_unique<PcmReadAhead>(normalizedFds[stem], frames);
        sources[stem] = readers_[stem]->sourceRef();
    }
    bank_ = std::make_unique<StemBankSource>(sources);
}

PreparedPlayback::~PreparedPlayback() { stop(); }

AudioSource PreparedPlayback::sourceRef() noexcept
{
    return bank_ ? bank_->sourceRef() : readers_[0]->sourceRef();
}

void PreparedPlayback::resetBankForSession()
{
    if (stopped_) throw std::logic_error("Cannot reuse a stopped playback owner");
    if (!bank_) return;
    std::array<AudioSource, stemCount> sources{};
    if (live_) sources = live_->buffer.sourceRefs();
    else
        for (std::size_t stem = 0; stem < stemCount; ++stem)
            sources[stem] = readers_[stem]->sourceRef();
    // Construct before replacing: a failed allocation leaves every existing
    // bank, view, producer and health field intact. This never restarts an owner.
    auto replacement = std::make_unique<StemBankSource>(sources);
    bank_ = std::move(replacement);
}

bool PreparedPlayback::prime(std::uint64_t frame, std::chrono::milliseconds timeout, std::stop_token stop)
{
    const auto now = Clock::now();
    if (live_) timeout = std::min(timeout, std::chrono::duration_cast<std::chrono::milliseconds>(liveWait));
    if (frame > frames_ || timeout.count() < 0 ||
        timeout > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now)) {
        primeError_.store(EINVAL, std::memory_order_relaxed);
        return false;
    }
    if (stopped_ || stop.stop_requested()) {
        primeError_.store(ECANCELED, std::memory_order_relaxed);
        return false;
    }
    primeError_.store(0, std::memory_order_relaxed);
    if (live_ && live_->health.load(std::memory_order_relaxed)) return false;
    if (bank_) {
        const auto source = bank_->sourceRef();
        source.discontinuity(source.context, frame);
    }
    const auto deadline = now + timeout;
    if (live_) {
        LiveStemBuffer::Request request;
        if (!live_->buffer.tryRequest(request) || request.firstFrame != frame) {
            primeError_.store(EIO, std::memory_order_relaxed);
            return false;
        }
        const auto neededEnd = frame + std::min(liveReserveFrames, frames_ - frame);
        auto* state = live_.get();
        // Register before locking: an already-cancelled token can invoke this
        // callback synchronously. Its lock serializes notify with wait setup.
        std::stop_callback cancellation(stop, [state] {
            const std::lock_guard lock(state->mutex);
            state->condition.notify_all();
        });
        std::unique_lock lock(state->mutex);
        auto ready = [&] {
            return state->health.load(std::memory_order_relaxed) || stop.stop_requested() ||
                state->stopping.load(std::memory_order_relaxed) || neededEnd == frame ||
                (state->publishedEpoch == request.epoch && state->publishedEnd >= neededEnd);
        };
        (void)state->condition.wait_until(lock, deadline, ready);
        if (state->health.load(std::memory_order_relaxed)) return false;
        if (stop.stop_requested() || state->stopping.load(std::memory_order_relaxed)) {
            primeError_.store(ECANCELED, std::memory_order_relaxed);
            return false;
        }
        if (neededEnd == frame || (state->publishedEpoch == request.epoch && state->publishedEnd >= neededEnd))
            return true;
        primeError_.store(ETIMEDOUT, std::memory_order_relaxed);
        return false;
    }
    for (const auto& reader : readers_) {
        if (!reader) continue;
        if (stop.stop_requested()) { primeError_.store(ECANCELED, std::memory_order_relaxed); return false; }
        const auto current = Clock::now();
        const auto remaining = current < deadline
            ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - current)
            : std::chrono::milliseconds(0);
        if (!reader->prime(frame, remaining)) {
            const auto code = reader->errorCode();
            primeError_.store(code ? code : ETIMEDOUT, std::memory_order_relaxed);
            return false;
        }
    }
    if (stop.stop_requested()) { primeError_.store(ECANCELED, std::memory_order_relaxed); return false; }
    return true;
}

bool PreparedPlayback::configureStemRamps(std::uint32_t frames) noexcept
{
    if (stopped_ || frames > StemBankSource::sampleRate) return false;
    return !bank_ || bank_->configureRamps(frames);
}

int PreparedPlayback::errorCode() const noexcept
{
    if (live_)
        if (const auto code = live_->health.load(std::memory_order_relaxed)) return code;
    if (const auto code = primeError_.load(std::memory_order_relaxed)) return code;
    for (const auto& reader : readers_)
        if (reader)
            if (const auto code = reader->errorCode()) return code;
    return 0;
}

std::string PreparedPlayback::error() const
{
    std::string result;
    if (live_)
        if (const auto code = live_->health.load(std::memory_order_relaxed))
            return "Live stem producer: " + std::generic_category().message(code);
    if (const auto code = primeError_.load(std::memory_order_relaxed))
        result = "Playback priming: " + std::generic_category().message(code);
    for (std::size_t stem = 0; stem < readers_.size(); ++stem) {
        if (!readers_[stem] || !readers_[stem]->errorCode()) continue;
        if (!result.empty()) result += "; ";
        result += bank_ ? "Stem " + std::to_string(stem) + ": " : "Source: ";
        result += readers_[stem]->error();
    }
    return result;
}

void PreparedPlayback::stop() noexcept
{
    if (stopped_) return;
    stopped_ = true;
    if (live_) {
        {
            const std::lock_guard lock(live_->mutex);
            live_->stopping.store(true, std::memory_order_relaxed);
        }
        live_->condition.notify_all();
        // request_stop may run user stop callbacks synchronously. This whole
        // owner belongs on the off-GUI retirement worker after render quiescence.
        live_->thread.request_stop();
        if (live_->thread.joinable()) live_->thread.join();
    }
    for (auto& reader : readers_)
        if (reader) reader->stop();
}
} // namespace conflux::audio

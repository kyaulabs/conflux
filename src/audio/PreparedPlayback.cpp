// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/StemBankSource.hpp"
#include "audio/LiveRecoveryPolicy.hpp"

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
using Recovery = detail::LiveRecoveryPolicy;
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(PreparedPlayback::liveReserveFrames == Recovery::reserveFrames);
static_assert(PreparedPlayback::liveWait == Recovery::maximumWait);
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
        : buffer(identity, frames), producer(std::move(callback)), policy(frames) {}
    [[nodiscard]] int code() const noexcept
    {
        if (const auto error = health.load(std::memory_order_relaxed)) return error;
        return Recovery::timedOut(attempt.load(std::memory_order_seq_cst)) ? ETIMEDOUT : 0;
    }
    // No callback lock/notification. A producer observes this before its next
    // service/publication; the exceptional counter exhaustion stays terminal.
    void failRender(int code) noexcept
    {
        int empty = 0;
        (void)health.compare_exchange_strong(empty, code, std::memory_order_relaxed);
    }
    void fail(int code) noexcept
    {
        if (code <= 0) code = EINVAL;
        {
            const std::lock_guard lock(mutex);
            failRender(code);
        }
        condition.notify_all();
    }
    bool advance() noexcept
    {
        if (code()) return false;
        if (demand.token == Recovery::maximumToken) { failRender(EOVERFLOW); return false; }
        if (!Recovery::advanceAttempt(attempt, demand.token, demand.token + 1)) return false;
        ++demand.token;
        return true;
    }
    void publishDemand() noexcept
    {
        // All payloads are atomic: a rejected seqlock snapshot must not race
        // plain fields. New attempt is already visible before this mailbox.
        version.store(demand.token * 2 - 1, std::memory_order_seq_cst);
        wait.store(demand.waitSerial, std::memory_order_seq_cst);
        epoch.store(demand.epoch, std::memory_order_seq_cst);
        cursor.store(demand.cursor, std::memory_order_seq_cst);
        active.store(demand.active, std::memory_order_seq_cst);
        version.store(demand.token * 2, std::memory_order_seq_cst);
    }
    bool snapshot(Recovery::Demand& output) const noexcept
    {
        const auto before = version.load(std::memory_order_seq_cst);
        if (before & 1U) return false;
        Recovery::Demand result{before / 2, wait.load(std::memory_order_seq_cst),
            epoch.load(std::memory_order_seq_cst), cursor.load(std::memory_order_seq_cst),
            active.load(std::memory_order_seq_cst)};
        if (before != version.load(std::memory_order_seq_cst) ||
            attempt.load(std::memory_order_seq_cst) != result.token) return false;
        output = result;
        return true;
    }
    bool closeGate() noexcept
    {
        if (nextWait == std::numeric_limits<std::uint64_t>::max()) {
            failRender(EOVERFLOW); return false;
        }
        if (!advance()) return false;
        demand.waitSerial = ++nextWait;
        return true;
    }
    bool openGate() noexcept
    {
        if (!advance()) return false;
        demand.waitSerial = 0;
        publishDemand();
        return true;
    }
    void setActive(bool value) noexcept
    {
        if (demand.active == value || !advance()) return;
        demand.active = value;
        publishDemand();
    }
    LiveStemBuffer buffer;
    LiveProducer producer;
    Recovery policy; // Sole producer; never touched by rendering or priming.
    std::atomic<int> health{0};
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> attempt{1}, grant{0};
    std::atomic<std::uint64_t> version{2}, wait{1}, epoch{1}, cursor{0};
    std::atomic<bool> active{false};
    // Render owner only, including quiesced prime/reset/lifecycle disarm.
    Recovery::Demand demand{1, 1, 1, 0, false};
    std::uint64_t nextWait = 1;
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
        !state_->code() && state_->buffer.tryRequest(request);
}

LiveStemBuffer::PublishResult PreparedPlayback::LiveProducerPort::tryPublish(
    const LiveStemBuffer::Packet& packet) noexcept
{
    if (state_->stopping.load(std::memory_order_relaxed) || state_->code())
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

bool PreparedPlayback::LiveProducerPort::serviceRecovery() noexcept
{
    if (state_->stopping.load(std::memory_order_relaxed) || state_->code()) return false;
    Recovery::Demand demand;
    if (!state_->snapshot(demand)) return !state_->code();
    Recovery::Coverage coverage;
    {
        const std::lock_guard lock(state_->mutex);
        coverage = {state_->publishedEpoch, state_->publishedEnd};
    }
    const auto decision = state_->policy.update(demand, coverage, Clock::now());
    if (decision.kind == Recovery::Decision::Kind::Grant) {
        // A stale serial is harmless even if rendering advances after this
        // check: only that exact never-reused wait can consume the grant.
        if (!state_->code() && state_->attempt.load(std::memory_order_seq_cst) == decision.token)
            state_->grant.store(decision.waitSerial, std::memory_order_release);
    } else if (decision.kind == Recovery::Decision::Kind::Timeout &&
               Recovery::commitTimeout(state_->attempt, decision)) {
        state_->fail(ETIMEDOUT);
    }
    return !state_->stopping.load(std::memory_order_relaxed) && !state_->code();
}

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
    if (!live_) return bank_ ? bank_->sourceRef() : readers_[0]->sourceRef();
    return {frames_, nullptr, this,
        [](void* context, std::uint64_t frame, std::span<float> output) noexcept {
            return static_cast<PreparedPlayback*>(context)->readLive(frame, output);
        },
        [](void* context, std::uint64_t frame) noexcept {
            static_cast<PreparedPlayback*>(context)->seekLive(frame);
        },
        [](void* context, std::size_t stem, float gain) noexcept {
            return static_cast<PreparedPlayback*>(context)->bank_->setGain(stem, gain);
        },
        [](const void* context, std::size_t stem) noexcept {
            return static_cast<const PreparedPlayback*>(context)->bank_->gain(stem);
        },
        [](void* context, bool active) noexcept {
            static_cast<PreparedPlayback*>(context)->setLiveActive(active);
        }};
}

AudioReadResult PreparedPlayback::readLive(std::uint64_t frame, std::span<float> output) noexcept
{
    if (output.size() % StemBankSource::channels ||
        output.size() / StemBankSource::channels > StemBankSource::blockFrames ||
        frame > frames_ || frame != live_->demand.cursor) return {0, true};
    if (live_->code() || live_->stopping.load(std::memory_order_relaxed)) return {0, true};
    if (output.empty() || frame == frames_) return {};
    if (live_->demand.waitSerial) {
        if (live_->grant.load(std::memory_order_acquire) != live_->demand.waitSerial) return {};
        if (!live_->openGate()) return {0, true};
    }
    const auto source = bank_->sourceRef();
    const auto result = source.read(source.context, frame, output);
    live_->demand.cursor += result.frames;
    const auto wanted = std::min<std::uint64_t>(output.size() / StemBankSource::channels, frames_ - frame);
    if (!result.failed && result.frames < wanted) {
        // Keep the bank's faster-view surplus and the exact common cursor. An
        // underrun is not a seek and must not reset provider/converter epochs.
        if (!live_->closeGate()) return {result.frames, true};
        live_->publishDemand();
    }
    return result;
}

void PreparedPlayback::seekLive(std::uint64_t frame) noexcept
{
    if (frame > frames_ || live_->stopping.load(std::memory_order_relaxed) || !live_->closeGate()) return;
    // Token invalidation precedes the bounded bank/provider work, so an old
    // deadline cannot win after this new attempt has already superseded it.
    const auto source = bank_->sourceRef();
    source.discontinuity(source.context, frame);
    LiveStemBuffer::Request request;
    if (!live_->buffer.tryRequest(request) || request.firstFrame != frame) {
        live_->failRender(EIO); return;
    }
    live_->demand.epoch = request.epoch;
    live_->demand.cursor = frame;
    live_->publishDemand();
}

void PreparedPlayback::setLiveActive(bool active) noexcept { live_->setActive(active); }

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
    if (live_) setLiveActive(false);
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
    if (live_ && live_->code()) return false;
    if (live_) {
        setLiveActive(false);
        seekLive(frame);
        if (live_->code()) return false;
    } else if (bank_) {
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
            return state->code() || stop.stop_requested() ||
                state->stopping.load(std::memory_order_relaxed) || neededEnd == frame ||
                (state->publishedEpoch == request.epoch && state->publishedEnd >= neededEnd);
        };
        (void)state->condition.wait_until(lock, deadline, ready);
        if (state->code()) return false;
        if (stop.stop_requested() || state->stopping.load(std::memory_order_relaxed)) {
            primeError_.store(ECANCELED, std::memory_order_relaxed);
            return false;
        }
        if (neededEnd == frame || (state->publishedEpoch == request.epoch && state->publishedEnd >= neededEnd))
            return state->openGate();
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
        if (const auto code = live_->code()) return code;
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
        if (const auto code = live_->code())
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
        setLiveActive(false); // Render already quiesced; never clears health.
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

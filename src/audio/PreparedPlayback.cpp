// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/PreparedLoopCache.hpp"
#include "audio/PreparedTempoSource.hpp"
#include "audio/StemBankSource.hpp"
#include "audio/LiveRecoveryPolicy.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <new>
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
static_assert(PreparedTempoSource::blockFrames == StemBankSource::blockFrames);
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
    const std::array normalizedFds{normalizedFd};
    createLoopCache(normalizedFds);
    createTempoSource(normalizedFds);
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
    createLoopCache(normalizedFds);
    createTempoSource(normalizedFds);
}

PreparedPlayback::~PreparedPlayback() { stop(); }

void PreparedPlayback::createLoopCache(std::span<const int> fds) noexcept
{
    // Optional loop resources must not turn a healthy linear owner into a
    // failed load. Cache construction and worker creation are off callback.
    try {
        loopCache_ = std::make_unique<PreparedLoopCache>(fds, frames_);
    } catch (const std::system_error& error) {
        loopSetupError_ = error.code().value() > 0 ? error.code().value() : EIO;
    } catch (const std::bad_alloc&) {
        loopSetupError_ = ENOMEM;
    } catch (const std::invalid_argument&) {
        loopSetupError_ = EINVAL;
    } catch (...) {
        loopSetupError_ = EIO;
    }
}

void PreparedPlayback::createTempoSource(std::span<const int> fds) noexcept
{
    // The optional independent worker owns its duplicated FDs and raw slots;
    // failure leaves the existing unity readers, bank and loops usable.
    try {
        tempoSource_ = std::make_unique<PreparedTempoSource>(fds, frames_);
    } catch (const std::system_error& error) {
        tempoSetupError_ = error.code().value() > 0 ? error.code().value() : EIO;
    } catch (const std::bad_alloc&) {
        tempoSetupError_ = ENOMEM;
    } catch (const std::invalid_argument&) {
        tempoSetupError_ = EINVAL;
    } catch (...) {
        tempoSetupError_ = EIO;
    }
}

AudioSource PreparedPlayback::sourceRef() noexcept
{
    if (!live_) {
        AudioSource source{frames_, nullptr, this,
            [](void* context, std::uint64_t frame, std::span<float> output) noexcept {
                return static_cast<PreparedPlayback*>(context)->readPrepared(frame, output);
            },
            [](void* context, std::uint64_t frame) noexcept {
                static_cast<PreparedPlayback*>(context)->seekPrepared(frame);
            }};
        if (bank_) {
            source.setStemGain = [](void* context, std::size_t stem, float gain) noexcept {
                return static_cast<PreparedPlayback*>(context)->bank_->setGain(stem, gain);
            };
            source.stemGain = [](const void* context, std::size_t stem) noexcept {
                return static_cast<const PreparedPlayback*>(context)->bank_->gain(stem);
            };
        }
        source.loopControl = [](void* context, AudioLoopCommand command, std::uint64_t frame) noexcept {
            return static_cast<PreparedPlayback*>(context)->loopControl(command, frame);
        };
        source.loopState = [](const void* context) noexcept {
            return static_cast<const PreparedPlayback*>(context)->loopState();
        };
        source.tempoControl = [](void* context, AudioTempoCommand command, double percent,
                                 bool keyLock, std::uint64_t frame) noexcept {
            return static_cast<PreparedPlayback*>(context)->tempoControl(command, percent, keyLock, frame);
        };
        source.tempoState = [](const void* context) noexcept {
            return static_cast<const PreparedPlayback*>(context)->tempoState();
        };
        return source;
    }
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

int PreparedPlayback::preparedRenderError() const noexcept
{
    if (const int code = errorCode()) return code;
    return bank_ && bank_->hasReadFailure() ? EIO : 0;
}

AudioLoopState PreparedPlayback::loopState() const noexcept
{
    auto state = loop_;
    state.supported = !live_ && bool(loopCache_) && !tempoState().active;
    state.errorCode = loopSetupError_;
    if (loopCache_) {
        state.residentBytes = loopCache_->residentBytes();
        if (const int code = loopCache_->errorCode()) state.errorCode = code;
        const auto* view = loopCache_->view();
        state.ready = state.inSet && view && view->firstFrame == state.inFrame &&
            view->frames == state.limitFrame - state.inFrame && view->leafCount == (bank_ ? stemCount : 1);
    }
    if (const int code = preparedRenderError()) state.errorCode = code;
    state.ready = state.ready && !state.errorCode;
    return state;
}

AudioTempoState PreparedPlayback::tempoState() const noexcept
{
    AudioTempoState state;
    state.keyLock = tempoKeyLock_;
    state.errorCode = tempoSetupError_;
    if (tempoSource_) {
        state = tempoSource_->status();
        state.supported = !stopped_;
        if (!state.active) state.keyLock = tempoKeyLock_;
    }
    return state;
}

bool PreparedPlayback::tempoControl(AudioTempoCommand command, double percent, bool keyLock,
                                    std::uint64_t frame) noexcept
{
    if (live_ || stopped_ || !tempoSource_ || frame > frames_ ||
        !std::isfinite(percent) || percent < -6 || percent > 6)
        return false;
    const auto state = tempoSource_->status();
    if (command == AudioTempoCommand::Reset) {
        if (percent != 0) return false;
        if (state.active) {
            if (!tempoSource_->cancel()) return false;
            // The legacy bank/readers may still be parked at a pre-tempo
            // cursor. This is an asynchronous refill, never a stale resume.
            seekLinear(frame);
        }
        return true; // Keep the selected Master Tempo setting at legacy zero.
    }
    if (command != AudioTempoCommand::Set) return false;
    if (state.active) {
        bool accepted = state.keyLock == keyLock
            ? tempoSource_->requestSpeed(percent)
            : tempoSource_->request(frame, percent, keyLock);
        // A mode/session reset at zero creates a fresh exact-unity backend.
        // Its first nonzero speed needs explicit setup, unlike an active
        // SRC/R3 stream crossing zero, which requestSpeed already accepts.
        if (!accepted && state.keyLock == keyLock && state.percent == 0 && percent != 0 && !state.errorCode)
            accepted = tempoSource_->request(frame, percent, keyLock);
        if (accepted) tempoKeyLock_ = keyLock;
        return accepted;
    }
    if (loop_.inSet || loop_.outSet || loop_.cached || preparedRenderError()) return false;
    if (percent == 0) {
        tempoKeyLock_ = keyLock;
        return true;
    }
    if (!tempoSource_->request(frame, percent, keyLock)) return false;
    tempoKeyLock_ = keyLock;
    return true;
}

void PreparedPlayback::seekLinear(std::uint64_t frame) noexcept
{
    const auto source = bank_ ? bank_->sourceRef() : readers_[0]->sourceRef();
    source.discontinuity(source.context, frame);
}

void PreparedPlayback::clearLoop() noexcept
{
    if (!loop_.inSet && !loop_.outSet && !loop_.cached) return;
    // Invalidate every cached borrow before the worker may retire its storage.
    loop_ = {};
    if (loopCache_) (void)loopCache_->cancel();
}

void PreparedPlayback::seekPrepared(std::uint64_t frame) noexcept
{
    if (frame > frames_) return;
    clearLoop();
    seekLinear(frame);
    if (tempoSource_) {
        const auto state = tempoSource_->status();
        if (state.active) (void)tempoSource_->request(frame, state.percent, state.keyLock);
    }
}

bool PreparedPlayback::loopControl(AudioLoopCommand command, std::uint64_t frame) noexcept
{
    if (live_ || stopped_ || frame > frames_) return false;
    if (command == AudioLoopCommand::Clear) {
        const bool wasCached = loop_.cached;
        clearLoop();
        // Clear can precede a same-frame seek, for which RenderCore need not
        // send another discontinuity. Restore the parked reader here as well.
        if (wasCached) seekLinear(frame);
        return true;
    }
    // Crossing zero inside SRC/R3 preserves queued/filter state and remains
    // processed mode until an explicit reset restores the legacy source.
    if (tempoState().active) return false;
    if (!loopCache_) return false;
    if (command == AudioLoopCommand::In) {
        if (frame == frames_ || preparedRenderError()) return false;
        const bool wasCached = loop_.cached;
        loop_ = {};
        if (wasCached) seekLinear(frame);
        if (!loopCache_->request(frame)) return false;
        loop_.inFrame = frame;
        loop_.limitFrame = frame + std::min(PreparedLoopCache::maxFrames, frames_ - frame);
        loop_.inSet = true;
        return true;
    }
    if (command == AudioLoopCommand::Out) {
        if (loop_.outSet || !loopState().ready || frame <= loop_.inFrame ||
            frame > loop_.limitFrame || frame - loop_.inFrame < AudioLoopState::minimumFrames)
            return false;
        loop_.outFrame = frame;
        loop_.outSet = loop_.active = loop_.cached = true;
        return true;
    }
    if (command == AudioLoopCommand::Toggle && loop_.outSet) {
        if (loop_.active) {
            loop_.active = false;
            return true;
        }
        if (!loopState().ready) return false;
        // Before the cached EXIT remainder ends, the original bank still owns
        // its OUT staging. Only a resumed linear reader needs one new seek.
        if (!loop_.cached) seekLinear(loop_.outFrame);
        loop_.active = loop_.cached = true;
        return true;
    }
    return false;
}

AudioReadResult PreparedPlayback::readPrepared(std::uint64_t frame, std::span<float> output) noexcept
{
    if (output.size() % StemBankSource::channels || frame > frames_) return {0, true};
    if (tempoState().active) return readTempo(output);
    if (output.empty()) return {};
    if (loop_.cached) {
        if (frame < loop_.inFrame || frame > loop_.outFrame) return {0, true};
        if (frame == loop_.outFrame) {
            if (loop_.active) return {0, true}; // Natural wraps are render-owned.
            loop_.cached = false;
        } else {
            if (preparedRenderError()) return {0, true};
            const auto* view = loopCache_->view();
            if (!view || view->firstFrame != loop_.inFrame ||
                loop_.outFrame - loop_.inFrame > view->frames ||
                view->leafCount != (bank_ ? stemCount : 1)) return {0, true};
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                output.size() / StemBankSource::channels, loop_.outFrame - frame));
            auto clipped = output.first(count * StemBankSource::channels);
            const auto offset = frame - loop_.inFrame;
            if (bank_) {
                std::array<std::span<const float>, stemCount> leaves{};
                for (std::size_t stem = 0; stem < stemCount; ++stem) leaves[stem] = view->leaves[stem];
                return bank_->readCached(leaves, offset, clipped);
            }
            std::copy_n(view->leaves[0].data() + offset * StemBankSource::channels,
                        clipped.size(), clipped.data());
            return {count, false};
        }
    }
    const auto source = bank_ ? bank_->sourceRef() : readers_[0]->sourceRef();
    return source.read(source.context, frame, output);
}

AudioReadResult PreparedPlayback::readTempo(std::span<float> output) noexcept
{
    AudioReadResult result;
    result.positionValid = true;
    result.nominalPosition = tempoSource_->nominalPosition();
    result.finished = tempoSource_->status().finished;
    if (output.size() / StemBankSource::channels > PreparedTempoSource::blockFrames) {
        result.failed = true;
        return result;
    }
    if (output.empty()) return result;
    if (preparedRenderError()) { result.failed = true; return result; }
    // A DSP packet may be shorter than an output callback. Gather the actual
    // queued contiguous prefix, with a fixed ring-sized bound, before declaring
    // starvation. Mixing still uses the bank's one current gain-ramp state.
    for (std::size_t slot = 0; slot < PreparedTempoSource::slotCount &&
         result.frames < output.size() / StemBankSource::channels; ++slot) {
        const auto view = tempoSource_->peek();
        if (!view) break;
        if (view.generation != tempoSource_->generation()) { result.failed = true; break; }
        if (view.errorCode) {
            (void)tempoSource_->consume(0);
            result.failed = true;
            break;
        }
        if (view.terminal) {
            result.failed = !tempoSource_->consume(0);
            break;
        }
        const auto count = std::min(output.size() / StemBankSource::channels - result.frames, view.frames);
        auto clipped = output.subspan(result.frames * StemBankSource::channels, count * StemBankSource::channels);
        if (view.leafCount != (bank_ ? stemCount : 1)) { result.failed = true; break; }
        AudioReadResult part;
        if (bank_) part = bank_->readCached(view.leaves, 0, clipped);
        else {
            if (view.leaves[0].size() != view.frames * StemBankSource::channels) {
                result.failed = true;
                break;
            }
            std::copy_n(view.leaves[0].data(), clipped.size(), clipped.data());
            part.frames = count;
        }
        if (part.frames > count) { result.failed = true; break; }
        if (!tempoSource_->consume(part.frames)) { result.failed = true; break; }
        result.frames += part.frames;
        if (part.failed || part.frames != count) { result.failed = true; break; }
    }
    result.nominalPosition = tempoSource_->nominalPosition();
    result.finished = tempoSource_->status().finished;
    return result;
}

AudioReadResult PreparedPlayback::readLive(std::uint64_t frame, std::span<float> output) noexcept
{
    if (output.size() % StemBankSource::channels ||
        output.size() / StemBankSource::channels > StemBankSource::blockFrames ||
        frame > frames_ || frame != live_->demand.cursor) return {0, true};
    if (!output.empty()) unusedLivePrime_ = false;
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
    unusedLivePrime_ = false;
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

void PreparedPlayback::resetBankForSession() { resetBankForSessionAt(0); }

void PreparedPlayback::resetBankForSessionAt(std::uint64_t frame)
{
    if (stopped_) throw std::logic_error("Cannot reuse a stopped playback owner");
    if (!live_) clearLoop();
    if (!bank_) return;
    std::array<AudioSource, stemCount> sources{};
    if (live_) sources = live_->buffer.sourceRefs();
    else
        for (std::size_t stem = 0; stem < stemCount; ++stem)
            sources[stem] = readers_[stem]->sourceRef();
    // Construct before replacing: a failed allocation leaves every existing
    // bank, view, producer and health field intact. This never restarts an owner.
    auto replacement = std::make_unique<StemBankSource>(sources, frame);
    if (live_) setLiveActive(false);
    bank_ = std::move(replacement);
}

bool PreparedPlayback::prime(std::uint64_t frame, std::chrono::milliseconds timeout, std::stop_token stop)
{
    if (!live_) clearLoop();
    // An explicit attempt supersedes every older success, even when argument
    // validation or an already-cancelled token prevents a new request.
    unusedLivePrime_ = false;
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
    } else if (bank_ || tempoState().active) seekLinear(frame);
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
        if (neededEnd == frame || (state->publishedEpoch == request.epoch && state->publishedEnd >= neededEnd)) {
            if (!state->openGate()) return false;
            livePrimeFrame_ = frame;
            livePrimeEpoch_ = request.epoch;
            unusedLivePrime_ = true;
            return true;
        }
        primeError_.store(ETIMEDOUT, std::memory_order_relaxed);
        return false;
    }
    if (tempoSource_) {
        const auto state = tempoSource_->status();
        if (state.active) {
            // Preserve the whole owner's selected mode across quiesced
            // session installation, but invalidate its old queued generation.
            if (!tempoSource_->request(frame, state.percent, state.keyLock)) {
                const int code = tempoSource_->errorCode();
                primeError_.store(code ? code : EIO, std::memory_order_relaxed);
                return false;
            }
            for (;;) {
                if (stop.stop_requested()) {
                    primeError_.store(ECANCELED, std::memory_order_relaxed);
                    return false;
                }
                if (const int code = tempoSource_->errorCode()) {
                    primeError_.store(code, std::memory_order_relaxed);
                    return false;
                }
                const auto view = tempoSource_->peek();
                if (view.errorCode) {
                    primeError_.store(view.errorCode, std::memory_order_relaxed);
                    return false;
                }
                if (view && view.generation == tempoSource_->generation()) return true;
                const auto current = Clock::now();
                if (current >= deadline) {
                    primeError_.store(ETIMEDOUT, std::memory_order_relaxed);
                    return false;
                }
                std::this_thread::sleep_until(std::min(deadline, current + std::chrono::milliseconds(1)));
            }
        }
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

bool PreparedPlayback::primeForSession(std::uint64_t frame, std::chrono::milliseconds timeout, std::stop_token stop)
{
    if (!live_) clearLoop();
    // Validate the effective timeout before considering an already-ready bank.
    // Preserve ordinary prime's live cap and transient-error retry behavior.
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
    bool reuse = false;
    if (live_ && unusedLivePrime_ && livePrimeFrame_ == frame) {
        LiveStemBuffer::Request request;
        if (live_->buffer.tryRequest(request) && request.firstFrame == frame && request.epoch == livePrimeEpoch_ &&
            live_->demand.epoch == request.epoch && live_->demand.cursor == frame && !live_->demand.waitSerial) {
            const auto neededEnd = frame + std::min(liveReserveFrames, frames_ - frame);
            const std::lock_guard lock(live_->mutex); // Off-thread/quiesced only.
            reuse = !live_->code() && !live_->stopping.load(std::memory_order_relaxed) && !stop.stop_requested() &&
                (neededEnd == frame || (live_->publishedEpoch == request.epoch && live_->publishedEnd >= neededEnd));
        }
    }
    if (reuse) {
        // Initializing an empty bank at the retained cursor must not send a
        // provider discontinuity or consume any of the already-ready views.
        resetBankForSessionAt(frame);
        if (live_->code()) return false;
        if (stop.stop_requested() || live_->stopping.load(std::memory_order_relaxed)) {
            primeError_.store(ECANCELED, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
    resetBankForSession();
    return prime(frame, timeout, stop);
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
    if (tempoSource_)
        if (const auto code = tempoSource_->errorCode()) return code;
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
    if (tempoSource_)
        if (const auto code = tempoSource_->errorCode()) {
            if (!result.empty()) result += "; ";
            result += "Prepared tempo: " + std::generic_category().message(code);
        }
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
    unusedLivePrime_ = false;
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
    if (tempoSource_) tempoSource_->stop();
    if (loopCache_) {
        loop_ = {};
        loopCache_->stop();
    }
    for (auto& reader : readers_)
        if (reader) reader->stop();
}
} // namespace conflux::audio

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/PreparedScratchSource.hpp"
#include "audio/PreparedLoopCache.hpp"
#include "audio/PreparedTempoSource.hpp"
#include "audio/StemBankSource.hpp"
#include "audio/LiveRecoveryPolicy.hpp"
#include "audio/ResidentStemHold.hpp"

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
static_assert(PreparedScratchSource::maxReadFrames == StemBankSource::blockFrames);
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
    LiveState(std::uint64_t identity, std::uint64_t frames, LiveProducer callback,
              std::optional<AudioResidentHoldRange> range)
        : buffer(identity, frames), producer(std::move(callback)), policy(frames),
          hold(range ? std::make_unique<ResidentStemHold>(identity,frames,*range) : nullptr) {}
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
    std::unique_ptr<ResidentStemHold> hold;
    // Sole render owner; producer only touches hold's capture/release-ready.
    std::uint64_t holdOffset = 0;
    bool holdArmed = false, holdActive = false, holdExit = false, holdTransition = false;
    bool holdTempoAligned = false; // Render owner: compatible live output revision observed.
    std::atomic<int> health{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> tempoRenderActive{false};
    std::uint64_t tempoInputGeneration = 0; // Tempo worker, or quiesced control.
    std::unique_ptr<std::array<float, liveTempoHistoryFrames * stemCount * 2>> tempoHistory;
    std::uint64_t tempoHistoryFirst = 0, tempoHistoryEnd = 0;
    bool tempoHistoryValid = false; // Same raw-input owner; reset only after join.
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
        if (state_->hold) state_->hold->captureAccepted(packet);
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
    createLiveTempoSource();
}

std::unique_ptr<PreparedPlayback> PreparedPlayback::makeLive(
    std::uint64_t sourceId, std::uint64_t exactFrames, LiveProducer producer,
    std::optional<AudioResidentHoldRange> residentHold)
{
    if (!producer) throw std::invalid_argument("Live playback requires an owned producer callable");
    auto owner = std::unique_ptr<PreparedPlayback>(new PreparedPlayback(
        std::make_unique<LiveState>(sourceId, exactFrames, std::move(producer), residentHold), exactFrames));
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
    createScratchSource(normalizedFds);
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
    createScratchSource(normalizedFds);
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

std::unique_ptr<PreparedTempoSource> PreparedPlayback::makeLiveTempoSource()
{
    if (!live_->tempoHistory)
        live_->tempoHistory = std::make_unique_for_overwrite<std::array<float, liveTempoHistoryFrames * stemCount * 2>>();
    return std::make_unique<PreparedTempoSource>(PreparedTempoSource::StreamInput{
        frames_, stemCount, this,
        [](void* context, std::uint64_t generation, std::uint64_t frame, std::span<float> output) noexcept {
            return static_cast<PreparedPlayback*>(context)->readUnmixedLive(generation, frame, output);
        },
        [](const void* context) noexcept {
            const auto& owner = *static_cast<const PreparedPlayback*>(context);
            if (const int code = owner.live_->code()) return code;
            return owner.bank_->hasReadFailure() ? EIO : 0;
        },
        [](void* context) noexcept {
            auto& owner = *static_cast<PreparedPlayback*>(context);
            owner.live_->setActive(owner.live_->tempoRenderActive.load(std::memory_order_relaxed));
        }});
}

std::unique_ptr<PreparedTempoSource> PreparedPlayback::makeResidentTempoSource()
{
    const auto length = live_->hold->range().outFrame-live_->hold->range().inFrame;
    return std::make_unique<PreparedTempoSource>(PreparedTempoSource::StreamInput{
        length, stemCount, this,
        [](void* context, std::uint64_t, std::uint64_t frame, std::span<float> output) noexcept {
            const auto& owner = *static_cast<const PreparedPlayback*>(context);
            const auto& cache = *owner.live_->hold;
            const auto length = cache.range().outFrame-cache.range().inFrame;
            constexpr auto channels = stemCount*2;
            if (frame > length || output.size()%channels || output.size()/channels > PreparedTempoSource::blockFrames ||
                owner.bank_->hasReadFailure()) return AudioReadResult{0,true};
            if (!cache.ready()) return AudioReadResult{};
            const auto leaves = cache.leaves();
            const auto count = std::size_t(std::min<std::uint64_t>(output.size()/channels,length-frame));
            for (std::size_t f=0;f<count;++f)
                for (std::size_t role=0;role<stemCount;++role)
                    for (std::size_t channel=0;channel<2;++channel)
                        output[f*channels+role*2+channel] = leaves[role][(frame+f)*2+channel];
            return AudioReadResult{count,false};
        },
        [](const void* context) noexcept {
            // A failed producer does not invalidate accepted immutable resident
            // PCM. Malformed bank staging remains a fault for both paths.
            return static_cast<const PreparedPlayback*>(context)->bank_->hasReadFailure() ? EIO : 0;
        }, nullptr, true});
}

void PreparedPlayback::createLiveTempoSource() noexcept
{
    try {
        auto linear = makeLiveTempoSource();
        auto resident = live_->hold ? makeResidentTempoSource() : nullptr;
        tempoSource_ = std::move(linear);
        residentTempoSource_ = std::move(resident);
    }
    catch (const std::system_error& error) { tempoSetupError_ = error.code().value() > 0 ? error.code().value() : EIO; }
    catch (const std::bad_alloc&) { tempoSetupError_ = ENOMEM; }
    catch (const std::invalid_argument&) { tempoSetupError_ = EINVAL; }
    catch (...) { tempoSetupError_ = EIO; }
}

void PreparedPlayback::createScratchSource(std::span<const int> fds) noexcept
{
    // Optional duplicated raw FDs/window storage must not make healthy legacy
    // playback fail. Construction and worker start belong off callback.
    try {
        scratchSource_ = std::make_unique<PreparedScratchSource>(fds, frames_);
    } catch (const std::system_error& error) {
        scratchSetupError_ = error.code().value() > 0 ? error.code().value() : EIO;
    } catch (const std::bad_alloc&) {
        scratchSetupError_ = ENOMEM;
    } catch (const std::invalid_argument&) {
        scratchSetupError_ = EINVAL;
    } catch (...) {
        scratchSetupError_ = EIO;
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
        source.scratchControl = [](void* context, AudioScratchCommand command, std::int64_t cumulativeFrames,
                                   double intervalSeconds, std::uint64_t frame) noexcept {
            return static_cast<PreparedPlayback*>(context)->scratchControl(command, cumulativeFrames, intervalSeconds, frame);
        };
        source.scratchState = [](const void* context) noexcept {
            return static_cast<const PreparedPlayback*>(context)->scratchState();
        };
        return source;
    }
    AudioSource source{frames_, nullptr, this,
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
    {
        source.tempoControl = [](void* context, AudioTempoCommand command, double percent,
                                 bool keyLock, std::uint64_t frame) noexcept {
            return static_cast<PreparedPlayback*>(context)->tempoControl(command, percent, keyLock, frame);
        };
        source.tempoState = [](const void* context) noexcept {
            return static_cast<const PreparedPlayback*>(context)->tempoState();
        };
    }
    if (live_->hold) {
        source.residentHoldControl = [](void* context, AudioResidentHoldCommand command,
                                       std::uint64_t rangeId, std::uint64_t frame) noexcept {
            return static_cast<PreparedPlayback*>(context)->residentHoldControl(command,rangeId,frame);
        };
        source.residentHoldState = [](const void* context) noexcept {
            return static_cast<const PreparedPlayback*>(context)->residentHoldState();
        };
    }
    return source;
}

int PreparedPlayback::preparedRenderError() const noexcept
{
    if (const int code = errorCode()) return code;
    return bank_ && bank_->hasReadFailure() ? EIO : 0;
}

AudioLoopState PreparedPlayback::loopState() const noexcept
{
    auto state = loop_;
    state.supported = !live_ && bool(loopCache_) &&
        !(scratchSource_ && scratchSource_->active());
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
        auto* source = live_ && liveTempoInput_ && live_->holdActive
            ? residentTempoSource_.get() : tempoSource_.get();
        state = source->status();
        if (state.active && !state.finished && !state.errorCode && !source->peekPrepared()) state.pending = true;
        state.supported = !stopped_ && !(scratchSource_ && scratchSource_->active());
        if (!state.active) state.keyLock = tempoKeyLock_;
    }
    return state;
}

bool PreparedPlayback::tempoControl(AudioTempoCommand command, double percent, bool keyLock,
                                    std::uint64_t frame) noexcept
{
    if (stopped_ || !tempoSource_ || (live_ && live_->holdActive) ||
        (scratchSource_ && scratchSource_->active()) || frame > frames_ ||
        !std::isfinite(percent) || percent < -6 || percent > 6)
        return false;
    unusedContinuationPrime_ = false;
    const auto state = tempoSource_->status();
    if (command == AudioTempoCommand::Reset) {
        if (percent != 0) return false;
        if (state.active) {
            if (live_) {
                // Keep the raw input's single worker owner. A fresh exact-unity
                // generation drops the old SRC/R3 tail and retains key lock.
                const bool accepted = tempoSource_->request(frame, 0, tempoKeyLock_);
                if (accepted) (void)residentHoldControl(AudioResidentHoldCommand::Cancel,0,frame);
                return accepted;
            }
            if (!tempoSource_->cancel()) return false;
            // The legacy bank/readers may still be parked at a pre-tempo
            // cursor. This is an asynchronous refill, never a stale resume.
            // Raw unity uses the already validated resident loop. Park the
            // ordinary reader at OUT for the later EXIT remainder handoff.
            loop_.cached = loop_.active;
            seekLinear(loop_.cached ? loop_.outFrame : frame);
        }
        return true; // Keep the selected Master Tempo setting at legacy zero.
    }
    if (command != AudioTempoCommand::Set || preparedRenderError()) return false;
    if (loop_.active && !loopState().ready) return false;
    const auto request = [&] {
        const bool previous = liveTempoInput_;
        if (live_) liveTempoInput_ = true; // Stop render-side raw access before publication.
        const bool accepted = tempoSource_->request(frame, percent, keyLock,
            loop_.active ? loop_.inFrame : 0, loop_.active ? loop_.outFrame : 0);
        if (!accepted) liveTempoInput_ = previous;
        else if (live_) unusedLivePrime_ = false;
        return accepted;
    };
    if (state.active) {
        bool accepted = state.keyLock == keyLock
            ? tempoSource_->requestSpeed(percent)
            : request();
        // A mode/session reset at zero creates a fresh exact-unity backend.
        // Its first nonzero speed needs explicit setup, unlike an active
        // SRC/R3 stream crossing zero, which requestSpeed already accepts.
        if (!accepted && state.keyLock == keyLock && state.percent == 0 && percent != 0 && !state.errorCode)
            accepted = request();
        if (accepted) {
            tempoKeyLock_ = keyLock;
            if (live_) (void)residentHoldControl(AudioResidentHoldCommand::Cancel,0,frame);
        }
        return accepted;
    }
    if (percent == 0) {
        tempoKeyLock_ = keyLock;
        if (live_) (void)residentHoldControl(AudioResidentHoldCommand::Cancel,0,frame);
        return true;
    }
    if (!request()) return false;
    loop_.cached = false;
    tempoKeyLock_ = keyLock;
    if (live_) (void)residentHoldControl(AudioResidentHoldCommand::Cancel,0,frame);
    return true;
}

void PreparedPlayback::seekLinear(std::uint64_t frame) noexcept
{
    unusedContinuationPrime_ = false;
    const auto source = bank_ ? bank_->sourceRef() : readers_[0]->sourceRef();
    source.discontinuity(source.context, frame);
}

AudioScratchState PreparedPlayback::scratchState() const noexcept
{
    AudioScratchState state;
    // Capability only; Begin enforces the loop and source-health gates. Do not
    // call tempo status()/peek() from this pure scratch-state observation.
    state.supported = !live_ && !stopped_ && bool(scratchSource_);
    state.errorCode = scratchSetupError_;
    if (scratchSource_) {
        state.position = scratchSource_->position();
        state.generation = scratchSource_->generation();
        state.active = scratchSource_->active();
        state.pending = scratchSource_->pending();
        if (const int code = scratchSource_->errorCode()) state.errorCode = code;
    }
    return state;
}

bool PreparedPlayback::scratchControl(AudioScratchCommand command, std::int64_t cumulativeFrames,
                                      double intervalSeconds, std::uint64_t frame) noexcept
{
    if (live_ || stopped_ || !scratchSource_ || frame > frames_) return false;
    unusedContinuationPrime_ = false;
    if (command == AudioScratchCommand::Begin) {
        if (loop_.inSet || loop_.outSet || loop_.cached || preparedRenderError()) return false;
        return scratchSource_->begin(frame);
    }
    if (command == AudioScratchCommand::Move) return scratchSource_->move(cumulativeFrames, intervalSeconds);
    if (command != AudioScratchCommand::End || !scratchSource_->active()) return false;
    const double position = scratchSource_->position();
    if (!std::isfinite(position) || position < 0 || position > double(frames_)) return false;
    const auto restart = static_cast<std::uint64_t>(std::floor(position));
    const auto tempo = tempoState();
    // The parked producer retains the selection, but none of its queued PCM
    // belongs to this consumed scratch cursor. Admit a fresh generation before
    // ending healthy raw takeover. An already-failed producer stays failed:
    // retain its generation/error so readTempo keeps the old queue muted, but
    // allow release and Pause to make the deck replaceable.
    if (tempo.active && !preparedRenderError() &&
        !tempoSource_->request(restart, tempo.percent, tempo.keyLock))
        return false;
    scratchSource_->end();
    // Legacy playback still needs its one ordinary-reader refill. Processed
    // playback resumes the new generation; Reset handles its linear handoff.
    if (!tempo.active) seekLinear(restart);
    return true;
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
    unusedContinuationPrime_ = false;
    if (scratchSource_) {
        scratchSource_->end();
        (void)scratchSource_->relocate(frame);
    }
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
    if (scratchSource_ && scratchSource_->active()) return false;
    unusedContinuationPrime_ = false;
    const auto tempo = tempoState();
    const auto request = [&](std::uint64_t cursor, std::uint64_t in = 0, std::uint64_t out = 0) {
        return !tempo.active || tempoSource_->request(cursor, tempo.percent, tempo.keyLock, in, out);
    };
    if (command == AudioLoopCommand::Clear) {
        if (loop_.active && !request(frame)) return false;
        const bool wasCached = loop_.cached;
        clearLoop();
        // Clear can precede a same-frame seek, for which RenderCore need not
        // send another discontinuity. Restore the parked reader here as well.
        if (wasCached) seekLinear(frame);
        return true;
    }
    if (!loopCache_) return false;
    if (command == AudioLoopCommand::In) {
        if (frame == frames_ || preparedRenderError()) return false;
        if (loop_.active && !request(frame)) return false;
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
        if (!request(loop_.inFrame, loop_.inFrame, frame)) return false;
        loop_.outFrame = frame;
        loop_.outSet = loop_.active = true;
        loop_.cached = !tempo.active;
        return true;
    }
    if (command == AudioLoopCommand::Toggle && loop_.outSet) {
        if (loop_.active) {
            if (!request(frame)) return false;
            loop_.active = false;
            return true;
        }
        if (!loopState().ready || !request(loop_.inFrame, loop_.inFrame, loop_.outFrame)) return false;
        // The raw unity path resumes its cached range. A processed loop uses
        // one new cyclic stream, then retains its DSP state at every wrap.
        if (!tempo.active && !loop_.cached) seekLinear(loop_.outFrame);
        loop_.active = true;
        loop_.cached = !tempo.active;
        return true;
    }
    return false;
}

AudioReadResult PreparedPlayback::readPrepared(std::uint64_t frame, std::span<float> output) noexcept
{
    if (output.size() % StemBankSource::channels || frame > frames_) return {0, true};
    if (!output.empty()) unusedContinuationPrime_ = false;
    if (scratchSource_ && scratchSource_->active()) return readScratch(output);
    // Optional inactive prefetch follows the nominal source cursor during
    // ordinary or processed playback. Failure remains scratch-only.
    if (scratchSource_) (void)scratchSource_->follow(frame);
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

AudioReadResult PreparedPlayback::readScratch(std::span<float> output) noexcept
{
    AudioReadResult result;
    result.positionValid = true;
    result.nominalPosition = scratchSource_->position();
    if (output.size() / StemBankSource::channels > PreparedScratchSource::maxReadFrames) {
        result.failed = true;
        return result;
    }
    if (output.empty()) return result;
    if (preparedRenderError()) { result.failed = true; return result; }
    const auto requested = output.size() / StemBankSource::channels;
    const auto raw = scratchSource_->read(requested);
    result.nominalPosition = raw.nominalPosition;
    if (raw.frames > requested || !std::isfinite(raw.nominalPosition) || raw.nominalPosition < 0 ||
        raw.nominalPosition > double(frames_) || scratchSource_->leafCount() != (bank_ ? stemCount : 1)) {
        result.failed = true;
        return result;
    }
    for (std::size_t stem = 0; stem < scratchSource_->leafCount(); ++stem) {
        if (raw.leaves[stem].size() != raw.frames * StemBankSource::channels) {
            result.failed = true;
            return result;
        }
    }
    if (raw.frames) {
        auto clipped = output.first(raw.frames * StemBankSource::channels);
        if (bank_) {
            const auto mixed = bank_->readCached(raw.leaves, 0, clipped);
            result.frames = mixed.frames;
            result.failed = mixed.failed || mixed.frames != raw.frames;
        } else {
            std::copy_n(raw.leaves[0].data(), clipped.size(), clipped.data());
            result.frames = raw.frames;
        }
    }
    result.failed = result.failed || raw.failed;
    // Never EOF while a platter is held, including a reverse-ready hold at F.
    return result;
}

AudioReadResult PreparedPlayback::readTempo(std::span<float> output) noexcept
{
    AudioReadResult result;
    result.positionValid = true;
    result.nominalPosition = tempoSource_->nominalPosition();
    result.finished = tempoSource_->status().finished;
    const auto beforeWraps = tempoSource_->loopWraps();
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
        const auto view = tempoSource_->peekPrepared();
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
    const auto wraps = tempoSource_->loopWraps() - beforeWraps;
    if (wraps > UINT32_MAX) result.failed = true;
    else result.loopWraps = static_cast<std::uint32_t>(wraps);
    return result;
}

std::uint64_t PreparedPlayback::residentHoldBytes() const noexcept
{ return live_ && live_->hold ? live_->hold->bytes() : 0; }

AudioResidentHoldState PreparedPlayback::residentHoldState() const noexcept
{
    if (!live_ || !live_->hold) return {};
    const auto& range = live_->hold->range();
    bool ready = live_->hold->ready();
    if (liveTempoInput_) {
        if (live_->holdActive) ready = ready && !residentTempoSource_->errorCode();
        else if (live_->holdArmed) {
            const auto view = residentTempoSource_->peekPrepared();
            ready = ready && (live_->holdTempoAligned || live_->code()) && view.frames &&
                !view.errorCode && !residentTempoSource_->errorCode();
        }
    }
    return {range.id,range.inFrame,range.outFrame,
        liveTempoInput_ ? std::uint64_t(tempoSource_->nominalPosition()) : live_->demand.cursor,
        range.inFrame+live_->holdOffset,live_->hold->bytes(),range.provenance,
        live_->code() ? live_->code() : bank_->hasReadFailure() ? EIO : 0,
        true,ready,live_->holdArmed,live_->holdActive,live_->holdExit};
}
bool PreparedPlayback::residentHoldControl(AudioResidentHoldCommand command, std::uint64_t rangeId,
                                          std::uint64_t frame) noexcept
{
    if (!live_ || !live_->hold || stopped_ || live_->stopping.load(std::memory_order_relaxed)) return false;
    if (command == AudioResidentHoldCommand::Cancel) {
        // Zero is reserved for engine transport invalidation. The public
        // RenderCore command requires this owner's nonzero configured rangeId.
        if (rangeId && rangeId != live_->hold->range().id) return false;
        live_->holdArmed = live_->holdActive = live_->holdExit = live_->holdTransition = live_->holdTempoAligned = false;
        if (residentTempoSource_ && residentTempoSource_->status().active)
            (void)residentTempoSource_->cancel();
        return true; // No seek, source reset, health clear or automatic restart.
    }
    const auto linear = liveTempoInput_ ? std::uint64_t(tempoSource_->nominalPosition()) : live_->demand.cursor;
    if (rangeId != live_->hold->range().id || frame != linear || frame >= frames_) return false;
    if (command == AudioResidentHoldCommand::Arm) {
        if (live_->holdActive) return false;
        if (liveTempoInput_) {
            const auto tempo = tempoSource_->status();
            if (!residentTempoSource_->request(0,tempo.percent,tempo.keyLock,0,
                    live_->hold->range().outFrame-live_->hold->range().inFrame)) return false;
            live_->holdTempoAligned = tempoSource_->outputConfigurationCurrent();
        }
        live_->holdArmed = true; live_->holdExit = false; live_->holdOffset = 0; return true;
    }
    if (command == AudioResidentHoldCommand::Exit) {
        if (!live_->holdActive) return false;
        live_->holdExit = true; return true;
    }
    if (command != AudioResidentHoldCommand::Enter || !live_->holdArmed || live_->holdActive ||
        !residentHoldState().ready || bank_->hasReadFailure()) return false;
    if (liveTempoInput_) {
        // The live output consumer remains at F. Its raw-input worker may fill
        // the bounded linear queue independently; no raw cursor crosses owners.
        live_->holdActive = live_->holdTransition = true; live_->holdOffset = 0; return true;
    }
    // Park the existing linear providers/epoch at F. A healthy producer keeps
    // filling the SAME reserve request; permanent failure remains terminal.
    if (!live_->code() && !live_->demand.waitSerial) {
        if (!live_->closeGate()) return false;
        live_->publishDemand();
    }
    live_->holdActive = live_->holdTransition = true; live_->holdOffset = 0; return true;
}
AudioReadResult PreparedPlayback::readResidentHold(std::span<float> output) noexcept
{
    // Independent producer failure does not corrupt resident audio. A malformed
    // staged/provider read is different: retain that fault and fail closed.
    if (!live_->hold->ready() || bank_->hasReadFailure()) return {0,true};
    AudioReadResult result; result.positionValid = result.residentHold = true;
    result.nominalPosition = double(live_->demand.cursor);
    const auto length = live_->hold->range().outFrame-live_->hold->range().inFrame;
    const auto leaves = live_->hold->leaves();
    if (live_->holdTransition) { result.transitionFrame = 0; live_->holdTransition = false; }
    const auto wanted = output.size()/2;
    while (result.frames < wanted) {
        if (live_->holdOffset == length) {
            const bool reserve = !live_->code() && !bank_->hasReadFailure() && live_->demand.waitSerial &&
                live_->grant.load(std::memory_order_acquire) == live_->demand.waitSerial;
            if (live_->holdExit && reserve && live_->openGate()) {
                const auto offset = result.frames;
                const auto linear = readLinearLive(live_->demand.cursor,output.subspan(offset*2));
                // Commit exit only after linear audio actually commits. A
                // producer failure may race the grant/open test; zero returned
                // frames cannot discard valid resident audio. Staging failure
                // remains fail-closed and never gets this independent bypass.
                if (linear.frames || bank_->hasReadFailure()) {
                    live_->holdActive = live_->holdArmed = live_->holdExit = false;
                    result.frames += linear.frames; result.failed = linear.failed;
                    result.nominalPosition = double(live_->demand.cursor);
                    result.finished = live_->demand.cursor == frames_;
                    if (linear.frames) result.transitionFrame = std::uint32_t(offset);
                    return result;
                }
            }
            live_->holdOffset = 0;
            result.transitionFrame = std::uint32_t(result.frames);
        }
        const auto count = std::min<std::uint64_t>(wanted-result.frames,length-live_->holdOffset);
        auto part = bank_->readCached(leaves,live_->holdOffset,
            output.subspan(result.frames*2,count*2));
        live_->holdOffset += part.frames; result.frames += part.frames;
        if (part.failed || part.frames != count) { result.failed = true; break; }
    }
    // Range>=2048 and read<=1024 guarantee at most one wrap per source read.
    return result;
}
AudioReadResult PreparedPlayback::readResidentTempo(std::span<float> output) noexcept
{
    AudioReadResult result;
    result.positionValid = result.residentHold = true;
    result.nominalPosition = tempoSource_->nominalPosition();
    if (!live_->hold->ready() || bank_->hasReadFailure()) { result.failed = true; return result; }
    const auto length = live_->hold->range().outFrame-live_->hold->range().inFrame;
    const auto wanted = output.size()/2;
    // Loop OUT can split a queued slot without retiring it. One source read
    // crosses at most one OUT, so eight queued slots can require nine pieces.
    for (std::size_t slot=0;slot<PreparedTempoSource::slotCount+1U && result.frames<wanted;++slot) {
        if (live_->holdOffset == length) {
            if (live_->holdExit && !live_->code() && !tempoSource_->errorCode()) {
                const auto offset = result.frames;
                const auto linear = readTempo(output.subspan(offset*2));
                if (linear.frames || bank_->hasReadFailure()) {
                    live_->holdActive = live_->holdArmed = live_->holdExit = live_->holdTransition = live_->holdTempoAligned = false;
                    result.frames += linear.frames; result.failed = linear.failed;
                    result.nominalPosition = linear.nominalPosition; result.finished = linear.finished;
                    if (linear.frames) result.transitionFrame = std::uint32_t(offset);
                    (void)residentTempoSource_->cancel();
                    return result;
                }
            }
            live_->holdOffset = std::uint64_t(residentTempoSource_->nominalPosition());
            live_->holdTransition = true;
        }
        const auto view = residentTempoSource_->peekPrepared();
        if (!view) break;
        if (view.generation != residentTempoSource_->generation() || view.errorCode || view.terminal ||
            view.leafCount != stemCount || !std::isfinite(view.nominalStart) ||
            !std::isfinite(view.nominalStep) || view.nominalStep < .94 || view.nominalStep > 1.06 ||
            view.nominalStart < 0 || view.nominalStart >= double(length)) {
            result.failed = true; break;
        }
        const auto toOut = std::size_t(std::max(1.,std::ceil((double(length)-view.nominalStart)/view.nominalStep)));
        const auto count = std::min({wanted-result.frames,view.frames,toOut});
        const auto beforeWraps = residentTempoSource_->loopWraps();
        const auto part = bank_->readCached(view.leaves,0,output.subspan(result.frames*2,count*2));
        if (part.failed || part.frames != count || !residentTempoSource_->consume(part.frames)) {
            result.failed = true; break;
        }
        if (live_->holdTransition) {
            result.transitionFrame = std::uint32_t(result.frames);
            live_->holdTransition = false;
        }
        result.frames += part.frames;
        const auto wraps = residentTempoSource_->loopWraps()-beforeWraps;
        if (wraps>1) { result.failed = true; break; }
        live_->holdOffset = wraps ? length : std::uint64_t(residentTempoSource_->nominalPosition());
    }
    return result;
}
AudioReadResult PreparedPlayback::readLive(std::uint64_t frame, std::span<float> output) noexcept
{
    if (output.size()%2 || output.size()/2 > StemBankSource::blockFrames || frame > frames_ ||
        stopped_ || live_->stopping.load(std::memory_order_relaxed)) return {0,true};
    if (liveTempoInput_) {
        if (frame != static_cast<std::uint64_t>(tempoSource_->nominalPosition())) return {0,true};
        if (live_->holdActive) return readResidentTempo(output);
        auto result = readTempo(output);
        if (live_->holdArmed && tempoSource_->outputConfigurationCurrent()) live_->holdTempoAligned = true;
        if (result.frames == output.size()/2 || !live_->holdArmed || bank_->hasReadFailure() ||
            !residentHoldControl(AudioResidentHoldCommand::Enter,live_->hold->range().id,
                std::uint64_t(tempoSource_->nominalPosition()))) return result;
        const auto offset = result.frames;
        const auto held = readResidentTempo(output.subspan(offset*2));
        result.frames += held.frames; result.failed = held.failed;
        result.positionValid = result.residentHold = true;
        result.nominalPosition = held.nominalPosition;
        result.finished = held.finished;
        if (held.frames) result.transitionFrame = std::uint32_t(offset);
        return result;
    }
    if (frame != live_->demand.cursor) return {0,true};
    if (output.empty() || frame == frames_) return {};
    if (live_->holdActive) return readResidentHold(output);
    auto result = readLinearLive(frame,output);
    const auto wanted = std::min<std::uint64_t>(output.size()/2,frames_-frame);
    if (result.frames == wanted || !live_->holdArmed || !live_->hold || !live_->hold->ready() ||
        !residentHoldControl(AudioResidentHoldCommand::Enter,live_->hold->range().id,live_->demand.cursor))
        return result;
    const auto offset = result.frames;
    const auto held = readResidentHold(output.subspan(offset*2));
    result.frames += held.frames; result.failed = held.failed;
    result.positionValid = result.residentHold = true;
    result.nominalPosition = double(live_->demand.cursor);
    result.transitionFrame = std::uint32_t(offset);
    return result;
}
AudioReadResult PreparedPlayback::readLinearLive(std::uint64_t frame, std::span<float> output) noexcept
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

AudioReadResult PreparedPlayback::readUnmixedLive(std::uint64_t generation, std::uint64_t frame,
                                                  std::span<float> output) noexcept
{
    constexpr auto packedChannels = stemCount * 2;
    if (!generation || frame > frames_ || output.size()%packedChannels ||
        output.size()/packedChannels > PreparedTempoSource::blockFrames ||
        live_->code() || live_->stopping.load(std::memory_order_relaxed)) return {0,true};
    if (generation != live_->tempoInputGeneration) {
        const bool retained = live_->tempoHistoryValid && frame >= live_->tempoHistoryFirst &&
            frame <= live_->tempoHistoryEnd;
        if (!retained) {
            // First activation retains the exact staged common prefix. Later
            // requests outside the recent validated range need a fresh epoch.
            if (live_->tempoInputGeneration || frame != live_->demand.cursor) seekLiveInput(frame);
            live_->tempoHistoryFirst = live_->tempoHistoryEnd = frame;
            live_->tempoHistoryValid = true;
        }
        live_->tempoInputGeneration = generation;
    }
    if (live_->code() || frame < live_->tempoHistoryFirst) return {0,true};
    if (frame < live_->tempoHistoryEnd) {
        const auto count = std::size_t(std::min<std::uint64_t>(output.size()/packedChannels,
            live_->tempoHistoryEnd-frame));
        const auto offset = std::size_t(frame%liveTempoHistoryFrames);
        const auto first = std::min<std::size_t>(count, liveTempoHistoryFrames-offset);
        std::copy_n(live_->tempoHistory->data()+offset*packedChannels, first*packedChannels, output.data());
        std::copy_n(live_->tempoHistory->data(), (count-first)*packedChannels, output.data()+first*packedChannels);
        return {count,false};
    }
    if (frame != live_->demand.cursor || frame != live_->tempoHistoryEnd) return {0,true};
    if (live_->demand.waitSerial) {
        if (live_->grant.load(std::memory_order_acquire) != live_->demand.waitSerial) return {};
        if (!live_->openGate()) return {0,true};
    }
    const auto result = bank_->readUnmixed(frame, output);
    if (result.failed) return result;
    const auto offset = std::size_t(frame%liveTempoHistoryFrames);
    const auto first = std::min<std::size_t>(result.frames, liveTempoHistoryFrames-offset);
    std::copy_n(output.data(), first*packedChannels, live_->tempoHistory->data()+offset*packedChannels);
    std::copy_n(output.data()+first*packedChannels, (result.frames-first)*packedChannels, live_->tempoHistory->data());
    live_->demand.cursor += result.frames;
    live_->tempoHistoryEnd += result.frames;
    if (live_->tempoHistoryEnd > liveTempoHistoryFrames)
        live_->tempoHistoryFirst = std::max(live_->tempoHistoryFirst, live_->tempoHistoryEnd-liveTempoHistoryFrames);
    const auto wanted = std::min<std::uint64_t>(output.size()/packedChannels, frames_-frame);
    if (result.frames < wanted) {
        if (!live_->closeGate()) return {result.frames,true};
        live_->publishDemand();
    }
    return result;
}

void PreparedPlayback::seekLiveInput(std::uint64_t frame) noexcept
{
    if (frame > frames_ || live_->stopping.load(std::memory_order_relaxed) || !live_->closeGate()) return;
    if (!bank_->seekInput(frame)) { live_->failRender(EINVAL); return; }
    LiveStemBuffer::Request request;
    if (!live_->buffer.tryRequest(request) || request.firstFrame != frame) {
        live_->failRender(EIO); return;
    }
    live_->demand.epoch = request.epoch;
    live_->demand.cursor = frame;
    live_->publishDemand();
}

void PreparedPlayback::seekLive(std::uint64_t frame) noexcept
{
    (void)residentHoldControl(AudioResidentHoldCommand::Cancel,0,frame);
    unusedLivePrime_ = false;
    if (liveTempoInput_) {
        const auto state = tempoSource_->status();
        if (!tempoSource_->request(frame, state.percent, state.keyLock))
            primeError_.store(tempoSource_->errorCode() ? tempoSource_->errorCode() : EIO, std::memory_order_relaxed);
        return;
    }
    seekLiveInput(frame);
}

void PreparedPlayback::setLiveActive(bool active) noexcept
{
    live_->tempoRenderActive.store(active, std::memory_order_relaxed);
    if (!liveTempoInput_) live_->setActive(active);
}

void PreparedPlayback::resetBankForSession() { resetBankForSessionAt(0); }

void PreparedPlayback::resetBankForSessionAt(std::uint64_t frame)
{
    unusedContinuationPrime_ = false;
    if (stopped_) throw std::logic_error("Cannot reuse a stopped playback owner");
    if (scratchSource_) scratchSource_->end();
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
    if (live_ && liveTempoInput_) {
        // Allocate the replacement while the old worker remains intact. A
        // failed allocation leaves its bank/input/processor ownership intact.
        auto replacementTempo = makeLiveTempoSource();
        auto replacementResident = live_->hold ? makeResidentTempoSource() : nullptr;
        liveTempoRestore_ = tempoState();
        tempoSource_->stop(); // Off callback, before raw control ownership returns.
        if (residentTempoSource_) residentTempoSource_->stop();
        tempoSource_ = std::move(replacementTempo);
        residentTempoSource_ = std::move(replacementResident);
        live_->tempoInputGeneration = 0;
        live_->tempoHistoryValid = false;
        liveTempoInput_ = false;
    }
    if (live_) setLiveActive(false);
    bank_ = std::move(replacement);
}

bool PreparedPlayback::prime(std::uint64_t frame, std::chrono::milliseconds timeout, std::stop_token stop)
{
    unusedContinuationPrime_ = false;
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
    // Do not wait for or require the optional scratch cache for normal load.
    // A later valid Begin may be pending while its bounded worker catches up.
    if (scratchSource_) {
        scratchSource_->end();
        (void)scratchSource_->relocate(frame);
    }
    if (live_) {
        if (liveTempoInput_) resetBankForSessionAt(frame);
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
            if (liveTempoRestore_.active) {
                const auto selected = liveTempoRestore_;
                liveTempoRestore_ = {};
                liveTempoInput_ = true;
                unusedLivePrime_ = false;
                lock.unlock();
                return primeTempo(frame, selected.percent, selected.keyLock, deadline, stop);
            }
            return true;
        }
        primeError_.store(ETIMEDOUT, std::memory_order_relaxed);
        return false;
    }
    if (tempoSource_) {
        const auto state = tempoSource_->status();
        if (state.active) return primeTempo(frame, state.percent, state.keyLock, deadline, stop);
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
    continuationPrimePosition_ = double(frame);
    unusedContinuationPrime_ = bool(bank_);
    return true;
}

bool PreparedPlayback::primeContinuation(double position, const AudioTempoState& tempo,
                                          std::chrono::milliseconds timeout, std::stop_token stop)
{
    unusedContinuationPrime_ = false;
    const auto now = Clock::now();
    if (live_ || !bank_ || !std::isfinite(position) || position < 0 || position >= double(frames_) ||
        !std::isfinite(tempo.percent) || tempo.percent < -6 || tempo.percent > 6 || tempo.finished ||
        (tempo.active && !tempo.supported) ||
        (!tempo.active && (tempo.percent != 0 || position != std::floor(position))) ||
        loop_.inSet || loop_.outSet || loop_.active || loop_.cached ||
        (scratchSource_ && scratchSource_->active()) || timeout.count() < 0 ||
        timeout > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max()-now)) {
        primeError_.store(EINVAL,std::memory_order_relaxed);
        return false;
    }
    if (stopped_ || stop.stop_requested()) {
        primeError_.store(ECANCELED,std::memory_order_relaxed);
        return false;
    }
    primeError_.store(0,std::memory_order_relaxed);
    if (!tempo.active) {
        if (tempoSource_ && tempoSource_->status().active && !tempoSource_->cancel()) {
            primeError_.store(EIO,std::memory_order_relaxed);
            return false;
        }
        tempoKeyLock_ = tempo.keyLock;
        const auto remaining = std::max(std::chrono::milliseconds(0),
            std::chrono::duration_cast<std::chrono::milliseconds>(now+timeout-Clock::now()));
        return prime(std::uint64_t(position),remaining,stop);
    }
    if (!tempoSource_) {
        primeError_.store(tempoSetupError_ ? tempoSetupError_ : EIO,std::memory_order_relaxed);
        return false;
    }
    if (!primeTempo(position,tempo.percent,tempo.keyLock,now+timeout,stop)) return false;
    tempoKeyLock_ = tempo.keyLock;
    continuationPrimePosition_ = position;
    unusedContinuationPrime_ = true;
    return true;
}

bool PreparedPlayback::primeTempo(double position, double percent, bool keyLock,
                                  Clock::time_point deadline, std::stop_token stop)
{
    const bool requested = live_ ? tempoSource_->request(std::uint64_t(position),percent,keyLock)
                                 : tempoSource_->requestAt(position,percent,keyLock);
    if (!requested) {
        const int code = tempoSource_->errorCode();
        primeError_.store(code ? code : EIO, std::memory_order_relaxed);
        return false;
    }
    for (;;) {
        if (stop.stop_requested()) { primeError_.store(ECANCELED, std::memory_order_relaxed); return false; }
        if (const int code = tempoSource_->errorCode()) { primeError_.store(code, std::memory_order_relaxed); return false; }
        const auto view = tempoSource_->peekPrepared();
        if (view.errorCode) { primeError_.store(view.errorCode, std::memory_order_relaxed); return false; }
        if (view && view.generation == tempoSource_->generation()) return true;
        const auto current = Clock::now();
        if (current >= deadline) { primeError_.store(ETIMEDOUT, std::memory_order_relaxed); return false; }
        std::this_thread::sleep_until(std::min(deadline, current + std::chrono::milliseconds(1)));
    }
}

bool PreparedPlayback::primeForSession(std::uint64_t frame, std::chrono::milliseconds timeout, std::stop_token stop)
{
    unusedContinuationPrime_ = false;
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

bool PreparedPlayback::copyStemMixStateFrom(const PreparedPlayback& source) noexcept
{
    if (this == &source || live_ || stopped_ || source.stopped_ || !bank_ || !source.bank_ ||
        frames_ != source.frames_ || errorCode()) return false;
    return bank_->copyMixStateFrom(*source.bank_);
}

bool PreparedPlayback::primedForContinuation(double position) const noexcept
{
    return !live_ && !stopped_ && bank_ && unusedContinuationPrime_ && std::isfinite(position) && position >= 0 && position < double(frames_) &&
        continuationPrimePosition_ == position && !errorCode();
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
            result += "Tempo processing: " + std::generic_category().message(code);
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
    unusedContinuationPrime_ = false;
    unusedLivePrime_ = false;
    if (stopped_) return;
    (void)residentHoldControl(AudioResidentHoldCommand::Cancel,0,0);
    stopped_ = true;
    if (tempoSource_) tempoSource_->stop();
    if (residentTempoSource_) residentTempoSource_->stop();
    liveTempoInput_ = false;
    liveTempoRestore_ = {};
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
    if (scratchSource_) {
        scratchSource_->end();
        scratchSource_->stop();
    }
    if (loopCache_) {
        loop_ = {};
        loopCache_->stop();
    }
    for (auto& reader : readers_)
        if (reader) reader->stop();
}
} // namespace conflux::audio

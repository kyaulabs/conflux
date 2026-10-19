// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/LiveSession.hpp"
#include "audio/LiveStemBuffer.hpp"
#include "audio/PreparedPlayback.hpp"
#include "audio/RenderCore.hpp"
#include "audio/SourceHandoff.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using conflux::audio::LiveSession;
using conflux::audio::LiveStemBuffer;
using conflux::audio::PreparedPlayback;
using conflux::audio::RenderCore;
using conflux::audio::SourceHandoff;
constexpr std::uint64_t totalFrames = 400000;

void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

template<class Predicate>
void await(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    require(predicate(), message);
}

float sample(std::size_t role, std::uint64_t frame, std::size_t channel)
{
    const auto numerator = float((role + 1) * 16 + frame % 31);
    return (channel ? -numerator : numerator) / 4096.F;
}
float sum(std::uint64_t frame, std::size_t channel)
{
    double result = 0;
    for (std::size_t role = 0; role < 4; ++role) result += sample(role, frame, channel);
    return float(result);
}

struct Control {
    std::atomic<std::uint64_t> allowance{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> epoch{0}, target{0}, publishedEnd{0};
    std::atomic<unsigned> starts{0}, exits{0}, targetRequests{0};
    std::atomic<int> forceFailure{0}, afterFailurePublish{-1}, afterStopPublish{-1};
    std::atomic<bool> fullObserved{false};
    std::uint64_t countedTarget = std::numeric_limits<std::uint64_t>::max();
    std::mutex mutex;
    std::thread::id producerThread{};
};

PreparedPlayback::LiveProducer producer(const std::shared_ptr<Control>& control,
                                      std::uint64_t frames = totalFrames,
                                      std::size_t packetFrames = LiveStemBuffer::blockFrames)
{
    return [control, frames, packetFrames](std::stop_token stop, PreparedPlayback::LiveProducerPort& port) {
        {
            const std::lock_guard lock(control->mutex);
            control->producerThread = std::this_thread::get_id();
        }
        ++control->starts;
        std::array<std::array<float, LiveStemBuffer::blockFrames * 2>, 4> pcm{};
        std::uint64_t epoch = 0, cursor = 0, target = 0, sourceId = 0;
        while (!stop.stop_requested()) {
            LiveStemBuffer::Request request;
            if (!port.tryRequest(request)) { std::this_thread::yield(); continue; }
            sourceId = request.sourceId;
            if (request.epoch != epoch) {
                epoch = request.epoch;
                cursor = target = request.firstFrame;
                control->fullObserved.store(false);
                control->target.store(target);
                control->publishedEnd.store(target);
                control->epoch.store(epoch);
                if (target == control->countedTarget) ++control->targetRequests;
            }
            if (const auto failure = control->forceFailure.load()) {
                port.fail(failure);
                if (cursor < frames) {
                    LiveStemBuffer::Packet late{request.sourceId, epoch, cursor, 1, cursor + 1 == frames, {}};
                    for (std::size_t role = 0; role < 4; ++role) late.stems[role] = std::span(pcm[role]).first(2);
                    control->afterFailurePublish.store(int(port.tryPublish(late)));
                }
                break;
            }
            const auto available = control->allowance.load();
            if (cursor == frames || cursor - target >= available) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            const auto count = std::size_t(std::min<std::uint64_t>(
                {frames - cursor, packetFrames, available - (cursor - target)}));
            LiveStemBuffer::Packet packet{request.sourceId, epoch, cursor, count, cursor + count == frames, {}};
            for (std::size_t role = 0; role < 4; ++role) {
                for (std::size_t frame = 0; frame < count; ++frame)
                    for (std::size_t channel = 0; channel < 2; ++channel)
                        pcm[role][frame * 2 + channel] = sample(role, cursor + frame, channel);
                packet.stems[role] = std::span(pcm[role]).first(count * 2);
            }
            const auto result = port.tryPublish(packet);
            if (result == LiveStemBuffer::PublishResult::Accepted) {
                cursor += count;
                control->publishedEnd.store(cursor);
            } else if (result == LiveStemBuffer::PublishResult::Invalid) {
                LiveStemBuffer::Request current;
                if (port.tryRequest(current)) port.fail(EPROTO);
                break;
            } else {
                if (result == LiveStemBuffer::PublishResult::Full) control->fullObserved.store(true);
                std::this_thread::sleep_for(1ms);
            }
        }
        if (stop.stop_requested() && epoch && cursor < frames) {
            LiveStemBuffer::Packet late{sourceId, epoch, cursor, 1, cursor + 1 == frames, {}};
            for (std::size_t role = 0; role < 4; ++role) late.stems[role] = std::span(pcm[role]).first(2);
            control->afterStopPublish.store(int(port.tryPublish(late)));
        }
        ++control->exits;
    };
}

void reserveCancellationAndExactTail()
{
    auto state = std::make_shared<Control>();
    state->allowance = PreparedPlayback::liveReserveFrames - 1;
    auto owner = PreparedPlayback::makeLive(101, totalFrames, producer(state));
    require(owner && owner->isLive(), "Live factory did not return its whole owner");
    const auto controlThread = std::this_thread::get_id();
    std::atomic<bool> primeReturned{false};
    auto pending = std::async(std::launch::async, [&] {
        const bool result = owner->prime(0, 2s);
        primeReturned.store(true);
        return result;
    });
    await([&] { return state->epoch.load() >= 2 &&
                       state->publishedEnd.load() == PreparedPlayback::liveReserveFrames - 1; },
          "Producer did not reach the sub-reserve prefix");
    require(!primeReturned.load(), "Live prime treated slots or a partial reserve as full readiness");
    state->allowance = PreparedPlayback::liveReserveFrames;
    require(pending.get(), "The exact 262144-frame accepted prefix did not satisfy live prime");
    require(owner->frames() == totalFrames && !owner->errorCode(), "Priming changed geometry or health");
    {
        const std::lock_guard lock(state->mutex);
        require(state->producerThread != controlThread, "Producer ran synchronously on its creator thread");
    }
    owner->stop(); owner->stop();
    require(state->starts == 1 && state->exits == 1 && !owner->errorCode() &&
            state->afterStopPublish == int(LiveStemBuffer::PublishResult::Invalid),
            "Idempotent stop missed producer teardown or accepted a stopped producer's late publication");

    auto stopped = std::make_shared<Control>();
    stopped->allowance = 0;
    auto cancelled = PreparedPlayback::makeLive(102, totalFrames, producer(stopped));
    std::stop_source cancellation;
    auto waiting = std::async(std::launch::async, [&] { return cancelled->prime(0, 2s, cancellation.get_token()); });
    await([&] { return stopped->epoch.load() >= 2; }, "Cancelled prime did not publish its request");
    cancellation.request_stop();
    require(!waiting.get() && cancelled->errorCode() == ECANCELED, "Cancellation did not end live reserve wait with ECANCELED");
    stopped->allowance = std::numeric_limits<std::uint64_t>::max();
    require(cancelled->prime(totalFrames - 37, 2s), "Short tail incorrectly required a full startup reserve");
    std::array<float, 74> tail{};
    const auto source = cancelled->sourceRef();
    const auto read = source.read(source.context, totalFrames - 37, tail);
    require(read.frames == 37 && !read.failed, "Exact clipped tail was not readable");
    for (std::size_t frame = 0; frame < 37; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(tail[frame * 2 + channel] == sum(totalFrames - 37 + frame, channel), "Tail role mix or source origin changed");
    require(cancelled->prime(totalFrames, 0ms), "Known exact EOF did not have a zero-frame reserve");
    cancelled->stop();

    auto expiredState = std::make_shared<Control>();
    expiredState->allowance = 0;
    auto expired = PreparedPlayback::makeLive(103, totalFrames, producer(expiredState));
    require(!expired->prime(0, 0ms) && expired->errorCode() == ETIMEDOUT,
            "A zero-timeout unavailable live prefix did not time out");
    expiredState->forceFailure = EBADMSG;
    await([&] { return expired->errorCode() == EBADMSG && expiredState->afterFailurePublish.load() >= 0; },
          "A prior prime timeout concealed terminal producer health");
    require(expiredState->afterFailurePublish == int(LiveStemBuffer::PublishResult::Invalid),
            "Producer failure still allowed a valid current-epoch packet to be published");
    expired->stop();
}

void producerFailureIsAtomicAndTerminal()
{
    auto failed = PreparedPlayback::makeLive(104, totalFrames,
        [](std::stop_token, PreparedPlayback::LiveProducerPort& port) { port.fail(EBADMSG); });
    require(!failed->prime(0, 2s) && failed->errorCode() == EBADMSG,
            "Producer failure was replaced by an unrelated prime timeout/normal exit");
    require(!failed->prime(totalFrames, 0ms) && failed->errorCode() == EBADMSG,
            "Known EOF concealed a terminal producer failure");
    failed->stop();
    auto thrown = PreparedPlayback::makeLive(105, totalFrames,
        [](std::stop_token, PreparedPlayback::LiveProducerPort&) { throw std::runtime_error("fake producer failed"); });
    require(!thrown->prime(0, 2s) && thrown->errorCode() == EIO, "Escaped producer exception did not publish atomic EIO");
    thrown->stop();
    auto returned = PreparedPlayback::makeLive(106, totalFrames,
        [](std::stop_token, PreparedPlayback::LiveProducerPort&) {});
    require(!returned->prime(0, 2s) && returned->errorCode() == EPIPE, "Unrequested producer exit looked like live readiness/EOF");
    returned->stop();
}

void nonzeroInitializationRetainsThePrimedEpoch()
{
    constexpr std::uint64_t target = 7319;
    auto state = std::make_shared<Control>();
    state->countedTarget = target;
    LiveSession::PlaybackSources owners{};
    owners[0] = PreparedPlayback::makeLive(201, totalFrames, producer(state));
    LiveSession::InitialState initial;
    initial.sourceFrame[0] = target;
    initial.cueFrame[0] = target - 5;
    initial.stemGain[0] = {.5F, .25F, 1.F, 0.F};
    LiveSession session(std::move(owners), initial);
    require(session.stoppedState().sourceFrame[0] == target && session.stoppedState().stemBank[0],
            "Session lost a primed nonzero live cursor or bank capability");
    const auto primedEpoch = state->epoch.load();
    auto taken = session.stopAndTakePlaybacks();
    require(taken.active[0] && !taken.active[1] && !taken.replacement && state->exits == 0,
            "Ownership-return operation lost or stopped an inactive live owner");
    const auto source = taken.active[0]->sourceRef();
    std::array<float, 128> actual{};
    const auto read = source.read(source.context, target, actual);
    require(read.frames == 64 && !read.failed && state->targetRequests == 1 && state->epoch == primedEpoch,
            "Initializing/restoring a nonzero cursor invalidated its already primed epoch");
    for (std::size_t frame = 0; frame < 64; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const double expected = double(sample(0, target + frame, channel)) * .5 +
                                    double(sample(1, target + frame, channel)) * .25 +
                                    double(sample(2, target + frame, channel));
            require(actual[frame * 2 + channel] == float(expected), "Restored live stem targets or PCM origin changed");
        }
    auto again = session.stopAndTakePlaybacks();
    require(!again.active[0] && !again.active[1] && !again.replacement,
            "Repeated ownership return duplicated a whole live owner");
    require(!session.requestRecorder(nullptr) && !session.recorderChangePending(),
            "Terminal detached session retained a new recorder-tap request");
    session.stop();
    require(state->exits == 0, "Detached session stop touched its former producer");
    taken.active[0]->stop();
    require(state->exits == 1, "Returned whole owner did not retain producer lifetime until retirement");
}

void sameCursorRetryUsesTheRealRenderCommand()
{
    constexpr std::uint64_t target = 17;
    auto state = std::make_shared<Control>();
    auto owner = PreparedPlayback::makeLive(301, totalFrames, producer(state));
    require(owner->prime(target, 2s), "Cannot prime retry fixture");
    std::array<float, 2048> peer{};
    for (std::size_t frame = 0; frame < peer.size() / 2; ++frame) {
        peer[frame * 2] = .03125F;
        peer[frame * 2 + 1] = -.0625F;
    }
    RenderCore core(owner->sourceRef(), conflux::audio::AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, target) && core.setPlaying(0, false) && core.setPlaying(1, true) &&
            core.setCueFrame(0, 3) && core.setDeckGain(0, .5F) && core.setStemGain(0, 1, .25F),
            "Cannot restore retry fixture transport/gains");
    const auto oldEpoch = state->epoch.load();
    require(core.seek(0, target) && core.jog(0, 0), "No-op transport setup failed");
    RenderCore::Command retry{RenderCore::Command::Type::RetrySource, 0};
    require(core.apply(retry), "Explicit same-cursor retry command was rejected");
    await([&] { return state->epoch.load() > oldEpoch; }, "Same-cursor retry did not invalidate the live epoch");
    require(state->epoch == oldEpoch + 1 && state->target == target && core.sourceFrame(0) == target &&
            !core.isPlaying(0) && core.cueFrame(0) == 3 && core.deckGain(0) == .5F && core.stemGain(0, 1) == .25F &&
            core.frameClock() == 0, "Retry changed unrelated transport/gains or notified more than once");
    std::array<float, 128> output{};
    require(core.render(output), "Peer render failed while live retry was pending");
    for (std::size_t frame = 0; frame < 64; ++frame) {
        require(output[frame * 2] == .03125F && output[frame * 2 + 1] == -.0625F,
                "Same-cursor retry disturbed the audible peer");
    }
    require(core.frameClock() == 64 && core.sourceFrame(0) == target && core.sourceFrame(1) == 64 &&
            !core.restorePrimedCursor(0, target), "Pre-stream restore remained usable after rendering");
    retry.deck = 1;
    require(!core.apply(retry), "Memory-only peer accepted an unsupported source retry");
    owner->stop();
}

void renderedOwnerRebuildPreservesCursorTargetsAndProducer()
{
    constexpr std::uint64_t target = 512;
    constexpr std::array<float, 4> gains{.125F, .25F, .5F, 1.F};
    auto state = std::make_shared<Control>();
    auto owner = PreparedPlayback::makeLive(351, totalFrames, producer(state));
    require(owner->prime(target, 2s), "Cannot prime rendered-owner rebuild fixture");
    std::uint64_t cursor = 0;
    {
        RenderCore core(owner->sourceRef());
        require(core.restorePrimedCursor(0, target), "Cannot restore initial rendered-owner position");
        for (std::size_t role = 0; role < 4; ++role)
            require(core.setStemGain(0, role, gains[role]), "Cannot set independent bank targets before first render");
        std::array<float, 128> samples{};
        require(core.render(samples), "Initial whole owner did not actually render");
        for (std::size_t frame = 0; frame < 64; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                double expected = 0;
                for (std::size_t role = 0; role < 4; ++role)
                    expected += double(sample(role, target + frame, channel)) * gains[role];
                require(samples[frame * 2 + channel] == float(expected), "Initial bank targets did not apply to exact PCM");
            }
        cursor = core.sourceFrame();
        require(cursor == target + 64 && !owner->configureStemRamps(0),
                "Existing configureRamps guard was weakened after actual consumption");
    } // Every old borrowed render context is quiesced before reset/reborrow.
    LiveSession::PlaybackSources owners{};
    owners[0] = std::move(owner);
    LiveSession::InitialState initial;
    initial.sourceFrame[0] = cursor;
    initial.cueFrame[0] = target;
    initial.stemGain[0] = gains;
    LiveSession rebuilt(std::move(owners), initial);
    const auto restored = rebuilt.stoppedState();
    require(restored.sourceFrame[0] == cursor && restored.stemGain[0] == gains &&
            state->starts == 1 && state->exits == 0 && !restored.sourceReadError[0],
            "Rebuild lost captured cursor/targets or recreated/stopped the producer");
    auto taken = rebuilt.stopAndTakePlaybacks();
    const auto source = taken.active[0]->sourceRef();
    std::array<float, 128> actual{};
    const auto read = source.read(source.context, cursor, actual);
    require(read.frames == 64 && !read.failed, "Rebuilt bank did not retain the exact continuation prefix");
    for (std::size_t frame = 0; frame < 64; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            double expected = 0;
            for (std::size_t role = 0; role < 4; ++role)
                expected += double(sample(role, cursor + frame, channel)) * gains[role];
            require(actual[frame * 2 + channel] == float(expected), "Rebuild repeated/skipped PCM or lost exact gain targets");
        }
    taken.active[0]->stop();
    bool stoppedResetRejected = false;
    try { taken.active[0]->resetBankForSession(); } catch (const std::exception&) { stoppedResetRejected = true; }
    require(stoppedResetRejected && state->starts == 1 && state->exits == 1,
            "Stopped owner reset was accepted or reuse changed producer lifetime");
}

void wholeOwnerRemainsThroughCallbackEnd()
{
    auto oldState = std::make_shared<Control>(), nextState = std::make_shared<Control>();
    auto old = PreparedPlayback::makeLive(401, totalFrames, producer(oldState));
    auto next = PreparedPlayback::makeLive(402, totalFrames, producer(nextState));
    require(old->prime(0, 2s) && next->prime(0, 2s), "Cannot prime whole-owner handoff fixtures");
    await([&] { return oldState->fullObserved.load() &&
                       oldState->publishedEnd.load() >= (LiveStemBuffer::slotCount - 1) * LiveStemBuffer::blockFrames; },
          "Old producer did not fill its bounded ring before replacement");
    RenderCore core(old->sourceRef());
    require(core.setPlaying(0, false), "Cannot silence old source before handoff");
    SourceHandoff handoff;
    require(handoff.request({0, next->sourceRef(), next.get(), 1}), "Cannot publish live whole owner");
    SourceHandoff::Request request;
    SourceHandoff::Completion completion;
    require(handoff.readPending(request) && core.replaceSource(0, request.source) &&
            !handoff.takeCompletion(completion) && oldState->exits == 0 && nextState->exits == 0,
            "A candidate swap returned ownership before callback-end acknowledgement");
    std::array<float, 32> silence{};
    require(core.render(silence) && handoff.complete(true, {0, 0, false}, core.frameClock()),
            "Cannot complete the final callback use");
    require(oldState->exits == 0 && handoff.takeCompletion(completion) && completion.installed,
            "Acknowledgement stopped storage before its control-owner acquire");
    auto retired = std::move(old);
    old = std::move(next);
    auto retirement = std::async(std::launch::async, [owner = std::move(retired)]() mutable { owner.reset(); });
    retirement.get();
    require(oldState->exits == 1 && nextState->exits == 0, "Retirement stopped the new producer or lost the old one");
    require(core.setPlaying(0, true) && core.render(silence), "Installed live bank failed after old owner retired");
    for (std::size_t frame = 0; frame < 16; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(silence[frame * 2 + channel] == sum(frame, channel), "Installed bank lost its borrowed source storage");
    auto cancelledState = std::make_shared<Control>();
    auto cancelled = PreparedPlayback::makeLive(403, totalFrames, producer(cancelledState));
    require(cancelled->prime(0, 2s) && handoff.request({0, cancelled->sourceRef(), cancelled.get(), 2}),
            "Cannot stage the uninstalled live candidate");
    // The preceding render call has ended. No callback sees this request;
    // unlike an active private graph stop, this branch is deterministic.
    handoff.quiesce();
    require(handoff.takeCompletion(completion) && !completion.installed && completion.generation == 2 &&
            cancelledState->exits == 0 && nextState->exits == 0,
            "Quiescence installed or stopped an unobserved live candidate");
    auto cancelRetirement = std::async(std::launch::async,
        [owner = std::move(cancelled)]() mutable { owner.reset(); });
    cancelRetirement.get();
    require(cancelledState->exits == 1 && nextState->exits == 0 && core.render(silence),
            "Uninstalled candidate retirement affected the retained active owner");
    for (std::size_t frame = 0; frame < 16; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(silence[frame * 2 + channel] == sum(16 + frame, channel),
                    "Uninstalled candidate cancellation moved the active source cursor");
    old->stop();
}
} // namespace

int main()
{
    try {
        reserveCancellationAndExactTail();
        producerFailureIsAtomicAndTerminal();
        nonzeroInitializationRetainsThePrimedEpoch();
        sameCursorRetryUsesTheRealRenderCommand();
        renderedOwnerRebuildPreservesCursorTargetsAndProducer();
        wholeOwnerRemainsThroughCallbackEnd();
        std::cout << "Live whole-owner reserve, generation, initial cursor, failure and retirement checks completed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

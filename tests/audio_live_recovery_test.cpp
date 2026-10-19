// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioSource.hpp"
#include "audio/LiveRecoveryPolicy.hpp"
#include "audio/PreparedPlayback.hpp"
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using conflux::audio::AudioSource;
using conflux::audio::LiveStemBuffer;
using conflux::audio::PreparedPlayback;
using conflux::audio::RenderCore;
using Policy = conflux::audio::detail::LiveRecoveryPolicy;
using Kind = Policy::Decision::Kind;
constexpr auto reserve = Policy::reserveFrames;
constexpr std::uint64_t total = 700000;

void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

template<class Predicate> void await(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        if (predicate()) return;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(1ms);
    }
    require(false, message);
}

void deterministicPolicyAndCommit()
{
    const Policy::Clock::time_point start(100s);
    Policy::Demand d{1, 1, 2, 0, true};
    Policy policy(total);
    require(policy.update(d, {2, 0}, start).kind == Kind::None, "Empty reserve was granted");
    require(policy.update(d, {2, reserve - 1}, start + 59s).kind == Kind::None,
            "Partial progress granted readiness");
    const auto expired = policy.update(d, {2, reserve}, start + 60s);
    require(expired.kind == Kind::Timeout && expired.token == 1 && expired.waitSerial == 1,
            "Partial progress renewed the absolute deadline or late readiness bypassed expiry");

    std::atomic<std::uint64_t> pauseWins{1};
    require(Policy::advanceAttempt(pauseWins, 1, 2), "Pause did not replace the old token");
    require(!Policy::commitTimeout(pauseWins, expired) && pauseWins == 2,
            "A stale timeout poisoned a superseding pause");
    require(Policy::advanceAttempt(pauseWins, 2, 3), "Seek did not replace the paused token");
    require(!Policy::commitTimeout(pauseWins, expired) && pauseWins == 3,
            "A stale timeout poisoned a superseding seek");

    std::atomic<std::uint64_t> timeoutWins{1};
    require(Policy::commitTimeout(timeoutWins, expired) && Policy::timedOut(timeoutWins.load()),
            "Current timeout did not commit terminal state");
    require(!Policy::advanceAttempt(timeoutWins, 1, 2) && Policy::timedOut(timeoutWins.load()),
            "Pause/open cleared a timeout which already won");
    require(!Policy::commitTimeout(timeoutWins, expired), "Timeout commit was not exact-token");
    std::atomic<std::uint64_t> boundary{Policy::maximumToken};
    require(!Policy::advanceAttempt(boundary, Policy::maximumToken, 0) &&
            boundary == Policy::maximumToken, "Attempt token wrapped");
    require(!Policy::advanceAttempt(pauseWins, 3, 3) && pauseWins == 3, "Attempt token was reused");

    Policy paused(total);
    require(paused.update(d, {2, 0}, start).kind == Kind::None, "Initial attempt failed");
    auto inactive = d; inactive.token = 2; inactive.active = false;
    require(paused.update(inactive, {2, 0}, start + 120s).kind == Kind::None,
            "Paused waiting source expired");
    auto resumed = d; resumed.token = 3;
    require(paused.update(resumed, {2, 0}, start + 120s).kind == Kind::None,
            "Resume inherited the paused attempt's deadline");
    require(paused.update(resumed, {2, reserve - 1}, start + 179s).kind == Kind::None,
            "Resumed attempt timed out early");
    require(paused.update(resumed, {2, reserve - 1}, start + 180s).kind == Kind::Timeout,
            "Resumed attempt was not bounded");

    Policy coalesced(total);
    (void)coalesced.update(d, {2, 0}, start);
    require(coalesced.update(resumed, {2, 0}, start + 120s).kind == Kind::None,
            "A pause/resume pair coalesced between polls did not reset the attempt");

    Policy ready(total);
    require(ready.update(d, {1, reserve}, start).kind == Kind::None, "Wrong-epoch reserve granted");
    require(ready.update(d, {2, reserve}, start + 1s).kind == Kind::Grant, "Exact reserve not granted");
    auto open = d; open.token = 2; open.waitSerial = 0; open.cursor = 1;
    require(ready.update(open, {2, reserve}, start + 1000s).kind == Kind::None,
            "An opened active gate rearmed after60s");
    Policy nearEof(total);
    auto tail = d; tail.cursor = total - 7;
    require(nearEof.update(tail, {2, total - 1}, start).kind == Kind::None,
            "Incomplete clipped tail granted");
    require(nearEof.update(tail, {2, total}, start + 1s).kind == Kind::Grant,
            "Exact remaining tail did not clip reserve");
    tail.token = 2; tail.cursor = total;
    require(nearEof.update(tail, {2, total}, start + 1000s).kind == Kind::None,
            "Known EOF rearmed recovery");
    Policy pausedReady(total);
    require(pausedReady.update(inactive, {2, reserve}, start).kind == Kind::Grant,
            "Paused ready reserve could not be pre-granted");
}

struct ActivitySource {
    std::array<float, 32> samples{};
    std::array<bool, 32> edges{};
    std::size_t count = 0, notifications = 0;
    bool active = false;
    AudioSource source()
    {
        auto source = AudioSource::fromMemory(samples);
        source.context = this;
        source.setRenderActive = [](void* context, bool active) noexcept {
            auto& self = *static_cast<ActivitySource*>(context);
            ++self.notifications;
            if (self.active != active) {
                if (self.count < self.edges.size()) self.edges[self.count++] = active;
                self.active = active;
            }
        };
        return source;
    }
};

void actualRenderActivityEdges()
{
    ActivitySource a, b;
    RenderCore core(a.source());
    std::array<float, 2> output{};
    require(core.render(output) && a.active, "Valid render did not refresh source activity");
    a.count = 0;
    require(core.setPlaying(0, false) && core.setPlaying(0, true),
            "Batched pause/play rejected");
    require(a.count == 2 && !a.edges[0] && a.edges[1],
            "Pause/play between blocks disappeared from activity");
    require(core.setDeckGain(0, 0) && core.setMasterGain(0) && core.render(output) && a.active,
            "Muted playing deck was treated as inactive");
    require(core.setPlaying(0, false) && core.seek(0) && core.setCueFrame(0, 0),
            "Cue fixture setup failed");
    a.count = 0;
    require(core.cuePress(0) && a.active && core.cueRelease(0) && !a.active,
            "Held-CUE preview/release activity incorrect");
    require(a.count == 2 && a.edges[0] && !a.edges[1],
            "CUE edges between renders disappeared");
    const auto notifications = a.notifications;
    std::array<float, 3> invalid{};
    require(!core.render(invalid), "Odd render geometry accepted");
    (void)core.render({});
    require(!core.setPlaying(5, true) && !core.seek(0, 17), "Invalid transport accepted");
    require(a.notifications == notifications, "Invalid/empty operation notified activity");
    require(core.setPlaying(0, true) && core.seek(0, 16) && !a.active,
            "Requested playing at EOF remained active");
    require(core.seek(0, 0) && a.active, "Seek away from EOF missed activity");
    require(!core.replaceSource(0, b.source()) && a.active, "Rejected replacement disarmed old source");
    require(core.setPlaying(0, false), "Replacement pause failed");
    const auto beforeRemoval = a.notifications;
    require(core.replaceSource(0, b.source()) && !a.active &&
            a.notifications > beforeRemoval && !b.active,
            "Accepted source removal omitted old-source disarm");
}

float sample(std::size_t stem, std::uint64_t frame, std::size_t channel)
{
    const auto value = float((stem + 1) * 16 + frame % 31) / 4096.F;
    return channel ? -value : value;
}
float mixed(std::uint64_t frame, std::size_t channel)
{
    double value = 0;
    for (std::size_t stem = 0; stem < 4; ++stem) value += sample(stem, frame, channel);
    return float(value);
}
struct Control {
    std::atomic<std::uint64_t> allowance{reserve}, epoch{0}, target{0}, end{0}, turns{0};
    std::atomic<bool> enabled{true}, idle{false};
    std::atomic<int> failure{0};
};
PreparedPlayback::LiveProducer producer(std::shared_ptr<Control> state)
{
    return [state](std::stop_token stop, PreparedPlayback::LiveProducerPort& port) {
        std::array<std::array<float, LiveStemBuffer::blockFrames * 2>, 4> pcm{};
        std::uint64_t epoch = 0, cursor = 0, origin = 0, cachedEpoch = 0, cachedCursor = 0;
        std::size_t cachedCount = 0;
        while (!stop.stop_requested()) {
            if (!state->enabled.load()) {
                state->idle.store(true);
                std::this_thread::sleep_for(1ms);
                continue;
            }
            state->idle.store(false);
            if (const auto error = state->failure.load()) { port.fail(error); break; }
            if (!port.serviceRecovery()) break;
            ++state->turns;
            LiveStemBuffer::Request request;
            if (!port.tryRequest(request)) { std::this_thread::sleep_for(1ms); continue; }
            if (epoch != request.epoch) {
                epoch = request.epoch; cursor = origin = request.firstFrame;
                state->target.store(origin); state->end.store(origin); state->epoch.store(epoch);
            }
            const auto allowance = state->allowance.load();
            if (cursor == total || cursor - origin >= allowance) {
                std::this_thread::sleep_for(1ms); continue;
            }
            const auto count = std::size_t(std::min<std::uint64_t>(
                {total - cursor, LiveStemBuffer::blockFrames, allowance - (cursor - origin)}));
            // Retain a Full candidate; repeated retries do not regenerate PCM.
            if (cachedEpoch != epoch || cachedCursor != cursor || cachedCount != count) {
                for (std::size_t stem = 0; stem < 4; ++stem)
                    for (std::size_t frame = 0; frame < count; ++frame)
                        for (std::size_t channel = 0; channel < 2; ++channel)
                            pcm[stem][frame * 2 + channel] = sample(stem, cursor + frame, channel);
                cachedEpoch = epoch; cachedCursor = cursor; cachedCount = count;
            }
            LiveStemBuffer::Packet packet{request.sourceId, epoch, cursor, count, cursor + count == total, {}};
            for (std::size_t stem = 0; stem < 4; ++stem)
                packet.stems[stem] = std::span(pcm[stem]).first(count * 2);
            const auto result = port.tryPublish(packet);
            if (result == LiveStemBuffer::PublishResult::Accepted) {
                cursor += count; state->end.store(cursor);
            } else if (result == LiveStemBuffer::PublishResult::Invalid) {
                LiveStemBuffer::Request current;
                if (port.tryRequest(current)) port.fail(EPROTO);
                break;
            } else std::this_thread::sleep_for(1ms);
        }
    };
}

void realOwnerReserveRecoveryAndPeer()
{
    auto state = std::make_shared<Control>();
    auto owner = PreparedPlayback::makeLive(901, total, producer(state));
    require(owner->prime(0, 10s), "Initial exact reserve did not prime");
    const auto primedEpoch = state->epoch.load();
    std::vector<float> peer(1000000 * 2);
    for (std::size_t frame = 0; frame < peer.size() / 2; ++frame) {
        peer[frame * 2] = .125F; peer[frame * 2 + 1] = -.25F;
    }
    RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
    auto render = [&](std::size_t frames) {
        const auto first = core.sourceFrame(0), peerFirst = core.sourceFrame(1), clock = core.frameClock();
        std::vector<float> output(frames * 2, 100.F);
        require(core.render(output), "Valid bank/peer render rejected");
        const auto delivered = core.sourceFrame(0) - first;
        require(delivered <= frames && core.sourceFrame(1) == peerFirst + frames &&
                core.frameClock() == clock + frames, "Starvation changed peer or output clock");
        for (std::size_t frame = 0; frame < frames; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto expected = (channel ? -.25F : .125F) +
                    (frame < delivered ? mixed(first + frame, channel) : 0.F);
                require(output[frame * 2 + channel] == expected, "Master included wrong/stale/duplicated PCM");
            }
        return delivered;
    };
    while (core.sourceFrame(0) < reserve - 17)
        require(render(std::size_t(std::min<std::uint64_t>(1024, reserve - 17 - core.sourceFrame(0)))) > 0,
                "Primed prefix stalled");
    require(render(32) == 17 && core.sourceFrame(0) == reserve && core.isBuffering(0),
            "Underrun discarded a valid partial prefix or advanced through its missing suffix");
    state->allowance = reserve * 2 - 1;
    await([&] { return state->end == reserve * 2 - 1; }, "Recovery sub-reserve was not published");
    require(render(1) == 0 && core.sourceFrame(0) == reserve,
            "Closed gate consumed sub-reserve data");
    state->allowance = reserve * 2;
    await([&] { return state->end == reserve * 2; }, "Exact recovery reserve not published");
    await([&] { return render(1) == 1; }, "Exact recovery reserve did not open");
    require(core.sourceFrame(0) == reserve + 1 && state->epoch == primedEpoch,
            "Recovery skipped the held cursor or reset the provider/converter epoch");

    auto park = [&] {
        state->enabled = false;
        await([&] { return state->idle.load(); }, "Producer did not park between bounded operations");
    };
    auto run = [&] { state->enabled = true; };
    park();
    const auto oldEpoch = state->epoch.load();
    state->allowance = total - 600000;
    require(core.seek(0, 600000), "Late seek rejected");
    run();
    await([&] { return state->epoch > oldEpoch && state->target == 600000 && state->end == total; },
          "Clipped ready tail was not accepted");
    const auto turn = state->turns.load();
    await([&] { return state->turns > turn + 2; }, "Producer did not service ready tail grant");
    park();
    const auto readyEpoch = state->epoch.load();
    state->allowance = total - 600000 - 1;
    require(core.retrySource(0), "Same-cursor retry rejected");
    run();
    await([&] { return state->epoch > readyEpoch && state->end == total - 1; },
          "Same-cursor retry did not make a new epoch");
    require(render(3) == 0 && core.sourceFrame(0) == 600000,
            "Old ready grant opened a new same-cursor retry");

    const auto source = owner->sourceRef();
    std::array<float, 2> sentinel{99.F, 99.F};
    const auto wrong = source.read(source.context, 600001, sentinel);
    require(wrong.frames == 0 && wrong.failed && sentinel[0] == 99.F && sentinel[1] == 99.F,
            "Mismatched read moved or wrote live state");
    const auto empty = source.read(source.context, 600000, {});
    require(empty.frames == 0 && !empty.failed, "Valid empty read consumed data or failed");
    std::array<float, LiveStemBuffer::blockFrames * 2 + 2> oversized{};
    require(source.read(source.context, 600000, oversized).failed, "Oversized bank read accepted");
    std::array<float, 3> odd{};
    require(source.read(source.context, 600000, odd).failed, "Odd bank read accepted");
    state->allowance = total - 600000;
    await([&] { return state->end == total; }, "Final clipped reserve did not publish");
    await([&] { return render(1) == 1; }, "Clipped exact EOF reserve did not open");
    require(core.sourceFrame(0) == 600001, "Invalid/empty reads altered the retry origin");

    state->failure = EIO;
    await([&] { return owner->errorCode() == EIO; }, "Producer failure did not surface");
    require(render(4) == 0 && core.readFailed(0), "Queued reserve hid terminal producer failure");
    require(core.retrySource(0) && render(4) == 0 && owner->errorCode() == EIO,
            "Retry or ready coverage cleared terminal producer health");
    require(core.seek(0, total) && render(4) == 0 && owner->errorCode() == EIO,
            "Known EOF cleared terminal producer health");
}
} // namespace

int main()
{
    try {
        deterministicPolicyAndCommit();
        actualRenderActivityEdges();
        realOwnerReserveRecoveryAndPeer();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

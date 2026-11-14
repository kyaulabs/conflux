// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioSource.hpp"
#include "audio/LiveRecoveryPolicy.hpp"
#include "audio/PreparedPlayback.hpp"
#include "audio/RenderCore.hpp"
#include "audio/ResidentStemHold.hpp"
#include "audio/RecorderTap.hpp"
#include "audio/StemBankSource.hpp"

#include <sndfile.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
using conflux::audio::AudioResidentHoldCommand;
using conflux::audio::AudioResidentHoldRange;
using conflux::audio::ResidentStemHold;
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
// Full-frame, role- and channel-asymmetric dyadic markers. Short periodic
// signals cannot distinguish a genuine parked-F exit from a stale loop turn.
float holdSample(std::size_t stem, std::uint64_t frame, std::size_t channel)
{
    return channel ? -float((stem + 1) * 524288 + 2 * frame + 1) / 33554432.F
                   : float((stem + 1) * 1048576 + frame) / 33554432.F;
}
struct Control {
    std::atomic<std::uint64_t> allowance{reserve}, epoch{0}, target{0}, end{0}, turns{0};
    std::atomic<bool> enabled{true}, idle{false};
    std::atomic<int> failure{0};
};
PreparedPlayback::LiveProducer producer(std::shared_ptr<Control> state, bool markers = false)
{
    return [state, markers](std::stop_token stop, PreparedPlayback::LiveProducerPort& port) {
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
                            pcm[stem][frame * 2 + channel] = markers ? holdSample(stem, cursor + frame, channel)
                                                                  : sample(stem, cursor + frame, channel);
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

AudioResidentHoldRange approvedHold(std::uint64_t first = reserve - 2048)
{
    AudioResidentHoldRange range;
    range.id = 771; range.inFrame = first; range.outFrame = first + 2048;
    range.admittedBytes = 2048 * 32;
    range.provenance = AudioResidentHoldRange::Provenance::DjRange;
    return range;
}

void residentRangeValidationAndEpochCapture()
{
    const auto valid = approvedHold(0);
    auto rejects = [&](AudioResidentHoldRange range, bool budget = false) {
        std::atomic<unsigned> started{0};
        bool rejected = false;
        try {
            auto owner = PreparedPlayback::makeLive(901, total,
                [&](std::stop_token, PreparedPlayback::LiveProducerPort&) { ++started; }, range);
        } catch (const std::length_error&) { rejected = budget; }
          catch (const std::invalid_argument&) { rejected = !budget; }
        require(rejected && !started.load(), "Invalid resident range started its producer or bypassed admission");
    };
    auto bad = valid; bad.id = 0; rejects(bad);
    bad = valid; bad.provenance = AudioResidentHoldRange::Provenance::None; rejects(bad);
    bad = valid; bad.provenance = static_cast<AudioResidentHoldRange::Provenance>(99); rejects(bad);
    bad = valid; bad.outFrame = bad.inFrame; rejects(bad);
    bad = valid; bad.outFrame = 2047; rejects(bad);
    bad = valid; bad.outFrame = 384001; rejects(bad);
    bad = valid; bad.inFrame = total - 2047; bad.outFrame = total + 1; rejects(bad);
    bad = valid; bad.inFrame = UINT64_MAX - 2047; bad.outFrame = UINT64_MAX; rejects(bad);
    bad = valid; --bad.admittedBytes; rejects(bad, true);
    ResidentStemHold exact(901, total, valid);
    require(exact.bytes() == 65536 && !exact.ready(), "Exact resident charge or initial readiness differs");
    // The finite live tail has its own authoritative geometry, not the source
    // file's one-frame-shorter original conversion geometry.
    auto tail = approvedHold(1123201 - 2048);
    ResidentStemHold exactTail(901, 1123201, tail);
    bool shortened = false;
    try { ResidentStemHold wrong(901, 1123200, tail); }
    catch (const std::invalid_argument&) { shortened = true; }
    require(shortened, "Original-file geometry incorrectly admitted a live tail range");

    std::array<std::array<float, 2048>, 4> pcm{};
    auto packet = [&](std::uint64_t epoch, std::uint64_t first) {
        for (std::size_t stem = 0; stem < 4; ++stem)
            for (std::size_t frame = 0; frame < 1024; ++frame)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    pcm[stem][frame * 2 + channel] = holdSample(stem, first + frame, channel);
        LiveStemBuffer::Packet value{901, epoch, first, 1024, false, {}};
        for (std::size_t stem = 0; stem < 4; ++stem) value.stems[stem] = pcm[stem];
        return value;
    };
    auto value = packet(1, 0); value.sourceId = 902; exact.captureAccepted(value);
    require(!exact.ready(), "Foreign source populated resident material");
    value = packet(1, 0); value.stems[2] = value.stems[2].first(2046); exact.captureAccepted(value);
    value = packet(1, 0); pcm[1][17] = std::numeric_limits<float>::quiet_NaN(); exact.captureAccepted(value);
    exact.captureAccepted(packet(1, 1024));
    require(!exact.ready(), "Malformed/gapped packet made an incomplete hold ready");
    exact.captureAccepted(packet(1, 0));
    exact.captureAccepted(packet(2, 1024));
    require(!exact.ready(), "Mixed-epoch halves became a resident range");
    exact.captureAccepted(packet(2, 0)); exact.captureAccepted(packet(2, 1024));
    require(exact.ready(), "Exact coherent accepted range did not become ready");
    const auto leaves = exact.leaves();
    for (std::size_t stem = 0; stem < 4; ++stem)
        for (std::size_t frame = 0; frame < 2048; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(leaves[stem][frame * 2 + channel] == holdSample(stem, frame, channel),
                        "Resident capture changed a role/channel/source frame");
    value = packet(3, 0); pcm[0].fill(99.F); exact.captureAccepted(value);
    require(exact.leaves()[0][0] == holdSample(0, 0, 0) && exact.bytes() == 65536,
            "A ready immutable range was overwritten or charged twice");
}

// Independent per-output scalar reference. It knows the approved source-frame
// sequence and existing envelope formula, never reads the implementation's
// audible cursor or gain-ramp internals to decide its expected samples.
struct HoldOracle {
    struct Gain {
        double current = 0, start = 0, target = 0;
        unsigned position = 0;
        void set(double value) { if (value != target) { start = current; target = value; position = 0; } }
        double next() {
            if (current == target || ++position >= 240) return current = target;
            const double fraction = double(position) / 240;
            return current = (1 - fraction) * start + fraction * target;
        }
    };
    struct Envelope {
        std::array<float, 2> last{}, start{};
        unsigned position = 0;
        bool active = true;
        void boundary() { start = last; position = 0; active = true; }
        std::array<float, 2> apply(std::array<float, 2> value) {
            if (active) {
                if (++position >= 240) active = false;
                else for (std::size_t channel = 0; channel < 2; ++channel) {
                    const double fraction = double(position) / 240;
                    value[channel] = float((1 - fraction) * start[channel] + fraction * value[channel]);
                }
            }
            return last = value;
        }
    };
    std::array<Gain, 4> gains{{{1, 1, 1}, {.5, .5, .5}, {.25, .25, .25}, {0, 0, 0}}};
    Envelope a, b;
    std::uint64_t linear = 0, audible = reserve - 2048, output = 0;
    bool holding = false, enterBoundary = false, exitReady = false, available = true;
    void enter() { holding = enterBoundary = true; audible = reserve - 2048; }
    std::array<float, 4> next() {
        if (enterBoundary) { a.boundary(); enterBoundary = false; }
        if (holding && audible == reserve) {
            a.boundary();
            if (exitReady) holding = false;
            else audible = reserve - 2048;
        }
        const bool supplied = holding || linear < total;
        if (available != supplied) { a.boundary(); available = supplied; }
        const auto frame = holding ? audible++ : linear;
        if (!holding && supplied) ++linear;
        std::array<double, 2> sum{};
        for (std::size_t stem = 0; supplied && stem < 4; ++stem) {
            const auto gain = gains[stem].next();
            for (std::size_t channel = 0; channel < 2; ++channel)
                sum[channel] += double(holdSample(stem, frame, channel)) * gain;
        }
        const auto av = a.apply({float(sum[0]), float(sum[1])});
        const auto bv = b.apply({float(output + 1) / 16777216.F, -float(output + 3) / 33554432.F});
        ++output;
        return {float((double(av[0]) * .5 + double(bv[0]) * .25) * .75),
                float((double(av[1]) * .5 + double(bv[1]) * .25) * .75),
                av[0] * .25F, av[1] * .25F};
    }
};

struct ResidentFixture {
    std::shared_ptr<Control> state = std::make_shared<Control>();
    std::unique_ptr<PreparedPlayback> owner;
    std::vector<float> peer = std::vector<float>(1000000 * 2);
    std::unique_ptr<RenderCore> core;
    HoldOracle oracle;
    std::array<float, 4097 * 2> master{}, cue{};
    ResidentFixture() {
        owner = PreparedPlayback::makeLive(901, total, producer(state, true), approvedHold());
        require(owner->prime(0, 10s), "Resident fixture did not prime its exact live reserve");
        auto source = owner->sourceRef();
        constexpr std::array gains{1.F, .5F, .25F, 0.F};
        for (std::size_t stem = 0; stem < 4; ++stem)
            require(source.setStemGain(source.context, stem, gains[stem]), "Cannot set initial resident stem restrictions");
        require(owner->configureStemRamps(240), "Cannot configure resident bank ramps");
        for (std::size_t frame = 0; frame < peer.size() / 2; ++frame) {
            peer[frame * 2] = float(frame + 1) / 16777216.F;
            peer[frame * 2 + 1] = -float(frame + 3) / 33554432.F;
        }
        core = std::make_unique<RenderCore>(source, AudioSource::fromMemory(peer));
        require(core->setDeckGain(0, .5F) && core->setDeckGain(1, .25F) && core->setMasterGain(.75F) &&
            core->setCueSelected(0, true) && core->setCueGain(.25F) && core->configureRamps(240),
            "Cannot configure resident reference buses");
        const auto hold = core->residentHoldState();
        require(hold.supported && hold.ready && !hold.armed && !hold.active && hold.rangeId == 771 &&
            hold.inFrame == reserve - 2048 && hold.outFrame == reserve && hold.residentBytes == 65536 &&
            owner->residentHoldBytes() == 65536, "Resident Accepted capture or source-bound charge differs");
    }
    void render(std::size_t frames) {
        require(frames && frames <= master.size() / 2, "Resident test requested an invalid block");
        require(core->renderBuses(std::span(master).first(frames * 2), std::span(cue).first(frames * 2)),
                "Resident render rejected valid buses");
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto expected = oracle.next();
            require(master[frame * 2] == expected[0] && master[frame * 2 + 1] == expected[1] &&
                cue[frame * 2] == expected[2] && cue[frame * 2 + 1] == expected[3],
                "Resident PCM changed source frame, role gain, envelope, stereo, CUE or peer");
        }
        require(core->sourceFrame() == oracle.linear && core->sourcePosition() == double(oracle.linear) &&
            core->sourceFrame(1) == oracle.output && core->frameClock() == oracle.output &&
            !core->isBuffering() && !core->starvedFrames(), "Resident audio advanced F or starved/rewound a peer/clock");
    }
    void advance(std::uint64_t frames) {
        constexpr std::array<std::size_t, 4> sizes{1, 73, 1024, 17};
        for (std::size_t n = 0; frames; ++n) {
            const auto count = std::size_t(std::min<std::uint64_t>(frames, sizes[n % sizes.size()]));
            render(count); frames -= count;
        }
    }
    void park() {
        state->enabled = false;
        await([&] { return state->idle.load(); }, "Resident producer did not park off callback");
    }
    void command(AudioResidentHoldCommand command) {
        const auto revision = core->transportRevision(0);
        require(core->residentHoldControl(0, command, 771, revision) && core->transportRevision(0) == revision + 1,
                "Resident command lost its exact transport revision");
    }
    void enterAtF() {
        advance(reserve - 73);
        require(core->setStemGain(0, 0, 0.F), "Cannot start halfway resident mute ramp");
        oracle.gains[0].set(0);
        advance(73); park();
        command(AudioResidentHoldCommand::Arm); command(AudioResidentHoldCommand::Enter); oracle.enter();
        require(core->residentHoldState().active && core->residentHoldState().linearFrame == reserve,
                "Resident entry did not park the exact live cursor");
    }
};

// The finite bank uses the same source-frame/role markers as the live producer.
// Temporary raw leaves exercise the real unused-prime and stem-ramp transfer;
// no original-mix substitute or constant signal can hide a wrong resume frame.
struct ResidentPreparedBank {
    struct Close { void operator()(std::FILE* file) const noexcept { if (file) std::fclose(file); } };
    std::array<std::unique_ptr<std::FILE, Close>, 4> files;
    ResidentPreparedBank() {
        std::array<float, 1024 * 2> block{};
        for (std::size_t stem = 0; stem < files.size(); ++stem) {
            files[stem].reset(std::tmpfile());
            require(bool(files[stem]), "Cannot create resident continuation leaf");
            for (std::uint64_t first = 0; first < total;) {
                const auto count = std::size_t(std::min<std::uint64_t>(1024, total-first));
                for (std::size_t frame = 0; frame < count; ++frame)
                    for (std::size_t channel = 0; channel < 2; ++channel)
                        block[frame*2+channel] = holdSample(stem, first+frame, channel);
                require(std::fwrite(block.data(), sizeof(float), count*2, files[stem].get()) == count*2,
                    "Cannot write resident continuation leaf");
                first += count;
            }
            require(std::fflush(files[stem].get()) == 0, "Cannot flush resident continuation leaf");
        }
    }
    std::array<int, 4> fds() const {
        return {fileno(files[0].get()), fileno(files[1].get()), fileno(files[2].get()), fileno(files[3].get())};
    }
};

void residentPreparedTakeoverAtExactOutPreservesMarkerRampsAndFailure()
{
    using Status = RenderCore::ContinuationStatus;
    ResidentPreparedBank leaves;
    for (const bool producerFails : {false, true}) {
        // Destruction order keeps the candidate alive through the core's final
        // borrowed use, just as the callback mailbox retains both owners.
        auto candidate = std::make_unique<PreparedPlayback>(leaves.fds(), total);
        ResidentFixture f;
        require(f.core->setCueFrame(0, 113), "Cannot retain a nonzero resident CUE marker");
        f.enterAtF();
        const auto epoch = f.state->epoch.load();
        const auto revision = f.core->transportRevision(0);
        const auto clock = f.core->frameClock();
        require(candidate->prime(reserve, 2s) && candidate->primedForContinuation(reserve),
            "Finite resident continuation did not prime exact parked F");
        const auto source = candidate->sourceRef();
        if (producerFails) {
            f.state->failure = EIO; f.state->enabled = true;
            await([&] { return f.owner->errorCode() == EIO; }, "Held producer failure did not settle");
        }
        require(!f.core->readFailed(), "Resident PCM unexpectedly fabricated a failed render read");
        require(!f.core->canContinueSource(0, source, reserve, revision) &&
            !f.core->continueSource(0, source, reserve, revision) &&
            f.core->continuationStatus(0, source, reserve, revision, 771) == Status::WaitingForHoldOut,
            "Ordinary continuation cut an active hold or boundary mode lost its waiting state");
        f.render(61); // Neither approved OUT nor the remaining1987frames align to1024.
        require(!f.core->continueSource(0, source, reserve, revision, 771) &&
            candidate->primedForContinuation(reserve) && f.core->sourceFrame() == reserve,
            "Early boundary request consumed candidate PCM or moved parked F");
        f.render(1970);
        require(f.core->residentHoldState().audibleFrame == reserve-17 &&
            f.core->continuationPrefixFrames(0,1024)==17 && f.core->continuationPrefixFrames(0,3)==3 &&
            f.core->setStemGain(0, 0, .375F), "Cannot bound the raw OUT prefix or retarget its mute");
        f.oracle.gains[0].set(.375);
        f.render(17); // Last old-source prefix; ramp is now17/240, not its earlier value.
        require(f.core->residentHoldState().audibleFrame == reserve && f.core->continuationPrefixFrames(0,1024)==0 &&
            f.core->sourceFrame() == reserve && f.core->transportRevision(0) == revision &&
            f.core->continuationStatus(0, source, reserve, revision, 771) == Status::Ready &&
            candidate->primedForContinuation(reserve), "Exact OUT was skipped, wrapped or consumed finite readiness");
        require(candidate->copyStemMixStateFrom(*f.owner) &&
            f.core->continueSource(0, source, reserve, revision, 771), "Exact OUT failed its current ramp-state takeover");
        f.oracle.exitReady = true;
        require(f.core->sourceFrame() == reserve && f.core->transportRevision(0) == revision+1 &&
            f.core->cueFrame(0) == 113 && f.core->isCueSelected(0) && f.core->isPlaying(0) &&
            !f.owner->sourceRef().residentHoldState(f.owner.get()).active &&
            !f.core->residentHoldState().supported && f.core->readFailed() == producerFails &&
            f.owner->errorCode() == (producerFails ? EIO : 0) && !candidate->errorCode(),
            "Commit reset F, retained an old hold borrow, copied failure or lost cumulative producer health");
        f.render(33);
        require(f.core->setStemGain(0, 0, .25F), "Cannot retarget transferred stem trajectory");
        f.oracle.gains[0].set(.25);
        f.render(974); // Core scalar/bus proof; actual LiveSession span splitting needs its owned callback test.
        require(!candidate->primedForContinuation(reserve) && f.core->sourceFrame() == reserve+1007 &&
            f.core->sourceFrame(1) == clock+2048+1007 && f.core->frameClock() == clock+2048+1007 &&
            f.core->stemGain(0, 3) == 0.F && f.core->readFailed() == producerFails &&
            f.state->epoch == epoch && !f.core->starvedFrames(),
            "Takeover changed a master/CUE marker, peer clock, role exclusion, failure or live epoch");
    }
}

void residentPreparedTakeoverRejectsNewerManualIntent()
{
    using Status = RenderCore::ContinuationStatus;
    ResidentPreparedBank leaves;
    auto candidate = std::make_unique<PreparedPlayback>(leaves.fds(), total);
    ResidentFixture f; f.enterAtF();
    require(candidate->prime(reserve, 2s), "Cannot prime stale resident candidate");
    const auto source = candidate->sourceRef();
    const auto revision = f.core->transportRevision(0);
    require(f.core->continuationStatus(0, source, reserve, revision, 772) == Status::Invalid &&
        f.core->continuationStatus(0, source, reserve+1, revision, 771) == Status::Invalid &&
        f.core->continuationStatus(0, source, reserve, revision-1, 771) == Status::Invalid &&
        f.core->continuationStatus(0, source, reserve, UINT64_MAX, 771) == Status::Invalid &&
        candidate->primedForContinuation(reserve) && f.core->residentHoldState().active,
        "Rejected range/frame/revision mutated a retained owner or candidate");
    require(f.core->setDeckGain(0, .375F) && f.core->continuationStatus(0, source, reserve, revision, 771) ==
        Status::WaitingForHoldOut, "Human mix gain incorrectly revoked the source-bound boundary");
    f.command(AudioResidentHoldCommand::Exit);
    require(f.core->continuationStatus(0, source, reserve, revision, 771) == Status::Invalid &&
        f.core->continuationStatus(0, source, reserve, f.core->transportRevision(0), 771) == Status::Invalid,
        "Explicit resume intent was overwritten by an automatic prepared takeover");
    f.command(AudioResidentHoldCommand::Cancel);
    f.command(AudioResidentHoldCommand::Arm); f.command(AudioResidentHoldCommand::Enter);
    const auto rearmed = f.core->transportRevision(0);
    require(f.core->continuationStatus(0, source, reserve, rearmed, 771) == Status::WaitingForHoldOut &&
        f.core->seek(0, reserve+1) && f.core->seek(0, reserve) &&
        f.core->continuationStatus(0, source, reserve, rearmed, 771) == Status::Invalid &&
        !f.core->continueSource(0, source, reserve, rearmed, 771) &&
        !f.core->residentHoldState().active && candidate->primedForContinuation(reserve),
        "Seek-away/back reused an old range continuation or consumed rejected candidate readiness");
}

void residentLoopPcmRampsReserveExitAndRecording()
{
    ResidentFixture f; f.enterAtF();
    const auto epoch = f.state->epoch.load();
    const auto revision = f.core->transportRevision(0);
    auto pattern = (std::filesystem::temp_directory_path() / "conflux-resident-hold-XXXXXX").string();
    const auto* created = mkdtemp(pattern.data());
    require(created, "Cannot create resident recording fixture");
    const std::filesystem::path directory(created), path = directory / "hold.wav";
    struct Remove { std::filesystem::path path; ~Remove() { std::error_code code; std::filesystem::remove_all(path, code); } } remove{directory};
    conflux::audio::RecorderTap tap;
    conflux::audio::StereoRecorder writer(path);
    std::vector<float> expected;
    expected.reserve(32768);
    require(tap.request(&writer), "Cannot attach resident recording");
    auto recorded = [&](std::size_t frames) {
        f.render(frames);
        const auto pcm = std::span(f.master).first(frames * 2);
        tap.beginBlock(); const auto pushed = tap.push(pcm); tap.endBlock();
        require(pushed, "Resident recorder rejected generated master PCM");
        expected.insert(expected.end(), pcm.begin(), pcm.end());
    };
    recorded(61);
    require(f.core->setStemGain(0, 0, .375F), "Cannot retarget the in-flight resident mute");
    f.oracle.gains[0].set(.375);
    require(!f.core->residentHoldControl(0, AudioResidentHoldCommand::Cancel, 771, revision - 1) &&
        f.core->residentHoldState().active, "Stale cancellation changed a current resident hold");
    for (const auto count : {1024U, 17U, 1U, 73U, 4097U, 1024U}) recorded(count);
    require(f.core->transportRevision(0) == revision && f.state->epoch == epoch && f.state->end == reserve &&
        f.core->stemGain(0, 3) == 0.F && f.core->residentHoldState().residentBytes == 65536,
        "Resident wraps changed a generation/revision, exclusion or bounded charge");
    f.command(AudioResidentHoldCommand::Exit);
    recorded(2048);
    require(f.core->residentHoldState().exitPending && f.core->residentHoldState().active,
            "An empty reserve exited an approved hold");
    f.state->allowance = reserve * 2 - 1; f.state->enabled = true;
    await([&] { return f.state->end == reserve * 2 - 1; }, "Resident sub-reserve was not published");
    recorded(2048);
    require(f.core->residentHoldState().active && f.core->sourceFrame() == reserve,
            "A one-frame-short live reserve bypassed resident exit readiness");
    f.state->allowance = reserve * 2;
    await([&] { return f.state->end == reserve * 2; }, "Resident exit reserve was not published");
    const auto turns = f.state->turns.load();
    await([&] { return f.state->turns > turns + 2; }, "Resident reserve grant was not serviced");
    f.oracle.exitReady = true;
    const auto remaining = reserve - f.oracle.audible;
    for (std::uint64_t left = remaining + 97; left;) {
        const auto count = std::size_t(std::min<std::uint64_t>(left, 1024));
        recorded(count); left -= count;
    }
    require(!f.core->residentHoldState().active && f.core->sourceFrame() == reserve + 97 &&
        f.state->epoch == epoch && f.owner->errorCode() == 0 && f.core->stemGain(0, 3) == 0.F,
        "Resident exit restarted inference, skipped F or revived an excluded stem");
    require(tap.request(nullptr), "Cannot detach resident recorder");
    tap.beginBlock(); tap.endBlock(); writer.stop();
    require(!tap.attached() && !tap.changePending() && !writer.running() && !writer.failed() &&
        writer.acceptedFrames() == expected.size() / 2 && writer.writtenFrames() == expected.size() / 2 &&
        writer.overflowCount() == 0, "Resident recording lost accepted frames or ownership acknowledgement");
    SF_INFO info{};
    std::unique_ptr<SNDFILE, int(*)(SNDFILE*)> input(sf_open(path.c_str(), SFM_READ, &info), sf_close);
    require(input && info.frames == sf_count_t(expected.size() / 2) && info.channels == 2 && info.samplerate == 48000,
            "Resident WAV geometry differs from its complete generated master");
    std::vector<float> actual(expected.size());
    require(sf_readf_float(input.get(), actual.data(), info.frames) == info.frames && actual == expected &&
        sf_error(input.get()) == SF_ERR_NO_ERROR, "Resident WAV changed an entry/wrap/exit or peer sample");
}

void residentTerminalFailureKeepsEverySubblockAudible()
{
    ResidentFixture f; f.enterAtF();
    const auto epoch = f.state->epoch.load();
    f.state->failure = EIO; f.state->enabled = true;
    await([&] { return f.owner->errorCode() == EIO; }, "Resident producer EIO did not latch");
    f.render(4097); // More than four internal1024-frame reads in ONE callback.
    f.command(AudioResidentHoldCommand::Exit);
    f.render(4097);
    require(f.owner->errorCode() == EIO && f.core->residentHoldState().errorCode == EIO &&
        f.core->residentHoldState().active && f.core->residentHoldState().exitPending &&
        f.core->sourceFrame() == reserve && f.state->epoch == epoch && f.core->stemGain(0, 3) == 0.F,
        "Resident data hid terminal health, auto-exited or rewound/restarted a failed provider");
    require(f.core->retrySource(0) && !f.core->residentHoldState().armed &&
        !f.core->residentHoldState().active && f.owner->errorCode() == EIO,
        "Human retry retained the old loop or cleared permanent producer health");
}

void residentNearEofFailureDoesNotTruncateCallbackOrLoopHealthyEnd()
{
    for (const bool fail : {false, true}) {
        ResidentFixture f;
        constexpr auto first = total - 64;
        require(f.owner->prime(first, 10s) && f.core->restorePrimedCursor(0, first),
                "Cannot prime the exact finite tail for resident EOF checks");
        f.oracle.linear = first;
        f.command(AudioResidentHoldCommand::Arm);
        if (fail) {
            f.state->failure = EIO;
            await([&] { return f.owner->errorCode() == EIO; }, "Near-EOF producer failure did not latch");
            f.oracle.enter(); // Expected automatic entry on the first failed read.
        }
        const auto revision = f.core->transportRevision(0);
        f.render(4097);
        require(f.core->transportRevision(0) == revision &&
            f.core->residentHoldState().active == fail &&
            f.core->sourceFrame() == (fail ? first : total) &&
            f.owner->errorCode() == (fail ? EIO : 0),
            "Near-EOF hold truncated a callback, advanced F, or repeated healthy EOF");
    }
}

void residentPendingCaptureAndManualIntent()
{
    auto state = std::make_shared<Control>(); state->enabled = false; state->allowance = 1024;
    auto owner = PreparedPlayback::makeLive(901, total, producer(state, true), approvedHold(0));
    std::array<float, 1024> peer{}; peer.fill(.125F);
    RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
    const auto range = approvedHold(0);
    auto command = [&](AudioResidentHoldCommand value) {
        return core.residentHoldControl(0, value, range.id, core.transportRevision(0));
    };
    require(command(AudioResidentHoldCommand::Arm) && core.residentHoldState().armed &&
        !core.residentHoldState().ready && !command(AudioResidentHoldCommand::Enter),
        "Pending arm required ready PCM or entered before Accepted capture");
    std::array<float, 32> output{};
    require(core.render(output) && core.sourceFrame() == 0 && !core.residentHoldState().active &&
        std::ranges::all_of(output, [](float value) { return value == .125F; }),
        "An incomplete resident range leaked PCM or advanced the live source");
    state->enabled = true;
    await([&] { return state->end == 1024; }, "Pending hold half did not publish");
    require(!core.residentHoldState().ready && core.render(output) && core.sourceFrame() == 0 &&
        !core.residentHoldState().active, "One accepted half became an audible hold");
    state->allowance = 2048;
    await([&] { return state->end == 2048; }, "Pending exact hold did not publish");
    const auto revision = core.transportRevision(0);
    require(core.render(output) && core.residentHoldState().ready && core.residentHoldState().active &&
        core.sourceFrame() == 0 && core.transportRevision(0) == revision,
        "Complete explicitly armed range did not recover the same exact cursor");
    for (std::size_t frame = 0; frame < 16; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            double expected = .125;
            double stems = 0;
            for (std::size_t stem = 0; stem < 4; ++stem) stems += holdSample(stem, frame, channel);
            require(output[frame * 2 + channel] == float(double(float(stems)) + expected),
                    "Pending-capture recovery changed a complete raw stem marker");
        }
    require(!core.residentHoldControl(0, AudioResidentHoldCommand::Cancel, range.id, revision - 1) &&
        core.residentHoldState().active, "Stale transport intent cancelled a current hold");
    require(!core.residentHoldControl(0, AudioResidentHoldCommand::Cancel, 0, revision) &&
        !core.residentHoldControl(0, AudioResidentHoldCommand::Cancel, range.id + 1, revision) &&
        !core.residentHoldControl(0, AudioResidentHoldCommand::Cancel, range.id, UINT64_MAX) &&
        core.residentHoldState().active && core.transportRevision(0) == revision,
        "Wrong-range or saturated-revision cancellation mutated a current hold");
    require(core.setStemGain(0, 3, 0.F) && core.setEqGainDb(0, 1, -6.F) && core.setCueSelected(0, true) &&
        core.residentHoldState().active, "Current human mix controls cancelled an approved hold");
    require(core.setPlaying(0, false) && !core.residentHoldState().armed && !core.residentHoldState().active &&
        core.sourceFrame() == 0, "Human pause rewound F or failed to cancel the resident arm");
    require(command(AudioResidentHoldCommand::Arm) && !command(AudioResidentHoldCommand::Enter) &&
        core.setPlaying(0, true) && !core.residentHoldState().armed,
        "Paused entry or later Play silently retained an old arm");
    require(command(AudioResidentHoldCommand::Arm), "Cannot explicitly rearm retained resident PCM");
    const auto ticket = core.transportRevision(0);
    require(core.seek(0, 13) && core.seek(0, 0) && !core.residentHoldState().armed &&
        !core.residentHoldControl(0, AudioResidentHoldCommand::Enter, range.id, ticket),
        "Seek-away-and-back reused a stale resident ticket");
    require(command(AudioResidentHoldCommand::Arm) && core.cuePress(0) &&
        !core.residentHoldState().armed && !core.residentHoldState().active && core.cueRelease(0),
        "Human CUE retained or resurrected a resident arm");
    require(core.seek(0, total) && !command(AudioResidentHoldCommand::Arm) &&
        !command(AudioResidentHoldCommand::Enter) && core.sourceFrame() == total,
        "Authoritative EOF started an automatic resident loop");
    const auto oldRevision = core.transportRevision(0);
    require(core.replaceSource(0, AudioSource::fromMemory(peer)) && !core.residentHoldState().supported &&
        !core.residentHoldControl(0, AudioResidentHoldCommand::Enter, range.id, oldRevision),
        "Source replacement reused the previous owner's resident authorization");
}

void residentMetadataAndStagedFailuresRejectSafely()
{
    // A plausible optional hook is not authority to move the parked cursor by
    // a fractional frame or by more than this result actually supplied.
    struct Fault {
        double nominal = 0;
        static conflux::audio::AudioReadResult read(void* context, std::uint64_t, std::span<float> output) noexcept {
            std::fill(output.begin(), output.end(), 99.F);
            conflux::audio::AudioReadResult result;
            result.frames = output.size() / 2; result.positionValid = result.residentHold = true;
            result.nominalPosition = static_cast<Fault*>(context)->nominal;
            return result;
        }
        AudioSource source() {
            AudioSource result{64, nullptr, this, read};
            result.setStemGain = [](void*, std::size_t, float) noexcept { return true; };
            result.stemGain = [](const void*, std::size_t) noexcept { return 1.F; };
            result.residentHoldControl = [](void*, AudioResidentHoldCommand, std::uint64_t, std::uint64_t) noexcept { return false; };
            result.residentHoldState = [](const void*) noexcept {
                conflux::audio::AudioResidentHoldState state;
                state.supported = state.ready = state.armed = state.active = true;
                return state;
            };
            return result;
        }
    };
    std::array<float, 64> peer; peer.fill(.25F);
    for (const double nominal : {.5, 9.0}) {
        Fault fault{nominal};
        RenderCore core(fault.source(), AudioSource::fromMemory(peer));
        std::array<float, 16> output{};
        require(core.render(output) && core.readFailed() && core.sourceFrame() == 0 &&
            core.starvedFrames() == 8 && core.sourceFrame(1) == 8 && core.frameClock() == 8 &&
            std::ranges::all_of(output, [](float value) { return value == .25F; }),
            "Malformed resident position escaped validation or leaked PCM/changed the peer");
    }
    struct Broken {
        static conflux::audio::AudioReadResult read(void*, std::uint64_t, std::span<float>) noexcept { return {0, true}; }
    };
    std::array<AudioSource, 4> broken;
    broken.fill(AudioSource{8, nullptr, nullptr, Broken::read});
    conflux::audio::StemBankSource bank(broken);
    const auto source = bank.sourceRef();
    std::array<float, 16> output; output.fill(77.F);
    require(source.read(source.context, 0, output).failed && bank.hasReadFailure(),
            "Staged-failure control did not fail its linear bank");
    std::array<std::array<float, 16>, 4> pcm{};
    std::array<std::span<const float>, 4> leaves{};
    for (std::size_t stem = 0; stem < 4; ++stem) {
        pcm[stem].fill(.125F); leaves[stem] = pcm[stem];
    }
    require(bank.readCached(leaves, 0, output).failed && bank.hasReadFailure() &&
        std::ranges::all_of(output, [](float value) { return value == 77.F; }),
        "Resident mixing cleared or bypassed malformed staged-provider health");
}
} // namespace

int main()
{
    try {
        deterministicPolicyAndCommit();
        actualRenderActivityEdges();
        realOwnerReserveRecoveryAndPeer();
        residentRangeValidationAndEpochCapture();
        residentPreparedTakeoverAtExactOutPreservesMarkerRampsAndFailure();
        residentPreparedTakeoverRejectsNewerManualIntent();
        residentLoopPcmRampsReserveExitAndRecording();
        residentTerminalFailureKeepsEverySubblockAudible();
        residentNearEofFailureDoesNotTruncateCallbackOrLoopHealthyEnd();
        residentPendingCaptureAndManualIntent();
        residentMetadataAndStagedFailuresRejectSafely();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

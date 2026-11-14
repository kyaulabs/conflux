// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/PreparedScratchSource.hpp"
#include "audio/PreparedTempoSource.hpp"
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace conflux::audio;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
bool near(float actual, double expected, double tolerance = 2e-6)
{ return std::isfinite(actual) && std::abs(double(actual) - expected) <= tolerance; }
struct CloseFile { void operator()(std::FILE* file) const { if (file) std::fclose(file); } };
using File = std::unique_ptr<std::FILE, CloseFile>;
File rawFile(std::span<const float> pcm)
{
    File file(std::tmpfile());
    require(file && std::fwrite(pcm.data(), sizeof(float), pcm.size(), file.get()) == pcm.size() &&
        std::fflush(file.get()) == 0, "Cannot write generated scratch playback fixture");
    return file;
}
// Independent original-track oracle: asymmetric, distinct per leaf/channel,
// affine away from EOF so fractional reconstruction has a known value without
// copying the interpolator's table or comparing against its own output.
double signal(double frame, std::size_t leaf, std::size_t channel)
{
    return channel ? -.0625 * double(leaf + 2) - frame * .000005 * double(leaf + 1)
                   : .125 * double(leaf + 1) + frame * .00001 * double(leaf + 1);
}
struct Fixture {
    std::uint64_t frames;
    std::array<std::vector<float>, 4> pcm;
    std::array<File, 4> files;
    std::array<struct stat, 4> identities{};
    explicit Fixture(std::uint64_t count = 32768) : frames(count)
    {
        for (std::size_t leaf = 0; leaf < 4; ++leaf) {
            pcm[leaf].resize(frames * 2);
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    pcm[leaf][frame * 2 + channel] = float(signal(double(frame), leaf, channel));
            files[leaf] = rawFile(pcm[leaf]);
            require(fstat(fileno(files[leaf].get()), &identities[leaf]) == 0, "Cannot identify generated PCM");
        }
    }
    std::unique_ptr<PreparedPlayback> owner(bool bank) const
    {
        if (bank) return std::make_unique<PreparedPlayback>(std::array<int, 4>{
            fileno(files[0].get()), fileno(files[1].get()), fileno(files[2].get()), fileno(files[3].get())}, frames);
        return std::make_unique<PreparedPlayback>(fileno(files[0].get()), frames);
    }
    float mixed(std::uint64_t frame, std::size_t channel, const std::array<double, 4>& gains) const
    {
        double sum = 0;
        for (std::size_t leaf = 0; leaf < 4; ++leaf) sum += double(pcm[leaf][frame * 2 + channel]) * gains[leaf];
        return float(sum);
    }
};
std::vector<float> peerSignal(std::size_t frames = 65536)
{
    std::vector<float> pcm(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        pcm[frame * 2] = float(int(frame % 31) - 15) / 32.F;
        pcm[frame * 2 + 1] = float(int(frame % 29) - 14) / 64.F;
    }
    return pcm;
}
struct Buses { std::vector<float> master, cue; };
Buses render(RenderCore& core, std::size_t frames)
{
    Buses result{std::vector<float>(frames * 2), std::vector<float>(frames * 2)};
    require(core.renderBuses(result.master, result.cue), "Actual renderer rejected complete stereo output");
    return result;
}
void readyWhileHeld(RenderCore& core)
{
    const auto deadline = Clock::now() + 2s;
    const auto position = core.sourcePosition();
    while (core.scratchState(0).pending) {
        require(Clock::now() < deadline && !core.scratchState(0).errorCode, "Optional scratch worker did not settle");
        render(core, 1); // Real held read adopts publication, status itself is pure.
        require(core.sourcePosition() == position && !core.readFailed(0), "Prefetch/held silence advanced or failed source");
        std::this_thread::sleep_for(1ms);
    }
}
void warm(RenderCore& core, double target, double stepBound = 1)
{
    require(std::isfinite(target) && std::isfinite(stepBound) && stepBound > 0 && stepBound <= PreparedScratchSource::maximumSpeed,
            "Invalid fixture warm target or motion bound");
    const auto deadline = Clock::now() + 2s;
    for (std::size_t count = 0; core.sourcePosition() != target; ++count) {
        require(count < 2000 && Clock::now() < deadline, "Prepared scratch did not reach finite nearby target");
        const auto before = core.sourcePosition();
        require(std::isfinite(before), "Prepared scratch warm started from a nonfinite cursor");
        const auto remaining = std::abs(target - before);
        // Simulate bounded callback blocks without reading held surplus after
        // the target; this preserves the next full-envelope PCM assertion.
        const auto batch = static_cast<std::size_t>(std::clamp(std::ceil(remaining / stepBound),
            1., double(PreparedScratchSource::maxReadFrames)));
        render(core, batch);
        const auto after = core.sourcePosition();
        const auto moved = (target > before ? 1 : -1) * (after - before);
        require(!core.readFailed(0) && !core.isBuffering(0) && !core.scratchState(0).errorCode &&
            std::isfinite(after) && moved > 0 && moved <= remaining && moved <= stepBound * double(batch),
            "Prepared scratch warm stopped, lacked healthy PCM, reversed, overshot or exceeded its bounded source advance");
    }
}
// A normal refill may be unavailable: retain source position and let the peer
// and output clock continue. Return ONLY the first actually consumed sample.
Buses firstLinearFrame(RenderCore& core)
{
    const auto deadline = Clock::now() + 2s;
    for (;;) {
        const auto before = core.sourceFrame();
        const auto clock = core.frameClock(), peer = core.sourceFrame(1);
        auto output = render(core, 1);
        require(core.frameClock() == clock + 1 && core.sourceFrame(1) == peer + 1 && !core.readFailed(0),
                "Ordinary refill stopped peer/output clock or reported source failure");
        if (core.sourceFrame() == before + 1) return output;
        require(core.sourceFrame() == before && core.isBuffering(0) && Clock::now() < deadline,
                "Unavailable ordinary data advanced source or failed to settle");
        std::this_thread::sleep_for(1ms);
    }
}

void normalPlatterBusAndRelease()
{
    for (const bool playing : {false, true}) {
        Fixture fixture; auto owner = fixture.owner(false);
        require(owner->prime(10000, 2s), "Cannot prime real normal playback");
        const auto peer = peerSignal();
        RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
        require(core.restorePrimedCursor(0, 10000) && core.setPlaying(0, playing) &&
            core.setDeckGain(0, .5F) && core.setDeckGain(1, .25F) && core.setMasterGain(.75F) &&
            core.setCrossfader(.75F) && core.setCueSelected(0, true) && core.setCueGain(.5F) &&
            core.beginScratch(0, 1), "Cannot enter real prepared scratch mode");
        readyWhileHeld(core);
        const auto oldClock = core.frameClock(), oldPeer = core.sourceFrame(1);
        const auto held = render(core, 17);
        for (std::size_t frame = 0; frame < 17; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(near(held.master[frame * 2 + channel], double(peer[(oldPeer + frame) * 2 + channel]) * .25 * .75) &&
                    held.cue[frame * 2 + channel] == 0.F, "Held platter emitted DC or changed peer/master/cue routing");
        require(core.sourcePosition() == 10000 && core.sourceFrame() == 10000 && core.isPlaying() == playing &&
            core.frameClock() == oldClock + 17 && core.sourceFrame(1) == oldPeer + 17 &&
            !core.isBuffering(0) && !core.starvedFrames(0) && !owner->errorCode(), "Held platter changed clocks/play request/health");
        require(core.moveScratch(0, 1, 256, 256.0 / 48000), "Cannot warm actual forward platter");
        warm(core, 10256); // More than240 moving output frames: motion envelope is fully open.
        require(core.moveScratch(0, 1, 736, .01), "Cannot set steady unit platter speed");
        const auto beforePeer = core.sourceFrame(1);
        const auto forward = render(core, 480);
        for (std::size_t frame = 0; frame < 480; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const double raw = fixture.pcm[0][(10256 + frame) * 2 + channel];
                require(near(forward.master[frame * 2 + channel],
                    (raw * .5 * .5 + double(peer[(beforePeer + frame) * 2 + channel]) * .25) * .75) &&
                    near(forward.cue[frame * 2 + channel], raw * .5), "Forward scratch did not use actual PCM/prefader cue/gain/crossfader");
            }
        require(core.sourcePosition() == 10736 && core.sourceFrame() == 10736 && core.isPlaying() == playing,
                "Consumed forward scratch lost nominal/play-state contract");
        require(core.moveScratch(0, 1, 1216, .01), "Cannot create active forward backlog"); render(core, 1);
        require(core.moveScratch(0, 1, 976, .01), "Cannot reverse an already active platter");
        const auto turn = core.sourcePosition();
        const auto settleClock = core.frameClock(), settlePeer = core.sourceFrame(1);
        const auto settling = render(core, 272);
        for (const auto& bus : {settling.master, settling.cue})
            require(std::ranges::all_of(bus, [](float sample) { return std::isfinite(sample); }),
                    "Reversal settling block produced nonfinite output");
        require(core.frameClock() == settleClock + 272 && core.sourceFrame(1) == settlePeer + 272 &&
            core.sourcePosition() < turn && core.sourcePosition() >= turn - 240 && !core.isBuffering(0) &&
            !core.readFailed(0) && !core.scratchState(0).errorCode && !owner->errorCode(),
            "Reversal settling block lost clocks, passed its target or lacked healthy backward motion");
        // Continue the same half-rate negative gesture before the per-sample
        // oracle; instrumented assertions are not new physical motion input.
        require(core.moveScratch(0, 1, 960, 32.0 / 48000), "Cannot continue settled reverse platter motion");
        std::size_t compared = 0;
        for (std::size_t frame = 272; frame < 300; ++frame) {
            const auto before = core.sourcePosition();
            const auto peerFrame = core.sourceFrame(1);
            const auto output = render(core, 1); const auto after = core.sourcePosition();
            const double samplePosition = after < before ? after : before;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                require(std::isfinite(output.master[channel]) && std::isfinite(output.cue[channel]), "Reversal envelope produced nonfinite output");
                // Braking and the240-frame shared attack are intentional. Once
                // settled, compare actual source geometry rather than copying
                // the envelope/interpolator implementation into this oracle.
                if (frame >= 272 && after < before) {
                    const double raw = signal(samplePosition, 0, channel);
                    require(near(output.master[channel], (raw * .5 * .5 + double(peer[peerFrame * 2 + channel]) * .25) * .75, 1e-5) &&
                        near(output.cue[channel], raw * .5, 1e-5), "Reversing scratch was not aligned actual track-derived stereo PCM");
                    ++compared;
                }
            }
        }
        require(compared >= 32 && core.sourcePosition() < turn && !core.readFailed(0) && !owner->errorCode(),
                "Active scratch did not reverse/settle through real renderer");
        const auto released = core.sourcePosition();
        require(core.endScratch(0, 1) && !core.scratchState(0).active && core.isPlaying() == playing &&
            core.sourceFrame() == std::uint64_t(released) && core.sourcePosition() == released,
            "Release changed play request or used target instead of actual consumed scratch cursor");
        require(!core.moveScratch(0, 1, 1000, .01), "Late move revived a released gesture");
        if (!playing) {
            const auto position = core.sourcePosition(); render(core, 32);
            require(core.sourcePosition() == position && !core.isPlaying(), "Paused scratch release started playback");
        } else {
            const auto firstFrame = core.sourceFrame();
            auto output = firstLinearFrame(core);
            const auto deliveredPeer = core.sourceFrame(1) - 1;
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(near(output.master[channel], (double(fixture.pcm[0][firstFrame * 2 + channel]) * .25 +
                    double(peer[deliveredPeer * 2 + channel]) * .25) * .75), "Release resumed wrong ordinary PCM frame");
            require(core.endScratch(0, 1), "Duplicate matching release was not idempotent");
            const auto nextFrame = core.sourceFrame(); render(core, 1);
            require(core.sourceFrame() == nextFrame + 1 && !core.isBuffering(0), "Duplicate release restarted reader/rewound source");
        }
    }
}

void sharedStemGainsAndLinearContinuation()
{
    Fixture fixture; auto owner = fixture.owner(true);
    require(owner->prime(1000, 2s), "Cannot prime real four-stem owner");
    const auto source = owner->sourceRef();
    constexpr std::array<double, 4> initial{1, .5, .25, 0}, targets{0, .75, .5, 1};
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(source.setStemGain(source.context, leaf, float(initial[leaf])), "Cannot set initial bank targets");
    require(owner->configureStemRamps(4), "Cannot configure real shared stem ramps");
    const auto peer = peerSignal();
    RenderCore core(source, AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, 1000) && core.setDeckGain(0, .5F) && core.setMasterGain(.75F) &&
        core.setCrossfader(.75F) && core.setDeckGain(1, 0) && core.setCueSelected(0, true) && core.setCueGain(.5F),
        "Cannot configure bank integration buses");
    render(core, 3); require(core.sourceFrame() == 1003, "Linear stem prefix did not advance before scratch");
    require(core.setPlaying(0, false) && core.beginScratch(0, 10), "Cannot pause/enter bank scratch"); readyWhileHeld(core);
    // Refresh genuine cumulative unit motion after startup, then fully open
    // the motion gate without reading a held surplus at the oracle cursor.
    require(core.moveScratch(0, 10, 64, 64.0 / 48000), "Cannot start bank platter warm motion"); warm(core, 1067);
    require(core.moveScratch(0, 10, 256, 192.0 / 48000), "Cannot continue bank platter warm motion"); warm(core, 1259);
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(core.setStemGain(0, leaf, float(targets[leaf])), "Scratch mode rejected render-side stem targets");
    require(core.moveScratch(0, 10, 736, .01), "Cannot continue bank unit motion");
    for (std::size_t frame = 0; frame < 4; ++frame) {
        const auto output = render(core, 1); std::array<double, 4> gains{};
        for (std::size_t leaf = 0; leaf < 4; ++leaf) gains[leaf] = initial[leaf] + (targets[leaf] - initial[leaf]) * double(frame + 1) / 4;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto raw = fixture.mixed(1259 + frame, channel, gains);
            require(near(output.master[channel], double(raw) * .5 * .5 * .75) && near(output.cue[channel], double(raw) * .5),
                    "Cached scratch mixing lost shared stem ramps, stereo roles or prefader cue");
        }
    }
    require(core.sourceFrame() == 1263 && !core.readFailed(0) && core.endScratch(0, 10) && !core.isPlaying(),
            "Bank scratch cursor/paused release failed");
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(core.stemGain(0, leaf) == targets[leaf], "Scratch end lost current stem gain target");
    require(core.setPlaying(0, true), "Cannot resume ordinary bank after scratch");
    const auto output = firstLinearFrame(core);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto raw = fixture.mixed(1263, channel, targets);
        require(near(output.master[channel], double(raw) * .5 * .5 * .75) && near(output.cue[channel], double(raw) * .5),
                "Bank exit replayed parked old surplus or reset shared gain ramps");
    }
    require(core.sourceFrame() == 1264 && !owner->errorCode(), "Ordinary bank continuation did not commit exact first frame");
}

void explicitCancellationAndTokens()
{
    Fixture fixture; auto owner = fixture.owner(false); require(owner->prime(10000, 2s), "Cannot prime cancellation fixture");
    const auto peer = peerSignal(); RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, 10000) && core.setPlaying(0, false) && core.beginScratch(0, 20), "Cannot enter paused scratch");
    readyWhileHeld(core);
    require(core.moveScratch(0, 20, 17, 17.0 / 48000), "Cannot move cancellation fixture"); warm(core, 10017);
    const auto generation = core.scratchState(0).generation, clock = core.frameClock(), peerFrame = core.sourceFrame(1);
    require(core.seek(0, core.sourceFrame()) && !core.scratchState(0).active && core.scratchState(0).generation > generation &&
        core.sourcePosition() == 10017 && core.frameClock() == clock && core.sourceFrame(1) == peerFrame &&
        !core.moveScratch(0, 20, 480, .01) && !core.beginScratch(0, 20), "Same-frame seek retained stale scratch motion/token");
    require(core.beginScratch(0, 21), "Fresh gesture after explicit seek rejected"); readyWhileHeld(core);
    const auto retryGeneration = core.scratchState(0).generation;
    require(core.retrySource(0) && !core.scratchState(0).active && core.scratchState(0).generation > retryGeneration &&
        core.sourcePosition() == 10017 && !core.moveScratch(0, 21, -480, .01), "Retry did not cancel the same-frame scratch generation");
    require(core.beginScratch(0, 22) && !core.isDeckSilentForReplacement(0) &&
        !core.replaceSource(0, AudioSource::fromMemory(peer)) && !core.endScratch(0, 21) &&
        !core.moveScratch(0, 22, 10, 0) && !core.moveScratch(0, 22, 10, std::numeric_limits<double>::quiet_NaN()) &&
        !core.beginScratch(2, 1) && !core.beginScratch(0, 0), "Active scratch lifetime/token/invalid-control guard failed");
    require(core.setPlaying(0, false) && !core.scratchState(0).active && !core.isPlaying(), "Explicit pause did not cancel held scratch");
}

void processedTempoScratchReprimesAtConsumedCursor()
{
    Fixture fixture(96000); auto owner = fixture.owner(false);
    constexpr std::uint64_t initialFrame = 24000;
    require(owner->prime(initialFrame, 2s), "Cannot prime processed scratch fixture");
    const auto peer = peerSignal(); RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, initialFrame) && core.setDeckGain(1, 0) && core.setCueSelected(0, true) &&
        core.setTempoPercent(0, 6) && core.toggleMasterTempo(0) && owner->prime(initialFrame, 2s),
        "Cannot prepare actual +6 percent Master Tempo playback");
    // Collect only delivered frames, preserving real initial/refill starvation.
    // Muted peer data must still advance with every output callback.
    auto processedFrames = [&](std::size_t count) {
        std::vector<float> delivered; delivered.reserve(count * 2);
        const auto deadline = Clock::now() + 2s;
        while (delivered.size() < count * 2) {
            require(Clock::now() < deadline, "Processed scratch handoff did not deliver its bounded prefix");
            const auto before = core.sourcePosition();
            const auto clock = core.frameClock(), peerFrame = core.sourceFrame(1);
            const auto output = render(core, 1);
            require(core.frameClock() == clock + 1 && core.sourceFrame(1) == peerFrame + 1 &&
                !core.readFailed(0) && !owner->errorCode() && !core.tempoState(0).finished,
                "Processed scratch handoff failed or stopped peer/output clocks");
            if (core.sourcePosition() == before) {
                require(core.isBuffering(0) && output.master[0] == 0.F && output.master[1] == 0.F &&
                    output.cue[0] == 0.F && output.cue[1] == 0.F,
                    "Unavailable processed data advanced source or leaked stale PCM");
                std::this_thread::sleep_for(1ms);
                continue;
            }
            require(std::abs(core.sourcePosition() - before - 1.06) < 1e-8 &&
                output.master == output.cue, "Processed frame lost selected speed or stereo bus routing");
            delivered.insert(delivered.end(), output.cue.begin(), output.cue.end());
        }
        return delivered;
    };
    (void)processedFrames(257); // Consume real R3 output; leave its continuation queued/prefetched.
    const auto selected = core.tempoState(0);
    require(selected.active && selected.percent == 6 && selected.keyLock && selected.appliedPercent == 6,
            "Processed scratch setup did not consume the selected native configuration");
    const auto capturedFrame = core.sourceFrame();
    require(core.beginScratch(0, 30), "Selected nonunity Master Tempo incorrectly rejects raw scratching");
    readyWhileHeld(core);
    require(core.scratchState(0).active && core.sourcePosition() == double(capturedFrame) && core.isPlaying() &&
        core.tempoState(0).generation == selected.generation && core.tempoState(0).percent == 6 &&
        core.tempoState(0).keyLock && !core.setTempoPercent(0, 5) && !core.toggleMasterTempo(0),
        "Raw takeover lost the selected tempo, changed play request or permitted a held-gesture mode edit");
    require(core.moveScratch(0, 30, 256, 256.0 / 48000), "Cannot warm processed-deck raw scratch");
    warm(core, double(capturedFrame + 256));
    const auto turn = core.sourcePosition();
    require(core.moveScratch(0, 30, -224, 480.0 / (48000 * .75)), "Cannot reverse processed-deck raw scratch");
    bool fractionalReverse = false;
    for (std::size_t frame = 0; frame < 304; ++frame) {
        const auto before = core.sourcePosition(); const auto output = render(core, 1);
        const auto after = core.sourcePosition();
        require(!core.readFailed(0) && !owner->errorCode() && core.isPlaying(), "Raw takeover failed or stopped playback");
        if (frame >= 300 && after < before && after != std::floor(after)) {
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(near(output.cue[channel], signal(after, 0, channel), 1e-5) &&
                    near(output.master[channel], signal(after, 0, channel), 1e-5),
                    "Processed deck scratch was not original track-derived reverse stereo PCM");
            fractionalReverse = true;
            break;
        }
    }
    const auto released = core.sourcePosition();
    const auto restart = static_cast<std::uint64_t>(std::floor(released));
    require(fractionalReverse && released < turn && released != double(restart),
            "Generated gesture did not exercise a consumed noninteger reverse release");
    require(core.endScratch(0, 30) && !core.scratchState(0).active && core.isPlaying() &&
        core.sourceFrame() == restart && core.sourcePosition() == double(restart),
        "Processed release failed to adopt the same integer source coordinate as its new generation");
    const auto resumedState = core.tempoState(0);
    require(resumedState.active && resumedState.pending && !resumedState.finished && !resumedState.errorCode &&
        resumedState.percent == 6 && resumedState.keyLock && resumedState.generation > selected.generation &&
        !core.moveScratch(0, 30, 1000, .01),
        "Processed release reused a queued generation, lost its selection or revived a released gesture");
    constexpr std::size_t prefixFrames = 2049;
    const auto resumed = processedFrames(prefixFrames);
    // Independent fresh producer at the consumed release coordinate is a
    // discontinuity oracle, not a DSP quality/pitch oracle. Never compare with
    // the paused old tempo queue or nominally crop its output.
    const std::array<int, 1> fd{fileno(fixture.files[0].get())};
    PreparedTempoSource reference(fd, fixture.frames);
    require(reference.request(restart, 6, true), "Cannot start fresh release-coordinate reference");
    std::vector<float> expected; expected.reserve(resumed.size());
    const auto deadline = Clock::now() + 2s;
    while (expected.size() < resumed.size()) {
        require(Clock::now() < deadline && !reference.errorCode(), "Fresh release-coordinate reference did not settle");
        const auto view = reference.peek();
        if (!view) { std::this_thread::sleep_for(1ms); continue; }
        require(!view.terminal && !view.errorCode && view.leafCount == 1,
                "Fresh release-coordinate reference ended before the bounded prefix");
        const auto count = std::min(view.frames, (resumed.size() - expected.size()) / 2);
        expected.insert(expected.end(), view.leaves[0].begin(), view.leaves[0].begin() + count * 2);
        require(reference.consume(count), "Cannot consume fresh release-coordinate reference");
    }
    for (std::size_t sample = 0; sample < resumed.size(); ++sample)
        require(near(resumed[sample], expected[sample]), "Scratch release exposed queued pre-scratch tempo PCM");
    require(core.tempoState(0).generation == resumedState.generation && core.tempoState(0).appliedPercent == 6 &&
        !core.readFailed(0) && !owner->errorCode(), "Fresh processed continuation changed mode or failed after delivery");
}

// Real stereo/four-stem rate and pitch-mode handoffs, paused and playing.
void processedScratchModeMatrix()
{
    for (const bool bank : {false, true})
        for (const double percent : {-6.0, 6.0})
            for (const bool keyLock : {false, true}) {
                Fixture fixture(96000);
                auto owner = fixture.owner(bank);
                const auto source = owner->sourceRef();
                if (bank) {
                    constexpr std::array<float, 4> initial{1, .25F, .5F, 0};
                    for (std::size_t leaf = 0; leaf < 4; ++leaf)
                        require(source.setStemGain(source.context, leaf, initial[leaf]), "Cannot initialize processed bank gains");
                    require(owner->configureStemRamps(8), "Cannot configure processed bank gain ramps");
                }
                constexpr std::uint64_t initial = 24000;
                require(owner->prime(initial, 2s), "Cannot prime processed mode matrix source");
                const auto peer = peerSignal();
                RenderCore core(source, AudioSource::fromMemory(peer));
                require(core.restorePrimedCursor(0, initial) && core.setDeckGain(0, .5F) && core.setDeckGain(1, 0) &&
                    core.setMasterGain(.5F) && core.setCueSelected(0, true) && core.setCueGain(.5F) &&
                    core.setTempoPercent(0, percent) && (!keyLock || core.toggleMasterTempo(0)) && owner->prime(initial, 2s),
                    "Cannot select actual processed matrix configuration");
                const double step = 1 + percent / 100;
                auto delivered = [&](std::size_t wanted) {
                    const auto deadline = Clock::now() + 2s;
                    for (std::size_t count = 0; count < wanted;) {
                        require(Clock::now() < deadline, "Processed matrix prefix did not settle");
                        const auto before = core.sourcePosition();
                        const auto clock = core.frameClock(), peerFrame = core.sourceFrame(1);
                        const auto out = render(core, 1);
                        require(core.frameClock() == clock + 1 && core.sourceFrame(1) == peerFrame + 1 &&
                            core.isPlaying() && !core.readFailed(0) && !owner->errorCode() && !core.tempoState(0).finished,
                            "Processed matrix failed health/play/peer clocks");
                        if (core.sourcePosition() == before) {
                            require(core.isBuffering(0) && out.master[0] == 0.F && out.master[1] == 0.F &&
                                out.cue[0] == 0.F && out.cue[1] == 0.F, "Processed starvation leaked stale PCM");
                            std::this_thread::sleep_for(1ms);
                            continue;
                        }
                        require(std::abs(core.sourcePosition() - before - step) < 1e-8,
                                "Consumed matrix frame lost selected nominal speed");
                        for (std::size_t channel = 0; channel < 2; ++channel)
                            require(std::isfinite(out.cue[channel]) && near(out.master[channel], double(out.cue[channel]) * .5),
                                    "Processed matrix changed stereo bus gains");
                        ++count;
                    }
                };
                std::uint64_t token = 100;
                for (const bool playing : {false, true}) {
                    require(core.setPlaying(0, true), "Cannot deliver selected mode before scratch");
                    delivered(64); // Positive consumed backend PCM, not configuration/prefetch alone.
                    const auto selected = core.tempoState(0);
                    require(selected.active && selected.percent == percent && selected.appliedPercent == percent &&
                        selected.keyLock == keyLock, "Matrix setup did not consume selected backend configuration");
                    const auto first = core.sourceFrame();
                    require(core.setPlaying(0, playing) && core.beginScratch(0, ++token), "Selected processed mode rejected raw takeover");
                    readyWhileHeld(core);
                    const std::array<double, 4> gains = playing ? std::array<double, 4>{0, .75, .25, 1}
                                                               : std::array<double, 4>{.25, 0, .5, .75};
                    if (bank)
                        for (std::size_t leaf = 0; leaf < 4; ++leaf)
                            require(core.setStemGain(0, leaf, float(gains[leaf])), "Held processed deck rejected stem gains");
                    const auto clock = core.frameClock(), peerFrame = core.sourceFrame(1);
                    const auto held = render(core, 17); // Also settles the eight-frame bank gain ramp on raw zero input.
                    require(core.sourcePosition() == double(first) && core.isPlaying() == playing &&
                        core.frameClock() == clock + 17 && core.sourceFrame(1) == peerFrame + 17 &&
                        core.tempoState(0).generation == selected.generation && !core.readFailed(0),
                        "Held raw takeover consumed processed queue or changed play request");
                    for (const auto& bus : {held.master, held.cue})
                        require(std::ranges::all_of(bus, [](float value) { return value == 0.F; }), "Fresh held platter was not silent");
                    require(core.moveScratch(0, token, -1024, 1024.0 / 48000), "Cannot start matrix reverse motion");
                    render(core, 512); // One bounded block opens the shared motion envelope; no exact startup-speed oracle.
                    const auto before = core.sourcePosition();
                    require(before < double(first) && double(first) - before <= 512 &&
                        core.moveScratch(0, token, -1536, 512.0 / 48000), "Matrix motion jumped or failed reverse direction");
                    const auto raw = render(core, 16);
                    for (std::size_t frame = 0; frame < 16; ++frame)
                        for (std::size_t channel = 0; channel < 2; ++channel) {
                            double expected = 0;
                            for (std::size_t leaf = 0; leaf < (bank ? 4U : 1U); ++leaf)
                                expected += signal(before - double(frame + 1), leaf, channel) * (bank ? gains[leaf] : 1);
                            require(near(raw.master[frame * 2 + channel], expected * .25, 1e-5) &&
                                near(raw.cue[frame * 2 + channel], expected * .5, 1e-5),
                                "Processed matrix raw reverse lost original stereo/leaf/gain geometry");
                        }
                    require(core.sourcePosition() == before - 16 && core.isPlaying() == playing && !core.readFailed(0),
                            "Matrix consumed scratch cursor/play request differs");
                    const auto restart = static_cast<std::uint64_t>(std::floor(core.sourcePosition()));
                    require(core.endScratch(0, token) && !core.scratchState(0).active && core.isPlaying() == playing &&
                        core.sourcePosition() == double(restart), "Processed matrix release lost consumed integer coordinate");
                    const auto resumed = core.tempoState(0);
                    require(resumed.active && resumed.pending && resumed.generation > selected.generation &&
                        resumed.percent == percent && resumed.keyLock == keyLock && !resumed.errorCode && !resumed.finished &&
                        !core.moveScratch(0, token, -2048, .01), "Matrix release lost selection/new generation/token lifetime");
                    if (!playing) {
                        const auto pausedClock = core.frameClock(), pausedPeer = core.sourceFrame(1);
                        const auto silent = render(core, 19);
                        require(core.sourcePosition() == double(restart) && !core.isPlaying() &&
                            core.frameClock() == pausedClock + 19 && core.sourceFrame(1) == pausedPeer + 19 &&
                            std::ranges::all_of(silent.master, [](float value) { return value == 0.F; }) &&
                            std::ranges::all_of(silent.cue, [](float value) { return value == 0.F; }),
                            "Paused processed release consumed prefetch, emitted PCM or stopped peer/output clocks");
                    }
                    require(core.setPlaying(0, true), "Cannot resume matrix selected processing");
                    delivered(16); // Actual first current-generation output; full2049-frame oracle is a separate case.
                    require(core.tempoState(0).generation == resumed.generation && core.tempoState(0).appliedPercent == percent &&
                        core.deckGain(0) == .5F && core.masterGain() == .5F && core.crossfader() == .5F && core.cueGain() == .5F,
                        "Matrix resume changed generation, processing or bus gains");
                    if (bank)
                        for (std::size_t leaf = 0; leaf < 4; ++leaf)
                            require(core.stemGain(0, leaf) == gains[leaf], "Processed restart lost render-side stem gain target");
                }
            }
}

void loopTempoGatesAndReverseFromEof()
{
    Fixture fixture(8192); auto owner = fixture.owner(false); require(owner->prime(1000, 2s), "Cannot prime interaction fixture");
    const auto peer = peerSignal(); RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, 1000) && core.loopIn(0), "Cannot set real prepared loop IN");
    const auto loop = core.loopState(0);
    require(!core.beginScratch(0, 1) && core.loopState(0).inSet && core.loopState(0).inFrame == loop.inFrame &&
        core.sourcePosition() == 1000, "Scratch rejection destroyed existing loop selection");
    require(core.seek(0, 1000) && core.beginScratch(0, 1), "Cannot scratch after clearing loop explicitly"); readyWhileHeld(core);
    require(!core.loopIn(0) && !core.loopOut(0) && !core.loopToggle(0) && !core.setTempoPercent(0, 2) &&
        !core.toggleMasterTempo(0) && core.scratchState(0).active && core.endScratch(0, 1), "Scratch failed reciprocal loop/tempo exclusion");
    require(core.setTempoPercent(0, 2), "Cannot start real prepared tempo for raw takeover");
    const auto tempo = core.tempoState(0);
    require(tempo.active && core.beginScratch(0, 2) && core.tempoState(0).active && core.tempoState(0).percent == 2 &&
        core.sourcePosition() == 1000, "Raw scratch takeover reset active tempo/configuration");
    readyWhileHeld(core);
    require(core.endScratch(0, 2) && core.tempoState(0).active && core.tempoState(0).percent == 2 &&
        core.tempoState(0).generation > tempo.generation, "Raw scratch release failed to re-prime the selected tempo");
    require(core.resetTempo(0) && core.seek(0, fixture.frames) && core.setPlaying(0, false) && core.beginScratch(0, 3),
            "Cannot hold exact EOF after explicit tempo reset"); readyWhileHeld(core);
    render(core, 7);
    require(core.sourcePosition() == double(fixture.frames) && !core.isPlaying() && !core.readFailed(0), "Held EOF became finished or failed");
    require(core.moveScratch(0, 3, -256, 256.0 / 48000), "Cannot move backward from EOF"); warm(core, double(fixture.frames - 256));
    require(core.moveScratch(0, 3, -736, .01) && core.setDeckGain(1, 0), "Cannot continue exact reverse EOF motion");
    const auto reversed = render(core, 480);
    for (std::size_t frame = 0; frame < 480; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(near(reversed.master[frame * 2 + channel], fixture.pcm[0][(fixture.frames - 257 - frame) * 2 + channel]),
                    "Paused EOF reverse did not render exact preceding original stereo samples");
    require(core.sourceFrame() == fixture.frames - 736 && !core.isPlaying() && !core.readFailed(0), "EOF reverse clocks/direction failed");
}

struct GateState {
    struct stat identity{}; std::uint64_t offset = 0;
    std::mutex mutex; std::condition_variable changed;
    bool claimed = false, entered = false, released = false;
};
std::mutex gateMutex;
std::shared_ptr<GateState> activeGate;
class Gate {
public:
    Gate(int fd, std::uint64_t offset) : state_(std::make_shared<GateState>())
    {
        require(fstat(fd, &state_->identity) == 0, "Cannot identify blocked scratch source"); state_->offset = offset;
        const std::lock_guard lock(gateMutex); require(!activeGate, "Nested scratch read gate"); activeGate = state_;
    }
    ~Gate() { release(); }
    void entered()
    {
        std::unique_lock lock(state_->mutex);
        require(state_->changed.wait_for(lock, 2s, [&] { return state_->entered; }), "Real scratch worker did not reach gate");
    }
    bool isEntered() const
    {
        const std::lock_guard lock(state_->mutex); return state_->entered;
    }
    void release()
    {
        { const std::lock_guard lock(state_->mutex); state_->released = true; } state_->changed.notify_all();
        const std::lock_guard lock(gateMutex); if (activeGate == state_) activeGate.reset();
    }
private: std::shared_ptr<GateState> state_;
};
void holdRead(int fd, std::uint64_t offset)
{
    std::shared_ptr<GateState> state;
    { const std::lock_guard lock(gateMutex); state = activeGate; }
    if (!state || offset != state->offset) return;
    struct stat actual{};
    if (fstat(fd, &actual) || actual.st_dev != state->identity.st_dev || actual.st_ino != state->identity.st_ino) return;
    std::unique_lock lock(state->mutex);
    if (state->claimed) return;
    state->claimed = state->entered = true; state->changed.notify_all();
    state->changed.wait(lock, [&] { return state->released; });
}
std::set<int> matchingDescriptors(const Fixture& fixture)
{
    std::set<int> result;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const int fd = std::stoi(entry.path().filename().string()); struct stat actual{};
        if (fstat(fd, &actual) == 0 && std::ranges::any_of(fixture.identities, [&](const auto& wanted) {
                return actual.st_dev == wanted.st_dev && actual.st_ino == wanted.st_ino;
            })) result.insert(fd);
    }
    return result;
}
void failedProcessedScratchCanReleaseAndReplace()
{
    for (const bool explicitEnd : {false, true}) {
        constexpr std::uint64_t initial = 24000;
        Fixture fixture(96000);
        const auto originals = matchingDescriptors(fixture);
        auto owner = fixture.owner(false);
        require(owner->prime(initial, 2s), "Cannot prime real error-release playback");
        const auto source = owner->sourceRef();
        const auto peer = peerSignal();
        const auto replacement = peerSignal(4096);
        RenderCore core(source, AudioSource::fromMemory(peer));
        require(core.restorePrimedCursor(0, initial) && core.setDeckGain(1, .25F) &&
            core.setCueSelected(0, true) && core.configureRamps(8),
            "Cannot configure failed-tempo replacement buses");
        // Ordinary prime has completed; scratch reads at0. Stop-first blocks
        // the first tempo input. End blocks later input after playback can
        // fill its startup queue, retaining actual processed PCM at failure.
        // Gate is destroyed before owner on every assertion failure as well.
        const auto blockedFrame = initial + (explicitEnd
            ? 2 * PreparedTempoSource::slotCount * PreparedTempoSource::blockFrames : 0);
        Gate gate(fileno(fixture.files[0].get()), blockedFrame * 8);
        require(core.setTempoPercent(0, 6), "Cannot start blocked tempo generation");
        if (!explicitEnd) gate.entered();
        if (explicitEnd) {
            const auto deliveryDeadline = Clock::now() + 2s;
            std::uint64_t delivered = 0;
            while (!gate.isEntered()) {
                require(Clock::now() < deliveryDeadline,
                    "Processed playback did not reach the later gated input read");
                const auto before = core.sourcePosition();
                const auto clock = core.frameClock(), peerFrame = core.sourceFrame(1);
                const auto out = render(core, 1); // Count only actual consumed SRC frames.
                require(core.frameClock() == clock + 1 && core.sourceFrame(1) == peerFrame + 1 &&
                    !core.readFailed(0) && !core.readFailed(1) && !owner->errorCode(),
                    "Consuming the queued prefix failed or interrupted peer/output clocks");
                for (const auto& bus : {out.master, out.cue})
                    require(std::ranges::all_of(bus, [](float value) { return std::isfinite(value); }),
                        "Queued processed prefix produced nonfinite bus output");
                if (core.sourcePosition() == before) {
                    require(core.isBuffering(0) && out.cue[0] == 0.F && out.cue[1] == 0.F,
                        "Unavailable processed prefix advanced or leaked old source PCM");
                    std::this_thread::sleep_for(1ms);
                } else {
                    require(std::abs(core.sourcePosition() - before - 1.06) < 1e-8,
                        "Queued prefix changed the consumed SRC clock");
                    if (++delivered % 64 == 0) std::this_thread::sleep_for(1ms);
                }
            }
            const auto consumed = core.tempoState(0);
            require(delivered && std::abs(core.sourcePosition() - double(initial) - double(delivered) * 1.06) < 1e-8 &&
                core.sourceFrame() == std::uint64_t(core.sourcePosition()) && consumed.appliedPercent == 6 &&
                consumed.active && !consumed.keyLock && !consumed.pending &&
                !consumed.finished && !consumed.errorCode,
                "Later-read fixture lacks exact consumed SRC frames and a remaining queued prefix");
            // status.pending==false requires peek() to retain a current slot
            // after those consumed frames. The next input read is still gated,
            // so no EOF/error marker or later input can satisfy this proof.
        }
        const auto heldFrame = core.sourceFrame();
        constexpr std::uint64_t token = 501;
        require(core.beginScratch(0, token), "Cannot enter raw takeover before tempo failure");
        readyWhileHeld(core); // Healthy publication must finish BEFORE truncation.
        render(core, 16); // Settle existing bus ramps while the fresh hold is silent.
        require(core.sourcePosition() == double(heldFrame) && core.isPlaying() &&
            core.scratchState(0).active && !core.readFailed(0),
            "Held failure setup consumed source data or changed play state");
        const auto selected = core.tempoState(0);
        require(selected.active && selected.percent == 6 && !selected.errorCode,
            "Blocked tempo setup did not retain its selected current generation");
        require(ftruncate(fileno(fixture.files[0].get()), 0) == 0,
            "Cannot truncate the real blocked tempo input");
        gate.release(); // The already-admitted pread returns0: actual worker EIO.

        const auto peerOnly = [&](std::size_t count) {
            const auto clock = core.frameClock(), peerFrame = core.sourceFrame(1);
            const auto position = core.sourcePosition();
            require(peerFrame + count <= peer.size() / 2, "Error fixture exhausted its independent peer");
            const auto out = render(core, count);
            require(core.frameClock() == clock + count && core.sourceFrame(1) == peerFrame + count &&
                core.sourcePosition() == position && !core.readFailed(1),
                "Failed target moved its cursor or interrupted the healthy peer/output clocks");
            for (std::size_t frame = 0; frame < count; ++frame)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    require(near(out.master[frame * 2 + channel],
                        double(peer[(peerFrame + frame) * 2 + channel]) * .25) &&
                        out.cue[frame * 2 + channel] == 0.F,
                        "Failed tempo exposed queued processed/raw/linear PCM or contaminated peer/cue output");
        };
        const auto deadline = Clock::now() + 2s;
        while (!core.tempoState(0).errorCode) {
            require(Clock::now() < deadline, "Real blocked tempo read did not publish its failure");
            peerOnly(16);
            std::this_thread::sleep_for(1ms);
        }
        const auto failed = core.tempoState(0);
        require(failed.errorCode == EIO && owner->errorCode() == EIO &&
            failed.generation == selected.generation && failed.percent == selected.percent &&
            failed.keyLock == selected.keyLock && failed.active && !failed.finished &&
            core.scratchState(0).active && core.isPlaying(),
            "Fixture did not retain the actual current-generation tempo EIO while held");
        peerOnly(16);
        require(core.readFailed(0), "Failed held source was not reported as a source read failure");
        // Pause-first remains the actual Stop/replacement regression. The
        // second fixture checks explicit End with a retained processed prefix.
        if (explicitEnd) {
            require(core.endScratch(0, token), "Errored processed scratch cannot explicitly release");
            peerOnly(16); // Playing failure must not expose its old queued prefix.
        }
        require(core.setPlaying(0, false), "Errored processed scratch prevents Pause/replacement");
        require(!core.scratchState(0).active && !core.isPlaying() &&
            core.sourcePosition() == double(heldFrame) && !core.moveScratch(0, token, 64, .01),
            "Failed release retained its gesture, moved the cursor or ignored Pause");
        const auto retained = source.tempoState(source.context);
        require(retained.errorCode == failed.errorCode && retained.generation == failed.generation &&
            retained.percent == failed.percent && retained.keyLock == failed.keyLock &&
            retained.active && !retained.finished && owner->errorCode() == EIO,
            "Error release cleared/retried the failed generation or changed its selection");
        peerOnly(16); // Also drains the configured eight-frame transport transition.
        require(core.isDeckSilentForReplacement(0) &&
            core.replaceSource(0, AudioSource::fromMemory(replacement)),
            "Released failed scratch cannot reach silence and replace its source");
        require(!core.isPlaying() && !core.readFailed(0) && core.sourcePosition() == 0 &&
            core.sourceFrames(0) == replacement.size() / 2,
            "Replacement retained failed-source transport or health state");
        peerOnly(16);
        owner->stop(); // Renderer now borrows only the two memory sources.
        require(matchingDescriptors(fixture) == originals,
            "Failed-source replacement left an owned reader/tempo/scratch descriptor alive");
    }
}

void pendingMovementAndQuiescedCleanup()
{
    constexpr std::uint64_t window = PreparedScratchSource::windowFrames;
    Fixture fixture(window * 3); const auto originals = matchingDescriptors(fixture);
    const int fd = fileno(fixture.files[0].get());
    require(lseek(fd, 321, SEEK_SET) == 321, "Cannot set original offset");
    auto owner = fixture.owner(false); require(owner->prime(17, 2s), "Cannot prime real pending fixture");
    for (const int duplicate : matchingDescriptors(fixture))
        if (!originals.contains(duplicate)) require((fcntl(duplicate, F_GETFD) & FD_CLOEXEC) != 0, "Playback worker descriptor lacks CLOEXEC");
    const auto peer = peerSignal(); RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, 17) && core.setPlaying(0, false) && core.beginScratch(0, 1), "Cannot ready first scratch window");
    readyWhileHeld(core); require(core.endScratch(0, 1), "Cannot end initial ready gesture");
    // Only scratch's centered-window first read matches this offset. Ordinary
    // PcmReadAhead starts at far, and loop/tempo workers have no active request.
    Gate gate(fd, (fixture.frames - window) * 8);
    const auto far = fixture.frames - 8192;
    require(core.seek(0, far) && core.beginScratch(0, 2), "Cannot start far pending scratch generation"); gate.entered();
    render(core, 23);
    require(core.sourcePosition() == double(far) && !core.isBuffering(0) && core.scratchState(0).pending &&
        !core.starvedFrames(0) && !core.readFailed(0), "Pending held zero was misreported as movement/starvation/failure");
    require(core.moveScratch(0, 2, 480, .01), "Cannot move while centered window is pending");
    const auto clock = core.frameClock(), peerFrame = core.sourceFrame(1), starved = core.starvedFrames(0);
    const auto unavailable = render(core, 32);
    require(core.sourcePosition() == double(far) && core.frameClock() == clock + 32 && core.sourceFrame(1) == peerFrame + 32 &&
        core.starvedFrames(0) == starved + 32 && core.isBuffering(0) && !core.readFailed(0) && !owner->errorCode(),
        "Unavailable moving PCM advanced source, stopped peer/output, or concealed starvation");
    for (std::size_t frame = 0; frame < 32; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(unavailable.master[frame * 2 + channel] == peer[(peerFrame + frame) * 2 + channel],
                    "Unavailable scratch leaked old-position PCM into master");
    const auto generation = core.scratchState(0).generation;
    require(core.seek(0, far) && !core.scratchState(0).active && core.scratchState(0).generation > generation &&
        !core.moveScratch(0, 2, -480, .01), "Same-frame cancellation retained a blocked old-generation gesture");
    gate.release(); // Always before any possible owner stop/join.
    require(core.setPlaying(0, true), "Cannot resume normal source after pending cancellation");
    const auto firstFrame = core.sourceFrame(); const auto resumed = firstLinearFrame(core);
    const auto deliveredPeer = core.sourceFrame(1) - 1;
    for (std::size_t channel = 0; channel < 2; ++channel)
        require(near(resumed.master[channel], double(fixture.pcm[0][firstFrame * 2 + channel]) +
            peer[deliveredPeer * 2 + channel]), "Cancelled pending scratch contaminated ordinary continuation");
    require(lseek(fd, 0, SEEK_CUR) == 321, "Real worker reads changed original shared FD offset");
    owner->stop(); owner->stop(); // Render is quiescent; core never called again.
    require(matchingDescriptors(fixture) == originals, "Joined playback retained loop/tempo/scratch/reader duplicates");
}
} // namespace

extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* out, size_t size, off_t offset)
{ holdRead(fd, std::uint64_t(offset)); return __real_pread(fd, out, size, offset); }
extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" ssize_t __wrap_pread64(int fd, void* out, size_t size, off64_t offset)
{ holdRead(fd, std::uint64_t(offset)); return __real_pread64(fd, out, size, offset); }
extern "C" ssize_t __real___pread_chk(int, void*, size_t, off_t, size_t);
extern "C" ssize_t __wrap___pread_chk(int fd, void* out, size_t size, off_t offset, size_t bound)
{ holdRead(fd, std::uint64_t(offset)); return __real___pread_chk(fd, out, size, offset, bound); }
extern "C" ssize_t __real___pread64_chk(int, void*, size_t, off64_t, size_t);
extern "C" ssize_t __wrap___pread64_chk(int fd, void* out, size_t size, off64_t offset, size_t bound)
{ holdRead(fd, std::uint64_t(offset)); return __real___pread64_chk(fd, out, size, offset, bound); }

int main()
{
    try {
        failedProcessedScratchCanReleaseAndReplace();
        normalPlatterBusAndRelease();
        sharedStemGainsAndLinearContinuation();
        explicitCancellationAndTokens();
        processedTempoScratchReprimesAtConsumedCursor();
        processedScratchModeMatrix();
        loopTempoGatesAndReverseFromEof();
        pendingMovementAndQuiescedCleanup();
        std::cout << "audio scratch playback: 8 functional cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio scratch playback: " << error.what() << '\n';
        return 1;
    }
}

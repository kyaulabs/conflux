// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace {
using conflux::audio::RenderCore;
using Command = RenderCore::Command;
using Type = Command::Type;

constexpr std::array<float, 24> sourceA{
    -0.F, .0625F, .125F, -.1875F, .25F, -.3125F, .375F, -.4375F,
    .5F, -.5625F, .625F, -.6875F, .75F, -.8125F, .875F, -.9375F,
    1.F, -1.0625F, 1.125F, -1.1875F, 1.25F, -1.3125F, 1.375F, -1.4375F};
constexpr std::array<float, 8> sourceB{-.5F, .75F, .25F, -.125F, .0625F, .5F, -1.F, .25F};

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

struct State {
    std::uint64_t clock;
    std::array<std::uint64_t, 2> cursor, cue;
    std::array<bool, 2> playing, held, preview;
    std::array<float, 2> gain;
    float master;
    bool operator==(const State&) const = default;
};

State state(const RenderCore& core)
{
    return {core.frameClock(), {core.sourceFrame(0), core.sourceFrame(1)},
            {core.cueFrame(0), core.cueFrame(1)}, {core.isPlaying(0), core.isPlaying(1)},
            {core.isCueHeld(0), core.isCueHeld(1)},
            {core.isCuePreviewing(0), core.isCuePreviewing(1)},
            {core.deckGain(0), core.deckGain(1)}, core.masterGain()};
}

template<std::size_t N>
bool silence(const std::array<float, N>& samples)
{
    return std::ranges::all_of(samples, [](float sample) { return sample == 0.F; });
}

bool hotCueCaptureRecallAndIsolation()
{
    bool passed = true;
    RenderCore core(sourceA, sourceB);
    std::array<float, 2> output{};
    passed &= require(core.setPlaying(0, false) && core.seek(0, 3) && core.setCueFrame(0, 1) &&
        core.setDeckGain(0, .5F) && core.setPlaying(1, false), "hot-cue setup failed");
    Command command{Type::HotCue, 0, 999};
    command.hotCue = 7;
    const auto before = state(core);
    passed &= require(core.apply(command) && core.hotCues(0).set[7] && core.hotCues(0).frames[7] == 3 &&
        state(core) == before, "empty hot cue did not capture the render cursor without changing transport");
    passed &= require(core.seek(0, 9) && core.apply(command) && core.isPlaying() &&
        core.sourceFrame() == 3 && core.cueFrame() == 1 && core.render(output) &&
        output == std::array<float, 2>{.1875F, -.21875F} && core.sourceFrame() == 4,
        "hot cue did not atomically seek/play the actual source with its gain and temporary cue preserved");
    passed &= require(core.hotCues(1) == RenderCore::HotCues{} && !core.isPlaying(1),
        "hot cue changed the other deck");
    passed &= require(core.setPlaying(0, false) && core.seek(0, 1) && core.cuePress(0) &&
        core.isCuePreviewing() && core.apply(command) && core.cueRelease(0) && core.isPlaying() &&
        core.sourceFrame() == 3, "release of temporary CUE stopped hot-cue playback");
    passed &= require(core.render(output) && core.sourceFrame() == 4 && core.apply(command) &&
        core.sourceFrame() == 3, "repeated hot cue did not retrigger during playback");
    const auto running = state(core);
    command.type = Type::HotCueClear;
    passed &= require(core.apply(command) && core.hotCues(0) == RenderCore::HotCues{} &&
        state(core) == running && core.apply(command), "clearing a hot cue changed transport or was not idempotent");
    return passed;
}

bool hotCueValidationAndSourceLifetime()
{
    bool passed = true;
    RenderCore core(sourceA);
    RenderCore::HotCues points;
    for (std::size_t index = 0; index < points.set.size(); ++index) {
        points.set[index] = true;
        points.frames[index] = index;
    }
    passed &= require(core.restoreHotCues(0, points) && core.hotCues(0) == points,
        "eight source-bound hot cues could not be restored");
    const auto before = state(core);
    for (const auto deck : {std::size_t(1), std::size_t(2), std::numeric_limits<std::size_t>::max()})
        passed &= require(!core.hotCue(deck, 0) && !core.clearHotCue(deck, 0) &&
            !core.restoreHotCues(deck, points) && core.hotCues(deck) == RenderCore::HotCues{},
            "invalid/unloaded hot-cue deck was accepted");
    for (const auto slot : {std::size_t(8), std::numeric_limits<std::size_t>::max()})
        passed &= require(!core.hotCue(0, slot) && !core.clearHotCue(0, slot), "invalid hot-cue slot was accepted");
    auto invalid = points;
    invalid.frames[7] = sourceA.size() / 2;
    passed &= require(!core.restoreHotCues(0, invalid) && core.hotCues(0) == points && state(core) == before,
        "invalid restored hot cue partially changed accepted state");
    passed &= require(core.clearHotCue(0, 0) && core.seek(0, core.sourceFrames()) && !core.hotCue(0, 0) &&
        !core.hotCues(0).set[0], "hot cue captured EOF instead of an actual source sample");
    passed &= require(core.setPlaying(0, false) && core.seek(0, 0) && core.replaceSource(0, {sourceB.size() / 2, sourceB.data()}) &&
        core.hotCues(0) == RenderCore::HotCues{}, "replacing the source retained another track's hot cues");
    RenderCore empty;
    passed &= require(!empty.hotCue(0, 0) && !empty.clearHotCue(0, 0) &&
        !empty.restoreHotCues(0, points), "empty source admitted hot cues");
    return passed;
}

bool settingAndBackCue()
{
    bool passed = true;
    RenderCore core(sourceA);
    passed &= require(core.cueFrame() == 0 && !core.isCueHeld() && !core.isCuePreviewing(),
                      "new deck did not have an unheld temporary cue at frame zero");
    passed &= require(core.setPlaying(0, false) && core.seek(3), "paused setup failed");
    std::array<float, 4> output{};
    passed &= require(core.render(output) && silence(output) && core.sourceFrame() == 3,
                      "paused setup advanced the source");
    // A stale/malicious command payload cannot supply the set point. The owner
    // sets CUE from its cursor after the earlier ordered seek has taken effect.
    passed &= require(core.apply({Type::CuePress, 0, 999}) && core.cueFrame() == 3 &&
                          core.isCueHeld() && !core.isPlaying() && !core.isCuePreviewing(),
                      "paused CUE did not use the exact render cursor");
    const auto held = state(core);
    passed &= require(core.cuePress(0) && state(core) == held &&
                          core.render(output) && silence(output) && core.sourceFrame() == 3,
                      "setting CUE or a duplicate down event started auditioning");
    passed &= require(core.cueRelease(0) && core.setPlaying(0, true) && core.render(output) &&
                          output == std::array<float, 4>{.375F, -.4375F, .5F, -.5625F} &&
                          core.sourceFrame() == 5,
                      "normal playback did not start from the new cue cursor");
    const auto clock = core.frameClock();
    passed &= require(core.apply({Type::CuePress, 0}) && core.sourceFrame() == 3 &&
                          core.cueFrame() == 3 && !core.isPlaying() && core.isCueHeld() &&
                          !core.isCuePreviewing() && core.frameClock() == clock,
                      "CUE during playback did not return to cue and pause");
    passed &= require(core.render(output) && silence(output) && core.sourceFrame() == 3 &&
                          core.cueRelease(0),
                      "back-cue press auditioned before a separate press");
    return passed;
}

bool auditionReleaseAndPlayLatch()
{
    bool passed = true;
    RenderCore core(sourceA);
    passed &= require(core.setPlaying(0, false) && core.setCueFrame(0, 2) && core.seek(2) &&
                          core.cuePress(0) && core.isCueHeld() && core.isCuePreviewing() &&
                          !core.isPlaying(),
                      "press at the stored cue did not start a temporary audition");
    std::array<float, 6> output{};
    passed &= require(core.render(output) &&
                          output == std::array<float, 6>{.25F, -.3125F, .375F, -.4375F, .5F, -.5625F} &&
                          core.sourceFrame() == 5 && core.frameClock() == 3 && !core.isPlaying(),
                      "audition did not render the expected unity stereo samples");
    const auto held = state(core);
    passed &= require(core.cuePress(0) && state(core) == held,
                      "duplicate CUE down reset or latched an audition");
    passed &= require(core.apply({Type::CueRelease, 0}) && core.sourceFrame() == 2 &&
                          !core.isCueHeld() && !core.isCuePreviewing() && !core.isPlaying() &&
                          core.frameClock() == 3,
                      "CUE release failed to return the unlatched audition to cue");
    const auto released = state(core);
    passed &= require(core.cueRelease(0) && state(core) == released,
                      "duplicate CUE release changed state");

    std::array<float, 2> frame{};
    passed &= require(core.cuePress(0) && core.render(frame) && core.sourceFrame() == 3 &&
                          core.apply({Type::Play, 0}) && core.isPlaying() && core.isCueHeld() &&
                          !core.isCuePreviewing(),
                      "PLAY while CUE was held did not latch playback");
    const auto latched = state(core);
    passed &= require(core.cuePress(0) && state(core) == latched && core.render(frame) &&
                          frame == std::array<float, 2>{.375F, -.4375F} &&
                          core.cueRelease(0) && core.sourceFrame() == 4 && core.isPlaying() &&
                          !core.isCueHeld() && core.cueFrame() == 2,
                      "CUE release rewound or stopped latched playback");
    passed &= require(core.render(frame) && frame == std::array<float, 2>{.5F, -.5625F},
                      "latched playback skipped the next source frame");

    RenderCore zero(sourceA);
    passed &= require(zero.setPlaying(0, false) && zero.cuePress(0) && zero.render(frame) &&
                          std::bit_cast<std::uint32_t>(frame[0]) == std::bit_cast<std::uint32_t>(-0.F),
                      "unity cue audition changed the source's signed-zero sample");
    return passed;
}

bool seekPauseAndIndependentEof()
{
    bool passed = true;
    RenderCore core(sourceA);
    std::array<float, 4> output{};
    passed &= require(core.setPlaying(0, false) && core.cuePress(0) && core.render(output) &&
                          core.seek(7) && !core.isCuePreviewing() && core.isCueHeld() &&
                          !core.isPlaying() && core.cueFrame() == 0 && core.render(output) &&
                          silence(output) && core.cueRelease(0) && core.sourceFrame() == 7,
                      "SEEK failed to cancel audition while preserving pause and stored cue");
    passed &= require(core.cuePress(0) && core.cueFrame() == 7 && !core.isCuePreviewing() &&
                          core.cueRelease(0) && core.cuePress(0) && core.render(output) &&
                          core.apply({Type::Pause, 0}) && core.sourceFrame() == 9 &&
                          !core.isCuePreviewing() && core.isCueHeld() && !core.isPlaying() &&
                          core.cueRelease(0) && core.sourceFrame() == 9 && core.cueFrame() == 7,
                      "PAUSE failed to cancel audition at the exact current cursor");
    passed &= require(core.setPlaying(0, true) && core.seek(4) && core.isPlaying() &&
                          core.cueFrame() == 7,
                      "ordinary playing seek lost the persistent request or changed cue");

    RenderCore mixed(sourceA, sourceB);
    passed &= require(mixed.setPlaying(0, false) && mixed.setCueFrame(0, 10) &&
                          mixed.seek(0, 10) && mixed.cuePress(0), "mixed cue setup failed");
    std::array<float, 8> sum{};
    passed &= require(mixed.render(sum) &&
                          sum == std::array<float, 8>{.75F, -.5625F, 1.625F, -1.5625F, .0625F, .5F, -1.F, .25F} &&
                          mixed.sourceFrame(0) == 12 && mixed.sourceFrame(1) == 4 &&
                          mixed.isCuePreviewing(0) && !mixed.isPlaying(0) && mixed.isPlaying(1) &&
                          mixed.frameClock() == 4,
                      "cue preview and normal peer did not reach independent EOFs");
    passed &= require(mixed.render(sum) && silence(sum) && mixed.frameClock() == 8 &&
                          mixed.cueRelease(0) && mixed.sourceFrame(0) == 10 &&
                          mixed.sourceFrame(1) == 4 && mixed.isPlaying(1) &&
                          mixed.cuePress(1) && mixed.sourceFrame(1) == 0 && !mixed.isPlaying(1) &&
                          mixed.cueFrame(0) == 10,
                      "EOF back-cue or release affected the other deck");

    RenderCore end(sourceA);
    passed &= require(end.setPlaying(0, false) && end.seek(12) && end.cuePress(0) &&
                          end.cueFrame() == 12 && end.cueRelease(0) && end.cuePress(0) &&
                          end.render(output) && silence(output) && end.sourceFrame() == 12 &&
                          end.isCuePreviewing() && end.cueRelease(0) && end.sourceFrame() == 12,
                      "a stored EOF cue did not remain bounded silent transport");
    return passed;
}

bool invalidStateAndRestoration()
{
    bool passed = true;
    RenderCore core(sourceA);
    passed &= require(core.setPlaying(0, false) && core.setCueFrame(0, 2) && core.seek(2) &&
                          core.cuePress(0), "rejection setup failed");
    const auto before = state(core);
    for (const auto deck : {std::size_t{1}, std::size_t{2}, std::numeric_limits<std::size_t>::max()}) {
        passed &= require(!core.cuePress(deck) && !core.cueRelease(deck) &&
                              !core.setCueFrame(deck, 0) && state(core) == before,
                          "invalid or unloaded cue operation changed state");
        passed &= require(core.cueFrame(deck) == 0 && !core.isCueHeld(deck) && !core.isCuePreviewing(deck),
                          "invalid or unloaded cue getter was not empty");
    }
    passed &= require(!core.setCueFrame(0, 13) && !core.seek(13) &&
                          !core.setCueFrame(0, std::numeric_limits<std::uint64_t>::max()) &&
                          !core.apply({Type::CuePress, 2}) && !core.apply({Type::CueRelease, 1}) &&
                          !core.apply({static_cast<Type>(255), 0}) && state(core) == before,
                      "rejected bounds or command canceled a valid audition");
    std::array<float, 3> malformed{42.F, 42.F, 42.F};
    passed &= require(!core.render(malformed) && state(core) == before &&
                          malformed == std::array<float, 3>{42.F, 42.F, 42.F} &&
                          core.render({}) && state(core) == before,
                      "invalid/empty output changed cue state or destination");
    passed &= require(core.cueRelease(0) && core.seek(6) && core.setDeckGain(0, .5F) &&
                          core.setMasterGain(.25F), "restoration source setup failed");
    RenderCore restored(sourceA);
    passed &= require(restored.seek(core.sourceFrame()) &&
                          restored.setPlaying(0, core.isPlaying()) &&
                          restored.setCueFrame(0, core.cueFrame()) &&
                          restored.setDeckGain(0, core.deckGain()) && restored.setMasterGain(core.masterGain()) &&
                          state(restored) == state(core) && !restored.isCueHeld() && !restored.isCuePreviewing(),
                      "stopped-session cue restoration changed independent transport state");
    RenderCore empty;
    passed &= require(!empty.cuePress(0) && !empty.cueRelease(0) && !empty.setCueFrame(0, 0),
                      "default silence accepted a cue command without a source");
    return passed;
}

bool partitionParity()
{
    bool passed = true;
    RenderCore whole(sourceA, sourceB), split(sourceA, sourceB);
    for (auto* core : {&whole, &split})
        passed &= require(core->setPlaying(0, false) && core->setCueFrame(0, 2) && core->seek(2),
                          "partition setup failed");
    const auto command = [&](Command value) {
        return require(whole.apply(value) && split.apply(value) && state(whole) == state(split),
                       "command effects differed at a fixed output boundary");
    };
    const auto render = [&](std::size_t frames) {
        std::vector<float> expected(frames * 2), actual(frames * 2);
        bool ok = require(whole.render(expected), "whole cue render failed");
        for (std::size_t frame = 0; frame < frames; ++frame)
            ok &= require(split.render(std::span(actual).subspan(frame * 2, 2)),
                          "single-frame cue partition failed");
        return ok && require(expected == actual && state(whole) == state(split),
                             "cue audio or state depended on block partitioning");
    };
    passed &= command({Type::CuePress, 0});
    passed &= render(3);
    passed &= command({Type::CueRelease, 0});
    passed &= render(2);
    passed &= command({Type::CuePress, 0});
    passed &= render(4);
    passed &= command({Type::Play, 0});
    passed &= render(5);
    passed &= command({Type::CueRelease, 0});
    passed &= render(4);
    passed &= command({Type::CuePress, 0});
    passed &= render(1);
    passed &= command({Type::CueRelease, 0});
    passed &= command({Type::Seek, 0, 8});
    passed &= command({Type::CuePress, 0});
    passed &= render(2);
    passed &= command({Type::CueRelease, 0});
    passed &= command({Type::CuePress, 0});
    passed &= render(8);
    passed &= command({Type::CueRelease, 0});
    return passed;
}
} // namespace

int main()
{
    const bool setting = settingAndBackCue();
    const bool audition = auditionReleaseAndPlayLatch();
    const bool transport = seekPauseAndIndependentEof();
    const bool invalid = invalidStateAndRestoration();
    const bool partitions = partitionParity();
    return hotCueCaptureRecallAndIsolation() && hotCueValidationAndSourceLifetime() &&
        setting && audition && transport && invalid && partitions ? 0 : 1;
}

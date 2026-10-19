// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioSource.hpp"
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using conflux::audio::AudioReadResult;
using conflux::audio::AudioSource;
using conflux::audio::RenderCore;

bool require(bool value, const char* message)
{
    if (!value)
        std::cerr << message << '\n';
    return value;
}

template<class Function>
bool rejects(Function&& function)
{
    try { function(); }
    catch (const std::invalid_argument&) { return true; }
    return require(false, "invalid source descriptor was accepted");
}

std::vector<float> signal(std::size_t frames, int seed)
{
    std::vector<float> result(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        result[frame * 2] = static_cast<float>((frame * 7 + seed) % 101) / 32.F - 1.F;
        result[frame * 2 + 1] = static_cast<float>((frame * 11 + seed * 3) % 97) / 16.F - 2.F;
    }
    if (seed == 0)
        result[0] = -0.F;
    return result;
}

// No callback storage grows. The fixture enforces sequential reads unless an
// owner discontinuity explicitly changes the expected source position.
struct FakeSource {
    std::span<const float> pcm;
    std::uint64_t expectedFrame = 0, lastDiscontinuity = 0;
    std::size_t calls = 0, discontinuities = 0, badPositions = 0, largestRequest = 0;
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    bool ready = true, fail = false, dropOnDiscontinuity = false;
    bool overreport = false, nonfinite = false;

    AudioSource source() noexcept { return {pcm.size() / 2, nullptr, this, read, discontinuity}; }

    static AudioReadResult read(void* context, std::uint64_t first, std::span<float> output) noexcept
    {
        auto& self = *static_cast<FakeSource*>(context);
        ++self.calls;
        self.largestRequest = std::max(self.largestRequest, output.size() / 2);
        if (first != self.expectedFrame || first > self.pcm.size() / 2 || output.size() % 2) {
            ++self.badPositions;
            return {0, true};
        }
        if (self.overreport)
            return {output.size() / 2 + 1, false};
        if (!self.ready)
            return {0, self.fail};
        const auto frames = std::min({output.size() / 2, self.limit,
                                     self.pcm.size() / 2 - static_cast<std::size_t>(first)});
        std::copy_n(self.pcm.data() + first * 2, frames * 2, output.data());
        if (self.nonfinite && frames)
            output[0] = std::numeric_limits<float>::quiet_NaN();
        self.expectedFrame += frames;
        return {frames, self.fail};
    }

    static void discontinuity(void* context, std::uint64_t frame) noexcept
    {
        auto& self = *static_cast<FakeSource*>(context);
        ++self.discontinuities;
        self.lastDiscontinuity = self.expectedFrame = frame;
        if (self.dropOnDiscontinuity)
            self.ready = false;
    }
};

bool descriptorsAndMemoryParity()
{
    bool passed = true;
    const auto a = signal(4099, 0), b = signal(3001, 5);
    FakeSource invalid{a};
    passed &= rejects([] { RenderCore core{AudioSource{}}; });
    passed &= rejects([] { RenderCore core{AudioSource{20}}; });
    passed &= rejects([&] { RenderCore core{AudioSource{20, a.data(), &invalid, FakeSource::read}}; });
    passed &= rejects([&] { RenderCore core{AudioSource{0, nullptr, &invalid, FakeSource::read}}; });
    RenderCore memory(a, b), descriptors(AudioSource::fromMemory(a), AudioSource::fromMemory(b));
    FakeSource first{a}, second{b};
    RenderCore streamed(first.source(), second.source());
    FakeSource splitA{a}, splitB{b};
    RenderCore split(splitA.source(), splitB.source());
    for (auto* core : {&memory, &descriptors, &streamed, &split})
        passed &= require(core->setDeckGain(0, .37F) && core->setDeckGain(1, .61F) && core->setMasterGain(.73F),
                          "source parity gain setup failed");
    std::vector<float> expected(7001 * 2), descriptorOutput(expected.size()),
        streamedOutput(expected.size()), partitioned(expected.size());
    passed &= require(memory.render(expected) && descriptors.render(descriptorOutput) && streamed.render(streamedOutput),
                      "whole source parity render failed");
    constexpr std::array<std::size_t, 5> sizes{1, 7, 256, 1024, 2049};
    for (std::size_t offset = 0, index = 0; offset < partitioned.size(); ++index) {
        const auto samples = std::min(sizes[index % sizes.size()] * 2, partitioned.size() - offset);
        passed &= require(split.render(std::span(partitioned).subspan(offset, samples)),
                          "partitioned source render failed");
        offset += samples;
    }
    passed &= require(expected == descriptorOutput && expected == streamedOutput && expected == partitioned,
                      "source/memory mix or partition parity changed exact samples");
    for (auto* core : {&descriptors, &streamed, &split})
        passed &= require(core->frameClock() == 7001 && core->sourceFrame(0) == 4099 && core->sourceFrame(1) == 3001 &&
                              !core->isBuffering(0) && !core->isBuffering(1) && !core->readFailed(0) &&
                              !core->readFailed(1) && core->starvedFrames(0) == 0 && core->starvedFrames(1) == 0,
                          "ready-source EOF affected clocks or starvation state");
    passed &= require(first.badPositions == 0 && second.badPositions == 0 && splitA.badPositions == 0 &&
                          splitB.badPositions == 0 && first.largestRequest <= 1024 && second.largestRequest <= 1024 &&
                          splitA.largestRequest <= 1024 && splitB.largestRequest <= 1024,
                      "source reads were out of order or exceeded fixed scratch storage");

    FakeSource unity{a};
    RenderCore single(unity.source());
    std::array<float, 8> samples{};
    passed &= require(single.render(samples), "single streamed unity render failed");
    for (std::size_t index = 0; index < samples.size(); ++index)
        passed &= require(std::bit_cast<std::uint32_t>(samples[index]) == std::bit_cast<std::uint32_t>(a[index]),
                          "streamed unity output changed headroom or signed-zero bits");
    FakeSource onlyB{b};
    RenderCore peerOnly(AudioSource{}, onlyB.source());
    passed &= require(peerOnly.sourceFrames(0) == 0 && peerOnly.sourceFrames(1) == 3001 &&
                          !peerOnly.isPlaying(0) && peerOnly.isPlaying(1) && peerOnly.render(samples) &&
                          std::equal(samples.begin(), samples.end(), b.begin()),
                      "single loaded source on deck B did not retain stereo/transport behavior");
    return passed;
}

bool partialReadStallAndRecovery()
{
    bool passed = true;
    const auto a = signal(3000, 1), b = signal(3000, 5);
    FakeSource first{a};
    first.limit = 3;
    RenderCore core(first.source(), AudioSource::fromMemory(b));
    std::vector<float> output(2050 * 2);
    passed &= require(core.render(output), "partial source render was treated as invalid output");
    for (std::size_t sample = 0; sample < output.size(); ++sample) {
        const auto expected = sample < 6 ? a[sample] + b[sample] : b[sample];
        passed &= require(output[sample] == expected, "partial source reused stale samples or interrupted its peer");
    }
    passed &= require(first.calls == 1 && core.sourceFrame(0) == 3 && core.sourceFrame(1) == 2050 &&
                          core.frameClock() == 2050 && core.isBuffering(0) && !core.isBuffering(1) &&
                          core.starvedFrames(0) == 2047 && !core.readFailed(0),
                      "partial read did not silence the remaining block with independent clocks");
    first.ready = false;
    first.limit = std::numeric_limits<std::size_t>::max();
    std::array<float, 8> block{};
    passed &= require(core.render(block) && core.sourceFrame(0) == 3 && core.sourceFrame(1) == 2054 &&
                          core.starvedFrames(0) == 2051 && core.isBuffering(0) &&
                          std::equal(block.begin(), block.end(), b.begin() + 2050 * 2),
                      "missing read skipped source time or silenced the healthy peer");
    const auto calls = first.calls;
    passed &= require(core.setPlaying(0, false) && core.render(block) && first.calls == calls &&
                          !core.isBuffering(0) && core.starvedFrames(0) == 2051 && core.sourceFrame(0) == 3,
                      "paused source was read or counted as buffering/starved");
    first.ready = true;
    passed &= require(core.setPlaying(0, true) && core.setDeckGain(1, 0.F) && core.render(block) &&
                          std::equal(block.begin(), block.end(), a.begin() + 6) &&
                          core.sourceFrame(0) == 7 && core.sourceFrame(1) == 2062 && core.frameClock() == 2062 &&
                          !core.isBuffering(0) && core.starvedFrames(0) == 2051 && !core.readFailed(0),
                      "ready recovery skipped the missing source prefix or reset cumulative counters");

    FakeSource nearEnd{std::span(a).first(10)};
    nearEnd.limit = 2;
    RenderCore eof(nearEnd.source());
    std::array<float, 20> tenFrames{};
    passed &= require(eof.render(tenFrames) && eof.sourceFrame() == 2 && eof.frameClock() == 10 &&
                          eof.starvedFrames() == 3 && eof.isBuffering(),
                      "known EOF silence was incorrectly counted as unavailable source frames");
    nearEnd.limit = std::numeric_limits<std::size_t>::max();
    passed &= require(eof.render(tenFrames) && eof.sourceFrame() == 5 && !eof.isBuffering() &&
                          eof.starvedFrames() == 3 && eof.isPlaying(), "ready EOF recovery changed starvation or request");
    const auto eofCalls = nearEnd.calls;
    passed &= require(eof.render(tenFrames) && nearEnd.calls == eofCalls && eof.frameClock() == 30 &&
                          std::ranges::all_of(tenFrames, [](float x) { return x == 0.F; }),
                      "EOF read the source again or did not advance clocked silence");
    return passed;
}

bool failureAndInvalidOutput()
{
    bool passed = true;
    const auto a = signal(10, 1), b = signal(10, 5);
    FakeSource failed{a};
    failed.limit = 2;
    failed.fail = true;
    RenderCore core(failed.source(), AudioSource::fromMemory(b));
    std::array<float, 12> output{};
    passed &= require(core.render(output) && core.sourceFrame(0) == 2 && core.sourceFrame(1) == 6 &&
                          core.starvedFrames(0) == 4 && core.readFailed(0) && core.isBuffering(0),
                      "failed read lost a valid prefix or blocked its peer");
    for (std::size_t sample = 0; sample < output.size(); ++sample)
        passed &= require(output[sample] == (sample < 4 ? a[sample] + b[sample] : b[sample]),
                          "failed read delivered an invalid suffix");
    failed.fail = false;
    failed.limit = std::numeric_limits<std::size_t>::max();
    std::array<float, 8> block{};
    passed &= require(core.render(block) && core.sourceFrame(0) == 6 && core.sourceFrame(1) == 10 &&
                          !core.isBuffering(0) && core.readFailed(0) && core.starvedFrames(0) == 4,
                      "later valid data cleared the sticky read failure or lost source position");

    FakeSource stalled{a};
    stalled.ready = false;
    RenderCore invalid(stalled.source());
    passed &= require(invalid.render(block) && invalid.isBuffering() && invalid.starvedFrames() == 4,
                      "invalid-output setup did not stall");
    const auto calls = stalled.calls, clock = invalid.frameClock();
    std::array<float, 3> malformed{42.F, 42.F, 42.F};
    passed &= require(!invalid.render(malformed) && invalid.render({}) && stalled.calls == calls &&
                          invalid.frameClock() == clock && invalid.sourceFrame() == 0 &&
                          invalid.isBuffering() && invalid.starvedFrames() == 4 && !invalid.readFailed() &&
                          malformed == std::array<float, 3>{42.F, 42.F, 42.F},
                      "invalid/empty output changed the source, stats or destination");
    passed &= require(!invalid.seek(11) && !invalid.seek(1, 0) && !invalid.cuePress(1) &&
                          !invalid.cueRelease(1) && stalled.discontinuities == 0 && stalled.calls == calls &&
                          !invalid.isBuffering(2) && invalid.starvedFrames(2) == 0 && !invalid.readFailed(2),
                      "invalid transport changed stream state or invalid-deck getters");

    for (const bool nonfinite : {false, true}) {
        FakeSource bad{a};
        bad.nonfinite = nonfinite;
        bad.overreport = !nonfinite;
        RenderCore malformedSource(bad.source());
        passed &= require(malformedSource.render(block) && malformedSource.readFailed() &&
                              malformedSource.sourceFrame() == 0 && malformedSource.starvedFrames() == 4 &&
                              std::ranges::all_of(block, [](float x) { return x == 0.F; }),
                          "malformed source result escaped finite output/bounds validation");
    }
    return passed;
}

bool stoppedBufferingState()
{
    const auto input = signal(40, 1);
    FakeSource reader{input};
    reader.ready = false;
    RenderCore core(reader.source());
    std::array<float, 8> block{};
    bool passed = require(core.render(block) && core.isBuffering() && core.setPlaying(0, false) &&
                          !core.isBuffering() && core.frameClock() == 4 && core.starvedFrames() == 4,
                          "pause retained stale buffering without a following render");
    passed &= require(core.setPlaying(0, true) && core.render(block) && core.isBuffering() &&
                          core.seek(0) && !core.isBuffering() && core.frameClock() == 8 && core.starvedFrames() == 8,
                          "same-position seek retained stale buffering or changed clocks");
    passed &= require(core.cuePress(0) && !core.isPlaying() && !core.isBuffering() && core.cueRelease(0) &&
                          core.cuePress(0) && core.isCuePreviewing() && core.render(block) && core.isBuffering() &&
                          core.cueRelease(0) && !core.isBuffering() && !core.isCuePreviewing() &&
                          core.sourceFrame() == 0 && core.frameClock() == 12 && core.starvedFrames() == 12,
                          "release cleanup retained a stopped buffering flag or changed clocks");
    return passed;
}

bool discontinuityAndCue()
{
    bool passed = true;
    const auto input = signal(40, 1);
    FakeSource reader{input};
    reader.dropOnDiscontinuity = true;
    RenderCore core(reader.source());
    std::array<float, 8> output{};
    passed &= require(core.render(output) && core.sourceFrame() == 4 && core.seek(4) &&
                          reader.discontinuities == 0 && core.render(output) && core.sourceFrame() == 8,
                      "an unchanged cursor unnecessarily invalidated ready source data");
    passed &= require(core.seek(20) && reader.discontinuities == 1 && reader.lastDiscontinuity == 20 &&
                          core.render(output) && core.sourceFrame() == 20 && core.isBuffering() &&
                          std::ranges::all_of(output, [](float x) { return x == 0.F; }),
                      "seek did not notify the source or leaked cached pre-seek samples");
    reader.ready = true;
    passed &= require(core.render(output) && std::equal(output.begin(), output.end(), input.begin() + 40) &&
                          core.sourceFrame() == 24 && !core.isBuffering(), "seek recovery read the wrong source generation");
    passed &= require(core.setPlaying(0, false) && core.seek(10) && reader.discontinuities == 2,
                      "paused seek did not publish its discontinuity");
    const auto reads = reader.calls;
    passed &= require(core.render(output) && reader.calls == reads && core.cuePress(0) &&
                          core.cueFrame() == 10 && !core.isCuePreviewing() && reader.discontinuities == 2 &&
                          core.cueRelease(0) && core.cuePress(0) && core.isCuePreviewing(),
                      "paused CUE set changed cursor or read inactive source data");
    reader.ready = true;
    passed &= require(core.render(output) && core.sourceFrame() == 14 && core.cueRelease(0) &&
                          core.sourceFrame() == 10 && reader.discontinuities == 3 && reader.lastDiscontinuity == 10,
                      "cue release-return did not invalidate the advanced source cursor");
    passed &= require(core.seek(10) && reader.discontinuities == 3 && core.setPlaying(0, true),
                      "same-position seek invalidated the cue return twice");
    reader.ready = true;
    passed &= require(core.render(output) && core.sourceFrame() == 14 && core.cuePress(0) &&
                          core.sourceFrame() == 10 && !core.isPlaying() && reader.discontinuities == 4 &&
                          core.cueRelease(0) && core.cuePress(0),
                      "playing back-cue did not stop and notify the stored target");
    reader.ready = true;
    passed &= require(core.render(output) && core.setPlaying(0, true) && core.cueRelease(0) &&
                          core.sourceFrame() == 14 && core.isPlaying() && reader.discontinuities == 4 &&
                          core.render(output) && reader.badPositions == 0,
                      "PLAY latch introduced a false discontinuity or stale stream read");
    return passed;
}
} // namespace

int main()
{
    const bool parity = descriptorsAndMemoryParity();
    const bool partial = partialReadStallAndRecovery();
    const bool failure = failureAndInvalidOutput();
    const bool discontinuity = discontinuityAndCue();
    const bool stopped = stoppedBufferingState();
    return parity && partial && failure && discontinuity && stopped ? 0 : 1;
}

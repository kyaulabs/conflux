// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace {
using conflux::audio::AudioReadResult;
using conflux::audio::AudioSource;
using conflux::audio::RenderCore;
using Command = RenderCore::Command;
using Type = Command::Type;

bool require(bool value, const char* message)
{
    if (!value)
        std::cerr << message << '\n';
    return value;
}

bool sameMeter(const RenderCore::StereoMeter& first, const RenderCore::StereoMeter& second)
{
    return first.peak == second.peak && first.rms == second.rms &&
        first.overrangeSamples == second.overrangeSamples;
}

bool sameMeters(const RenderCore::Meters& first, const RenderCore::Meters& second)
{
    return first.blockFrames == second.blockFrames && sameMeter(first.deck[0], second.deck[0]) &&
        sameMeter(first.deck[1], second.deck[1]) && sameMeter(first.master, second.master) && sameMeter(first.cue, second.cue);
}

bool sameBits(std::span<const float> first, std::span<const float> second)
{
    if (first.size() != second.size())
        return false;
    for (std::size_t index = 0; index < first.size(); ++index)
        if (std::bit_cast<std::uint32_t>(first[index]) != std::bit_cast<std::uint32_t>(second[index]))
            return false;
    return true;
}

bool meterMatches(const RenderCore::StereoMeter& meter, std::span<const float> output,
                  std::array<std::uint64_t, 2> accumulatedOverrange)
{
    std::array<double, 2> peak{}, square{};
    for (std::size_t frame = 0; frame < output.size() / 2; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const double sample = output[frame * 2 + channel];
            peak[channel] = std::max(peak[channel], std::abs(sample));
            square[channel] += sample * sample;
        }
    for (std::size_t channel = 0; channel < 2; ++channel)
        if (meter.peak[channel] != peak[channel] ||
            std::abs(meter.rms[channel] - std::sqrt(square[channel] / (output.size() / 2))) > 1e-12 ||
            meter.overrangeSamples[channel] != accumulatedOverrange[channel])
            return false;
    return true;
}

struct Reader {
    std::span<const float> samples;
    std::size_t calls = 0, largestRequest = 0, limit = std::numeric_limits<std::size_t>::max();
    bool failed = false;
    AudioSource source() noexcept { return {samples.size() / 2, nullptr, this, read, nullptr}; }
    static AudioReadResult read(void* context, std::uint64_t first, std::span<float> destination) noexcept
    {
        auto& self = *static_cast<Reader*>(context);
        ++self.calls;
        self.largestRequest = std::max(self.largestRequest, destination.size() / 2);
        const auto count = std::min({self.limit, destination.size() / 2,
                                     self.samples.size() / 2 - static_cast<std::size_t>(first)});
        std::copy_n(self.samples.data() + first * 2, count * 2, destination.data());
        return {count, self.failed};
    }
};

bool independentBusesAndPreFaderMeters()
{
    bool passed = true;
    constexpr std::array<float, 8> a{.25F, -.5F, .5F, .75F, -1.25F, 1.5F, 0.F, -0.F};
    constexpr std::array<float, 6> b{.75F, .25F, -.25F, .5F, 1.F, -.5F};
    constexpr std::array<float, 8> expectedMaster{.15625F, -.09375F, .09375F, .25F, -.1875F, .3125F, 0.F, -0.F};
    constexpr std::array<float, 8> expectedCue{.25F, -.0625F, .0625F, .3125F, -.0625F, .25F, 0.F, -0.F};
    RenderCore core(a, b), legacy(a, b);
    for (auto* renderer : {&core, &legacy})
        passed &= require(renderer->setDeckGain(0, .5F) && renderer->setDeckGain(1, .25F) &&
                              renderer->setMasterGain(.5F), "bus setup failed");
    passed &= require(!core.isCueSelected(0) && !core.isCueSelected(1) && core.cueGain() == 1.F &&
                          core.setCueSelected(0, true) && core.apply({Type::CueOn, 1}) && core.setCueGain(.25F),
                      "headphone cue defaults or selection commands differ");
    std::array<float, 8> master{}, cue{}, old{};
    passed &= require(core.renderBuses(master, cue) && legacy.render(old) && sameBits(master, old) &&
                          sameBits(master, expectedMaster) && sameBits(cue, expectedCue) && core.frameClock() == 4 &&
                          core.sourceFrame(0) == 4 && core.sourceFrame(1) == 3,
                      "cue altered master samples, stereo sums or source/output clock advancement");
    passed &= require(core.meters().blockFrames == 4 && meterMatches(core.meters().deck[0], a, {1, 1}) &&
                          meterMatches(core.meters().master, master, {0, 0}) &&
                          meterMatches(core.meters().cue, cue, {0, 0}), "bus or pre-fader deck meters differ");
    passed &= require(core.seek(0, 0) && core.setDeckGain(0, 0.F) && core.setMasterGain(0.F) &&
                          core.apply({Type::CueOff, 1}) && core.apply({Type::CueGain, 999, 0, 1.F}) &&
                          core.renderBuses(master, cue) && sameBits(cue, a) &&
                          std::ranges::all_of(master, [](float value) { return value == 0.F; }) &&
                          core.sourceFrame(0) == 4 && core.frameClock() == 8,
                      "channel/master mute incorrectly muted or stopped pre-fader headphone cue");
    passed &= require(meterMatches(core.meters().deck[0], a, {2, 2}) &&
                          meterMatches(core.meters().cue, cue, {1, 1}) &&
                          meterMatches(core.meters().master, master, {0, 0}),
                      "muted channel meters or cumulative overrange history were incorrect");
    return passed;
}

bool selectionAndTransportCueAreIndependent()
{
    bool passed = true;
    constexpr std::array<float, 12> source{.25F, -.5F, .5F, .75F, -1.25F, 1.5F, .125F, -.25F, .5F, -.75F, 1.F, -1.F};
    RenderCore core(source);
    std::array<float, 4> master{}, cue{};
    passed &= require(core.renderBuses(master, cue) &&
                          std::ranges::all_of(cue, [](float value) { return value == 0.F; }),
                      "unselected deck leaked into headphone cue");
    passed &= require(core.setPlaying(0, false) && core.setCueSelected(0, true) &&
                          core.renderBuses(master, cue) && core.sourceFrame() == 2 &&
                          std::ranges::all_of(master, [](float value) { return value == 0.F; }) &&
                          std::ranges::all_of(cue, [](float value) { return value == 0.F; }),
                      "headphone selection started a paused source");
    passed &= require(core.cuePress(0) && core.cueFrame() == 2 && !core.isCuePreviewing() &&
                          core.cueRelease(0) && core.cuePress(0) && core.renderBuses(master, cue) &&
                          sameBits(master, std::span(source).subspan(4, 4)) && sameBits(master, cue) &&
                          !core.isPlaying() && core.isCuePreviewing() && core.isCueSelected(),
                      "temporary transport CUE did not remain independent of headphone selection");
    passed &= require(core.setCueSelected(0, false) && core.renderBuses(master, cue) && core.sourceFrame() == 6 &&
                          std::ranges::all_of(cue, [](float value) { return value == 0.F; }) &&
                          core.isCuePreviewing() && core.cueRelease(0) && core.sourceFrame() == 2,
                      "headphone deselection rewound or canceled the transport audition");
    return passed;
}

bool sourceReadsStarvationAndRmsSilence()
{
    bool passed = true;
    constexpr std::array<float, 10> a{2.F, -3.F, -1.F, 1.F, .125F, .25F, .25F, -.125F, 0.F, 0.F};
    constexpr std::array<float, 8> b{.5F, .25F, -.5F, -.25F, .25F, .5F, -.25F, -.5F};
    constexpr std::array<float, 16> deliveredA{2.F, -3.F, -1.F, 1.F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<float, 16> expectedMaster{.5F, .25F, -.5F, -.25F, .25F, .5F, -.25F, -.5F, 0, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<float, 16> expectedCue{1.25F, -1.375F, -.75F, .375F, .125F, .25F, -.125F, -.25F, 0, 0, 0, 0, 0, 0, 0, 0};
    Reader reader{a};
    reader.limit = 2;
    reader.failed = true;
    RenderCore core(reader.source(), AudioSource::fromMemory(b));
    passed &= require(core.setDeckGain(0, 0.F) && core.setCueSelected(0, true) &&
                          core.setCueSelected(1, true) && core.setCueGain(.5F), "partial bus setup failed");
    std::array<float, 16> master{}, cue{};
    passed &= require(core.renderBuses(master, cue) && sameBits(master, expectedMaster) && sameBits(cue, expectedCue) &&
                          reader.calls == 1 && core.sourceFrame(0) == 2 && core.sourceFrame(1) == 4 &&
                          core.frameClock() == 8 && core.starvedFrames() == 3 && core.isBuffering() && core.readFailed(),
                      "bus rendering duplicated reads or mishandled partial/error/EOF source state");
    passed &= require(meterMatches(core.meters().deck[0], deliveredA, {1, 1}) &&
                          meterMatches(core.meters().master, expectedMaster, {0, 0}) &&
                          meterMatches(core.meters().cue, expectedCue, {1, 1}),
                      "RMS excluded starvation/EOF silence or overrange counted exact full scale as clipping");
    passed &= require(core.setPlaying(0, false) && core.renderBuses(master, cue) && reader.calls == 1 &&
                          core.meters().master.peak == std::array<double, 2>{} &&
                          core.meters().cue.rms == std::array<double, 2>{} &&
                          core.meters().deck[0].overrangeSamples == std::array<std::uint64_t, 2>{1, 1} &&
                          core.meters().cue.overrangeSamples == std::array<std::uint64_t, 2>{1, 1},
                      "paused/EOF bus silence read the source or cleared clipping history across snapshot gaps");

    std::vector<float> longSource(4099 * 2, .125F);
    Reader full{longSource};
    RenderCore once(full.source());
    passed &= require(once.setCueSelected(0, true), "single-read bus setup failed");
    std::vector<float> longMaster(2050 * 2), longCue(longMaster.size());
    passed &= require(once.renderBuses(longMaster, longCue) && full.calls == 3 && full.largestRequest <= 1024 &&
                          once.sourceFrame() == 2050 && once.frameClock() == 2050 && sameBits(longMaster, longCue),
                      "two buses read or advanced a streamed source more than once per render segment");
    return passed;
}

bool invalidCallsAndCommandsPreserveState()
{
    bool passed = true;
    constexpr std::array<float, 8> source{.5F, -.25F, 1.5F, -2.F, .25F, .75F, -.5F, 1.F};
    Reader reader{source};
    RenderCore core(reader.source());
    std::array<float, 4> master{}, cue{};
    passed &= require(core.setCueSelected(0, true) && core.renderBuses(master, cue), "invalid bus setup failed");
    const auto before = core.meters();
    const auto clock = core.frameClock(), cursor = core.sourceFrame();
    const auto calls = reader.calls;
    std::array<float, 8> output{};
    output.fill(42.F);
    passed &= require(!core.renderBuses(std::span(output).first(3), cue) &&
                          !core.renderBuses(master, std::span(output).first(3)) &&
                          !core.renderBuses(master, std::span(output).first(2)) &&
                          !core.renderBuses(master, {}) && !core.renderBuses({}, cue) &&
                          !core.renderBuses(std::span(output).first(4), std::span(output).first(4)) &&
                          !core.renderBuses(std::span(output).first(4), std::span(output).subspan(2, 4)) &&
                          core.renderBuses({}, {}) && core.render({}) &&
                          core.frameClock() == clock && core.sourceFrame() == cursor && reader.calls == calls &&
                          sameMeters(before, core.meters()) && std::ranges::all_of(output, [](float x) { return x == 42.F; }),
                      "invalid/empty bus geometry changed samples, reads, clocks or meters");
    for (const float bad : {-.01F, 1.01F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        passed &= require(!core.setCueGain(bad) && !core.apply({Type::CueGain, 0, 0, bad}) && core.cueGain() == 1.F,
                          "invalid global cue gain changed state");
    passed &= require(!core.setCueSelected(1, true) && !core.setCueSelected(1, false) &&
                          !core.setCueSelected(2, true) && !core.apply({Type::CueOn, 1}) && !core.apply({Type::CueOff, 2}) &&
                          core.isCueSelected(0) && !core.isCueSelected(1) && !core.isCueSelected(2) &&
                          sameMeters(before, core.meters()) && core.frameClock() == clock && reader.calls == calls,
                      "invalid/unloaded headphone cue command changed state");
    return passed;
}

bool finiteOverflowAndMeterSquares()
{
    bool passed = true;
    const auto maximum = std::numeric_limits<float>::max();
    const std::array<float, 2> source{maximum, -maximum};
    RenderCore core(source, source);
    std::array<float, 2> master{}, cue{};
    passed &= require(core.setCueSelected(0, true) && core.setCueSelected(1, true) &&
                          core.setMasterGain(0.F) && core.setCueGain(0.F) && core.renderBuses(master, cue) &&
                          master == std::array<float, 2>{} && cue == std::array<float, 2>{} &&
                          std::isfinite(core.meters().deck[0].rms[0]) &&
                          core.meters().deck[0].peak[0] == maximum &&
                          core.meters().master.overrangeSamples == std::array<std::uint64_t, 2>{} &&
                          core.meters().cue.overrangeSamples == std::array<std::uint64_t, 2>{},
                      "muting a finite overflowing sum produced nonfinite buses or meter squares");
    passed &= require(core.seek(0, 0) && core.seek(1, 0) && core.setMasterGain(1.F) && core.setCueGain(1.F) &&
                          core.renderBuses(master, cue) && master == source && cue == source &&
                          core.meters().master.peak[0] == maximum && std::isfinite(core.meters().master.rms[0]) &&
                          core.meters().master.overrangeSamples == std::array<std::uint64_t, 2>{1, 1} &&
                          core.meters().cue.overrangeSamples == std::array<std::uint64_t, 2>{1, 1} &&
                          core.meters().deck[0].overrangeSamples == std::array<std::uint64_t, 2>{2, 2},
                      "bus saturation or meter overrange history did not describe the actual finite float output");
    return passed;
}

bool partitionsPreserveBusesAndMeterEnergy()
{
    bool passed = true;
    std::vector<float> a(4099 * 2), b(3001 * 2);
    for (std::size_t index = 0; index < a.size(); ++index)
        a[index] = static_cast<float>(static_cast<int>((index * 7) % 57) - 28) / 16.F;
    for (std::size_t index = 0; index < b.size(); ++index)
        b[index] = static_cast<float>(static_cast<int>((index * 11) % 33) - 16) / 16.F;
    Reader first{a}, second{b};
    RenderCore whole(a, b), partitioned(first.source(), second.source());
    for (auto* core : {&whole, &partitioned})
        passed &= require(core->setDeckGain(0, .5F) && core->setDeckGain(1, .25F) && core->setMasterGain(.5F) &&
                              core->setCueSelected(0, true) && core->setCueSelected(1, true) && core->setCueGain(.75F),
                          "partition bus setup failed");
    constexpr std::size_t frames = 4207;
    std::vector<float> expectedMaster(frames * 2), expectedCue(frames * 2), master(frames * 2), cue(frames * 2);
    passed &= require(whole.renderBuses(expectedMaster, expectedCue), "whole bus render failed");
    std::array<std::array<double, 2>, 4> energy{}, peaks{};
    constexpr std::array<std::size_t, 5> partitions{1, 7, 256, 1024, 1537};
    for (std::size_t offset = 0, index = 0; offset < frames; ++index) {
        const auto count = std::min(partitions[index % partitions.size()], frames - offset);
        passed &= require(partitioned.renderBuses(std::span(master).subspan(offset * 2, count * 2),
                                                  std::span(cue).subspan(offset * 2, count * 2)),
                          "partitioned bus render failed");
        const auto& meters = partitioned.meters();
        const std::array blocks{meters.deck[0], meters.deck[1], meters.master, meters.cue};
        for (std::size_t bus = 0; bus < blocks.size(); ++bus)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                energy[bus][channel] += blocks[bus].rms[channel] * blocks[bus].rms[channel] * count;
                peaks[bus][channel] = std::max(peaks[bus][channel], blocks[bus].peak[channel]);
            }
        offset += count;
    }
    const auto& reference = whole.meters();
    const auto& actual = partitioned.meters();
    const std::array referenceMeters{reference.deck[0], reference.deck[1], reference.master, reference.cue};
    const std::array actualMeters{actual.deck[0], actual.deck[1], actual.master, actual.cue};
    for (std::size_t bus = 0; bus < referenceMeters.size(); ++bus)
        for (std::size_t channel = 0; channel < 2; ++channel)
            passed &= require(peaks[bus][channel] == referenceMeters[bus].peak[channel] &&
                                  std::abs(std::sqrt(energy[bus][channel] / frames) - referenceMeters[bus].rms[channel]) < 1e-12 &&
                                  actualMeters[bus].overrangeSamples[channel] == referenceMeters[bus].overrangeSamples[channel],
                              "meter energy, peaks or cumulative overrange depended on render partitioning");
    passed &= require(sameBits(master, expectedMaster) && sameBits(cue, expectedCue) &&
                          partitioned.frameClock() == frames && partitioned.sourceFrame(0) == 4099 &&
                          partitioned.sourceFrame(1) == 3001 && first.largestRequest <= 1024 && second.largestRequest <= 1024,
                      "bus samples, bounded stream reads or independent EOF clocks changed across partitions");
    return passed;
}
} // namespace

int main()
{
    const bool independent = independentBusesAndPreFaderMeters();
    const bool transport = selectionAndTransportCueAreIndependent();
    const bool source = sourceReadsStarvationAndRmsSilence();
    const bool invalid = invalidCallsAndCommandsPreserveState();
    const bool overflow = finiteOverflowAndMeterSquares();
    const bool partitions = partitionsPreserveBusesAndMeterEnergy();
    return independent && transport && source && invalid && overflow && partitions ? 0 : 1;
}

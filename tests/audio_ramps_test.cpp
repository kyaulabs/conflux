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
#include <stdexcept>
#include <vector>

namespace {
using conflux::audio::AudioReadResult;
using conflux::audio::AudioSource;
using conflux::audio::RenderCore;
using Command = RenderCore::Command;
using Type = Command::Type;

bool require(bool value, const char* message)
{
    if (!value) std::cerr << message << '\n';
    return value;
}
std::vector<float> constant(std::size_t frames, float left = 1.F, float right = -.5F)
{
    std::vector<float> result(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        result[frame * 2] = left;
        result[frame * 2 + 1] = right;
    }
    return result;
}
struct Output { std::vector<float> master, cue; };
Output render(RenderCore& core, std::size_t frames)
{
    Output result{std::vector<float>(frames * 2), std::vector<float>(frames * 2)};
    if (!core.renderBuses(result.master, result.cue)) throw std::runtime_error("valid ramp render failed");
    return result;
}
bool sameBits(std::span<const float> first, std::span<const float> second)
{
    if (first.size() != second.size()) return false;
    for (std::size_t i = 0; i < first.size(); ++i)
        if (std::bit_cast<std::uint32_t>(first[i]) != std::bit_cast<std::uint32_t>(second[i])) return false;
    return true;
}
bool channelEquals(std::span<const float> actual, std::span<const float> left, std::span<const float> right)
{
    if (actual.size() != left.size() * 2 || left.size() != right.size()) return false;
    for (std::size_t frame = 0; frame < left.size(); ++frame)
        if (std::abs(actual[frame * 2] - left[frame]) > 1e-7F ||
            std::abs(actual[frame * 2 + 1] - right[frame]) > 1e-7F) return false;
    return true;
}

struct Reader {
    std::span<const float> pcm;
    std::size_t calls = 0, discontinuities = 0, largestRequest = 0;
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    bool ready = true, failed = false;
    std::uint64_t expected = 0;
    bool badPosition = false;
    AudioSource source() noexcept { return {pcm.size() / 2, nullptr, this, read, discontinuity}; }
    static AudioReadResult read(void* context, std::uint64_t first, std::span<float> output) noexcept
    {
        auto& self = *static_cast<Reader*>(context);
        ++self.calls;
        self.largestRequest = std::max(self.largestRequest, output.size() / 2);
        if (first != self.expected || first > self.pcm.size() / 2) {
            self.badPosition = true;
            return {0, true};
        }
        if (!self.ready) return {0, self.failed};
        const auto count = std::min({self.limit, output.size() / 2, self.pcm.size() / 2 - std::size_t(first)});
        std::copy_n(self.pcm.data() + first * 2, count * 2, output.data());
        self.expected += count;
        return {count, self.failed};
    }
    static void discontinuity(void* context, std::uint64_t first) noexcept
    {
        auto& self = *static_cast<Reader*>(context);
        ++self.discontinuities;
        self.expected = first;
    }
};

bool legacyAndConfiguration()
{
    constexpr std::array<float, 12> pcm{-0.F, 0.F, 2.75F, -3.25F, .25F, -.5F, -0.F, -.125F, 1.F, -1.F, 0.F, -0.F};
    RenderCore legacy(pcm), explicitZero(pcm);
    bool passed = require(explicitZero.configureRamps(0), "zero ramp configuration rejected");
    passed &= require(sameBits(render(legacy, 6).master, pcm) && sameBits(render(explicitZero, 6).master, pcm),
                      "zero ramps changed unity float samples, signed zero or headroom");
    const auto input = constant(64);
    RenderCore configured(input);
    std::array<float, 3> odd{};
    passed &= require(!configured.render(odd) && configured.render({}) && configured.configureRamps(5) &&
                          configured.configureRamps(48000) && !configured.configureRamps(48001) &&
                          configured.configureRamps(4), "invalid/empty render or configuration bound changed state");
    render(configured, 1);
    passed &= require(!configured.configureRamps(0), "ramp configuration changed after nonempty rendering");
    return passed;
}

bool gainRamps()
{
    bool passed = true;
    const auto pcm = constant(128);
    RenderCore core(pcm);
    passed &= require(core.setCueSelected(0, true) && core.configureRamps(4), "ramp cue setup failed");
    const auto initial = render(core, 4);
    constexpr std::array<float, 4> inL{.25F, .5F, .75F, 1.F}, inR{-.125F, -.25F, -.375F, -.5F};
    passed &= require(channelEquals(initial.master, inL, inR), "initial transport fade is not four frame linear");
    core.setDeckGain(0, 0.F); core.setMasterGain(.5F); core.setCueGain(0.F);
    const auto mixed = render(core, 4);
    constexpr std::array<float, 4> masterL{.65625F, .375F, .15625F, 0.F};
    constexpr std::array<float, 4> masterR{-.328125F, -.1875F, -.078125F, 0.F};
    constexpr std::array<float, 4> cueL{.75F, .5F, .25F, 0.F}, cueR{-.375F, -.25F, -.125F, 0.F};
    passed &= require(channelEquals(mixed.master, masterL, masterR) && channelEquals(mixed.cue, cueL, cueR) &&
                          core.deckGain() == 0.F && core.masterGain() == .5F && core.cueGain() == 0.F,
                      "independent deck/master/cue ramp products or requested targets differ");
    RenderCore interrupted(pcm);
    interrupted.configureRamps(4); render(interrupted, 4);
    interrupted.setDeckGain(0, 0.F);
    render(interrupted, 1); // Last applied deck gain is .75.
    interrupted.setDeckGain(0, 1.F);
    interrupted.setDeckGain(0, .5F); // No intervening samples: anchor remains .75.
    auto first = render(interrupted, 2);
    interrupted.setDeckGain(0, .5F); // Duplicate target cannot extend the ramp.
    auto second = render(interrupted, 2);
    first.master.insert(first.master.end(), second.master.begin(), second.master.end());
    constexpr std::array<float, 4> interruptedL{.6875F, .625F, .5625F, .5F};
    constexpr std::array<float, 4> interruptedR{-.34375F, -.3125F, -.28125F, -.25F};
    passed &= require(channelEquals(first.master, interruptedL, interruptedR),
                      "interrupted/repeated targets reanchored to a requested rather than emitted gain");
    RenderCore selection(pcm);
    selection.configureRamps(5); render(selection, 5);
    selection.setCueSelected(0, true);
    const auto on = render(selection, 5);
    selection.setCueSelected(0, false);
    const auto off = render(selection, 5);
    constexpr std::array<float, 5> onL{.2F, .4F, .6F, .8F, 1.F}, onR{-.1F, -.2F, -.3F, -.4F, -.5F};
    constexpr std::array<float, 5> offL{.8F, .6F, .4F, .2F, 0.F}, offR{-.4F, -.3F, -.2F, -.1F, 0.F};
    passed &= require(channelEquals(on.cue, onL, onR) && channelEquals(off.cue, offL, offR),
                      "headphone selection did not use its own five-frame send ramp");
    passed &= require(on.master == constant(5) && off.master == constant(5), "cue selection changed the master bus");
    return passed;
}

bool transportAndCue()
{
    bool passed = true;
    auto pcm = constant(128);
    for (std::size_t frame = 16; frame < pcm.size() / 2; ++frame) {
        pcm[frame * 2] = -1.F; pcm[frame * 2 + 1] = .25F;
    }
    Reader reader{pcm};
    RenderCore core(reader.source());
    core.configureRamps(4); render(core, 4);
    passed &= require(core.setPlaying(0, false) && !core.isDeckSilentForReplacement(0), "pause lost its pending tail");
    const auto cursor = core.sourceFrame(), reads = reader.calls;
    auto tail = render(core, 2);
    core.setPlaying(0, false);
    const auto rest = render(core, 2);
    tail.master.insert(tail.master.end(), rest.master.begin(), rest.master.end());
    constexpr std::array<float, 4> tailL{.75F, .5F, .25F, 0.F}, tailR{-.375F, -.25F, -.125F, 0.F};
    passed &= require(channelEquals(tail.master, tailL, tailR) && core.sourceFrame() == cursor && reader.calls == reads &&
                          core.isDeckSilentForReplacement(0), "pause tail read source, advanced cursor or restarted on duplicate pause");
    core.seek(32); core.setPlaying(0, true);
    const auto started = render(core, 4);
    constexpr std::array<float, 4> startL{-.25F, -.5F, -.75F, -1.F}, startR{.0625F, .125F, .1875F, .25F};
    passed &= require(channelEquals(started.master, startL, startR) && core.sourceFrame() == 36,
                      "seek/play fade changed source delivery positions");
    const auto discontinuities = reader.discontinuities;
    core.seek(36);
    passed &= require(render(core, 1).master == constant(1, -1.F, .25F) && reader.discontinuities == discontinuities,
                      "same-cursor seek restarted a fade or invalidated source cache");
    core.seek(0);
    const auto sought = render(core, 4);
    constexpr std::array<float, 4> seekL{-.5F, 0.F, .5F, 1.F}, seekR{.0625F, -.125F, -.3125F, -.5F};
    passed &= require(channelEquals(sought.master, seekL, seekR) && core.sourceFrame() == 4,
                      "seek did not crossfade from last emitted stereo sample to exact new frames");
    core.cuePress(0);
    passed &= require(channelEquals(render(core, 4).master, tailL, tailR) && core.sourceFrame() == 0 && !core.isPlaying(),
                      "back-cue did not hold exact cue through its decay");
    core.cueRelease(0); core.cuePress(0);
    auto audition = render(core, 2);
    core.setPlaying(0, true); core.cueRelease(0);
    const auto latched = render(core, 2);
    audition.master.insert(audition.master.end(), latched.master.begin(), latched.master.end());
    constexpr std::array<float, 4> fadeL{.25F, .5F, .75F, 1.F}, fadeR{-.125F, -.25F, -.375F, -.5F};
    passed &= require(channelEquals(audition.master, fadeL, fadeR) && core.sourceFrame() == 4 && core.isPlaying() &&
                          !core.isCueHeld() && !core.isCuePreviewing(), "PLAY latch restarted an active preview fade");
    auto extreme = constant(12, std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
    for (std::size_t frame = 8; frame < 12; ++frame) {
        extreme[frame * 2] = .125F; extreme[frame * 2 + 1] = -0.F;
    }
    RenderCore finite(extreme);
    finite.configureRamps(4); render(finite, 4); finite.seek(8);
    const auto transitioned = render(finite, 4);
    passed &= require(std::ranges::all_of(transitioned.master, [](float sample) { return std::isfinite(sample); }) &&
                          sameBits(std::span(transitioned.master).last(2), std::span(extreme).last(2)),
                      "large transition anchor overflowed or erased exact small/signed-zero endpoint");
    return passed && require(!reader.badPosition && reader.largestRequest <= 1024, "ramped source violated read position/bounds");
}

bool eofStarvationAndInvalidOutput()
{
    bool passed = true;
    const auto shortPcm = constant(6);
    RenderCore eof(shortPcm);
    eof.configureRamps(4); render(eof, 6);
    constexpr std::array<float, 4> tailL{.75F, .5F, .25F, 0.F}, tailR{-.375F, -.25F, -.125F, 0.F};
    passed &= require(channelEquals(render(eof, 4).master, tailL, tailR) && eof.sourceFrame() == 6 && eof.isPlaying(),
                      "EOF tail advanced source or cleared persistent playing");
    const auto pcm = constant(128);
    Reader reader{pcm};
    RenderCore core(reader.source());
    core.configureRamps(4); render(core, 4);
    reader.ready = false;
    const auto stalled = render(core, 2);
    constexpr std::array<float, 2> stallL{.75F, .5F}, stallR{-.375F, -.25F};
    passed &= require(channelEquals(stalled.master, stallL, stallR) && core.sourceFrame() == 4 &&
                          core.starvedFrames() == 2 && core.isBuffering(), "starvation did not fade while holding undelivered source frames");
    reader.ready = true;
    const auto recovery = render(core, 4);
    constexpr std::array<float, 4> recoverL{.625F, .75F, .875F, 1.F}, recoverR{-.3125F, -.375F, -.4375F, -.5F};
    passed &= require(channelEquals(recovery.master, recoverL, recoverR) && core.sourceFrame() == 8 && !core.isBuffering(),
                      "recovery did not reanchor to the last emitted tail sample");
    core.setDeckGain(0, 0.F);
    const auto clock = core.frameClock(), source = core.sourceFrame(), calls = reader.calls;
    std::array<float, 8> shared{};
    std::array<float, 3> odd{};
    passed &= require(!core.render(odd) && !core.renderBuses(shared, shared) && core.renderBuses({}, {}) &&
                          !core.setDeckGain(0, std::numeric_limits<float>::quiet_NaN()) &&
                          !core.setMasterGain(std::numeric_limits<float>::infinity()) && !core.setCueGain(-.1F) &&
                          core.frameClock() == clock && core.sourceFrame() == source && reader.calls == calls,
                      "invalid/empty output consumed transitions, source reads or clocks");
    const auto gain = render(core, 1);
    passed &= require(gain.master == constant(1, .75F, -.375F), "invalid output consumed a pending gain step");
    reader.failed = true; reader.limit = 2;
    render(core, 4);
    passed &= require(core.readFailed() && core.isBuffering(), "partial source failure lost read diagnostics");
    return passed;
}

bool jogAndReplacement()
{
    bool passed = true;
    const auto a = constant(128, 2.F, -2.F), b = constant(128, .25F, -.5F), replacement = constant(16, .75F, .125F);
    Reader old{a}, next{replacement};
    RenderCore core(old.source(), AudioSource::fromMemory(b));
    core.setDeckGain(0, .5F); core.setDeckGain(1, .25F); core.setMasterGain(.75F); core.setCueGain(.5F);
    core.setCueSelected(0, true); core.setCueFrame(0, 3); core.configureRamps(4);
    render(core, 8);
    passed &= require(!core.replaceSource(0, next.source()) && !core.isDeckSilentForReplacement(2),
                      "replacement accepted an active target or invalid slot");
    old.failed = true; old.limit = 0; render(core, 1);
    passed &= require(core.readFailed() && core.starvedFrames() == 1, "replacement error/reset fixture did not stall");
    core.setPlaying(0, false); render(core, 4);
    const auto peer = core.sourceFrame(1), clock = core.frameClock(), oldReads = old.calls;
    const auto overrange = core.meters().deck[0].overrangeSamples;
    passed &= require(core.isDeckSilentForReplacement(0) && !core.replaceSource(0, {}) &&
                          !core.replaceSource(0, AudioSource{16}) && !core.replaceSource(2, next.source()),
                      "replacement accepted invalid or empty source geometry");
    passed &= require(core.replaceSource(0, next.source()) && core.sourceFrame() == 0 && core.sourceFrames() == 16 &&
                          !core.isPlaying() && core.cueFrame() == 0 && !core.isCueSelected() && core.deckGain() == 1.F &&
                          !core.isBuffering() && !core.readFailed() && core.starvedFrames() == 0 &&
                          core.sourceFrame(1) == peer && core.isPlaying(1) && core.frameClock() == clock &&
                          core.masterGain() == .75F && core.cueGain() == .5F &&
                          core.meters().deck[0].overrangeSamples == overrange,
                      "source swap changed peer/global/clock/history or failed target reset");
    render(core, 3);
    passed &= require(old.calls == oldReads && next.calls == 0 && core.sourceFrame(1) == peer + 3,
                      "retired or paused new source was read, or peer stopped during replacement");
    core.setPlaying(0, true); render(core, 4);
    passed &= require(next.calls > 0 && core.sourceFrame() == 4 && old.calls == oldReads,
                      "replacement source did not activate independently");
    core.setPlaying(0, false); render(core, 4);
    core.setCueFrame(0, 2);
    passed &= require(core.jog(0, std::numeric_limits<std::int64_t>::max()) && core.sourceFrame() == 16 &&
                          core.jog(0, std::numeric_limits<std::int64_t>::min()) && core.sourceFrame() == 0 &&
                          core.jog(0, 7) && core.jog(0, -2) && core.sourceFrame() == 5 &&
                          core.cueFrame() == 2 && !core.isPlaying() && !core.jog(2, 1),
                      "signed jog overflow/clamping or paused/stored-cue invariants failed");
    core.seek(0, 2); core.cuePress(0); render(core, 1);
    Command command; command.type = Type::Jog; command.deltaFrames = 0;
    passed &= require(core.isCuePreviewing() && core.apply(command) && !core.isCuePreviewing() &&
                          core.isCueHeld() && !core.isPlaying() && core.cueFrame() == 2,
                      "zero positional jog did not cancel preview while retaining paired release state");
    core.cueRelease(0);
    RenderCore unloaded(false);
    passed &= require(unloaded.isDeckSilentForReplacement(0) && unloaded.replaceSource(0, AudioSource::fromMemory(replacement)) &&
                          !unloaded.isPlaying() && !unloaded.jog(1, 0), "unloaded slot replacement/jog policy failed");
    return passed;
}

bool partitionParity()
{
    std::vector<float> a(256 * 2), b(a.size());
    for (std::size_t frame = 0; frame < 256; ++frame) {
        a[frame * 2] = float(int(frame % 11) - 5) / 4;
        a[frame * 2 + 1] = float(int(frame % 7) - 3) / 8;
        b[frame * 2] = float(int(frame % 9) - 4) / 8;
        b[frame * 2 + 1] = float(int(frame % 13) - 6) / 4;
    }
    Reader readerA{a}, readerB{b};
    RenderCore whole(a, b), split(a, b), streamed(readerA.source(), readerB.source());
    constexpr std::array<std::size_t, 17> events{0, 7, 9, 13, 18, 23, 27, 33, 40, 41, 45, 47, 48, 55, 70, 73, 80};
    auto execute = [&](RenderCore& core, bool partitioned) {
        core.setCueSelected(0, true); core.configureRamps(5);
        Output result;
        for (std::size_t event = 0; event + 1 < events.size(); ++event) {
            switch (events[event]) {
            case 7: core.setDeckGain(0, .2F); break;
            case 9: core.setCueSelected(1, true); break;
            case 13: core.setMasterGain(.5F); core.setCueGain(.6F); break;
            case 18: core.seek(0, 80); break;
            case 23: core.setPlaying(1, false); break;
            case 27: core.jog(0, -3); break;
            case 33: core.setPlaying(1, true); break;
            case 40: core.cuePress(0); break;
            case 41: core.cueRelease(0); break;
            case 45: core.cuePress(0); break;
            case 47: core.setPlaying(0, true); break;
            case 48: core.cueRelease(0); break;
            case 55: core.setDeckGain(0, 1.F); break;
            case 70: core.setPlaying(0, false); core.setPlaying(1, false); break;
            case 73: core.seek(0, 12); break;
            }
            auto remaining = events[event + 1] - events[event];
            for (std::size_t part = 0; remaining; ++part) {
                const auto frames = partitioned ? std::min(remaining, std::array<std::size_t, 3>{1, 2, 3}[part % 3]) : remaining;
                const auto block = render(core, frames);
                result.master.insert(result.master.end(), block.master.begin(), block.master.end());
                result.cue.insert(result.cue.end(), block.cue.begin(), block.cue.end());
                remaining -= frames;
            }
        }
        return result;
    };
    const auto expected = execute(whole, false), actual = execute(split, true), streamedOutput = execute(streamed, true);
    return require(sameBits(expected.master, actual.master) && sameBits(expected.cue, actual.cue) &&
                       sameBits(expected.master, streamedOutput.master) && sameBits(expected.cue, streamedOutput.cue) &&
                       whole.frameClock() == 80 && split.frameClock() == 80 && streamed.frameClock() == 80 &&
                       whole.sourceFrame(0) == split.sourceFrame(0) && split.sourceFrame(0) == streamed.sourceFrame(0) &&
                       whole.sourceFrame(1) == split.sourceFrame(1) && split.sourceFrame(1) == streamed.sourceFrame(1) &&
                       whole.meters().master.overrangeSamples == split.meters().master.overrangeSamples &&
                       whole.meters().cue.overrangeSamples == split.meters().cue.overrangeSamples &&
                       !readerA.badPosition && !readerB.badPosition,
                   "fixed-event gain/transport/jog/CUE ramps changed across partitions or memory/stream sources");
}
} // namespace

int main()
{
    try {
        const bool legacy = legacyAndConfiguration(), gains = gainRamps(), transport = transportAndCue();
        const bool unavailable = eofStarvationAndInvalidOutput(), replacement = jogAndReplacement(), partitions = partitionParity();
        return legacy && gains && transport && unavailable && replacement && partitions ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

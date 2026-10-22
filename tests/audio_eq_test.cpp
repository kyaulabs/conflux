// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/ChannelEq.hpp"
#include "audio/RenderCore.hpp"
#include "audio/StemBankSource.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace conflux::audio;
using Command = RenderCore::Command;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool bits(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
void same(std::span<const float> a, std::span<const float> b, const char* message)
{
    require(a.size() == b.size(), message);
    for (std::size_t i = 0; i < a.size(); ++i) require(bits(a[i], b[i]), message);
}
std::vector<float> pcm(std::size_t frames, double offset = 0)
{
    std::vector<float> result(frames * 2);
    for (std::size_t i = 0; i < frames; ++i)
        for (std::size_t channel = 0; channel < 2; ++channel)
            for (const double hz : {40., 1000., 12000.})
                result[i * 2 + channel] += float(.02 * std::sin(2. * std::numbers::pi * hz *
                    (double(i) + offset + double(channel) * .37) / 48000.));
    return result;
}
struct Reader {
    std::span<const float> input;
    std::size_t calls = 0, largest = 0;
    AudioSource source() noexcept { return {input.size() / 2, nullptr, this, read, nullptr}; }
    static AudioReadResult read(void* context, std::uint64_t frame, std::span<float> output) noexcept
    {
        auto& self = *static_cast<Reader*>(context);
        ++self.calls; self.largest = std::max(self.largest, output.size() / 2);
        const auto count = std::min(output.size() / 2, self.input.size() / 2 - std::size_t(frame));
        std::copy_n(self.input.data() + frame * 2, count * 2, output.data());
        return {count, false};
    }
};
struct Buses { std::vector<float> master, cue; };
Buses render(RenderCore& core, std::size_t frames)
{
    Buses output{std::vector<float>(frames * 2), std::vector<float>(frames * 2)};
    require(core.renderBuses(output.master, output.cue), "EQ bus render failed");
    return output;
}

void neutralMemoryAndCallbackPaths()
{
    auto a = pcm(4099), b = pcm(4099, 31.);
    a[0] = -0.F; a[1] = 0.F; a[4] = 2.5F; a[5] = -3.25F;
    Reader reader{a};
    RenderCore memory(a), callback(reader.source());
    require(memory.setCueSelected(0, true) && callback.setCueSelected(0, true), "Neutral cue setup failed");
    const auto first = render(memory, 4099), second = render(callback, 4099);
    same(first.master, a, "Neutral memory EQ changed raw PCM bits/headroom");
    same(first.cue, a, "Neutral cue changed raw PCM bits");
    same(first.master, second.master, "Neutral callback EQ differs from memory EQ");
    same(first.cue, second.cue, "Neutral callback cue differs from memory cue");
    require(reader.calls == 5 && reader.largest <= 1024 && callback.sourceFrame() == 4099,
            "EQ duplicated/broadened callback source reads or changed its clock");
    RenderCore pair(a, b);
    const auto mixed = render(pair, 4099);
    for (std::size_t i = 0; i < a.size(); ++i)
        require(bits(mixed.master[i], float(double(a[i]) + b[i])), "Neutral two-deck sum changed");
}

void equalizedDecksFeedMetersAndPrefaderCue()
{
    const auto a = pcm(2048), b = pcm(2048, 47.);
    for (const bool callback : {false, true}) {
        Reader ra{a}, rb{b};
        RenderCore core(callback ? ra.source() : AudioSource::fromMemory(a),
                        callback ? rb.source() : AudioSource::fromMemory(b));
        ChannelEq eqA, eqB;
        constexpr std::array<float, 3> gainsA{-18.F, 6.F, -7.F}, gainsB{4.F, -12.F, 3.F};
        for (std::size_t band = 0; band < 3; ++band)
            require(core.setEqGainDb(0, band, gainsA[band]) && eqA.setGain(band, gainsA[band], 0) &&
                    core.setEqGainDb(1, band, gainsB[band]) && eqB.setGain(band, gainsB[band], 0), "Routing EQ setup failed");
        require(core.setDeckGain(0, .5F) && core.setDeckGain(1, .25F) && core.setMasterGain(.5F) &&
                core.setCrossfader(.75F) && core.setCueGain(.25F) && core.setCueSelected(0, true) &&
                core.setCueFrame(0, 17), "Routing controls failed");
        const auto output = render(core, 2048);
        std::array<std::array<double, 2>, 2> peak{}, energy{};
        for (std::size_t frame = 0; frame < 2048; ++frame) {
            const std::array filtered{eqA.process(a[frame * 2], a[frame * 2 + 1]),
                                      eqB.process(b[frame * 2], b[frame * 2 + 1])};
            for (std::size_t channel = 0; channel < 2; ++channel) {
                require(bits(output.master[frame * 2 + channel],
                             float((double(filtered[0][channel]) * .25 + double(filtered[1][channel]) * .25) * .5)) &&
                        bits(output.cue[frame * 2 + channel], float(double(filtered[0][channel]) * .25)),
                        "EQ was applied after channel/crossfader/master gain or omitted from prefader cue");
                for (std::size_t deck = 0; deck < 2; ++deck) {
                    peak[deck][channel] = std::max(peak[deck][channel], std::abs(double(filtered[deck][channel])));
                    energy[deck][channel] += double(filtered[deck][channel]) * filtered[deck][channel];
                }
            }
        }
        for (std::size_t deck = 0; deck < 2; ++deck)
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(core.meters().deck[deck].peak[channel] == peak[deck][channel] &&
                        std::abs(core.meters().deck[deck].rms[channel] - std::sqrt(energy[deck][channel] / 2048.)) < 1e-12,
                        "Deck meters did not measure post-EQ prefader PCM");
        require(core.sourceFrame(0) == 2048 && core.sourceFrame(1) == 2048 && core.frameClock() == 2048 &&
                core.cueFrame(0) == 17 && !core.isCueSelected(1), "EQ disturbed peer, CUE or transport state");
    }
}

void stemMixPrecedesChannelEq()
{
    std::array<std::vector<float>, 4> leaves;
    std::array<AudioSource, 4> sources;
    for (std::size_t stem = 0; stem < 4; ++stem) {
        leaves[stem] = pcm(257, double(stem) * 19.);
        sources[stem] = AudioSource::fromMemory(leaves[stem]);
    }
    StemBankSource bank(sources);
    constexpr std::array<float, 4> gains{.25F, .5F, .75F, 1.F};
    RenderCore core(bank.sourceRef()); ChannelEq eq;
    for (std::size_t stem = 0; stem < 4; ++stem) require(core.setStemGain(0, stem, gains[stem]), "Stem setup failed");
    require(core.setEqGainDb(0, 1, -20.F) && eq.setGain(1, -20.F, 0), "Bank EQ setup failed");
    const auto output = render(core, 257);
    for (std::size_t frame = 0; frame < 257; ++frame) {
        std::array<float, 2> raw{};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            double sum = 0.;
            for (std::size_t stem = 0; stem < 4; ++stem) sum += double(leaves[stem][frame * 2 + channel]) * gains[stem];
            raw[channel] = float(sum);
        }
        const auto expected = eq.process(raw[0], raw[1]);
        same(std::span(output.master).subspan(frame * 2, 2), expected, "Channel EQ did not follow the current stem mix");
    }
    require(bank.sourceFrame() == 257 && core.stemGain(0, 2) == .75F, "EQ rewrote stem gains or source alignment");
}

void commandsAreAtomicAndPartitionsPreserveRamps()
{
    const auto a = pcm(2048), b = pcm(2048, 11.);
    RenderCore whole(a, b), split(a, b);
    Command command; command.type = Command::Type::EqGain; command.deck = 1; command.band = 1; command.eqGainDb = -12.F;
    for (auto* core : {&whole, &split})
        require(core->configureRamps(240) && core->setEqGainDb(0, 2, 6.F) && core->apply(command) &&
                core->setCueSelected(0, true) && core->eqGainDb(1, 1) == -12.F, "EQ command/ramp setup failed");
    for (const float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -26.01F, 6.01F}) {
        command.eqGainDb = value;
        require(!split.apply(command), "Invalid queued EQ gain was accepted");
    }
    command.eqGainDb = 1.F; command.band = 3;
    require(!split.apply(command) && !split.setEqGainDb(2, 0, 1.F) && split.eqGainDb(1, 1) == -12.F,
            "Invalid EQ band/deck changed accepted state");
    const auto expected = render(whole, 2048);
    Buses actual;
    for (const std::size_t frames : {1U, 7U, 64U, 257U, 1024U, 695U}) {
        const auto part = render(split, frames);
        actual.master.insert(actual.master.end(), part.master.begin(), part.master.end());
        actual.cue.insert(actual.cue.end(), part.cue.begin(), part.cue.end());
    }
    same(actual.master, expected.master, "Render partitions/rejected EQ commands changed master ramp PCM");
    same(actual.cue, expected.cue, "Render partitions/rejected EQ commands changed cue ramp PCM");
    require(split.frameClock() == 2048 && split.sourceFrame(0) == 2048 && split.sourceFrame(1) == 2048,
            "EQ commands advanced clocks outside rendering");
}

void filteredPauseFadeAndChannelOwnedSourceReset()
{
    auto a = pcm(1024); std::vector<float> b(2048 * 2, 0.F);
    for (std::size_t i = 0; i < 2048; ++i) b[i * 2 + 1] = .03125F;
    Reader reader{a}; RenderCore core(reader.source(), AudioSource::fromMemory(b));
    require(core.configureRamps(4) && core.setEqGainDb(0, 0, 6.F) && core.setCueSelected(0, true), "Fade setup failed");
    const auto active = render(core, 64); const auto calls = reader.calls;
    require(core.setPlaying(0, false), "EQ deck pause failed");
    const auto faded = render(core, 8);
    for (std::size_t frame = 0; frame < 8; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const float expected = frame < 4 ? float(double(active.cue[126 + channel]) * (1. - double(frame + 1) / 4.)) : 0.F;
            require(faded.cue[frame * 2 + channel] == expected, "Pause reapplied EQ to decay or exposed the free-running filter tail");
        }
    require(reader.calls == calls && core.sourceFrame(0) == 64 && core.sourceFrame(1) == 72 &&
            core.isDeckSilentForReplacement(0), "Filtered pause disturbed peer/read/retirement state");
    auto next = pcm(256, 7.); std::fill_n(next.begin(), 64 * 2, 0.F);
    require(core.replaceSource(0, AudioSource::fromMemory(next)) && core.eqGainDb(0, 0) == 6.F &&
            core.setCueSelected(0, true) && core.setPlaying(0, true), "Source swap discarded channel EQ settings");
    const auto replaced = render(core, 256); ChannelEq reference;
    require(reference.setGain(0, 6.F, 0), "Replacement reference setup failed");
    for (std::size_t frame = 0; frame < 256; ++frame) {
        const auto expected = reference.process(next[frame * 2], next[frame * 2 + 1]);
        same(std::span(replaced.cue).subspan(frame * 2, 2), expected, "Replacement leaked prior-source EQ history or lost its setting");
    }
    require(core.sourceFrame(0) == 256 && core.sourceFrame(1) == 328 && core.frameClock() == 328,
            "EQ source swap reset or stalled peer/output clocks");
    RenderCore unloaded(a);
    require(unloaded.setEqGainDb(1, 2, -9.F) && unloaded.eqGainDb(1, 2) == -9.F && unloaded.sourceFrame(1) == 0 &&
            unloaded.replaceSource(1, AudioSource::fromMemory(next)) && unloaded.eqGainDb(1, 2) == -9.F,
            "Valid unloaded channel did not retain its independent EQ setting through installation");
}
} // namespace
int main()
{
    try {
        neutralMemoryAndCallbackPaths(); equalizedDecksFeedMetersAndPrefaderCue(); stemMixPrecedesChannelEq();
        commandsAreAtomicAndPartitionsPreserveRamps(); filteredPauseFadeAndChannelOwnedSourceReset();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Channel EQ routing, clocks, commands, ramps and source transitions passed\n";
    return 0;
}

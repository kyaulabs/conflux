// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"
#include "audio/StemBankSource.hpp"

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
using conflux::audio::StemBankSource;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function&& function)
{
    try { function(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Malformed stem bank was accepted");
}

struct Signals {
    std::array<std::vector<float>, 4> stems;
    explicit Signals(std::size_t frames)
    {
        for (std::size_t stem = 0; stem < stems.size(); ++stem) {
            stems[stem].resize(frames * 2);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                stems[stem][frame * 2] = float((frame * 3 + stem * 7) % 37) / 16.F;
                stems[stem][frame * 2 + 1] = -float((frame * 5 + stem * 11) % 41) / 8.F;
            }
        }
    }
    std::array<AudioSource, 4> sources() const
    {
        return {AudioSource::fromMemory(stems[0]), AudioSource::fromMemory(stems[1]),
                AudioSource::fromMemory(stems[2]), AudioSource::fromMemory(stems[3])};
    }
    float mixed(std::size_t frame, std::size_t channel,
                const std::array<float, 4>& gains = {1.F, 1.F, 1.F, 1.F}) const
    {
        double sum = 0;
        for (std::size_t stem = 0; stem < stems.size(); ++stem)
            sum += double(stems[stem][frame * 2 + channel]) * gains[stem];
        return static_cast<float>(std::clamp(sum, -double(std::numeric_limits<float>::max()),
                                            double(std::numeric_limits<float>::max())));
    }
};

// Enforces sequential consumption. A bank that throws away a faster stem's
// surplus must either read an old position or produce the wrong numbered frame.
struct Reader {
    std::span<const float> pcm;
    std::uint64_t cursor = 0, lastDiscontinuity = 0;
    std::size_t calls = 0, discontinuities = 0, badPositions = 0, largestRequest = 0;
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    bool ready = true, failed = false, nonfinite = false, overreport = false;

    AudioSource source() noexcept { return {pcm.size() / 2, nullptr, this, read, discontinuity}; }
    static AudioReadResult read(void* context, std::uint64_t frame, std::span<float> output) noexcept
    {
        auto& self = *static_cast<Reader*>(context);
        ++self.calls;
        self.largestRequest = std::max(self.largestRequest, output.size() / 2);
        if (frame != self.cursor || frame > self.pcm.size() / 2 || output.size() % 2) {
            ++self.badPositions;
            return {0, true};
        }
        if (!self.ready) return {0, self.failed};
        const auto frames = std::min({self.limit, output.size() / 2,
                                     self.pcm.size() / 2 - static_cast<std::size_t>(frame)});
        std::copy_n(self.pcm.data() + frame * 2, frames * 2, output.data());
        if (self.nonfinite && frames) output[0] = std::numeric_limits<float>::quiet_NaN();
        self.cursor += frames;
        return {self.overreport ? output.size() / 2 + 1 : frames, self.failed};
    }
    static void discontinuity(void* context, std::uint64_t frame) noexcept
    {
        auto& self = *static_cast<Reader*>(context);
        self.lastDiscontinuity = self.cursor = frame;
        ++self.discontinuities;
    }
};

struct Readers {
    std::array<Reader, 4> readers;
    explicit Readers(const Signals& signals)
    {
        for (std::size_t stem = 0; stem < readers.size(); ++stem) readers[stem].pcm = signals.stems[stem];
    }
    std::array<AudioSource, 4> sources() noexcept
    { return {readers[0].source(), readers[1].source(), readers[2].source(), readers[3].source()}; }
};

void validateDescriptorsAndControl()
{
    Signals signals(32);
    const auto valid = signals.sources();
    rejects([] { StemBankSource bank(std::array<AudioSource, 4>{}); });
    for (std::size_t bad = 0; bad < 4; ++bad) {
        auto sources = valid;
        sources[bad] = {};
        rejects([&] { StemBankSource bank(sources); });
        sources = valid;
        --sources[bad].frames;
        rejects([&] { StemBankSource bank(sources); });
        sources = valid;
        sources[bad].read = Reader::read;
        rejects([&] { StemBankSource bank(sources); });
        sources = valid;
        sources[bad].memory = nullptr;
        rejects([&] { StemBankSource bank(sources); });
    }
    auto overflow = valid;
    for (auto& source : overflow) source.frames = std::numeric_limits<std::uint64_t>::max();
    rejects([&] { StemBankSource bank(overflow); });
    StemBankSource bank(valid);
    require(bank.frames() == 32 && bank.sourceFrame() == 0, "Bank geometry/cursor differ");
    for (std::size_t stem = 0; stem < 4; ++stem) {
        require(bank.gain(stem) == 1.F && bank.setGain(stem, 0.F) && bank.setGain(stem, 1.F),
                "Stem gain default/boundary differs");
        for (const float value : {-1.F, 1.01F, std::numeric_limits<float>::infinity(),
                                  std::numeric_limits<float>::quiet_NaN()})
            require(!bank.setGain(stem, value) && bank.gain(stem) == 1.F,
                    "Invalid stem gain changed its target");
    }
    require(!bank.setGain(4, 1.F) && !bank.configureRamps(48001), "Invalid stem/ramp limit accepted");
    const auto source = bank.sourceRef();
    std::array<float, 3> malformed{19.F, 19.F, 19.F};
    require(source.read(source.context, 0, malformed).failed &&
                malformed == std::array<float, 3>{19.F, 19.F, 19.F} && bank.sourceFrame() == 0,
            "Malformed destination was written or changed the cursor");
    std::array<float, (StemBankSource::blockFrames + 1) * 2> oversized{};
    oversized.fill(27.F);
    require(source.read(source.context, 0, oversized).failed && bank.sourceFrame() == 0 &&
                std::ranges::all_of(oversized, [](float value) { return value == 27.F; }),
            "Oversized bank request exceeded fixed storage or changed its destination");
    require(!source.read(source.context, 0, {}).failed && bank.configureRamps(4),
            "Invalid/empty reads froze prestream ramp configuration");
    std::array<float, 2> frame{};
    require(source.read(source.context, 0, frame).frames == 1 && !bank.configureRamps(0),
            "Ramp configuration changed after the first nonempty read");
    const auto before = bank.sourceFrame();
    source.discontinuity(source.context, 33);
    require(bank.sourceFrame() == before, "Out-of-range discontinuity changed bank position");
}

void retainedSurplusAndMutedAlignment()
{
    Signals signals(64);
    Readers inputs(signals);
    inputs.readers[0].limit = 5;
    StemBankSource bank(inputs.sources());
    require(bank.setGain(3, 0.F), "Cannot mute vocals");
    const auto source = bank.sourceRef();
    std::array<float, 16> output{};
    auto result = source.read(source.context, 0, output);
    require(result.frames == 5 && !result.failed && bank.sourceFrame() == 5,
            "Unequal readers did not commit their common prefix");
    for (std::size_t frame = 0; frame < 5; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(output[frame * 2 + channel] == signals.mixed(frame, channel, {1.F, 1.F, 1.F, 0.F}),
                    "Partial bank output mixed different source positions");
    inputs.readers[0].ready = false;
    output.fill(91.F);
    result = source.read(source.context, 5, output);
    require(result.frames == 0 && !result.failed && bank.sourceFrame() == 5 &&
                std::ranges::all_of(output, [](float value) { return value == 91.F; }),
            "A missing stem advanced the bank or wrote a stale suffix");
    const std::array calls{inputs.readers[1].calls, inputs.readers[2].calls, inputs.readers[3].calls};
    inputs.readers[0].ready = true;
    inputs.readers[0].limit = std::numeric_limits<std::size_t>::max();
    require(bank.setGain(3, 1.F), "Cannot unmute vocals");
    result = source.read(source.context, 5, output);
    require(result.frames == 8 && !result.failed && bank.sourceFrame() == 13,
            "Bank did not recover the missing prefix");
    for (std::size_t frame = 0; frame < 8; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(output[frame * 2 + channel] == signals.mixed(frame + 5, channel),
                    "Retained surplus or a previously muted stem lost alignment");
    for (std::size_t stem = 0; stem < 4; ++stem) {
        require(inputs.readers[stem].cursor == 13 && !inputs.readers[stem].badPositions &&
                    inputs.readers[stem].discontinuities == 0,
                "Normal recovery reread old positions or manufactured a seek");
        if (stem) require(inputs.readers[stem].calls == calls[stem - 1],
                          "Recovery refetched a faster stem instead of retaining its surplus");
    }
}

void stalledBankLeavesPeerAndClocksRunning()
{
    Signals signals(64);
    Readers inputs(signals);
    inputs.readers[0].limit = 3;
    StemBankSource bank(inputs.sources());
    std::array<float, 64> peer{};
    peer.fill(.5F);
    RenderCore core(bank.sourceRef(), AudioSource::fromMemory(peer));
    std::array<float, 16> output{};
    require(core.render(output) && core.sourceFrame(0) == 3 && core.sourceFrame(1) == 8 &&
                core.frameClock() == 8 && core.starvedFrames(0) == 5 && core.isBuffering(0),
            "Partial bank stalled its peer or advanced beyond its common prefix");
    for (std::size_t frame = 0; frame < 8; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(output[frame * 2 + channel] ==
                        (frame < 3 ? signals.mixed(frame, channel) + .5F : .5F),
                    "Partial bank did not silence only its unavailable suffix");
    inputs.readers[0].ready = false;
    require(core.render(output) && core.sourceFrame(0) == 3 && core.sourceFrame(1) == 16 &&
                core.frameClock() == 16 && core.starvedFrames(0) == 13 && !core.readFailed(0) &&
                std::ranges::all_of(output, [](float value) { return value == .5F; }),
            "Unavailable bank interrupted the healthy deck or skipped source frames");
    inputs.readers[0].ready = true;
    inputs.readers[0].limit = std::numeric_limits<std::size_t>::max();
    require(core.render(output) && core.sourceFrame(0) == 11 && core.sourceFrame(1) == 24 &&
                core.frameClock() == 24 && core.starvedFrames(0) == 13 && !core.isBuffering(0),
            "Bank recovery did not preserve its exact stopped source cursor");
    for (std::size_t frame = 0; frame < 8; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(output[frame * 2 + channel] == signals.mixed(frame + 3, channel) + .5F,
                    "Recovered bank resumed a stale or independently advanced stem");
}

void sumsBusesHeadroomAndBounds()
{
    Signals signals(2053);
    Readers inputs(signals);
    StemBankSource bank(inputs.sources());
    constexpr std::array gains{.5F, 0.F, .75F, 1.F};
    for (std::size_t stem = 0; stem < 4; ++stem) require(bank.setGain(stem, gains[stem]), "Gain setup failed");
    std::vector<float> peer(2053 * 2, .25F);
    RenderCore core(bank.sourceRef(), AudioSource::fromMemory(peer));
    require(core.setDeckGain(0, .5F) && core.setDeckGain(1, .25F) && core.setMasterGain(.5F) &&
                core.setCueSelected(0, true) && core.setCueGain(.25F), "Bus setup failed");
    std::vector<float> master(2059 * 2), cue(master.size());
    require(core.renderBuses(master, cue), "Stem bank bus render failed");
    for (std::size_t frame = 0; frame < master.size() / 2; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const double mixed = frame < 2053 ? signals.mixed(frame, channel, gains) : 0;
            const float expectedMaster = frame < 2053 ? float((mixed * .5 + .25 * .25) * .5) : 0.F;
            require(master[frame * 2 + channel] == expectedMaster && cue[frame * 2 + channel] == float(mixed * .25),
                    "Stem order/gains, master or pre-fader cue sum differs");
        }
    require(core.sourceFrame(0) == 2053 && core.sourceFrame(1) == 2053 && core.frameClock() == 2059 &&
                !core.isBuffering(0) && !core.readFailed(0) && core.starvedFrames(0) == 0,
            "Known bank EOF was counted as starvation or advanced beyond its end");
    for (const auto& reader : inputs.readers)
        require(reader.largestRequest <= 1024 && !reader.badPositions, "Bank exceeded its read bound or cursor");

    const float maximum = std::numeric_limits<float>::max();
    const std::array<float, 2> extreme{maximum, -maximum};
    const auto value = AudioSource::fromMemory(extreme);
    StemBankSource saturating({value, value, value, value});
    const auto saturated = saturating.sourceRef();
    std::array<float, 2> output{};
    require(saturated.read(saturated.context, 0, output).frames == 1 &&
                output[0] == maximum && output[1] == -maximum,
            "Stem summation overflowed finite float representation or clipped normal headroom");
}

void gainRampsArePartitionInvariant()
{
    Signals signals(64);
    StemBankSource whole(signals.sources()), split(signals.sources());
    constexpr std::array targets{0.F, .5F, .25F, 1.F};
    for (auto* bank : {&whole, &split}) {
        require(bank->configureRamps(8), "Cannot configure bank ramps");
        for (std::size_t stem = 0; stem < 4; ++stem) require(bank->setGain(stem, targets[stem]), "Ramp target failed");
    }
    RenderCore first(whole.sourceRef()), second(split.sourceRef());
    std::array<float, 128> expected{}, partitioned{};
    require(first.render(std::span(expected).first(6)) && second.render(std::span(partitioned).first(6)),
            "Initial ramp prefix failed");
    require(whole.setGain(0, .25F) && split.setGain(0, .25F), "Mid-ramp target change failed");
    require(first.render(std::span(expected).subspan(6)), "Whole ramp suffix failed");
    constexpr std::array<std::size_t, 4> partitions{1, 7, 3, 11};
    for (std::size_t offset = 6, index = 0; offset < partitioned.size(); ++index) {
        const auto count = std::min(partitions[index % partitions.size()] * 2, partitioned.size() - offset);
        require(second.render(std::span(partitioned).subspan(offset, count)), "Partitioned ramp failed");
        offset += count;
    }
    for (std::size_t sample = 0; sample < expected.size(); ++sample)
        require(std::bit_cast<std::uint32_t>(expected[sample]) == std::bit_cast<std::uint32_t>(partitioned[sample]),
                "Gain ramp depends on render partitioning");
    for (std::size_t frame = 0; frame < 3; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const double fraction = double(frame + 1) / 8;
            double oracle = 0;
            for (std::size_t stem = 0; stem < 4; ++stem)
                oracle += signals.stems[stem][frame * 2 + channel] *
                          ((1.0 - fraction) + fraction * targets[stem]);
            require(expected[frame * 2 + channel] == float(oracle), "Ramp does not advance linearly per committed frame");
        }

    Readers inputs(signals);
    inputs.readers[0].ready = false;
    StemBankSource stalled(inputs.sources());
    require(stalled.configureRamps(4), "Stalled ramp setup failed");
    for (std::size_t stem = 0; stem < 4; ++stem) require(stalled.setGain(stem, 0.F), "Stalled gain target failed");
    const auto source = stalled.sourceRef();
    std::array<float, 2> output{};
    require(source.read(source.context, 0, output).frames == 0, "Missing stem did not stall gain fixture");
    inputs.readers[0].ready = true;
    require(source.read(source.context, 0, output).frames == 1, "Stalled gain fixture did not recover");
    for (std::size_t channel = 0; channel < 2; ++channel)
        require(output[channel] == signals.mixed(0, channel, {.75F, .75F, .75F, .75F}),
                "A zero-prefix read advanced gain ramps");
}

void commonTransportAndEof()
{
    Signals signals(64);
    Readers inputs(signals);
    StemBankSource bank(inputs.sources());
    RenderCore core(bank.sourceRef());
    std::array<float, 8> output{};
    const auto check = [&](std::size_t first) {
        require(core.render(output), "Transport render failed");
        for (std::size_t frame = 0; frame < 4; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(output[frame * 2 + channel] == signals.mixed(first + frame, channel),
                        "Transport exposed different stem positions");
    };
    inputs.readers[0].limit = 2;
    require(core.render(output) && core.sourceFrame() == 2 && core.isBuffering(),
            "Transport fixture did not retain faster stems' partial-read surplus");
    inputs.readers[0].limit = std::numeric_limits<std::size_t>::max();
    require(core.seek(20) && core.seek(37) && core.seek(20), "Repeated bank seek failed");
    for (const auto& reader : inputs.readers)
        require(reader.cursor == 20 && reader.discontinuities == 3, "Seek did not reach every stem");
    check(20);
    require(core.jog(0, -9), "Bank jog failed");
    check(15);
    require(core.setPlaying(0, false) && core.seek(10) && core.cuePress(0) && core.cueRelease(0) &&
                core.cuePress(0) && core.isCuePreviewing(0), "Bank CUE preview setup failed");
    check(10);
    require(core.cueRelease(0) && core.sourceFrame() == 10 && bank.sourceFrame() == 10,
            "CUE release did not return the complete bank");
    require(core.setPlaying(0, true) && core.seek(64), "Bank EOF seek failed");
    const auto calls = inputs.readers[0].calls;
    const auto starvedBeforeEof = core.starvedFrames();
    require(core.render(output) && core.sourceFrame() == 64 && !core.isBuffering() &&
                core.starvedFrames() == starvedBeforeEof &&
                inputs.readers[0].calls == calls &&
                std::ranges::all_of(output, [](float value) { return value == 0.F; }),
            "Bank EOF reread stems, leaked staged audio or counted known-end silence as starvation");
    require(!core.seek(65) && core.sourceFrame() == 64, "Invalid seek changed bank position");
    for (const auto& reader : inputs.readers)
        require(reader.cursor == 64 && reader.lastDiscontinuity == 64 && !reader.badPositions,
                "Transport left a stem at another cursor");
}

void failuresDoNotLeakStaleAudioOrBlockPeer()
{
    for (const int fault : {0, 1, 2}) {
        Signals signals(64);
        Readers inputs(signals);
        auto& bad = inputs.readers[2];
        if (fault == 0) { bad.failed = true; bad.limit = 2; }
        if (fault == 1) bad.nonfinite = true;
        if (fault == 2) bad.overreport = true;
        StemBankSource bank(inputs.sources());
        // Even a muted malformed provider must not silently pass validation.
        const std::array gains{1.F, 1.F, fault == 1 ? 0.F : 1.F, 1.F};
        require(bank.setGain(2, gains[2]), "Failure gain setup failed");
        const std::array<float, 12> peer{.5F, -.25F, .5F, -.25F, .5F, -.25F,
                                        .5F, -.25F, .5F, -.25F, .5F, -.25F};
        RenderCore core(bank.sourceRef(), AudioSource::fromMemory(peer));
        require(core.setCueSelected(0, true), "Failure cue setup failed");
        std::array<float, 12> master{}, cue{};
        require(core.renderBuses(master, cue), "Bank failure invalidated the complete render");
        const std::size_t prefix = fault == 0 ? 2 : 0;
        require(core.readFailed(0) && core.isBuffering(0) && core.sourceFrame(0) == prefix &&
                    core.sourceFrame(1) == 6 && core.starvedFrames(0) == 6 - prefix && core.frameClock() == 6,
                "Malformed stem advanced the common cursor or blocked the healthy peer");
        for (std::size_t frame = 0; frame < 6; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const float mixed = frame < prefix ? signals.mixed(frame, channel, gains) : 0.F;
                require(std::isfinite(master[frame * 2 + channel]) &&
                            master[frame * 2 + channel] == mixed + peer[frame * 2 + channel] &&
                            cue[frame * 2 + channel] == mixed,
                        "A failed stem leaked stale/nonfinite PCM or silently substituted a full mix");
            }
        bad.failed = bad.nonfinite = bad.overreport = false;
        bad.limit = std::numeric_limits<std::size_t>::max();
        require(core.seek(0, 20), "Failed bank could not begin a fresh seek generation");
        require(core.renderBuses(master, cue) && core.sourceFrame(0) == 26,
                "Fresh bank seek did not clear per-generation failure/staging");
        for (std::size_t frame = 0; frame < 6; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(cue[frame * 2 + channel] == signals.mixed(frame + 20, channel, gains),
                        "Failure recovery leaked another generation's staged PCM");
    }
}
} // namespace

int main()
{
    try {
        validateDescriptorsAndControl();
        retainedSurplusAndMutedAlignment();
        stalledBankLeavesPeerAndClocksRunning();
        sumsBusesHeadroomAndBounds();
        gainRampsArePartitionInvariant();
        commonTransportAndEof();
        failuresDoNotLeakStaleAudioOrBlockPeer();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

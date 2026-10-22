// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/TempoProcessor.hpp"

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
#include <tuple>
#include <vector>

namespace {
using Processor = conflux::audio::TempoProcessor;
using Config = Processor::Config;
using Phase = Processor::Phase;
using Backend = Processor::Backend;
constexpr std::size_t rate = Processor::sampleRate;
constexpr std::size_t sourceFrames = 8192;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
bool bits(std::span<const float> a, std::span<const float> b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](float x, float y) {
        return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
    });
}
auto countKey(const Processor::Counts& n)
{
    return std::tuple{n.inputFramesConsumed, n.preferredStartPad, n.padInputFrames, n.startDelayFrames,
        n.discardedStartFrames, n.rawOutputFrames, n.outputFrames, n.processCalls, n.endOfInputCalls,
        n.retrieveCalls, n.ratioApplications, n.fullyDrained};
}
struct Snapshot {
    std::uint64_t generation;
    Config config;
    Backend backend;
    Phase phase;
    Processor::Counts counts;
    explicit Snapshot(const Processor& p) : generation(p.generation()), config(p.requestedConfig()),
        backend(p.backend()), phase(p.phase()), counts(p.counts()) {}
    bool unchanged(const Processor& p) const
    {
        const auto now = p.requestedConfig();
        return generation == p.generation() && config.channels == now.channels &&
            config.speedPercent == now.speedPercent && config.keyLock == now.keyLock &&
            backend == p.backend() && phase == p.phase() && countKey(counts) == countKey(p.counts());
    }
};
template<class Function> void rejects(Function&& function)
{
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Invalid call was accepted or threw the wrong exception category");
}
template<class Function> void unchangedRejection(Processor& p, std::vector<float>& output, Function&& function)
{
    const Snapshot before(p);
    const auto saved = output;
    rejects(function);
    require(before.unchanged(p) && bits(saved, output), "Rejected call mutated state, counters, generation or caller output");
}
std::vector<float> generated(std::size_t frames, std::size_t channels)
{
    std::vector<float> pcm(frames * channels);
    constexpr std::array<double, 3> frequencies{375, 613, 997};
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (channels == 2) {
            pcm[frame * 2] = float(.3 * std::sin(2 * std::numbers::pi * 375 * frame / rate));
            pcm[frame * 2 + 1] = float(.2 * std::sin(2 * std::numbers::pi * 997 * frame / rate + .37));
        } else for (std::size_t pair = 0; pair < frequencies.size(); ++pair) {
            const auto value = float(.3 * std::sin(2 * std::numbers::pi * frequencies[pair] * frame / rate + .17 * pair));
            pcm[frame * channels + pair * 2] = value;
            pcm[frame * channels + pair * 2 + 1] = -.5F * value;
        }
    }
    return pcm;
}

// One fixed-input stream driver: retain every unconsumed frame and every
// returned output frame. No duration estimate, repair, shift or output crop.
struct Stream {
    Processor& processor;
    const std::uint64_t generation;
    const std::size_t channels;
    std::vector<float> pcm;
    Processor::Step last{};
    std::size_t calls = 0;
    bool partial = false, backpressure = false, internalOnly = false;
    explicit Stream(Processor& p) : processor(p), generation(p.generation()), channels(p.requestedConfig().channels) {}
    Processor::Step step(std::span<const float> input, std::size_t capacity, bool eof = false)
    {
        require(++calls <= 200000, "Bounded stream driver made too many calls without finishing");
        std::array<float, Processor::maximumBlockFrames * 8> output{};
        output.fill(91.F);
        const auto before = processor.counts();
        const auto result = processor.process(generation, input, std::span(output).first(capacity * channels), eof);
        const auto after = processor.counts();
        require(result.generation == generation && result.phase == processor.phase() &&
                    result.inputFramesUsed <= input.size() / channels && result.outputFrames <= capacity &&
                    after.inputFramesConsumed - before.inputFramesConsumed == result.inputFramesUsed &&
                    after.outputFrames - before.outputFrames == result.outputFrames &&
                    result.requestedConfiguration >= result.appliedInputConfiguration,
                "Step identity, actual partial counts, phase or configuration revisions differ");
        require(after.processCalls - before.processCalls <= (processor.backend() == Backend::SampleRateBestQuality ? 2U : 1U) &&
                    after.retrieveCalls - before.retrieveCalls <= 1,
                "A stream step hid an unbounded DSP/retrieve loop");
        const auto valid = std::span(output).first(result.outputFrames * channels);
        require(std::ranges::all_of(valid, [](float value) { return std::isfinite(value); }), "Stream returned nonfinite PCM");
        pcm.insert(pcm.end(), valid.begin(), valid.end());
        require(pcm.size() <= (12U * rate * channels) && after.outputFrames == pcm.size() / channels,
                "Stream output is missing, duplicated or exceeded the fixture safety cap");
        partial |= !input.empty() && result.inputFramesUsed < input.size() / channels;
        backpressure |= !input.empty() && !result.inputFramesUsed && result.outputFrames && before.inputFramesConsumed;
        internalOnly |= result.internalProgress && !result.inputFramesUsed && !result.outputFrames;
        last = result;
        return result;
    }
    void gap(std::size_t capacity = 1)
    {
        const auto before = processor.counts();
        const auto result = step({}, capacity);
        const auto after = processor.counts();
        require(!result.inputFramesUsed && after.inputFramesConsumed == before.inputFramesConsumed &&
                    after.endOfInputCalls == before.endOfInputCalls && !after.fullyDrained &&
                    result.phase != Phase::Draining && result.phase != Phase::Finished,
                "Temporary input gap was mistaken for EOF or advanced the real source");
    }
    void feed(std::span<const float> input, std::size_t& position, std::size_t capacity)
    {
        const auto total = input.size() / channels;
        require(position < total, "Fixture tried to feed beyond its real input");
        const auto count = std::min<std::size_t>(Processor::maximumBlockFrames, total - position);
        position += step(input.subspan(position * channels, count * channels), capacity).inputFramesUsed;
    }
    void finish(std::span<const float> input, std::size_t position = 0)
    {
        constexpr std::array<std::size_t, 4> capacities{1, 17, 257, 1024};
        while (position < input.size() / channels) feed(input, position, capacities[calls % capacities.size()]);
        while (processor.phase() != Phase::Finished) step({}, capacities[calls % capacities.size()], true);
        const auto counts = processor.counts();
        require(counts.inputFramesConsumed == input.size() / channels && counts.outputFrames == pcm.size() / channels &&
                    counts.rawOutputFrames == counts.outputFrames + counts.discardedStartFrames && counts.fullyDrained,
                "Finished stream did not retain all actual backend output and input counts");
        if (processor.backend() == Backend::RubberBandFiner)
            require(counts.padInputFrames == counts.preferredStartPad && counts.discardedStartFrames == counts.startDelayFrames &&
                        counts.endOfInputCalls == 1 && counts.retrieveCalls,
                    "R3 padding, startup compensation or real final drain is incomplete");
        const auto before = processor.counts();
        const auto repeated = step({}, 1, true);
        require(!repeated.inputFramesUsed && !repeated.outputFrames && !repeated.internalProgress &&
                    countKey(before) == countKey(processor.counts()), "Finished empty EOF was not idempotent");
    }
};
void unityBitsAndPartialConsumption()
{
    constexpr std::array<float, 10> values{0.F, -0.F, 3.25F, -4.5F, .125F, -.375F,
        std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::min(), std::numeric_limits<float>::denorm_min()};
    for (const std::size_t channels : {2U, 8U}) for (const bool lock : {false, true}) {
        std::vector<float> input(257 * channels);
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = values[i % values.size()];
        Processor processor({channels, 0, lock});
        Stream stream(processor); stream.gap();
        std::size_t position = 0;
        stream.feed(input, position, 1);
        require(position == 1 && stream.last.outputFrames == 1 && stream.partial && processor.backend() == Backend::UnityBypass,
                "Unity did not expose exact one-frame consumption under tiny-output backpressure");
        stream.gap(); stream.finish(input, position);
        const auto n = processor.counts();
        require(bits(input, stream.pcm) && !n.processCalls && !n.retrieveCalls && !n.endOfInputCalls && !n.padInputFrames &&
                    !n.startDelayFrames && !n.discardedStartFrames,
                "Exact unity changed raw bits/headroom/signed zero or entered a DSP backend");
    }
}
void invalidCallsAndExplicitEof()
{
    for (const auto config : {Config{4, 0, false}, Config{2, 6.001, false},
                              Config{2, std::numeric_limits<double>::quiet_NaN(), true}})
        rejects([&] { Processor invalid(config); });
    Processor processor({2, 0, false});
    std::vector<float> output(2048, 91.F), input(2048, .25F), tooBig(2050, .25F);
    const auto generation = processor.generation();
    unchangedRejection(processor, output, [&] { (void)processor.process(generation, {}, output, true); });
    unchangedRejection(processor, output, [&] { (void)processor.process(generation, tooBig, output); });
    unchangedRejection(processor, output, [&] { (void)processor.process(generation, std::span(input).first(3), output); });
    unchangedRejection(processor, output, [&] { (void)processor.process(generation, input, std::span(output).first(3)); });
    unchangedRejection(processor, output, [&] { (void)processor.process(generation, input, {}); });
    std::vector<float> tooBigOutput(2050, 91.F);
    unchangedRejection(processor, tooBigOutput, [&] { (void)processor.process(generation, input, tooBigOutput); });
    unchangedRejection(processor, output, [&] { (void)processor.process(generation + 1, input, output); });
    unchangedRejection(processor, output, [&] { (void)processor.process(generation, output, output); });
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        auto malformed = input; malformed.back() = bad;
        unchangedRejection(processor, output, [&] { (void)processor.process(generation, malformed, output); });
    }
    unchangedRejection(processor, output, [&] { (void)processor.reset({2, -6.001, false}); });
    unchangedRejection(processor, output, [&] { (void)processor.requestSpeed(generation, 6); });
    Stream stream(processor); stream.gap();
    std::size_t position = 0; stream.feed(input, position, 1);
    unchangedRejection(processor, output, [&] { (void)processor.requestSpeed(generation, 6); });
    unchangedRejection(processor, output, [&] { (void)processor.process(generation, input, output, true); });
    stream.finish(input, position);
    unchangedRejection(processor, output, [&] { (void)processor.process(generation, {}, output, false); });
    unchangedRejection(processor, output, [&] { (void)processor.process(generation, input, output, true); });
}

void fixedRatesAndBackpressure()
{
    for (const std::size_t channels : {2U, 8U}) for (const bool lock : {false, true}) for (const double speed : {-6.0, 6.0}) {
        const auto input = generated(sourceFrames, channels);
        Processor processor({channels, speed, lock}); Stream stream(processor);
        stream.gap();
        std::size_t position = 0;
        // Insist on a real tiny-output pending interval before larger output
        // spans. Padding-only calls do not masquerade as real-input pressure.
        while (!stream.backpressure && position < sourceFrames) stream.feed(input, position, 1);
        require(stream.backpressure && stream.partial, "DSP fixture did not exercise real pending-output backpressure");
        stream.gap(1);
        stream.finish(input, position);
        const auto n = processor.counts();
        require(n.outputFrames > 3072 && std::ranges::any_of(stream.pcm, [](float value) { return std::abs(value) > .02F; }),
                "Finite DSP input produced no useful complete output");
        if (!lock) {
            const auto ideal = sourceFrames / (1 + speed / 100.0);
            require(std::abs(double(n.outputFrames) - ideal) <= 2, "SRC actual finite count differs from arithmetic resampling duration");
            double error = 0;
            for (std::size_t frame = 1536; frame + 1536 < n.outputFrames; ++frame)
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    constexpr std::array<double, 3> frequencies{375, 613, 997};
                    const auto frequency = channels == 2 ? (channel ? 997.0 : 375.0) : frequencies[std::min<std::size_t>(channel / 2, 2)];
                    const auto amplitude = channels == 2 ? (channel ? .2 : .3) : .3;
                    const auto phase = channels == 2 ? (channel ? .37 : 0) : .17 * (channel / 2);
                    double expected = amplitude * std::sin(2 * std::numbers::pi * frequency * (1 + speed / 100.0) * frame / rate + phase);
                    if (channels == 8) expected = channel >= 6 ? 0 : channel % 2 ? -.5 * expected : expected;
                    error = std::max(error, std::abs(double(stream.pcm[frame * channels + channel]) - expected));
                }
            require(error < 1e-3, "Best SRC lost independent tone timing, channel/role order, polarity or silence");
        }
        if (channels == 8)
            for (std::size_t frame = 0; frame < n.outputFrames; ++frame)
                require(std::abs(stream.pcm[frame * 8 + 6]) <= 1e-6F && std::abs(stream.pcm[frame * 8 + 7]) <= 1e-6F,
                        "Multichannel processing leaked active audio into the independent silent pair");
        // R3 geometry/drain/silent-lane checks are not a proportional-phase or
        // nominal-duration guarantee; those cannot be inferred from this API.
    }
}

void speedCrossingZeroAndGenerationReset()
{
    const auto input = generated(32768, 2);
    for (const bool lock : {false, true}) {
        Processor processor({2, -6, lock}); Stream old(processor);
        std::size_t position = 0;
        while (processor.counts().outputFrames < 256 || !old.last.pendingOutput || !processor.counts().inputFramesConsumed)
            old.feed(input, position, 1);
        std::vector<float> output(2048, 91.F), malformed(16, .25F);
        malformed.back() = std::numeric_limits<float>::quiet_NaN();
        unchangedRejection(processor, output, [&] { (void)processor.process(old.generation, malformed, output); });
        unchangedRejection(processor, output, [&] { (void)processor.reset({8, std::numeric_limits<double>::infinity(), true}); });
        unchangedRejection(processor, output, [&] { (void)processor.requestSpeed(old.generation, 6.001); });
        const auto backend = processor.backend();
        for (const double target : {0.0, 6.0}) {
            while (!old.last.pendingOutput) old.feed(input, position, 1);
            const auto previousApplied = old.last.appliedInputConfiguration;
            const auto applications = processor.counts().ratioApplications;
            const auto before = processor.counts();
            require(before.outputFrames >= 256 && before.inputFramesConsumed && old.last.pendingOutput,
                    "Ratio change requires delivered output and a real active pending stream");
            const auto revision = processor.requestSpeed(old.generation, target);
            require(revision > old.last.requestedConfiguration && processor.requestedConfig().speedPercent == target &&
                        processor.generation() == old.generation && processor.backend() == backend && countKey(before) == countKey(processor.counts()),
                    "Speed request silently reset/switched backend, touched data or did not expose requested state");
            while (old.last.pendingOutput) {
                old.gap(1);
                require(old.last.appliedInputConfiguration == previousApplied && processor.counts().ratioApplications == applications,
                        "Pending-output/gap drain claimed a new ratio had reached a real input call");
            }
            do { old.feed(input, position, 1); } while (!old.last.inputFramesUsed);
            require(old.last.requestedConfiguration == revision && old.last.appliedInputConfiguration == revision &&
                        processor.counts().ratioApplications == applications + 1 && processor.backend() == backend &&
                        processor.generation() == old.generation,
                    "Deferred ratio did not apply exactly before real input within the same generation/backend");
        }
        while (!old.last.pendingOutput) old.feed(input, position, 1);
        const auto prior = processor.generation();
        const auto next = processor.reset({8, 0, true});
        require(next == prior + 1 && !processor.counts().inputFramesConsumed && !processor.counts().outputFrames &&
                    !processor.counts().rawOutputFrames && processor.backend() == Backend::UnityBypass,
                "Explicit reset did not discard old pending data under a fresh generation");
        unchangedRejection(processor, output, [&] { (void)processor.process(prior, {}, output); });
        std::vector<float> fresh(257 * 8, -3.25F); fresh[0] = -0.F; fresh[1] = 0.F;
        Stream replacement(processor); replacement.finish(fresh);
        require(bits(fresh, replacement.pcm), "Old backend output leaked into reset unity stream");
        const auto third = processor.reset({2, -6, false});
        require(third == next + 1, "Successful reset reused/skipped its monotonic generation");
        Stream current(processor); std::size_t currentPosition = 0;
        while (!processor.counts().inputFramesConsumed) current.feed(input, currentPosition, 1);
        unchangedRejection(processor, output, [&] { (void)processor.requestSpeed(prior, 6); });
        current.finish(input, currentPosition);
    }
}

void actualR3FinalImpulseTailIsNotNominallyCropped()
{
    constexpr std::size_t frames = 2 * rate;
    std::vector<float> input(frames * 2);
    input[(frames - 1) * 2] = .5F; input[(frames - 1) * 2 + 1] = -.25F;
    Processor processor({2, -6, true}); Stream stream(processor); stream.finish(input);
    const auto nominal = std::size_t(std::ceil(frames / .94));
    require(processor.counts().outputFrames > nominal, "R3 tail was cropped to a nominal duration");
    double excessPeak = 0;
    for (std::size_t i = nominal * 2; i < stream.pcm.size(); ++i)
        excessPeak = std::max(excessPeak, std::abs(double(stream.pcm[i])));
    require(excessPeak > 1e-4, "Incremental EOF drain lost real final-impulse energy beyond the hypothetical boundary");
}
} // namespace

int main()
{
    try {
        unityBitsAndPartialConsumption();
        invalidCallsAndExplicitEof();
        fixedRatesAndBackpressure();
        speedCrossingZeroAndGenerationReset();
        actualR3FinalImpulseTailIsNotNominallyCropped();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Streaming tempo fixture failed: " << error.what() << '\n';
        return 1;
    }
}

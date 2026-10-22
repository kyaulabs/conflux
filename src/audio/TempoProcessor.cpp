// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/TempoProcessor.hpp"

#include <rubberband/RubberBandStretcher.h>
#include <samplerate.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace conflux::audio {
namespace {
using Processor = TempoProcessor;
using Stretcher = RubberBand::RubberBandStretcher;
constexpr std::size_t maximumChannels = 8;
constexpr std::size_t maximumCompensation = Processor::sampleRate;

void validateConfig(Processor::Config config)
{
    if ((config.channels != 2 && config.channels != 8) ||
        !std::isfinite(config.speedPercent) || config.speedPercent < -6 || config.speedPercent > 6)
        throw std::invalid_argument("Tempo stream requires 2/8 channels and finite speed [-6,6]");
}

Processor::Backend backendFor(Processor::Config config) noexcept
{
    return config.speedPercent == 0 ? Processor::Backend::UnityBypass :
        config.keyLock ? Processor::Backend::RubberBandFiner : Processor::Backend::SampleRateBestQuality;
}

double ratio(double speedPercent) noexcept { return 1.0 / (1.0 + speedPercent / 100.0); }

void add(std::uint64_t& counter, std::uint64_t amount)
{
    if (amount > std::numeric_limits<std::uint64_t>::max() - counter)
        throw std::runtime_error("Tempo stream counter exhausted");
    counter += amount;
}

struct Range { std::uintptr_t first, last; };
Range range(const float* data, std::size_t samples)
{
    const auto first = reinterpret_cast<std::uintptr_t>(data);
    const auto bytes = samples * sizeof(float); // Complete shape is already bounded.
    if ((samples && !data) || bytes > std::numeric_limits<std::uintptr_t>::max() - first)
        throw std::invalid_argument("Tempo stream has invalid sample range");
    return {first, first + bytes};
}
bool overlaps(Range a, Range b) noexcept
{
    return a.first != a.last && b.first != b.last && a.first < b.last && b.first < a.last;
}

struct DeleteSrc { void operator()(SRC_STATE* state) const noexcept { src_delete(state); } };
} // namespace

class TempoProcessor::Impl {
public:
    explicit Impl(Config config) : config(config), backend(backendFor(config)), appliedSpeedPercent(config.speedPercent)
    {
        if (backend == Backend::SampleRateBestQuality) {
            int error = 0;
            src.reset(src_new(SRC_SINC_BEST_QUALITY, static_cast<int>(config.channels), &error));
            if (!src || error)
                throw std::runtime_error(std::string("Cannot create Best tempo SRC: ") + src_strerror(error));
            if (const int code = src_set_ratio(src.get(), ratio(config.speedPercent)))
                throw std::runtime_error(std::string("Cannot initialise tempo SRC ratio: ") + src_strerror(code));
        } else if (backend == Backend::RubberBandFiner) {
            constexpr auto options = Stretcher::OptionProcessRealTime | Stretcher::OptionEngineFiner |
                Stretcher::OptionThreadingNever | Stretcher::OptionChannelsTogether;
            rubber = std::make_unique<Stretcher>(sampleRate, config.channels, options,
                                               ratio(config.speedPercent), 1.0);
            rubber->setDebugLevel(0);
            if (rubber->getEngineVersion() != 3 || rubber->getProcessSizeLimit() < maximumBlockFrames)
                throw std::runtime_error("Requested bounded Rubber Band R3 engine unavailable");
            rubber->setMaxProcessSize(maximumBlockFrames);
            counts.preferredStartPad = rubber->getPreferredStartPad();
            counts.startDelayFrames = rubber->getStartDelay();
            if (counts.preferredStartPad > maximumCompensation || counts.startDelayFrames > maximumCompensation)
                throw std::runtime_error("Tempo compensation exceeds the fixed one-second safety bound");
            padRemaining = counts.preferredStartPad;
            discardRemaining = counts.startDelayFrames;
            if (padRemaining) phase = Phase::Priming;
            for (std::size_t channel = 0; channel < config.channels; ++channel) {
                inputPointers[channel] = planarInput.data() + channel * maximumBlockFrames;
                outputPointers[channel] = planarOutput.data() + channel * maximumBlockFrames;
            }
        }
    }

    void validate(std::span<const float> input, std::span<float> output, bool eof) const
    {
        const auto channels = config.channels;
        if (input.size() % channels || input.size() / channels > maximumBlockFrames ||
            output.empty() || output.size() % channels || output.size() / channels > maximumBlockFrames)
            throw std::invalid_argument("Tempo stream requires bounded complete input/output frames");
        if (overlaps(range(input.data(), input.size()), range(output.data(), output.size())) ||
            std::ranges::any_of(input, [](float sample) { return !std::isfinite(sample); }))
            throw std::invalid_argument("Tempo stream input/output overlaps or input is nonfinite");
        if ((eof && (!input.empty() || !counts.inputFramesConsumed)) || (eofRequested && !eof) ||
            (phase == Phase::Finished && (!input.empty() || !eof)))
            throw std::invalid_argument("Tempo stream input/EOF differs from current stream state");
        if (phase == Phase::Failed) throw std::runtime_error("Tempo stream failed; explicit reset required");
    }

    void applyRequestedRatio()
    {
        if (appliedRevision == requestedRevision) return;
        if (backend == Backend::RubberBandFiner) rubber->setTimeRatio(ratio(config.speedPercent));
        // SRC_DATA.src_ratio performs the library's documented smooth change;
        // src_set_ratio here would instead force a different, abrupt policy.
        appliedSpeedPercent = config.speedPercent;
        appliedRevision = requestedRevision;
        add(counts.ratioApplications, 1);
    }

    int rubberAvailable()
    {
        const int available = rubber->available();
        if (available < -1 || (available == -1 && !rubberFinalSent))
            throw std::runtime_error("Rubber Band availability/finish state invalid");
        if (available == -1) {
            if (discardRemaining) throw std::runtime_error("Rubber Band ended before its reported start delay");
            counts.fullyDrained = true;
            phase = Phase::Finished;
        }
        return available;
    }

    Step rubberStep(std::span<const float> input, std::span<float> output)
    {
        Step step;
        const int available = rubberAvailable();
        if (available == -1) return step;
        if (available > 0) {
            const auto capacity = output.size() / config.channels;
            const auto wanted = std::min<std::uint64_t>(maximumBlockFrames, capacity + discardRemaining);
            const auto requested = std::min<std::size_t>(static_cast<std::size_t>(available), wanted);
            const auto actual = rubber->retrieve(outputPointers.data(), requested);
            add(counts.retrieveCalls, 1);
            if (!actual || actual > requested) throw std::runtime_error("Rubber Band retrieve made invalid progress");
            for (std::size_t channel = 0; channel < config.channels; ++channel)
                if (std::ranges::any_of(std::span(outputPointers[channel], actual),
                                      [](float sample) { return !std::isfinite(sample); }))
                    throw std::runtime_error("Rubber Band returned nonfinite output");
            const auto skipped = std::min<std::uint64_t>(actual, discardRemaining);
            discardRemaining -= skipped;
            add(counts.rawOutputFrames, actual);
            add(counts.discardedStartFrames, skipped);
            step.outputFrames = actual - skipped;
            for (std::size_t frame = 0; frame < step.outputFrames; ++frame)
                for (std::size_t channel = 0; channel < config.channels; ++channel)
                    output[frame * config.channels + channel] = outputPointers[channel][frame + skipped];
            add(counts.outputFrames, step.outputFrames);
            step.pendingOutput = rubberAvailable() > 0;
            return step; // Never accept new input in a pending-output step.
        }
        if (padRemaining) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(padRemaining, maximumBlockFrames));
            rubber->process(inputPointers.data(), count, false); // Zero-initialised until the first real feed.
            add(counts.processCalls, 1);
            add(counts.padInputFrames, count);
            padRemaining -= count;
            if (!padRemaining) phase = Phase::Running;
        } else if (eofRequested) {
            if (rubberFinalSent) throw std::runtime_error("Single-threaded Rubber Band stalled during final drain");
            rubber->process(inputPointers.data(), 0, true);
            rubberFinalSent = true;
            add(counts.processCalls, 1);
            add(counts.endOfInputCalls, 1);
        } else if (!input.empty()) {
            applyRequestedRatio();
            const auto count = input.size() / config.channels;
            for (std::size_t channel = 0; channel < config.channels; ++channel)
                for (std::size_t frame = 0; frame < count; ++frame)
                    planarInput[channel * maximumBlockFrames + frame] = input[frame * config.channels + channel];
            rubber->process(inputPointers.data(), count, false);
            add(counts.processCalls, 1);
            add(counts.inputFramesConsumed, count);
            step.inputFramesUsed = count;
        }
        step.pendingOutput = rubberAvailable() > 0;
        return step;
    }

    struct SrcProgress { std::size_t used, generated; };
    SrcProgress srcCall(std::span<const float> input, std::span<float> output, bool eof)
    {
        static constexpr float zero = 0.F; // Non-null empty EOF reaches SRC 0.2.2's padding path.
        SRC_DATA data{};
        data.data_in = input.empty() ? &zero : input.data();
        data.data_out = output.data();
        data.input_frames = static_cast<long>(input.size() / config.channels);
        data.output_frames = static_cast<long>(output.size() / config.channels);
        data.src_ratio = ratio(appliedSpeedPercent);
        data.end_of_input = eof;
        add(counts.processCalls, 1);
        if (eof) add(counts.endOfInputCalls, 1);
        const auto code = src_process(src.get(), &data);
        const auto error = code ? code : src_error(src.get());
        if (error) throw std::runtime_error(std::string("Best tempo SRC failed: ") + src_strerror(error));
        if (data.input_frames_used < 0 || data.input_frames_used > data.input_frames ||
            data.output_frames_gen < 0 || data.output_frames_gen > data.output_frames)
            throw std::runtime_error("Tempo SRC returned invalid input/output counts");
        const auto used = static_cast<std::size_t>(data.input_frames_used);
        const auto generated = static_cast<std::size_t>(data.output_frames_gen);
        if (std::ranges::any_of(output.first(generated * config.channels),
                              [](float sample) { return !std::isfinite(sample); }))
            throw std::runtime_error("Tempo SRC returned nonfinite output");
        add(counts.inputFramesConsumed, used);
        add(counts.rawOutputFrames, generated);
        add(counts.outputFrames, generated);
        return {used, generated};
    }

    Step srcStep(std::span<const float> input, std::span<float> output)
    {
        Step step;
        if (srcMayHaveOutput || eofRequested) {
            const auto progress = srcCall({}, output, eofRequested);
            if (progress.used) throw std::runtime_error("Tempo SRC consumed input during empty drain");
            step.outputFrames = progress.generated;
            if (progress.generated) {
                srcMayHaveOutput = step.pendingOutput = true;
                return step;
            }
            srcMayHaveOutput = false;
            if (eofRequested) {
                counts.fullyDrained = true;
                phase = Phase::Finished;
                return step;
            }
        }
        if (!input.empty()) {
            applyRequestedRatio();
            const auto progress = srcCall(input, output, false);
            if (!progress.used && !progress.generated) throw std::runtime_error("Tempo SRC made no progress on real input");
            step.inputFramesUsed = progress.used;
            step.outputFrames = progress.generated;
            srcMayHaveOutput = step.pendingOutput = true;
        }
        return step;
    }

    Config config;
    const Backend backend;
    Phase phase = Phase::Running;
    Counts counts;
    double appliedSpeedPercent;
    std::uint64_t requestedRevision = 1, appliedRevision = 1;
    std::uint64_t padRemaining = 0, discardRemaining = 0;
    bool eofRequested = false, rubberFinalSent = false, srcMayHaveOutput = false;
    std::unique_ptr<SRC_STATE, DeleteSrc> src;
    std::unique_ptr<Stretcher> rubber;
    std::array<float, maximumChannels * maximumBlockFrames> planarInput{}, planarOutput{};
    std::array<const float*, maximumChannels> inputPointers{};
    std::array<float*, maximumChannels> outputPointers{};
};

TempoProcessor::TempoProcessor(Config config)
{
    validateConfig(config);
    impl_ = std::make_unique<Impl>(config);
}
TempoProcessor::~TempoProcessor() = default;
TempoProcessor::Config TempoProcessor::requestedConfig() const noexcept { return impl_->config; }
TempoProcessor::Backend TempoProcessor::backend() const noexcept { return impl_->backend; }
TempoProcessor::Phase TempoProcessor::phase() const noexcept { return impl_->phase; }
TempoProcessor::Counts TempoProcessor::counts() const noexcept { return impl_->counts; }

std::uint64_t TempoProcessor::reset(Config config)
{
    validateConfig(config);
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        impl_->phase = Phase::Failed;
        throw std::runtime_error("Tempo stream generation exhausted");
    }
    try {
        auto replacement = std::make_unique<Impl>(config);
        impl_ = std::move(replacement);
        return ++generation_;
    } catch (...) {
        impl_->phase = Phase::Failed;
        throw;
    }
}

std::uint64_t TempoProcessor::requestSpeed(std::uint64_t generation, double speedPercent)
{
    auto config = impl_->config;
    config.speedPercent = speedPercent;
    validateConfig(config);
    if (generation != generation_ || impl_->phase != Phase::Running || !impl_->counts.inputFramesConsumed ||
        (impl_->backend == Backend::UnityBypass && config.speedPercent != 0))
        throw std::invalid_argument("Tempo ratio request needs current running non-transitioning stream");
    if (config.speedPercent == impl_->config.speedPercent) return impl_->requestedRevision;
    if (impl_->requestedRevision == std::numeric_limits<std::uint64_t>::max()) {
        impl_->phase = Phase::Failed;
        throw std::runtime_error("Tempo configuration revision exhausted");
    }
    impl_->config = config;
    return ++impl_->requestedRevision;
}

TempoProcessor::Step TempoProcessor::process(std::uint64_t generation,
    std::span<const float> input, std::span<float> output, bool endOfInput)
{
    if (generation != generation_) throw std::invalid_argument("Stale tempo stream generation");
    impl_->validate(input, output, endOfInput);
    const auto previous = impl_->counts;
    const auto previousPhase = impl_->phase;
    const bool previousPending = impl_->srcMayHaveOutput;
    Step step;
    try {
        if (impl_->phase != Phase::Finished) {
            if (endOfInput) {
                impl_->eofRequested = true;
                impl_->phase = Phase::Draining;
            }
            if (impl_->backend == Backend::RubberBandFiner) step = impl_->rubberStep(input, output);
            else if (impl_->backend == Backend::SampleRateBestQuality) step = impl_->srcStep(input, output);
            else if (endOfInput) {
                impl_->phase = Phase::Finished;
                impl_->counts.fullyDrained = true;
            } else {
                step.inputFramesUsed = step.outputFrames = std::min(input.size(), output.size()) / impl_->config.channels;
                std::copy_n(input.data(), step.outputFrames * impl_->config.channels, output.data());
                add(impl_->counts.inputFramesConsumed, step.inputFramesUsed);
                add(impl_->counts.rawOutputFrames, step.outputFrames);
                add(impl_->counts.outputFrames, step.outputFrames);
            }
        }
        step.internalProgress = step.inputFramesUsed || step.outputFrames || impl_->phase != previousPhase ||
            impl_->counts.processCalls != previous.processCalls || impl_->counts.retrieveCalls != previous.retrieveCalls ||
            previousPending != impl_->srcMayHaveOutput;
        step.generation = generation_;
        step.requestedConfiguration = impl_->requestedRevision;
        step.appliedInputConfiguration = impl_->appliedRevision;
        step.phase = impl_->phase;
        return step;
    } catch (...) {
        impl_->phase = Phase::Failed;
        throw;
    }
}

} // namespace conflux::audio

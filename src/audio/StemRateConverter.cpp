// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/StemRateConverter.hpp"

#include <samplerate.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace conflux::audio {
namespace {
constexpr double ratio = 48000.0 / 44100.0;
constexpr auto samplesPerBlock = StemRateConverter::blockFrames * StemRateConverter::channels;
constexpr auto maximumFrames = std::numeric_limits<std::size_t>::max() / (2 * sizeof(float));
struct DeleteState { void operator()(SRC_STATE* state) const noexcept { src_delete(state); } };
using State = std::unique_ptr<SRC_STATE, DeleteState>;

void checkStop(std::stop_token stop)
{
    if (stop.stop_requested()) throw std::runtime_error("Stem frame count cancelled");
}

State makeState()
{
    int error = 0;
    State result(src_new(SRC_SINC_BEST_QUALITY, 2, &error));
    if (!result || error)
        throw std::runtime_error(std::string("Cannot create stem sample-rate converter: ") + src_strerror(error));
    return result;
}

struct Progress { std::size_t used = 0, generated = 0; };
Progress convert(SRC_STATE* state, std::span<const float> input, std::span<float> output, bool eof)
{
    static constexpr float zero = 0;
    SRC_DATA data{};
    data.data_in = input.empty() ? &zero : input.data();
    data.data_out = output.data();
    data.input_frames = static_cast<long>(input.size() / 2);
    data.output_frames = static_cast<long>(output.size() / 2);
    data.src_ratio = ratio;
    data.end_of_input = eof;
    const auto error = src_process(state, &data);
    const auto fault = error ? error : src_error(state);
    if (fault)
        throw std::runtime_error(std::string("Stem sample-rate conversion failed: ") + src_strerror(fault));
    if (data.input_frames_used < 0 || data.input_frames_used > data.input_frames ||
        data.output_frames_gen < 0 || data.output_frames_gen > data.output_frames)
        throw std::runtime_error("Stem sample-rate converter returned invalid progress");
    const auto generated = static_cast<std::size_t>(data.output_frames_gen);
    if (std::ranges::any_of(output.first(generated * 2), [](float value) { return !std::isfinite(value); }))
        throw std::runtime_error("Converted stems contain non-finite samples");
    return {static_cast<std::size_t>(data.input_frames_used), generated};
}

struct Range { std::uintptr_t first, last; };
Range range(const float* data, std::size_t samples)
{
    const auto first = reinterpret_cast<std::uintptr_t>(data);
    const auto bytes = samples * sizeof(float); // Shape is bounded before this call.
    if ((samples && !data) || bytes > std::numeric_limits<std::uintptr_t>::max() - first)
        throw std::invalid_argument("Invalid stem sample span");
    return {first, first + bytes};
}
bool overlaps(Range left, Range right) noexcept
{
    return left.first != left.last && right.first != right.last &&
           left.first < right.last && right.first < left.last;
}
} // namespace

StemRateConverter::FramePlan StemRateConverter::countFrames(std::uint64_t inputFrames,
    std::uint64_t maximumOutputFrames, std::stop_token stop)
{
    if (!inputFrames || inputFrames > maximumFrames || !maximumOutputFrames || maximumOutputFrames > maximumFrames)
        throw std::invalid_argument("Stem count geometry is empty or unaddressable");
    checkStop(stop);
    auto state = makeState();
    std::array<float, samplesPerBlock> zeros{}, output{};
    auto remaining = inputFrames;
    std::uint64_t total = 0;
    for (;;) {
        checkStop(stop);
        const auto offered = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, blockFrames));
        const auto progress = convert(state.get(), std::span(zeros).first(offered * channels), output, !remaining);
        checkStop(stop);
        if (progress.generated > maximumOutputFrames - total)
            throw std::length_error("Stem output exceeds its counted frame budget");
        total += progress.generated;
        remaining -= progress.used;
        if (!offered && !progress.generated) {
            if (!total) throw std::runtime_error("Stem conversion produced no frames");
            return FramePlan(inputFrames, total);
        }
        if (offered && !progress.used && !progress.generated)
            throw std::runtime_error("Stem frame count made no progress");
    }
}

class StemRateConverter::Impl {
public:
    Impl(FramePlan plan, std::uint64_t target) : plan_(plan), outputFrame_(target)
    {
        if (target > plan.outputFrames()) throw std::invalid_argument("Stem seek exceeds counted EOF");
        const auto cell = target / outputGrid;
        const auto anchorCell = cell > prerollCells ? cell - prerollCells : 0;
        inputFrame_ = inputStart_ = anchorCell * inputGrid;
        generatedFrame_ = anchorCell * outputGrid;
        if (inputStart_ > plan.inputFrames() || generatedFrame_ > target)
            throw std::invalid_argument("Stem seek anchor exceeds input geometry");
        discard_ = target - generatedFrame_;
        for (auto& state : states_) state = makeState();
    }

    Step process(std::array<std::span<const float>, stems> input,
                 std::array<std::span<float>, stems> output, bool eof)
    {
        if (failed_) throw std::runtime_error("Stem converter generation has failed");
        const auto inputSamples = input.front().size(), outputSamples = output.front().size();
        if (inputSamples % channels || inputSamples > samplesPerBlock ||
            !outputSamples || outputSamples % channels || outputSamples > samplesPerBlock)
            throw std::invalid_argument("Stem conversion requires bounded stereo spans");
        std::array<Range, stems> inputRanges{}, outputRanges{};
        for (std::size_t role = 0; role < stems; ++role) {
            if (input[role].size() != inputSamples || output[role].size() != outputSamples)
                throw std::invalid_argument("Stem conversion requires equal role geometry");
            inputRanges[role] = range(input[role].data(), inputSamples);
            outputRanges[role] = range(output[role].data(), outputSamples);
        }
        for (std::size_t role = 0; role < stems; ++role) {
            for (std::size_t other = 0; other < stems; ++other) {
                if ((role != other && overlaps(outputRanges[role], outputRanges[other])) ||
                    overlaps(outputRanges[role], inputRanges[other]))
                    throw std::invalid_argument("Stem output overlaps borrowed input/output");
            }
            if (std::ranges::any_of(input[role], [](float value) { return !std::isfinite(value); }))
                throw std::invalid_argument("Stem input contains non-finite samples");
        }
        const auto offered = inputSamples / channels, capacity = outputSamples / channels;
        if (offered > plan_.inputFrames() - inputFrame_ ||
            (eof && (offered || inputFrame_ != plan_.inputFrames())) ||
            (eofStarted_ && !eof) || (finished_ && (offered || !eof)))
            throw std::invalid_argument("Stem input/EOF differs from exact stream coverage");
        if (finished_) return {0, 0, outputFrame_, true};
        if (eof) eofStarted_ = true;
        const auto firstOutput = outputFrame_;
        try {
            // A call can produce only hidden preroll with no input consumed.
            // Continue that bounded discard here, rather than returning false
            // unavailability to an owner which still offers positive input.
            for (;;) {
                const auto scratchCapacity = static_cast<std::size_t>(
                    std::min<std::uint64_t>(blockFrames, discard_ + capacity));
                std::array<Progress, stems> progress{};
                for (std::size_t role = 0; role < stems; ++role)
                    progress[role] = convert(states_[role].get(), input[role],
                        std::span(scratch_[role]).first(scratchCapacity * channels), eof);
                const auto common = progress.front();
                if (std::ranges::any_of(progress, [&](const auto& value) {
                        return value.used != common.used || value.generated != common.generated;
                    }))
                    throw std::runtime_error("Stem converters produced divergent role progress");
                if (common.generated > plan_.outputFrames() - generatedFrame_)
                    throw std::runtime_error("Stem conversion exceeded its exact counted output");
                const auto discard = static_cast<std::size_t>(std::min<std::uint64_t>(discard_, common.generated));
                const auto visible = common.generated - discard;
                inputFrame_ += common.used;
                generatedFrame_ += common.generated;
                discard_ -= discard;
                if (eof && !common.generated) {
                    if (inputFrame_ != plan_.inputFrames() || generatedFrame_ != plan_.outputFrames() || discard_)
                        throw std::runtime_error("Stem conversion EOF differs from counted coverage");
                    finished_ = true;
                }
                if (offered && !common.used && !common.generated)
                    throw std::runtime_error("Stem conversion made no input progress");
                if (visible) {
                    for (std::size_t role = 0; role < stems; ++role)
                        std::copy_n(scratch_[role].data() + discard * channels, visible * channels, output[role].data());
                    outputFrame_ += visible;
                }
                if (common.used || visible || !discard || finished_)
                    return {common.used, visible, firstOutput, finished_};
                // No input consumed and only discarded output produced: input
                // spans remain unchanged. Each retry removes >=1 of at most
                // 32*160+159 preroll frames; storage and work cannot grow.
            }
        } catch (...) {
            failed_ = true; // Library states may already have advanced. Never repair/reuse.
            throw;
        }
    }

    FramePlan plan_;
    std::array<State, stems> states_{};
    std::array<std::array<float, samplesPerBlock>, stems> scratch_{};
    std::uint64_t inputStart_ = 0, inputFrame_ = 0, generatedFrame_ = 0, outputFrame_ = 0, discard_ = 0;
    bool eofStarted_ = false, finished_ = false, failed_ = false;
};

StemRateConverter::StemRateConverter(FramePlan plan, std::uint64_t targetFrame)
    : impl_(std::make_unique<Impl>(plan, targetFrame)) {}
StemRateConverter::~StemRateConverter() = default;
std::uint64_t StemRateConverter::inputStartFrame() const noexcept { return impl_->inputStart_; }
StemRateConverter::Step StemRateConverter::process(std::array<std::span<const float>, stems> input,
    std::array<std::span<float>, stems> output, bool endOfInput)
{
    return impl_->process(input, output, endOfInput);
}
} // namespace conflux::audio

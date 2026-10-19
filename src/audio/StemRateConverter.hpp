// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>

namespace conflux::audio {

// Off-thread, fixed stereo44100->48000 four-stem conversion. No process, file,
// queue, source identity, generation, playback owner or RT callback lives here.
class StemRateConverter {
public:
    static constexpr std::size_t stems = 4, channels = 2, blockFrames = 4096;
    static constexpr std::uint64_t inputGrid = 147, outputGrid = 160, prerollCells = 32;

    // Only countFrames creates a valid plan. Immutable/copyable; retain once per
    // source and reuse across seeks. It owns no PCM or converter state.
    class FramePlan {
    public:
        [[nodiscard]] std::uint64_t inputFrames() const noexcept { return input_; }
        [[nodiscard]] std::uint64_t outputFrames() const noexcept { return output_; }
    private:
        friend class StemRateConverter;
        FramePlan(std::uint64_t input, std::uint64_t output) noexcept : input_(input), output_(output) {}
        std::uint64_t input_, output_;
    };
    // Bounded4096 scratch, one stereo zero state, best-quality constant ratio,
    // positive input then deferred zero-input EOF; count actual generated data.
    // Check stop before allocation and between each bounded library call.
    // Zero/unaddressable input OR output-cap geometry ->invalid_argument;
    // actual output over cap
    // ->length_error; cancellation/library error ->runtime_error. No ceil guess.
    static FramePlan countFrames(std::uint64_t inputFrames, std::uint64_t maximumOutputFrames,
                                 std::stop_token stop = {});

    // Construct fresh states for an output cursor in[0,plan.outputFrames()].
    // Anchor is max(0,floor(target/160)-32)*147; internal discard reaches the
    // exact target before any samples become output. The closed finite fixture
    // and closed4*HOP+211 interior-seek fixtures support this candidate; this
    // is finite numerical evidence, not an all-duration equivalence proof.
    explicit StemRateConverter(FramePlan plan, std::uint64_t targetFrame = 0);
    ~StemRateConverter();
    StemRateConverter(const StemRateConverter&) = delete;
    StemRateConverter& operator=(const StemRateConverter&) = delete;
    StemRateConverter(StemRateConverter&&) = delete;
    StemRateConverter& operator=(StemRateConverter&&) = delete;

    [[nodiscard]] std::uint64_t inputStartFrame() const noexcept;
    struct Step {
        std::size_t inputFramesUsed = 0, outputFrames = 0;
        std::uint64_t firstOutputFrame = 0;
        bool finished = false;
    };
    // Four equal even input spans:0..4096frames. Four equal even output spans:
    //1..4096frames. Read-only input roles may alias each other. Every writable
    // output must be disjoint from every other output and every input range.
    // Caller retains unconsumed input. Output is only the returned prefix;
    // firstOutputFrame is the absolute48000 origin, including after a seek.
    // Empty input/EOFfalse may drain available output and then return0,false;
    // it never flushes, pads, resets or finishes a temporary gap.
    // EOFtrue requires empty input after exact remaining44100 coverage; use it
    // until finished. Once the first valid EOFtrue call begins tail drain,
    // EOFfalse is rejected without state/output change. Positive-input EOF is
    // rejected before changing state.
    // finished only follows actual zero-output EOF drain and exact global tail
    // coverage. The owner retains the final generated prefix until that proof
    // before marking its ring packet final. No invented/truncated frame.
    // Validate shape/finite/coverage/nonoverlap before changing any state or
    // output. Library error, divergent role progress or final count mismatch is
    // terminal for this converter; its generation must be discarded off RT.
    // After finished, empty EOF is idempotent; new input/EOFfalse is invalid.
    Step process(std::array<std::span<const float>, stems> input,
                 std::array<std::span<float>, stems> output, bool endOfInput = false);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace conflux::audio

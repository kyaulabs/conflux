// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace conflux::audio {

// Incremental DSP boundary. Every method has one non-RT owner; this
// object has no thread, reader, queue, source cursor, callback or loop policy.
class TempoProcessor {
public:
    static constexpr std::size_t sampleRate = 48000;
    static constexpr std::size_t maximumBlockFrames = 1024;
    struct Config {
        std::size_t channels = 2; // Exactly 2 or 8 aligned interleaved channels.
        double speedPercent = 0; // Finite [-6,6]; duration ratio = 1/(1+p/100).
        bool keyLock = false;
    };
    enum class Backend { UnityBypass, SampleRateBestQuality, RubberBandFiner };
    enum class Phase { Priming, Running, Draining, Finished, Failed };
    struct Counts {
        std::uint64_t inputFramesConsumed = 0;
        std::uint64_t preferredStartPad = 0, padInputFrames = 0, startDelayFrames = 0;
        std::uint64_t discardedStartFrames = 0, rawOutputFrames = 0, outputFrames = 0;
        std::uint64_t processCalls = 0, endOfInputCalls = 0, retrieveCalls = 0;
        std::uint64_t ratioApplications = 0;
        bool fullyDrained = false;
    };
    struct Step {
        std::uint64_t generation = 0;
        std::uint64_t requestedConfiguration = 0, appliedInputConfiguration = 0;
        std::size_t inputFramesUsed = 0, outputFrames = 0;
        Phase phase = Phase::Running;
        bool pendingOutput = false;
        // Padding, delay discard, a pending-output probe or final signalling
        // can progress without consuming real input or delivering output.
        bool internalProgress = false;
    };

    explicit TempoProcessor(Config config);
    ~TempoProcessor();
    TempoProcessor(const TempoProcessor&) = delete;
    TempoProcessor& operator=(const TempoProcessor&) = delete;
    TempoProcessor(TempoProcessor&&) = delete;
    TempoProcessor& operator=(TempoProcessor&&) = delete;

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] Config requestedConfig() const noexcept;
    [[nodiscard]] Backend backend() const noexcept;
    [[nodiscard]] Phase phase() const noexcept;
    [[nodiscard]] Counts counts() const noexcept;

    // New explicit stream: discard the previous generation's pending data and
    // construct fresh state off callback. Generation starts at 1, never wraps
    // or repeats; stale process/requestSpeed tokens reject without mutation.
    // Invalid config preserves the current stream. Backend construction failure
    // makes the old stream Failed; successful reset also recovers Failed state.
    std::uint64_t reset(Config config);

    // After at least one real input frame, before EOF, request another speed
    // in the SAME active backend. Initial config covers priming. SRC/R3 may
    // cross zero while retaining their filter/phase state: backend() stays
    // truthful and zero then is NOT the bit-exact bypass. Entering nonzero from
    // UnityBypass or changing key-lock/backend requires reset. Explicit reset
    // to zero restores exact bypass; never silently discard pending data.
    // Returns a monotonically increasing per-generation request revision.
    // Apply only before the next real-input call, after pending output drains.
    // This acknowledges requested configuration, NOT audible effect timing:
    // SRC interpolates ratios and R3 has internal input/output delay.
    std::uint64_t requestSpeed(std::uint64_t generation, double speedPercent);

    // Input: 0..1024 complete frames; output: 1..1024 complete frames. Ranges
    // must not overlap; all offered input is finite. Validate the entire call
    // before any state/output mutation. Caller retains every unconsumed input
    // frame; a step may drain/prime without consuming the offered input.
    //
    // Empty input with EOF=false is a temporary gap, never a flush. EOF=true
    // requires EMPTY input and at least one previously consumed real frame;
    // caller attests all its real input has been consumed. Subsequent calls
    // must remain empty/EOF=true. Finish only after actual backend drain; every
    // R3 frame after reported start-delay discard is returned, without cropping
    // to nominal duration. Finished empty EOF is idempotent.
    //
    // Pending output drains before accepting more real input. Work is bounded
    // to at most two SRC calls, or one R3 process/retrieve plus at most two
    // availability queries per step; no pump-until-full loop is hidden here.
    // pendingOutput for SRC means another bounded empty-input probe is needed.
    //
    // Invalid shape/range/token/state throws invalid_argument with no change.
    // Backend/count/nonfinite-output failure throws and latches Failed; discard
    // that call's output (it may have been written) and reset before reuse.
    // Counts are actual retrieved/delivered frames, not an exact R3 source map.
    Step process(std::uint64_t generation, std::span<const float> input,
                 std::span<float> output, bool endOfInput = false);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::uint64_t generation_ = 1;
};

} // namespace conflux::audio

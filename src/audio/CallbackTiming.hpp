// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace conflux::audio {

// One render owner writes observations. Read counters on that owner or after
// callbacks quiesce; aggregate percentiles/reset only off the render thread.
// Construct this fixed storage before streaming. There are no internal locks.
class CallbackTiming {
public:
    static constexpr std::uint32_t schemaVersion = 1;
    static constexpr std::size_t histogramBins = 100001;
    static constexpr std::uint64_t histogramResolutionNs = 1000;
    static constexpr std::uint64_t overflowThresholdNs = 100000000;

    // Copy only capabilities supported by the integration's actual SPA headers.
    // A zero flag mask means unavailable, not an observed zero-event guarantee.
    struct ClockCapabilities {
        bool cycle = false;
        std::uint32_t discontinuityFlag = 0;
        std::uint32_t xrunRecoveryFlag = 0;
    };

    struct DriverClock {
        std::uint32_t id = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t cycle = 0;
        std::uint64_t position = 0;
        std::uint64_t duration = 0;
        std::uint32_t rateNum = 0;
        std::uint32_t rateDenom = 0;
        std::uint32_t flags = 0;
    };

    struct Counters {
        std::uint64_t callbacks = 0;
        std::uint64_t maxExecutionNs = 0;
        std::uint64_t deadlineMisses = 0;
        std::uint64_t graphDiscontinuities = 0;
        std::uint64_t xrunRecoveryFlags = 0;
        std::uint64_t graphQuantumMin = 0;
        std::uint64_t graphQuantumMax = 0;
        bool graphPositionObserved = false;
    };

    struct Summary {
        Counters counters{};
        ClockCapabilities capabilities{};
        double p50UpperUs = 0;
        double p95UpperUs = 0;
        double p99UpperUs = 0;
        double p999UpperUs = 0;
        std::uint64_t atLeast100ms = 0;
    };

    CallbackTiming() noexcept;
    explicit CallbackTiming(ClockCapabilities capabilities) noexcept;

    // Constant-time RT observation, matching the existing audio probe. A valid
    // driver-clock quantum supplies the execution deadline; otherwise use the
    // rendered-frame duration at the engine's fixed 48000 Hz. No timestamp or
    // scheduler-lateness inference is made. Copy the clock at callback entry.
    void observe(std::uint64_t executionNs, std::uint64_t renderedFrames,
                 const DriverClock* clock = nullptr) noexcept;
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    [[nodiscard]] ClockCapabilities capabilities() const noexcept { return capabilities_; }
    // Non-RT only. Overflow-bin percentiles use the exact observed maximum,
    // expressed in microseconds, as in the probe.
    [[nodiscard]] Summary summary() const noexcept;
    void reset() noexcept;

private:
    [[nodiscard]] double percentile(double fraction) const noexcept;
    std::array<std::uint64_t, histogramBins> histogram_{};
    ClockCapabilities capabilities_{};
    Counters counters_{};
    std::uint64_t graphQuantumMin_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t previousEnd_ = 0;
    std::uint32_t previousClock_ = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t previousCycle_ = 0, previousRateNum_ = 0, previousRateDenom_ = 0;
};

} // namespace conflux::audio

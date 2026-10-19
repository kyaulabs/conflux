// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/CallbackTiming.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>

namespace {
using conflux::audio::CallbackTiming;
using Clock = CallbackTiming::DriverClock;

static_assert(std::is_trivially_copyable_v<Clock> && std::is_standard_layout_v<Clock>);
static_assert(std::is_trivially_copyable_v<CallbackTiming::Counters>);

bool require(bool value, const char* message)
{
    if (!value)
        std::cerr << message << '\n';
    return value;
}

bool histogramAndReset()
{
    bool passed = true;
    CallbackTiming timing({true, 2, 8});
    const auto empty = timing.summary();
    passed &= require(empty.counters.callbacks == 0 && empty.counters.maxExecutionNs == 0 &&
                          empty.counters.deadlineMisses == 0 && empty.counters.graphDiscontinuities == 0 &&
                          empty.counters.xrunRecoveryFlags == 0 && !empty.counters.graphPositionObserved &&
                          empty.counters.graphQuantumMin == 0 && empty.counters.graphQuantumMax == 0 &&
                          empty.p50UpperUs == 0 && empty.p95UpperUs == 0 && empty.p99UpperUs == 0 &&
                          empty.p999UpperUs == 0 && empty.atLeast100ms == 0,
                      "unobserved timing did not report empty statistics");
    // Exactly one observation in each 1-us bin from [0,1) through [999,1000).
    // Nearest ranks 500, 950, 990 and 999 have these independently known bounds.
    for (std::uint64_t index = 0; index < 1000; ++index)
        timing.observe(index * 1000, 48);
    const auto distribution = timing.summary();
    passed &= require(distribution.counters.callbacks == 1000 && distribution.counters.maxExecutionNs == 999000 &&
                          distribution.counters.deadlineMisses == 0 && distribution.p50UpperUs == 500 &&
                          distribution.p95UpperUs == 950 && distribution.p99UpperUs == 990 &&
                          distribution.p999UpperUs == 999 && distribution.atLeast100ms == 0,
                      "fixed histogram percentile rank or one-microsecond upper bound differs");

    Clock clock{7, 3, 100, 256, 1, 48000, 2 | 8};
    timing.observe(500, 256, &clock);
    timing.reset();
    const auto reset = timing.summary();
    passed &= require(reset.counters.callbacks == 0 && reset.counters.maxExecutionNs == 0 &&
                          reset.counters.graphDiscontinuities == 0 && reset.counters.xrunRecoveryFlags == 0 &&
                          reset.counters.graphQuantumMin == 0 && reset.counters.graphQuantumMax == 0 &&
                          !reset.counters.graphPositionObserved && reset.p999UpperUs == 0 && reset.atLeast100ms == 0 &&
                          reset.capabilities.cycle && reset.capabilities.discontinuityFlag == 2 &&
                          reset.capabilities.xrunRecoveryFlag == 8,
                      "reset retained observations or lost version-dependent capabilities");
    clock.position = 9000;
    clock.flags = 0;
    timing.observe(999, 256, &clock);
    timing.observe(1000, 256);
    timing.observe(0, 256);
    const auto boundary = timing.summary();
    passed &= require(boundary.counters.graphDiscontinuities == 0 && boundary.counters.callbacks == 3 &&
                          boundary.p50UpperUs == 1 && boundary.p95UpperUs == 2 && boundary.p999UpperUs == 2,
                      "reset retained a clock baseline or exact histogram boundaries changed");
    return passed;
}

bool overflowBin()
{
    bool passed = true;
    CallbackTiming timing;
    for (const std::uint64_t elapsed : {99ULL, 999ULL, 1000ULL, 99999999ULL, 100000000ULL, 150123456ULL})
        timing.observe(elapsed, 0);
    const auto result = timing.summary();
    passed &= require(result.counters.callbacks == 6 && result.counters.maxExecutionNs == 150123456 &&
                          result.counters.deadlineMisses == 0 && result.atLeast100ms == 2 &&
                          result.p50UpperUs == 2 && std::abs(result.p999UpperUs - 150123.456) < 1e-6 &&
                          result.p95UpperUs == result.p999UpperUs && result.p99UpperUs == result.p999UpperUs,
                      "100-ms overflow bin did not retain its count and observed maximum bound");
    timing.reset();
    timing.observe(std::numeric_limits<std::uint64_t>::max(), 0);
    const auto maximum = timing.summary();
    passed &= require(maximum.atLeast100ms == 1 && maximum.counters.callbacks == 1 &&
                          maximum.counters.maxExecutionNs == std::numeric_limits<std::uint64_t>::max() &&
                          std::isfinite(maximum.p999UpperUs) && maximum.p999UpperUs ==
                              std::numeric_limits<std::uint64_t>::max() / 1000.0,
                      "large elapsed value overflowed the fixed histogram index or summary");
    return passed;
}

bool executionDeadlines()
{
    bool passed = true;
    CallbackTiming timing;
    timing.observe(999999, 48);
    timing.observe(1000000, 48);
    timing.observe(1000001, 48);
    timing.observe(9000000, 0);
    passed &= require(timing.counters().callbacks == 4 && timing.counters().deadlineMisses == 1,
                      "48-kHz fallback execution deadline or strict greater-than comparison differs");
    Clock clock{3, 0, 0, 96, 1, 48000, 0};
    timing.observe(1500000, 1, &clock);
    clock.position += 96;
    timing.observe(2000000, 1, &clock);
    clock.position += 96;
    timing.observe(2000001, 1, &clock);
    passed &= require(timing.counters().deadlineMisses == 2,
                      "driver quantum did not override rendered-frame fallback duration");
    clock.rateNum = 0;
    timing.observe(1000001, 48, &clock);
    clock.rateNum = 1;
    clock.rateDenom = 0;
    timing.observe(1000001, 48, &clock);
    clock.rateDenom = 48000;
    clock.duration = 0;
    timing.observe(1000001, 48, &clock);
    passed &= require(timing.counters().deadlineMisses == 5,
                      "zero driver rate or duration did not use the output-frame fallback");
    clock.duration = 48;
    clock.rateNum = 2;
    timing.observe(2000001, 256, &clock);
    timing.observe(2000000, 0, &clock);
    passed &= require(timing.counters().callbacks == 12 && timing.counters().deadlineMisses == 6 &&
                          timing.counters().maxExecutionNs == 9000000,
                      "fractional driver rate or observed maximum was calculated incorrectly");
    return passed;
}

bool graphClockChangesAndFlags()
{
    bool passed = true;
    CallbackTiming timing({true, 2, 8});
    const auto observe = [&](Clock clock) { timing.observe(10, 256, &clock); };
    observe({42, 1, 100, 48, 1, 48000, 0});
    observe({42, 1, 148, 48, 1, 48000, 0});
    passed &= require(timing.counters().graphDiscontinuities == 0, "contiguous driver position counted a gap");
    observe({42, 1, 210, 48, 1, 48000, 0});
    observe({42, 1, 258, 96, 1, 48000, 0});
    passed &= require(timing.counters().graphDiscontinuities == 1,
                      "same-clock position gap or contiguous quantum change differs");
    observe({43, 1, 9999, 256, 1, 48000, 0});
    observe({43, 2, 700, 256, 1, 48000, 0});
    observe({43, 2, 99999, 512, 1, 96000, 0});
    passed &= require(timing.counters().graphDiscontinuities == 1,
                      "driver identity, supported cycle or rate change did not establish a new baseline");
    observe({43, 2, 100511, 512, 1, 96000, 2});
    observe({43, 2, 101023, 256, 1, 96000, 8});
    observe({43, 2, 101279, 256, 1, 96000, 8});
    observe({44, 5, 5000, 128, 1, 48000, 2 | 8});
    passed &= require(timing.counters().graphDiscontinuities == 3 && timing.counters().xrunRecoveryFlags == 3 &&
                          timing.counters().callbacks == 11 && timing.counters().graphQuantumMin == 48 &&
                          timing.counters().graphQuantumMax == 512 && timing.counters().graphPositionObserved,
                      "explicit position flags, repeated recovery flags or quantum extrema differ");
    timing.observe(10, 128);
    observe({44, 5, 5256, 128, 1, 48000, 0});
    passed &= require(timing.counters().graphDiscontinuities == 4 && timing.counters().xrunRecoveryFlags == 3,
                      "missing clock observation invented a baseline or hid the later observed position gap");
    return passed;
}

bool unavailableCapabilitiesAndClockWrap()
{
    bool passed = true;
    CallbackTiming timing;
    Clock clock{1, 1, 0, 256, 1, 48000, 2 | 8};
    timing.observe(10, 256, &clock);
    clock.cycle = 2;
    clock.position = 256;
    timing.observe(10, 256, &clock);
    clock.cycle = 3;
    clock.position = 999;
    timing.observe(10, 256, &clock);
    const auto result = timing.summary();
    passed &= require(!result.capabilities.cycle && result.capabilities.discontinuityFlag == 0 &&
                          result.capabilities.xrunRecoveryFlag == 0 && result.counters.xrunRecoveryFlags == 0 &&
                          result.counters.graphDiscontinuities == 1,
                      "unavailable header capabilities were treated as supported flags or cycles");
    timing.reset();
    clock = {7, 0, std::numeric_limits<std::uint64_t>::max() - 15, 16, 1, 48000, 0};
    timing.observe(10, 16, &clock);
    clock.position = 0;
    timing.observe(10, 16, &clock);
    passed &= require(timing.counters().graphDiscontinuities == 0 && timing.counters().graphQuantumMin == 16 &&
                          timing.counters().graphQuantumMax == 16,
                      "unsigned driver-position wrap differed from the probe's position arithmetic");
    timing.reset();
    clock = {9, 0, 10, 0, 0, 0, 0};
    timing.observe(10, 0, &clock);
    passed &= require(!timing.counters().graphPositionObserved && timing.counters().graphQuantumMin == 0 &&
                          timing.counters().graphQuantumMax == 0 && timing.counters().deadlineMisses == 0,
                      "zero-duration driver snapshot claimed negotiated graph timing");
    clock = {9, 0, 100, 128, 1, 48000, 0};
    timing.observe(10, 128, &clock);
    passed &= require(timing.counters().graphPositionObserved && timing.counters().graphQuantumMin == 0 &&
                          timing.counters().graphQuantumMax == 128 && timing.counters().graphDiscontinuities == 0,
                      "zero/valid rate transition changed the probe's minimum or baseline semantics");
    return passed;
}
} // namespace

int main()
{
    const bool histogram = histogramAndReset();
    const bool overflow = overflowBin();
    const bool deadlines = executionDeadlines();
    const bool graph = graphClockChangesAndFlags();
    const bool unavailable = unavailableCapabilitiesAndClockWrap();
    return histogram && overflow && deadlines && graph && unavailable ? 0 : 1;
}

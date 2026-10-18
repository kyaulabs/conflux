// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/CallbackTiming.hpp"

#include <algorithm>
#include <cmath>

namespace conflux::audio {

CallbackTiming::CallbackTiming() noexcept = default;

CallbackTiming::CallbackTiming(ClockCapabilities capabilities) noexcept : capabilities_(capabilities) {}

void CallbackTiming::observe(std::uint64_t executionNs, std::uint64_t renderedFrames,
                             const DriverClock* clock) noexcept
{
    double deadlineNs = 0;
    if (clock) {
        graphQuantumMin_ = std::min(graphQuantumMin_, clock->duration);
        counters_.graphQuantumMax = std::max(counters_.graphQuantumMax, clock->duration);
        counters_.graphQuantumMin = counters_.graphQuantumMax ? graphQuantumMin_ : 0;
        counters_.graphPositionObserved = counters_.graphQuantumMax != 0;
        if (clock->rateNum && clock->rateDenom)
            deadlineNs = 1.0e9 * clock->duration * clock->rateNum / clock->rateDenom;
        bool sameClock = previousClock_ == clock->id && previousRateNum_ == clock->rateNum &&
            previousRateDenom_ == clock->rateDenom;
        if (capabilities_.cycle) {
            sameClock = sameClock && previousCycle_ == clock->cycle;
            previousCycle_ = clock->cycle;
        }
        const bool discontinuity = (sameClock && previousEnd_ != clock->position) ||
            (clock->flags & capabilities_.discontinuityFlag);
        if (discontinuity)
            ++counters_.graphDiscontinuities;
        if (clock->flags & capabilities_.xrunRecoveryFlag)
            ++counters_.xrunRecoveryFlags;
        previousClock_ = clock->id;
        previousRateNum_ = clock->rateNum;
        previousRateDenom_ = clock->rateDenom;
        previousEnd_ = clock->position + clock->duration;
    }
    if (deadlineNs == 0)
        deadlineNs = 1.0e9 * renderedFrames / 48000;
    ++histogram_[std::min<std::uint64_t>(executionNs / histogramResolutionNs, histogram_.size() - 1)];
    counters_.maxExecutionNs = std::max(counters_.maxExecutionNs, executionNs);
    if (deadlineNs > 0 && static_cast<double>(executionNs) > deadlineNs)
        ++counters_.deadlineMisses;
    ++counters_.callbacks;
}

double CallbackTiming::percentile(double fraction) const noexcept
{
    if (!counters_.callbacks)
        return 0;
    const auto rank = static_cast<std::uint64_t>(std::ceil(counters_.callbacks * fraction));
    std::uint64_t count = 0;
    for (std::size_t bin = 0; bin < histogram_.size(); ++bin) {
        count += histogram_[bin];
        if (count >= rank)
            return bin == histogram_.size() - 1 ? counters_.maxExecutionNs / 1000.0 : bin + 1.0;
    }
    return 0;
}

CallbackTiming::Summary CallbackTiming::summary() const noexcept
{
    return {counters_, capabilities_, percentile(.50), percentile(.95), percentile(.99),
            percentile(.999), histogram_.back()};
}

void CallbackTiming::reset() noexcept
{
    histogram_.fill(0);
    counters_ = {};
    graphQuantumMin_ = std::numeric_limits<std::uint64_t>::max();
    previousEnd_ = 0;
    previousClock_ = std::numeric_limits<std::uint32_t>::max();
    previousCycle_ = previousRateNum_ = previousRateDenom_ = 0;
}

} // namespace conflux::audio

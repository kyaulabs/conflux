// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/LiveRecoveryPolicy.hpp"

#include <algorithm>
#include <stdexcept>

namespace conflux::audio::detail {

LiveRecoveryPolicy::LiveRecoveryPolicy(std::uint64_t totalFrames) : totalFrames_(totalFrames)
{
    if (!totalFrames) throw std::invalid_argument("Recovery requires exact nonempty geometry");
}

LiveRecoveryPolicy::Decision LiveRecoveryPolicy::update(
    Demand demand, Coverage coverage, Clock::time_point now) noexcept
{
    if (!demand.token || demand.token > maximumToken || !demand.waitSerial ||
        demand.cursor >= totalFrames_) {
        armedToken_ = 0;
        return {};
    }
    const auto requiredEnd = demand.cursor + std::min(reserveFrames, totalFrames_ - demand.cursor);
    const bool ready = coverage.epoch == demand.epoch && coverage.end >= requiredEnd;
    if (!demand.active) {
        armedToken_ = 0;
        return ready ? Decision{Decision::Kind::Grant, demand.token, demand.waitSerial} : Decision{};
    }
    // An already armed attempt expires before a reserve first observed late.
    // Progress cannot extend this absolute deadline.
    if (armedToken_ == demand.token && now >= deadline_)
        return {Decision::Kind::Timeout, demand.token, demand.waitSerial};
    if (ready) {
        armedToken_ = 0;
        return {Decision::Kind::Grant, demand.token, demand.waitSerial};
    }
    if (armedToken_ != demand.token) {
        armedToken_ = demand.token;
        deadline_ = now > Clock::time_point::max() - maximumWait
            ? Clock::time_point::max() : now + maximumWait;
    }
    return {};
}

bool LiveRecoveryPolicy::advanceAttempt(std::atomic<std::uint64_t>& word,
                                       std::uint64_t oldToken, std::uint64_t newToken) noexcept
{
    if (!oldToken || oldToken >= maximumToken || newToken != oldToken + 1) return false;
    return word.compare_exchange_strong(oldToken, newToken, std::memory_order_seq_cst);
}

bool LiveRecoveryPolicy::commitTimeout(std::atomic<std::uint64_t>& word, const Decision& decision) noexcept
{
    if (decision.kind != Decision::Kind::Timeout || !decision.token ||
        decision.token > maximumToken || !decision.waitSerial) return false;
    auto expected = decision.token;
    return word.compare_exchange_strong(expected, decision.token | terminalBit, std::memory_order_seq_cst);
}

bool LiveRecoveryPolicy::timedOut(std::uint64_t word) noexcept { return (word & terminalBit) != 0; }

} // namespace conflux::audio::detail

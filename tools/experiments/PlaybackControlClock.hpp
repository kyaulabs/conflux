// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/LiveSession.hpp"
#include <algorithm>
#include <cstdint>

namespace conflux::experiments {
inline constexpr int liveDiagnosticTailSeconds = 60;

// Keep the terminal replay playable through the measured boundary, with
// ten seconds of margin for recovery and idle-close in the finite drain.
constexpr std::uint64_t hourReplayStartFrame(std::uint64_t sourceFrames, std::uint64_t remainingMs) noexcept
{
    constexpr std::uint64_t framesPerMs = 48;
    constexpr std::uint64_t nominalTailFrames = (liveDiagnosticTailSeconds - 10) * 1000 * framesPerMs;
    if (sourceFrames <= nominalTailFrames)
        return 0;
    const auto afterTailReserve = sourceFrames - nominalTailFrames;
    return afterTailReserve / framesPerMs < remainingMs ? 0 :
        afterTailReserve - remainingMs * framesPerMs;
}

// The fixed soak uses unit tempo. Starved output frames do not consume source.
inline bool playbackControlClockMatches(const audio::LiveSession::State& before,
                                       const audio::LiveSession::State& after)
{
    if (after.outputFrame < before.outputFrame) return false;
    const auto advance = after.outputFrame - before.outputFrame;
    for (std::size_t deck = 0; deck < before.sourceFrame.size(); ++deck) {
        if (before.sourceFrames[deck] != after.sourceFrames[deck] ||
            before.sourceFrame[deck] > before.sourceFrames[deck] ||
            after.starvedFrames[deck] < before.starvedFrames[deck]) return false;
        const auto missing = after.starvedFrames[deck] - before.starvedFrames[deck];
        if (missing > advance) return false;
        const auto consumed = std::min(advance - missing, before.sourceFrames[deck] - before.sourceFrame[deck]);
        if (after.sourceFrame[deck] != before.sourceFrame[deck] + consumed) return false;
    }
    return true;
}
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/LiveSession.hpp"
#include <algorithm>

namespace conflux::experiments {
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

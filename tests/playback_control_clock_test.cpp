// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../tools/experiments/PlaybackControlClock.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using State = conflux::audio::LiveSession::State;
using conflux::experiments::playbackControlClockMatches;
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

void advancingAndStarvedDecks()
{
    State before, after;
    before.outputFrame = 1000;
    before.sourceFrames = {10000, 20000};
    before.sourceFrame = {2000, 3000};
    before.starvedFrames = {500, 0};
    after = before;
    after.outputFrame += 768;
    after.sourceFrame = {2768, 3768};
    require(playbackControlClockMatches(before, after), "Uninterrupted clocks rejected");
    after.starvedFrames[0] += 512;
    after.sourceFrame[0] -= 512;
    require(playbackControlClockMatches(before, after), "Recorded starvation mistaken for a transport jump");
    ++after.sourceFrame[0];
    require(!playbackControlClockMatches(before, after), "Source jump hidden by starvation");
    --after.sourceFrame[0];
    --after.sourceFrame[1];
    require(!playbackControlClockMatches(before, after), "Unaffected peer clock mismatch accepted");
}

void invalidCountersAndEof()
{
    State before, after;
    before.outputFrame = 1000;
    before.sourceFrames = {10000, 20000};
    before.sourceFrame = {9990, 19990};
    before.starvedFrames = {500, 500};
    after = before;
    after.outputFrame += 256;
    after.sourceFrame = before.sourceFrames;
    require(playbackControlClockMatches(before, after), "EOF clamping rejected");
    after.starvedFrames[0] = 499;
    require(!playbackControlClockMatches(before, after), "Decreasing starvation counter accepted");
    after.starvedFrames[0] = 757;
    require(!playbackControlClockMatches(before, after), "Starvation greater than output advance accepted");
    after = before;
    --after.outputFrame;
    require(!playbackControlClockMatches(before, after), "Reversed output clock accepted");
    after = before;
    ++after.sourceFrames[0];
    require(!playbackControlClockMatches(before, after), "Changed source length accepted");
    before.sourceFrame[0] = 10001;
    after = before;
    require(!playbackControlClockMatches(before, after), "Out-of-range source cursor accepted");
}

void clocksNearIntegerLimit()
{
    State before, after;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    before.outputFrame = 1;
    before.sourceFrames = {maximum, maximum};
    before.sourceFrame = {maximum - 10, maximum - 20};
    after = before;
    after.outputFrame += 256;
    after.sourceFrame = before.sourceFrames;
    require(playbackControlClockMatches(before, after), "EOF arithmetic wrapped near uint64 limit");
}
}

int main()
{
    try {
        advancingAndStarvedDecks(); invalidCountersAndEof(); clocksNearIntegerLimit();
        std::cout << "Control clock starvation, peer, EOF and invalid-counter checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

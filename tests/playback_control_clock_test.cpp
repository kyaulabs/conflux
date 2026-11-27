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

void terminalReplayFitsTheFiniteDrain()
{
    using conflux::experiments::hourReplayStartFrame;
    constexpr std::uint64_t shortSource = 1123201;
    constexpr std::uint64_t longSource = 4492801;
    constexpr std::uint64_t framesPerMs = 48;
    constexpr std::uint64_t nominalTailFrames = 50 * 1000 * framesPerMs;
    static_assert(hourReplayStartFrame(shortSource, 5001) == 0);
    static_assert(hourReplayStartFrame(longSource, 50000) == 0);
    static_assert(hourReplayStartFrame(longSource, 43600) == 1);
    for (std::uint64_t ms = 5001; ms <= 100000; ++ms) {
        const auto start = hourReplayStartFrame(longSource, ms);
        const auto remainingFrames = ms * framesPerMs;
        const auto nominalAdvance = longSource - start;
        require(nominalAdvance == std::min(longSource, remainingFrames + nominalTailFrames),
            "Replay left an unbounded tail or omitted more source than needed");
        require(!start || nominalAdvance > remainingFrames,
            "A shortened replay would finish before the measured hour ends");
    }
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    static_assert(hourReplayStartFrame(longSource, maximum) == 0);
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
        terminalReplayFitsTheFiniteDrain();
        std::cout << "Control clock starvation, peer, EOF and invalid-counter checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <vector>

int main()
{
    using conflux::audio::RenderCore;
    const auto require = [](bool condition, const char* message) {
        if (!condition)
            std::cerr << message << '\n';
        return condition;
    };
    bool passed = true;
    // Cross both a table wrap and common live callback boundaries.
    std::vector<float> whole(100003 * RenderCore::channels);
    std::vector<float> partitioned(whole.size());
    RenderCore offline(true), live(true);
    passed &= require(offline.render(whole), "whole render failed");
    const std::array<std::size_t, 5> partitions{1, 256, 512, 7, 1024};
    for (std::size_t offset = 0, part = 0; offset < partitioned.size(); ++part) {
        const auto samples = std::min(partitions[part % partitions.size()] * 2,
                                      partitioned.size() - offset);
        passed &= require(live.render(std::span(partitioned).subspan(offset, samples)),
                          "partition render failed");
        offset += samples;
    }
    passed &= require(whole == partitioned, "live/offline partition parity failed");
    passed &= require(live.frameClock() == 100003 && offline.frameClock() == 100003,
                      "frame clock depends on partitioning");
    bool hasSignal = false;
    for (std::size_t i = 0; i < whole.size(); i += 2) {
        passed &= require(std::isfinite(whole[i]) && std::abs(whole[i]) <= RenderCore::amplitude,
                          "tone has invalid numeric range");
        passed &= require(whole[i] == whole[i + 1], "stereo channels differ");
        if (i >= RenderCore::sampleRate * 2)
            passed &= require(whole[i] == whole[i - RenderCore::sampleRate * 2],
                              "tone period differs across table wrap");
        hasSignal |= whole[i] != 0.0F;
    }
    passed &= require(hasSignal, "tone is silent");
    RenderCore silence;
    std::array<float, 1024> silent;
    silent.fill(42.0F);
    passed &= require(silence.render(silent) && silence.frameClock() == 512 &&
                      std::ranges::all_of(silent, [](float value) { return value == 0.0F; }),
                      "default output is not silence");
    const auto before = live.frameClock();
    std::array<float, 3> malformed{42.0F, 42.0F, 42.0F};
    passed &= require(!live.render(malformed) && live.frameClock() == before &&
                      malformed[0] == 42.0F && malformed[2] == 42.0F,
                      "incomplete stereo frame was accepted");
    passed &= require(live.render({}) && live.frameClock() == before,
                      "empty render changes the clock");
    return passed ? 0 : 1;
}

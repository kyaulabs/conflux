// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace conflux::audio {

// Render-owner-only stereo EQ at 48 kHz. Low/Mid/High are bands 0/1/2.
// Fixed storage; parameter changes and processing never allocate or lock.
class ChannelEq {
public:
    static constexpr std::size_t bandCount = 3;
    static constexpr float minGainDb = -26.F;
    static constexpr float maxGainDb = 6.F;

    // Invalid requests leave every target, coefficient, ramp and history intact.
    // Zero frames applies immediately; duplicate targets do not extend a ramp.
    bool setGain(std::size_t band, float db, std::uint32_t rampFrames) noexcept;
    [[nodiscard]] float gainDb(std::size_t band) const noexcept;
    std::array<float, 2> process(float left, float right) noexcept;
    // Source discontinuity: forget audio history, preserving parameter progress.
    void clearState() noexcept;

private:
    using Coefficients = std::array<double, 5>; // b0, b1, b2, a1, a2; a0 = 1.
    struct History { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    struct Band {
        Coefficients current{1, 0, 0, 0, 0}, target{1, 0, 0, 0, 0}, step{};
        std::array<History, 2> history{};
        float targetDb = 0.F;
        std::uint32_t remaining = 0;
    };
    static Coefficients coefficients(std::size_t band, float db) noexcept;
    void clearChannel(std::size_t channel) noexcept;
    std::array<Band, bandCount> bands_{};
};

} // namespace conflux::audio

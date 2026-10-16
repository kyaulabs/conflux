// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace conflux::audio {

// Shared by the live probe and headless rendering. Construct before processing.
class RenderCore {
public:
    static constexpr std::uint32_t sampleRate = 48000;
    static constexpr std::uint32_t channels = 2;
    static constexpr float amplitude = 0.01F;

    explicit RenderCore(bool tone = false);
    // Reject incomplete stereo frames without writing or advancing the clock.
    bool render(std::span<float> interleaved) noexcept;
    [[nodiscard]] std::uint64_t frameClock() const noexcept { return frameClock_; }

private:
    std::array<float, sampleRate> tone_{};
    std::uint64_t frameClock_ = 0;
    std::uint32_t phase_ = 0;
    bool audible_;
};

} // namespace conflux::audio

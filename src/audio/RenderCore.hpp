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
    // Borrow immutable stereo 48 kHz PCM. Its storage must outlive this renderer.
    explicit RenderCore(std::span<const float> pcm);
    // Reject incomplete stereo frames without writing or advancing the clock.
    bool render(std::span<float> interleaved) noexcept;
    // Only the render owner may seek, between render calls or before streaming.
    // The output frame clock remains monotonic across source seeks and EOF.
    bool seek(std::uint64_t frame) noexcept;
    [[nodiscard]] std::uint64_t frameClock() const noexcept { return frameClock_; }
    [[nodiscard]] std::uint64_t sourceFrame() const noexcept { return sourceFrame_; }
    [[nodiscard]] std::uint64_t sourceFrames() const noexcept { return source_.size() / channels; }

private:
    std::array<float, sampleRate> tone_{};
    std::span<const float> source_{};
    std::uint64_t frameClock_ = 0;
    std::uint64_t sourceFrame_ = 0;
    std::uint32_t phase_ = 0;
    bool audible_;
};

} // namespace conflux::audio

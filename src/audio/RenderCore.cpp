// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"

#include <cmath>
#include <numbers>

namespace conflux::audio {

RenderCore::RenderCore(bool tone) : audible_(tone)
{
    for (std::size_t i = 0; i < tone_.size(); ++i)
        tone_[i] = amplitude * static_cast<float>(std::sin(
            2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / sampleRate));
}

bool RenderCore::render(std::span<float> interleaved) noexcept
{
    if (interleaved.size() % channels != 0)
        return false;
    for (std::size_t i = 0; i < interleaved.size(); i += channels) {
        const float sample = audible_ ? tone_[phase_] : 0.0F;
        interleaved[i] = interleaved[i + 1] = sample;
        if (++phase_ == sampleRate)
            phase_ = 0;
    }
    frameClock_ += interleaved.size() / channels;
    return true;
}

} // namespace conflux::audio

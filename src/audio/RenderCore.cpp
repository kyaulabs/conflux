// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace conflux::audio {

RenderCore::RenderCore(bool tone) : audible_(tone)
{
    for (std::size_t i = 0; i < tone_.size(); ++i)
        tone_[i] = amplitude * static_cast<float>(std::sin(
            2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / sampleRate));
}

RenderCore::RenderCore(std::span<const float> pcm) : source_(pcm), audible_(false)
{
    if (pcm.empty() || pcm.size() % channels != 0 ||
        std::ranges::any_of(pcm, [](float sample) { return !std::isfinite(sample); }))
        throw std::invalid_argument("Playback PCM must contain finite, complete stereo frames");
}

bool RenderCore::seek(std::uint64_t frame) noexcept
{
    if (source_.empty() || frame > sourceFrames())
        return false;
    sourceFrame_ = frame;
    return true;
}

bool RenderCore::render(std::span<float> interleaved) noexcept
{
    if (interleaved.size() % channels != 0)
        return false;
    if (interleaved.empty())
        return true;
    if (!source_.empty()) {
        const auto count = std::min<std::uint64_t>(interleaved.size() / channels,
                                                   sourceFrames() - sourceFrame_);
        const auto copiedSamples = static_cast<std::size_t>(count * channels);
        std::copy_n(source_.data() + sourceFrame_ * channels, copiedSamples, interleaved.data());
        std::fill(interleaved.begin() + copiedSamples, interleaved.end(), 0.0F);
        sourceFrame_ += count;
        frameClock_ += interleaved.size() / channels;
        return true;
    }
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

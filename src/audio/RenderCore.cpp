// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace conflux::audio {

RenderCore::RenderCore(bool tone) : audible_(tone)
{
    for (std::size_t i = 0; i < tone_.size(); ++i)
        tone_[i] = amplitude * static_cast<float>(std::sin(
            2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / sampleRate));
}

RenderCore::RenderCore(std::span<const float> pcm) : RenderCore(pcm, {}) {}

RenderCore::RenderCore(std::span<const float> deckA, std::span<const float> deckB) : audible_(false)
{
    if (deckA.empty() && deckB.empty())
        throw std::invalid_argument("Playback PCM must contain finite, complete stereo frames");
    const std::array sources{deckA, deckB};
    for (std::size_t index = 0; index < deckCount; ++index) {
        const auto pcm = sources[index];
        if (pcm.size() % channels != 0 ||
            std::ranges::any_of(pcm, [](float sample) { return !std::isfinite(sample); }))
            throw std::invalid_argument("Playback PCM must contain finite, complete stereo frames");
        decks_[index].source = pcm;
        decks_[index].playing = !pcm.empty();
    }
}

bool RenderCore::seek(std::uint64_t frame) noexcept
{
    return seek(0, frame);
}

bool RenderCore::seek(std::size_t deck, std::uint64_t frame) noexcept
{
    if (deck >= deckCount || decks_[deck].source.empty() || frame > sourceFrames(deck))
        return false;
    decks_[deck].frame = frame;
    decks_[deck].cuePreviewing = false;
    return true;
}

bool RenderCore::setPlaying(std::size_t deck, bool playing) noexcept
{
    if (deck >= deckCount || decks_[deck].source.empty())
        return false;
    decks_[deck].playing = playing;
    decks_[deck].cuePreviewing = false;
    return true;
}

bool RenderCore::setDeckGain(std::size_t deck, float gain) noexcept
{
    if (deck >= deckCount || decks_[deck].source.empty() ||
        !std::isfinite(gain) || gain < 0.0F || gain > 1.0F)
        return false;
    decks_[deck].gain = gain;
    return true;
}

bool RenderCore::setMasterGain(float gain) noexcept
{
    if (!std::isfinite(gain) || gain < 0.0F || gain > 1.0F)
        return false;
    masterGain_ = gain;
    return true;
}

bool RenderCore::cuePress(std::size_t deck) noexcept
{
    if (deck >= deckCount || decks_[deck].source.empty())
        return false;
    auto& state = decks_[deck];
    if (state.cueHeld)
        return true;
    state.cueHeld = true;
    if (state.playing) {
        state.frame = state.cueFrame;
        state.playing = false;
    } else if (state.frame != state.cueFrame) {
        state.cueFrame = state.frame;
    } else {
        state.cuePreviewing = true;
    }
    return true;
}

bool RenderCore::cueRelease(std::size_t deck) noexcept
{
    if (deck >= deckCount || decks_[deck].source.empty())
        return false;
    auto& state = decks_[deck];
    if (!state.cueHeld)
        return true;
    state.cueHeld = false;
    if (state.cuePreviewing) {
        state.frame = state.cueFrame;
        state.cuePreviewing = false;
    }
    return true;
}

bool RenderCore::setCueFrame(std::size_t deck, std::uint64_t frame) noexcept
{
    if (deck >= deckCount || decks_[deck].source.empty() || frame > sourceFrames(deck))
        return false;
    decks_[deck].cueFrame = frame;
    return true;
}

bool RenderCore::apply(const Command& command) noexcept
{
    switch (command.type) {
    case Command::Type::Play: return setPlaying(command.deck, true);
    case Command::Type::Pause: return setPlaying(command.deck, false);
    case Command::Type::Seek: return seek(command.deck, command.frame);
    case Command::Type::DeckGain: return setDeckGain(command.deck, command.gain);
    case Command::Type::MasterGain: return setMasterGain(command.gain);
    case Command::Type::CuePress: return cuePress(command.deck);
    case Command::Type::CueRelease: return cueRelease(command.deck);
    }
    return false;
}

bool RenderCore::render(std::span<float> interleaved) noexcept
{
    if (interleaved.size() % channels != 0)
        return false;
    if (interleaved.empty())
        return true;
    if (!decks_[0].source.empty() || !decks_[1].source.empty()) {
        const auto frames = interleaved.size() / channels;
        std::array<std::uint64_t, deckCount> available{};
        for (std::size_t index = 0; index < deckCount; ++index)
            if (decks_[index].playing || decks_[index].cuePreviewing)
                available[index] = std::min<std::uint64_t>(frames, sourceFrames(index) - decks_[index].frame);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            double left = 0.0, right = 0.0;
            bool contributed = false;
            for (std::size_t index = 0; index < deckCount; ++index) {
                if (frame >= available[index])
                    continue;
                const auto& deck = decks_[index];
                const auto source = (deck.frame + frame) * channels;
                const auto deckLeft = static_cast<double>(deck.source[source]) * deck.gain;
                const auto deckRight = static_cast<double>(deck.source[source + 1]) * deck.gain;
                if (contributed) {
                    left += deckLeft;
                    right += deckRight;
                } else {
                    left = deckLeft;
                    right = deckRight;
                    contributed = true;
                }
            }
            constexpr auto limit = static_cast<double>(std::numeric_limits<float>::max());
            interleaved[frame * channels] = static_cast<float>(std::clamp(left * masterGain_, -limit, limit));
            interleaved[frame * channels + 1] = static_cast<float>(std::clamp(right * masterGain_, -limit, limit));
        }
        for (std::size_t index = 0; index < deckCount; ++index)
            decks_[index].frame += available[index];
        frameClock_ += frames;
        return true;
    }
    for (std::size_t i = 0; i < interleaved.size(); i += channels) {
        const float sample = audible_ ? tone_[phase_] * masterGain_ : 0.0F;
        interleaved[i] = interleaved[i + 1] = sample;
        if (++phase_ == sampleRate)
            phase_ = 0;
    }
    frameClock_ += interleaved.size() / channels;
    return true;
}

} // namespace conflux::audio

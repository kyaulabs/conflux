// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace conflux::audio {
namespace {
AudioSource validatedMemory(std::span<const float> pcm)
{
    if (pcm.size() % RenderCore::channels != 0 ||
        std::ranges::any_of(pcm, [](float sample) { return !std::isfinite(sample); }))
        throw std::invalid_argument("Playback PCM must contain finite, complete stereo frames");
    return AudioSource::fromMemory(pcm);
}
} // namespace

RenderCore::RenderCore(bool tone) : audible_(tone)
{
    for (std::size_t i = 0; i < tone_.size(); ++i)
        tone_[i] = amplitude * static_cast<float>(std::sin(
            2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / sampleRate));
}

RenderCore::RenderCore(std::span<const float> pcm) : RenderCore(pcm, std::span<const float>{}) {}

RenderCore::RenderCore(std::span<const float> deckA, std::span<const float> deckB)
    : RenderCore(validatedMemory(deckA), validatedMemory(deckB)) {}

RenderCore::RenderCore(AudioSource source) : RenderCore(source, AudioSource{}) {}

RenderCore::RenderCore(AudioSource deckA, AudioSource deckB) : audible_(false)
{
    if (!deckA.frames && !deckB.frames)
        throw std::invalid_argument("Playback PCM must contain finite, complete stereo frames");
    const std::array sources{deckA, deckB};
    for (std::size_t index = 0; index < deckCount; ++index) {
        const auto source = sources[index];
        if ((!source.frames && (source.memory || source.read)) ||
            (source.frames && bool(source.memory) == bool(source.read)) ||
            (source.memory && source.frames > std::numeric_limits<std::size_t>::max() / (channels * sizeof(float))))
            throw std::invalid_argument("Playback source must contain known stereo frames and exactly one reader");
        decks_[index].source = source;
        decks_[index].playing = source.frames != 0;
    }
}

void RenderCore::moveCursor(Deck& deck, std::uint64_t frame) noexcept
{
    deck.buffering = false;
    if (deck.frame == frame)
        return;
    deck.frame = frame;
    if (deck.source.discontinuity)
        deck.source.discontinuity(deck.source.context, frame);
}

bool RenderCore::seek(std::uint64_t frame) noexcept
{
    return seek(0, frame);
}

bool RenderCore::seek(std::size_t deck, std::uint64_t frame) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames || frame > sourceFrames(deck))
        return false;
    moveCursor(decks_[deck], frame);
    decks_[deck].cuePreviewing = false;
    return true;
}

bool RenderCore::setPlaying(std::size_t deck, bool playing) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames)
        return false;
    decks_[deck].playing = playing;
    decks_[deck].cuePreviewing = false;
    decks_[deck].buffering = false;
    return true;
}

bool RenderCore::setDeckGain(std::size_t deck, float gain) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames ||
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

bool RenderCore::setCueSelected(std::size_t deck, bool selected) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames)
        return false;
    decks_[deck].cueSelected = selected;
    return true;
}

bool RenderCore::setCueGain(float gain) noexcept
{
    if (!std::isfinite(gain) || gain < 0.0F || gain > 1.0F)
        return false;
    cueGain_ = gain;
    return true;
}

bool RenderCore::cuePress(std::size_t deck) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames)
        return false;
    auto& state = decks_[deck];
    if (state.cueHeld)
        return true;
    state.cueHeld = true;
    if (state.playing) {
        moveCursor(state, state.cueFrame);
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
    if (deck >= deckCount || !decks_[deck].source.frames)
        return false;
    auto& state = decks_[deck];
    if (!state.cueHeld)
        return true;
    state.cueHeld = false;
    if (state.cuePreviewing) {
        moveCursor(state, state.cueFrame);
        state.cuePreviewing = false;
    }
    return true;
}

bool RenderCore::setCueFrame(std::size_t deck, std::uint64_t frame) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames || frame > sourceFrames(deck))
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
    case Command::Type::CueOn: return setCueSelected(command.deck, true);
    case Command::Type::CueOff: return setCueSelected(command.deck, false);
    case Command::Type::CueGain: return setCueGain(command.gain);
    }
    return false;
}

bool RenderCore::render(std::span<float> interleaved) noexcept
{
    return renderInternal(interleaved, {});
}

bool RenderCore::renderBuses(std::span<float> master, std::span<float> cue) noexcept
{
    if (master.size() != cue.size() || master.size() % channels != 0)
        return false;
    if (master.empty())
        return true;
    const auto first = reinterpret_cast<std::uintptr_t>(master.data());
    const auto second = reinterpret_cast<std::uintptr_t>(cue.data());
    if (first <= second ? second - first < master.size_bytes() : first - second < cue.size_bytes())
        return false;
    return renderInternal(master, cue);
}

void RenderCore::beginMeters(std::size_t frames) noexcept
{
    meters_.blockFrames = frames;
    for (auto* meter : {&meters_.deck[0], &meters_.deck[1], &meters_.master, &meters_.cue}) {
        meter->peak = {};
        meter->rms = {};
    }
}

void RenderCore::finishMeters() noexcept
{
    for (auto* meter : {&meters_.deck[0], &meters_.deck[1], &meters_.master, &meters_.cue})
        for (auto& square : meter->rms)
            square = std::sqrt(square / meters_.blockFrames);
}

void RenderCore::measure(StereoMeter& meter, float left, float right) noexcept
{
    const std::array<double, channels> samples{left, right};
    for (std::size_t channel = 0; channel < channels; ++channel) {
        const auto magnitude = std::abs(samples[channel]);
        meter.peak[channel] = std::max(meter.peak[channel], magnitude);
        meter.rms[channel] += samples[channel] * samples[channel];
        if (magnitude > 1.0)
            ++meter.overrangeSamples[channel];
    }
}

void RenderCore::writeBuses(std::span<float> master, std::span<float> cue, std::size_t frame,
                          double left, double right, double cueLeft, double cueRight) noexcept
{
    constexpr auto limit = static_cast<double>(std::numeric_limits<float>::max());
    const auto masterLeft = static_cast<float>(std::clamp(left * masterGain_, -limit, limit));
    const auto masterRight = static_cast<float>(std::clamp(right * masterGain_, -limit, limit));
    const auto headphoneLeft = static_cast<float>(std::clamp(cueLeft * cueGain_, -limit, limit));
    const auto headphoneRight = static_cast<float>(std::clamp(cueRight * cueGain_, -limit, limit));
    master[frame * channels] = masterLeft;
    master[frame * channels + 1] = masterRight;
    if (!cue.empty()) {
        cue[frame * channels] = headphoneLeft;
        cue[frame * channels + 1] = headphoneRight;
    }
    measure(meters_.master, masterLeft, masterRight);
    measure(meters_.cue, headphoneLeft, headphoneRight);
}

bool RenderCore::renderInternal(std::span<float> interleaved, std::span<float> cue) noexcept
{
    if (interleaved.size() % channels != 0)
        return false;
    if (interleaved.empty())
        return true;
    beginMeters(interleaved.size() / channels);
    for (auto& deck : decks_)
        deck.buffering = false;
    if (decks_[0].source.frames || decks_[1].source.frames) {
        if ((decks_[0].source.frames && !decks_[0].source.isMemory()) ||
            (decks_[1].source.frames && !decks_[1].source.isMemory()))
            return renderSources(interleaved, cue);
        const auto frames = interleaved.size() / channels;
        std::array<std::uint64_t, deckCount> available{};
        for (std::size_t index = 0; index < deckCount; ++index)
            if (decks_[index].playing || decks_[index].cuePreviewing)
                available[index] = std::min<std::uint64_t>(frames, sourceFrames(index) - decks_[index].frame);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            double left = 0.0, right = 0.0;
            double cueLeft = 0.0, cueRight = 0.0;
            bool contributed = false, cueContributed = false;
            for (std::size_t index = 0; index < deckCount; ++index) {
                if (frame >= available[index])
                    continue;
                const auto& deck = decks_[index];
                const auto source = (deck.frame + frame) * channels;
                const auto rawLeft = deck.source.memory[source], rawRight = deck.source.memory[source + 1];
                measure(meters_.deck[index], rawLeft, rawRight);
                const auto deckLeft = static_cast<double>(rawLeft) * deck.gain;
                const auto deckRight = static_cast<double>(rawRight) * deck.gain;
                if (contributed) {
                    left += deckLeft;
                    right += deckRight;
                } else {
                    left = deckLeft;
                    right = deckRight;
                    contributed = true;
                }
                if (deck.cueSelected) {
                    if (cueContributed) {
                        cueLeft += rawLeft;
                        cueRight += rawRight;
                    } else {
                        cueLeft = rawLeft;
                        cueRight = rawRight;
                        cueContributed = true;
                    }
                }
            }
            writeBuses(interleaved, cue, frame, left, right, cueLeft, cueRight);
        }
        for (std::size_t index = 0; index < deckCount; ++index)
            decks_[index].frame += available[index];
        frameClock_ += frames;
        finishMeters();
        return true;
    }
    for (std::size_t i = 0; i < interleaved.size(); i += channels) {
        const float sample = audible_ ? tone_[phase_] * masterGain_ : 0.0F;
        interleaved[i] = interleaved[i + 1] = sample;
        if (!cue.empty())
            cue[i] = cue[i + 1] = 0.0F;
        measure(meters_.master, sample, sample);
        if (++phase_ == sampleRate)
            phase_ = 0;
    }
    frameClock_ += interleaved.size() / channels;
    finishMeters();
    return true;
}

bool RenderCore::renderSources(std::span<float> interleaved, std::span<float> cue) noexcept
{
    const auto frames = interleaved.size() / channels;
    std::array<std::uint64_t, deckCount> needed{}, delivered{};
    std::array<bool, deckCount> stalled{};
    for (std::size_t index = 0; index < deckCount; ++index)
        if (decks_[index].playing || decks_[index].cuePreviewing)
            needed[index] = std::min<std::uint64_t>(frames, sourceFrames(index) - decks_[index].frame);

    for (std::size_t offset = 0; offset < frames;) {
        const auto count = std::min(sourceBlockFrames, frames - offset);
        std::array<std::size_t, deckCount> available{};
        std::array<const float*, deckCount> samples{};
        for (std::size_t index = 0; index < deckCount; ++index) {
            if (stalled[index] || delivered[index] == needed[index])
                continue;
            auto& deck = decks_[index];
            const auto requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(count, needed[index] - delivered[index]));
            if (deck.source.isMemory()) {
                samples[index] = deck.source.memory + deck.frame * channels;
                available[index] = requested;
            } else {
                auto buffer = std::span(sourceScratch_[index]).first(requested * channels);
                auto result = deck.source.read(deck.source.context, deck.frame, buffer);
                if (result.frames > requested || std::ranges::any_of(
                        buffer.first(std::min(result.frames, requested) * channels),
                        [](float sample) { return !std::isfinite(sample); }))
                    result = {0, true};
                samples[index] = buffer.data();
                available[index] = result.frames;
                deck.readFailed |= result.failed;
                // Do not repeatedly query a missing suffix within one render
                // call. The peer and output clock continue through its silence.
                stalled[index] = result.frames < requested || result.failed;
            }
        }

        for (std::size_t frame = 0; frame < count; ++frame) {
            double left = 0.0, right = 0.0;
            double cueLeft = 0.0, cueRight = 0.0;
            bool contributed = false, cueContributed = false;
            for (std::size_t index = 0; index < deckCount; ++index) {
                if (frame >= available[index])
                    continue;
                const auto rawLeft = samples[index][frame * channels], rawRight = samples[index][frame * channels + 1];
                measure(meters_.deck[index], rawLeft, rawRight);
                const auto deckLeft = static_cast<double>(rawLeft) * decks_[index].gain;
                const auto deckRight = static_cast<double>(rawRight) * decks_[index].gain;
                if (contributed) {
                    left += deckLeft;
                    right += deckRight;
                } else {
                    left = deckLeft;
                    right = deckRight;
                    contributed = true;
                }
                if (decks_[index].cueSelected) {
                    if (cueContributed) {
                        cueLeft += rawLeft;
                        cueRight += rawRight;
                    } else {
                        cueLeft = rawLeft;
                        cueRight = rawRight;
                        cueContributed = true;
                    }
                }
            }
            writeBuses(interleaved, cue, offset + frame, left, right, cueLeft, cueRight);
        }
        for (std::size_t index = 0; index < deckCount; ++index) {
            decks_[index].frame += available[index];
            delivered[index] += available[index];
        }
        offset += count;
    }
    for (std::size_t index = 0; index < deckCount; ++index) {
        auto& deck = decks_[index];
        const auto missing = needed[index] - delivered[index];
        deck.buffering = missing != 0;
        deck.starvedFrames += missing;
    }
    frameClock_ += frames;
    finishMeters();
    return true;
}

} // namespace conflux::audio

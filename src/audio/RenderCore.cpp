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
bool validSource(AudioSource source) noexcept
{
    const bool controls = source.setStemGain && source.stemGain;
    if (bool(source.setStemGain) != bool(source.stemGain) ||
        (controls && (!source.frames || source.memory || !source.read || !source.context)))
        return false;
    return !((!source.frames && (source.memory || source.read)) ||
        (source.frames && bool(source.memory) == bool(source.read)) ||
        (source.memory && source.frames > std::numeric_limits<std::size_t>::max() / (RenderCore::channels * sizeof(float))));
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
        if (!validSource(source))
            throw std::invalid_argument("Playback source must contain known stereo frames and exactly one reader");
        decks_[index].source = source;
        decks_[index].playing = source.frames != 0;
    }
}

void RenderCore::GainRamp::snap(double value) noexcept
{
    current = start = target = value;
    position = 0;
}

void RenderCore::GainRamp::set(double value, std::uint32_t frames) noexcept
{
    if (!frames) { snap(value); return; }
    if (value == target) return;
    start = current;
    target = value;
    position = 0;
}

double RenderCore::GainRamp::next(std::uint32_t frames) noexcept
{
    if (!frames || current == target) return current = target;
    if (++position >= frames) return current = target;
    const double fraction = double(position) / frames;
    return current = (1.0 - fraction) * start + fraction * target;
}

bool RenderCore::configureRamps(std::uint32_t frames) noexcept
{
    if (frameClock_ || frames > sampleRate) return false;
    rampFrames_ = frames;
    masterRamp_.snap(masterGain_);
    cueRamp_.snap(cueGain_);
    crossfaderRamps_[0].snap(std::min(1.0, 2.0 * (1.0 - double(crossfader_))));
    crossfaderRamps_[1].snap(std::min(1.0, 2.0 * double(crossfader_)));
    for (auto& deck : decks_) {
        deck.gainRamp.snap(deck.gain);
        deck.cueRamp.snap(deck.cueSelected ? 1.0 : 0.0);
        deck.last = deck.transitionStart = {};
        deck.available = false;
        deck.transition = frames && (deck.playing || deck.cuePreviewing);
        deck.transitionPosition = 0;
    }
    return true;
}

void RenderCore::transition(Deck& deck) noexcept
{
    deck.transition = rampFrames_ && ((deck.playing || deck.cuePreviewing) || deck.last[0] != 0.F || deck.last[1] != 0.F);
    deck.transitionStart = deck.last;
    deck.transitionPosition = 0;
}

bool RenderCore::renderActive(const Deck& deck) noexcept
{
    return (deck.playing || deck.cuePreviewing) && deck.frame < deck.source.frames;
}

void RenderCore::notifyActivity(const Deck& deck) noexcept
{
    if (deck.source.setRenderActive)
        deck.source.setRenderActive(deck.source.context, renderActive(deck));
}

void RenderCore::refreshActivity() const noexcept
{
    for (const auto& deck : decks_) notifyActivity(deck);
}

void RenderCore::moveCursor(Deck& deck, std::uint64_t frame) noexcept
{
    deck.buffering = false;
    if (deck.frame == frame)
        return;
    deck.frame = frame;
    transition(deck);
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
    auto& state = decks_[deck];
    const bool active = state.playing || state.cuePreviewing;
    const bool wasRenderActive = renderActive(state);
    moveCursor(state, frame);
    state.cuePreviewing = false;
    if (active != state.playing) transition(state);
    if (wasRenderActive != renderActive(state)) notifyActivity(state);
    return true;
}

bool RenderCore::restorePrimedCursor(std::size_t deck, std::uint64_t frame) noexcept
{
    if (frameClock_ || deck >= deckCount || !decks_[deck].source.frames || frame > sourceFrames(deck))
        return false;
    decks_[deck].frame = frame;
    decks_[deck].buffering = false;
    return true;
}

bool RenderCore::retrySource(std::size_t deck) noexcept
{
    if (deck >= deckCount) return false;
    auto& state = decks_[deck];
    if (!state.source.frames || state.source.isMemory() || !state.source.read || !state.source.discontinuity)
        return false;
    state.buffering = false;
    transition(state);
    state.source.discontinuity(state.source.context, state.frame);
    return true;
}

bool RenderCore::jog(std::size_t deck, std::int64_t deltaFrames) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames) return false;
    const auto current = decks_[deck].frame;
    std::uint64_t target;
    if (deltaFrames < 0) {
        const auto magnitude = std::uint64_t(-(deltaFrames + 1)) + 1U;
        target = current - std::min(current, magnitude);
    } else {
        target = current + std::min(sourceFrames(deck) - current, std::uint64_t(deltaFrames));
    }
    return seek(deck, target);
}

bool RenderCore::isDeckSilentForReplacement(std::size_t deck) const noexcept
{
    if (deck >= deckCount) return false;
    const auto& state = decks_[deck];
    return !state.playing && !state.cuePreviewing && !state.transition;
}

bool RenderCore::replaceSource(std::size_t deck, AudioSource source) noexcept
{
    if (!source.frames || !validSource(source) || !isDeckSilentForReplacement(deck)) return false;
    if (decks_[deck].source.setRenderActive)
        decks_[deck].source.setRenderActive(decks_[deck].source.context, false);
    decks_[deck] = Deck{};
    decks_[deck].source = source;
    notifyActivity(decks_[deck]);
    return true;
}

bool RenderCore::setPlaying(std::size_t deck, bool playing) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames)
        return false;
    auto& state = decks_[deck];
    const bool active = state.playing || state.cuePreviewing;
    const bool wasRenderActive = renderActive(state);
    state.playing = playing;
    state.cuePreviewing = false;
    state.buffering = false;
    if (active != playing) transition(state);
    if (wasRenderActive != renderActive(state)) notifyActivity(state);
    return true;
}

bool RenderCore::setDeckGain(std::size_t deck, float gain) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames ||
        !std::isfinite(gain) || gain < 0.0F || gain > 1.0F)
        return false;
    decks_[deck].gain = gain;
    decks_[deck].gainRamp.set(gain, rampFrames_);
    return true;
}

bool RenderCore::isStemBank(std::size_t deck) const noexcept
{
    return deck < deckCount && decks_[deck].source.frames &&
        decks_[deck].source.setStemGain && decks_[deck].source.stemGain;
}

float RenderCore::stemGain(std::size_t deck, std::size_t stem) const noexcept
{
    if (!isStemBank(deck) || stem >= stemCount) return 0.F;
    const auto& source = decks_[deck].source;
    return source.stemGain(source.context, stem);
}

bool RenderCore::setStemGain(std::size_t deck, std::size_t stem, float gain) noexcept
{
    if (!isStemBank(deck) || stem >= stemCount || !std::isfinite(gain) || gain < 0.F || gain > 1.F)
        return false;
    const auto& source = decks_[deck].source;
    return source.setStemGain(source.context, stem, gain);
}

bool RenderCore::setMasterGain(float gain) noexcept
{
    if (!std::isfinite(gain) || gain < 0.0F || gain > 1.0F)
        return false;
    masterGain_ = gain;
    masterRamp_.set(gain, rampFrames_);
    return true;
}

bool RenderCore::setCrossfader(float position) noexcept
{
    if (!std::isfinite(position) || position < 0.0F || position > 1.0F)
        return false;
    crossfader_ = position;
    crossfaderRamps_[0].set(std::min(1.0, 2.0 * (1.0 - double(position))), rampFrames_);
    crossfaderRamps_[1].set(std::min(1.0, 2.0 * double(position)), rampFrames_);
    return true;
}

bool RenderCore::setCueSelected(std::size_t deck, bool selected) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames)
        return false;
    decks_[deck].cueSelected = selected;
    decks_[deck].cueRamp.set(selected ? 1.0 : 0.0, rampFrames_);
    return true;
}

bool RenderCore::setCueGain(float gain) noexcept
{
    if (!std::isfinite(gain) || gain < 0.0F || gain > 1.0F)
        return false;
    cueGain_ = gain;
    cueRamp_.set(gain, rampFrames_);
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
    const bool active = state.playing || state.cuePreviewing;
    const bool wasRenderActive = renderActive(state);
    if (state.playing) {
        moveCursor(state, state.cueFrame);
        state.playing = false;
    } else if (state.frame != state.cueFrame) {
        state.cueFrame = state.frame;
    } else {
        state.cuePreviewing = true;
    }
    if (active != (state.playing || state.cuePreviewing)) transition(state);
    if (wasRenderActive != renderActive(state)) notifyActivity(state);
    return true;
}

bool RenderCore::cueRelease(std::size_t deck) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames)
        return false;
    auto& state = decks_[deck];
    if (!state.cueHeld)
        return true;
    const bool wasRenderActive = renderActive(state);
    state.cueHeld = false;
    if (state.cuePreviewing) {
        moveCursor(state, state.cueFrame);
        state.cuePreviewing = false;
        transition(state);
    }
    if (wasRenderActive != renderActive(state)) notifyActivity(state);
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
    case Command::Type::Crossfader: return setCrossfader(command.gain);
    case Command::Type::CuePress: return cuePress(command.deck);
    case Command::Type::CueRelease: return cueRelease(command.deck);
    case Command::Type::CueOn: return setCueSelected(command.deck, true);
    case Command::Type::CueOff: return setCueSelected(command.deck, false);
    case Command::Type::CueGain: return setCueGain(command.gain);
    case Command::Type::Jog: return jog(command.deck, command.deltaFrames);
    case Command::Type::StemGain: return setStemGain(command.deck, command.stem, command.gain);
    case Command::Type::RetrySource: return retrySource(command.deck);
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
    const auto masterGain = masterRamp_.next(rampFrames_);
    const auto cueGain = cueRamp_.next(rampFrames_);
    const auto masterLeft = static_cast<float>(std::clamp(left * masterGain, -limit, limit));
    const auto masterRight = static_cast<float>(std::clamp(right * masterGain, -limit, limit));
    const auto headphoneLeft = static_cast<float>(std::clamp(cueLeft * cueGain, -limit, limit));
    const auto headphoneRight = static_cast<float>(std::clamp(cueRight * cueGain, -limit, limit));
    master[frame * channels] = masterLeft;
    master[frame * channels + 1] = masterRight;
    if (!cue.empty()) {
        cue[frame * channels] = headphoneLeft;
        cue[frame * channels + 1] = headphoneRight;
    }
    measure(meters_.master, masterLeft, masterRight);
    measure(meters_.cue, headphoneLeft, headphoneRight);
}

void RenderCore::mixDeck(std::size_t index, bool available, float rawLeft, float rawRight,
                         double& left, double& right, double& cueLeft, double& cueRight,
                         bool& contributed, bool& cueContributed) noexcept
{
    auto& deck = decks_[index];
    if (available != deck.available) {
        transition(deck);
        deck.available = available;
    }
    std::array<float, channels> samples{available ? rawLeft : 0.F, available ? rawRight : 0.F};
    if (deck.transition) {
        if (++deck.transitionPosition >= rampFrames_) {
            deck.transition = false; // Exact raw float, including signed zero.
        } else {
            const double fraction = double(deck.transitionPosition) / rampFrames_;
            constexpr double limit = std::numeric_limits<float>::max();
            for (std::size_t channel = 0; channel < channels; ++channel)
                samples[channel] = static_cast<float>(std::clamp(
                    (1.0 - fraction) * deck.transitionStart[channel] + fraction * samples[channel], -limit, limit));
        }
    }
    deck.last = samples;
    const double gain = deck.gainRamp.next(rampFrames_);
    const double crossfaderGain = crossfaderRamps_[index].next(rampFrames_);
    const double cueSend = deck.cueRamp.next(rampFrames_);
    measure(meters_.deck[index], samples[0], samples[1]);
    if (!available && samples[0] == 0.F && samples[1] == 0.F) return;
    if (crossfaderGain != 0.0) {
        const double scale = gain * crossfaderGain;
        const double deckLeft = double(samples[0]) * scale, deckRight = double(samples[1]) * scale;
        if (contributed) { left += deckLeft; right += deckRight; }
        else { left = deckLeft; right = deckRight; contributed = true; }
    }
    if (!cueSend) return;
    const double headphoneLeft = double(samples[0]) * cueSend, headphoneRight = double(samples[1]) * cueSend;
    if (cueContributed) { cueLeft += headphoneLeft; cueRight += headphoneRight; }
    else { cueLeft = headphoneLeft; cueRight = headphoneRight; cueContributed = true; }
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
                const auto& deck = decks_[index];
                const bool supplied = frame < available[index];
                const auto source = supplied ? (deck.frame + frame) * channels : 0;
                mixDeck(index, supplied, supplied ? deck.source.memory[source] : 0.F,
                        supplied ? deck.source.memory[source + 1] : 0.F,
                        left, right, cueLeft, cueRight, contributed, cueContributed);
            }
            writeBuses(interleaved, cue, frame, left, right, cueLeft, cueRight);
        }
        for (std::size_t index = 0; index < deckCount; ++index)
            decks_[index].frame += available[index];
        frameClock_ += frames;
        finishMeters();
        refreshActivity();
        return true;
    }
    for (std::size_t i = 0; i < interleaved.size(); i += channels) {
        const float sample = audible_ ? tone_[phase_] : 0.0F;
        writeBuses(interleaved, cue, i / channels, sample, sample, 0.0, 0.0);
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
                const bool supplied = frame < available[index];
                mixDeck(index, supplied, supplied ? samples[index][frame * channels] : 0.F,
                        supplied ? samples[index][frame * channels + 1] : 0.F,
                        left, right, cueLeft, cueRight, contributed, cueContributed);
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
    refreshActivity();
    return true;
}

} // namespace conflux::audio

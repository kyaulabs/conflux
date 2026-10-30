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
    if (bool(source.residentHoldControl) != bool(source.residentHoldState) ||
        (source.residentHoldControl && (!source.frames || source.memory || !source.read || !source.context ||
            !source.setStemGain || !source.stemGain))) return false;
    if (bool(source.scratchControl) != bool(source.scratchState) ||
        (source.scratchControl && (!source.frames || source.memory || !source.read || !source.context)))
        return false;
    if (bool(source.tempoControl) != bool(source.tempoState) ||
        (source.tempoControl && (!source.frames || source.memory || !source.read || !source.context)))
        return false;
    if (bool(source.loopControl) != bool(source.loopState) ||
        (source.loopControl && (!source.frames || source.memory || !source.read || !source.context)))
        return false;
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

void RenderCore::transportIntent(Deck& deck, bool cancelHold) noexcept
{
    if (cancelHold && deck.source.residentHoldControl)
        (void)deck.source.residentHoldControl(deck.source.context,AudioResidentHoldCommand::Cancel,0,deck.frame);
    // Saturation makes further continuation ineligible rather than reusing an
    // identity. Ordinary human transport remains available.
    if (deck.transportRevision != std::numeric_limits<std::uint64_t>::max())
        ++deck.transportRevision;
}

bool RenderCore::renderActive(const Deck& deck) noexcept
{
    if (deck.source.scratchState && deck.source.scratchState(deck.source.context).active) return true;
    if (!deck.playing && !deck.cuePreviewing) return false;
    if (deck.source.tempoState) {
        const auto tempo = deck.source.tempoState(deck.source.context);
        if (tempo.active) return !tempo.finished;
    }
    return deck.frame < deck.source.frames;
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
    const bool processed = deck.source.tempoState && deck.source.tempoState(deck.source.context).active;
    const bool scratching = deck.source.scratchState && deck.source.scratchState(deck.source.context).active;
    if (deck.frame == frame && !processed && !scratching)
        return;
    deck.frame = frame;
    deck.nominalPosition = double(frame);
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
    transportIntent(state);
    const bool active = state.playing || state.cuePreviewing;
    const bool wasRenderActive = renderActive(state);
    if (state.source.loopControl)
        state.source.loopControl(state.source.context, AudioLoopCommand::Clear, frame);
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
    transportIntent(decks_[deck]);
    decks_[deck].frame = frame;
    decks_[deck].nominalPosition = double(frame);
    decks_[deck].buffering = false;
    return true;
}

bool RenderCore::retrySource(std::size_t deck) noexcept
{
    if (deck >= deckCount) return false;
    auto& state = decks_[deck];
    if (!state.source.frames || state.source.isMemory() || !state.source.read || !state.source.discontinuity)
        return false;
    transportIntent(state);
    if (state.source.loopControl)
        state.source.loopControl(state.source.context, AudioLoopCommand::Clear, state.frame);
    state.buffering = false;
    state.nominalPosition = double(state.frame);
    transition(state);
    state.source.discontinuity(state.source.context, state.frame);
    return true;
}

AudioLoopState RenderCore::loopState(std::size_t deck) const noexcept
{
    if (deck >= deckCount || !decks_[deck].source.loopState) return {};
    const auto& source = decks_[deck].source;
    return source.loopState(source.context);
}

AudioResidentHoldState RenderCore::residentHoldState(std::size_t deck) const noexcept
{
    if (deck >= deckCount || !decks_[deck].source.residentHoldState) return {};
    const auto& source = decks_[deck].source;
    return source.residentHoldState(source.context);
}
bool RenderCore::residentHoldControl(std::size_t deck, AudioResidentHoldCommand command,
                                     std::uint64_t rangeId, std::uint64_t revision) noexcept
{
    if (deck >= deckCount || revision == UINT64_MAX) return false;
    auto& state = decks_[deck];
    if (state.transportRevision != revision || !state.source.residentHoldControl ||
        !rangeId || residentHoldState(deck).rangeId != rangeId ||
        (command == AudioResidentHoldCommand::Enter && !state.playing && !state.cuePreviewing) ||
        !state.source.residentHoldControl(state.source.context,command,rangeId,state.frame)) return false;
    transportIntent(state,false); // Explicit accepted intent; do not self-cancel.
    if (command == AudioResidentHoldCommand::Cancel) transition(state);
    notifyActivity(state);
    return true;
}
AudioTempoState RenderCore::tempoState(std::size_t deck) const noexcept
{
    if (deck >= deckCount || !decks_[deck].source.tempoState) return {};
    const auto& source = decks_[deck].source;
    return source.tempoState(source.context);
}

AudioScratchState RenderCore::scratchState(std::size_t deck) const noexcept
{
    if (deck >= deckCount || !decks_[deck].source.scratchState) return {};
    const auto& source = decks_[deck].source;
    auto result = source.scratchState(source.context);
    result.scratchToken = decks_[deck].scratchToken;
    return result;
}

bool RenderCore::beginScratch(std::size_t deck, std::uint64_t token) noexcept
{
    if (deck >= deckCount || !token) return false;
    auto& state = decks_[deck];
    const auto scratch = scratchState(deck);
    const auto loop = loopState(deck);
    if (!state.source.scratchControl || !scratch.supported || scratch.active || scratch.errorCode ||
        token <= state.scratchToken || state.cueHeld || state.cuePreviewing ||
        loop.inSet || loop.outSet || loop.cached || loop.active)
        return false;
    if (!state.source.scratchControl(state.source.context, AudioScratchCommand::Begin, 0, 0, state.frame))
        return false;
    transportIntent(state);
    state.scratchToken = token;
    state.nominalPosition = double(state.frame);
    state.buffering = false;
    transition(state);
    notifyActivity(state);
    return true;
}

bool RenderCore::moveScratch(std::size_t deck, std::uint64_t token, std::int64_t cumulativeFrames,
                             double intervalSeconds) noexcept
{
    if (deck >= deckCount || !token || token != decks_[deck].scratchToken ||
        !std::isfinite(intervalSeconds) || intervalSeconds <= 0 || intervalSeconds > 1 ||
        !scratchState(deck).active)
        return false;
    auto& state = decks_[deck];
    if (!state.source.scratchControl(state.source.context, AudioScratchCommand::Move,
        cumulativeFrames, intervalSeconds, state.frame)) return false;
    transportIntent(state);
    return true;
}

bool RenderCore::endScratch(std::size_t deck, std::uint64_t token) noexcept
{
    if (deck >= deckCount || !token || token != decks_[deck].scratchToken) return false;
    auto& state = decks_[deck];
    const auto scratch = scratchState(deck);
    if (!scratch.active) { transportIntent(state); return true; }
    if (!std::isfinite(scratch.position) || scratch.position < 0 || scratch.position > double(state.source.frames) ||
        !state.source.scratchControl(state.source.context, AudioScratchCommand::End, 0, 0, state.frame))
        return false;
    transportIntent(state);
    state.frame = static_cast<std::uint64_t>(scratch.position);
    // A processed restart accepts integer source frames, as an explicit seek
    // does. Match its new generation before even a zero-frame pending read.
    state.nominalPosition = tempoState(deck).active ? double(state.frame) : scratch.position;
    state.buffering = false;
    transition(state);
    notifyActivity(state);
    return true;
}

bool RenderCore::changeTempo(std::size_t deck, AudioTempoCommand command, double percent, bool keyLock) noexcept
{
    if (deck >= deckCount || !std::isfinite(percent) || percent < -6.0 || percent > 6.0)
        return false;
    auto& state = decks_[deck];
    if (state.cuePreviewing || scratchState(deck).active || !state.source.tempoControl) return false;
    const auto before = tempoState(deck);
    if (!state.source.tempoControl(state.source.context, command, percent, keyLock, state.frame))
        return false;
    transportIntent(state);
    const auto after = tempoState(deck);
    if (before.generation != after.generation || before.active != after.active) {
        state.nominalPosition = double(state.frame);
        state.buffering = false;
        transition(state);
    }
    notifyActivity(state);
    return true;
}

bool RenderCore::setTempoPercent(std::size_t deck, double percent) noexcept
{
    return changeTempo(deck, AudioTempoCommand::Set, percent, tempoState(deck).keyLock);
}

bool RenderCore::toggleMasterTempo(std::size_t deck) noexcept
{
    const auto tempo = tempoState(deck);
    return changeTempo(deck, AudioTempoCommand::Set, tempo.percent, !tempo.keyLock);
}

bool RenderCore::resetTempo(std::size_t deck) noexcept
{
    return changeTempo(deck, AudioTempoCommand::Reset, 0, tempoState(deck).keyLock);
}

bool RenderCore::loopIn(std::size_t deck) noexcept
{
    if (deck >= deckCount) return false;
    auto& state = decks_[deck];
    const auto before = tempoState(deck);
    if (state.cuePreviewing || scratchState(deck).active || !state.source.loopControl ||
        !state.source.loopControl(state.source.context, AudioLoopCommand::In, state.frame)) return false;
    transportIntent(state);
    if (before.generation != tempoState(deck).generation) {
        state.nominalPosition = double(state.frame);
        state.buffering = false;
        transition(state);
    }
    return true;
}

bool RenderCore::loopOut(std::size_t deck) noexcept
{
    if (deck >= deckCount) return false;
    auto& state = decks_[deck];
    if (!state.playing || state.cuePreviewing || scratchState(deck).active || !state.source.loopControl ||
        !state.source.loopControl(state.source.context, AudioLoopCommand::Out, state.frame))
        return false;
    transportIntent(state);
    const bool wasActive = renderActive(state);
    state.frame = loopState(deck).inFrame;
    state.nominalPosition = double(state.frame);
    state.buffering = false;
    if (tempoState(deck).active) transition(state);
    if (wasActive != renderActive(state)) notifyActivity(state);
    return true;
}

bool RenderCore::loopToggle(std::size_t deck) noexcept
{
    if (deck >= deckCount) return false;
    auto& state = decks_[deck];
    const auto before = tempoState(deck);
    if (state.cuePreviewing || scratchState(deck).active || !state.source.loopControl ||
        !state.source.loopControl(state.source.context, AudioLoopCommand::Toggle, state.frame))
        return false;
    transportIntent(state);
    if (const auto loop = loopState(deck); loop.active) {
        const bool wasActive = renderActive(state);
        state.frame = loop.inFrame;
        state.nominalPosition = double(state.frame);
        state.playing = true;
        state.buffering = false;
        if (!wasActive) transition(state);
        notifyActivity(state);
    }
    if (before.generation != tempoState(deck).generation) {
        state.nominalPosition = double(state.frame);
        state.buffering = false;
        transition(state);
    }
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
    return !state.playing && !state.cuePreviewing && !state.transition && !scratchState(deck).active;
}

bool RenderCore::replaceSource(std::size_t deck, AudioSource source) noexcept
{
    if (!source.frames || !validSource(source) || !isDeckSilentForReplacement(deck)) return false;
    if (decks_[deck].source.loopControl)
        decks_[deck].source.loopControl(decks_[deck].source.context, AudioLoopCommand::Clear, decks_[deck].frame);
    if (decks_[deck].source.setRenderActive)
        decks_[deck].source.setRenderActive(decks_[deck].source.context, false);
    transportIntent(decks_[deck]);
    const auto revision = decks_[deck].transportRevision;
    auto eq = decks_[deck].eq;
    eq.clearState();
    decks_[deck] = Deck{};
    decks_[deck].transportRevision = revision;
    decks_[deck].eq = eq;
    decks_[deck].source = source;
    notifyActivity(decks_[deck]);
    return true;
}

bool RenderCore::canContinueSource(std::size_t deck, AudioSource source,
    std::uint64_t nextFrame, std::uint64_t revision) const noexcept
{
    return continuationStatus(deck, source, nextFrame, revision) == ContinuationStatus::Ready;
}

RenderCore::ContinuationStatus RenderCore::continuationStatus(std::size_t deck, AudioSource source,
    std::uint64_t nextFrame, std::uint64_t revision, std::uint64_t rangeId) const noexcept
{
    if (deck >= deckCount || !validSource(source)) return ContinuationStatus::Invalid;
    const auto& state = decks_[deck];
    if (!isStemBank(deck) || !source.setStemGain || !source.stemGain ||
        source.frames != state.source.frames || nextFrame >= source.frames ||
        state.frame != nextFrame || state.nominalPosition != double(nextFrame) ||
        state.transportRevision != revision || revision == std::numeric_limits<std::uint64_t>::max() ||
        (source.context == state.source.context && source.read == state.source.read))
        return ContinuationStatus::Invalid;
    const auto linear = [](const AudioSource& candidate, bool allowHold) noexcept {
        const auto tempo = candidate.tempoState ? candidate.tempoState(candidate.context) : AudioTempoState{};
        const auto scratch = candidate.scratchState ? candidate.scratchState(candidate.context) : AudioScratchState{};
        const auto loop = candidate.loopState ? candidate.loopState(candidate.context) : AudioLoopState{};
        // Inactive scratch prefetch may be pending independently of linear
        // playback. Begin marks a gesture active even while its window waits.
        const auto hold = candidate.residentHoldState ? candidate.residentHoldState(candidate.context) : AudioResidentHoldState{};
        return !tempo.active && !tempo.pending && !tempo.keyLock && tempo.percent == 0 && tempo.appliedPercent == 0 &&
            !scratch.active && !loop.inSet && !loop.outSet && !loop.active && !loop.cached &&
            (allowHold || (!hold.armed && !hold.active));
    };
    if (!linear(state.source, rangeId != 0) || !linear(source, false)) return ContinuationStatus::Invalid;
    if (!rangeId) return ContinuationStatus::Ready;
    const auto hold = residentHoldState(deck);
    if (!renderActive(state) || !hold.supported || !hold.ready || !hold.armed || !hold.active || hold.exitPending ||
        hold.rangeId != rangeId || hold.linearFrame != nextFrame || hold.inFrame >= hold.outFrame ||
        hold.outFrame > state.source.frames || hold.outFrame-hold.inFrame < AudioResidentHoldRange::minimumFrames ||
        hold.outFrame-hold.inFrame > AudioResidentHoldRange::maximumFrames ||
        hold.residentBytes != (hold.outFrame-hold.inFrame)*AudioResidentHoldRange::bytesPerFrame ||
        hold.audibleFrame < hold.inFrame || hold.audibleFrame > hold.outFrame)
        return ContinuationStatus::Invalid;
    return hold.audibleFrame == hold.outFrame ? ContinuationStatus::Ready : ContinuationStatus::WaitingForHoldOut;
}

bool RenderCore::continueSource(std::size_t deck, AudioSource source,
    std::uint64_t nextFrame, std::uint64_t revision, std::uint64_t rangeId) noexcept
{
    if (continuationStatus(deck, source, nextFrame, revision, rangeId) != ContinuationStatus::Ready) return false;
    auto& state = decks_[deck];
    if (rangeId) {
        // Resident PCM can remain healthy after its producer failed, without a
        // failed render read. Retain that cumulative fault after owner retirement.
        state.readFailed |= residentHoldState(deck).errorCode != 0;
        (void)state.source.residentHoldControl(state.source.context, AudioResidentHoldCommand::Cancel, 0, nextFrame);
    }
    if (state.source.setRenderActive)
        state.source.setRenderActive(state.source.context, false);
    state.source = source;
    state.buffering = false;
    if (rangeId) transition(state); // Same envelope as the existing hold-to-live OUT.
    transportIntent(state);
    notifyActivity(state);
    return true;
}

bool RenderCore::setPlaying(std::size_t deck, bool playing) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames)
        return false;
    auto& state = decks_[deck];
    if (scratchState(deck).active && !endScratch(deck, state.scratchToken)) return false;
    transportIntent(state);
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

bool RenderCore::setEqGainDb(std::size_t deck, std::size_t band, float db) noexcept
{
    return deck < deckCount && decks_[deck].eq.setGain(band, db, rampFrames_);
}

float RenderCore::eqGainDb(std::size_t deck, std::size_t band) const noexcept
{
    return deck < deckCount ? decks_[deck].eq.gainDb(band) : 0.F;
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
    transportIntent(state);
    if (state.cueHeld)
        return true;
    state.cueHeld = true;
    const bool active = state.playing || state.cuePreviewing;
    const bool wasRenderActive = renderActive(state);
    if (state.source.loopControl)
        state.source.loopControl(state.source.context, AudioLoopCommand::Clear,
                                 state.playing ? state.cueFrame : state.frame);
    if (state.playing) {
        moveCursor(state, state.cueFrame);
        state.playing = false;
    } else if (state.frame != state.cueFrame) {
        state.cueFrame = state.frame;
        moveCursor(state, state.frame);
    } else {
        moveCursor(state, state.frame);
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
    transportIntent(state);
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
    transportIntent(decks_[deck]);
    decks_[deck].cueFrame = frame;
    return true;
}

bool RenderCore::hotCue(std::size_t deck, std::size_t slot) noexcept
{
    if (deck >= deckCount || slot >= HotCues::count || !decks_[deck].source.frames)
        return false;
    auto& state = decks_[deck];
    if (!state.hotCues.set[slot]) {
        if (state.frame >= sourceFrames(deck) || scratchState(deck).active || loopState(deck).active)
            return false;
        state.hotCues.frames[slot] = state.frame;
        state.hotCues.set[slot] = true;
        return true;
    }
    // Both calls run on the render owner without an intervening audio block.
    // Seek clears loop/preview state and notifies the existing read-ahead/DSP
    // source; Play uses the existing transport envelope and activity contract.
    return seek(deck, state.hotCues.frames[slot]) && setPlaying(deck, true);
}

bool RenderCore::clearHotCue(std::size_t deck, std::size_t slot) noexcept
{
    if (deck >= deckCount || slot >= HotCues::count || !decks_[deck].source.frames)
        return false;
    decks_[deck].hotCues.set[slot] = false;
    decks_[deck].hotCues.frames[slot] = 0;
    return true;
}

bool RenderCore::restoreHotCues(std::size_t deck, const HotCues& cues) noexcept
{
    if (deck >= deckCount || !decks_[deck].source.frames)
        return false;
    for (std::size_t slot = 0; slot < HotCues::count; ++slot)
        if (cues.set[slot] && cues.frames[slot] >= sourceFrames(deck))
            return false;
    decks_[deck].hotCues = cues;
    return true;
}

bool RenderCore::apply(const Command& command) noexcept
{
    switch (command.type) {
    case Command::Type::ResidentHoldArm: return residentHoldControl(command.deck,AudioResidentHoldCommand::Arm,command.residentHoldToken,command.transportRevision);
    case Command::Type::ResidentHoldEnter: return residentHoldControl(command.deck,AudioResidentHoldCommand::Enter,command.residentHoldToken,command.transportRevision);
    case Command::Type::ResidentHoldExit: return residentHoldControl(command.deck,AudioResidentHoldCommand::Exit,command.residentHoldToken,command.transportRevision);
    case Command::Type::ResidentHoldCancel: return residentHoldControl(command.deck,AudioResidentHoldCommand::Cancel,command.residentHoldToken,command.transportRevision);
    case Command::Type::Play: return setPlaying(command.deck, true);
    case Command::Type::Pause: return setPlaying(command.deck, false);
    case Command::Type::Seek: return seek(command.deck, command.frame);
    case Command::Type::DeckGain: return setDeckGain(command.deck, command.gain);
    case Command::Type::EqGain: return setEqGainDb(command.deck, command.band, command.eqGainDb);
    case Command::Type::MasterGain: return setMasterGain(command.gain);
    case Command::Type::Crossfader: return setCrossfader(command.gain);
    case Command::Type::CuePress: return cuePress(command.deck);
    case Command::Type::CueRelease: return cueRelease(command.deck);
    case Command::Type::HotCue: return hotCue(command.deck, command.hotCue);
    case Command::Type::HotCueClear: return clearHotCue(command.deck, command.hotCue);
    case Command::Type::CueOn: return setCueSelected(command.deck, true);
    case Command::Type::CueOff: return setCueSelected(command.deck, false);
    case Command::Type::CueGain: return setCueGain(command.gain);
    case Command::Type::Jog: return jog(command.deck, command.deltaFrames);
    case Command::Type::StemGain: return setStemGain(command.deck, command.stem, command.gain);
    case Command::Type::RetrySource: return retrySource(command.deck);
    case Command::Type::LoopIn: return loopIn(command.deck);
    case Command::Type::LoopOut: return loopOut(command.deck);
    case Command::Type::LoopToggle: return loopToggle(command.deck);
    case Command::Type::TempoPercent: return setTempoPercent(command.deck, command.tempoPercent);
    case Command::Type::MasterTempo: return toggleMasterTempo(command.deck);
    case Command::Type::TempoReset: return resetTempo(command.deck);
    case Command::Type::ScratchBegin: return beginScratch(command.deck, command.scratchToken);
    case Command::Type::ScratchMove: return moveScratch(command.deck, command.scratchToken,
        command.deltaFrames, command.scratchIntervalSeconds);
    case Command::Type::ScratchEnd: return endScratch(command.deck, command.scratchToken);
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
    const auto filtered = deck.eq.process(available ? rawLeft : 0.F, available ? rawRight : 0.F);
    // The transport envelope follows EQ, so pauses/outages fade the last audible
    // filtered sample once, without leaking filter tails after the fade ends.
    std::array<float, channels> samples{available ? filtered[0] : 0.F, available ? filtered[1] : 0.F};
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
        for (std::size_t index = 0; index < deckCount; ++index) {
            decks_[index].frame += available[index];
            decks_[index].nominalPosition = double(decks_[index].frame);
        }
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
        if (renderActive(decks_[index]))
            needed[index] = (scratchState(index).active || tempoState(index).active || loopState(index).active || residentHoldState(index).active) ? frames :
                std::min<std::uint64_t>(frames, sourceFrames(index) - decks_[index].frame);

    for (std::size_t offset = 0; offset < frames;) {
        auto count = std::min(sourceBlockFrames, frames - offset);
        // Split both buses at either deck's cached OUT. The source's normal
        // reader remains parked there while repetitions use resident PCM.
        for (std::size_t index = 0; index < deckCount; ++index) {
            const auto& deck = decks_[index];
            if (stalled[index] || delivered[index] == needed[index]) continue;
            const auto loop = loopState(index);
            if (loop.cached && deck.frame < loop.outFrame)
                count = std::min<std::size_t>(count, loop.outFrame - deck.frame);
            const auto hold = residentHoldState(index);
            // A ready armed source can become an unbounded resident hold at a
            // shortage near finite EOF. Split here so a newly active hold may
            // extend the callback without leaving a clipped-suffix hole.
            if (hold.armed && hold.ready && !hold.active && deck.frame < deck.source.frames)
                count = std::min<std::size_t>(count, deck.source.frames - deck.frame);
        }
        std::array<std::size_t, deckCount> available{};
        std::array<AudioReadResult, deckCount> results{};
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
                const auto beforeHold = residentHoldState(index);
                auto result = deck.source.read(deck.source.context, deck.frame, buffer);
                const auto afterHold = residentHoldState(index);
                const bool resident = result.residentHold && beforeHold.supported && afterHold.supported &&
                    (beforeHold.active || afterHold.active);
                const bool processed = tempoState(index).active;
                const bool scratching = scratchState(index).active;
                const auto loop = loopState(index);
                const bool processedLoop = processed && loop.supported && loop.ready && loop.active && !loop.cached &&
                    loop.inFrame < loop.outFrame && loop.outFrame <= deck.source.frames &&
                    loop.outFrame - loop.inFrame >= AudioLoopState::minimumFrames &&
                    loop.outFrame - loop.inFrame <= AudioLoopState::maximumFrames;
                bool badLoopPosition = (result.loopWraps && !processedLoop) ||
                    (processed && loop.active && !processedLoop);
                if (processedLoop) {
                    const double length = double(loop.outFrame - loop.inFrame);
                    const double advance = result.nominalPosition - deck.nominalPosition + double(result.loopWraps) * length;
                    const double tolerance = 1e-6 + 8 * std::numeric_limits<double>::epsilon() *
                        std::max(double(deck.source.frames), length);
                    badLoopPosition = result.finished || result.residentHold || scratching ||
                        deck.nominalPosition < double(loop.inFrame) || deck.nominalPosition >= double(loop.outFrame) ||
                        result.nominalPosition < double(loop.inFrame) || result.nominalPosition >= double(loop.outFrame) ||
                        result.loopWraps > 1 + (result.frames * 106U / 100U) / AudioLoopState::minimumFrames ||
                        advance < .94 * double(result.frames) - tolerance ||
                        advance > 1.06 * double(result.frames) + tolerance;
                }
                const double residentLimit = deck.nominalPosition+double(result.frames)*(processed ? 1.06 : 1.)+
                    (processed ? 8*std::numeric_limits<double>::epsilon()*std::max(1.,deck.nominalPosition) : 0.);
                if (badLoopPosition || ((processed || scratching || result.residentHold) && !result.positionValid) ||
                    (result.residentHold && (!resident ||
                        (!processed && result.nominalPosition != std::floor(result.nominalPosition)) ||
                        (afterHold.active && result.nominalPosition != deck.nominalPosition && beforeHold.active) ||
                        result.nominalPosition > residentLimit)) ||
                    (result.transitionFrame != AudioReadResult::noTransition &&
                        (!resident || result.transitionFrame >= result.frames)) ||
                    (result.positionValid && ((!processed && !scratching && !resident) || !std::isfinite(result.nominalPosition) ||
                        (!scratching && !processedLoop && result.nominalPosition < deck.nominalPosition) || result.nominalPosition < 0 ||
                        result.nominalPosition > double(deck.source.frames) ||
                        (!result.frames && !result.finished && result.nominalPosition != deck.nominalPosition))) ||
                    (result.finished && !processed && !resident) || result.frames > requested || std::ranges::any_of(
                        buffer.first(std::min(result.frames, requested) * channels),
                        [](float sample) { return !std::isfinite(sample); }))
                    result = {0, true};
                samples[index] = buffer.data();
                available[index] = result.frames;
                results[index] = result;
                if (result.residentHold && resident && afterHold.active) needed[index] = frames;
                if (result.finished) needed[index] = delivered[index] + result.frames;
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
                if (supplied && results[index].transitionFrame == frame) transition(decks_[index]);
                mixDeck(index, supplied, supplied ? samples[index][frame * channels] : 0.F,
                        supplied ? samples[index][frame * channels + 1] : 0.F,
                        left, right, cueLeft, cueRight, contributed, cueContributed);
            }
            writeBuses(interleaved, cue, offset + frame, left, right, cueLeft, cueRight);
        }
        for (std::size_t index = 0; index < deckCount; ++index) {
            auto& deck = decks_[index];
            if (results[index].positionValid) {
                deck.nominalPosition = results[index].nominalPosition;
                deck.frame = static_cast<std::uint64_t>(deck.nominalPosition);
            } else if (available[index]) {
                deck.frame += available[index];
                deck.nominalPosition = double(deck.frame);
            }
            delivered[index] += available[index];
            const auto loop = loopState(index);
            if (loop.active && loop.cached) {
                if (deck.frame == loop.outFrame) deck.frame = loop.inFrame;
                deck.nominalPosition = double(deck.frame);
            }
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

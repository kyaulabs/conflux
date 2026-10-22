// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/StemBankSource.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace conflux::audio {

StemBankSource::StemBankSource(std::array<AudioSource, stemCount> stems, std::uint64_t initialFrame)
    : cursor_(initialFrame)
{
    constexpr auto maximumFrames = std::numeric_limits<std::size_t>::max() / (channels * sizeof(float));
    frames_ = stems.front().frames;
    if (!frames_ || frames_ > maximumFrames)
        throw std::invalid_argument("Stem bank requires nonempty addressable stereo PCM");
    if (initialFrame > frames_) throw std::invalid_argument("Stem bank initial cursor exceeds source frames");
    for (std::size_t stem = 0; stem < stemCount; ++stem) {
        const auto source = stems[stem];
        if (source.frames != frames_ || bool(source.memory) == bool(source.read))
            throw std::invalid_argument("Stem bank requires four equally sized sources with exactly one reader each");
        stems_[stem].source = source;
    }
}

AudioSource StemBankSource::sourceRef() noexcept
{
    return {frames_, nullptr, this,
        [](void* context, std::uint64_t frame, std::span<float> output) noexcept {
            return static_cast<StemBankSource*>(context)->read(frame, output);
        },
        [](void* context, std::uint64_t frame) noexcept {
            static_cast<StemBankSource*>(context)->discontinuity(frame);
        },
        [](void* context, std::size_t stem, float gain) noexcept {
            return static_cast<StemBankSource*>(context)->setGain(stem, gain);
        },
        [](const void* context, std::size_t stem) noexcept {
            return static_cast<const StemBankSource*>(context)->gain(stem);
        }};
}

void StemBankSource::GainRamp::snap() noexcept
{
    current = start = target;
    position = 0;
}

void StemBankSource::GainRamp::set(double value, std::uint32_t frames) noexcept
{
    if (!frames) {
        target = value;
        snap();
    } else if (value != target) {
        start = current;
        target = value;
        position = 0;
    }
}

double StemBankSource::GainRamp::next(std::uint32_t frames) noexcept
{
    if (!frames || current == target) return current = target;
    if (++position >= frames) return current = target;
    const double fraction = double(position) / frames;
    return current = (1.0 - fraction) * start + fraction * target;
}

bool StemBankSource::configureRamps(std::uint32_t frames) noexcept
{
    if (started_ || frames > sampleRate) return false;
    rampFrames_ = frames;
    for (auto& gain : gains_) gain.snap();
    return true;
}

bool StemBankSource::setGain(std::size_t stem, float gain) noexcept
{
    if (stem >= stemCount || !std::isfinite(gain) || gain < 0.F || gain > maxGain) return false;
    gains_[stem].set(gain, rampFrames_);
    return true;
}

float StemBankSource::gain(std::size_t stem) const noexcept
{
    return stem < stemCount ? static_cast<float>(gains_[stem].target) : 0.F;
}

bool StemBankSource::hasReadFailure() const noexcept
{
    return std::ranges::any_of(stems_, [](const StagedStem& stem) { return stem.failed; });
}

AudioReadResult StemBankSource::readCached(
    const std::array<std::span<const float>, stemCount>& leaves,
    std::uint64_t offsetFrames, std::span<float> output) noexcept
{
    if (output.size() % channels || output.size() / channels > blockFrames ||
        leaves.front().size() % channels || offsetFrames > leaves.front().size() / channels)
        return {0, true};
    if (std::ranges::any_of(leaves, [&](const auto leaf) { return leaf.size() != leaves.front().size(); }))
        return {0, true};
    if (output.empty()) return {};
    started_ = true;
    if (hasReadFailure()) return {0, true};
    const auto count = std::min(output.size() / channels,
        leaves.front().size() / channels - static_cast<std::size_t>(offsetFrames));
    std::array<std::span<const float>, stemCount> inputs{};
    for (std::size_t stem = 0; stem < stemCount; ++stem) {
        inputs[stem] = leaves[stem].subspan(static_cast<std::size_t>(offsetFrames) * channels, count * channels);
        if (std::ranges::any_of(inputs[stem], [](float sample) { return !std::isfinite(sample); }))
            return {0, true};
    }
    mix(inputs, count, output);
    return {count, false};
}

void StemBankSource::mix(const std::array<std::span<const float>, stemCount>& inputs,
                         std::size_t frames, std::span<float> output) noexcept
{
    constexpr double limit = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < frames; ++index) {
        std::array<double, channels> mixed{};
        for (std::size_t stem = 0; stem < stemCount; ++stem) {
            const double gain = gains_[stem].next(rampFrames_);
            for (std::size_t channel = 0; channel < channels; ++channel)
                mixed[channel] += double(inputs[stem][index * channels + channel]) * gain;
        }
        for (std::size_t channel = 0; channel < channels; ++channel)
            output[index * channels + channel] = static_cast<float>(std::clamp(mixed[channel], -limit, limit));
    }
}

void StemBankSource::discontinuity(std::uint64_t frame) noexcept
{
    if (frame > frames_) return;
    cursor_ = frame;
    // No output can be read between these notifications: there is one render
    // owner. Workers may prepare independently, but only a shared prefix commits.
    for (auto& stem : stems_) {
        stem.offset = stem.count = 0;
        stem.failed = false;
        if (stem.source.discontinuity)
            stem.source.discontinuity(stem.source.context, frame);
    }
}

void StemBankSource::fill(StagedStem& stem, std::size_t wanted) noexcept
{
    if (stem.failed || stem.count >= wanted) return;
    if (stem.offset) {
        std::copy_n(stem.samples.data() + stem.offset * channels, stem.count * channels, stem.samples.data());
        stem.offset = 0;
    }
    const auto first = cursor_ + stem.count;
    const auto missing = wanted - stem.count;
    auto output = std::span(stem.samples).subspan(stem.count * channels, missing * channels);
    AudioReadResult result;
    if (stem.source.isMemory()) {
        std::copy_n(stem.source.memory + first * channels, output.size(), output.data());
        result.frames = missing;
    } else {
        // At most one bounded provider call per stem per bank read. A short
        // provider is retried by a later render call, never a callback busy loop.
        result = stem.source.read(stem.source.context, first, output);
    }
    if (result.frames > missing || std::ranges::any_of(
            output.first(std::min(result.frames, missing) * channels),
            [](float sample) { return !std::isfinite(sample); })) {
        // The provider may already have advanced. Do not use its malformed
        // prefix or retry an old position; only a new discontinuity recovers it.
        stem.failed = true;
        return;
    }
    stem.count += result.frames;
    stem.failed = result.failed;
}

AudioReadResult StemBankSource::read(std::uint64_t frame, std::span<float> output) noexcept
{
    if (output.size() % channels || output.size() / channels > blockFrames || frame > frames_)
        return {0, true};
    if (output.empty()) return {};
    started_ = true;
    if (frame != cursor_) discontinuity(frame);
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(output.size() / channels, frames_ - cursor_));
    std::size_t common = wanted;
    bool failed = false;
    for (auto& stem : stems_) {
        fill(stem, wanted);
        common = std::min(common, stem.count);
        failed |= stem.failed;
    }
    std::array<std::span<const float>, stemCount> inputs{};
    for (std::size_t stem = 0; stem < stemCount; ++stem)
        inputs[stem] = std::span(stems_[stem].samples).subspan(stems_[stem].offset * channels, common * channels);
    mix(inputs, common, output);
    for (auto& stem : stems_) {
        stem.offset += common;
        stem.count -= common;
    }
    cursor_ += common;
    return {common, failed};
}

} // namespace conflux::audio

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace conflux::audio {

// One aligned deck source, in drums/bass/other/vocals order. Borrowed sources
// must already contain validated stereo 48 kHz PCM and equal nonzero lengths.
// They and this bank must outlive every borrowed descriptor/render callback.
// Construction validates descriptors/geometry without reading PCM or doing I/O;
// this class neither owns readers nor validates/publishes a persistent cache.
//
// All methods belong to the render owner or execute before streaming. Reads
// retain faster stems' raw surplus and commit only their common prefix. Muted
// stems still participate; a missing stem never substitutes an original mix.
// Callback work/storage is bounded; no allocation, blocking or I/O occurs here.
class StemBankSource {
public:
    static constexpr std::size_t stemCount = 4;
    static constexpr std::size_t blockFrames = 1024;
    static constexpr std::size_t channels = 2;
    static constexpr std::uint32_t sampleRate = 48000;
    static constexpr float maxGain = 1.F;

    // Sources are already positioned at initialFrame; empty staging adopts
    // that cursor without reading or notifying any borrowed provider.
    explicit StemBankSource(std::array<AudioSource, stemCount> stems, std::uint64_t initialFrame = 0);
    StemBankSource(const StemBankSource&) = delete;
    StemBankSource& operator=(const StemBankSource&) = delete;
    StemBankSource(StemBankSource&&) = delete;
    StemBankSource& operator=(StemBankSource&&) = delete;

    [[nodiscard]] AudioSource sourceRef() noexcept;
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t sourceFrame() const noexcept { return cursor_; }

    // Same linear target/ramp convention as RenderCore: default zero preserves
    // exact static gains; configure before the first nonempty valid read, up to
    // one second. Ramps advance only for committed source frames, not starvation.
    bool configureRamps(std::uint32_t frames) noexcept;
    // Same render owner, or both banks quiesced. Copy only the exact current
    // gain trajectories and ramp duration into an unread, equally sized bank.
    // Preserve this bank's providers, primed cursor, staging and source health.
    bool copyMixStateFrom(const StemBankSource& source) noexcept;
    // Index order: 0 drums, 1 bass, 2 other, 3 vocals. Gain targets are finite
    // [0,1]; summation still preserves headroom above +/-1 with double arithmetic.
    bool setGain(std::size_t stem, float gain) noexcept;
    [[nodiscard]] float gain(std::size_t stem) const noexcept;
    // Render-owner resident reads share the same gain ramps while the linear
    // providers, staged surplus and cursor remain parked for loop exit.
    [[nodiscard]] AudioReadResult readCached(
        const std::array<std::span<const float>, stemCount>& leaves,
        std::uint64_t offsetFrames, std::span<float> output) noexcept;
    [[nodiscard]] bool hasReadFailure() const noexcept;

private:
    struct GainRamp {
        double current = 1.0, start = 1.0, target = 1.0;
        std::uint32_t position = 0;
        void set(double value, std::uint32_t frames) noexcept;
        double next(std::uint32_t frames) noexcept;
        void snap() noexcept;
    };
    struct StagedStem {
        AudioSource source{};
        std::array<float, blockFrames * channels> samples{};
        std::size_t offset = 0, count = 0;
        // A valid prefix can accompany failure. Retain that prefix and report
        // failure until a discontinuity begins a fresh reader generation.
        bool failed = false;
    };

    AudioReadResult read(std::uint64_t frame, std::span<float> output) noexcept;
    void discontinuity(std::uint64_t frame) noexcept;
    void fill(StagedStem& stem, std::size_t wanted) noexcept;
    void mix(const std::array<std::span<const float>, stemCount>& inputs,
             std::size_t frames, std::span<float> output) noexcept;

    std::array<StagedStem, stemCount> stems_{};
    std::array<GainRamp, stemCount> gains_{};
    std::uint64_t frames_ = 0, cursor_ = 0;
    std::uint32_t rampFrames_ = 0;
    bool started_ = false;
};

} // namespace conflux::audio

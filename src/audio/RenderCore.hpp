// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace conflux::audio {

// Shared by the live probe and headless rendering. Construct before processing.
// State access belongs to the render owner, or occurs before/after streaming.
class RenderCore {
public:
    static constexpr std::uint32_t sampleRate = 48000;
    static constexpr std::uint32_t channels = 2;
    static constexpr float amplitude = 0.01F;
    static constexpr std::size_t deckCount = 2;

    struct Command {
        enum class Type { Play, Pause, Seek, DeckGain, MasterGain };
        Type type = Type::Play;
        std::uint32_t deck = 0;
        std::uint64_t frame = 0;
        float gain = 1.0F;
        std::uint32_t id = 0;
    };

    explicit RenderCore(bool tone = false);
    // Borrow immutable stereo 48 kHz PCM. Its storage must outlive this renderer.
    explicit RenderCore(std::span<const float> pcm);
    // One deck may be empty/unloaded; at least one must contain valid PCM.
    RenderCore(std::span<const float> deckA, std::span<const float> deckB);
    // Reject incomplete stereo frames without writing or advancing the clock.
    // Mix with double intermediates, preserving headroom above +/-1. Only
    // representation overflow saturates at the finite float limits.
    bool render(std::span<float> interleaved) noexcept;
    // Only the render owner may seek, between render calls or before streaming.
    // The output frame clock remains monotonic across source seeks and EOF.
    bool seek(std::uint64_t frame) noexcept;
    bool seek(std::size_t deck, std::uint64_t frame) noexcept;
    bool setPlaying(std::size_t deck, bool playing) noexcept;
    bool setDeckGain(std::size_t deck, float gain) noexcept;
    bool setMasterGain(float gain) noexcept;
    bool apply(const Command& command) noexcept;
    [[nodiscard]] std::uint64_t frameClock() const noexcept { return frameClock_; }
    [[nodiscard]] std::uint64_t sourceFrame(std::size_t deck = 0) const noexcept
    { return deck < deckCount ? decks_[deck].frame : 0; }
    [[nodiscard]] std::uint64_t sourceFrames(std::size_t deck = 0) const noexcept
    { return deck < deckCount ? decks_[deck].source.size() / channels : 0; }
    [[nodiscard]] bool isPlaying(std::size_t deck = 0) const noexcept
    { return deck < deckCount && decks_[deck].playing; }
    [[nodiscard]] float deckGain(std::size_t deck = 0) const noexcept
    { return deck < deckCount && !decks_[deck].source.empty() ? decks_[deck].gain : 0.0F; }
    [[nodiscard]] float masterGain() const noexcept { return masterGain_; }

private:
    struct Deck {
        std::span<const float> source{};
        std::uint64_t frame = 0;
        float gain = 1.0F;
        bool playing = false;
    };
    std::array<float, sampleRate> tone_{};
    std::array<Deck, deckCount> decks_{};
    std::uint64_t frameClock_ = 0;
    std::uint32_t phase_ = 0;
    float masterGain_ = 1.0F;
    bool audible_;
};

} // namespace conflux::audio

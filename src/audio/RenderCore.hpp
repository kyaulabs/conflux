// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"

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
    static constexpr std::size_t stemCount = 4;

    struct StereoMeter {
        std::array<double, channels> peak{}, rms{};
        std::array<std::uint64_t, channels> overrangeSamples{};
    };
    struct Meters {
        std::array<StereoMeter, deckCount> deck{};
        StereoMeter master{}, cue{};
        std::uint64_t blockFrames = 0;
    };

    struct Command {
        enum class Type { Play, Pause, Seek, DeckGain, MasterGain, CuePress, CueRelease, CueOn, CueOff, CueGain, Jog, StemGain, RetrySource, Crossfader };
        Type type = Type::Play;
        std::uint32_t deck = 0;
        std::uint64_t frame = 0;
        float gain = 1.0F;
        std::uint32_t id = 0;
        std::int64_t deltaFrames = 0;
        std::uint32_t stem = 0;
    };

    explicit RenderCore(bool tone = false);
    // Borrow immutable stereo 48 kHz PCM. Its storage must outlive this renderer.
    explicit RenderCore(std::span<const float> pcm);
    // One deck may be empty/unloaded; at least one must contain valid PCM.
    RenderCore(std::span<const float> deckA, std::span<const float> deckB);
    // Borrow validated sources for the renderer's whole lifetime. Stream reads
    // are bounded to 1024 stereo frames and never allocate, block or perform I/O.
    explicit RenderCore(AudioSource source);
    RenderCore(AudioSource deckA, AudioSource deckB);
    // Configure before the first nonempty render. Zero preserves exact legacy
    // samples; live output uses 240 frames (5 ms), bounded here to one second.
    bool configureRamps(std::uint32_t frames) noexcept;
    // Reject incomplete stereo frames without writing or advancing the clock.
    // Mix with double intermediates, preserving headroom above +/-1. Only
    // representation overflow saturates at the finite float limits.
    bool render(std::span<float> interleaved) noexcept;
    // Separate stereo buses in one engine pass. Buffers must be equal-sized,
    // complete stereo and disjoint; invalid/empty calls preserve all state.
    bool renderBuses(std::span<float> master, std::span<float> cue) noexcept;
    // Only the render owner may seek, between render calls or before streaming.
    // The output frame clock remains monotonic across source seeks and EOF.
    bool seek(std::uint64_t frame) noexcept;
    bool seek(std::size_t deck, std::uint64_t frame) noexcept;
    // Pre-first-render only; caller already primed this exact source cursor.
    // No discontinuity notification or output-clock/transport change.
    bool restorePrimedCursor(std::size_t deck, std::uint64_t frame) noexcept;
    // Render owner: explicitly invalidate a callback source at the same cursor,
    // preserving transport/cue/gains and cumulative diagnostics.
    bool retrySource(std::size_t deck) noexcept;
    // Positional search, not scratch or speed control. Clamp from the owner's
    // current cursor; preserve play request/cue and cancel temporary preview.
    bool jog(std::size_t deck, std::int64_t deltaFrames) noexcept;
    // Caller validates finite PCM and retains its owner. Only an inactive deck
    // whose decay has completed may swap; no scan, allocation or ownership here.
    bool replaceSource(std::size_t deck, AudioSource source) noexcept;
    [[nodiscard]] bool isDeckSilentForReplacement(std::size_t deck) const noexcept;
    bool setPlaying(std::size_t deck, bool playing) noexcept;
    bool setDeckGain(std::size_t deck, float gain) noexcept;
    // Fixed bank targets are owned by the source; normal/unloaded decks reject.
    bool setStemGain(std::size_t deck, std::size_t stem, float gain) noexcept;
    [[nodiscard]] bool isStemBank(std::size_t deck = 0) const noexcept;
    [[nodiscard]] float stemGain(std::size_t deck, std::size_t stem) const noexcept;
    bool setMasterGain(float gain) noexcept;
    // Fixed A-left/B-right crossfader. Center preserves both unity sends;
    // channel gains, pre-fader cue and source clocks remain independent.
    bool setCrossfader(float position) noexcept;
    // Headphone cue is independent of temporary transport CUE: selected active
    // decks sum before channel/master gains, then receive this global cue gain.
    bool setCueSelected(std::size_t deck, bool selected) noexcept;
    bool setCueGain(float gain) noexcept;
    // CUE uses the owner's exact cursor. Press during playback returns to cue
    // and pauses; a paused press away from cue sets it, otherwise it auditions.
    // Release returns an unlatched audition to cue. PLAY latches the audition;
    // PAUSE/SEEK cancel it at the current/new cursor without changing the cue.
    bool cuePress(std::size_t deck) noexcept;
    bool cueRelease(std::size_t deck) noexcept;
    // Restore a stored cue before/after streaming, without seeking or playing.
    // Newly constructed decks have a temporary cue at frame zero, including
    // leading silence. EOF is a valid stored cue, as it is for source seeks.
    bool setCueFrame(std::size_t deck, std::uint64_t frame) noexcept;
    bool apply(const Command& command) noexcept;
    [[nodiscard]] std::uint64_t frameClock() const noexcept { return frameClock_; }
    [[nodiscard]] std::uint64_t sourceFrame(std::size_t deck = 0) const noexcept
    { return deck < deckCount ? decks_[deck].frame : 0; }
    [[nodiscard]] std::uint64_t sourceFrames(std::size_t deck = 0) const noexcept
    { return deck < deckCount ? decks_[deck].source.frames : 0; }
    [[nodiscard]] bool isPlaying(std::size_t deck = 0) const noexcept
    { return deck < deckCount && decks_[deck].playing; }
    [[nodiscard]] std::uint64_t cueFrame(std::size_t deck = 0) const noexcept
    { return deck < deckCount ? decks_[deck].cueFrame : 0; }
    [[nodiscard]] bool isCueHeld(std::size_t deck = 0) const noexcept
    { return deck < deckCount && decks_[deck].cueHeld; }
    [[nodiscard]] bool isCuePreviewing(std::size_t deck = 0) const noexcept
    { return deck < deckCount && decks_[deck].cuePreviewing; }
    [[nodiscard]] float deckGain(std::size_t deck = 0) const noexcept
    { return deck < deckCount && decks_[deck].source.frames ? decks_[deck].gain : 0.0F; }
    [[nodiscard]] bool isBuffering(std::size_t deck = 0) const noexcept
    { return deck < deckCount && decks_[deck].buffering; }
    [[nodiscard]] std::uint64_t starvedFrames(std::size_t deck = 0) const noexcept
    { return deck < deckCount ? decks_[deck].starvedFrames : 0; }
    [[nodiscard]] bool readFailed(std::size_t deck = 0) const noexcept
    { return deck < deckCount && decks_[deck].readFailed; }
    [[nodiscard]] float masterGain() const noexcept { return masterGain_; }
    [[nodiscard]] float crossfader() const noexcept { return crossfader_; }
    [[nodiscard]] bool isCueSelected(std::size_t deck = 0) const noexcept
    { return deck < deckCount && decks_[deck].cueSelected; }
    [[nodiscard]] float cueGain() const noexcept { return cueGain_; }
    // Peak/RMS cover the last nonempty block, including silence in its RMS
    // denominator. Decks measure pre-fader PCM including transport decay; buses measure actual
    // post-gain float output. abs(sample)>1 counts accumulate for this renderer.
    [[nodiscard]] const Meters& meters() const noexcept { return meters_; }

private:
    struct GainRamp {
        double current = 1.0, start = 1.0, target = 1.0;
        std::uint32_t position = 0;
        void set(double value, std::uint32_t frames) noexcept;
        double next(std::uint32_t frames) noexcept;
        void snap(double value) noexcept;
    };
    struct Deck {
        AudioSource source{};
        std::uint64_t frame = 0;
        std::uint64_t cueFrame = 0;
        std::uint64_t starvedFrames = 0;
        float gain = 1.0F;
        GainRamp gainRamp{};
        GainRamp cueRamp{0.0, 0.0, 0.0, 0};
        std::array<float, channels> last{}, transitionStart{};
        std::uint32_t transitionPosition = 0;
        bool transition = false, available = false;
        bool playing = false;
        bool cueHeld = false;
        bool cuePreviewing = false;
        bool buffering = false;
        bool readFailed = false;
        bool cueSelected = false;
    };
    static bool renderActive(const Deck& deck) noexcept;
    static void notifyActivity(const Deck& deck) noexcept;
    void refreshActivity() const noexcept;
    void moveCursor(Deck& deck, std::uint64_t frame) noexcept;
    void transition(Deck& deck) noexcept;
    void mixDeck(std::size_t index, bool available, float left, float right,
                 double& masterLeft, double& masterRight, double& cueLeft, double& cueRight,
                 bool& contributed, bool& cueContributed) noexcept;
    bool renderInternal(std::span<float> master, std::span<float> cue) noexcept;
    bool renderSources(std::span<float> master, std::span<float> cue) noexcept;
    void beginMeters(std::size_t frames) noexcept;
    void finishMeters() noexcept;
    static void measure(StereoMeter& meter, float left, float right) noexcept;
    void writeBuses(std::span<float> master, std::span<float> cue, std::size_t frame,
                    double left, double right, double cueLeft, double cueRight) noexcept;
    static constexpr std::size_t sourceBlockFrames = 1024;
    std::array<float, sampleRate> tone_{};
    std::array<Deck, deckCount> decks_{};
    std::array<std::array<float, sourceBlockFrames * channels>, deckCount> sourceScratch_{};
    Meters meters_{};
    std::uint64_t frameClock_ = 0;
    std::uint32_t phase_ = 0;
    float masterGain_ = 1.0F;
    float crossfader_ = 0.5F;
    float cueGain_ = 1.0F;
    GainRamp masterRamp_{}, cueRamp_{};
    std::array<GainRamp, deckCount> crossfaderRamps_{};
    std::uint32_t rampFrames_ = 0;
    bool audible_;
};

} // namespace conflux::audio

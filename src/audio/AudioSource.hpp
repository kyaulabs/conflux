// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace conflux::audio {

struct AudioReadResult {
    std::size_t frames = 0;
    bool failed = false;
    // Processed prepared sources report their fractional nominal transport
    // cursor after this prefix. It is separate from output delivery and file
    // read-ahead; Rubber Band does not supply an exact per-sample source map.
    // Active scratching reports its consumed cursor, which may move backwards.
    bool positionValid = false;
    double nominalPosition = 0;
    // True only after the consumer has reached the backend's final marker,
    // including all returned tail frames. An unavailable suffix is not EOF.
    bool finished = false;
    // A resident-hold read may emit audio while keeping the linear service
    // cursor parked. Valid only for a supported hold active before/after read.
    bool residentHold = false;
    // One entry/wrap/exit boundary in this <=1024-frame source prefix. The
    // renderer applies its existing filtered transport envelope at this frame.
    static constexpr std::uint32_t noTransition = UINT32_MAX;
    std::uint32_t transitionFrame = noTransition;
};

enum class AudioResidentHoldCommand { Arm, Enter, Exit, Cancel };
struct AudioResidentHoldRange {
    enum class Provenance { None, DjRange, ValidatedRange };
    // At least twice the maximum1024-frame source read means one wrap/read;
    // eight seconds bounds extra four-role raw payload at12,288,000bytes.
    static constexpr std::uint64_t minimumFrames = 2048, maximumFrames = 8 * 48000;
    static constexpr std::uint64_t bytesPerFrame = 4 * 2 * sizeof(float);
    // These tags identify a DJ-approved or separately validated range record;
    // the engine validates geometry, not musical suitability from a label.
    std::uint64_t id = 0, inFrame = 0, outFrame = 0, admittedBytes = 0;
    Provenance provenance = Provenance::None;
};
struct AudioResidentHoldState {
    std::uint64_t rangeId = 0, inFrame = 0, outFrame = 0;
    std::uint64_t linearFrame = 0, audibleFrame = 0, residentBytes = 0;
    AudioResidentHoldRange::Provenance provenance = AudioResidentHoldRange::Provenance::None;
    int errorCode = 0;
    bool supported = false, ready = false, armed = false, active = false, exitPending = false;
};

enum class AudioTempoCommand { Set, Reset };

struct AudioTempoState {
    double percent = 0, appliedPercent = 0;
    std::uint64_t generation = 0;
    int errorCode = 0;
    bool supported = false, active = false, pending = false;
    bool finished = false, keyLock = false;
};

enum class AudioLoopCommand { In, Out, Toggle, Clear };

enum class AudioScratchCommand { Begin, Move, End };

struct AudioScratchState {
    double position = 0;
    std::uint64_t generation = 0, scratchToken = 0;
    int errorCode = 0;
    bool supported = false, active = false, pending = false;
};

struct AudioLoopState {
    static constexpr std::uint64_t minimumFrames = 256;
    std::uint64_t inFrame = 0, outFrame = 0, limitFrame = 0;
    std::uint64_t residentBytes = 0;
    int errorCode = 0;
    bool supported = false, inSet = false, ready = false;
    bool outSet = false, active = false, cached = false;
};

// Borrowed, already validated stereo 48 kHz PCM. All storage and context must
// outlive the renderer. A normal read supplies a contiguous prefix at firstFrame;
// an unavailable suffix is not EOF. During processed tempo, firstFrame is the
// owner's floored nominal cursor, not a DSP output offset; returned position
// metadata governs advancement. Callbacks must not allocate, block or do I/O.
struct AudioSource {
    std::uint64_t frames = 0;
    const float* memory = nullptr;
    void* context = nullptr;
    AudioReadResult (*read)(void*, std::uint64_t firstFrame, std::span<float>) noexcept = nullptr;
    void (*discontinuity)(void*, std::uint64_t firstFrame) noexcept = nullptr;
    // Optional fixed four-stem control pair, drums/bass/other/vocals. These
    // callbacks share the source's borrowed context and render-owner lifetime.
    bool (*setStemGain)(void*, std::size_t stem, float gain) noexcept = nullptr;
    float (*stemGain)(const void*, std::size_t stem) noexcept = nullptr;
    // Optional render-owner activity notification, independent of audible gain.
    // Repeated block refreshes are allowed; an owner deduplicates actual edges.
    void (*setRenderActive)(void*, bool) noexcept = nullptr;
    // Optional prepared-PCM manual loop pair. Sole render owner, between read
    // calls: IN/OUT capture its exact cursor; Clear invalidates cached borrows.
    // Natural wraps never invoke discontinuity. Owners retain immutable cache
    // storage until a later render-boundary invalidation or quiesced teardown.
    bool (*loopControl)(void*, AudioLoopCommand, std::uint64_t frame) noexcept = nullptr;
    AudioLoopState (*loopState)(const void*) noexcept = nullptr;
    // Optional prepared-tempo pair. Commands capture the render cursor and
    // publish bounded worker requests; DSP setup and I/O stay off callback.
    // Active includes pending/finished processing and DSP at zero percent.
    // appliedPercent identifies the consumed packet's input configuration,
    // not a guarantee of immediate audible effect or exact source mapping.
    bool (*tempoControl)(void*, AudioTempoCommand, double percent, bool keyLock,
                         std::uint64_t frame) noexcept = nullptr;
    AudioTempoState (*tempoState)(const void*) noexcept = nullptr;

    // Prepared-only track-derived platter motion. One render owner controls
    // the source; reads return the actual fractional position, which may move
    // backwards or stay fixed while held. Source hooks do not own gesture IDs;
    // RenderCore guards tokens and stamps them into the reported state.
    bool (*scratchControl)(void*, AudioScratchCommand, std::int64_t cumulativeFrames,
                           double intervalSeconds, std::uint64_t capturedFrame) noexcept = nullptr;
    AudioScratchState (*scratchState)(const void*) noexcept = nullptr;

    // Optional preconfigured same-source four-stem recovery hold. Sole render
    // owner; no allocation/lock/I/O. It is independent of manual IN/OUT loops.
    // Cancel with rangeId zero is reserved for engine transport/lifecycle
    // invalidation; public range commands require the configured nonzero id.
    bool (*residentHoldControl)(void*, AudioResidentHoldCommand, std::uint64_t rangeId,
                                std::uint64_t linearFrame) noexcept = nullptr;
    AudioResidentHoldState (*residentHoldState)(const void*) noexcept = nullptr;

    [[nodiscard]] bool isMemory() const noexcept { return memory != nullptr; }

    // The caller validates complete stereo frames and finite samples first.
    [[nodiscard]] static AudioSource fromMemory(std::span<const float> pcm) noexcept
    {
        return {pcm.size() / 2U, pcm.empty() ? nullptr : pcm.data(), nullptr, nullptr, nullptr};
    }
};

} // namespace conflux::audio

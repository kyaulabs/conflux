// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace conflux::audio {

// One raw-PCM tempo worker over immutable files or bounded live input.
// Construct and stop off callback; all other methods except errorCode have one
// render owner, or a control owner before streaming/after callback quiescence.
class PreparedTempoSource {
public:
    static constexpr std::size_t blockFrames = 1024, slotCount = 8, maximumLeaves = 4;
    static constexpr std::uint64_t payloadBytes = slotCount * maximumLeaves * blockFrames * 2U * sizeof(float);
    struct View {
        std::array<std::span<const float>, maximumLeaves> leaves{};
        std::size_t leafCount = 0, frames = 0;
        std::uint64_t generation = 0;
        bool terminal = false; // Separate zero-frame EOF marker, not producer completion.
        int errorCode = 0;     // Separate zero-frame failed marker; never successful EOF.
        double nominalStart = 0, nominalStep = 1;
        double requestedPercent = 0, appliedInputPercent = 0;
        explicit operator bool() const noexcept { return frames || terminal || errorCode; }
    };

    // One or four readable exact regular stereo native-float48k leaves.
    // Duplicates CLOEXEC; bounded pread preserves caller descriptor offsets.
    // Frame geometry is also limited to 2^53 for double nominal-frame reporting.
    // Geometry/identity is captured before the thread starts. Finite samples
    // and unchanged dev/inode/size/mtime/ctime are checked by the worker.
    // Sources are immutable for this owner's lifetime. Constructor exceptions
    // own/close every partial resource; callers may retain healthy legacy PCM.
    PreparedTempoSource(std::span<const int> fds, std::uint64_t frames);

    // Borrow one/four aligned, raw, interleaved stereo leaves from a live input.
    // Both callbacks run ONLY on this source's non-RT worker. They must return
    // promptly; missing data is a zero-frame prefix, never inserted silence or
    // EOF. Every reported sample must be written and finite. The provider owns
    // its input cursor: a new nonzero generation starts at the requested frame;
    // subsequent reads in that generation request sequential common prefixes.
    // Context and its storage must outlive stop()/destruction. Sequential live
    // input uses explicit seek generations. Immutable resident input may opt
    // into bounded loop reads, including a backward jump only at loop OUT.
    struct StreamInput {
        std::uint64_t frames = 0;
        std::size_t leafCount = 0;
        void* context = nullptr;
        AudioReadResult (*read)(void*, std::uint64_t generation, std::uint64_t firstFrame,
                                std::span<float> interleavedLeaves) noexcept = nullptr;
        int (*errorCode)(const void*) noexcept = nullptr;
        // Optional bounded worker-side activity service. Also runs while the
        // output ring is full or EOF has finished, until cancel/stop. This lets
        // a live provider observe pause without requiring another input read.
        void (*service)(void*) noexcept = nullptr;
        bool allowLoops = false; // Immutable resident input only; live stays sequential.
    };
    explicit PreparedTempoSource(StreamInput input);
    ~PreparedTempoSource();
    PreparedTempoSource(const PreparedTempoSource&) = delete;
    PreparedTempoSource& operator=(const PreparedTempoSource&) = delete;

    // Fixed stores only: no IO, allocation, lock, notification or DSP work.
    // Accept linear [0,frames], finite[-6,6]. A valid request gets a never-reused new
    // generation and invalidates every previous synchronous View borrow BEFORE
    // publication. Linear frame==frames produces a consumable empty EOF marker.
    // A same-frame request is still an explicit new generation/re-prime.
    // Optional [loopIn,loopOut) repeats immutable input through the same DSP
    // state. Both zero means linear playback; otherwise frame must be within
    // the bounded 256-frame to 64-second range. Natural wraps never re-prime.
    bool request(std::uint64_t frame, double percent, bool keyLock,
                 std::uint64_t loopIn = 0, std::uint64_t loopOut = 0) noexcept;
    // Prepared linear continuation keeps a fractional nominal transport cursor.
    // Immutable PCM starts at floor(position); this does not transfer the old
    // DSP filter state or promise sub-frame audio-phase alignment.
    bool requestAt(double position, double percent, bool keyLock) noexcept;
    // Same generation/backend, including active SRC/R3 crossing zero. Fresh
    // unity->nonzero requires request(). Key-lock changes also use request().
    // Latest requests may coalesce; old queued data keeps its old metadata.
    bool requestSpeed(double percent) noexcept;
    // No queued older configuration remains after the initial generation
    // request, or once the latest input revision has reached the consumer.
    // Unlike pending, this remains true across a temporary empty output ring.
    [[nodiscard]] bool outputConfigurationCurrent() const noexcept;
    bool cancel() noexcept;

    // Bounded SPSC borrow: at most one current-generation slot, possibly a
    // suffix after partial consume. Empty means unavailable, not EOF. Finish
    // using its spans before consume/request/cancel/stop; never retain them
    // across callback boundaries. Worker cannot recycle an unconsumed slot.
    View peek() noexcept;
    // Playback startup borrow: wait for the existing bounded queue to fill
    // once per generation, or a shorter EOF/error. Never consumes PCM and
    // never reprimes later reads. Same single-owner borrow rules as peek().
    View peekPrepared() noexcept;
    // Acknowledge only the returned prefix; reject over-consumption unchanged.
    // consume(0) acknowledges a zero-frame EOF/error marker. Finished becomes
    // true only after EOF consumption, after every queued actual tail frame.
    bool consume(std::size_t frames) noexcept;
    [[nodiscard]] double nominalPosition() const noexcept;
    // Consumed nominal wraps in the current generation, never producer lead.
    [[nodiscard]] std::uint64_t loopWraps() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    // percent is requested; appliedPercent comes from consumed packet input
    // revision metadata, NOT an exact audible-effect or R3 source mapping.
    // pending includes initial fill/deferred configuration. Position advances
    // only with consumed output; loops wrap inside their range, while linear
    // playback clamps at source EOF as its actual tail drains.
    AudioTempoState status() noexcept;
    // Atomic current-generation health; safe for external/control sampling.
    // Optional setup failure must not be confused with legacy reader health.
    [[nodiscard]] int errorCode() const noexcept;
    // Quiesce every render borrow first. Idempotent join before FD/storage
    // destruction; this is never an audio-callback operation.
    void stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace conflux::audio

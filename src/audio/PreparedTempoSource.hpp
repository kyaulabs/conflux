// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace conflux::audio {

// One prepared-PCM tempo worker, independent of legacy readers/bank/loop cache.
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
    ~PreparedTempoSource();
    PreparedTempoSource(const PreparedTempoSource&) = delete;
    PreparedTempoSource& operator=(const PreparedTempoSource&) = delete;

    // Fixed stores only: no IO, allocation, lock, notification or DSP work.
    // Accept [0,frames], finite[-6,6]. A valid request gets a never-reused new
    // generation and invalidates every previous synchronous View borrow BEFORE
    // publication. Frame==frames produces a consumable empty EOF marker.
    // A same-frame request is still an explicit new generation/re-prime.
    bool request(std::uint64_t frame, double percent, bool keyLock) noexcept;
    // Same generation/backend, including active SRC/R3 crossing zero. Fresh
    // unity->nonzero requires request(). Key-lock changes also use request().
    // Latest requests may coalesce; old queued data keeps its old metadata.
    bool requestSpeed(double percent) noexcept;
    bool cancel() noexcept;

    // Bounded SPSC borrow: at most one current-generation slot, possibly a
    // suffix after partial consume. Empty means unavailable, not EOF. Finish
    // using its spans before consume/request/cancel/stop; never retain them
    // across callback boundaries. Worker cannot recycle an unconsumed slot.
    View peek() noexcept;
    // Acknowledge only the returned prefix; reject over-consumption unchanged.
    // consume(0) acknowledges a zero-frame EOF/error marker. Finished becomes
    // true only after EOF consumption, after every queued actual tail frame.
    bool consume(std::size_t frames) noexcept;
    [[nodiscard]] double nominalPosition() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    // percent is requested; appliedPercent comes from consumed packet input
    // revision metadata, NOT an exact audible-effect or R3 source mapping.
    // pending includes initial fill/deferred configuration. Position advances
    // only with consumed output and clamps at source EOF while tail drains.
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

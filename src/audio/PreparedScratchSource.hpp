// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>

namespace conflux::audio {

// Prepared PCM scratching. One render owner, one file worker. All storage and filter
// tables are constructed off callback. Control/read/status calls belong to the
// render owner (or a control owner while rendering is quiesced). No RT call
// allocates, locks, signals a condition, performs IO or waits for the worker.
class PreparedScratchSource {
  public:
    static constexpr std::size_t sampleRate = 48000;
    static constexpr std::size_t maxReadFrames = 1024;
    static constexpr double maximumSpeed = 8.0;
    static constexpr std::size_t windowFrames = 196608;
    static constexpr std::size_t filterTaps = 64;
    static constexpr std::size_t windowCount = 3;
    static constexpr auto motionTimeout = std::chrono::milliseconds(30);

    struct Read {
        // Only leafCount() spans are populated; each holds frames*2 samples.
        // Borrow ends before the NEXT mutating call or quiesced stop, never
        // retain across callbacks. Worker never owns these output buffers.
        std::array<std::span<const float>, 4> leaves{};
        std::size_t frames = 0;
        double nominalPosition = 0;
        bool failed = false;
    };

    // Exact regular readable raw stereo F32 48k descriptors, one or four equally
    // sized leaves. Duplicates are CLOEXEC; pread preserves shared FD offsets.
    // Frames must be in 1..2^53 and fit off_t byte geometry. Construction and initial prefetch run off the audio
    // callback.
    PreparedScratchSource(std::span<const int> fds, std::uint64_t frames);
    ~PreparedScratchSource();
    PreparedScratchSource(const PreparedScratchSource &) = delete;
    PreparedScratchSource &operator=(const PreparedScratchSource &) = delete;

    // Off callback only. One representable deadline, explicit new epoch even at
    // same frame. Invalid budget/already-stopped token preserves state.
    // Cancellation/timeout returns false; no worker is detached.
    bool prime(std::uint64_t frame, std::chrono::milliseconds timeout, std::stop_token stop = {});
    // Explicit discontinuity: cancel motion, new never-reused epoch, immediately
    // hide old-generation windows. EOF is a valid cursor (reverse is allowed).
    bool relocate(std::uint64_t frame) noexcept;
    // Inactive ordinary playback follows its actual cursor without new epoch.
    // Quantized overlapping refill; does not discard a usable front window.
    bool follow(std::uint64_t frame) noexcept;
    // Valid begin may wait for resident PCM. Repeated begin/current failure
    // rejects without changing active motion. Held position reads full silence.
    bool begin(std::uint64_t frame) noexcept;
    // Displacement is cumulative from begin, NOT an incremental seek. Interval
    // is a positive finite motion-event interval estimate (<=1 second).
    // Target clamps to track; velocity slews and cannot exceed maximumSpeed.
    // A direction reversal discards undelivered motion and rebases the target
    // at the ACTUAL cursor plus the signed input delta, without a cursor jump.
    // A 30ms stale-input watchdog stops movement, never extrapolates forever.
    bool move(std::int64_t cumulativeDisplacement, double intervalSeconds) noexcept;
    void end() noexcept;
    // 1..1024 frames. Invalid/empty request returns empty without state change.
    // Fresh hold: full zero PCM, stable cursor. After motion stops, a shared
    // 240-frame gate fades the last valid track samples without advancing the
    // cursor, then returns exact zeros. Missing window: only a bounded valid
    // prefix, held cursor, pending true. Never stale/teleported PCM. Reverse
    // reads the sample interval preceding the current cursor.
    Read read(std::size_t frames) noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] double position() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept { return epoch(); }
    [[nodiscard]] std::size_t leafCount() const noexcept;
    // Pure sole-owner observations: only read/follow/begin/prime adopt a newly
    // published window. Status access never exchanges/recycles a window.
    [[nodiscard]] bool pending() const noexcept;
    // Atomic-only error observation; safe concurrently with the render owner.
    // Worker failures become visible when an owning call adopts publication.
    [[nodiscard]] int errorCode() const noexcept;
    // Fixed three-window payload allocation, excludes filter table/objects.
    [[nodiscard]] std::uint64_t residentBytes() const noexcept;
    // Off callback after render quiescence, idempotent; join then close/free.
    void stop() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace conflux::audio

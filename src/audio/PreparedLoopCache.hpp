// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace conflux::audio {

// A bounded, optional cache for immutable prepared PCM, not a live source.
// request/cancel/view/errorCode have one render owner, or a control owner before
// streaming/after callback quiescence. Requests belong at callback boundaries:
// every borrowed View and its spans must be finished before the next request or
// cancel. Never retain a View across callbacks that can change the request.
class PreparedLoopCache {
public:
    static constexpr std::uint64_t maxFrames = 64U * 48000U;
    static constexpr std::uint64_t maxBytes = 4U * maxFrames * 2U * sizeof(float);

    struct View {
        std::uint64_t firstFrame = 0, frames = 0;
        std::array<std::vector<float>, 4> leaves;
        std::size_t leafCount = 0;
    };

    // One stereo source or four stereo stems, native float32 at 48 kHz.
    // Headerless PCM cannot attest its rate: callers supply already prepared
    // 48 kHz descriptors and authoritative frame geometry. Duplicates are owned
    // CLOEXEC; pread never changes the caller's shared file offsets.
    PreparedLoopCache(std::span<const int> fds, std::uint64_t frames);
    ~PreparedLoopCache();
    PreparedLoopCache(const PreparedLoopCache&) = delete;
    PreparedLoopCache& operator=(const PreparedLoopCache&) = delete;

    // No IO, allocation, locking or notification. Invalid frames leave the
    // current request unchanged. Every accepted request/cancel gets a new
    // serial; exhaustion disables the view permanently with EOVERFLOW.
    [[nodiscard]] bool request(std::uint64_t firstFrame) noexcept;
    [[nodiscard]] bool cancel() noexcept;
    // Null while pending, cancelled or failed. A published View is immutable
    // until the owner publishes another request, cancel, or quiesced stop.
    [[nodiscard]] const View* view() const noexcept;
    // Current cache-only errno; zero while pending/cancelled. Normal prepared
    // playback need not fail when optional cache construction fails.
    [[nodiscard]] int errorCode() const noexcept;
    // Atomic payload allocation statistic; may be read by another thread.
    [[nodiscard]] std::uint64_t residentBytes() const noexcept;
    // Off callback, after all render access has quiesced. Idempotent; joins
    // the worker before releasing cache storage and descriptors.
    void stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace conflux::audio

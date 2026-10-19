// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"
#include "audio/PcmReadAhead.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace conflux::audio {
class StemBankSource;

// One whole playback owner: a normal stereo source or four aligned stems.
// Construction, prime, error text and stop/join belong off the audio callback.
// Keep this object alive until the last render callback has been acknowledged.
class PreparedPlayback {
public:
    static constexpr std::size_t stemCount = 4;
    // Borrow readable regular FDs whose exact size is frames*2*sizeof(float).
    // Each reader duplicates its FD; callers retain their original descriptors.
    PreparedPlayback(int normalizedFd, std::uint64_t frames);
    PreparedPlayback(std::array<int, stemCount> normalizedFds, std::uint64_t frames);
    ~PreparedPlayback();
    PreparedPlayback(const PreparedPlayback&) = delete;
    PreparedPlayback& operator=(const PreparedPlayback&) = delete;

    [[nodiscard]] AudioSource sourceRef() noexcept;
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    // Before concurrent rendering. All readers share this single timeout budget.
    bool prime(std::uint64_t frame, std::chrono::milliseconds timeout);
    // Configure before concurrent rendering. Normal PCM is an unchanged no-op;
    // a bank uses these frames for future gain changes. Never call from the GUI
    // once this owner has entered the render-boundary handoff.
    bool configureStemRamps(std::uint32_t frames) noexcept;
    // Atomic reader/control health only; safe to sample from the render owner.
    // RenderCore separately publishes failures reported by the bank's RT read.
    [[nodiscard]] int errorCode() const noexcept;
    [[nodiscard]] std::string error() const;
    // Quiesce the render consumer first. Idempotent; never call from callback.
    void stop() noexcept;

private:
    std::array<std::unique_ptr<PcmReadAhead>, stemCount> readers_{};
    std::unique_ptr<StemBankSource> bank_;
    std::uint64_t frames_;
    std::atomic<int> primeError_{0};
    bool stopped_ = false;
};
} // namespace conflux::audio

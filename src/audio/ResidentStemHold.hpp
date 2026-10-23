// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "audio/LiveStemBuffer.hpp"
#include <array>
#include <atomic>
#include <vector>

namespace conflux::audio {
// One producer captures accepted raw packets until a complete immutable range
// is release-published. The render owner only borrows it after acquire-ready.
// Configure/allocate before the producer starts; destroy after both owners stop.
class ResidentStemHold {
public:
    static constexpr std::uint64_t minimumFrames = AudioResidentHoldRange::minimumFrames;
    static constexpr std::uint64_t maximumFrames = AudioResidentHoldRange::maximumFrames;
    static constexpr std::uint64_t bytesPerFrame = AudioResidentHoldRange::bytesPerFrame;
    ResidentStemHold(std::uint64_t sourceId, std::uint64_t totalFrames, AudioResidentHoldRange);
    void captureAccepted(const LiveStemBuffer::Packet&) noexcept; // producer only
    [[nodiscard]] bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    [[nodiscard]] const AudioResidentHoldRange& range() const noexcept { return range_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return (range_.outFrame-range_.inFrame)*bytesPerFrame; }
    [[nodiscard]] std::array<std::span<const float>,4> leaves() const noexcept; // render after ready
private:
    const std::uint64_t sourceId_;
    const AudioResidentHoldRange range_;
    std::array<std::vector<float>,4> leaves_;
    std::atomic<bool> ready_{false};
    std::uint64_t epoch_ = 0, capturedEnd_ = 0; // producer only
};
} // namespace conflux::audio

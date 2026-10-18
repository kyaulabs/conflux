// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/StereoRecorder.hpp"

#include <atomic>
#include <cstdint>
#include <span>

namespace conflux::audio {

// One control owner publishes at most one outstanding change. The render owner
// applies it at block entry and acknowledges only after its last possible use
// in that block. Keep old/new writers alive until a detach is acknowledged or
// callbacks have quiesced and quiesce() has returned. No ownership crosses RT.
class RecorderTap {
public:
    bool request(StereoRecorder* recorder) noexcept
    {
        const auto revision = requested_.load(std::memory_order_relaxed);
        if (acknowledged_.load(std::memory_order_acquire) != revision)
            return false;
        pending_ = recorder;
        requested_.store(revision + 1, std::memory_order_release);
        return true;
    }

    // Render owner only. A second request cannot overwrite pending_ until the
    // first block's endBlock() has acknowledged it.
    void beginBlock() noexcept
    {
        const auto revision = requested_.load(std::memory_order_acquire);
        if (revision != applied_) {
            active_ = pending_;
            applied_ = revision;
        }
    }

    bool push(std::span<const float> master) noexcept
    { return !active_ || active_->push(master); }

    void endBlock() noexcept
    {
        attached_.store(active_ != nullptr, std::memory_order_relaxed);
        acknowledged_.store(applied_, std::memory_order_release);
    }

    [[nodiscard]] bool changePending() const noexcept
    {
        return requested_.load(std::memory_order_relaxed) !=
            acknowledged_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool attached() const noexcept
    { return attached_.load(std::memory_order_acquire); }

    // Control owner only, after all process callbacks have stopped. Pending
    // attachments are discarded; no more writer access is possible afterward.
    void quiesce() noexcept
    {
        active_ = nullptr;
        applied_ = requested_.load(std::memory_order_acquire);
        endBlock();
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    StereoRecorder* pending_ = nullptr;
    StereoRecorder* active_ = nullptr;
    std::uint64_t applied_ = 0;
    std::atomic<std::uint64_t> requested_{0}, acknowledged_{0};
    std::atomic<bool> attached_{false};
};

} // namespace conflux::audio

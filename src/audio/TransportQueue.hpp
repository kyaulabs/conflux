// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/RenderCore.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace conflux::audio {

// Exactly one producer and one consumer. Full/empty calls fail immediately;
// rejection preserves both the queue and the caller's output command.
class TransportQueue {
public:
    static constexpr std::size_t capacity = 32;

    bool tryPush(const RenderCore::Command& command) noexcept
    {
        const auto position = written_.load(std::memory_order_relaxed);
        if (position - read_.load(std::memory_order_acquire) >= capacity)
            return false;
        commands_[position % capacity] = command;
        written_.store(position + 1U, std::memory_order_release);
        return true;
    }

    bool tryPop(RenderCore::Command& command) noexcept
    {
        const auto position = read_.load(std::memory_order_relaxed);
        if (position == written_.load(std::memory_order_acquire))
            return false;
        command = commands_[position % capacity];
        read_.store(position + 1U, std::memory_order_release);
        return true;
    }

private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::is_trivially_copyable_v<RenderCore::Command>);
    std::array<RenderCore::Command, capacity> commands_{};
    std::atomic<std::uint32_t> written_{0};
    std::atomic<std::uint32_t> read_{0};
};

} // namespace conflux::audio

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <spa/buffer/buffer.h>
#include <cstddef>
#include <cstdint>

namespace conflux::experiments {

// Read only contiguous, valid mono float frames from a dequeued input buffer.
inline const float* pipeWireInputSamples(const spa_data* plane, std::uint32_t frames) noexcept
{
    if (!frames || !plane || !plane->data || !plane->chunk || !plane->maxsize) return nullptr;
    const auto& chunk = *plane->chunk;
    const auto offset = chunk.offset % plane->maxsize;
    const auto bytes = std::size_t(frames) * sizeof(float);
    if (chunk.stride != sizeof(float) || chunk.size < bytes || bytes > plane->maxsize - offset ||
        (chunk.flags & (SPA_CHUNK_FLAG_CORRUPTED | SPA_CHUNK_FLAG_EMPTY))) return nullptr;
    const auto* start = static_cast<const char*>(plane->data) + offset;
    return reinterpret_cast<std::uintptr_t>(start) % alignof(float) == 0 ?
        reinterpret_cast<const float*>(start) : nullptr;
}

} // namespace conflux::experiments

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace conflux::audio {

struct AudioReadResult {
    std::size_t frames = 0;
    bool failed = false;
};

// Borrowed, already validated stereo 48 kHz PCM. All storage and context must
// outlive the renderer. A read supplies only a contiguous prefix at firstFrame;
// an unavailable suffix is not EOF. Callbacks must not allocate, block or do I/O.
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

    [[nodiscard]] bool isMemory() const noexcept { return memory != nullptr; }

    // The caller validates complete stereo frames and finite samples first.
    [[nodiscard]] static AudioSource fromMemory(std::span<const float> pcm) noexcept
    {
        return {pcm.size() / 2U, pcm.empty() ? nullptr : pcm.data(), nullptr, nullptr, nullptr};
    }
};

} // namespace conflux::audio

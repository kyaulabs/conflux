// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace conflux::audio {

// Fixed-format, immutable PCM prepared before the audio stream starts. Keep this
// owner alive and unmoved while a RenderCore borrows its samples.
class DecodedAudio {
public:
    static constexpr std::size_t defaultMaxDecodedBytes = 64U * 1024U * 1024U;

    DecodedAudio(DecodedAudio&&) noexcept = default;
    DecodedAudio& operator=(DecodedAudio&&) noexcept = default;
    DecodedAudio(const DecodedAudio&) = delete;
    DecodedAudio& operator=(const DecodedAudio&) = delete;

    [[nodiscard]] std::span<const float> samples() const noexcept { return samples_; }
    [[nodiscard]] std::uint64_t frames() const noexcept;

private:
    explicit DecodedAudio(std::vector<float> samples) noexcept;
    std::vector<float> samples_;
    friend DecodedAudio decodeAudioFile(const std::filesystem::path&, std::size_t);
};

// This first playback slice accepts nonempty stereo 48 kHz files. The byte limit
// bounds decoded PCM, independently of compressed file size. Performs file I/O
// and allocation; never call from the audio callback.
DecodedAudio decodeAudioFile(const std::filesystem::path& path,
                            std::size_t maxDecodedBytes = DecodedAudio::defaultMaxDecodedBytes);

} // namespace conflux::audio

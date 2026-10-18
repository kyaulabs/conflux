// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
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

// Accept mono/stereo 8–192 kHz files; prepare stereo 48 kHz float PCM. Stereo
// 48 kHz bypasses conversion; other rates use persistent best-quality sinc.
// The byte limit bounds final PCM, independently of compressed/input size.
// Performs file I/O, conversion and allocation outside the audio callback.
DecodedAudio decodeAudioFile(const std::filesystem::path& path,
                            std::size_t maxDecodedBytes = DecodedAudio::defaultMaxDecodedBytes);

// Sequentially prepared, anonymous temporary stereo 48 kHz float file. Only
// workers read its descriptor; a renderer must use bounded PCM read-ahead.
class PreparedAudio {
public:
    static constexpr std::size_t defaultMaxPreparedBytes = 256U * 1024U * 1024U;
    ~PreparedAudio();
    PreparedAudio(PreparedAudio&&) noexcept;
    PreparedAudio& operator=(PreparedAudio&&) noexcept;
    PreparedAudio(const PreparedAudio&) = delete;
    PreparedAudio& operator=(const PreparedAudio&) = delete;
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::span<const float> peaks() const noexcept { return peaks_; }
private:
    explicit PreparedAudio(int descriptor) noexcept;
    int descriptor_ = -1;
    std::uint64_t frames_ = 0;
    std::vector<float> peaks_;
    friend PreparedAudio prepareAudioFile(const std::filesystem::path&, std::size_t, std::stop_token,
                                          const std::filesystem::path&);
    friend PreparedAudio prepareAudioFile(int, std::size_t, std::stop_token, const std::filesystem::path&);
    friend PreparedAudio reopenPreparedAudio(int, std::uint64_t, std::span<const float>, std::size_t);
};

PreparedAudio prepareAudioFile(const std::filesystem::path& path,
    std::size_t maximumBytes = PreparedAudio::defaultMaxPreparedBytes, std::stop_token stop = {},
    const std::filesystem::path& spoolDirectory = {});

// Borrow an already-open regular source; rewind before decoding and never close
// the caller's descriptor. Hashing must use pread rather than its shared offset.
PreparedAudio prepareAudioFile(int sourceDescriptor,
    std::size_t maximumBytes = PreparedAudio::defaultMaxPreparedBytes, std::stop_token stop = {},
    const std::filesystem::path& spoolDirectory = {});

// Borrow a read-only regular PCM descriptor already opened beneath the cache
// directory without following symlinks. Validate exact size and finite normalized
// overview geometry, then return a CLOEXEC duplicate without reopening a path.
// The store verifies PCM digest/finiteness through this returned descriptor
// before handing it to a reader; the caller retains its original descriptor.
PreparedAudio reopenPreparedAudio(int pcmDescriptor, std::uint64_t frames,
    std::span<const float> peaks, std::size_t maximumBytes = PreparedAudio::defaultMaxPreparedBytes);

} // namespace conflux::audio

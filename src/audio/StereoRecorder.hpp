// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace conflux::audio {

// One PCM producer and one background disk writer. Construct off the audio
// thread; the destination must not exist. Quiesce the producer before stop or
// destruction. Status queries may run concurrently; error() is non-RT.
class StereoRecorder {
public:
    static constexpr std::uint32_t sampleRate = 48000;
    static constexpr std::size_t channels = 2;
    // Reserve more than the WAV header needs, keeping RIFF's 32-bit size valid.
    static constexpr std::uint64_t maximumFrames = (UINT32_MAX - 4096ULL) / (channels * sizeof(float));

    explicit StereoRecorder(const std::filesystem::path& path, std::size_t capacityFrames = sampleRate);
    ~StereoRecorder();
    StereoRecorder(const StereoRecorder&) = delete;
    StereoRecorder& operator=(const StereoRecorder&) = delete;

    // All-or-nothing, with no allocation, locking, I/O or worker notification.
    // Bad PCM, queue overflow or the WAV limit terminally fail this recording;
    // later pushes fail and the worker drains its previously accepted prefix.
    bool push(std::span<const float> interleaved) noexcept;
    // Owner only, after producer quiescence. Drains, joins and checks close.
    // A storage failure may leave writtenFrames() below acceptedFrames().
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] std::uint64_t acceptedFrames() const noexcept;
    [[nodiscard]] std::uint64_t writtenFrames() const noexcept;
    [[nodiscard]] std::uint64_t overflowCount() const noexcept;
    [[nodiscard]] std::uint64_t overflowFrames() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace conflux::audio

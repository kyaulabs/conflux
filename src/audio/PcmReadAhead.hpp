// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace conflux::audio {

// One render consumer and one owned file worker. Construction, prime, error and
// stop belong to the control thread; prime must precede concurrent rendering.
// The source descriptor remains borrowed until callbacks have been quiesced.
class PcmReadAhead {
public:
    static constexpr std::size_t blockFrames = 4096;
    static constexpr std::size_t slotCount = 32;
    static constexpr std::size_t cacheBytes = blockFrames * slotCount * 2U * sizeof(float);

    // Duplicate a readable regular-file FD containing native stereo float PCM.
    // The supplied frame count is authoritative; premature EOF is an error.
    PcmReadAhead(int normalizedFd, std::uint64_t frames);
    ~PcmReadAhead();
    PcmReadAhead(const PcmReadAhead&) = delete;
    PcmReadAhead& operator=(const PcmReadAhead&) = delete;

    [[nodiscard]] AudioSource sourceRef() noexcept;
    bool prime(std::uint64_t frame, std::chrono::milliseconds timeout);
    [[nodiscard]] int errorCode() const noexcept;
    [[nodiscard]] std::string error() const;
    // Call only after the render consumer has stopped. Idempotent.
    void stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace conflux::audio

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace conflux::audio {

// Fixed whole-bank 48 kHz delivery, drums/bass/other/vocals. Exactly one non-RT
// producer calls tryRequest/tryPublish; exactly one render owner consumes all
// four views through StemBankSource. No callback allocation, blocking or I/O.
// Construction allocates the fixed PCM storage plus fixed metadata, off-stream.
// Borrowed views and the buffer must outlive every read/discontinuity. Quiesce
// both owners before destruction; this pure provider owns no thread or process.
class LiveStemBuffer {
public:
    static constexpr std::size_t stemCount = 4;
    static constexpr std::size_t channels = 2;
    static constexpr std::size_t blockFrames = 4096;
    static constexpr std::size_t slotCount = 72;
    static constexpr std::size_t pcmBytes = slotCount * blockFrames * channels * stemCount * sizeof(float);

    struct Request {
        std::uint64_t sourceId = 0, epoch = 0, firstFrame = 0;
    };
    struct Packet {
        std::uint64_t sourceId = 0, epoch = 0, firstFrame = 0;
        std::size_t frames = 0;
        bool final = false;
        std::array<std::span<const float>, stemCount> stems{};
    };
    enum class PublishResult { Accepted, Full, Stale, Invalid };

    // Identity is a fixed, nonzero owner token, not a content digest. The future
    // worker bridge must independently validate full source/model identities.
    // totalFrames is exact stereo 48 kHz geometry; no guessed/padded EOF.
    LiveStemBuffer(std::uint64_t sourceId, std::uint64_t totalFrames);
    ~LiveStemBuffer();
    LiveStemBuffer(const LiveStemBuffer&) = delete;
    LiveStemBuffer& operator=(const LiveStemBuffer&) = delete;
    LiveStemBuffer(LiveStemBuffer&&) = delete;
    LiveStemBuffer& operator=(LiveStemBuffer&&) = delete;

    [[nodiscard]] std::uint64_t sourceId() const noexcept;
    [[nodiscard]] std::uint64_t frames() const noexcept;
    [[nodiscard]] std::array<AudioSource, stemCount> sourceRefs() noexcept;

    // Initial request is epoch1/frame0. Snapshot failure is bounded and leaves
    // output unchanged; the producer retries away from the audio callback.
    bool tryRequest(Request& output) const noexcept;
    // Caller keeps input spans alive/immutable until return. One finite aligned
    // packet is copied atomically; nonempty frames<=4096, exact stereo spans,
    // contiguous current-epoch origin, final iff end==total. Full never waits or
    // scans/copies PCM. Rejections do not advance the published audio sequence.
    PublishResult tryPublish(const Packet& packet) noexcept;

    // Views accept sequential prefixes of at most4096 stereo frames. A wrong
    // position/shape fails without writing or advancing; no implicit seek.
    // Empty valid reads may retire stale storage without consuming audio.
    // StemBankSource's synchronous four distinct-view seek notifications are
    // coalesced; a later complete same-frame round is a fresh epoch. A valid
    // read closes a partial notification round. Only the render owner retires
    // cells, after all four copies or an explicit seek invalidation. One old
    // packet can race final publication; it is rejected on render consumption.
    // Missing data is {0,false}, including at exact known EOF; it never inserts
    // silence, repeats audio, substitutes a mix, or advances the source cursor.

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace conflux::audio

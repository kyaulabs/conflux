// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioFile.hpp"
#include "audio/PreparedPlayback.hpp"
#include "library/LiveStemObservations.hpp"
#include "library/TrackPresentation.hpp"

#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <stop_token>

namespace conflux::library {

struct PreparedTrack {
    audio::PreparedAudio audio;
    QString sourceSha256;
    QString artifactKey;
    bool cacheHit = false;
    TrackPresentation presentation{};
};

// Explicit local engine inputs, not a shell command or a plugin interface.
struct StemEngine {
    QString pythonExecutable, exporterPath, workerPath, runtimePath, modelPath;
    QString backend = "vulkan";
    int threads = 2;
    int maxSeconds = 7200, requestTimeout = 30;
    std::size_t maxDiagnosticBytes = 65536;
};

struct PreparedStemTrack {
    // Fixed separator roles: drums, bass, other, vocals. Every owner has the
    // same actual 48000 Hz frame count, including converter rounding.
    std::array<audio::PreparedAudio, 4> stems;
    QString sourceSha256;
    QString artifactKey;
    bool cacheHit = false;
    TrackPresentation presentation{};
};

// A caller-approved source-relative range, never inferred from a CUE marker.
// Its source SHA is verified before constructing any live source or producer.
struct LiveStemResidentHold {
    QString sourceSha256;
    audio::AudioResidentHoldRange range;
};

struct PreparedLiveStemTrack {
    std::unique_ptr<audio::PreparedPlayback> playback;
    QString sourceSha256, inputSha256, modelSha256;
    std::uint64_t inputFrames = 0, outputFrames = 0, reservedDiskBytes = 0;
    // Normalized whole-source44100 overview, not a separated-stem waveform.
    std::vector<float> sourceOverview;
    std::shared_ptr<LiveStemObservations> observations;
    TrackPresentation presentation{};
    // Expected compatible prepared-bank identity from this live setup's pins;
    // a key does not mean a complete bank is present in the cache.
    QString artifactKey;
    // Configured capture only; readiness/arming/active state belongs to audio.
    std::optional<audio::AudioResidentHoldRange> residentHold;
};

// Worker-owned persistent PCM preparation. No method belongs on the GUI or
// render thread. Immutable artifact directories hold normal audio + metadata or
// four fixed stem leaves + metadata. Source digests remain distinct from keys.
// Storage accounting covers linked entries and this operation's transients;
// callers must also budget separately retained, unlinked playback owners.
class PreparedTrackStore {
public:
    struct Limits {
        // Current ceilings; callers may tighten them, including in tests.
        std::size_t maxPreparedBytes = audio::PreparedAudio::defaultMaxPreparedBytes;
        std::uint64_t maxSourceBytes = 1024ULL * 1024 * 1024;
        // Linked cache entries plus this operation's admitted transient files.
        // Separately retained unlinked FD owners require caller admission.
        std::uint64_t maxCacheBytes = 2ULL * 1024 * 1024 * 1024;
        std::size_t maxEntries = 64;
        std::size_t maxManifestBytes = 16U * 1024;
    };

    explicit PreparedTrackStore(QString cacheRoot);
    PreparedTrackStore(QString cacheRoot, Limits limits);
    PreparedTrack prepare(const QString& sourcePath, std::stop_token stop = {});
    PreparedStemTrack prepareStemBank(const QString& sourcePath, const StemEngine& engine,
                                      std::stop_token stop = {});
    // Read-only bank discovery; absence or a captured-identity mismatch is a miss.
    // Validates used inputs and current budgets without repair/export/staging.
    // A captured key can reject a missing artifact before using source/engine;
    // a present candidate still requires their full current identity checks.
    // A bounded engine --describe is permitted, but never model inference.
    std::optional<PreparedStemTrack> findPreparedStemBank(const QString& sourcePath,
        const StemEngine& engine, std::stop_token stop = {}, const QString& expectedArtifactKey = {});
    // Off-thread transient whole owner beneath this store's root/limits.
    // Returns validated service-ready metadata; first real prime starts load.
    // Never primes here, creates a complete stem file, or borrows the caller's
    // stop token after return. A distinct nonzero token identifies this owner.
    PreparedLiveStemTrack prepareLiveStem(const QString& sourcePath, const StemEngine& engine,
        const QString& servicePath, std::uint64_t sourceToken, std::stop_token stop = {},
        std::optional<LiveStemResidentHold> residentHold = {});

private:
    QString cacheRoot_;
    Limits limits_;
};

} // namespace conflux::library

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioFile.hpp"

#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stop_token>

namespace conflux::library {

struct PreparedTrack {
    audio::PreparedAudio audio;
    QString sourceSha256;
    QString artifactKey;
    bool cacheHit = false;
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

private:
    QString cacheRoot_;
    Limits limits_;
};

} // namespace conflux::library

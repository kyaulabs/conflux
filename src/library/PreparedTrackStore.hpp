// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioFile.hpp"

#include <QString>
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

// Worker-owned persistent PCM preparation. No method belongs on the GUI or
// render thread. Entries are immutable <artifactKey>/{audio.f32,manifest.json}
// directories; the source digest is distinct from the versioned artifact key.
class PreparedTrackStore {
public:
    struct Limits {
        // Current ceilings; callers may tighten them, including in tests.
        std::size_t maxPreparedBytes = audio::PreparedAudio::defaultMaxPreparedBytes;
        std::uint64_t maxSourceBytes = 1024ULL * 1024 * 1024;
        std::uint64_t maxCacheBytes = 2ULL * 1024 * 1024 * 1024;
        std::size_t maxEntries = 64;
        std::size_t maxManifestBytes = 16U * 1024;
    };

    explicit PreparedTrackStore(QString cacheRoot);
    PreparedTrackStore(QString cacheRoot, Limits limits);
    PreparedTrack prepare(const QString& sourcePath, std::stop_token stop = {});

private:
    QString cacheRoot_;
    Limits limits_;
};

} // namespace conflux::library

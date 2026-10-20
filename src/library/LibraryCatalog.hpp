// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace conflux::library {

struct CuePoint {
    int trackNumber = 0;
    QString label;
    std::uint64_t frame = 0;
    bool operator==(const CuePoint&) const = default;
};

struct LibraryTrack {
    QString sourceSha256;
    QString sourcePath;
    QString name;
    QString artifactKey;
    std::uint64_t frames = 0;
    bool available = false;
    std::vector<CuePoint> cues{};
};

// Worker-only metadata persistence; this never hashes, decodes or prepares audio.
// Version 2 adds imported source-relative cue points; version 1 reads without cues.
// Availability is refreshed from regular source-file existence, never saved.
class LibraryCatalog {
public:
    static constexpr std::size_t maxEntries = 1024;
    static constexpr std::size_t maxBytes = 1024U * 1024;

    explicit LibraryCatalog(QString absoluteFilePath);
    std::vector<LibraryTrack> read() const;
    // Audio reimports retain existing points for the same content; callers
    // install a new verified point set only through updateCues.
    std::vector<LibraryTrack> upsert(const LibraryTrack& track);
    std::vector<LibraryTrack> updateCues(const QString& sourceId, std::uint64_t expectedFrames,
                                       const std::vector<CuePoint>& cues);

private:
    QString filePath_;
};

} // namespace conflux::library

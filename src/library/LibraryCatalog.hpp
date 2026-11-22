// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioFile.hpp"

#include <QString>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    std::optional<audio::SignalLevels> levels{};
    std::optional<std::uint64_t> savedCueFrame{};
    std::optional<double> manualBpm{};
};

// Worker-only metadata persistence; this never hashes, decodes or prepares audio.
// Version 5 adds a manual reference BPM. Older rows have no saved reference.
// Availability is refreshed from regular source-file existence, never saved.
class LibraryCatalog {
public:
    static constexpr std::size_t maxEntries = 1024;
    static constexpr std::size_t maxBytes = 1024U * 1024;

    explicit LibraryCatalog(QString absoluteFilePath);
    std::vector<LibraryTrack> read() const;
    // Reimports retain user metadata for the same content.
    std::vector<LibraryTrack> upsert(const LibraryTrack& track);
    std::vector<LibraryTrack> updateCues(const QString& sourceId, std::uint64_t expectedFrames,
                                       const std::vector<CuePoint>& cues);
    std::vector<LibraryTrack> updateSavedCue(const QString& sourceId, std::uint64_t expectedFrames,
                                           std::uint64_t frame);
    std::vector<LibraryTrack> updateManualBpm(const QString& sourceId, std::uint64_t expectedFrames,
                                            std::optional<double> bpm);

private:
    QString filePath_;
};

} // namespace conflux::library

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
};

// Worker-only metadata persistence; this never hashes, decodes or prepares audio.
// Version 4 adds one manually saved source-frame CUE; older rows read as unknown.
// Availability is refreshed from regular source-file existence, never saved.
class LibraryCatalog {
public:
    static constexpr std::size_t maxEntries = 1024;
    static constexpr std::size_t maxBytes = 1024U * 1024;

    explicit LibraryCatalog(QString absoluteFilePath);
    std::vector<LibraryTrack> read() const;
    // Audio reimports retain existing cues for the same content. Only the
    // dedicated updates install imported points or a manually saved CUE.
    std::vector<LibraryTrack> upsert(const LibraryTrack& track);
    std::vector<LibraryTrack> updateCues(const QString& sourceId, std::uint64_t expectedFrames,
                                       const std::vector<CuePoint>& cues);
    std::vector<LibraryTrack> updateSavedCue(const QString& sourceId, std::uint64_t expectedFrames,
                                           std::uint64_t frame);

private:
    QString filePath_;
};

} // namespace conflux::library

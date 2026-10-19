// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace conflux::library {

struct LibraryTrack {
    QString sourceSha256;
    QString sourcePath;
    QString name;
    QString artifactKey;
    std::uint64_t frames = 0;
    bool available = false;
};

// Worker-only metadata persistence; this never hashes, decodes or prepares audio.
// Version 1 stores exactly the five identity/path/name/artifact/frame fields.
// Availability is refreshed from regular source-file existence, never saved.
class LibraryCatalog {
public:
    static constexpr std::size_t maxEntries = 1024;
    static constexpr std::size_t maxBytes = 1024U * 1024;

    explicit LibraryCatalog(QString absoluteFilePath);
    std::vector<LibraryTrack> read() const;
    std::vector<LibraryTrack> upsert(const LibraryTrack& track);

private:
    QString filePath_;
};

} // namespace conflux::library

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>
#include <stop_token>

namespace conflux::library {

struct TrackPresentation {
    QString title, artist, album, artworkDataUrl;
    bool operator==(const TrackPresentation&) const = default;
};

// Worker-only optional tags/artwork from the already retained local source.
// Preserves its offset and bytes; cancellation/source changes fail the load.
// Unsupported or malformed optional tags/artwork use the filename/no-art fallback.
// Presentation support: FLAC, basic uncompressed MPEG ID3, Ogg Vorbis/Opus.
// Other containers (including WAV/AIFF/MP4) retain their ordinary audio path.
TrackPresentation readTrackPresentation(int descriptor, const QString& sourcePath,
                                        std::stop_token stop = {});

} // namespace conflux::library

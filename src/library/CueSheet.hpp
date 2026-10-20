// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "library/LibraryCatalog.hpp"
#include <stop_token>

namespace conflux::library {

struct CueSheetResult {
    std::vector<CuePoint> cues;
    QString error;
    bool cancelled = false;
};

// Import AUDIO track INDEX 01 starts for one already-imported source. Performs
// bounded local-file reads and source identity checks on a worker, never audio
// decoding or playback. Other FILE references are parsed but never opened.
CueSheetResult readCueSheet(const QString& absoluteCuePath, const LibraryTrack& selected,
                           std::stop_token stop = {});

} // namespace conflux::library

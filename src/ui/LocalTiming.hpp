// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"
#include "library/BpmAnalyzer.hpp"

#include <QVariantMap>
#include <cmath>
#include <optional>

namespace conflux::ui {

// GUI-thread projection only. This is an original-speed interbeat estimate,
// independent of the editable reference BPM, DSP output clock or confidence.
inline QVariantMap localTimingForDisplay(const library::BpmEstimate& estimate, double sourcePosition,
    const audio::AudioResidentHoldState& hold = {},
    const std::optional<audio::AudioResidentHoldRange>& recoveryRange = {})
{
    QVariantMap result{{"status", "Unknown"}, {"bpm", 0.}, {"confidence", "Unknown"},
        {"positionKnown", false}, {"sourceFrame", qulonglong(0)},
        {"firstFrame", qulonglong(0)}, {"endFrame", qulonglong(0)}, {"resident", hold.active}};
    if ((estimate.status != "Estimated" && estimate.status != "Unknown") ||
        estimate.pcmSha256.size() != 64 || !estimate.analysisFrames ||
        estimate.analysisFrames > audio::PreparedAudio::defaultMaxPreparedBytes / 8U ||
        !std::isfinite(sourcePosition) || sourcePosition < 0 ||
        sourcePosition > double(audio::PreparedAudio::defaultMaxPreparedBytes / 8U))
        return result;
    auto frame = static_cast<std::uint64_t>(sourcePosition);
    if (hold.active) {
        // The service cursor is parked during resident playback. Require this
        // installed owner's exact range, and use its audible source cursor.
        // At OUT the next snapshot must establish wrap versus finite takeover;
        // neither the parked cursor nor a guessed wrap is an audible position.
        if (!recoveryRange || !recoveryRange->id || !hold.supported || !hold.ready ||
            hold.rangeId != recoveryRange->id || hold.provenance != recoveryRange->provenance ||
            hold.inFrame != recoveryRange->inFrame || hold.outFrame != recoveryRange->outFrame ||
            sourcePosition != double(hold.linearFrame) || hold.audibleFrame < hold.inFrame ||
            hold.audibleFrame >= hold.outFrame)
            return result;
        frame = hold.audibleFrame;
    }
    if (frame >= estimate.analysisFrames) return result;
    result["positionKnown"] = true;
    result["sourceFrame"] = QVariant::fromValue<qulonglong>(frame);
    // The existing bounded query validates the full map and never extrapolates.
    if (const auto interval = library::localTempoAt(estimate, frame)) {
        result["status"] = "Estimated";
        result["bpm"] = interval->bpm;
        result["firstFrame"] = QVariant::fromValue<qulonglong>(interval->firstFrame);
        result["endFrame"] = QVariant::fromValue<qulonglong>(interval->endFrame);
    }
    return result;
}

} // namespace conflux::ui

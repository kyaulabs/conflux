// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"
#include "library/BpmAnalyzer.hpp"

#include <QVariantMap>
#include <cmath>
#include <optional>

namespace conflux::ui {

// GUI-thread clock for metadata attached to the original source.
inline std::optional<std::uint64_t> analysisSourceFrame(double sourcePosition,
    const audio::AudioResidentHoldState& hold,
    const std::optional<audio::AudioResidentHoldRange>& recoveryRange) noexcept
{
    if (!std::isfinite(sourcePosition) || sourcePosition < 0 ||
        sourcePosition > double(audio::PreparedAudio::defaultMaxPreparedBytes / 8U))
        return std::nullopt;
    const auto frame = static_cast<std::uint64_t>(sourcePosition);
    if (!hold.active) return frame;
    // The service cursor is parked during resident playback. Master Tempo
    // can park at a fractional position, while the hold reports its frame.
    // At OUT the next snapshot must establish wrap versus finite takeover.
    if (!recoveryRange || !recoveryRange->id || !hold.supported || !hold.ready ||
        hold.rangeId != recoveryRange->id || hold.provenance != recoveryRange->provenance ||
        hold.inFrame != recoveryRange->inFrame || hold.outFrame != recoveryRange->outFrame ||
        frame != hold.linearFrame || hold.audibleFrame < hold.inFrame || hold.audibleFrame >= hold.outFrame)
        return std::nullopt;
    return hold.audibleFrame;
}

// This original-speed interbeat estimate is independent of the editable
// reference BPM, DSP output clock or confidence.
inline QVariantMap localTimingForDisplay(const library::BpmTiming& timing, double sourcePosition,
    const audio::AudioResidentHoldState& hold = {},
    const std::optional<audio::AudioResidentHoldRange>& recoveryRange = {})
{
    const auto& estimate = timing.estimate();
    QVariantMap result{{"status", "Unknown"}, {"bpm", 0.}, {"confidence", "Unknown"},
        {"positionKnown", false}, {"sourceFrame", qulonglong(0)},
        {"firstFrame", qulonglong(0)}, {"endFrame", qulonglong(0)}, {"resident", hold.active},
        {"bar", QVariantMap{}}};
    if ((estimate.status != "Estimated" && estimate.status != "Unknown") ||
        estimate.pcmSha256.size() != 64 || !estimate.analysisFrames ||
        estimate.analysisFrames > audio::PreparedAudio::defaultMaxPreparedBytes / 8U)
        return result;
    const auto frame = analysisSourceFrame(sourcePosition, hold, recoveryRange);
    if (!frame || *frame >= estimate.analysisFrames) return result;
    result["positionKnown"] = true;
    result["sourceFrame"] = QVariant::fromValue<qulonglong>(*frame);
    if (const auto interval = timing.at(*frame)) {
        result["status"] = "Estimated";
        result["bpm"] = interval->bpm;
        result["firstFrame"] = QVariant::fromValue<qulonglong>(interval->firstFrame);
        result["endFrame"] = QVariant::fromValue<qulonglong>(interval->endFrame);
        if (const auto bar = timing.barAt(*frame)) {
            result["bar"] = QVariantMap{{"firstFrame", bar->firstFrame}, {"endFrame", bar->endFrame},
                {"beat", bar->beat}, {"beats", bar->beats}};
        }
    }
    return result;
}

} // namespace conflux::ui

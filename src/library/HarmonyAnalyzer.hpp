// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioFile.hpp"
#include <QByteArray>
#include <QString>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace conflux::library {
inline constexpr char harmonyAnalysisRecipe[] = "local-chroma-krumhansl-v1";
inline constexpr std::uint64_t harmonyWindowFrames = 192000;
struct HarmonyEngine {
    QString pythonExecutable, workerPath;
    int maxSeconds = 30;
};
struct HarmonyCandidate {
    int tonic = 0; // C=0, C#=1, ..., B=11; no Camelot/pitch-shift semantics.
    QString mode;
    double correlation = 0; // Raw profile similarity, never confidence.
};
struct HarmonyWindow {
    std::uint64_t firstFrame = 0, endFrame = 0;
    QString status, reason;
    std::array<double, 12> chroma{};
    std::vector<HarmonyCandidate> candidates;
};
struct HarmonyEstimate {
    QString status = "Unavailable", error;
    std::uint64_t analysisFrames = 0;
    QByteArray pcmSha256;
    std::vector<HarmonyWindow> windows;
};
class HarmonySourceError : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};
// Worker only. Original immutable stereo48k PCM; no learned model or cache.
// Fixed source windows yield unqualified candidates, not tonal truth, bars,
// tuning, compatibility approval or calibrated confidence. Ordinary analysis
// failures return Failed; corrupt PCM throws and cancellation discards load.
HarmonyEstimate analyzeHarmony(const audio::PreparedAudio&, const HarmonyEngine&, std::stop_token stop = {});
// Off-callback query. Returns the containing source interval, including its
// explicit Unknown/Ambiguous state. No extrapolation, allocation or clock shift.
const HarmonyWindow* localHarmonyAt(const HarmonyEstimate&, std::uint64_t sourceFrame) noexcept;
} // namespace conflux::library

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioFile.hpp"

#include <QString>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace conflux::library {

inline constexpr char bpmAnalysisRecipe[] = "beat-this-final0-linear-mel-v1";

struct BpmEngine {
    QString pythonExecutable, sourceDirectory, modelPath, workerPath;
    int maxSeconds = 180; // Worker deadline, 1..180; bounded cleanup follows it.
};

struct BpmEstimate {
    std::optional<double> bpm;
    QString status = QStringLiteral("Unavailable");
    QString error;
};

class BpmSourceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Worker only. Reads the retained immutable stereo48k native-float PCM using
// positioned reads; its original FD offset and lifetime stay with the caller.
// The isolated child receives that exact PCM through stdin. Ordinary analysis
// errors return Failed/null BPM without failing playable audio. Cancellation
// throws runtime_error, allowing the existing load acceptance gate to discard.
// Corrupt/changed PCM throws BpmSourceError so the load cannot accept it.
// Child address space is capped at 8 GiB; this is not a hard RSS guarantee.
BpmEstimate analyzeBpm(const audio::PreparedAudio&, const BpmEngine&, std::stop_token stop = {});

// Manual target entry, independent of analysis confidence. Validate the actual
// target rail before correcting tiny floating rounding at the +/-6% endpoints.
std::optional<double> tempoPercentForBpm(double reference, double target) noexcept;

} // namespace conflux::library

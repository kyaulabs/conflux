// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioRouting.hpp"

#include <algorithm>

namespace conflux::audio {

bool samePortIdentity(const PortIdentity& a, const PortIdentity& b) noexcept
{
    return a.globalId == b.globalId && a.nodeId == b.nodeId &&
        a.serial == b.serial && a.nodeSerial == b.nodeSerial;
}

bool isPlaybackPort(const PortIdentity& port) noexcept
{
    constexpr auto invalid = std::numeric_limits<std::uint32_t>::max();
    return port.globalId != invalid && port.nodeId != invalid && port.serial && port.nodeSerial &&
        port.input && !port.monitor && port.audioSink && port.monoDsp;
}

namespace {
std::optional<RoutePair> validatePair(const RoutingSnapshot& snapshot, const RoutePair& selected,
                                    std::string& error, bool& missing)
{
    auto find = [&](const PortIdentity& identity) -> const PortIdentity* {
        const PortIdentity* found = nullptr;
        for (const auto& port : snapshot.playbackPorts) {
            if (!samePortIdentity(identity, port)) continue;
            if (found) {
                error = "Selected playback port identity is ambiguous";
                return nullptr;
            }
            found = &port;
        }
        if (!found) {
            missing = true;
            error = "Selected playback port is missing or its runtime identity changed; select it again";
        }
        return found;
    };
    const auto* left = find(selected.left);
    if (!left) return {};
    const auto* right = find(selected.right);
    if (!right) return {};
    if (!isPlaybackPort(*left) || !isPlaybackPort(*right)) {
        error = "Select mono audio playback inputs of an Audio/Sink, excluding monitor ports";
        return {};
    }
    if (left->globalId == right->globalId || left->nodeId != right->nodeId || left->nodeSerial != right->nodeSerial) {
        error = "A stereo pair requires two distinct ports of the same sink";
        return {};
    }
    return RoutePair{*left, *right};
}

std::optional<RoutePair> defaultPair(const RoutingSnapshot& snapshot, std::string& error)
{
    if (snapshot.defaultSinkName.empty()) {
        error = "No default audio sink is available; select a master pair";
        return {};
    }
    const PortIdentity* left = nullptr;
    const PortIdentity* right = nullptr;
    for (const auto& port : snapshot.playbackPorts) {
        if (port.nodeName != snapshot.defaultSinkName || !isPlaybackPort(port)) continue;
        auto** target = port.channel == "FL" ? &left : port.channel == "FR" ? &right : nullptr;
        if (!target) continue;
        if (*target) {
            error = "Default sink does not have one unambiguous FL/FR playback pair";
            return {};
        }
        *target = &port;
    }
    if (!left || !right || left->nodeId != right->nodeId || left->nodeSerial != right->nodeSerial ||
        left->globalId == right->globalId) {
        error = "Default sink does not have one complete FL/FR playback pair";
        return {};
    }
    return RoutePair{*left, *right};
}
} // namespace

ResolvedRouting resolveAudioRouting(const RoutingSnapshot& snapshot, const RoutingConfig& config)
{
    ResolvedRouting result;
    bool missing = false;
    result.master = config.master ? validatePair(snapshot, *config.master, result.masterError, missing)
                                  : defaultPair(snapshot, result.masterError);
    if (!config.cue) return result;
    missing = false;
    result.cue = validatePair(snapshot, *config.cue, result.cueError, missing);
    if (!result.cue) {
        result.cueStatus = missing ? CueRouteStatus::MissingEndpoint : CueRouteStatus::InvalidSelection;
        return result;
    }
    if (!result.master) {
        result.cue.reset();
        result.cueStatus = CueRouteStatus::InvalidSelection;
        result.cueError = "Cue remains muted until the master pair is valid";
        return result;
    }
    const auto& master = *result.master;
    const auto& cue = *result.cue;
    if (master.left.globalId == cue.left.globalId || master.left.globalId == cue.right.globalId ||
        master.right.globalId == cue.left.globalId || master.right.globalId == cue.right.globalId) {
        result.cue.reset();
        result.cueStatus = CueRouteStatus::InvalidSelection;
        result.cueError = "Master and cue require four distinct playback ports";
        return result;
    }
    result.cueStatus = CueRouteStatus::Pending;
    return result;
}

} // namespace conflux::audio

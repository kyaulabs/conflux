// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace conflux::audio {

enum class CueRouteStatus : std::uint8_t {
    Disabled, Pending, Ready, InvalidSelection, MissingEndpoint, LinkError, UnexpectedLink
};

// Runtime identities, not persistent device IDs. Names are labels; resolution
// requires the current node/port IDs and serials to match after every removal.
struct PortIdentity {
    std::uint32_t globalId = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t nodeId = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t serial = 0, nodeSerial = 0;
    std::string nodeName, portName, channel;
    bool input = false, monitor = false, audioSink = false, monoDsp = false;
};

struct RoutePair { PortIdentity left, right; };

struct RoutingConfig {
    // Unset master selects unique FL/FR inputs of default.audio.sink at start.
    // Unset cue is disabled. Missing explicit endpoints never fall back.
    std::optional<RoutePair> master, cue;
};

struct RoutingSnapshot {
    std::vector<PortIdentity> playbackPorts;
    std::string defaultSinkName;
    std::optional<RoutePair> defaultMaster, master, cue;
    bool masterReady = false, cueReady = false;
    CueRouteStatus cueStatus = CueRouteStatus::Disabled;
    std::string masterError, cueError;
};

struct ResolvedRouting {
    std::optional<RoutePair> master, cue;
    CueRouteStatus cueStatus = CueRouteStatus::Disabled;
    std::string masterError, cueError;
};

[[nodiscard]] bool samePortIdentity(const PortIdentity& a, const PortIdentity& b) noexcept;
[[nodiscard]] bool isPlaybackPort(const PortIdentity& port) noexcept;
// Control-thread only; no PipeWire dependency. A cue failure leaves a valid
// master intact. Four distinct destination ports are required for active cue.
[[nodiscard]] ResolvedRouting resolveAudioRouting(const RoutingSnapshot& snapshot,
                                                const RoutingConfig& config);

} // namespace conflux::audio

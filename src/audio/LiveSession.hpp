// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/RenderCore.hpp"
#include "audio/AudioRouting.hpp"
#include "audio/CallbackTiming.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace conflux::audio {
class PreparedAudio;
class StereoRecorder;

// One control-thread owner; process callbacks own DSP while running. Borrowed
// immutable PCM must outlive the session. Source replacement requires stop.
class LiveSession {
public:
    struct InitialState {
        std::array<std::uint64_t, RenderCore::deckCount> sourceFrame{};
        std::array<std::uint64_t, RenderCore::deckCount> cueFrame{};
        std::array<bool, RenderCore::deckCount> playing{};
        std::array<float, RenderCore::deckCount> deckGain{1.0F, 1.0F};
        float masterGain = 1.0F;
        std::array<bool, RenderCore::deckCount> cueSelected{};
        float cueGain = 1.0F;
    };

    struct State {
        std::uint64_t outputFrame = 0;
        std::array<std::uint64_t, RenderCore::deckCount> sourceFrame{}, sourceFrames{};
        std::array<std::uint64_t, RenderCore::deckCount> cueFrame{};
        std::array<bool, RenderCore::deckCount> playing{};
        std::array<bool, RenderCore::deckCount> cueHeld{}, cuePreview{};
        std::array<bool, RenderCore::deckCount> buffering{}, sourceReadError{};
        std::array<bool, RenderCore::deckCount> sourceReadErrorObserved{};
        std::array<std::uint64_t, RenderCore::deckCount> starvedFrames{};
        std::array<float, RenderCore::deckCount> deckGain{};
        float masterGain = 1.0F;
        std::array<bool, RenderCore::deckCount> cueSelected{};
        float cueGain = 1.0F;
        RenderCore::Meters meters{};
        bool cueReady = false;
        CueRouteStatus cueRouteStatus = CueRouteStatus::Disabled;
        std::uint32_t graphRate = 0;
        std::uint64_t applied = 0, rejected = 0, lastCommandId = 0;
        std::uint64_t callbacks = 0, invalidBuffers = 0, snapshotDrops = 0;
        std::uint64_t maxExecutionNs = 0, deadlineMisses = 0;
        std::uint64_t streamFramesMin = 0, streamFramesMax = 0;
        bool ready = false;
    };

    explicit LiveSession(std::span<const float> a = {}, std::span<const float> b = {});
    LiveSession(std::span<const float> a, std::span<const float> b, const InitialState& initial);
    using PreparedSources = std::array<const PreparedAudio*, RenderCore::deckCount>;
    // Duplicated file readers own fixed read-ahead caches, prime before output,
    // and stop after callbacks. Prepared owners may then be released safely.
    LiveSession(PreparedSources sources, const InitialState& initial);
    ~LiveSession();
    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    // Connect asynchronously. Poll ready/error from the control thread; error()
    // also enforces the five-second deadline for the first rendered audio block.
    // Later readiness changes do not restart that deadline. Immediate failure
    // returns only after resources are quiesced.
    bool start(std::string& error);
    bool start(std::string& error, const RoutingConfig& routing);
    // Export an inactive, unlinked filter for control-only endpoint discovery.
    // No audio is processed. Stop before starting a playback session.
    bool startDiscovery(std::string& error);
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string error();
    bool submit(const RenderCore::Command& command) noexcept;
    bool pollState(State& state) noexcept;
    // Control owner only. Current exact endpoint identities and route status;
    // stale selections stay muted until explicitly selected in a new session.
    [[nodiscard]] RoutingSnapshot routingSnapshot() const;
    // No percentile scan while callbacks run. Throws if the session is active.
    [[nodiscard]] CallbackTiming::Summary stoppedTimingSummary() const;
    // Keep each writer alive until detach is acknowledged or stop returns.
    bool requestRecorder(StereoRecorder* recorder) noexcept;
    [[nodiscard]] bool recorderChangePending() const noexcept;
    [[nodiscard]] bool recorderAttached() const noexcept;
    // Quiesces callbacks, then applies remaining accepted commands. Idempotent.
    void stop() noexcept;
    // Exact final DSP state, only before start or after stop. Output clock and
    // counters belong to this session; constructing another session resets them.
    [[nodiscard]] State stoppedState() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace conflux::audio

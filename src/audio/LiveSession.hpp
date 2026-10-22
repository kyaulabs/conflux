// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/RenderCore.hpp"
#include "audio/AudioRouting.hpp"
#include "audio/CallbackTiming.hpp"
#include "audio/PreparedPlayback.hpp"
#include "audio/SourceHandoff.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

namespace conflux::audio {
class PreparedAudio;
class StereoRecorder;

// One control-thread owner; process callbacks own DSP while running. Borrowed
// immutable memory PCM must outlive the session. Prepared owners are retained
// through callback-end source replacement acknowledgement and control acquire.
class LiveSession {
public:
    struct InitialState {
        std::array<std::uint64_t, RenderCore::deckCount> sourceFrame{};
        std::array<std::uint64_t, RenderCore::deckCount> cueFrame{};
        std::array<bool, RenderCore::deckCount> playing{};
        std::array<float, RenderCore::deckCount> deckGain{1.0F, 1.0F};
        float masterGain = 1.0F;
        float crossfader = 0.5F;
        std::array<bool, RenderCore::deckCount> cueSelected{};
        float cueGain = 1.0F;
        std::array<std::array<float, RenderCore::stemCount>, RenderCore::deckCount> stemGain{{
            {1.F, 1.F, 1.F, 1.F}, {1.F, 1.F, 1.F, 1.F}}};
    };

    struct State {
        std::uint64_t outputFrame = 0;
        std::array<std::uint64_t, RenderCore::deckCount> sourceFrame{}, sourceFrames{};
        std::array<double, RenderCore::deckCount> sourcePosition{};
        std::array<std::uint64_t, RenderCore::deckCount> sourceGeneration{};
        std::array<std::uint64_t, RenderCore::deckCount> cueFrame{};
        std::array<bool, RenderCore::deckCount> playing{};
        std::array<bool, RenderCore::deckCount> cueHeld{}, cuePreview{};
        std::array<bool, RenderCore::deckCount> buffering{}, sourceReadError{};
        std::array<bool, RenderCore::deckCount> sourceReadErrorObserved{};
        std::array<std::uint64_t, RenderCore::deckCount> starvedFrames{};
        std::array<float, RenderCore::deckCount> deckGain{};
        std::array<bool, RenderCore::deckCount> stemBank{};
        std::array<AudioLoopState, RenderCore::deckCount> loops{};
        std::array<AudioTempoState, RenderCore::deckCount> tempo{};
        std::array<AudioScratchState, RenderCore::deckCount> scratch{};
        std::array<std::array<float, RenderCore::stemCount>, RenderCore::deckCount> stemGain{};
        float masterGain = 1.0F;
        float crossfader = 0.5F;
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
    using PlaybackSources = std::array<std::unique_ptr<PreparedPlayback>, RenderCore::deckCount>;
    // Takes whole normal/stem-bank owners and primes them before output starts.
    LiveSession(PlaybackSources sources, const InitialState& initial, std::stop_token stop = {});
    ~LiveSession();
    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    // Connect asynchronously. Poll ready/error from the control thread; error()
    // also enforces the five-second deadline for the first rendered audio block.
    // Later readiness changes do not restart that deadline. Immediate failure
    // returns only after callbacks are quiesced. Failed starts retain source
    // owners for explicit stop/off-thread retirement and cannot be restarted.
    bool start(std::string& error);
    bool start(std::string& error, const RoutingConfig& routing);
    // Export an inactive, unlinked filter for control-only endpoint discovery.
    // No audio is processed. Stop before starting a playback session.
    bool startDiscovery(std::string& error);
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string error();
    bool submit(const RenderCore::Command& command) noexcept;
    bool pollState(State& state) noexcept;
    struct SourceReplacementResult {
        SourceHandoff::Completion completion;
        std::unique_ptr<PreparedPlayback> retiredPlayback;
    };
    // One control-owner request. Prime the candidate at frame zero off the GUI.
    // Generations increase across requests in this session. Rejection leaves
    // it owned by the caller. Collect completion before another
    // request; stop/join its retired whole owner away from GUI/render threads.
    bool requestSourceReplacement(std::size_t deck, std::unique_ptr<PreparedPlayback>& candidate,
                                  std::uint64_t generation) noexcept;
    [[nodiscard]] bool sourceReplacementPending() const noexcept;
    std::optional<SourceReplacementResult> takeSourceReplacementResult() noexcept;
    struct StoppedPlaybacks {
        PlaybackSources active;
        std::optional<SourceReplacementResult> replacement;
    };
    // Quiesce all callbacks and return whole active+retired/candidate owners,
    // without stop requests, joins or producer cleanup. Retire the bundle off
    // GUI; this session becomes terminal and detached callbacks are never used.
    StoppedPlaybacks stopAndTakePlaybacks() noexcept;
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

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <algorithm>
#include <cstdint>

namespace conflux::experiments {

// Actual-player harness policy and fixed-size measured-loop UI accounting.
class PlaybackUiPolicy {
public:
    static constexpr std::int64_t frameAgeLimitNs = 2000000000LL;
    struct State {
        bool exists, visible, exposed, hasFrame;
        std::int64_t frameAgeNs;
    };

    explicit PlaybackUiPolicy(bool background = false) : background_(background) {}

    bool requiresStartupPresentation(bool functional) const noexcept
    { return !functional && !background_; }

    bool healthy(bool functional, const State& state) const noexcept
    {
        if (background_) return state.exists;
        return functional || (state.exists && state.visible && state.exposed && state.frameAgeNs < frameAgeLimitNs);
    }

    void observe(const State& state) noexcept
    {
        ++checks_;
        missingWindow_ += !state.exists;
        hidden_ += !state.visible;
        unexposed_ += !state.exposed;
        missingFrame_ += !state.hasFrame;
        staleFrame_ += state.frameAgeNs >= frameAgeLimitNs;
        const bool covered = state.exists && state.visible && state.exposed && state.hasFrame &&
            state.frameAgeNs < frameAgeLimitNs;
        visibleHealthy_ += covered;
        if (state.hasFrame) maximumFrameAgeNs_ = std::max(maximumFrameAgeNs_, state.frameAgeNs);
    }

    QJsonObject report() const
    {
        return {{"checks", qint64(checks_)}, {"visible_healthy_checks", qint64(visibleHealthy_)},
            {"missing_window_checks", qint64(missingWindow_)}, {"hidden_checks", qint64(hidden_)},
            {"unexposed_checks", qint64(unexposed_)}, {"stale_frame_checks", qint64(staleFrame_)},
            {"missing_frame_checks", qint64(missingFrame_)}, {"maximum_frame_age_ns", qint64(maximumFrameAgeNs_)},
            {"any_gap", visibleHealthy_ != checks_}, {"visible_ui_qualification_eligible", !background_},
            {"scope", "Measured-loop snapshots only; frame ages follow observed frameSwapped callbacks, not physical scanout"},
            {"capture_end", "Before controller shutdown and window close"}};
    }

private:
    bool background_;
    std::uint64_t checks_ = 0, visibleHealthy_ = 0, missingWindow_ = 0, hidden_ = 0;
    std::uint64_t unexposed_ = 0, staleFrame_ = 0, missingFrame_ = 0;
    std::int64_t maximumFrameAgeNs_ = 0;
};

} // namespace conflux::experiments

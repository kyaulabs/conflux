// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"
#include "audio/RenderCore.hpp"

#include <atomic>
#include <cstdint>
#include <cmath>
#include <limits>

namespace conflux::audio {
class PreparedPlayback;

// One control producer and one render consumer. Owners remain outside this
// mailbox until completion is acquired, or callbacks have been quiesced.
class SourceHandoff {
public:
    enum class Kind { Replacement, Continuation };
    struct Request {
        std::size_t deck = 0;
        AudioSource source{};
        PreparedPlayback* playback = nullptr;
        std::uint64_t generation = 0;
        Kind kind = Kind::Replacement;
        std::uint64_t expectedSourceGeneration = 0, expectedTransportRevision = 0;
        double position = 0; // Exact nominal cursor; the containing raw frame may be fractional.
        // Nonzero continues at this active resident range's audible OUT, with
        // the linear frame parked. Zero retains ordinary immediate continuation.
        std::uint64_t residentHoldRangeId = 0;
    };
    struct RetiredState {
        std::uint64_t sourceFrame = 0, starvedFrames = 0;
        bool readFailed = false;
    };
    struct Completion {
        std::size_t deck = 0;
        std::uint64_t generation = 0, outputFrame = 0;
        RetiredState retired{};
        bool installed = false;
        Kind kind = Kind::Replacement;
    };

    // Source validation and reader priming belong to the control owner. An
    // uncollected completion also keeps the slot busy, preventing overwrite.
    bool request(const Request& next) noexcept
    {
        const auto revision = requested_.load(std::memory_order_relaxed);
        const bool continuation = next.kind == Kind::Continuation;
        if (revision != collected_ || next.deck >= RenderCore::deckCount ||
            !next.source.frames || !next.generation ||
            (next.kind != Kind::Replacement && !continuation) ||
            (!continuation && next.residentHoldRangeId) ||
            (continuation && (!std::isfinite(next.position) || next.position < 0 || next.position >= double(next.source.frames) ||
                next.generation <= next.expectedSourceGeneration ||
                next.expectedTransportRevision == std::numeric_limits<std::uint64_t>::max())))
            return false;
        pending_ = next;
        requested_.store(revision + 1, std::memory_order_release);
        return true;
    }

    // Render owner: capture BEFORE draining commands at callback entry. The
    // replacement can remain visible while its old deck fades to silence;
    // continuation instead validates the retained state without pausing it,
    // and a range-bound request can wait across callbacks for its audible OUT.
    bool readPending(Request& result) noexcept
    {
        const auto revision = requested_.load(std::memory_order_acquire);
        if (revision == completed_) return false;
        result = pending_;
        observed_ = revision;
        return true;
    }

    // Render owner, at callback END after the actual swap and all final source
    // access. Acknowledging a request never releases or destroys its owners.
    bool complete(bool installed, const RetiredState& retired, std::uint64_t outputFrame) noexcept
    {
        if (observed_ == completed_) return false;
        completion_ = {pending_.deck, pending_.generation, outputFrame, retired, installed, pending_.kind};
        completed_ = observed_;
        acknowledged_.store(completed_, std::memory_order_release);
        return true;
    }

    // Control owner only. Acquire the last render use before retiring storage.
    bool takeCompletion(Completion& result) noexcept
    {
        const auto revision = acknowledged_.load(std::memory_order_acquire);
        if (revision == collected_) return false;
        result = completion_;
        collected_ = revision;
        return true;
    }
    [[nodiscard]] bool busy() const noexcept
    { return requested_.load(std::memory_order_relaxed) != collected_; }

    // Control owner only, after callbacks stop. An uninstalled candidate can
    // now be returned to its owner even when no process callback observed it.
    void quiesce() noexcept
    {
        const auto revision = requested_.load(std::memory_order_acquire);
        if (revision == completed_) return;
        observed_ = revision;
        (void)complete(false, {}, 0);
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    Request pending_{};
    Completion completion_{};
    std::uint64_t observed_ = 0, completed_ = 0, collected_ = 0;
    std::atomic<std::uint64_t> requested_{0}, acknowledged_{0};
};
} // namespace conflux::audio

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace conflux::audio::detail {

// Private production policy used by PreparedPlayback's existing producer thread.
// No clock calls, callback state, files, condition variables or timer thread.
class LiveRecoveryPolicy {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::uint64_t reserveFrames = 262144;
    static constexpr auto maximumWait = std::chrono::seconds(60);
    static constexpr std::uint64_t terminalBit = std::uint64_t(1) << 63;
    static constexpr std::uint64_t maximumToken = terminalBit - 1;

    struct Demand {
        // Nonzero token changes on a new wait, actual active edge or gate opening.
        // Quiescence disarms through an actual inactive edge if needed.
        // Token also forms the SC mailbox's2*token version.
        std::uint64_t token = 0, waitSerial = 0, epoch = 0, cursor = 0;
        bool active = false;
        // waitSerial0 explicitly means gate open/no recovery attempt.
        // Closed waits have never-reused nonzero serials across pause/resume.
    };
    struct Coverage { std::uint64_t epoch = 0, end = 0; };
    struct Decision {
        enum class Kind { None, Grant, Timeout };
        Kind kind = Kind::None;
        std::uint64_t token = 0, waitSerial = 0;
    };

    // Fixed positive exact output geometry, validated by the owner.
    explicit LiveRecoveryPolicy(std::uint64_t totalFrames);
    // Only the producer calls update, with a coherent validated demand matching
    // the current nonterminal atomic attempt token and accepted coverage.
    // Synthetic time is only an internal policy-test input; production passes
    // Clock::now from LiveProducerPort::serviceRecovery, not a public test clock.
    //
    // Open/EOF disarm. Paused closed demand disarms but may grant ready reserve.
    // A new active token arms once if not already ready; partial progress never
    // renews. Existing attempt at now>=deadline times out before a late grant.
    // A new active/resumed token with ready reserve grants without a new wait.
    Decision update(Demand, Coverage, Clock::time_point now) noexcept;

    // The owner stores one atomic attempt word, initially1. Exactly one render
    // owner calls advanceAttempt(old,old+1), BEFORE publishing its new mailbox.
    // Producer first rejects a mailbox/attempt mismatch. Its only competing
    // write is commitTimeout on an exact timeout decision. No RT retry loop:
    // with valid render inputs a failed CAS means timeout won and stays sticky.
    static bool advanceAttempt(std::atomic<std::uint64_t>&, std::uint64_t oldToken,
                               std::uint64_t newToken) noexcept;
    static bool commitTimeout(std::atomic<std::uint64_t>&, const Decision&) noexcept;
    static bool timedOut(std::uint64_t word) noexcept;
    // Invalid/zero/overflow/reused token input rejects without changing word.
    // Timeout is exact-token CAS to(token|terminalBit). It never overwrites a
    // superseding pause/seek/open token. Further advance cannot clear terminal.

private:
    std::uint64_t totalFrames_, armedToken_ = 0;
    Clock::time_point deadline_{};
};

} // namespace conflux::audio::detail

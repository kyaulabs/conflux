// Source-only API proposal. Not a product header or implementation patch.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace conflux::library {
class PreparedTrackStore;
namespace detail { struct LiveStemObservationWriter; }

// Concrete service/bridge diagnostics. No PCM, paths, callbacks or subscribers.
class LiveStemObservations final {
public:
    static constexpr std::size_t capacity = 256;
    using Sha256Hex = std::array<char, 64>; // Exact validated lower-case hex; no NUL.

    struct Setup {
        // C++ std::chrono::steady_clock time_since_epoch, nanoseconds.
        // Never subtract service Python-monotonic absolute stamps from these.
        std::uint64_t factoryEntryNs = 0, inputCapturedNs = 0;
        std::uint64_t countCompleteNs = 0, factoryCommittedNs = 0;
        std::uint64_t sourceToken = 0, inputFrames44100 = 0, outputFrames48000 = 0;
        Sha256Hex sourceSha256{}, inputSha256{}, modelSha256{}, runtimeSha256{}, serviceSha256{};
    };

    enum class Kind : std::uint8_t {
        ServiceReady, RuntimeReady, RequestObserved, CommandQueued, CommandAccepted,
        ChunkValidated, PacketAccepted, PacketDiscarded,
        RawComplete, ConverterComplete, ServiceClosed
    };

    struct Record {
        Kind kind = Kind::ServiceReady;
        bool hopServicePresent = false, fullObserved = false, final = false;
        bool serviceTelemetryDroppedPresent = false;
        std::uint64_t ordinal = 0, observedNs = 0;
        std::uint64_t providerEpoch = 0, workerGeneration = 0;
        std::uint64_t commandId = 0, requestId = 0, leaseSequence = 0;
        std::uint64_t inputFirst44100 = 0, inputFrames44100 = 0;
        std::uint64_t outputFirst48000 = 0, outputFrames48000 = 0;
        // ChunkValidated: observedNs is receipt, validatedNs is whole-lease end.
        std::uint64_t validatedNs = 0;
        // PacketAccepted only. Full timestamp is valid only if fullObserved.
        std::uint64_t packetReadyNs = 0, firstFullNs = 0;
        std::uint64_t publishBeginNs = 0, publishEndNs = 0;
        // ChunkValidated only: retain wire precision/units, without clock mapping.
        double workerReadySeconds = 0, servicePublishedSeconds = 0, hopServiceMs = 0;
        // RawComplete/ServiceClosed only when the corresponding present flag is true.
        std::uint64_t serviceTelemetryDropped = 0;
    };

    struct Counters {
        std::uint64_t attemptedRecords = 0, droppedRecords = 0, invalidClockReads = 0;
        // No further observation writes; not OS-thread exit/join/group cleanup.
        bool producerFinished = false;
    };

    LiveStemObservations(const LiveStemObservations&) = delete;
    LiveStemObservations& operator=(const LiveStemObservations&) = delete;
    // Exactly one off-callback consumer, beginning after factory return.
    // Empty leaves output unchanged; no allocation, blocking or formatting.
    bool tryPop(Record& output) noexcept;
    // Load producerFinished acquire first, then the counts. Exact final counts
    // require that flag true (or producer join); active samples are advisory.
    [[nodiscard]] Counters counters() const noexcept;
    // Consumer only after factory publication. Bridge must use job/request
    // context, never Setup while the factory may still be writing it.
    [[nodiscard]] const Setup& setup() const noexcept;

private:
    friend class PreparedTrackStore;
    // Private access implemented only beside LiveJob/Bridge, not a public writer.
    friend struct detail::LiveStemObservationWriter;
    LiveStemObservations() noexcept;
    Setup setup_{}; // Factory only; immutable before the handle is returned.
    std::array<Record, capacity> records_{};
    std::atomic<std::uint32_t> written_{0}, read_{0};
    std::atomic<std::uint64_t> attempted_{0}, dropped_{0}, clockErrors_{0};
    std::atomic<bool> producerFinished_{false};
};

// Proposed existing-surface additions, not declarations to apply here:
// PreparedLiveStemTrack::observations : shared_ptr<LiveStemObservations>
// DeckController::liveStemObservations(int deck) const
//     -> shared_ptr<library::LiveStemObservations>   // GUI/control owner only
// PreparedPcm retains that same handle for a live clip; finite clips have none.
} // namespace conflux::library

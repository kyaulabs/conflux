// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioSource.hpp"
#include "audio/PcmReadAhead.hpp"
#include "audio/LiveStemBuffer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <memory>
#include <string>

namespace conflux::audio {
class StemBankSource;
class PreparedLoopCache;
class PreparedTempoSource;
class PreparedScratchSource;

// One whole playback owner: a normal stereo source or four aligned stems.
// Construction, prime, error text and stop/join belong off the audio callback.
// Keep this object alive until the last render callback has been acknowledged.
class PreparedPlayback {
    class LiveState;
public:
    static constexpr std::size_t stemCount = 4;
    // Borrow readable regular FDs whose exact size is frames*2*sizeof(float).
    // Each reader duplicates its FD; callers retain their original descriptors.
    PreparedPlayback(int normalizedFd, std::uint64_t frames);
    PreparedPlayback(std::array<int, stemCount> normalizedFds, std::uint64_t frames);
    static constexpr std::uint64_t liveReserveFrames = 262144;
    static constexpr auto liveWait = std::chrono::seconds(60);
    class LiveProducerPort {
    public:
        bool tryRequest(LiveStemBuffer::Request&) const noexcept;
        LiveStemBuffer::PublishResult tryPublish(const LiveStemBuffer::Packet&) noexcept;
        void fail(int errorCode) noexcept;
        // Sole producer: service even while Full or without a new raw event.
        // Uses the real monotonic clock off callback; false means terminal/stop.
        bool serviceRecovery() noexcept;
    private:
        friend class PreparedPlayback;
        explicit LiveProducerPort(LiveState&) noexcept;
        LiveState* state_;
    };
    using LiveProducer = std::function<void(std::stop_token, LiveProducerPort&)>;
    // One owned non-RT thread; its callable must observe stop and never retain
    // this borrowed port beyond its invocation. Stop/join before bank storage
    // is destroyed. Callable/setup construction completes before thread start.
    static std::unique_ptr<PreparedPlayback> makeLive(
        std::uint64_t sourceId, std::uint64_t exactFrames, LiveProducer producer);
    [[nodiscard]] bool isLive() const noexcept { return bool(live_); }
    ~PreparedPlayback();
    PreparedPlayback(const PreparedPlayback&) = delete;
    PreparedPlayback& operator=(const PreparedPlayback&) = delete;

    [[nodiscard]] AudioSource sourceRef() noexcept;
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    // Quiesced and before any new sourceRef borrow. Rebuild only bank staging/
    // ramp setup over the same owned views. All old borrows become invalid.
    // Reject stopped owners; allocation failure preserves the old bank/owner.
    void resetBankForSession();
    // Before concurrent rendering. All readers share this single timeout budget.
    // Live owners require a clipped 262144-frame contiguous reserve and use
    // one cancellable deadline, at most60s. No view is consumed for readiness.
    bool prime(std::uint64_t frame, std::chrono::milliseconds timeout, std::stop_token stop = {});
    // Quiesced, before a session borrows this owner. Reset bank/ramp staging;
    // only a healthy, exact, never-read live reserve may avoid a new epoch.
    // Explicit prime() always requests a fresh epoch, even at the same cursor.
    bool primeForSession(std::uint64_t frame, std::chrono::milliseconds timeout, std::stop_token stop = {});
    // Configure before concurrent rendering. Normal PCM is an unchanged no-op;
    // a bank uses these frames for future gain changes. Never call from the GUI
    // once this owner has entered the render-boundary handoff.
    bool configureStemRamps(std::uint32_t frames) noexcept;
    // Atomic reader/control health only; safe to sample from the render owner.
    // RenderCore separately publishes failures reported by the bank's RT read.
    [[nodiscard]] int errorCode() const noexcept;
    [[nodiscard]] std::string error() const;
    // Quiesce the render consumer first. Idempotent; never call from callback.
    void stop() noexcept;

private:
    PreparedPlayback(std::unique_ptr<LiveState> live, std::uint64_t frames);
    void createLoopCache(std::span<const int> fds) noexcept;
    void createTempoSource(std::span<const int> fds) noexcept;
    void createScratchSource(std::span<const int> fds) noexcept;
    AudioReadResult readPrepared(std::uint64_t frame, std::span<float> output) noexcept;
    AudioReadResult readTempo(std::span<float> output) noexcept;
    AudioReadResult readScratch(std::span<float> output) noexcept;
    void seekPrepared(std::uint64_t frame) noexcept;
    void seekLinear(std::uint64_t frame) noexcept;
    void clearLoop() noexcept;
    bool loopControl(AudioLoopCommand command, std::uint64_t frame) noexcept;
    [[nodiscard]] AudioLoopState loopState() const noexcept;
    bool tempoControl(AudioTempoCommand command, double percent, bool keyLock, std::uint64_t frame) noexcept;
    [[nodiscard]] AudioTempoState tempoState() const noexcept;
    bool scratchControl(AudioScratchCommand command, std::int64_t cumulativeFrames,
                        double intervalSeconds, std::uint64_t frame) noexcept;
    [[nodiscard]] AudioScratchState scratchState() const noexcept;
    [[nodiscard]] int preparedRenderError() const noexcept;
    AudioReadResult readLive(std::uint64_t frame, std::span<float> output) noexcept;
    void seekLive(std::uint64_t frame) noexcept;
    void setLiveActive(bool active) noexcept;
    void resetBankForSessionAt(std::uint64_t frame);
    std::unique_ptr<LiveState> live_;
    std::array<std::unique_ptr<PcmReadAhead>, stemCount> readers_{};
    std::unique_ptr<StemBankSource> bank_;
    std::unique_ptr<PreparedLoopCache> loopCache_;
    std::unique_ptr<PreparedTempoSource> tempoSource_;
    // Optional bounded bidirectional PCM windows, owned with this playback.
    std::unique_ptr<PreparedScratchSource> scratchSource_;
    AudioLoopState loop_{}; // Sole render owner, or control after quiescence.
    int loopSetupError_ = 0;
    int tempoSetupError_ = 0;
    int scratchSetupError_ = 0;
    bool tempoKeyLock_ = false; // Legacy unity selection does not start DSP.
    std::uint64_t frames_;
    std::atomic<int> primeError_{0};
    bool stopped_ = false;
    // Render/control owner only across quiescence; producer never touches it.
    std::uint64_t livePrimeFrame_ = 0, livePrimeEpoch_ = 0;
    bool unusedLivePrime_ = false;
};
} // namespace conflux::audio

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/RenderCore.hpp"
#include "audio/StemBankSource.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace conflux::audio;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Command = RenderCore::Command;
using Type = Command::Type;
constexpr std::uint64_t loopIn = 17, loopOut = 4114;
constexpr std::array<float, 4> initialGains{1.F, .5F, .25F, 0.F};

void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
bool sameBits(float a, float b)
{ return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
template<class Predicate> void wait(Predicate&& predicate, const char* message)
{
    const auto deadline = Clock::now() + 5s;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
struct CloseFile { void operator()(std::FILE* file) const { if (file) std::fclose(file); } };
using File = std::unique_ptr<std::FILE, CloseFile>;
File rawFile(std::span<const float> pcm)
{
    File file(std::tmpfile());
    require(bool(file) && std::fwrite(pcm.data(), sizeof(float), pcm.size(), file.get()) == pcm.size() &&
                std::fflush(file.get()) == 0, "Cannot write private loop fixture");
    return file;
}

struct Signals {
    std::uint64_t frames;
    std::array<std::vector<float>, 4> leaves;
    std::array<File, 4> files;
    std::vector<float> peer = std::vector<float>(100000 * 2);
    explicit Signals(std::uint64_t count = 10000) : frames(count)
    {
        for (std::size_t leaf = 0; leaf < leaves.size(); ++leaf) {
            auto& pcm = leaves[leaf];
            pcm.resize(frames * 2);
            for (std::uint64_t frame = 0; frame < frames; ++frame) {
                pcm[frame * 2] = float(int((frame * 7 + leaf * 31) % 4093) - 2046) / 1024.F;
                pcm[frame * 2 + 1] = float(int((frame * 13 + leaf * 43) % 4057) - 2028) / 512.F;
            }
            files[leaf] = rawFile(pcm);
        }
        for (std::size_t frame = 0; frame < peer.size() / 2; ++frame) {
            peer[frame * 2] = float(int(frame % 31) - 15) / 32.F;
            peer[frame * 2 + 1] = float(int(frame % 29) - 14) / 64.F;
        }
    }
    std::unique_ptr<PreparedPlayback> playback(bool bank) const
    {
        if (bank) return std::make_unique<PreparedPlayback>(std::array<int, 4>{
            fileno(files[0].get()), fileno(files[1].get()), fileno(files[2].get()), fileno(files[3].get())}, frames);
        return std::make_unique<PreparedPlayback>(fileno(files[0].get()), frames);
    }
    float sample(std::uint64_t frame, std::size_t channel, bool bank, const std::array<float, 4>& gains) const
    {
        if (!bank) return leaves[0][frame * 2 + channel];
        double mixed = 0;
        for (std::size_t leaf = 0; leaf < 4; ++leaf) mixed += double(leaves[leaf][frame * 2 + channel]) * gains[leaf];
        return static_cast<float>(mixed);
    }
};

// Only a real cache worker's first unaligned IN read is held. The ordinary
// readers are already primed at zero and issue block-aligned reads. A claimed
// one-shot gate cannot subsequently block a reader re-prime at the same IN.
struct ReadGateState {
    struct stat identity{};
    std::uint64_t offset = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool claimed = false, entered = false, released = false;
};
std::mutex gateMutex;
std::shared_ptr<ReadGateState> activeGate;
class ReadGate {
public:
    explicit ReadGate(int fd) : state_(std::make_shared<ReadGateState>())
    {
        require(fstat(fd, &state_->identity) == 0, "Cannot identify gated PCM");
        state_->offset = loopIn * 2 * sizeof(float);
        const std::lock_guard lock(gateMutex);
        require(!activeGate, "Nested read gate");
        activeGate = state_;
    }
    ~ReadGate() { release(); }
    void entered()
    {
        std::unique_lock lock(state_->mutex);
        require(state_->changed.wait_for(lock, 5s, [&] { return state_->entered; }),
                "Actual cache pread did not reach the fixture gate");
    }
    void release()
    {
        { const std::lock_guard lock(state_->mutex); state_->released = true; }
        state_->changed.notify_all();
        const std::lock_guard lock(gateMutex);
        if (activeGate == state_) activeGate.reset();
    }
private:
    std::shared_ptr<ReadGateState> state_;
};
void gateRead(int fd, std::uint64_t offset)
{
    std::shared_ptr<ReadGateState> state;
    { const std::lock_guard lock(gateMutex); state = activeGate; }
    if (!state || offset != state->offset) return;
    struct stat actual{};
    if (fstat(fd, &actual) != 0 || actual.st_dev != state->identity.st_dev || actual.st_ino != state->identity.st_ino)
        return;
    std::unique_lock lock(state->mutex);
    if (state->claimed) return;
    state->claimed = state->entered = true;
    state->changed.notify_all();
    state->changed.wait(lock, [&] { return state->released; });
}

void awaitCache(RenderCore& core)
{
    wait([&] { const auto state = core.loopState(0); return state.ready || state.errorCode; },
         "Prepared loop cache did not settle");
    require(core.loopState(0).ready && !core.loopState(0).errorCode, "Prepared loop cache failed");
}
bool sameLoop(const AudioLoopState& a, const AudioLoopState& b)
{
    return a.inFrame == b.inFrame && a.outFrame == b.outFrame && a.limitFrame == b.limitFrame &&
        a.errorCode == b.errorCode && a.supported == b.supported && a.inSet == b.inSet &&
        a.ready == b.ready && a.outSet == b.outSet && a.active == b.active && a.cached == b.cached;
}
void cleared(const RenderCore& core)
{
    const auto loop = core.loopState(0);
    require(loop.supported && !loop.inSet && !loop.ready && !loop.outSet && !loop.active && !loop.cached,
            "Explicit transport action retained loop state or adopted a cancelled completion");
}

struct Oracle {
    RenderCore& core;
    const Signals& signals;
    bool bank;
    std::array<float, 4> gains = initialGains;
    Oracle(RenderCore& renderer, const Signals& data, bool stemBank) : core(renderer), signals(data), bank(stemBank)
    {
        require(core.setDeckGain(0, .5F) && core.setDeckGain(1, .75F) && core.setCrossfader(.75F) &&
                    core.setMasterGain(.5F) && core.setCueSelected(0, true) && core.setCueSelected(1, true) &&
                    core.setCueGain(.25F), "Cannot configure independent loop buses");
        if (bank)
            for (std::size_t leaf = 0; leaf < gains.size(); ++leaf)
                require(core.setStemGain(0, leaf, gains[leaf]), "Cannot configure loop stem targets");
    }
    void controls() const
    {
        require(core.deckGain(0) == .5F && core.deckGain(1) == .75F && core.crossfader() == .75F &&
                    core.masterGain() == .5F && core.cueGain() == .25F && core.isCueSelected(0) && core.isCueSelected(1),
                "Loop action changed independent channel/crossfader/master/cue controls");
        if (bank)
            for (std::size_t leaf = 0; leaf < gains.size(); ++leaf)
                require(core.stemGain(0, leaf) == gains[leaf], "Loop action reset current stem targets");
    }
    void render(std::size_t frames, bool allowLinearShortage = false)
    {
        const auto before = core.sourceFrame(0), peer = core.sourceFrame(1), clock = core.frameClock();
        const auto starved = core.starvedFrames(0);
        const auto loop = core.loopState(0);
        const bool playing = core.isPlaying(0);
        std::vector<float> master(frames * 2), cue(frames * 2);
        require(peer + frames <= signals.peer.size() / 2 && core.renderBuses(master, cue), "Loop bus render failed");
        const auto wanted = playing ? (loop.active ? std::uint64_t(frames) :
            std::min<std::uint64_t>(frames, signals.frames - before)) : 0;
        std::uint64_t delivered = wanted;
        if (allowLinearShortage) {
            require(!loop.active && core.sourceFrame(0) >= before, "Linear shortage path jumped backwards");
            delivered = core.sourceFrame(0) - before;
            require(delivered <= wanted, "Linear reader overdelivered");
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto index = loop.active ? loop.inFrame + (before - loop.inFrame + frame) % (loop.outFrame - loop.inFrame)
                                           : before + frame;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const float raw = frame < delivered ? signals.sample(index, channel, bank, gains) : 0.F;
                const float other = signals.peer[(peer + frame) * 2 + channel];
                const float expectedMaster = static_cast<float>((double(raw) * .25 + double(other) * .75) * .5);
                const float expectedCue = static_cast<float>((double(raw) + other) * .25);
                require(sameBits(master[frame * 2 + channel], expectedMaster) && sameBits(cue[frame * 2 + channel], expectedCue),
                        "Loop master/cue PCM differs from the independent original-index oracle");
            }
        }
        const auto expectedFrame = !playing ? before : loop.active ?
            loop.inFrame + (before - loop.inFrame + frames) % (loop.outFrame - loop.inFrame) : before + delivered;
        require(core.sourceFrame(0) == expectedFrame && core.sourceFrame(1) == peer + frames &&
                    core.frameClock() == clock + frames && !core.readFailed(0) && !core.readFailed(1),
                "Loop/source/peer/output clocks or read health differ");
        require(core.starvedFrames(0) == starved + wanted - delivered && !core.isBuffering(1) &&
                    core.isBuffering(0) == (wanted != delivered), "Loop falsely hid or reported unavailable source frames");
        controls();
    }
    void linearTo(std::uint64_t target)
    {
        const auto deadline = Clock::now() + 5s;
        while (core.sourceFrame(0) < target) {
            require(Clock::now() < deadline, "Linear continuation did not recover within the fixture budget");
            const auto before = core.sourceFrame(0);
            render(std::size_t(std::min<std::uint64_t>(256, target - before)), true);
            if (core.sourceFrame(0) == before) std::this_thread::sleep_for(1ms);
        }
        require(core.sourceFrame(0) == target, "Linear continuation skipped its target");
    }
};

void repeatedWrapsAndExitReloop()
{
    for (const bool bank : {false, true}) {
        for (const bool eofOut : {false, true}) {
            for (const bool partitioned : {false, true}) {
                Signals signals(eofOut ? loopOut : 10000);
                auto owner = signals.playback(bank);
                require(owner->prime(0, 2s), "Cannot prime prepared loop fixture");
                RenderCore core(owner->sourceRef(), AudioSource::fromMemory(signals.peer));
                Oracle oracle(core, signals, bank);
                oracle.linearTo(loopIn);
                const auto oldClock = core.frameClock(), oldPeer = core.sourceFrame(1);
                require(core.apply({Type::LoopIn, 0}), "Render-owned IN rejected");
                const auto captured = core.loopState(0);
                require(captured.inSet && captured.inFrame == loopIn && !captured.outSet &&
                            core.frameClock() == oldClock && core.sourceFrame(1) == oldPeer,
                        "IN did not capture the exact source cursor independently of peer/output");
                awaitCache(core);
                oracle.linearTo(loopOut);
                const auto outClock = core.frameClock(), outPeer = core.sourceFrame(1);
                require(core.apply({Type::LoopOut, 0}) && core.sourceFrame(0) == loopIn &&
                            core.frameClock() == outClock && core.sourceFrame(1) == outPeer && core.isPlaying(0),
                        "OUT did not rewind to IN without resetting the continuous clocks");
                require(core.loopState(0).active && core.loopState(0).cached && core.loopState(0).outFrame == loopOut,
                        "OUT did not retain its exact half-open bounds");
                const auto length = loopOut - loopIn;
                std::size_t remaining = std::size_t(length * 3 + 37), part = 0;
                constexpr std::array<std::size_t, 5> partitions{1, 256, 1024, 511, 4099};
                while (remaining) {
                    const auto count = partitioned ? std::min(remaining, partitions[part++ % partitions.size()]) : remaining;
                    oracle.render(count);
                    remaining -= count;
                }
                // The current targets, not an old pre-mixed cache, affect subsequent repetitions.
                if (bank) {
                    oracle.gains = {.5F, 0.F, 1.F, .25F};
                    for (std::size_t leaf = 0; leaf < 4; ++leaf)
                        require(core.setStemGain(0, leaf, oracle.gains[leaf]), "Cannot change cached-loop stem target");
                    oracle.render(23);
                }
                const auto paused = core.sourceFrame(0);
                require(core.setPlaying(0, false), "Cannot pause active loop");
                oracle.render(7);
                require(core.loopState(0).active && core.loopState(0).outSet && core.sourceFrame(0) == paused &&
                            core.setPlaying(0, true), "Pause discarded loop bounds or advanced its source cursor");

                auto beforeExit = core.sourceFrame(0);
                require(core.apply({Type::LoopToggle, 0}) && !core.loopState(0).active && core.loopState(0).cached &&
                            core.sourceFrame(0) == beforeExit, "EXIT jumped position or discarded its cached remainder");
                oracle.render(11);
                require(core.loopToggle(0) && core.loopState(0).active && core.sourceFrame(0) == loopIn,
                        "RELOOP before the cached remainder ended did not return to IN");
                oracle.render(41);
                beforeExit = core.sourceFrame(0);
                require(core.loopToggle(0) && core.sourceFrame(0) == beforeExit, "Second EXIT jumped position");
                const auto remainder = loopOut - beforeExit;
                if (eofOut) {
                    oracle.render(std::size_t(remainder + 19));
                    require(core.sourceFrame(0) == signals.frames && !core.isBuffering(0),
                            "OUT at EOF produced starvation instead of known-end silence");
                } else {
                    oracle.render(std::size_t(remainder + 23));
                    require(!core.loopState(0).cached && core.sourceFrame(0) == loopOut + 23,
                            "EXIT did not continue from the parked OUT position");
                    require(core.loopToggle(0) && core.loopState(0).active && core.sourceFrame(0) == loopIn,
                            "RELOOP after linear progress did not restore IN");
                    oracle.render(31);
                    beforeExit = core.sourceFrame(0);
                    require(core.loopToggle(0) && core.sourceFrame(0) == beforeExit, "Post-linear EXIT jumped position");
                    oracle.render(std::size_t(loopOut - beforeExit));
                    // Re-priming parked readers is asynchronous. Every available
                    // prefix remains exact; peer/output clocks continue during a shortage.
                    oracle.linearTo(loopOut + 47);
                    require(!core.loopState(0).cached, "Second EXIT failed to return to linear reading");
                }
                oracle.controls();
            }
        }
    }
}

void pointHotCueExitsLoopWithoutStoringLoopAsPoint()
{
    for (const bool bank : {false, true}) {
        Signals signals;
        auto owner = signals.playback(bank);
        require(owner->prime(0, 2s), "Cannot prime hot-cue/loop fixture");
        RenderCore core(owner->sourceRef(), AudioSource::fromMemory(signals.peer));
        Oracle oracle(core, signals, bank);
        require(core.hotCue(0, 0), "Cannot capture prepared hot cue");
        oracle.linearTo(loopIn);
        require(core.loopIn(0), "Cannot capture loop IN");
        awaitCache(core);
        oracle.linearTo(loopIn + 256);
        require(core.loopOut(0), "Cannot capture loop OUT");
        const auto before = core.loopState(0);
        const auto points = core.hotCues(0);
        require(!core.hotCue(0, 1) && core.hotCues(0) == points && sameLoop(before, core.loopState(0)),
                "Active loop was silently stored as a point or changed by rejection");
        const auto peer = core.sourceFrame(1), clock = core.frameClock();
        require(core.hotCue(0, 0) && core.sourceFrame(0) == 0 && core.isPlaying(0) &&
                core.sourceFrame(1) == peer && core.frameClock() == clock,
                "Point recall failed to leave the loop independently of the other deck");
        cleared(core);
        oracle.controls();
        oracle.linearTo(32);
    }
}

void minimumAndRejectedOutPreserveTransport()
{
    Signals signals;
    auto owner = signals.playback(false);
    require(owner->prime(0, 2s), "Cannot prime OUT rejection fixture");
    RenderCore core(owner->sourceRef(), AudioSource::fromMemory(signals.peer));
    Oracle oracle(core, signals, false);
    require(!core.loopOut(0) && !core.loopToggle(0), "OUT/RELOOP without IN was accepted");
    oracle.linearTo(loopIn);
    ReadGate gate(fileno(signals.files[0].get()));
    require(core.loopIn(0), "Pending IN rejected");
    gate.entered();
    require(!core.loopState(0).ready, "Blocked cache was reported ready");
    oracle.linearTo(loopIn + 256);
    const auto pending = core.loopState(0);
    const auto frame = core.sourceFrame(0), clock = core.frameClock();
    require(!core.loopOut(0) && sameLoop(pending, core.loopState(0)) && core.sourceFrame(0) == frame &&
                core.frameClock() == clock && core.isPlaying(0), "OUT before cache readiness mutated transport or loop state");
    gate.release(); awaitCache(core);
    const auto source = owner->sourceRef();
    const auto ready = core.loopState(0);
    for (const auto invalid : {std::uint64_t(0), loopIn, loopIn + 255, ready.limitFrame + 1})
        require(!source.loopControl(source.context, AudioLoopCommand::Out, invalid) && sameLoop(ready, core.loopState(0)) &&
                    core.sourceFrame(0) == frame && core.frameClock() == clock && core.isPlaying(0),
                "Invalid OUT changed the ready request, playing state or clocks");
    require(core.setPlaying(0, false) && !core.loopOut(0) && sameLoop(ready, core.loopState(0)) &&
                core.setPlaying(0, true), "Paused OUT was accepted or changed loop bounds");
    require(core.loopOut(0) && core.sourceFrame(0) == loopIn && core.loopState(0).outFrame - loopIn == 256,
            "Exact 256-frame minimum loop was rejected");
    oracle.render(256 * 3 + 11);
}

void optionalCacheFailurePreservesHealthyLinearPlayback()
{
    constexpr std::uint64_t sourceFrames = 60U * 48000U, badFrame = 200000;
    Signals signals(1024); // Keep only the exercised, exact finite prefix in memory.
    signals.frames = sourceFrames;
    File sparse(std::tmpfile());
    require(bool(sparse), "Cannot create sparse cache-failure fixture");
    const int fd = fileno(sparse.get());
    const auto prefixBytes = signals.leaves[0].size() * sizeof(float);
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    // Populate before either reader captures its identity. The distant NaN is
    // beyond the ordinary 32x4096-frame ring, but inside the requested cache.
    require(ftruncate(fd, off_t(sourceFrames * 2U * sizeof(float))) == 0 &&
                pwrite(fd, signals.leaves[0].data(), prefixBytes, 0) == ssize_t(prefixBytes) &&
                pwrite(fd, &invalid, sizeof(invalid), off_t(badFrame * 2U * sizeof(float))) == ssize_t(sizeof(invalid)),
            "Cannot populate sparse cache-failure fixture");
    auto owner = std::make_unique<PreparedPlayback>(fd, sourceFrames);
    require(owner->prime(0, 2s), "Cannot prime healthy prefix before optional cache failure");
    RenderCore core(owner->sourceRef(), AudioSource::fromMemory(signals.peer));
    Oracle oracle(core, signals, false);
    oracle.linearTo(loopIn);
    require(core.loopIn(0), "Cache-failure IN rejected");
    wait([&] { return core.loopState(0).errorCode != 0; }, "Distant nonfinite cache input was not rejected");
    const auto failed = core.loopState(0);
    const auto frame = core.sourceFrame(0), peer = core.sourceFrame(1), clock = core.frameClock();
    require(failed.supported && failed.inSet && failed.inFrame == loopIn && failed.limitFrame == sourceFrames &&
                failed.errorCode == EILSEQ &&
                !failed.ready && !failed.outSet && !failed.active && !failed.cached && !failed.residentBytes &&
                !owner->errorCode() && !core.readFailed(0) && !core.isBuffering(0),
            "Optional nonfinite cache failure contaminated healthy prepared playback");
    require(!core.loopOut(0) && sameLoop(failed, core.loopState(0)) && core.sourceFrame(0) == frame &&
                core.sourceFrame(1) == peer && core.frameClock() == clock && core.isPlaying(0),
            "OUT accepted a failed cache or changed healthy linear transport");
    oracle.render(257);
    require(!owner->errorCode() && core.loopState(0).errorCode == EILSEQ && !core.loopState(0).active,
            "Healthy exact linear rendering hid the cache error or became a playback failure");
}

enum class ClearAction { Seek, Jog, Cue, Retry };
void sameFrameTransportInvalidatesPendingAndActiveLoops()
{
    for (const auto action : {ClearAction::Seek, ClearAction::Jog, ClearAction::Cue, ClearAction::Retry}) {
        Signals signals;
        auto owner = signals.playback(false);
        require(owner->prime(0, 2s), "Cannot prime cancellation fixture");
        RenderCore core(owner->sourceRef(), AudioSource::fromMemory(signals.peer));
        Oracle oracle(core, signals, false);
        const auto clear = [&] {
            const auto frame = core.sourceFrame(0), peer = core.sourceFrame(1), clock = core.frameClock();
            bool accepted = false;
            switch (action) {
            case ClearAction::Seek: accepted = core.seek(0, frame); break;
            case ClearAction::Jog: accepted = core.jog(0, 0); break;
            case ClearAction::Cue:
                require(core.setCueFrame(0, frame), "Cannot set same-frame CUE fixture");
                accepted = core.cuePress(0);
                require(core.cueRelease(0) && core.setPlaying(0, true), "Cannot release same-frame CUE");
                break;
            case ClearAction::Retry: accepted = core.retrySource(0); break;
            }
            require(accepted && core.sourceFrame(0) == frame && core.sourceFrame(1) == peer && core.frameClock() == clock,
                    "Same-frame loop invalidation moved a cursor or clock");
            cleared(core); oracle.controls();
        };
        oracle.linearTo(loopIn);
        {
            ReadGate gate(fileno(signals.files[0].get()));
            require(core.loopIn(0), "Pending cancellation IN rejected");
            gate.entered();
            clear();
            gate.release();
            wait([&] { return core.loopState(0).residentBytes == 0; }, "Cancelled in-flight cache was not retired");
            cleared(core);
        }
        require(core.loopIn(0), "Fresh IN after cancelled completion rejected");
        awaitCache(core);
        oracle.linearTo(loopIn + 256);
        require(core.loopOut(0), "Active cancellation OUT rejected");
        oracle.render(13);
        const auto linearStart = core.sourceFrame(0);
        clear();
        oracle.linearTo(linearStart + 29);
        cleared(core);
    }
}

struct PartialReader {
    std::span<const float> pcm;
    std::uint64_t cursor = 0;
    std::size_t calls = 0, limit = std::numeric_limits<std::size_t>::max(), badPosition = 0;
    bool failed = false;
    AudioSource source() noexcept { return {pcm.size() / 2, nullptr, this, read, seek}; }
    static AudioReadResult read(void* context, std::uint64_t frame, std::span<float> output) noexcept
    {
        auto& self = *static_cast<PartialReader*>(context);
        ++self.calls;
        if (frame != self.cursor || frame > self.pcm.size() / 2) { ++self.badPosition; return {0, true}; }
        const auto count = std::min({self.limit, output.size() / 2, self.pcm.size() / 2 - std::size_t(frame)});
        std::copy_n(self.pcm.data() + frame * 2, count * 2, output.data());
        self.cursor += count;
        return {count, self.failed};
    }
    static void seek(void* context, std::uint64_t frame) noexcept
    { static_cast<PartialReader*>(context)->cursor = frame; }
};
void cachedBankSharesRampsAndParksRawSurplus()
{
    Signals signals(64);
    std::array<PartialReader, 4> readers;
    std::array<AudioSource, 4> sources{};
    std::array<std::span<const float>, 4> cached{};
    for (std::size_t leaf = 0; leaf < 4; ++leaf) {
        readers[leaf].pcm = cached[leaf] = signals.leaves[leaf];
        sources[leaf] = readers[leaf].source();
    }
    readers[0].limit = 2;
    StemBankSource bank(sources);
    require(bank.configureRamps(4) && bank.setGain(0, 0.F), "Cannot configure partial-bank ramp");
    const auto source = bank.sourceRef();
    std::array<float, 16> output{};
    output.fill(91.F);
    auto result = source.read(source.context, 0, output);
    require(result.frames == 2 && !result.failed && bank.sourceFrame() == 2, "Partial bank did not park surplus");
    const auto check = [&](std::uint64_t first, std::span<const float> gains) {
        for (std::size_t frame = 0; frame < gains.size(); ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(sameBits(output[frame * 2 + channel],
                                 signals.sample(first + frame, channel, true, {gains[frame], 1.F, 1.F, 1.F})),
                        "Cached/linear bank did not share the exact gain-ramp progression");
    };
    check(0, std::array<float, 2>{.75F, .5F});
    require(std::ranges::all_of(std::span(output).subspan(4), [](float sample) { return sample == 91.F; }),
            "Partial linear read overwrote its unavailable suffix");
    const std::array calls{readers[0].calls, readers[1].calls, readers[2].calls, readers[3].calls};
    const std::array cursors{readers[0].cursor, readers[1].cursor, readers[2].cursor, readers[3].cursor};
    result = bank.readCached(cached, 17, std::span(output).first(4));
    require(result.frames == 2 && !result.failed && bank.sourceFrame() == 2, "Cached read moved the parked bank cursor");
    check(17, std::array<float, 2>{.25F, 0.F});
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(readers[leaf].calls == calls[leaf] && readers[leaf].cursor == cursors[leaf],
                "Cached read consumed/refetched a parked provider or discarded surplus");
    require(bank.setGain(0, .5F), "Cannot retarget cached bank gain");
    readers[0].limit = std::numeric_limits<std::size_t>::max();
    result = source.read(source.context, 2, std::span(output).first(8));
    require(result.frames == 4 && !result.failed && bank.sourceFrame() == 6, "Linear return lost its parked prefix");
    check(2, std::array<float, 4>{.125F, .25F, .375F, .5F});
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(!readers[leaf].badPosition && (leaf == 0 || readers[leaf].calls == calls[leaf]),
                "Linear return reread faster providers instead of consuming parked surplus");
    readers[0].limit = 1; readers[0].failed = true;
    result = source.read(source.context, 6, std::span(output).first(4));
    require(result.failed && bank.hasReadFailure(), "Parked provider failure was not latched");
    output.fill(91.F);
    result = bank.readCached(cached, 17, std::span(output).first(4));
    require(result.failed && !result.frames && std::ranges::all_of(output, [](float sample) { return sample == 91.F; }),
            "Cached path hid a parked raw failure or wrote a stale mix");
}

void unsupportedSourcesAndInvalidDecks()
{
    Signals signals(512);
    RenderCore memory(signals.leaves[0]);
    require(!memory.loopIn(0) && !memory.loopOut(0) && !memory.loopToggle(0) && !memory.loopState(0).supported,
            "Plain memory source unexpectedly advertised prepared looping");
    auto live = PreparedPlayback::makeLive(37, 512, [](std::stop_token stop, auto&) {
        while (!stop.stop_requested()) std::this_thread::sleep_for(1ms);
    });
    RenderCore core(live->sourceRef());
    require(!core.loopIn(0) && !core.loopOut(0) && !core.loopToggle(0) && !core.loopState(0).supported &&
                !core.loopIn(2) && !core.loopOut(2) && !core.loopToggle(2) && !core.loopState(2).supported &&
                !core.apply({Type::LoopIn, 2}) && !core.apply({Type::LoopOut, 2}) && !core.apply({Type::LoopToggle, 2}) &&
                core.sourceFrame(0) == 0 && core.frameClock() == 0,
            "Live/invalid-deck loop command was accepted or changed transport");
    live->stop();
}
} // namespace

// Keep the actual glibc ABIs, including large-file/fortified redirections.
// This executable alone interposes reads; production has no test hook.
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" ssize_t __real___pread_chk(int, void*, size_t, off_t, size_t);
extern "C" ssize_t __real___pread64_chk(int, void*, size_t, off64_t, size_t);
extern "C" ssize_t __wrap_pread(int fd, void* data, size_t count, off_t offset)
{ gateRead(fd, std::uint64_t(offset)); return __real_pread(fd, data, count, offset); }
extern "C" ssize_t __wrap_pread64(int fd, void* data, size_t count, off64_t offset)
{ gateRead(fd, std::uint64_t(offset)); return __real_pread64(fd, data, count, offset); }
extern "C" ssize_t __wrap___pread_chk(int fd, void* data, size_t count, off_t offset, size_t capacity)
{ gateRead(fd, std::uint64_t(offset)); return __real___pread_chk(fd, data, count, offset, capacity); }
extern "C" ssize_t __wrap___pread64_chk(int fd, void* data, size_t count, off64_t offset, size_t capacity)
{ gateRead(fd, std::uint64_t(offset)); return __real___pread64_chk(fd, data, count, offset, capacity); }

int main()
{
    try {
        repeatedWrapsAndExitReloop();
        pointHotCueExitsLoopWithoutStoringLoopAsPoint();
        minimumAndRejectedOutPreserveTransport();
        optionalCacheFailurePreservesHealthyLinearPlayback();
        sameFrameTransportInvalidatesPendingAndActiveLoops();
        cachedBankSharesRampsAndParksRawSurplus();
        unsupportedSourcesAndInvalidDecks();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

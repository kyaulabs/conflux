// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/PreparedTempoSource.hpp"
#include "audio/RenderCore.hpp"
#include "audio/TempoProcessor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
using namespace conflux::audio;
using namespace std::chrono_literals;
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
thread_local bool callback = false;
thread_local std::size_t callbackReads = 0, callbackAllocations = 0;
struct CallbackScope
{
    CallbackScope() { callback = true; }
    ~CallbackScope() { callback = false; }
};
template <class Predicate> void wait(Predicate &&predicate, const char *message)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate())
    {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
struct CloseFile
{
    void operator()(std::FILE *file) const
    {
        if (file)
            std::fclose(file);
    }
};
using File = std::unique_ptr<std::FILE, CloseFile>;
struct Signals
{
    static constexpr std::uint64_t frames = 48000;
    std::array<std::vector<float>, 4> pcm;
    std::array<File, 4> files;
    Signals()
    {
        for (std::size_t leaf = 0; leaf < 4; ++leaf)
        {
            pcm[leaf].resize(frames * 2);
            for (std::uint64_t frame = 0; frame < frames; ++frame)
            {
                pcm[leaf][frame * 2] = float(.1 * std::sin(2 * std::numbers::pi * (375 + leaf * 50) * frame / 48000.));
                pcm[leaf][frame * 2 + 1] = -.5F * pcm[leaf][frame * 2];
            }
            files[leaf].reset(std::tmpfile());
            require(files[leaf] &&
                        std::fwrite(pcm[leaf].data(), sizeof(float), pcm[leaf].size(), files[leaf].get()) ==
                            pcm[leaf].size() &&
                        std::fflush(files[leaf].get()) == 0,
                    "Cannot write generated loop/tempo fixture");
        }
    }
    std::array<int, 4> fds() const
    {
        return {fileno(files[0].get()), fileno(files[1].get()), fileno(files[2].get()), fileno(files[3].get())};
    }
    std::unique_ptr<PreparedPlayback> playback(bool bank) const
    {
        return bank ? std::make_unique<PreparedPlayback>(fds(), frames)
                    : std::make_unique<PreparedPlayback>(fds()[0], frames);
    }
};
PreparedTempoSource::View next(PreparedTempoSource &source)
{
    PreparedTempoSource::View result;
    wait(
        [&]
        {
            result = source.peek();
            return bool(result);
        },
        "Cyclic worker did not publish");
    require(result.frames && !result.terminal && !result.errorCode, "Cyclic worker failed or delivered EOF");
    return result;
}
bool same(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }

void unityWrapsPartialConsumptionAndRangeRejection()
{
    Signals signals;
    constexpr std::uint64_t in = 17, out = in + AudioLoopState::minimumFrames;
    for (const std::size_t leaves : {1U, 4U})
    {
        auto fds = signals.fds();
        PreparedTempoSource source{std::span(fds).first(leaves), Signals::frames};
        require(source.request(out - 1, 0, true, in, out), "Valid minimum loop rejected");
        const auto generation = source.generation();
        next(source);
        const auto paused = source.peek();
        std::this_thread::sleep_for(5ms);
        require(source.nominalPosition() == double(out - 1) && source.loopWraps() == 0 &&
                    source.peek().frames == paused.frames,
                "Loop prefetch advanced consumed position");
        for (const auto range : {std::array<std::uint64_t, 2>{in, 0},
                                 {in, in},
                                 {in, out - 1},
                                 {out, in},
                                 {in, Signals::frames + 1},
                                 {out, out + 256}})
            require(!source.request(in, 0, true, range[0], range[1]) && source.generation() == generation &&
                        source.nominalPosition() == double(out - 1),
                    "Invalid loop range changed current stream");
        std::uint64_t consumed = 0;
        for (std::size_t part = 0; consumed < 12000; ++part)
        {
            const auto view = next(source);
            constexpr std::array<std::size_t, 4> portions{1, 17, 257, 1024};
            const auto count = std::min(view.frames, portions[part % portions.size()]);
            for (std::size_t leaf = 0; leaf < leaves; ++leaf)
                for (std::size_t frame = 0; frame < count; ++frame)
                    for (std::size_t channel = 0; channel < 2; ++channel)
                        require(
                            same(
                                view.leaves[leaf][frame * 2 + channel],
                                signals.pcm[leaf][(in + (out - 1 - in + consumed + frame) % (out - in)) * 2 + channel]),
                            "Cyclic unity changed PCM, leaf/channel order or range bounds");
            require(!source.consume(view.frames + 1) && source.consume(count),
                    "Loop over-consume or valid prefix handling failed");
            consumed += count;
            require(source.nominalPosition() == double(in + (out - 1 - in + consumed) % (out - in)) &&
                        source.loopWraps() == (out - 1 - in + consumed) / (out - in) &&
                        source.generation() == generation && !source.status().finished,
                    "Partial cyclic consumption lost wraps or reset the DSP generation");
        }
        require(source.request(Signals::frames, 0, false) && source.loopWraps() == 0,
                "Linear request retained old loop wraps");
        PreparedTempoSource::View eof;
        wait(
            [&]
            {
                eof = source.peek();
                return bool(eof);
            },
            "Linear EOF missing after loop invalidation");
        require(eof.terminal && !eof.frames && !eof.errorCode && source.consume(0) && source.status().finished,
                "Linear EOF was replaced by a stale cyclic packet");
    }
}

// Independently feed one continuous DSP stream from repeating source ranges.
// The reference retains backend state across every input boundary; restarting
// a filter at each natural loop wrap will produce different samples.
std::vector<float> reference(const Signals &signals, std::size_t leaves, double percent, bool lock, std::uint64_t in,
                             std::uint64_t out, std::uint64_t start, std::size_t wanted)
{
    const auto channels = leaves * 2;
    TempoProcessor processor({channels, percent, lock});
    std::array<float, 1024 * 8> input{}, output{};
    std::size_t inputCount = 0, inputOffset = 0;
    auto cursor = start;
    std::vector<float> result;
    for (std::size_t call = 0; result.size() / channels < wanted; ++call)
    {
        require(call < 10000, "Continuous cyclic reference made no progress");
        if (inputCount == inputOffset)
        {
            if (cursor == out)
                cursor = in;
            inputCount = std::min<std::uint64_t>(1024, out - cursor);
            inputOffset = 0;
            for (std::size_t frame = 0; frame < inputCount; ++frame)
                for (std::size_t leaf = 0; leaf < leaves; ++leaf)
                    for (std::size_t channel = 0; channel < 2; ++channel)
                        input[frame * channels + leaf * 2 + channel] =
                            signals.pcm[leaf][(cursor + frame) * 2 + channel];
            cursor += inputCount;
        }
        const auto step =
            processor.process(processor.generation(),
                              std::span(input).subspan(inputOffset * channels, (inputCount - inputOffset) * channels),
                              std::span(output).first(1024 * channels));
        inputOffset += step.inputFramesUsed;
        const auto count = std::min(step.outputFrames, wanted - result.size() / channels);
        result.insert(result.end(), output.begin(), output.begin() + count * channels);
    }
    return result;
}
void continuousSrcAndMasterTempo()
{
    Signals signals;
    constexpr std::uint64_t in = 31, out = in + 4097, start = in + 19;
    constexpr std::size_t wanted = 24593;
    for (const std::size_t leaves : {1U, 4U})
        for (const bool lock : {false, true})
            for (const double percent : {-6., 6.})
            {
                const auto expected = reference(signals, leaves, percent, lock, in, out, start, wanted);
                const auto fds = signals.fds();
                PreparedTempoSource source{std::span(fds).first(leaves), Signals::frames};
                require(source.request(start, percent, lock, in, out), "Processed loop request rejected");
                std::size_t consumed = 0;
                while (consumed < wanted)
                {
                    const auto view = next(source);
                    const auto count = std::min({view.frames, std::size_t(257), wanted - consumed});
                    for (std::size_t frame = 0; frame < count; ++frame)
                        for (std::size_t leaf = 0; leaf < leaves; ++leaf)
                            for (std::size_t channel = 0; channel < 2; ++channel)
                                require(
                                    same(view.leaves[leaf][frame * 2 + channel],
                                         expected[(consumed + frame) * leaves * 2 + leaf * 2 + channel]),
                                    "Processed loop reset SRC/R3 state, lost a frame or changed stereo/stem alignment");
                    require(source.consume(count), "Processed cyclic prefix rejected");
                    consumed += count;
                    const long double distance =
                        start - in +
                        static_cast<long double>(consumed) * (1.L + static_cast<long double>(percent) / 100.L);
                    const double wantedPosition = double(in + std::fmod(distance, static_cast<long double>(out - in)));
                    require(std::abs(source.nominalPosition() - wantedPosition) < 1e-6 &&
                                source.loopWraps() == std::uint64_t(std::floor(distance / (out - in))) &&
                                !source.status().finished,
                            "Processed loop cursor/wrap count differs from consumed fractional progress");
                }
            }
}

void combinedControlsAndPeerClock()
{
    Signals signals;
    const std::vector<float> peer(1000000 * 2, .03125F);
    for (const bool bank : {false, true})
        for (const bool lock : {false, true})
            for (const bool tempoFirst : {false, true})
            {
                auto owner = signals.playback(bank);
                require(owner->prime(17, 2s), "Cannot prime combined-controls source");
                RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
                require(core.restorePrimedCursor(0, 17) && core.configureRamps(0),
                        "Cannot configure combined-controls cursor");
                auto render = [&](std::size_t frames = 257)
                {
                    std::array<float, 2048> output{};
                    const auto peerBefore = core.sourceFrame(1), clock = core.frameClock();
                    {
                        CallbackScope scope;
                        require(core.render(std::span(output).first(frames * 2)), "Combined render failed");
                    }
                    require(!core.readFailed(0) && core.sourceFrame(1) == peerBefore + frames &&
                                core.frameClock() == clock + frames,
                            "Loop/tempo disturbed source health, peer playback or output clock");
                    if (core.isBuffering(0))
                        std::this_thread::sleep_for(1ms);
                };
                if (lock)
                    require(core.toggleMasterTempo(0), "Master Tempo selection failed");
                if (tempoFirst)
                    require(core.setTempoPercent(0, 6), "Initial tempo failed");
                require(core.loopIn(0), "IN rejected during prepared tempo");
                wait([&] { return core.loopState(0).ready; }, "Combined IN cache never became ready");
                wait(
                    [&]
                    {
                        if (core.sourceFrame(0) >= 4114)
                            return true;
                        render();
                        return false;
                    },
                    "Combined source never reached OUT");
                require(core.loopOut(0), "OUT rejected during prepared tempo");
                if (!tempoFirst)
                    require(core.setTempoPercent(0, 6), "Tempo rejected during active prepared loop");
                const auto bounds = core.loopState(0);
                const auto ref = owner->sourceRef();
                require(!ref.scratchControl(ref.context, AudioScratchCommand::Begin, 0, 0, core.sourceFrame(0)) &&
                            !ref.scratchState(ref.context).active,
                        "Direct scratch takeover bypassed the stored-loop restriction");
                require(bounds.active && !bounds.cached && core.tempoState(0).active,
                        "Combined controls did not use processed loop");
                bool fractional = false;
                for (std::size_t block = 0; block < 80; ++block)
                {
                    render();
                    const auto position = core.sourcePosition(0);
                    require(position >= double(bounds.inFrame) && position < double(bounds.outFrame),
                            "Processed loop escaped its bounds");
                    fractional |= position != std::floor(position);
                }
                require(fractional, "RenderCore discarded processed loop's fractional cursor");
                const auto paused = core.sourcePosition(0);
                const auto generation = core.tempoState(0).generation;
                require(core.setPlaying(0, false), "Cannot pause processed loop");
                render();
                require(core.sourcePosition(0) == paused && core.loopState(0).active &&
                            core.tempoState(0).generation == generation && core.setPlaying(0, true),
                        "Pause consumed/reset the processed loop");
                require(core.setTempoPercent(0, -6) && core.tempoState(0).generation == generation,
                        "Loop speed change reset DSP");
                wait(
                    [&]
                    {
                        render();
                        return core.tempoState(0).appliedPercent == -6;
                    },
                    "Loop speed revision never reached consumed output");
                require(core.setTempoPercent(0, 0) && core.tempoState(0).generation == generation,
                        "Loop zero crossing reset DSP");
                wait(
                    [&]
                    {
                        render();
                        return core.tempoState(0).appliedPercent == 0;
                    },
                    "Loop zero crossing never applied");
                require(core.toggleMasterTempo(0) && core.loopState(0).active &&
                            core.tempoState(0).generation > generation,
                        "Loop key-lock rebuild lost the range or generation");
                render();
                require(core.resetTempo(0) && !core.tempoState(0).active && core.loopState(0).cached &&
                            core.loopState(0).active,
                        "Reset failed to return to the raw unity loop");
                for (int i = 0; i < 20; ++i)
                    render();
                const auto exit = core.sourceFrame(0);
                require(core.loopToggle(0) && !core.loopState(0).active && core.sourceFrame(0) == exit,
                        "Raw EXIT jumped position");
                wait(
                    [&]
                    {
                        render();
                        return core.sourceFrame(0) > bounds.outFrame + 17;
                    },
                    "Raw EXIT did not resume the parked linear reader");
                require(core.loopToggle(0) && core.sourceFrame(0) == bounds.inFrame && core.loopState(0).active,
                        "Raw RELOOP failed");
                require(core.setTempoPercent(0, 6), "Cannot reenter processed loop after Reset/EXIT/RELOOP");
                render();
                const auto beforeExit = core.sourceFrame(0), beforeExitGeneration = core.tempoState(0).generation;
                require(core.loopToggle(0) && !core.loopState(0).active && core.sourceFrame(0) == beforeExit &&
                            core.tempoState(0).generation > beforeExitGeneration,
                        "Processed EXIT failed to invalidate queued cyclic PCM");
                wait(
                    [&]
                    {
                        render();
                        return core.sourceFrame(0) > bounds.outFrame + 17;
                    },
                    "Processed EXIT kept repeating old loop PCM");
                require(core.loopToggle(0) && core.sourceFrame(0) == bounds.inFrame,
                        "Processed RELOOP did not return to IN");
                render();
                const auto beforeSeek = core.tempoState(0).generation;
                require(core.seek(0, 10000) && !core.loopState(0).inSet && core.tempoState(0).generation > beforeSeek,
                        "Seek did not invalidate processed loop bounds and packets");
                wait(
                    [&]
                    {
                        render();
                        return core.sourceFrame(0) > 10000;
                    },
                    "Seek retained cyclic input or failed to resume linear tempo");
            }
    require(!callbackReads && !callbackAllocations, "Loop/tempo render callback performed I/O/allocation");
}

void currentStemGainsAcrossProcessedWrapsAndReset()
{
    Signals signals;
    auto owner = signals.playback(true);
    require(owner->prime(17, 2s) && owner->configureStemRamps(4), "Cannot prime loop stem-gain fixture");
    auto source = owner->sourceRef();
    require(source.loopControl(source.context, AudioLoopCommand::In, 17), "Cannot set gain fixture IN");
    wait([&] { return source.loopState(source.context).ready; }, "Gain fixture cache never became ready");
    require(source.loopControl(source.context, AudioLoopCommand::Out, 273) &&
                source.tempoControl(source.context, AudioTempoCommand::Set, 6, false, 17) &&
                source.tempoControl(source.context, AudioTempoCommand::Set, 0, true, 17),
            "Cannot establish processed exact-unity loop for gain oracle");
    // Rebuilding key-lock at zero creates a fresh exact-unity backend. Queue
    // fill precedes gain changes, so this also detects producer-side mixing.
    std::this_thread::sleep_for(10ms);
    constexpr std::array<float, 4> gains{0.F, .5F, .25F, 1.F};
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(source.setStemGain(source.context, leaf, gains[leaf]), "Cannot retarget looping stem");
    std::size_t consumed = 0;
    std::uint64_t cursor = 17;
    while (consumed < 2300)
    {
        std::array<float, 2050> output{};
        AudioReadResult result;
        wait(
            [&]
            {
                {
                    CallbackScope scope;
                    result = source.read(source.context, cursor, std::span(output).first(514));
                }
                require(!result.failed && result.positionValid && !result.finished, "Processed gain read failed");
                return result.frames != 0;
            },
            "Processed gain read never became ready");
        for (std::size_t frame = 0; frame < result.frames; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
            {
                const double fraction = std::min(1., double(consumed + frame + 1) / 4.);
                double mixed = 0;
                for (std::size_t leaf = 0; leaf < 4; ++leaf)
                    mixed += double(signals.pcm[leaf][(17 + (consumed + frame) % 256) * 2 + channel]) *
                             ((1 - fraction) + fraction * gains[leaf]);
                require(same(output[frame * 2 + channel], float(mixed)),
                        "Loop pre-mixed stale stem gains or reset the shared gain ramp");
            }
        consumed += result.frames;
        cursor = 17 + consumed % 256;
        require(result.nominalPosition == double(cursor), "Processed gain oracle cursor differs");
    }
    require(source.tempoControl(source.context, AudioTempoCommand::Reset, 0, true, cursor),
            "Gain fixture reset rejected");
    std::array<float, 32> raw{};
    const auto result = source.read(source.context, cursor, raw);
    require(result.frames == std::min<std::uint64_t>(16, 273 - cursor) && !result.failed && !result.positionValid,
            "Reset lost resident loop PCM");
    for (std::size_t frame = 0; frame < result.frames; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            double mixed = 0;
            for (std::size_t leaf = 0; leaf < 4; ++leaf)
                mixed += double(signals.pcm[leaf][(cursor + frame) * 2 + channel]) * gains[leaf];
            require(same(raw[frame * 2 + channel], float(mixed)),
                    "Reset changed selected stem gains or resumed stale linear PCM");
        }
}

void cyclicSourceMutationAndCancellation()
{
    Signals signals;
    auto fds = signals.fds();
    PreparedTempoSource source{fds, Signals::frames};
    require(source.request(17, 6, true, 17, 4114), "Cannot start stale-loop fixture");
    next(source);
    const float changed = .875F;
    require(pwrite(fds[3], &changed, sizeof(changed), off_t(4200 * 2 * sizeof(float))) == sizeof(changed),
            "Cannot mutate generated loop source");
    wait(
        [&]
        {
            if (source.errorCode())
                return true;
            const auto view = source.peek();
            if (view)
                require(source.consume(view.frames), "Cannot drain pre-mutation queued prefix");
            return false;
        },
        "Cyclic worker did not reject changed source identity");
    require(source.errorCode() == ESTALE && !source.status().finished,
            "Cyclic source change was hidden or treated as EOF");
    const auto generation = source.generation();
    require(source.cancel() && source.generation() > generation && !source.peek() && !source.status().active &&
                !source.loopWraps(),
            "Cancellation exposed stale cyclic storage, activity or wrap counters");
    source.stop();
    require(!source.request(17, 0, false, 17, 4114), "Stopped cyclic worker accepted work");
}

void malformedLoopMetadataFailsClosed()
{
    struct Source
    {
        AudioTempoState tempo{6, 6, 1, 0, true, true};
        AudioLoopState loop{};
        AudioReadResult result{};
        Source()
        {
            loop.supported = loop.ready = loop.inSet = loop.outSet = loop.active = true;
            loop.outFrame = 256;
        }
        AudioSource ref()
        {
            AudioSource source{10000, nullptr, this, [](void *p, std::uint64_t, std::span<float> output) noexcept
                               {
                                   const auto &self = *static_cast<Source *>(p);
                                   std::fill(output.begin(), output.end(), .125F);
                                   return self.result;
                               }};
            source.tempoControl = [](void *, AudioTempoCommand, double, bool, std::uint64_t) noexcept { return true; };
            source.tempoState = [](const void *p) noexcept { return static_cast<const Source *>(p)->tempo; };
            source.loopControl = [](void *, AudioLoopCommand, std::uint64_t) noexcept { return true; };
            source.loopState = [](const void *p) noexcept { return static_cast<const Source *>(p)->loop; };
            return source;
        }
    };
    for (const std::size_t frames : {64U, 1024U})
    {
        Source valid;
        const double distance = 250. + frames * 1.06;
        valid.result = {frames, false, true, std::fmod(distance, 256.)};
        valid.result.loopWraps = std::uint32_t(distance / 256.);
        RenderCore good(valid.ref());
        require(good.restorePrimedCursor(0, 250), "Cannot restore loop validation cursor");
        std::vector<float> output(frames * 2);
        require(good.render(output) && !good.readFailed(0) && good.sourcePosition(0) == valid.result.nominalPosition,
                "Valid processed loop wrap was rejected");
        for (int mode = 0; mode < 9; ++mode)
        {
            Source bad;
            bad.result = valid.result;
            if (mode == 0)
                bad.result.loopWraps = 0;
            if (mode == 1)
                bad.result.loopWraps = UINT32_MAX;
            if (mode == 2)
                bad.result.positionValid = false;
            if (mode == 3)
                bad.result.nominalPosition = 256;
            if (mode == 4)
                bad.result.finished = true;
            if (mode == 5)
                bad.loop.active = false;
            if (mode == 6)
                bad.result.nominalPosition = std::numeric_limits<double>::quiet_NaN();
            if (mode == 7)
                bad.result.frames = 0;
            if (mode == 8)
                bad.loop.ready = false;
            RenderCore core(bad.ref());
            require(core.restorePrimedCursor(0, 250), "Cannot restore malformed loop cursor");
            require(core.render(output) && core.readFailed(0) && core.sourcePosition(0) == 250 &&
                        std::ranges::all_of(output, [](float sample) { return sample == 0; }),
                    "Forged loop metadata escaped fail-closed rendering");
        }
    }
}
} // namespace

[[gnu::noinline]] void *operator new(std::size_t bytes)
{
    if (callback)
        ++callbackAllocations;
    if (auto *p = std::malloc(bytes ? bytes : 1))
        return p;
    throw std::bad_alloc();
}
[[gnu::noinline]] void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
[[gnu::noinline]] void operator delete(void *p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void *p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void *p, std::size_t) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
extern "C" ssize_t __real_pread(int, void *, size_t, off_t);
extern "C" ssize_t __real_pread64(int, void *, size_t, off64_t);
extern "C" ssize_t __real___pread_chk(int, void *, size_t, off_t, size_t);
extern "C" ssize_t __real___pread64_chk(int, void *, size_t, off64_t, size_t);
extern "C" ssize_t __wrap_pread(int fd, void *data, size_t count, off_t offset)
{
    if (callback)
        ++callbackReads;
    return __real_pread(fd, data, count, offset);
}
extern "C" ssize_t __wrap_pread64(int fd, void *data, size_t count, off64_t offset)
{
    if (callback)
        ++callbackReads;
    return __real_pread64(fd, data, count, offset);
}
extern "C" ssize_t __wrap___pread_chk(int fd, void *data, size_t count, off_t offset, size_t capacity)
{
    if (callback)
        ++callbackReads;
    return __real___pread_chk(fd, data, count, offset, capacity);
}
extern "C" ssize_t __wrap___pread64_chk(int fd, void *data, size_t count, off64_t offset, size_t capacity)
{
    if (callback)
        ++callbackReads;
    return __real___pread64_chk(fd, data, count, offset, capacity);
}
int main()
{
    try
    {
        unityWrapsPartialConsumptionAndRangeRejection();
        continuousSrcAndMasterTempo();
        combinedControlsAndPeerClock();
        currentStemGainsAcrossProcessedWrapsAndReset();
        cyclicSourceMutationAndCancellation();
        malformedLoopMetadataFailsClosed();
        require(!callbackReads && !callbackAllocations, "Loop/tempo callback performed file I/O or allocated storage");
        std::cout << "Prepared loop/tempo PCM, continuous SRC/R3, transport and metadata checks passed\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedTempoSource.hpp"
#include "audio/PreparedPlayback.hpp"
#include "audio/TempoProcessor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace conflux::audio;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t frameBytes = 2U * sizeof(float);
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
bool same(std::span<const float> a, std::span<const float> b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](float x, float y) {
        return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
    });
}
template<class Function> void rejects(Function&& function, const char* message)
{
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
template<class Predicate> void wait(Predicate&& predicate, const char* message)
{
    const auto deadline = Clock::now() + 10s;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
struct CloseFile { void operator()(std::FILE* file) const { if (file) std::fclose(file); } };
using File = std::unique_ptr<std::FILE, CloseFile>;
struct Descriptor {
    int fd;
    explicit Descriptor(int value) : fd(value) { require(fd >= 0, "Cannot open fixture FD"); }
    ~Descriptor() { close(fd); }
};
File rawFile(std::span<const float> pcm)
{
    File result(std::tmpfile());
    require(bool(result) && std::fwrite(pcm.data(), sizeof(float), pcm.size(), result.get()) == pcm.size() &&
                std::fflush(result.get()) == 0, "Cannot write tempo fixture PCM");
    return result;
}
struct Fixture {
    std::uint64_t frames;
    std::array<std::vector<float>, 4> samples;
    std::array<File, 4> files;
    std::array<struct stat, 4> identities{};
    explicit Fixture(std::uint64_t count = 20017) : frames(count)
    {
        for (std::size_t leaf = 0; leaf < 4; ++leaf) {
            auto& pcm = samples[leaf];
            pcm.resize(frames * 2U);
            for (std::uint64_t frame = 0; frame < frames; ++frame) {
                pcm[frame * 2] = float(int((frame * 7 + leaf * 31) % 4093) - 2046) / 1024.F;
                pcm[frame * 2 + 1] = float(int((frame * 13 + leaf * 43) % 4057) - 2028) / 512.F;
            }
            if (frames > 18) {
                pcm[34] = -0.F; pcm[35] = 0.F;
                pcm[36] = 2.75F; pcm[37] = -3.25F;
            }
            files[leaf] = rawFile(pcm);
            require(fstat(fileno(files[leaf].get()), &identities[leaf]) == 0, "Cannot identify fixture PCM");
        }
    }
    std::array<int, 4> fds() const
    { return {fileno(files[0].get()), fileno(files[1].get()), fileno(files[2].get()), fileno(files[3].get())}; }
    void overwrite(std::size_t leaf, std::uint64_t frame, float value)
    {
        require(pwrite(fileno(files[leaf].get()), &value, sizeof(value), off_t(frame * frameBytes)) == sizeof(value),
                "Cannot change private fixture PCM");
        samples[leaf][frame * 2] = value;
    }
};
std::set<int> matchingDescriptors(const Fixture& fixture)
{
    std::set<int> result;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const int fd = std::stoi(entry.path().filename().string());
        struct stat status{};
        if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) continue;
        if (std::ranges::any_of(fixture.identities, [&](const auto& wanted) {
                return wanted.st_dev == status.st_dev && wanted.st_ino == status.st_ino;
            })) result.insert(fd);
    }
    return result;
}
PreparedTempoSource::View next(PreparedTempoSource& source)
{
    PreparedTempoSource::View view;
    wait([&] { view = source.peek(); return bool(view); }, "Tempo worker did not publish its next packet");
    return view;
}
void exactPacket(const PreparedTempoSource::View& view, const Fixture& fixture,
                 std::size_t leaves, std::uint64_t frame, std::uint64_t generation)
{
    require(view.generation == generation && view.leafCount == leaves && view.frames &&
                view.frames <= PreparedTempoSource::blockFrames && !view.terminal && !view.errorCode &&
                view.nominalStart == double(frame) && view.nominalStep == 1 &&
                view.requestedPercent == 0 && view.appliedInputPercent == 0,
            "Unity packet geometry, generation or source coordinates differ");
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(leaf < leaves ? same(view.leaves[leaf], std::span(fixture.samples[leaf]).subspan(frame * 2, view.frames * 2))
                             : view.leaves[leaf].empty(),
                "Tempo worker reordered roles/channels, changed signed zero/headroom or reused live storage");
}

void exactBypassBackpressureAndConsumedEof()
{
    for (const std::size_t leaves : {1U, 4U}) {
        Fixture fixture;
        auto fds = fixture.fds();
        const auto originals = matchingDescriptors(fixture);
        std::array<off_t, 4> offsets{};
        for (std::size_t leaf = 0; leaf < leaves; ++leaf) {
            offsets[leaf] = off_t((leaf + 3) * frameBytes);
            require(lseek(fds[leaf], offsets[leaf], SEEK_SET) == offsets[leaf], "Cannot position original FD");
        }
        PreparedTempoSource source{std::span(fds).first(leaves), fixture.frames};
        require(!source.peek() && !source.errorCode() && !source.status().active &&
                    !source.consume(0) && !source.requestSpeed(1), "Idle worker exposed PCM or accepted active controls");
        const auto owned = matchingDescriptors(fixture);
        require(owned.size() == originals.size() + leaves, "Tempo worker did not retain exactly one duplicate per role");
        for (const int fd : owned)
            if (!originals.contains(fd)) require((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "Tempo FD lacks CLOEXEC");
        require(source.request(17, 0, true), "Unity/key-lock request rejected");
        const auto generation = source.generation();
        auto first = next(source);
        exactPacket(first, fixture, leaves, 17, generation);
        std::array<std::vector<float>, 4> held;
        for (std::size_t leaf = 0; leaf < leaves; ++leaf)
            held[leaf].assign(first.leaves[leaf].begin(), first.leaves[leaf].end());
        // No consume models pause/backpressure. Prefetch may fill its ring but
        // cannot advance public position or recycle this unacknowledged slot.
        std::this_thread::sleep_for(20ms);
        require(source.nominalPosition() == 17 && !source.status().finished,
                "Prefetch advanced the public cursor or marked unconsumed audio finished");
        first = source.peek();
        exactPacket(first, fixture, leaves, 17, generation);
        for (std::size_t leaf = 0; leaf < leaves; ++leaf)
            require(same(first.leaves[leaf], held[leaf]), "Unconsumed ring slot was overwritten");
        for (const auto percent : {6.001, -6.001, std::numeric_limits<double>::quiet_NaN()})
            require(!source.request(17, percent, false) && !source.requestSpeed(percent) &&
                        source.generation() == generation && source.nominalPosition() == 17,
                    "Invalid speed changed a ready generation");
        require(!source.request(fixture.frames + 1, 0, false) && !source.requestSpeed(1) &&
                    source.generation() == generation && source.status().keyLock,
                "Invalid IN/fresh bypass backend transition changed ready state");
        std::uint64_t position = 17;
        for (std::size_t part = 0;; ++part) {
            const auto view = next(source);
            if (view.terminal) {
                require(!view.frames && !view.errorCode && position == fixture.frames &&
                            !source.status().finished && source.nominalPosition() == double(fixture.frames),
                        "EOF bypassed queued PCM or became finished before consumption");
                require(!source.consume(1) && !source.status().finished && source.consume(0) &&
                            source.status().finished && !source.peek() && !source.consume(0),
                        "EOF marker consumption is not explicit/exclusive");
                break;
            }
            exactPacket(view, fixture, leaves, position, generation);
            require(!source.consume(view.frames + 1) && source.nominalPosition() == double(position),
                    "Over-consume changed the current packet/cursor");
            exactPacket(source.peek(), fixture, leaves, position, generation);
            constexpr std::array<std::size_t, 4> portions{1, 17, 257, 73};
            const auto count = std::min(view.frames, portions[part % portions.size()]);
            require(source.consume(count), "Valid partial packet consumption failed");
            position += count;
            require(source.nominalPosition() == double(position), "Partial consumption lost nominal source position");
        }
        for (std::size_t leaf = 0; leaf < leaves; ++leaf)
            require(lseek(fds[leaf], 0, SEEK_CUR) == offsets[leaf], "Tempo pread changed caller offsets");
        for (auto& file : fixture.files) file.reset();
        require(source.request(fixture.frames - 3, 0, false) && source.generation() > generation,
                "Owned duplicate did not survive caller close");
        exactPacket(next(source), fixture, leaves, fixture.frames - 3, source.generation());
        source.stop(); source.stop();
        require(!source.peek() && !source.status().active && !source.request(0, 0, false) &&
                    !source.requestSpeed(0) && !source.cancel() && matchingDescriptors(fixture).empty(),
                "Stopped worker accepted work or retained duplicated descriptors");
    }
}

void constructorAndCurrentGenerationFailures()
{
    Fixture fixture(4099);
    const auto fds = fixture.fds();
    const auto before = matchingDescriptors(fixture);
    for (const std::size_t count : {0U, 2U, 3U})
        rejects([&] { PreparedTempoSource bad{std::span(fds).first(count), fixture.frames}; }, "Invalid role count accepted");
    const std::array<int, 5> five{fds[0], fds[1], fds[2], fds[3], fds[0]};
    rejects([&] { PreparedTempoSource bad(five, fixture.frames); }, "Five roles accepted");
    for (const auto frames : {std::uint64_t(0), fixture.frames - 1, fixture.frames + 1,
                              std::numeric_limits<std::uint64_t>::max()})
        rejects([&] { PreparedTempoSource bad(fds, frames); }, "Inexact/empty/unaddressable geometry accepted");
    auto bad = fds; bad[3] = -1;
    rejects([&] { PreparedTempoSource source(bad, fixture.frames); }, "Invalid fourth FD accepted");
    const auto path = "/proc/self/fd/" + std::to_string(fds[3]);
    Descriptor writeOnly(open(path.c_str(), O_WRONLY | O_CLOEXEC));
    bad[3] = writeOnly.fd;
    rejects([&] { PreparedTempoSource source(bad, fixture.frames); }, "Write-only fourth FD accepted");
#ifdef O_PATH
    Descriptor pathOnly(open(path.c_str(), O_PATH | O_CLOEXEC));
    bad[3] = pathOnly.fd;
    rejects([&] { PreparedTempoSource source(bad, fixture.frames); }, "O_PATH descriptor accepted");
#endif
    std::array<int, 2> pipes{};
    require(pipe(pipes.data()) == 0, "Cannot construct nonregular source fixture");
    Descriptor readPipe(pipes[0]), writePipe(pipes[1]);
    const std::array<int, 1> nonregular{readPipe.fd};
    rejects([&] { PreparedTempoSource source(nonregular, 1); }, "Nonregular source accepted");
    auto expected = before; expected.insert(writeOnly.fd);
#ifdef O_PATH
    expected.insert(pathOnly.fd);
#endif
    require(matchingDescriptors(fixture) == expected, "Rejected partial construction leaked FDs");
    for (std::size_t leaf = 0; leaf < 4; ++leaf) {
        Fixture corrupt(4099);
        corrupt.overwrite(leaf, 17, leaf % 2 ? std::numeric_limits<float>::infinity()
                                           : std::numeric_limits<float>::quiet_NaN());
        PreparedTempoSource source(corrupt.fds(), corrupt.frames);
        require(source.request(17, 0, false), "Nonfinite test request rejected before worker admission");
        const auto view = next(source);
        require(!view.frames && !view.terminal && view.errorCode == EILSEQ && source.errorCode() == EILSEQ &&
                    !source.status().finished && source.nominalPosition() == 17,
                "Nonfinite role was hidden, delivered or treated as successful EOF");
        require(source.consume(0) && !source.status().finished && source.cancel() && !source.errorCode(),
                "Error acknowledgement/cancel retained failure or marked EOF");
        require(source.request(corrupt.frames, 0, true), "Empty EOF request rejected");
        const auto eof = next(source);
        require(eof.terminal && !eof.frames && !eof.errorCode && !source.status().finished &&
                    source.consume(0) && source.status().finished, "Prior generation poisoned exact empty EOF");
    }
    for (std::size_t leaf = 0; leaf < 4; ++leaf) {
        Fixture truncated(4099);
        PreparedTempoSource source(truncated.fds(), truncated.frames);
        require(ftruncate(fileno(truncated.files[leaf].get()), off_t((truncated.frames - 1) * frameBytes)) == 0 &&
                    source.request(0, 0, false), "Cannot create changed-geometry request");
        const auto view = next(source);
        require(!view.frames && !view.terminal && view.errorCode == ESTALE && source.errorCode() == ESTALE &&
                    !source.status().finished, "Changed nonrepresentative leaf geometry did not fail closed");
    }
}

// One-shot gate on a real worker read; caller owns the release before owner
// destruction on every unwind path. There is no production test hook.
struct GateState {
    struct stat identity{};
    std::mutex mutex;
    std::condition_variable changed;
    bool claimed = false, entered = false, released = false;
};
std::mutex gateMutex;
std::shared_ptr<GateState> activeGate;
struct ReadGate {
    std::shared_ptr<GateState> state = std::make_shared<GateState>();
    explicit ReadGate(int fd)
    {
        require(fstat(fd, &state->identity) == 0, "Cannot identify gated source");
        const std::lock_guard lock(gateMutex);
        require(!activeGate, "Nested worker read gate"); activeGate = state;
    }
    ~ReadGate() { release(); }
    void entered()
    {
        std::unique_lock lock(state->mutex);
        require(state->changed.wait_for(lock, 10s, [&] { return state->entered; }), "Worker did not reach real pread gate");
    }
    void release()
    {
        { const std::lock_guard lock(state->mutex); state->released = true; }
        state->changed.notify_all();
        const std::lock_guard lock(gateMutex);
        if (activeGate == state) activeGate.reset();
    }
};
void gateRead(int fd, std::uint64_t offset)
{
    if (offset != 17 * frameBytes) return;
    std::shared_ptr<GateState> state;
    { const std::lock_guard lock(gateMutex); state = activeGate; }
    if (!state) return;
    struct stat actual{};
    if (fstat(fd, &actual) != 0 || actual.st_dev != state->identity.st_dev || actual.st_ino != state->identity.st_ino) return;
    std::unique_lock lock(state->mutex);
    if (state->claimed) return;
    state->claimed = state->entered = true;
    state->changed.notify_all();
    state->changed.wait(lock, [&] { return state->released; });
}
void inFlightSupersessionAndSameFrameGeneration()
{
    for (const bool nonfinite : {false, true}) {
        Fixture fixture(4099);
        if (nonfinite) fixture.overwrite(0, 17, std::numeric_limits<float>::quiet_NaN());
        const auto fds = fixture.fds();
        PreparedTempoSource source{std::span(fds).first(1), fixture.frames};
        ReadGate gate(fds[0]); // Releases before source destruction, including failed assertions.
        require(source.request(17, 0, false), "Initial gated request rejected");
        const auto oldGeneration = source.generation();
        gate.entered();
        require(!source.peek() && source.cancel() && !source.peek() && !source.errorCode() &&
                    source.generation() > oldGeneration, "Cancel did not immediately hide blocked old generation");
        const auto cancelledGeneration = source.generation();
        const auto target = nonfinite ? fixture.frames : 29;
        require(source.request(target, 0, true) && source.generation() > cancelledGeneration,
                "New generation rejected while prior pread was in flight");
        const auto current = source.generation();
        gate.release();
        const auto view = next(source);
        require(view.generation == current && !view.errorCode && !source.errorCode() && source.status().keyLock,
                "Old pread completion/error poisoned a new generation");
        if (nonfinite) require(view.terminal && !view.frames, "Stale failed read replaced new EOF");
        else {
            exactPacket(view, fixture, 1, target, current);
            require(source.consume(7) && source.nominalPosition() == 36 && source.request(36, 0, false) &&
                        source.generation() > current, "Same-frame explicit seek did not get a new generation");
            exactPacket(next(source), fixture, 1, 36, source.generation());
            const auto beforeKeyLock = source.generation();
            require(source.request(36, 0, true) && source.generation() > beforeKeyLock,
                    "Explicit key-lock rebuild reused the old generation");
            exactPacket(next(source), fixture, 1, 36, source.generation());
        }
    }
}

void actualDspPacketsAndTailBeforeFinished()
{
    for (const bool keyLock : {false, true}) {
        const std::size_t frames = keyLock ? 96000 : 8192;
        std::vector<float> input(frames * 2);
        if (keyLock) { input[input.size() - 2] = .5F; input.back() = -.25F; }
        else for (std::size_t frame = 0; frame < frames; ++frame) {
            input[frame * 2] = .3F * std::sin(2 * std::acos(-1.) * 375 * double(frame) / 48000);
            input[frame * 2 + 1] = .2F * std::sin(2 * std::acos(-1.) * 997 * double(frame) / 48000 + .37);
        }
        auto file = rawFile(input);
        const std::array<int, 1> fds{fileno(file.get())};
        PreparedTempoSource source(fds, frames);
        require(source.request(0, -6, keyLock), "Real DSP worker request rejected");
        std::vector<float> delivered;
        const auto hypothetical = std::size_t(std::ceil(double(frames) / .94));
        double previousPosition = 0;
        std::size_t part = 0;
        for (;;) {
            const auto view = next(source);
            require(!view.errorCode && !source.errorCode() && view.generation == source.generation() &&
                        !source.status().finished, "DSP worker failed or finished before terminal consumption");
            if (view.terminal) {
                require(!view.frames && source.consume(0) && source.status().finished &&
                            source.nominalPosition() == double(frames), "DSP terminal/cursor did not follow full tail");
                break;
            }
            require(view.leafCount == 1 && view.frames && view.frames <= 1024 &&
                        view.leaves[0].size() == view.frames * 2 && view.nominalStart >= previousPosition &&
                        view.nominalStart <= double(frames) && std::abs(view.nominalStep - .94) < 1e-12 &&
                        view.appliedInputPercent == -6, "DSP packet lost source mapping metadata or geometry");
            constexpr std::array<std::size_t, 4> portions{1, 17, 257, 1024};
            const auto count = std::min(view.frames, portions[part++ % portions.size()]);
            const auto samples = view.leaves[0].first(count * 2);
            require(std::ranges::all_of(samples, [](float value) { return std::isfinite(value); }), "DSP output is nonfinite");
            delivered.insert(delivered.end(), samples.begin(), samples.end());
            require(delivered.size() <= 48000U * 12U * 2U && source.consume(count), "DSP worker exceeded bounded fixture output");
            previousPosition = source.nominalPosition();
            require(previousPosition >= view.nominalStart && previousPosition <= double(frames) &&
                        source.status().appliedPercent == -6, "Consumed packet metadata/cursor differs");
        }
        if (keyLock) {
            require(delivered.size() / 2 > hypothetical, "Worker nominally cropped actual R3 output");
            double excessPeak = 0;
            for (const auto value : std::span(delivered).subspan(hypothetical * 2))
                excessPeak = std::max(excessPeak, std::abs(double(value)));
            require(excessPeak > 1e-4, "Worker discarded the genuine last-impulse tail after nominal EOF");
        } else {
            require(std::abs(double(delivered.size() / 2) - double(frames) / .94) <= 2,
                    "SRC worker lost input or final drain frames");
            for (std::size_t frame = 1536; frame + 1536 < delivered.size() / 2; ++frame)
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    const double frequency = channel ? 997 : 375, gain = channel ? .2 : .3, phase = channel ? .37 : 0;
                    const double expected = gain * std::sin(2 * std::acos(-1.) * frequency * double(frame) * .94 / 48000 + phase);
                    require(std::abs(double(delivered[frame * 2 + channel]) - expected) < 1e-3,
                            "SRC worker changed channel order, phase or actual speed");
                }
        }
    }
}

void preparedControlsAndSharedRenderSideGains()
{
    for (const bool bank : {false, true}) {
        Fixture fixture(4099);
        const auto fds = fixture.fds();
        auto playback = bank ? std::make_unique<PreparedPlayback>(fds, fixture.frames)
                             : std::make_unique<PreparedPlayback>(fds[0], fixture.frames);
        require(playback->prime(0, 2s), "Cannot prime integration's healthy legacy readers");
        auto source = playback->sourceRef();
        auto control = [&](AudioTempoCommand command, double percent, bool keyLock, std::uint64_t frame) {
            return source.tempoControl(source.context, command, percent, keyLock, frame);
        };
        auto status = [&] { return source.tempoState(source.context); };
        require(source.tempoControl && source.tempoState && status().supported && !status().active &&
                    source.loopControl(source.context, AudioLoopCommand::In, 17),
                "Prepared source lost optional tempo/loop capabilities");
        const auto idleGeneration = status().generation;
        require(control(AudioTempoCommand::Set, 0, true, 17) && status().generation == idleGeneration &&
                    status().keyLock && source.loopState(source.context).inSet &&
                    control(AudioTempoCommand::Set, -6, false, 17) && status().active &&
                    source.loopState(source.context).inSet,
                "Pending loop IN lost its bounds or rejected prepared tempo/zero-key-lock selection");
        require(source.loopControl(source.context, AudioLoopCommand::Clear, 17) &&
                    control(AudioTempoCommand::Set, -6, false, 17), "Cannot enter processed mode after clearing IN");
        const auto initial = status().generation;
        require(control(AudioTempoCommand::Set, 0, false, 17) && status().active && status().generation == initial &&
                    source.loopControl(source.context, AudioLoopCommand::In, 17) &&
                    source.loopControl(source.context, AudioLoopCommand::Clear, 17),
                "Active DSP crossing zero reset its generation or rejected/retained a new loop IN");
        require(control(AudioTempoCommand::Set, 0, true, 17) && status().generation > initial && status().keyLock,
                "Key-lock rebuild at zero did not establish a fresh generation");
        const auto beforeSession = status().generation;
        // A quiesced session replacement keeps the selected mode, re-primes a
        // fresh generation and consumes no output to establish readiness.
        require(playback->primeForSession(17, 2s), "Active zero could not re-prime for a new session");
        source = playback->sourceRef(); // Old source borrows end at bank rebuild.
        require(status().active && status().percent == 0 && status().keyLock &&
                    status().generation > beforeSession && !status().finished && playback->configureStemRamps(4),
                "Session prime reset tempo mode, consumed EOF or lost gain configuration");
        constexpr std::array<float, 4> targets{0.F, .5F, .25F, 1.F};
        if (bank) for (std::size_t leaf = 0; leaf < 4; ++leaf)
            require(source.setStemGain(source.context, leaf, targets[leaf]), "Cannot change render-side stem target");
        std::uint64_t position = 17;
        auto readExact = [&](std::size_t frames, bool ramp) {
            const auto began = position;
            std::size_t delivered = 0;
            while (delivered < frames) {
                std::array<float, 514> output{};
                const auto wanted = std::min<std::size_t>(257, frames - delivered);
                AudioReadResult result;
                wait([&] {
                    result = source.read(source.context, position, std::span(output).first(wanted * 2));
                    require(!result.failed && !result.finished && result.positionValid,
                            "Prepared active-unity read failed, ended early or dropped position metadata");
                    return result.frames != 0;
                }, "Prepared active-unity read did not become ready");
                require(result.frames <= wanted && result.nominalPosition == double(position + result.frames),
                        "Prepared active-unity output/cursor count differs");
                for (std::size_t frame = 0; frame < result.frames; ++frame)
                    for (std::size_t channel = 0; channel < 2; ++channel) {
                        const auto original = position + frame;
                        float expected = fixture.samples[0][original * 2 + channel];
                        if (bank) {
                            double mixed = 0;
                            const double fraction = ramp ? std::min(1., double(original - began + 1) / 4) : 1.;
                            for (std::size_t leaf = 0; leaf < 4; ++leaf) {
                                const auto gain = (1 - fraction) + fraction * targets[leaf];
                                mixed += double(fixture.samples[leaf][original * 2 + channel]) * gain;
                            }
                            expected = static_cast<float>(mixed);
                        }
                        require(std::bit_cast<std::uint32_t>(output[frame * 2 + channel]) ==
                                    std::bit_cast<std::uint32_t>(expected),
                                "Processed unity changed raw bits, role order or shared gain-ramp progression");
                    }
                position += result.frames;
                delivered += result.frames;
            }
        };
        readExact(263, true); // Partial slots and a gain ramp cross multiple reads.
        if (bank) for (std::size_t leaf = 0; leaf < 4; ++leaf)
            require(source.stemGain(source.context, leaf) == targets[leaf], "Tempo processing changed a stem gain target");
        const auto beforeSeek = status().generation;
        source.discontinuity(source.context, position); // Same-frame seek is still a fresh epoch.
        require(status().generation > beforeSeek && status().active && status().keyLock,
                "Same-frame discontinuity retained queued tempo packets or lost its mode");
        readExact(17, false);
        const auto freshUnity = status().generation;
        require(control(AudioTempoCommand::Set, 6, true, position) && status().active &&
                    status().generation > freshUnity && status().percent == 6,
                "First nonzero request from a rebuilt unity stream lacked explicit fresh setup");
        require(control(AudioTempoCommand::Reset, 0, true, position) && !status().active && status().keyLock &&
                    playback->prime(position, 2s), "Reset did not cancel DSP and restore the captured legacy cursor");
        std::array<float, 34> legacy{};
        const auto restored = source.read(source.context, position, legacy);
        require(!restored.failed && restored.frames == 17 && !restored.positionValid && !playback->errorCode(),
                "Legacy source did not resume after explicit reset");
        for (std::size_t frame = 0; frame < 17; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                double expected = fixture.samples[0][(position + frame) * 2 + channel];
                if (bank) {
                    expected = 0;
                    for (std::size_t leaf = 0; leaf < 4; ++leaf)
                        expected += double(fixture.samples[leaf][(position + frame) * 2 + channel]) * targets[leaf];
                }
                require(std::bit_cast<std::uint32_t>(legacy[frame * 2 + channel]) ==
                            std::bit_cast<std::uint32_t>(static_cast<float>(expected)),
                        "Reset resumed a stale cursor, changed selected gains or substituted processed PCM");
            }
    }
}
} // namespace

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
        exactBypassBackpressureAndConsumedEof();
        constructorAndCurrentGenerationFailures();
        inFlightSupersessionAndSameFrameGeneration();
        actualDspPacketsAndTailBeforeFinished();
        preparedControlsAndSharedRenderSideGains();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/PreparedScratchSource.hpp"
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace conflux::audio;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
bool near(float actual, double expected, double tolerance = 2e-6)
{ return std::isfinite(actual) && std::abs(double(actual) - expected) <= tolerance; }
struct CloseFile { void operator()(std::FILE* file) const { if (file) std::fclose(file); } };
using File = std::unique_ptr<std::FILE, CloseFile>;
File rawFile(std::span<const float> pcm)
{
    File file(std::tmpfile());
    require(file && std::fwrite(pcm.data(), sizeof(float), pcm.size(), file.get()) == pcm.size() &&
        std::fflush(file.get()) == 0, "Cannot write generated scratch playback fixture");
    return file;
}
// Independent original-track oracle: asymmetric, distinct per leaf/channel,
// affine away from EOF so fractional reconstruction has a known value without
// copying the interpolator's table or comparing against its own output.
double signal(double frame, std::size_t leaf, std::size_t channel)
{
    return channel ? -.0625 * double(leaf + 2) - frame * .000005 * double(leaf + 1)
                   : .125 * double(leaf + 1) + frame * .00001 * double(leaf + 1);
}
struct Fixture {
    std::uint64_t frames;
    std::array<std::vector<float>, 4> pcm;
    std::array<File, 4> files;
    std::array<struct stat, 4> identities{};
    explicit Fixture(std::uint64_t count = 32768) : frames(count)
    {
        for (std::size_t leaf = 0; leaf < 4; ++leaf) {
            pcm[leaf].resize(frames * 2);
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    pcm[leaf][frame * 2 + channel] = float(signal(double(frame), leaf, channel));
            files[leaf] = rawFile(pcm[leaf]);
            require(fstat(fileno(files[leaf].get()), &identities[leaf]) == 0, "Cannot identify generated PCM");
        }
    }
    std::unique_ptr<PreparedPlayback> owner(bool bank) const
    {
        if (bank) return std::make_unique<PreparedPlayback>(std::array<int, 4>{
            fileno(files[0].get()), fileno(files[1].get()), fileno(files[2].get()), fileno(files[3].get())}, frames);
        return std::make_unique<PreparedPlayback>(fileno(files[0].get()), frames);
    }
    float mixed(std::uint64_t frame, std::size_t channel, const std::array<double, 4>& gains) const
    {
        double sum = 0;
        for (std::size_t leaf = 0; leaf < 4; ++leaf) sum += double(pcm[leaf][frame * 2 + channel]) * gains[leaf];
        return float(sum);
    }
};
std::vector<float> peerSignal(std::size_t frames = 65536)
{
    std::vector<float> pcm(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        pcm[frame * 2] = float(int(frame % 31) - 15) / 32.F;
        pcm[frame * 2 + 1] = float(int(frame % 29) - 14) / 64.F;
    }
    return pcm;
}
struct Buses { std::vector<float> master, cue; };
Buses render(RenderCore& core, std::size_t frames)
{
    Buses result{std::vector<float>(frames * 2), std::vector<float>(frames * 2)};
    require(core.renderBuses(result.master, result.cue), "Actual renderer rejected complete stereo output");
    return result;
}
void readyWhileHeld(RenderCore& core)
{
    const auto deadline = Clock::now() + 2s;
    const auto position = core.sourcePosition();
    while (core.scratchState(0).pending) {
        require(Clock::now() < deadline && !core.scratchState(0).errorCode, "Optional scratch worker did not settle");
        render(core, 1); // Real held read adopts publication, status itself is pure.
        require(core.sourcePosition() == position && !core.readFailed(0), "Prefetch/held silence advanced or failed source");
        std::this_thread::sleep_for(1ms);
    }
}
void warm(RenderCore& core, double target)
{
    const auto deadline = Clock::now() + 2s;
    for (std::size_t count = 0; core.sourcePosition() != target; ++count) {
        require(count < 2000 && Clock::now() < deadline, "Prepared scratch did not reach finite nearby target");
        const auto before = core.sourcePosition(); render(core, 1);
        require(!core.readFailed(0) && std::abs(core.sourcePosition() - before) <= 8,
                "Actual playback rejected signed scratch metadata or jumped cursor");
    }
}
// A normal refill may be unavailable: retain source position and let the peer
// and output clock continue. Return ONLY the first actually consumed sample.
Buses firstLinearFrame(RenderCore& core)
{
    const auto deadline = Clock::now() + 2s;
    for (;;) {
        const auto before = core.sourceFrame();
        const auto clock = core.frameClock(), peer = core.sourceFrame(1);
        auto output = render(core, 1);
        require(core.frameClock() == clock + 1 && core.sourceFrame(1) == peer + 1 && !core.readFailed(0),
                "Ordinary refill stopped peer/output clock or reported source failure");
        if (core.sourceFrame() == before + 1) return output;
        require(core.sourceFrame() == before && core.isBuffering(0) && Clock::now() < deadline,
                "Unavailable ordinary data advanced source or failed to settle");
        std::this_thread::sleep_for(1ms);
    }
}

void normalPlatterBusAndRelease()
{
    for (const bool playing : {false, true}) {
        Fixture fixture; auto owner = fixture.owner(false);
        require(owner->prime(10000, 2s), "Cannot prime real normal playback");
        const auto peer = peerSignal();
        RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
        require(core.restorePrimedCursor(0, 10000) && core.setPlaying(0, playing) &&
            core.setDeckGain(0, .5F) && core.setDeckGain(1, .25F) && core.setMasterGain(.75F) &&
            core.setCrossfader(.75F) && core.setCueSelected(0, true) && core.setCueGain(.5F) &&
            core.beginScratch(0, 1), "Cannot enter real prepared scratch mode");
        readyWhileHeld(core);
        const auto oldClock = core.frameClock(), oldPeer = core.sourceFrame(1);
        const auto held = render(core, 17);
        for (std::size_t frame = 0; frame < 17; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(near(held.master[frame * 2 + channel], double(peer[(oldPeer + frame) * 2 + channel]) * .25 * .75) &&
                    held.cue[frame * 2 + channel] == 0.F, "Held platter emitted DC or changed peer/master/cue routing");
        require(core.sourcePosition() == 10000 && core.sourceFrame() == 10000 && core.isPlaying() == playing &&
            core.frameClock() == oldClock + 17 && core.sourceFrame(1) == oldPeer + 17 &&
            !core.isBuffering(0) && !core.starvedFrames(0) && !owner->errorCode(), "Held platter changed clocks/play request/health");
        require(core.moveScratch(0, 1, 256, 256.0 / 48000), "Cannot warm actual forward platter");
        warm(core, 10256); // More than240 moving output frames: motion envelope is fully open.
        require(core.moveScratch(0, 1, 736, .01), "Cannot set steady unit platter speed");
        const auto beforePeer = core.sourceFrame(1);
        const auto forward = render(core, 480);
        for (std::size_t frame = 0; frame < 480; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const double raw = fixture.pcm[0][(10256 + frame) * 2 + channel];
                require(near(forward.master[frame * 2 + channel],
                    (raw * .5 * .5 + double(peer[(beforePeer + frame) * 2 + channel]) * .25) * .75) &&
                    near(forward.cue[frame * 2 + channel], raw * .5), "Forward scratch did not use actual PCM/prefader cue/gain/crossfader");
            }
        require(core.sourcePosition() == 10736 && core.sourceFrame() == 10736 && core.isPlaying() == playing,
                "Consumed forward scratch lost nominal/play-state contract");
        require(core.moveScratch(0, 1, 1216, .01), "Cannot create active forward backlog"); render(core, 1);
        require(core.moveScratch(0, 1, 976, .01), "Cannot reverse an already active platter");
        const auto turn = core.sourcePosition();
        std::size_t compared = 0;
        for (std::size_t frame = 0; frame < 300; ++frame) {
            const auto before = core.sourcePosition();
            const auto peerFrame = core.sourceFrame(1);
            const auto output = render(core, 1); const auto after = core.sourcePosition();
            const double samplePosition = after < before ? after : before;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                require(std::isfinite(output.master[channel]) && std::isfinite(output.cue[channel]), "Reversal envelope produced nonfinite output");
                // Braking and the240-frame shared attack are intentional. Once
                // settled, compare actual source geometry rather than copying
                // the envelope/interpolator implementation into this oracle.
                if (frame >= 272 && after < before) {
                    const double raw = signal(samplePosition, 0, channel);
                    require(near(output.master[channel], (raw * .5 * .5 + double(peer[peerFrame * 2 + channel]) * .25) * .75, 1e-5) &&
                        near(output.cue[channel], raw * .5, 1e-5), "Reversing scratch was not aligned actual track-derived stereo PCM");
                    ++compared;
                }
            }
        }
        require(compared >= 32 && core.sourcePosition() < turn && !core.readFailed(0) && !owner->errorCode(),
                "Active scratch did not reverse/settle through real renderer");
        const auto released = core.sourcePosition();
        require(core.endScratch(0, 1) && !core.scratchState(0).active && core.isPlaying() == playing &&
            core.sourceFrame() == std::uint64_t(released) && core.sourcePosition() == released,
            "Release changed play request or used target instead of actual consumed scratch cursor");
        require(!core.moveScratch(0, 1, 1000, .01), "Late move revived a released gesture");
        if (!playing) {
            const auto position = core.sourcePosition(); render(core, 32);
            require(core.sourcePosition() == position && !core.isPlaying(), "Paused scratch release started playback");
        } else {
            const auto firstFrame = core.sourceFrame();
            auto output = firstLinearFrame(core);
            const auto deliveredPeer = core.sourceFrame(1) - 1;
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(near(output.master[channel], (double(fixture.pcm[0][firstFrame * 2 + channel]) * .25 +
                    double(peer[deliveredPeer * 2 + channel]) * .25) * .75), "Release resumed wrong ordinary PCM frame");
            require(core.endScratch(0, 1), "Duplicate matching release was not idempotent");
            const auto nextFrame = core.sourceFrame(); render(core, 1);
            require(core.sourceFrame() == nextFrame + 1 && !core.isBuffering(0), "Duplicate release restarted reader/rewound source");
        }
    }
}

void sharedStemGainsAndLinearContinuation()
{
    Fixture fixture; auto owner = fixture.owner(true);
    require(owner->prime(1000, 2s), "Cannot prime real four-stem owner");
    const auto source = owner->sourceRef();
    constexpr std::array<double, 4> initial{1, .5, .25, 0}, targets{0, .75, .5, 1};
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(source.setStemGain(source.context, leaf, float(initial[leaf])), "Cannot set initial bank targets");
    require(owner->configureStemRamps(4), "Cannot configure real shared stem ramps");
    const auto peer = peerSignal();
    RenderCore core(source, AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, 1000) && core.setDeckGain(0, .5F) && core.setMasterGain(.75F) &&
        core.setCrossfader(.75F) && core.setDeckGain(1, 0) && core.setCueSelected(0, true) && core.setCueGain(.5F),
        "Cannot configure bank integration buses");
    render(core, 3); require(core.sourceFrame() == 1003, "Linear stem prefix did not advance before scratch");
    require(core.setPlaying(0, false) && core.beginScratch(0, 10), "Cannot pause/enter bank scratch"); readyWhileHeld(core);
    require(core.moveScratch(0, 10, 256, 256.0 / 48000), "Cannot warm bank platter"); warm(core, 1259);
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(core.setStemGain(0, leaf, float(targets[leaf])), "Scratch mode rejected render-side stem targets");
    require(core.moveScratch(0, 10, 736, .01), "Cannot continue bank unit motion");
    for (std::size_t frame = 0; frame < 4; ++frame) {
        const auto output = render(core, 1); std::array<double, 4> gains{};
        for (std::size_t leaf = 0; leaf < 4; ++leaf) gains[leaf] = initial[leaf] + (targets[leaf] - initial[leaf]) * double(frame + 1) / 4;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto raw = fixture.mixed(1259 + frame, channel, gains);
            require(near(output.master[channel], double(raw) * .5 * .5 * .75) && near(output.cue[channel], double(raw) * .5),
                    "Cached scratch mixing lost shared stem ramps, stereo roles or prefader cue");
        }
    }
    require(core.sourceFrame() == 1263 && !core.readFailed(0) && core.endScratch(0, 10) && !core.isPlaying(),
            "Bank scratch cursor/paused release failed");
    for (std::size_t leaf = 0; leaf < 4; ++leaf)
        require(core.stemGain(0, leaf) == targets[leaf], "Scratch end lost current stem gain target");
    require(core.setPlaying(0, true), "Cannot resume ordinary bank after scratch");
    const auto output = firstLinearFrame(core);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto raw = fixture.mixed(1263, channel, targets);
        require(near(output.master[channel], double(raw) * .5 * .5 * .75) && near(output.cue[channel], double(raw) * .5),
                "Bank exit replayed parked old surplus or reset shared gain ramps");
    }
    require(core.sourceFrame() == 1264 && !owner->errorCode(), "Ordinary bank continuation did not commit exact first frame");
}

void explicitCancellationAndTokens()
{
    Fixture fixture; auto owner = fixture.owner(false); require(owner->prime(10000, 2s), "Cannot prime cancellation fixture");
    const auto peer = peerSignal(); RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, 10000) && core.setPlaying(0, false) && core.beginScratch(0, 20), "Cannot enter paused scratch");
    readyWhileHeld(core);
    require(core.moveScratch(0, 20, 17, 17.0 / 48000), "Cannot move cancellation fixture"); warm(core, 10017);
    const auto generation = core.scratchState(0).generation, clock = core.frameClock(), peerFrame = core.sourceFrame(1);
    require(core.seek(0, core.sourceFrame()) && !core.scratchState(0).active && core.scratchState(0).generation > generation &&
        core.sourcePosition() == 10017 && core.frameClock() == clock && core.sourceFrame(1) == peerFrame &&
        !core.moveScratch(0, 20, 480, .01) && !core.beginScratch(0, 20), "Same-frame seek retained stale scratch motion/token");
    require(core.beginScratch(0, 21), "Fresh gesture after explicit seek rejected"); readyWhileHeld(core);
    const auto retryGeneration = core.scratchState(0).generation;
    require(core.retrySource(0) && !core.scratchState(0).active && core.scratchState(0).generation > retryGeneration &&
        core.sourcePosition() == 10017 && !core.moveScratch(0, 21, -480, .01), "Retry did not cancel the same-frame scratch generation");
    require(core.beginScratch(0, 22) && !core.isDeckSilentForReplacement(0) &&
        !core.replaceSource(0, AudioSource::fromMemory(peer)) && !core.endScratch(0, 21) &&
        !core.moveScratch(0, 22, 10, 0) && !core.moveScratch(0, 22, 10, std::numeric_limits<double>::quiet_NaN()) &&
        !core.beginScratch(2, 1) && !core.beginScratch(0, 0), "Active scratch lifetime/token/invalid-control guard failed");
    require(core.setPlaying(0, false) && !core.scratchState(0).active && !core.isPlaying(), "Explicit pause did not cancel held scratch");
}

void loopTempoGatesAndReverseFromEof()
{
    Fixture fixture(8192); auto owner = fixture.owner(false); require(owner->prime(1000, 2s), "Cannot prime interaction fixture");
    const auto peer = peerSignal(); RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, 1000) && core.loopIn(0), "Cannot set real prepared loop IN");
    const auto loop = core.loopState(0);
    require(!core.beginScratch(0, 1) && core.loopState(0).inSet && core.loopState(0).inFrame == loop.inFrame &&
        core.sourcePosition() == 1000, "Scratch rejection destroyed existing loop selection");
    require(core.seek(0, 1000) && core.beginScratch(0, 1), "Cannot scratch after clearing loop explicitly"); readyWhileHeld(core);
    require(!core.loopIn(0) && !core.loopOut(0) && !core.loopToggle(0) && !core.setTempoPercent(0, 2) &&
        !core.toggleMasterTempo(0) && core.scratchState(0).active && core.endScratch(0, 1), "Scratch failed reciprocal loop/tempo exclusion");
    require(core.setTempoPercent(0, 2), "Cannot start real prepared tempo for exclusion");
    const auto tempo = core.tempoState(0);
    require(tempo.active && !core.beginScratch(0, 2) && core.tempoState(0).active && core.tempoState(0).percent == 2 &&
        core.sourcePosition() == 1000, "Scratch rejection reset active tempo/configuration");
    require(core.resetTempo(0) && core.seek(0, fixture.frames) && core.setPlaying(0, false) && core.beginScratch(0, 2),
            "Cannot hold exact EOF after explicit tempo reset"); readyWhileHeld(core);
    render(core, 7);
    require(core.sourcePosition() == double(fixture.frames) && !core.isPlaying() && !core.readFailed(0), "Held EOF became finished or failed");
    require(core.moveScratch(0, 2, -256, 256.0 / 48000), "Cannot move backward from EOF"); warm(core, double(fixture.frames - 256));
    require(core.moveScratch(0, 2, -736, .01) && core.setDeckGain(1, 0), "Cannot continue exact reverse EOF motion");
    const auto reversed = render(core, 480);
    for (std::size_t frame = 0; frame < 480; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(near(reversed.master[frame * 2 + channel], fixture.pcm[0][(fixture.frames - 257 - frame) * 2 + channel]),
                    "Paused EOF reverse did not render exact preceding original stereo samples");
    require(core.sourceFrame() == fixture.frames - 736 && !core.isPlaying() && !core.readFailed(0), "EOF reverse clocks/direction failed");
}

struct GateState {
    struct stat identity{}; std::uint64_t offset = 0;
    std::mutex mutex; std::condition_variable changed;
    bool claimed = false, entered = false, released = false;
};
std::mutex gateMutex;
std::shared_ptr<GateState> activeGate;
class Gate {
public:
    Gate(int fd, std::uint64_t offset) : state_(std::make_shared<GateState>())
    {
        require(fstat(fd, &state_->identity) == 0, "Cannot identify blocked scratch source"); state_->offset = offset;
        const std::lock_guard lock(gateMutex); require(!activeGate, "Nested scratch read gate"); activeGate = state_;
    }
    ~Gate() { release(); }
    void entered()
    {
        std::unique_lock lock(state_->mutex);
        require(state_->changed.wait_for(lock, 2s, [&] { return state_->entered; }), "Real scratch worker did not reach gate");
    }
    void release()
    {
        { const std::lock_guard lock(state_->mutex); state_->released = true; } state_->changed.notify_all();
        const std::lock_guard lock(gateMutex); if (activeGate == state_) activeGate.reset();
    }
private: std::shared_ptr<GateState> state_;
};
void holdRead(int fd, std::uint64_t offset)
{
    std::shared_ptr<GateState> state;
    { const std::lock_guard lock(gateMutex); state = activeGate; }
    if (!state || offset != state->offset) return;
    struct stat actual{};
    if (fstat(fd, &actual) || actual.st_dev != state->identity.st_dev || actual.st_ino != state->identity.st_ino) return;
    std::unique_lock lock(state->mutex);
    if (state->claimed) return;
    state->claimed = state->entered = true; state->changed.notify_all();
    state->changed.wait(lock, [&] { return state->released; });
}
std::set<int> matchingDescriptors(const Fixture& fixture)
{
    std::set<int> result;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const int fd = std::stoi(entry.path().filename().string()); struct stat actual{};
        if (fstat(fd, &actual) == 0 && std::ranges::any_of(fixture.identities, [&](const auto& wanted) {
                return actual.st_dev == wanted.st_dev && actual.st_ino == wanted.st_ino;
            })) result.insert(fd);
    }
    return result;
}
void pendingMovementAndQuiescedCleanup()
{
    constexpr std::uint64_t window = PreparedScratchSource::windowFrames;
    Fixture fixture(window * 3); const auto originals = matchingDescriptors(fixture);
    const int fd = fileno(fixture.files[0].get());
    require(lseek(fd, 321, SEEK_SET) == 321, "Cannot set original offset");
    auto owner = fixture.owner(false); require(owner->prime(17, 2s), "Cannot prime real pending fixture");
    for (const int duplicate : matchingDescriptors(fixture))
        if (!originals.contains(duplicate)) require((fcntl(duplicate, F_GETFD) & FD_CLOEXEC) != 0, "Playback worker descriptor lacks CLOEXEC");
    const auto peer = peerSignal(); RenderCore core(owner->sourceRef(), AudioSource::fromMemory(peer));
    require(core.restorePrimedCursor(0, 17) && core.setPlaying(0, false) && core.beginScratch(0, 1), "Cannot ready first scratch window");
    readyWhileHeld(core); require(core.endScratch(0, 1), "Cannot end initial ready gesture");
    // Only scratch's centered-window first read matches this offset. Ordinary
    // PcmReadAhead starts at far, and loop/tempo workers have no active request.
    Gate gate(fd, (fixture.frames - window) * 8);
    const auto far = fixture.frames - 8192;
    require(core.seek(0, far) && core.beginScratch(0, 2), "Cannot start far pending scratch generation"); gate.entered();
    render(core, 23);
    require(core.sourcePosition() == double(far) && !core.isBuffering(0) && core.scratchState(0).pending &&
        !core.starvedFrames(0) && !core.readFailed(0), "Pending held zero was misreported as movement/starvation/failure");
    require(core.moveScratch(0, 2, 480, .01), "Cannot move while centered window is pending");
    const auto clock = core.frameClock(), peerFrame = core.sourceFrame(1), starved = core.starvedFrames(0);
    const auto unavailable = render(core, 32);
    require(core.sourcePosition() == double(far) && core.frameClock() == clock + 32 && core.sourceFrame(1) == peerFrame + 32 &&
        core.starvedFrames(0) == starved + 32 && core.isBuffering(0) && !core.readFailed(0) && !owner->errorCode(),
        "Unavailable moving PCM advanced source, stopped peer/output, or concealed starvation");
    for (std::size_t frame = 0; frame < 32; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(unavailable.master[frame * 2 + channel] == peer[(peerFrame + frame) * 2 + channel],
                    "Unavailable scratch leaked old-position PCM into master");
    const auto generation = core.scratchState(0).generation;
    require(core.seek(0, far) && !core.scratchState(0).active && core.scratchState(0).generation > generation &&
        !core.moveScratch(0, 2, -480, .01), "Same-frame cancellation retained a blocked old-generation gesture");
    gate.release(); // Always before any possible owner stop/join.
    require(core.setPlaying(0, true), "Cannot resume normal source after pending cancellation");
    const auto firstFrame = core.sourceFrame(); const auto resumed = firstLinearFrame(core);
    const auto deliveredPeer = core.sourceFrame(1) - 1;
    for (std::size_t channel = 0; channel < 2; ++channel)
        require(near(resumed.master[channel], double(fixture.pcm[0][firstFrame * 2 + channel]) +
            peer[deliveredPeer * 2 + channel]), "Cancelled pending scratch contaminated ordinary continuation");
    require(lseek(fd, 0, SEEK_CUR) == 321, "Real worker reads changed original shared FD offset");
    owner->stop(); owner->stop(); // Render is quiescent; core never called again.
    require(matchingDescriptors(fixture) == originals, "Joined playback retained loop/tempo/scratch/reader duplicates");
}
} // namespace

extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* out, size_t size, off_t offset)
{ holdRead(fd, std::uint64_t(offset)); return __real_pread(fd, out, size, offset); }
extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" ssize_t __wrap_pread64(int fd, void* out, size_t size, off64_t offset)
{ holdRead(fd, std::uint64_t(offset)); return __real_pread64(fd, out, size, offset); }
extern "C" ssize_t __real___pread_chk(int, void*, size_t, off_t, size_t);
extern "C" ssize_t __wrap___pread_chk(int fd, void* out, size_t size, off_t offset, size_t bound)
{ holdRead(fd, std::uint64_t(offset)); return __real___pread_chk(fd, out, size, offset, bound); }
extern "C" ssize_t __real___pread64_chk(int, void*, size_t, off64_t, size_t);
extern "C" ssize_t __wrap___pread64_chk(int fd, void* out, size_t size, off64_t offset, size_t bound)
{ holdRead(fd, std::uint64_t(offset)); return __real___pread64_chk(fd, out, size, offset, bound); }

int main()
{
    try {
        normalPlatterBusAndRelease();
        sharedStemGainsAndLinearContinuation();
        explicitCancellationAndTokens();
        loopTempoGatesAndReverseFromEof();
        pendingMovementAndQuiescedCleanup();
        std::cout << "audio scratch playback: 5 functional cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio scratch playback: " << error.what() << '\n';
        return 1;
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// Generated-PCM transport fixture; no audio device or model.
#include "audio/RenderCore.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace conflux::audio;
void require(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(what);
}
struct Source {
    static constexpr std::uint64_t frames = 20000;
    AudioScratchState scratch{0, 1, 0, 0, true, false, false};
    AudioLoopState loop{};
    AudioTempoState tempo{};
    int beginCalls = 0, endCalls = 0, seekCalls = 0;
    std::size_t available = 1024;
    double target = 0, anchor = 0;
    bool badPosition = false;
    static float left(double f) { return float(f / 32768.); }
    static float right(double f) { return -float(f / 16384.); }
    AudioSource ref() {
        AudioSource s;
        s.frames = frames;
        s.context = this;
        s.read = [](void *p, std::uint64_t frame, std::span<float> out) noexcept {
            auto &s = *static_cast<Source *>(p);
            auto n = std::min(s.available, out.size() / 2);
            if (s.scratch.active) {
                for (std::size_t i = 0; i < n; ++i) {
                    if (s.scratch.position == s.target)
                        out[i * 2] = out[i * 2 + 1] = 0;
                    else {
                        s.scratch.position += std::clamp(s.target - s.scratch.position, -1., 1.);
                        out[i * 2] = left(s.scratch.position);
                        out[i * 2 + 1] = right(s.scratch.position);
                    }
                }
                return AudioReadResult{n, false, true, s.badPosition ? -1 : s.scratch.position};
            }
            for (std::size_t i = 0; i < n; ++i) {
                out[i * 2] = left(frame + i);
                out[i * 2 + 1] = right(frame + i);
            }
            if (s.tempo.active)
                return AudioReadResult{n, false, true, frame ? double(frame - 1) : 0};
            return AudioReadResult{n, false};
        };
        s.discontinuity = [](void *p, std::uint64_t f) noexcept {
            auto &s = *static_cast<Source *>(p);
            ++s.seekCalls;
            s.scratch.active = false;
            s.scratch.position = double(f);
        };
        s.scratchState = [](const void *p) noexcept { return static_cast<const Source *>(p)->scratch; };
        s.scratchControl = [](void *p, AudioScratchCommand c, std::int64_t d, double, std::uint64_t f) noexcept {
            auto &s = *static_cast<Source *>(p);
            if (c == AudioScratchCommand::Begin) {
                ++s.beginCalls;
                s.scratch.active = true;
                s.anchor = s.target = s.scratch.position = double(f);
                return true;
            }
            if (c == AudioScratchCommand::Move) {
                s.target = std::clamp(s.anchor + double(d), 0., double(frames));
                return true;
            }
            ++s.endCalls;
            s.scratch.active = false;
            return true;
        };
        s.loopState = [](const void *p) noexcept { return static_cast<const Source *>(p)->loop; };
        s.loopControl = [](void *, AudioLoopCommand, std::uint64_t) noexcept { return true; };
        s.tempoState = [](const void *p) noexcept { return static_cast<const Source *>(p)->tempo; };
        s.tempoControl = [](void *, AudioTempoCommand, double, bool, std::uint64_t) noexcept { return true; };
        return s;
    }
};
std::vector<float> render(RenderCore &core, std::size_t n) {
    std::vector<float> out(n * 2);
    require(core.render(out), "Render rejected PCM");
    return out;
}
void pausedMotionAndTokens() {
    Source s;
    RenderCore c(s.ref());
    require(c.setPlaying(0, false) && c.seek(0, 100), "Setup failed");
    const auto clock = c.frameClock();
    require(c.beginScratch(0, 1), "Paused Begin rejected");
    require(!c.isPlaying() && c.scratchState(0).active, "Begin changed play request");
    auto held = render(c, 32);
    require(std::ranges::all_of(held, [](float x) { return x == 0; }), "Held DC emitted");
    require(c.sourceFrame() == 100 && c.frameClock() == clock + 32 && !c.isBuffering() && !c.starvedFrames(),
            "Held clocks failed");
    require(c.moveScratch(0, 1, 8, .01), "Forward rejected");
    auto f = render(c, 8);
    for (std::size_t i = 0; i < 8; ++i)
        require(f[i * 2] == Source::left(101 + i) && f[i * 2 + 1] == Source::right(101 + i), "Forward PCM failed");
    require(c.moveScratch(0, 1, -8, .01), "Reverse rejected");
    auto r = render(c, 8);
    for (std::size_t i = 0; i < 8; ++i)
        require(r[i * 2] == Source::left(107 - i) && r[i * 2 + 1] == Source::right(107 - i), "Reverse PCM failed");
    require(c.sourceFrame() == 100 && !c.readFailed(), "Valid reverse metadata rejected");
    require(c.endScratch(0, 1) && !c.isPlaying() && !c.scratchState(0).active, "Paused End resumed");
    require(c.endScratch(0, 1) && s.endCalls == 1, "Duplicate End repeated restore");
    require(c.beginScratch(0, 2) && !c.moveScratch(0, 1, 5, .01) && !c.endScratch(0, 1) && c.scratchState(0).active,
            "Old token reached new gesture");
    require(!c.moveScratch(0, 2, 5, std::numeric_limits<double>::quiet_NaN()) && !c.moveScratch(0, 2, 5, 0),
            "Invalid interval accepted");
    require(c.endScratch(0, 2) && !c.beginScratch(0, 1), "Old Begin reused");
    require(c.seek(0, Source::frames) && c.beginScratch(0, 3) && c.moveScratch(0, 3, -4, .01),
            "EOF reverse request rejected");
    auto tail = render(c, 4);
    for (std::size_t i = 0; i < 4; ++i)
        require(tail[i * 2] == Source::left(Source::frames - 1 - i), "EOF reverse emitted wrong PCM");
    require(c.sourceFrame() == Source::frames - 4 && !c.readFailed() && c.endScratch(0, 3) && !c.isPlaying(),
            "EOF reverse clock or release failed");
}
void peerCueAndMissing() {
    Source s;
    std::vector<float> peer(10000, .125F);
    RenderCore c(s.ref(), AudioSource::fromMemory(peer));
    require(c.setPlaying(0, false) && c.seek(0, 100) && c.setCueSelected(0, true) && c.beginScratch(0, 1) &&
                c.moveScratch(0, 1, 20, .01),
            "Peer setup failed");
    std::array<float, 16> master{}, cue{};
    require(c.renderBuses(master, cue), "Bus rejected");
    for (std::size_t i = 0; i < 8; ++i) {
        require(cue[i * 2] == Source::left(101 + i) && cue[i * 2 + 1] == Source::right(101 + i), "Cue PCM failed");
        require(master[i * 2] == float(double(cue[i * 2]) + .125), "Peer mix failed");
    }
    const auto cursor = c.sourceFrame(), peerCursor = c.sourceFrame(1);
    s.available = 0;
    render(c, 16);
    require(c.sourceFrame() == cursor && c.sourceFrame(1) == peerCursor + 16 && c.isBuffering() &&
                c.starvedFrames() == 16 && !c.readFailed(),
            "Missing PCM changed cursor/peer/health");
    s.available = 1024;
    render(c, 8);
    require(!c.isBuffering() && c.sourceFrame() > cursor, "Recovery failed");
    s.badPosition = true;
    const auto accepted = c.sourceFrame();
    render(c, 8);
    require(c.readFailed() && c.sourceFrame() == accepted, "Invalid negative position accepted");
}
void modesAndReplacement() {
    Source s;
    RenderCore c(s.ref());
    c.setPlaying(0, false);
    c.seek(0, 100);
    s.loop.inSet = true;
    require(!c.beginScratch(0, 1) && s.beginCalls == 0, "Stored loop changed");
    s.loop = {};
    s.tempo.active = true;
    require(!c.beginScratch(0, 1) && s.beginCalls == 0, "Tempo changed");
    s.tempo = {};
    c.setCueFrame(0, 99);
    require(c.cuePress(0) && c.isCueHeld() && !c.isCuePreviewing(), "Held CUE setup failed");
    require(!c.beginScratch(0, 1) && c.isCueHeld(), "Held CUE overridden");
    c.cueRelease(0);
    require(c.beginScratch(0, 1) && !c.loopIn(0) && !c.setTempoPercent(0, 1), "Scratch mode gate failed");
    const auto seeks = s.seekCalls;
    require(c.seek(0, c.sourceFrame()) && !c.scratchState(0).active && s.seekCalls == seeks + 1,
            "Same-frame cancellation failed");
    require(c.beginScratch(0, 2) && c.retrySource(0) && !c.scratchState(0).active, "Retry cancellation failed");
    require(c.beginScratch(0, 3) && !c.isDeckSilentForReplacement(0), "Paused scratch retired");
    std::vector<float> next(128, .25F);
    require(!c.replaceSource(0, AudioSource::fromMemory(next)), "Active scratch replaced");
    require(c.setPlaying(0, false) && !c.scratchState(0).active && c.isDeckSilentForReplacement(0),
            "Pause failed to release");
    require(c.replaceSource(0, AudioSource::fromMemory(next)) && !c.moveScratch(0, 3, 1, .01) && !c.endScratch(0, 3),
            "Retired token reached source");
    require(!c.beginScratch(0, 4), "Unsupported memory source accepted");
}
void guards() {
    Source s;
    auto broken = s.ref();
    broken.scratchState = nullptr;
    bool caught = false;
    try {
        RenderCore invalid(broken);
    } catch (const std::invalid_argument &) {
        caught = true;
    }
    require(caught, "Incomplete hooks accepted");
    Source tempo;
    RenderCore c(tempo.ref());
    c.seek(0, 100);
    tempo.tempo.active = true;
    render(c, 1);
    require(c.readFailed() && c.sourceFrame() == 100, "Tempo monotonic guard weakened");
}
int main() {
    try {
        pausedMotionAndTokens();
        peerCueAndMissing();
        modesAndReplacement();
        guards();
        std::cout << "scratch transport: 4 generated CPU cases passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

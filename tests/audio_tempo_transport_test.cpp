// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using conflux::audio::AudioReadResult;
using conflux::audio::AudioSource;
using conflux::audio::AudioTempoCommand;
using conflux::audio::AudioTempoState;
using conflux::audio::RenderCore;
namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
// Deliberately independent synthetic processed source: six output frames for
// five nominal source frames, with a controlled unavailable prefix and tail.
// Real DSP, reader, queue and bank behavior have separate integration tests.
struct Source {
    AudioTempoState state{0, 0, 0, 0, true};
    std::size_t consumed = 0, budget = 0, reads = 0, seeks = 0;
    double position = 0;
    int malformed = 0;
    AudioSource ref()
    {
        AudioSource source{5, nullptr, this,
            [](void* p, std::uint64_t, std::span<float> output) noexcept {
                auto& self = *static_cast<Source*>(p);
                ++self.reads;
                const auto count = std::min({output.size() / 2, self.budget, 6 - self.consumed});
                for (std::size_t i = 0; i < count; ++i) {
                    output[i * 2] = float(self.consumed + i + 1);
                    output[i * 2 + 1] = -output[i * 2];
                }
                self.budget -= count;
                self.consumed += count;
                self.position = std::min(5.0, double(self.consumed) * 1.5);
                self.state.finished = self.consumed == 6;
                AudioReadResult result{count, false, true, self.position, self.state.finished};
                if (self.malformed == 1) result.nominalPosition = std::numeric_limits<double>::quiet_NaN();
                if (self.malformed == 2) result.nominalPosition = 5.01;
                if (self.malformed == 3) result.positionValid = false;
                if (self.malformed == 4) result.nominalPosition = 0;
                if (self.malformed == 5) result.nominalPosition = 1.5;
                return result;
            },
            [](void* p, std::uint64_t frame) noexcept {
                auto& self = *static_cast<Source*>(p);
                ++self.seeks; ++self.state.generation;
                self.position = double(frame);
                self.consumed = std::size_t(frame);
                self.state.finished = false;
            }};
        source.tempoControl = [](void* p, AudioTempoCommand command, double percent, bool lock,
                                  std::uint64_t) noexcept {
            auto& self = *static_cast<Source*>(p);
            if (command == AudioTempoCommand::Reset) {
                self.state.active = false;
                ++self.state.generation;
                self.state.percent = 0;
            } else {
                if (!self.state.active || self.state.keyLock != lock) ++self.state.generation;
                self.state.active = true;
                self.state.percent = percent;
                self.state.keyLock = lock;
            }
            return true;
        };
        source.tempoState = [](const void* p) noexcept { return static_cast<const Source*>(p)->state; };
        return source;
    }
};

void independentClocksPauseStarvationAndTail()
{
    Source source;
    const std::array<float, 24> peer{.25F,.5F,.25F,.5F,.25F,.5F,.25F,.5F,.25F,.5F,.25F,.5F,
                                   .25F,.5F,.25F,.5F,.25F,.5F,.25F,.5F,.25F,.5F,.25F,.5F};
    RenderCore core(source.ref(), AudioSource::fromMemory(peer));
    require(core.setTempoPercent(0, -6) && core.setCueSelected(0, true), "tempo or cue setup rejected");
    source.budget = 1;
    std::array<float, 8> master{}, cue{};
    require(core.renderBuses(master, cue) && core.sourcePosition(0) == 1.5 && core.sourceFrame(0) == 1 &&
            core.sourceFrame(1) == 4 && core.frameClock() == 4 && core.isBuffering(0) &&
            core.starvedFrames(0) == 3 && source.reads == 1, "fractional starvation advanced wrong clock");
    require(master == std::array<float,8>{1.25F,-.5F,.25F,.5F,.25F,.5F,.25F,.5F} &&
            cue == std::array<float,8>{1,-1,0,0,0,0,0,0}, "starvation/peer/pre-fader cue samples differ");
    const auto beforeReads = source.reads;
    require(core.setPlaying(0, false) && core.renderBuses(master, cue) && source.reads == beforeReads &&
            core.sourcePosition(0) == 1.5 && core.sourceFrame(1) == 8 && core.frameClock() == 8,
            "pause consumed processed source or paused peer clock");
    source.budget = 5;
    require(core.setPlaying(0, true) && core.setCrossfader(1) && core.renderBuses(master, cue) &&
            core.sourcePosition(0) == 5 && core.sourceFrame(0) == 5 && !source.state.finished &&
            core.starvedFrames(0) == 3 && !core.isBuffering(0), "muted processed source did not reach nominal EOF");
    require(cue == std::array<float,8>{2,-2,3,-3,4,-4,5,-5}, "processed cue was crossfaded or tail cropped");
    require(core.setCrossfader(.5F) && core.renderBuses(master, cue) &&
            master == std::array<float,8>{6,-6,0,0,0,0,0,0} && source.state.finished &&
            core.sourcePosition(0) == 5 && !core.isBuffering(0) && core.starvedFrames(0) == 3 &&
            core.frameClock() == 16, "actual tail/EOF suffix classified as starvation or clipped");
    const auto finishedReads = source.reads;
    require(core.render(master) && source.reads == finishedReads && core.frameClock() == 20 &&
            core.isPlaying(0) && !core.isBuffering(0), "consumed EOF kept reading or changed play request");
}

void commandsAndDiscontinuities()
{
    Source source;
    RenderCore core(source.ref());
    require(core.setTempoPercent(0, 6), "initial tempo rejected");
    const auto generation = core.tempoState(0).generation;
    require(core.seek(0, 0) && source.seeks == 1 && core.tempoState(0).generation == generation + 1,
            "same-frame seek failed to invalidate exactly once");
    require(core.retrySource(0) && source.seeks == 2, "retry failed to invalidate processed cursor");
    require(core.setPlaying(0, false) && core.cuePress(0) && source.seeks == 3 && core.isCuePreviewing(0) &&
            core.cueRelease(0) && source.seeks == 4, "same-frame CUE did not invalidate each discontinuity once");
    const auto before = core.tempoState(0);
    for (const auto value : {-6.01, 6.01, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()})
        require(!core.setTempoPercent(0, value), "invalid tempo accepted");
    require(!core.setTempoPercent(2, 1) && core.tempoState(0).generation == before.generation &&
            core.tempoState(0).percent == before.percent, "rejected command mutated tempo");
    RenderCore::Command command;
    command.type = RenderCore::Command::Type::TempoPercent; command.tempoPercent = -2.5;
    require(core.apply(command) && core.tempoState(0).percent == -2.5, "tempo command lost percent");
    command.type = RenderCore::Command::Type::MasterTempo;
    require(core.apply(command) && core.tempoState(0).keyLock, "Master Tempo command not applied");
    command.type = RenderCore::Command::Type::TempoReset;
    require(core.apply(command) && !core.tempoState(0).active && core.tempoState(0).percent == 0 &&
            core.tempoState(0).keyLock, "reset lost unity or key-lock selection");
    const std::array<float,2> memory{};
    RenderCore plain(memory);
    require(!plain.setTempoPercent(0, 1) && !plain.toggleMasterTempo(0) && !plain.resetTempo(0) &&
            !plain.tempoState(0).supported, "memory source unexpectedly supported processing");
}

void malformedMetadataFailsClosed()
{
    for (int mode = 1; mode <= 5; ++mode) {
        Source source;
        RenderCore core(source.ref());
        require(core.setTempoPercent(0, 6), "malformed setup rejected");
        source.budget = 1;
        std::array<float,2> output{};
        if (mode == 4) require(core.render(output) && core.sourcePosition(0) == 1.5, "regression setup failed");
        const auto position = core.sourcePosition(0);
        source.budget = mode == 5 ? 0 : 1; source.malformed = mode;
        require(core.render(output) && output == std::array<float,2>{0,0} && core.readFailed(0) &&
                core.sourcePosition(0) == position && core.isBuffering(0), "invalid position escaped fail-closed render");
    }
    Source source;
    auto halfHook = source.ref(); halfHook.tempoState = nullptr;
    bool rejected = false;
    try { RenderCore core(halfHook); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "incomplete optional tempo callback pair accepted");
}
}
int main()
{
    try {
        independentClocksPauseStarvationAndTail();
        commandsAndDiscontinuities();
        malformedMetadataFailsClosed();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    std::cout << "Tempo transport clocks, discontinuities, EOF tail and invalid metadata passed\n";
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/LiveSession.hpp"
#include "audio/PreparedScratchSource.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
using conflux::audio::LiveSession;
using conflux::audio::RenderCore;

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

template<typename Function>
bool rejects(Function&& function, const char* message)
{
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return require(false, message);
}

bool preparedTempoSurvivesQuiescedReconstruction()
{
    constexpr std::uint64_t frames = 16384, initialFrame = 17;
    std::vector<float> pcm(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        pcm[frame * 2] = float(int(frame % 67) - 33) / 64.F;
        pcm[frame * 2 + 1] = float(int(frame % 43) - 21) / 32.F;
    }
    const auto closeFile = [](std::FILE* file) { if (file) std::fclose(file); };
    const auto raw = std::unique_ptr<std::FILE, decltype(closeFile)>(std::tmpfile(), closeFile);
    if (!raw || std::fwrite(pcm.data(), sizeof(float), pcm.size(), raw.get()) != pcm.size() ||
        std::fflush(raw.get()) != 0)
        throw std::runtime_error("Cannot prepare tempo session lifecycle PCM");
    LiveSession::PlaybackSources sources{};
    sources[0] = std::make_unique<conflux::audio::PreparedPlayback>(fileno(raw.get()), frames);
    auto* const owner = sources[0].get();
    const auto source = owner->sourceRef();
    if (!source.tempoControl || !source.tempoState ||
        !source.tempoControl(source.context, conflux::audio::AudioTempoCommand::Set, 6, true, initialFrame))
        throw std::runtime_error("Cannot select prepared session tempo");
    const auto selectedGeneration = source.tempoState(source.context).generation;
    LiveSession::InitialState initial;
    initial.sourceFrame[0] = initialFrame;
    initial.cueFrame[0] = 7;
    initial.playing[0] = true;
    initial.deckGain[0] = .5F;
    initial.cueSelected[0] = true;
    initial.crossfader = .25F;
    constexpr std::array<std::array<float, 3>, 2> eqTargets{{{-18.F, 4.F, -7.F}, {6.F, -26.F, 2.F}}};
    initial.eqGainDb = eqTargets;
    LiveSession first(std::move(sources), initial);
    auto validUnconsumed = [&](const LiveSession::State& state) {
        const auto tempo = state.tempo[0];
        return !state.ready && state.outputFrame == 0 && state.callbacks == 0 &&
            state.sourceFrame[0] == initialFrame && state.sourcePosition[0] == double(initialFrame) &&
            state.sourceFrames[0] == frames && state.playing[0] && !state.sourceReadError[0] &&
            state.cueFrame[0] == initial.cueFrame[0] && state.cueSelected[0] &&
            state.deckGain[0] == .5F && state.crossfader == .25F && state.eqGainDb == eqTargets &&
            tempo.supported && tempo.active && tempo.pending && !tempo.finished && !tempo.errorCode &&
            tempo.percent == 6 && tempo.keyLock && tempo.appliedPercent == 0 && !state.tempo[1].supported;
    };
    const auto primed = first.stoppedState();
    bool passed = require(!sources[0] && validUnconsumed(primed) &&
                          primed.tempo[0].generation > selectedGeneration,
                          "session prime lost prepared tempo/EQ targets or consumed prefetched audio");
    auto retained = first.stopAndTakePlaybacks();
    const auto captured = first.stoppedState();
    passed &= require(retained.active[0].get() == owner && !retained.replacement && validUnconsumed(captured) &&
                      captured.tempo[0].generation == primed.tempo[0].generation,
                      "quiesced extraction stopped its owner, changed tempo/EQ targets or advanced source/output position");
    initial.sourceFrame = captured.sourceFrame;
    initial.eqGainDb = captured.eqGainDb;
    LiveSession rebuilt(std::move(retained.active), initial);
    const auto reprime = rebuilt.stoppedState();
    passed &= require(validUnconsumed(reprime) && reprime.tempo[0].generation > captured.tempo[0].generation,
                      "reconstructed session lost tempo/EQ targets, reused queued generation or consumed its readiness prefix");
    auto finalOwners = rebuilt.stopAndTakePlaybacks();
    passed &= require(finalOwners.active[0].get() == owner && validUnconsumed(rebuilt.stoppedState()) &&
                      !finalOwners.active[0]->errorCode(), "reconstruction failed to retain its healthy whole owner");
    return passed;
}

bool scratchFailureReportsOnlyWhileActive()
{
    using namespace conflux::audio;
    using namespace std::chrono_literals;
    // No server or callback: the source hooks and final snapshot have one
    // quiescent owner, as during a whole-owner session handoff.
    constexpr auto frames = PreparedScratchSource::windowFrames * 2;
    bool passed = true;
    for (const bool remainActive : {false, true}) {
        const auto closeFile = [](std::FILE* file) { if (file) std::fclose(file); };
        const auto raw = std::unique_ptr<std::FILE, decltype(closeFile)>(std::tmpfile(), closeFile);
        const std::vector<float> pcm(frames * 2, .125F);
        if (!raw || std::fwrite(pcm.data(), sizeof(float), pcm.size(), raw.get()) != pcm.size() ||
            std::fflush(raw.get()) != 0)
            throw std::runtime_error("Cannot prepare scratch health PCM");
        LiveSession::PlaybackSources sources{};
        sources[0] = std::make_unique<PreparedPlayback>(fileno(raw.get()), frames);
        auto* const owner = sources[0].get();
        LiveSession session(std::move(sources), {});
        const auto source = owner->sourceRef();
        std::array<float, 128> output{};
        const auto readyDeadline = std::chrono::steady_clock::now() + 2s;
        while (source.scratchState(source.context).pending && std::chrono::steady_clock::now() < readyDeadline) {
            (void)source.read(source.context, 0, output);
            std::this_thread::sleep_for(1ms);
        }
        if (source.scratchState(source.context).pending || source.scratchState(source.context).errorCode)
            throw std::runtime_error("Cannot prime scratch health window");
        // Force a genuinely obsolete file identity, then request a different
        // scratch window. The ordinary reader's healthy primed PCM is retained.
        const float replacement = .25F;
        if (::pwrite(fileno(raw.get()), &replacement, sizeof(replacement),
                     off_t(pcm.size() * sizeof(float) - sizeof(float))) != sizeof(replacement) ||
            !source.scratchControl(source.context, AudioScratchCommand::Begin, 0, 0,
                                   PreparedScratchSource::windowFrames + 4096))
            throw std::runtime_error("Cannot start scratch identity-failure check");
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!source.scratchState(source.context).errorCode && std::chrono::steady_clock::now() < deadline) {
            (void)source.read(source.context, 0, output);
            std::this_thread::sleep_for(1ms);
        }
        const auto scratch = source.scratchState(source.context);
        if (!scratch.active || !scratch.errorCode || owner->errorCode())
            throw std::runtime_error("Scratch identity error did not remain isolated from ordinary playback");
        if (!remainActive && !source.scratchControl(source.context, AudioScratchCommand::End, 0, 0, 0))
            throw std::runtime_error("Cannot end failed scratch gesture");
        const auto retained = session.stopAndTakePlaybacks();
        const auto state = session.stoppedState();
        passed &= require(retained.active[0].get() == owner && state.scratch[0].errorCode == scratch.errorCode &&
                          state.scratch[0].active == remainActive && state.sourceReadError[0] == remainActive &&
                          !state.sourceReadErrorObserved[0] && !state.callbacks,
                          "Snapshot hid an active scratch error or poisoned inactive optional scratch health");
    }
    return passed;
}

class PrivateRuntime {
public:
    PrivateRuntime()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "conflux-live-session-test-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created)
            throw std::runtime_error("Cannot create private runtime directory");
        path_ = created;
        for (std::size_t index = 0; index < names_.size(); ++index)
            if (const auto* current = std::getenv(names_[index]))
                previous_[index] = current;
        setenv(names_[0], path_.c_str(), 1);
        setenv(names_[1], path_.c_str(), 1);
        setenv(names_[2], "conflux-test-missing-server", 1);
    }
    ~PrivateRuntime()
    {
        for (std::size_t index = 0; index < names_.size(); ++index)
            if (previous_[index])
                setenv(names_[index], previous_[index]->c_str(), 1);
            else
                unsetenv(names_[index]);
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
private:
    const std::array<const char*, 3> names_{"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE"};
    std::array<std::optional<std::string>, 3> previous_;
    std::filesystem::path path_;
};
} // namespace

int main()
{
    try {
        bool passed = true;
        LiveSession empty;
        auto state = empty.stoppedState();
        passed &= require(!state.ready && state.outputFrame == 0 && state.sourceFrames == std::array<std::uint64_t, 2>{} &&
                          state.sourceGeneration == std::array<std::uint64_t, 2>{} &&
                          state.playing == std::array<bool, 2>{} && state.masterGain == 1 && state.crossfader == .5F,
                          "empty session did not start in stopped silence");
        RenderCore::Command play{RenderCore::Command::Type::Play, 0, 0, 1, 99};
        passed &= require(!empty.submit(play) && !empty.pollState(state) && empty.error().empty(),
                          "stopped session accepted transport or invented a snapshot/error");
        RenderCore::Command crossfader;
        crossfader.type = RenderCore::Command::Type::Crossfader;
        crossfader.gain = 1.F;
        passed &= require(!empty.submit(crossfader) && empty.stoppedState().crossfader == .5F &&
                          empty.stoppedState().applied == 0 && empty.stoppedState().rejected == 0,
                          "stopped session accepted a crossfader command or changed its default target");
        const auto routes = empty.routingSnapshot();
        passed &= require(routes.playbackPorts.empty() && !routes.masterReady && !routes.cueReady &&
                          routes.cueStatus == conflux::audio::CueRouteStatus::Disabled &&
                          !state.cueReady && state.graphRate == 0 && state.meters.blockFrames == 0 &&
                          empty.stoppedTimingSummary().counters.callbacks == 0,
                          "stopped session invented a route, meter block or timing observation");
        passed &= require(empty.requestRecorder(nullptr) && empty.recorderChangePending() &&
                          !empty.requestRecorder(nullptr) && !empty.recorderAttached(),
                          "stopped recorder change did not enforce a single pending acknowledgement");
        empty.stop();
        empty.stop();
        passed &= require(!empty.recorderChangePending() && !empty.recorderAttached(),
                          "stopping did not quiesce a pending recorder change");

        // A rejected handoff must retain the primed worker and its owner. This
        // uses only an anonymous regular file; no output server is contacted.
        const auto closeFile = [](std::FILE* file) { std::fclose(file); };
        const auto raw = std::unique_ptr<std::FILE, decltype(closeFile)>(std::tmpfile(), closeFile);
        const std::array<float, 4> replacementPcm{.125F, -.25F, .5F, -.75F};
        if (!raw || std::fwrite(replacementPcm.data(), sizeof(float), replacementPcm.size(), raw.get()) != replacementPcm.size() ||
            std::fflush(raw.get()) != 0)
            throw std::runtime_error("Cannot prepare private handoff PCM");
        auto candidate = std::make_unique<conflux::audio::PreparedPlayback>(fileno(raw.get()), replacementPcm.size() / 2);
        if (!candidate->prime(0, std::chrono::seconds(1)))
            throw std::runtime_error("Cannot prime private handoff PCM");
        auto* const candidateIdentity = candidate.get();
        passed &= require(!empty.requestSourceReplacement(0, candidate, 1) && candidate.get() == candidateIdentity &&
                          !empty.sourceReplacementPending() && !empty.takeSourceReplacementResult(),
                          "stopped session stole a rejected candidate or invented completion");
        empty.stop();
        passed &= require(candidate.get() == candidateIdentity && !candidate->errorCode(),
                          "repeated stopped-session cleanup changed rejected reader ownership");
        candidate.reset();
        LiveSession::PlaybackSources bankSources{};
        const int bankFd = fileno(raw.get());
        bankSources[0] = std::make_unique<conflux::audio::PreparedPlayback>(
            std::array<int, 4>{bankFd, bankFd, bankFd, bankFd}, replacementPcm.size() / 2);
        LiveSession::InitialState bankInitial;
        bankInitial.sourceFrame[0] = 1;
        bankInitial.cueFrame[0] = 1;
        bankInitial.cueSelected[0] = true;
        bankInitial.stemGain[0] = {.25F, 0.F, .75F, 1.F};
        bankInitial.crossfader = .875F;
        LiveSession bankRestored(std::move(bankSources), bankInitial);
        const auto bankState = bankRestored.stoppedState();
        passed &= require(!bankSources[0] && bankState.sourceFrames[0] == 2 && bankState.sourceFrame[0] == 1 &&
                          bankState.cueFrame[0] == 1 && bankState.cueSelected[0] && !bankState.sourceReadError[0],
                          "whole-bank session did not own/prime/restore the common source position");
        passed &= require(bankState.stemBank == std::array<bool, 2>{true, false} &&
                          bankState.stemGain[0] == bankInitial.stemGain[0] &&
                          bankState.stemGain[1] == std::array<float, 4>{} && bankState.crossfader == .875F,
                          "bank snapshot did not restore authoritative stem targets or invented normal stem controls");
        bankRestored.stop(); bankRestored.stop();
        passed &= require(!bankRestored.stoppedState().sourceReadError[0] &&
                          bankRestored.stoppedState().crossfader == .875F,
                          "stopping a healthy bank changed its crossfader or invented a reader error");

        passed &= preparedTempoSurvivesQuiescedReconstruction();
        passed &= scratchFailureReportsOnlyWhileActive();

        const std::array<float, 4> a{.25F, -.5F, .75F, -.25F};
        const std::array<float, 6> b{-.25F, .5F, -.75F, .25F, -.125F, .125F};
        LiveSession::InitialState initial;
        initial.sourceFrame = {1, 2};
        initial.cueFrame = {0, 1};
        initial.playing = {true, false};
        initial.deckGain = {.5F, .25F};
        initial.masterGain = .75F;
        initial.crossfader = .125F;
        initial.cueSelected = {true, false};
        initial.cueGain = .625F;
        LiveSession restored(a, b, initial);
        passed &= require(restored.stoppedState().stemBank == std::array<bool, 2>{} &&
                          restored.stoppedState().stemGain == std::array<std::array<float, 4>, 2>{},
                          "normal session invented bank capabilities or gain targets");
        auto invalidStems = initial;
        invalidStems.stemGain[0][0] = .5F;
        passed &= rejects([&] { LiveSession invalid(a, b, invalidStems); },
                          "normal source silently accepted initial stem targets");
        invalidStems = initial;
        invalidStems.stemGain[1][3] = std::numeric_limits<float>::quiet_NaN();
        passed &= rejects([&] { LiveSession invalid(a, b, invalidStems); },
                          "nonfinite initial stem target was accepted");
        LiveSession::InitialState unloadedStems;
        unloadedStems.stemGain[1][2] = .5F;
        passed &= rejects([&] { LiveSession invalid({}, {}, unloadedStems); },
                          "unloaded source silently accepted initial stem targets");
        state = restored.stoppedState();
        passed &= require(state.sourceFrame == initial.sourceFrame && state.sourceFrames == std::array<std::uint64_t, 2>{2, 3} &&
                          state.playing == initial.playing && state.deckGain == initial.deckGain &&
                          state.cueFrame == initial.cueFrame && !state.cueHeld[0] && !state.cueHeld[1] &&
                          !state.cuePreview[0] && !state.cuePreview[1] &&
                          state.masterGain == initial.masterGain && state.crossfader == initial.crossfader &&
                          state.cueGain == initial.cueGain &&
                          state.cueSelected == initial.cueSelected && state.outputFrame == 0 && state.callbacks == 0,
                          "initial source positions/gains/playing state were not restored");
        restored.stop();
        passed &= require(restored.stoppedState().sourceFrame == initial.sourceFrame &&
                          restored.stoppedState().crossfader == initial.crossfader && !restored.ready(),
                          "stopped session changed restored source positions or crossfader target");
        // Rebuilding output from a captured stopped state must retain the
        // global target without altering either deck's cue or gain settings.
        auto resumedInitial = initial;
        resumedInitial.crossfader = restored.stoppedState().crossfader;
        LiveSession resumed(a, b, resumedInitial);
        const auto resumedState = resumed.stoppedState();
        passed &= require(resumedState.crossfader == .125F && resumedState.masterGain == initial.masterGain &&
                          resumedState.deckGain == initial.deckGain && resumedState.cueGain == initial.cueGain &&
                          resumedState.cueSelected == initial.cueSelected && resumedState.cueFrame == initial.cueFrame,
                          "session reconstruction lost the crossfader or changed independent deck/cue targets");
        LiveSession bOnly({}, b);
        state = bOnly.stoppedState();
        passed &= require(state.sourceFrames == std::array<std::uint64_t, 2>{0, 3} &&
                          state.deckGain == std::array<float, 2>{0, 1} && !state.playing[1],
                          "B-only session did not retain unloaded A and initially paused B");

        auto invalid = initial;
        invalid.sourceFrame[0] = 3;
        passed &= rejects([&] { LiveSession rejected(a, b, invalid); }, "initial seek beyond EOF was accepted");
        invalid = initial;
        invalid.cueFrame[0] = 3;
        passed &= rejects([&] { LiveSession rejected(a, b, invalid); }, "initial cue beyond EOF was accepted");
        invalid = {};
        invalid.cueFrame[1] = 1;
        passed &= rejects([&] { LiveSession rejected(a, {}, invalid); }, "unloaded initial cue was accepted");
        invalid = {};
        invalid.cueSelected[1] = true;
        passed &= rejects([&] { LiveSession rejected(a, {}, invalid); }, "unloaded initial cue selection was accepted");
        invalid = initial;
        invalid.playing[1] = true;
        passed &= rejects([&] { LiveSession rejected(a, {}, invalid); }, "unloaded initial deck was accepted playing");
        for (std::size_t deck = 0; deck < 2; ++deck) {
            for (std::size_t band = 0; band < 3; ++band) {
                for (const auto gain : {std::nextafter(-26.F, -std::numeric_limits<float>::infinity()),
                                       std::nextafter(6.F, std::numeric_limits<float>::infinity()),
                                       std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                       std::numeric_limits<float>::quiet_NaN()}) {
                    invalid = {};
                    invalid.eqGainDb[deck][band] = gain;
                    passed &= rejects([&] { LiveSession rejected(a, {}, invalid); },
                                      "invalid initial EQ target was accepted on a loaded or unloaded channel");
                }
            }
        }
        for (const auto gain : {-.1F, 1.1F, std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            invalid = initial;
            invalid.deckGain[0] = gain;
            passed &= rejects([&] { LiveSession rejected(a, b, invalid); }, "invalid initial deck gain was accepted");
            invalid = initial;
            invalid.masterGain = gain;
            passed &= rejects([&] { LiveSession rejected(a, b, invalid); }, "invalid initial master gain was accepted");
            invalid = initial;
            invalid.cueGain = gain;
            passed &= rejects([&] { LiveSession rejected(a, b, invalid); }, "invalid initial cue gain was accepted");
            invalid = initial;
            invalid.crossfader = gain;
            passed &= rejects([&] { LiveSession rejected(a, b, invalid); }, "invalid initial crossfader was accepted");
        }
        for (const auto position : {0.F, .5F, 1.F}) {
            auto endpointInitial = initial;
            endpointInitial.crossfader = position;
            LiveSession endpoint(a, b, endpointInitial);
            const auto endpointState = endpoint.stoppedState();
            passed &= require(endpointState.crossfader == position && endpointState.deckGain == initial.deckGain &&
                              endpointState.masterGain == initial.masterGain && endpointState.cueGain == initial.cueGain &&
                              endpointState.cueSelected == initial.cueSelected && endpointState.cueFrame == initial.cueFrame,
                              "valid crossfader target was clamped or changed independent gain/cue state");
        }
        const std::array<float, 1> incomplete{0};
        passed &= rejects([&] { LiveSession rejected(incomplete); }, "incomplete PCM was accepted");
        const std::array<float, 2> nonfinite{0, std::numeric_limits<float>::infinity()};
        passed &= rejects([&] { LiveSession rejected(nonfinite); }, "nonfinite PCM was accepted");

        // No host PipeWire socket or hardware is reachable from this private
        // runtime. Exercise immediate/asynchronous failure and repeated teardown.
        PrivateRuntime privateRuntime;
        LiveSession unavailable(a, b, initial);
        std::string error;
        const bool started = unavailable.start(error);
        if (started) {
            passed &= rejects([&] { (void)unavailable.stoppedTimingSummary(); },
                              "running session allowed a percentile scan of mutable timing storage");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
            while (error.empty() && !unavailable.ready() && std::chrono::steady_clock::now() < deadline) {
                error = unavailable.error();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        passed &= require(!error.empty() && !unavailable.ready(), "unreachable output did not report startup failure");
        unavailable.stop();
        unavailable.stop();
        state = unavailable.stoppedState();
        passed &= require(!unavailable.submit(play) && !state.ready && state.outputFrame == 0 &&
                          !unavailable.submit(crossfader) && state.crossfader == initial.crossfader &&
                          unavailable.stoppedState().crossfader == initial.crossfader &&
                          state.sourceFrame == initial.sourceFrame && state.applied == 0 && state.rejected == 0,
                          "failed/stopped output changed source state or accepted transport");
        passed &= require(unavailable.stoppedTimingSummary().counters.callbacks == 0,
                          "unreachable playback invented process callbacks");
        LiveSession discovery;
        error.clear();
        if (discovery.startDiscovery(error)) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
            while (error.empty() && std::chrono::steady_clock::now() < deadline) {
                error = discovery.error();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        passed &= require(!error.empty() && !discovery.ready() && !discovery.routingSnapshot().cueReady,
                          "unreachable discovery mode became ready or hid its failure");
        discovery.stop();
        passed &= require(discovery.stoppedState().callbacks == 0 &&
                          discovery.stoppedTimingSummary().counters.callbacks == 0,
                          "inactive routing discovery advanced an audio callback");
        std::cout << "Live session initialization, validation and private-server failure checks completed\n";
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

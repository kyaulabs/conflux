// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/LiveSession.hpp"

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
                          state.playing == std::array<bool, 2>{} && state.masterGain == 1,
                          "empty session did not start in stopped silence");
        RenderCore::Command play{RenderCore::Command::Type::Play, 0, 0, 1, 99};
        passed &= require(!empty.submit(play) && !empty.pollState(state) && empty.error().empty(),
                          "stopped session accepted transport or invented a snapshot/error");
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
        auto candidate = std::make_unique<conflux::audio::PcmReadAhead>(fileno(raw.get()), replacementPcm.size() / 2);
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

        const std::array<float, 4> a{.25F, -.5F, .75F, -.25F};
        const std::array<float, 6> b{-.25F, .5F, -.75F, .25F, -.125F, .125F};
        LiveSession::InitialState initial;
        initial.sourceFrame = {1, 2};
        initial.cueFrame = {0, 1};
        initial.playing = {true, false};
        initial.deckGain = {.5F, .25F};
        initial.masterGain = .75F;
        initial.cueSelected = {true, false};
        initial.cueGain = .625F;
        LiveSession restored(a, b, initial);
        state = restored.stoppedState();
        passed &= require(state.sourceFrame == initial.sourceFrame && state.sourceFrames == std::array<std::uint64_t, 2>{2, 3} &&
                          state.playing == initial.playing && state.deckGain == initial.deckGain &&
                          state.cueFrame == initial.cueFrame && !state.cueHeld[0] && !state.cueHeld[1] &&
                          !state.cuePreview[0] && !state.cuePreview[1] &&
                          state.masterGain == initial.masterGain && state.cueGain == initial.cueGain &&
                          state.cueSelected == initial.cueSelected && state.outputFrame == 0 && state.callbacks == 0,
                          "initial source positions/gains/playing state were not restored");
        restored.stop();
        passed &= require(restored.stoppedState().sourceFrame == initial.sourceFrame && !restored.ready(),
                          "stopped session changed restored source positions");
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
        for (const auto gain : {-.1F, 1.1F, std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()}) {
            invalid = initial;
            invalid.deckGain[0] = gain;
            passed &= rejects([&] { LiveSession rejected(a, b, invalid); }, "invalid initial deck gain was accepted");
            invalid = initial;
            invalid.masterGain = gain;
            passed &= rejects([&] { LiveSession rejected(a, b, invalid); }, "invalid initial master gain was accepted");
            invalid = initial;
            invalid.cueGain = gain;
            passed &= rejects([&] { LiveSession rejected(a, b, invalid); }, "invalid initial cue gain was accepted");
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

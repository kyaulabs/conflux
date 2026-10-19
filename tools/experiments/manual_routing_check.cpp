// SPDX-License-Identifier: AGPL-3.0-or-later
// Private-daemon integration only. Run through private_routing_check.py; this
// deliberately refuses an ordinary laptop PipeWire socket. No physical output.
#include "audio/LiveSession.hpp"
#include "audio/StereoRecorder.hpp"
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <sndfile.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace std::chrono_literals;
using conflux::audio::CueRouteStatus;
using conflux::audio::LiveSession;
using conflux::audio::PreparedPlayback;
using conflux::audio::RenderCore;
using conflux::audio::RoutePair;
using conflux::audio::RoutingConfig;
using conflux::audio::RoutingSnapshot;
using conflux::audio::StereoRecorder;

namespace {
constexpr std::array<float, 2> masterValue{.0390625F, -.05859375F};
constexpr std::array<float, 2> cueValue{.046875F, .0234375F};
constexpr std::uint64_t startupWarmupFrames = 1024;
std::vector<std::string> checks;
std::filesystem::path diagnosticDirectory;
bool executionDeadlineGate = true;
void require(bool condition, const std::string& description)
{
    if (!condition) throw std::runtime_error(description);
    checks.push_back(description);
}
template<class Predicate> void wait(Predicate&& predicate, const char* description)
{
    const auto deadline = std::chrono::steady_clock::now() + 7s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(description);
        std::this_thread::sleep_for(5ms);
    }
}
std::string json(std::string_view value)
{
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c == '\n') result += "\\n";
        else if (c < 32) result += '?';
        else result += char(c);
    }
    return result + '"';
}
void writeChecks(std::ostream& output)
{
    output << '[';
    for (std::size_t i = 0; i < checks.size(); ++i) {
        if (i) output << ',';
        output << json(checks[i]);
    }
    output << ']';
}
void failureDiagnostic(LiveSession& session, const char* stage, const char* message) noexcept
{
    try {
        LiveSession::State state{};
        bool observed = false;
        while (session.pollState(state)) observed = true;
        const auto route = session.routingSnapshot();
        const auto temporary = diagnosticDirectory / "live-failure.json.tmp";
        std::ofstream output(temporary);
        output << "{\"stage\":" << json(stage) << ",\"error\":" << json(message)
               << ",\"execution_deadline_gate\":" << (executionDeadlineGate ? "true" : "false")
               << ",\"session_ready\":" << (session.ready() ? "true" : "false")
               << ",\"state_observed\":" << (observed ? "true" : "false")
               << ",\"state\":{\"callbacks\":" << state.callbacks << ",\"output_frame\":" << state.outputFrame
               << ",\"graph_rate\":" << state.graphRate << ",\"invalid_buffers\":" << state.invalidBuffers
               << ",\"execution_deadline_misses\":" << state.deadlineMisses
               << ",\"applied\":" << state.applied << ",\"rejected\":" << state.rejected
               << ",\"snapshot_drops\":" << state.snapshotDrops << ",\"source_frames\":["
               << state.sourceFrame[0] << ',' << state.sourceFrame[1] << "]}"
               << ",\"routing\":{\"master_ready\":" << (route.masterReady ? "true" : "false")
               << ",\"cue_ready\":" << (route.cueReady ? "true" : "false")
               << ",\"cue_status\":" << unsigned(route.cueStatus)
               << ",\"master_error\":" << json(route.masterError) << ",\"cue_error\":" << json(route.cueError)
               << ",\"default_sink\":" << json(route.defaultSinkName) << ",\"playback_ports\":[";
        for (std::size_t i = 0; i < route.playbackPorts.size(); ++i) {
            if (i) output << ',';
            const auto& port = route.playbackPorts[i];
            output << "{\"id\":" << port.globalId << ",\"serial\":" << port.serial
                   << ",\"node_id\":" << port.nodeId << ",\"node_serial\":" << port.nodeSerial
                   << ",\"node_name\":" << json(port.nodeName) << ",\"port_name\":" << json(port.portName)
                   << ",\"channel\":" << json(port.channel) << '}';
        }
        output << "]},\"checks\":";
        writeChecks(output);
        output << "}\n";
        output.close();
        std::filesystem::rename(temporary, diagnosticDirectory / "live-failure.json");
        // The runner can inspect the failing graph before stack unwinding
        // destroys it. This handshake never uses another PipeWire socket.
        if (std::getenv("CONFLUX_ROUTING_DIAGNOSTICS")) {
            const auto deadline = std::chrono::steady_clock::now() + 8s;
            while (!std::filesystem::exists(diagnosticDirectory / "release-failure") &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(20ms);
        }
    } catch (...) {
        std::cerr << "Could not preserve private live failure diagnostics\n";
    }
}

class Sink {
public:
    Sink(std::string name, std::size_t channels, bool cueOnly = false)
        : name_(std::move(name)), channels_(channels)
    {
        if (channels_ != 2 && channels_ != 4)
            throw std::invalid_argument("Private sink must have two or four channels");
        for (std::size_t channel = 0; channel < channels_; ++channel)
            expected_[channel] = channel >= 2 || cueOnly ? cueValue[channel % 2] : masterValue[channel % 2];
        loop_ = pw_thread_loop_new(name_.c_str(), nullptr);
        if (!loop_) throw std::runtime_error("Cannot create private sink loop");
        filter_ = pw_filter_new_simple(pw_thread_loop_get_loop(loop_), name_.c_str(),
            pw_properties_new(PW_KEY_NODE_NAME, name_.c_str(), PW_KEY_MEDIA_CLASS, "Audio/Sink",
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                PW_KEY_NODE_AUTOCONNECT, "false", "node.always-process", "true",
                PW_KEY_NODE_LATENCY, "256/48000", PW_KEY_NODE_RATE, "1/48000", nullptr),
            &events(), this);
        if (!filter_) { stop(); throw std::runtime_error("Cannot create private sink filter"); }
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            const auto portName = "input_" + std::to_string(channel);
            ports_[channel] = pw_filter_add_port(filter_, PW_DIRECTION_INPUT,
                PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
                pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME,
                    portName.c_str(), PW_KEY_AUDIO_CHANNEL, channel % 2 ? "FR" : "FL", nullptr), nullptr, 0);
            if (!ports_[channel]) { stop(); throw std::runtime_error("Cannot create private sink port"); }
        }
        if (pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0 || pw_thread_loop_start(loop_) < 0) {
            stop(); throw std::runtime_error("Cannot connect private sink");
        }
        started_ = true;
    }
    ~Sink() { stop(); }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    void stop() noexcept
    {
        if (!loop_) return;
        if (started_) pw_thread_loop_lock(loop_);
        if (filter_) { pw_filter_disconnect(filter_); pw_filter_destroy(filter_); filter_ = nullptr; }
        if (started_) { pw_thread_loop_unlock(loop_); pw_thread_loop_stop(loop_); }
        pw_thread_loop_destroy(loop_); loop_ = nullptr; started_ = false;
    }
    const std::string& name() const { return name_; }
    std::uint64_t received(std::size_t channel) const { return received_[channel].load(); }
    std::uint64_t zerosAfterSignal(std::size_t channel) const { return zerosAfterSignal_[channel].load(); }
    std::uint64_t maxSignalCallbackGapNs(std::size_t channel) const { return maxSignalGapNs_[channel].load(); }
    std::uint64_t wrong() const { return wrong_.load(); }
    void validateSamples(bool armed) { validationArmed_.store(armed, std::memory_order_release); }
private:
    static void process(void* data, spa_io_position* position) noexcept
    {
        auto& self = *static_cast<Sink*>(data);
        if (!position || position->clock.duration > 65536) return;
        const auto frames = std::uint32_t(position->clock.duration);
        const bool validate = self.validationArmed_.load(std::memory_order_acquire);
        for (std::size_t channel = 0; channel < self.channels_; ++channel) {
            const auto* samples = static_cast<const float*>(pw_filter_get_dsp_buffer(self.ports_[channel], frames));
            if (!samples || !validate) continue;
            std::uint64_t nonzero = 0, wrong = 0, zeros = 0;
            bool signalObserved = self.received_[channel].load(std::memory_order_relaxed) != 0;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                // Constant-sample validation is armed only after startup
                // settles. Fail-closed output can legitimately contain zeros.
                if (samples[frame] == 0.F) { zeros += signalObserved; continue; }
                signalObserved = true;
                ++nonzero;
                if (samples[frame] != self.expected_[channel]) ++wrong;
            }
            self.received_[channel].fetch_add(nonzero, std::memory_order_relaxed);
            self.zerosAfterSignal_[channel].fetch_add(zeros, std::memory_order_relaxed);
            self.wrong_.fetch_add(wrong, std::memory_order_relaxed);
            if (nonzero) {
                const auto now = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                const auto previous = self.lastSignalNs_[channel].load(std::memory_order_relaxed);
                if (previous) self.maxSignalGapNs_[channel].store(
                    std::max(self.maxSignalGapNs_[channel].load(std::memory_order_relaxed), now - previous),
                    std::memory_order_relaxed);
                self.lastSignalNs_[channel].store(now, std::memory_order_relaxed);
            }
        }
    }
    static const pw_filter_events& events()
    {
        static const auto value = [] { pw_filter_events e{}; e.version = PW_VERSION_FILTER_EVENTS; e.process = process; return e; }();
        return value;
    }
    std::string name_;
    std::size_t channels_;
    std::array<float, 4> expected_{};
    std::array<void*, 4> ports_{};
    std::array<std::atomic<std::uint64_t>, 4> received_{};
    std::array<std::atomic<std::uint64_t>, 4> zerosAfterSignal_{}, lastSignalNs_{}, maxSignalGapNs_{};
    std::atomic<std::uint64_t> wrong_{0};
    std::atomic<bool> validationArmed_{false};
    pw_thread_loop* loop_ = nullptr;
    pw_filter* filter_ = nullptr;
    bool started_ = false;
};

RoutePair pair(const RoutingSnapshot& snapshot, const std::string& node, unsigned left)
{
    auto find = [&](unsigned channel) {
        const auto name = "input_" + std::to_string(channel);
        for (const auto& port : snapshot.playbackPorts)
            if (port.nodeName == node && port.portName == name) return port;
        throw std::runtime_error("Private sink port missing from discovery: " + node + "/" + name);
    };
    return {find(left), find(left + 1)};
}
RoutingSnapshot discover(std::size_t expectedPorts)
{
    LiveSession discovery;
    std::string error;
    require(discovery.startDiscovery(error), "Inactive private discovery session connects");
    RoutingSnapshot result;
    wait([&] {
        result = discovery.routingSnapshot();
        return result.playbackPorts.size() >= expectedPorts;
    }, "Private playback port discovery timed out");
    discovery.stop();
    return result;
}
void awaitReady(LiveSession& session)
{
    try {
        wait([&] {
            const auto error = session.error();
            if (!error.empty()) throw std::runtime_error(error);
            return session.ready();
        }, "Explicit private master route did not become ready");
    } catch (const std::exception& error) {
        failureDiagnostic(session, "awaitReady", error.what());
        throw;
    }
}
std::uint64_t awaitSteadySamples(LiveSession& session)
{
    awaitReady(session);
    LiveSession::State state{};
    try {
        wait([&] {
            const auto error = session.error();
            if (!error.empty()) throw std::runtime_error(error);
            while (session.pollState(state)) {}
            return session.ready() && state.outputFrame >= startupWarmupFrames;
        }, "Private startup fade did not reach the steady validation window");
    } catch (const std::exception& error) {
        failureDiagnostic(session, "startup-warmup", error.what());
        throw;
    }
    return state.outputFrame;
}
void verifyRecording(const std::filesystem::path& path, std::uint64_t frames)
{
    SF_INFO info{};
    auto* input = sf_open(path.c_str(), SFM_READ, &info);
    require(input, "Post-master recording reopens after acknowledged detach");
    const auto guard = std::unique_ptr<SNDFILE, decltype(&sf_close)>(input, sf_close);
    require(info.channels == 2 && info.samplerate == 48000 && info.frames == sf_count_t(frames),
            "Recording geometry and drained frame count match accepted master PCM");
    std::array<float, 2048> samples{};
    sf_count_t total = 0;
    while (const auto count = sf_readf_float(input, samples.data(), samples.size() / 2)) {
        for (sf_count_t frame = 0; frame < count; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                if (samples[std::size_t(frame) * 2 + channel] != masterValue[channel])
                    throw std::runtime_error("Recorded sample differs from post-master bus (cue must not be recorded)");
        total += count;
    }
    require(total == sf_count_t(frames) && sf_error(input) == SF_ERR_NO_ERROR,
            "Every recorded sample equals the independent post-master expectation, excluding cue");
}

struct ScenarioResult {
    std::string name;
    std::uint64_t frames = 0, callbacks = 0, recorded = 0;
    std::uint64_t graphDiscontinuities = 0, recoveryFlags = 0;
    bool recoveryFlagsAvailable = false;
    std::array<std::uint64_t, 4> warmupFrames{};
    std::uint64_t deadlineMisses = 0;
};
ScenarioResult runTopology(const std::filesystem::path& directory, bool twoDevices,
                           std::span<const float> a, std::span<const float> b)
{
    const std::string label = twoDevices ? "two-stereo-sinks" : "one-four-channel-sink";
    Sink master("conflux-routing-check-" + label + "-master", twoDevices ? 2 : 4);
    std::unique_ptr<Sink> cue;
    if (twoDevices) cue = std::make_unique<Sink>("conflux-routing-check-" + label + "-cue", 2, true);
    const auto snapshot = discover(4);
    RoutingConfig routing{pair(snapshot, master.name(), 0), pair(snapshot, cue ? cue->name() : master.name(), cue ? 0 : 2)};
    LiveSession::InitialState initial;
    initial.playing = {true, true}; initial.deckGain = {.5F, .25F}; initial.masterGain = .5F;
    initial.cueSelected = {false, true}; initial.cueGain = .75F;
    const auto path = directory / (label + ".wav");
    auto recorder = std::make_unique<StereoRecorder>(path);
    LiveSession session(a, b, initial);
    std::string error;
    const auto sessionStarted = std::chrono::steady_clock::now();
    require(session.start(error, routing), label + ": explicit session connects");
    std::array<std::uint64_t, 4> warmupFrames{};
    warmupFrames[0] = awaitSteadySamples(session);
    master.validateSamples(true);
    if (cue) cue->validateSamples(true);
    require(session.requestRecorder(recorder.get()), label + ": steady recorder attachment accepted after startup warmup");
    wait([&] {
        if (!session.error().empty()) throw std::runtime_error(session.error());
        return master.received(0) >= 4096 && master.received(1) >= 4096 &&
            (cue ? cue->received(0) >= 4096 && cue->received(1) >= 4096 :
                   master.received(2) >= 4096 && master.received(3) >= 4096) &&
            !session.recorderChangePending() && session.recorderAttached() && recorder->acceptedFrames() >= 4096;
    }, "Private master/cue samples or recorder acknowledgement timed out");
    require(master.wrong() == 0 && (!cue || cue->wrong() == 0),
            label + ": every validated steady nonzero master/cue sample matches independent asymmetric sums");
    require(session.requestRecorder(nullptr), label + ": recorder detach accepted");
    wait([&] { return !session.recorderChangePending() && !session.recorderAttached(); }, "Recorder detach timed out");
    const auto recorded = recorder->acceptedFrames();
    std::this_thread::sleep_for(40ms);
    require(recorded >= 4096 && recorder->acceptedFrames() == recorded,
            label + ": acknowledged detach prevents later callback writes");
    recorder->stop();
    require(!recorder->failed() && recorder->writtenFrames() == recorded,
            label + ": recorder drains and closes without overflow or storage error");
    recorder.reset(); // Deliberately free while the private audio graph runs.
    std::this_thread::sleep_for(20ms);
    verifyRecording(path, recorded);
    require(session.routingSnapshot().cueReady, label + ": independent cue route is ready");
    if (cue) {
        LiveSession::State observedState{};
        const auto lateDeadline = sessionStarted + 8s;
        for (;;) {
            const auto error = session.error();
            if (!error.empty()) {
                failureDiagnostic(session, "before-late-cue-removal", error.c_str());
                throw std::runtime_error(error);
            }
            while (session.pollState(observedState)) {}
            if (session.ready() && observedState.outputFrame >= 249600 &&
                std::chrono::steady_clock::now() - sessionStarted >= 5200ms)
                break;
            if (std::chrono::steady_clock::now() >= lateDeadline)
                throw std::runtime_error("Did not reach 5.2 seconds of rendered private playback before cue removal");
            std::this_thread::sleep_for(5ms);
        }
        require(master.wrong() == 0 && !cue->wrong(), label + ": exact private master/cue playback continues past the startup deadline");
        const std::array before{master.received(0), master.received(1)};
        const std::array zerosBefore{master.zerosAfterSignal(0), master.zerosAfterSignal(1)};
        const std::array gapsBefore{master.maxSignalCallbackGapNs(0), master.maxSignalCallbackGapNs(1)};
        const auto framesBefore = observedState.outputFrame;
        const auto began = std::chrono::steady_clock::now();
        const auto deadline = began + 7s;
        std::vector<std::string> observations;
        std::uint64_t unready = 0, readyTransitions = 0;
        std::array<std::uint64_t, 2> terminalReceived{};
        std::uint64_t terminalUs = 0, firstHealthyUs = 0;
        bool terminalObserved = false, healthyObserved = false;
        bool previousReady = true;
        while (session.pollState(observedState)) {}
        auto saveRemoval = [&](bool recovered, const std::string& error) {
            std::ofstream output(diagnosticDirectory / "cue-removal.json");
            output << "{\"recovered\":" << (recovered ? "true" : "false") << ",\"error\":" << json(error)
                   << ",\"elapsed_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - began).count()
                   << ",\"unready_observations\":" << unready << ",\"observed_ready_transitions\":" << readyTransitions
                   << ",\"rendered_frames_before_removal\":" << framesBefore
                   << ",\"removal_after_session_start_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(
                        began - sessionStarted).count()
                   << ",\"master_zero_samples_after_signal_before_removal\":[" << zerosBefore[0] << ',' << zerosBefore[1]
                   << "],\"maximum_master_signal_callback_gap_ns_before_removal\":[" << gapsBefore[0] << ',' << gapsBefore[1] << ']'
                   << ",\"terminal_cue_observed\":" << (terminalObserved ? "true" : "false")
                   << ",\"terminal_cue_us\":" << terminalUs
                   << ",\"healthy_master_observed\":" << (healthyObserved ? "true" : "false")
                   << ",\"first_healthy_master_us\":" << firstHealthyUs
                   << ",\"master_nonzero_samples_after_removal\":[" << master.received(0) - before[0] << ','
                   << master.received(1) - before[1] << "],\"master_zero_samples_after_signal_during_removal\":["
                   << master.zerosAfterSignal(0) - zerosBefore[0] << ',' << master.zerosAfterSignal(1) - zerosBefore[1]
                   << "],\"maximum_master_signal_callback_gap_ns_for_whole_scenario\":["
                   << master.maxSignalCallbackGapNs(0) << ',' << master.maxSignalCallbackGapNs(1)
                   << "],\"master_nonzero_samples_after_terminal_cue\":["
                   << (terminalObserved ? master.received(0) - terminalReceived[0] : 0) << ','
                   << (terminalObserved ? master.received(1) - terminalReceived[1] : 0)
                   << "],\"gap_scope\":\"Virtual receiver callback spacing; not physical latency or uninterrupted-audio proof\""
                   << ",\"observations\":[";
            for (std::size_t i = 0; i < observations.size(); ++i) {
                if (i) output << ',';
                output << observations[i];
            }
            output << "]}\n";
        };
        cue->stop();
        try {
            for (;;) {
                // Check errors before ready: a short-circuit readiness check
                // must not hide a latched buffer, rate or routing failure.
                const auto error = session.error();
                const auto route = session.routingSnapshot();
                const bool ready = session.ready();
                while (session.pollState(observedState)) {}
                unready += !ready;
                readyTransitions += ready != previousReady;
                previousReady = ready;
                observations.push_back("{\"elapsed_us\":" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - began).count()) +
                    ",\"ready\":" + (ready ? "true" : "false") + ",\"error\":" + json(error) +
                    ",\"master_ready\":" + (route.masterReady ? "true" : "false") +
                    ",\"master_error\":" + json(route.masterError) +
                    ",\"cue_ready\":" + (route.cueReady ? "true" : "false") +
                    ",\"cue_status\":" + std::to_string(unsigned(route.cueStatus)) +
                    ",\"cue_error\":" + json(route.cueError) +
                    ",\"latest_state\":{\"callbacks\":" + std::to_string(observedState.callbacks) +
                    ",\"output_frame\":" + std::to_string(observedState.outputFrame) +
                    ",\"graph_rate\":" + std::to_string(observedState.graphRate) +
                    ",\"invalid_buffers\":" + std::to_string(observedState.invalidBuffers) +
                    ",\"deadline_misses\":" + std::to_string(observedState.deadlineMisses) +
                    ",\"max_execution_ns\":" + std::to_string(observedState.maxExecutionNs) +
                    ",\"snapshot_drops\":" + std::to_string(observedState.snapshotDrops) +
                    ",\"source_frame\":[" + std::to_string(observedState.sourceFrame[0]) + ',' +
                        std::to_string(observedState.sourceFrame[1]) + "]}" +
                    ",\"master_received\":[" + std::to_string(master.received(0)) + ',' +
                        std::to_string(master.received(1)) + "]}");
                if (!error.empty()) throw std::runtime_error("Cue removal latched session error: " + error);
                if (master.wrong()) throw std::runtime_error("Cue removal contaminated the exact master samples");
                // Link and endpoint removal notifications may arrive in either
                // order. PAUSED is a transient state, never a success by itself.
                const bool cueFailedClosed = !route.cueReady &&
                    (route.cueStatus == CueRouteStatus::MissingEndpoint || route.cueStatus == CueRouteStatus::LinkError);
                if (cueFailedClosed && !terminalObserved) {
                    terminalObserved = true;
                    terminalReceived = {master.received(0), master.received(1)};
                    terminalUs = std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - began).count());
                }
                if (cueFailedClosed && ready && route.masterReady && !healthyObserved) {
                    healthyObserved = true;
                    firstHealthyUs = std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - began).count());
                }
                if (cueFailedClosed && ready && route.masterReady && terminalObserved &&
                    master.received(0) >= terminalReceived[0] + 1024 && master.received(1) >= terminalReceived[1] + 1024)
                    break;
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Master did not recover after cue removal with cue safely muted");
                std::this_thread::sleep_for(5ms);
            }
            saveRemoval(true, {});
        } catch (const std::exception& error) {
            saveRemoval(false, error.what());
            failureDiagnostic(session, "cue-removal", error.what());
            throw;
        }
        require(true, label + ": cue removal fails closed and master recovers with exact advancing samples");
    }
    master.validateSamples(false);
    if (cue) cue->validateSamples(false);
    session.stop();
    const auto final = session.stoppedState();
    require(final.outputFrame > 0 && final.sourceFrame[0] == final.outputFrame && final.sourceFrame[1] == final.outputFrame,
            label + ": two buses advance each source exactly once per output frame");
    require(!final.invalidBuffers && !final.rejected,
            label + ": no invalid output buffers or rejected controls");
    if (executionDeadlineGate)
        require(!final.deadlineMisses, label + ": no execution deadline misses");
    require(!session.recorderChangePending() && !session.recorderAttached(), label + ": stopped session releases recorder tap");
    const auto timing = session.stoppedTimingSummary();

    // Separate sessions isolate each failure policy. No invalid cue selection
    // may redirect cue into the still-valid master pair.
    std::size_t warmupIndex = 1;
    for (const auto mode : {"disabled", "overlap", "missing"}) {
        RoutingConfig config{routing.master, {}};
        CueRouteStatus expected = CueRouteStatus::Disabled;
        if (std::string_view(mode) == "overlap") { config.cue = config.master; expected = CueRouteStatus::InvalidSelection; }
        if (std::string_view(mode) == "missing") {
            config.cue = routing.cue; ++config.cue->left.serial; expected = CueRouteStatus::MissingEndpoint;
        }
        LiveSession rejectedCue(a, b, initial);
        require(rejectedCue.start(error, config), label + ": " + mode + " cue session connects");
        warmupFrames[warmupIndex++] = awaitSteadySamples(rejectedCue);
        const auto status = rejectedCue.routingSnapshot();
        require(!status.cueReady && status.cueStatus == expected && rejectedCue.error().empty(),
                label + ": " + mode + " cue stays disabled with valid independent master");
        const std::array receivedBefore{master.received(0), master.received(1)};
        master.validateSamples(true);
        std::this_thread::sleep_for(30ms);
        require(!master.wrong() && master.received(0) > receivedBefore[0] && master.received(1) > receivedBefore[1],
                label + ": " + mode + " cue cannot contaminate fresh steady master samples");
        master.validateSamples(false);
        rejectedCue.stop();
    }
    return {label, final.outputFrame, final.callbacks, recorded, timing.counters.graphDiscontinuities,
            timing.counters.xrunRecoveryFlags, timing.capabilities.xrunRecoveryFlag != 0, warmupFrames,
            final.deadlineMisses};
}

struct HandoffStep {
    std::uint64_t generation = 0, beforeFrame = 0, ackFrame = 0, afterFrame = 0;
    std::uint64_t retiredSourceFrame = 0, recordedBefore = 0, recordedAfter = 0;
    bool retiredReaderFreed = false;
};
struct HandoffResult {
    std::uint64_t warmupFrames = 0, frames = 0, callbacks = 0, recorded = 0;
    std::uint64_t deadlineMisses = 0, wrongSamples = 0;
    std::array<HandoffStep, 2> steps{};
    std::array<std::uint64_t, 2> zeroSamples{}, maxSignalGapNs{};
};
void writeHandoff(std::ostream& output, const HandoffResult& result, bool passed, std::string_view error = {})
{
    output << "{\"passed\":" << (passed ? "true" : "false") << ",\"error\":" << json(error)
           << ",\"execution_deadline_gate\":" << (executionDeadlineGate ? "true" : "false")
           << ",\"virtual_sink_zero_gate\":" << (executionDeadlineGate ? "true" : "false")
           << ",\"wrong_nonzero_sink_samples\":" << result.wrongSamples
           << ",\"scope\":\"Two bounded callback source handoffs on one private virtual graph; muted target, exact audible peer, master recording attached continuously. Startup fade excluded after at least 1024 output frames. Hardware and sustained qualification remain separate.\""
           << ",\"warmup_output_frames\":" << result.warmupFrames << ",\"output_frames\":" << result.frames
           << ",\"callbacks\":" << result.callbacks << ",\"recorded_frames\":" << result.recorded
           << ",\"execution_deadline_misses\":" << (result.frames ? std::to_string(result.deadlineMisses) : "null")
           << ",\"zero_samples_after_signal\":[" << result.zeroSamples[0] << ',' << result.zeroSamples[1]
           << "],\"maximum_virtual_receiver_signal_gap_ns\":[" << result.maxSignalGapNs[0] << ',' << result.maxSignalGapNs[1]
           << "],\"steps\":[";
    for (std::size_t index = 0; index < result.steps.size(); ++index) {
        if (index) output << ',';
        const auto& step = result.steps[index];
        output << "{\"generation\":" << step.generation << ",\"before_output_frame\":" << step.beforeFrame
               << ",\"callback_end_ack_output_frame\":" << step.ackFrame << ",\"after_output_frame\":" << step.afterFrame
               << ",\"retired_source_frame\":" << step.retiredSourceFrame
               << ",\"retired_reader_freed_while_running\":" << (step.retiredReaderFreed ? "true" : "false")
               << ",\"recorded_before\":" << step.recordedBefore << ",\"recorded_after\":" << step.recordedAfter << '}';
    }
    output << "]}";
}

std::unique_ptr<PreparedPlayback> primedSyntheticSource(std::uint64_t frames, float left, float right,
                                                      bool stemBank = false)
{
    const auto closeFile = [](std::FILE* file) { if (file) (void)std::fclose(file); };
    const auto file = std::unique_ptr<std::FILE, decltype(closeFile)>(std::tmpfile(), closeFile);
    if (!file) throw std::runtime_error("Cannot create anonymous private handoff PCM");
    std::array<float, 8192> block{};
    for (std::size_t sample = 0; sample < block.size(); sample += 2) {
        block[sample] = stemBank ? left * .25F : left;
        block[sample + 1] = stemBank ? right * .25F : right;
    }
    for (std::uint64_t first = 0; first < frames;) {
        const auto count = std::size_t(std::min<std::uint64_t>(block.size() / 2, frames - first));
        if (std::fwrite(block.data(), sizeof(float), count * 2, file.get()) != count * 2)
            throw std::runtime_error("Cannot write anonymous private handoff PCM");
        first += count;
    }
    if (std::fflush(file.get())) throw std::runtime_error("Cannot flush anonymous private handoff PCM");
    // Four independent readers duplicate this anonymous quarter-amplitude
    // fixture; their coherent sum is the same desired quiet stereo constant.
    const auto fd = fileno(file.get());
    auto reader = stemBank
        ? std::make_unique<PreparedPlayback>(std::array<int, 4>{fd, fd, fd, fd}, frames)
        : std::make_unique<PreparedPlayback>(fd, frames);
    if (!reader->prime(0, 2s)) throw std::runtime_error("Cannot prime private handoff source: " + reader->error());
    return reader; // The reader owns its duplicate; no named spool is retained.
}

HandoffResult runSourceHandoff(const std::filesystem::path& directory, std::span<const float> oldTarget)
{
    HandoffResult result;
    Sink master("conflux-routing-check-source-handoff-master", 2);
    const auto snapshot = discover(2);
    const RoutingConfig routing{pair(snapshot, master.name(), 0), {}};
    std::vector<float> peer(oldTarget.size());
    for (std::size_t sample = 0; sample < peer.size(); sample += 2) {
        peer[sample] = masterValue[0]; peer[sample + 1] = masterValue[1];
    }
    const auto path = directory / "source-handoff.wav";
    auto recorder = std::make_unique<StereoRecorder>(path);
    LiveSession::InitialState initial;
    initial.playing = {true, true}; initial.deckGain = {0.F, 1.F}; initial.masterGain = 1.F;
    initial.cueFrame[0] = 96;
    LiveSession session(oldTarget, peer, initial);
    auto save = [&](bool passed, std::string_view error = {}) {
        std::ofstream output(diagnosticDirectory / "source-handoff.json");
        writeHandoff(output, result, passed, error);
        output << '\n';
    };
    try {
        std::string error;
        require(session.start(error, routing), "source-handoff: one private session connects with cue disabled");
        result.warmupFrames = awaitSteadySamples(session);
        master.validateSamples(true);
        require(session.requestRecorder(recorder.get()), "source-handoff: recorder attachment accepted after startup warmup");
        LiveSession::State state{};
        auto observe = [&] {
            const auto error = session.error();
            if (!error.empty()) throw std::runtime_error(error);
            while (session.pollState(state)) {}
            if (master.wrong()) throw std::runtime_error("Source handoff changed the exact audible peer samples");
        };
        wait([&] {
            observe();
            return !session.recorderChangePending() && session.recorderAttached() &&
                recorder->acceptedFrames() >= 2048 && master.received(0) >= 2048 && master.received(1) >= 2048;
        }, "Handoff recorder/peer did not enter steady playback");
        std::uint32_t commandId = 1000;
        for (std::size_t index = 0; index < result.steps.size(); ++index) {
            const auto generation = std::uint64_t(index + 1);
            if (index) {
                // The installed deck starts at gain1, paused. Set its gain to0
                // and let that control ramp settle BEFORE playing its reader.
                require(session.submit({RenderCore::Command::Type::DeckGain, 0, 0, 0.F, ++commandId}),
                    "source-handoff: installed target accepts mute while paused");
                wait([&] { observe(); return state.lastCommandId == commandId && state.deckGain[0] == 0.F; },
                    "Installed target mute was not applied");
                const auto mutedAt = state.outputFrame;
                wait([&] { observe(); return state.outputFrame >= mutedAt + startupWarmupFrames; },
                    "Installed target gain ramp did not settle");
                require(session.submit({RenderCore::Command::Type::Play, 0, 0, 1.F, ++commandId}),
                    "source-handoff: first installed reader starts while muted");
                wait([&] { observe(); return state.sourceGeneration[0] == 1 && state.sourceFrame[0] >= 4096; },
                    "First installed reader did not actually deliver frames before replacement");
                require(!state.buffering[0] && !state.starvedFrames[0] && !state.sourceReadError[0] &&
                        !state.sourceReadErrorObserved[0], "source-handoff: first installed reader runs without starvation/read failure");
            }
            auto candidate = primedSyntheticSource(oldTarget.size() / 2,
                index ? -.3125F : .1875F, index ? .4375F : -.375F, index == 0);
            auto* const identity = candidate.get();
            require(!session.requestSourceReplacement(index ? 0 : 2, candidate, index ? 1 : generation) &&
                    candidate.get() == identity, "source-handoff: rejected deck/generation request retains candidate ownership");
            require(session.submit({RenderCore::Command::Type::CueRelease, 0, 0, 1.F, ++commandId}),
                "source-handoff: old target CUE release enters FIFO before source publication");
            observe();
            auto& step = result.steps[index];
            step.generation = generation; step.beforeFrame = state.outputFrame;
            step.recordedBefore = recorder->acceptedFrames();
            const auto oldCursor = state.sourceFrame[0];
            const std::array receivedBefore{master.received(0), master.received(1)};
            require(session.requestSourceReplacement(0, candidate, generation) && !candidate,
                "source-handoff: accepted publication consumes only the candidate owner");
            require(!session.submit({RenderCore::Command::Type::Play, 0, 0, 1.F, ++commandId}),
                "source-handoff: target transport is rejected until its handoff acknowledgement is collected");
            require(session.submit({RenderCore::Command::Type::Play, 1, 0, 1.F, ++commandId}) &&
                    session.submit({RenderCore::Command::Type::MasterGain, 0, 0, 1.F, ++commandId}),
                "source-handoff: peer and global commands remain accepted while target is pending");
            bool acknowledged = false;
            wait([&] {
                observe();
                if (!acknowledged) {
                    auto completion = session.takeSourceReplacementResult();
                    if (completion) {
                        require(completion->completion.installed && completion->completion.deck == 0 &&
                                completion->completion.generation == generation &&
                                completion->completion.outputFrame > step.beforeFrame &&
                                completion->completion.retired.sourceFrame >= oldCursor &&
                                !completion->completion.retired.starvedFrames && !completion->completion.retired.readFailed,
                            "source-handoff: callback-end acknowledgement retains exact generation/boundary and clean retired state");
                        require(bool(completion->retiredPlayback) == bool(index),
                            "source-handoff: only the second swap retires the first installed four-reader bank");
                        step.ackFrame = completion->completion.outputFrame;
                        step.retiredSourceFrame = completion->completion.retired.sourceFrame;
                        completion->retiredPlayback.reset(); // free whole owner while the graph keeps running
                        step.retiredReaderFreed = index != 0;
                        acknowledged = true;
                    }
                }
                return acknowledged && state.sourceGeneration[0] == generation &&
                    master.received(0) >= receivedBefore[0] + 1024 && master.received(1) >= receivedBefore[1] + 1024 &&
                    recorder->acceptedFrames() >= step.recordedBefore + 1024;
            }, "Source handoff/continuous peer or recorder progress timed out");
            require(!session.sourceReplacementPending() && session.ready() && session.recorderAttached() &&
                    state.outputFrame >= step.ackFrame &&
                    !session.recorderChangePending() && state.sourceFrame[0] == 0 && !state.playing[0] &&
                    state.cueFrame[0] == 0 && !state.cueHeld[0] && !state.cuePreview[0] && !state.cueSelected[0] &&
                    state.deckGain[0] == 1.F && state.sourceFrame[1] == state.outputFrame && state.playing[1] &&
                    state.deckGain[1] == 1.F && state.masterGain == 1.F && !state.sourceReadError[0] &&
                    !state.sourceReadErrorObserved[0] && !state.starvedFrames[0],
                "source-handoff: target reset and peer/output/recording state remain coherent on one live session");
            step.afterFrame = state.outputFrame; step.recordedAfter = recorder->acceptedFrames();
        }
        require(session.requestRecorder(nullptr), "source-handoff: recorder detaches after both live source swaps");
        wait([&] { observe(); return !session.recorderChangePending() && !session.recorderAttached(); },
            "Handoff recorder detach timed out");
        result.recorded = recorder->acceptedFrames();
        std::this_thread::sleep_for(40ms);
        require(recorder->acceptedFrames() == result.recorded, "source-handoff: acknowledged detach stops all later recorder writes");
        recorder->stop();
        require(!recorder->failed() && !recorder->overflowCount() && recorder->writtenFrames() == result.recorded,
            "source-handoff: continuous recording drains without overflow/storage failure");
        verifyRecording(path, result.recorded);
        observe();
        result.zeroSamples = {master.zerosAfterSignal(0), master.zerosAfterSignal(1)};
        result.maxSignalGapNs = {master.maxSignalCallbackGapNs(0), master.maxSignalCallbackGapNs(1)};
        result.wrongSamples = master.wrong();
        // Every recorded frame above stays an exact functional oracle. Keep
        // nonzero routing errors strict; downstream zero delivery also depends
        // on virtual graph execution deadlines, which instrumentation excludes.
        require(!result.wrongSamples && (!executionDeadlineGate || (!result.zeroSamples[0] && !result.zeroSamples[1])),
            "source-handoff: nonzero peer samples remain exact; virtual zero gate follows execution mode (wrong=" +
                std::to_string(result.wrongSamples) + ", zeros=[" + std::to_string(result.zeroSamples[0]) + "," +
                std::to_string(result.zeroSamples[1]) + "])");
        master.validateSamples(false);
        session.stop();
        const auto final = session.stoppedState();
        result.frames = final.outputFrame; result.callbacks = final.callbacks;
        result.deadlineMisses = final.deadlineMisses;
        require(final.sourceFrame[1] == final.outputFrame && final.sourceGeneration[0] == 2 &&
                !final.invalidBuffers && !final.rejected &&
                !final.sourceReadErrorObserved[0] && !final.sourceReadErrorObserved[1] &&
                !final.sourceReadError[0] && !final.sourceReadError[1] &&
                !final.starvedFrames[0] && !final.starvedFrames[1],
            "source-handoff: one peer/output clock advances without invalid buffers, rejected controls or source failures");
        if (executionDeadlineGate)
            require(!final.deadlineMisses, "source-handoff: no execution deadline misses");
        save(true);
        return result;
    } catch (const std::exception& error) {
        save(false, error.what());
        failureDiagnostic(session, "source-handoff", error.what());
        throw;
    }
}

void validatePrivateEnvironment(const std::filesystem::path& runtime)
{
    const auto env = [](const char* name) { const auto* value = std::getenv(name); return value ? std::string(value) : std::string{}; };
    const auto remote = env("PIPEWIRE_REMOTE");
    require(env("CONFLUX_PRIVATE_ROUTING_CHECK") == "1" && runtime.is_absolute() &&
            env("XDG_RUNTIME_DIR") == runtime.string() && env("PIPEWIRE_RUNTIME_DIR") == runtime.string() &&
            remote.starts_with("conflux-private-routing-"), "Explicit private runtime environment validated");
    struct stat info{};
    require(::lstat(runtime.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::getuid() &&
            (info.st_mode & 0777) == 0700, "Private runtime is owned and mode 0700");
    const auto socket = runtime / remote;
    require(::lstat(socket.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) && info.st_uid == ::getuid(),
            "Explicit private daemon socket exists and is owned by this user");
}
} // namespace

int main(int argc, char** argv)
{
    std::vector<ScenarioResult> results;
    try {
        if (argc != 3 && (argc != 4 || std::string_view(argv[3]) != "--functional-only"))
            throw std::runtime_error("Usage: manual-routing-check PRIVATE_RUNTIME EMPTY_OUTPUT_DIRECTORY [--functional-only]");
        executionDeadlineGate = argc == 3;
        validatePrivateEnvironment(argv[1]);
        const std::filesystem::path output(argv[2]);
        diagnosticDirectory = output.parent_path();
        require(output.is_absolute() && std::filesystem::is_directory(output) && std::filesystem::is_empty(output),
                "Fresh absolute recording output directory validated");
        pw_init(nullptr, nullptr);
        std::vector<float> a(48000 * 10 * 2), b(a.size());
        for (std::size_t sample = 0; sample < a.size(); sample += 2) {
            a[sample] = .125F; a[sample + 1] = -.25F;
            b[sample] = .0625F; b[sample + 1] = .03125F;
        }
        results.push_back(runTopology(output, false, a, b));
        results.push_back(runTopology(output, true, a, b));
        const auto handoff = runSourceHandoff(output, a);
        std::cout << "{\"passed\":true,\"scope\":\"Private virtual PipeWire graph only; no physical routing, latency or sustained qualification\""
                  << ",\"execution_deadline_gate\":" << (executionDeadlineGate ? "true" : "false")
                  << ",\"startup_sample_scope\":\"Receiver constant-sample validation and recorder attachment start only after ready and at least 1024 output frames. Startup fade samples are excluded; analytic ramp correctness is covered by core tests, not click-free qualification here. Receiver counters are never reset.\""
                  << ",\"startup_warmup_min_output_frames\":" << startupWarmupFrames
                  << ",\"startup_warmup_order\":[\"main\",\"disabled\",\"overlap\",\"missing\"],\"scenarios\":[";
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (i) std::cout << ',';
            const auto& value = results[i];
            std::cout << "{\"name\":" << json(value.name) << ",\"output_frames\":" << value.frames
                      << ",\"callbacks\":" << value.callbacks << ",\"recorded_frames\":" << value.recorded
                      << ",\"execution_deadline_misses\":" << value.deadlineMisses
                      << ",\"driver_position_discontinuities\":" << value.graphDiscontinuities
                      << ",\"driver_recovery_flags\":" << value.recoveryFlags
                      << ",\"driver_recovery_flags_available\":" << (value.recoveryFlagsAvailable ? "true" : "false")
                      << ",\"startup_warmup_observed_output_frames\":[";
            for (std::size_t index = 0; index < value.warmupFrames.size(); ++index) {
                if (index) std::cout << ',';
                std::cout << value.warmupFrames[index];
            }
            std::cout << "]}";
        }
        std::cout << "],\"source_handoff\":";
        writeHandoff(std::cout, handoff, true);
        std::cout << ",\"checks\":[";
        for (std::size_t i = 0; i < checks.size(); ++i) { if (i) std::cout << ','; std::cout << json(checks[i]); }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"passed\":false,\"execution_deadline_gate\":" << (executionDeadlineGate ? "true" : "false")
                  << ",\"error\":" << json(error.what()) << ",\"completed_checks\":" << checks.size() << ",\"checks\":";
        writeChecks(std::cout);
        std::cout << "}\n";
        std::cerr << error.what() << '\n';
        return 1;
    }
}

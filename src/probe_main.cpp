// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"
#include "audio/AudioFile.hpp"

#include <pipewire/pipewire.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <time.h>

namespace {
using conflux::audio::RenderCore;
constexpr std::uint32_t stride = sizeof(float) * RenderCore::channels;
#ifdef SPA_IO_CLOCK_FLAG_XRUN_RECOVER
constexpr bool xrunFlagsAvailable = true;
constexpr bool clockCycleAvailable = true;
#else
constexpr bool xrunFlagsAvailable = false;
constexpr bool clockCycleAvailable = false;
#endif
#ifdef SPA_IO_CLOCK_FLAG_DISCONT
constexpr bool discontinuityFlagAvailable = true;
#else
constexpr bool discontinuityFlagAvailable = false;
#endif
std::uint64_t nowNs() noexcept
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL + time.tv_nsec;
}

struct Probe {
    explicit Probe(bool tone) : renderer(tone) {}
    explicit Probe(std::span<const float> samples) : renderer(samples) {}
    RenderCore renderer;
    pw_main_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    std::atomic<spa_io_position*> position{nullptr};
    std::atomic<bool> callbackFault{false};
    // Written only by the data thread; read only after pw_stream_destroy stops it.
    std::array<std::uint64_t, 100001> histogram{};
    std::uint64_t callbacks = 0, maxNs = 0, deadlineMisses = 0;
    std::uint64_t missingBuffers = 0, invalidBuffers = 0, truncatedFrames = 0;
    std::uint64_t graphDiscontinuities = 0, xrunRecoveryFlags = 0;
    std::uint64_t graphQuantumMin = UINT64_MAX, graphQuantumMax = 0;
    std::uint64_t framesMin = UINT64_MAX, framesMax = 0;
    std::uint64_t previousEnd = 0;
    std::uint32_t previousClock = SPA_ID_INVALID, previousCycle = 0;
    spa_fraction previousRate{};
    std::uint64_t startedNs = 0, connectedNs = 0, stoppedNs = 0;
    unsigned seconds = 10, quantum = 256;
    bool streaming = false, shuttingDown = false, timeValid = false, formatValid = false;
    spa_audio_info_raw format{};
    pw_time timing{};
    std::string failure;
    std::string stopFile;
    std::string inputFile;
    std::uint64_t initialSourceFrame = 0;
    double decodeMs = 0;
};
static_assert(std::atomic<spa_io_position*>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

void process(void* userdata)
{
    auto& data = *static_cast<Probe*>(userdata);
    const auto began = nowNs();
    double deadlineNs = 0;
    if (const auto* position = data.position.load(std::memory_order_acquire)) {
        const auto& clock = position->clock;
        data.graphQuantumMin = std::min(data.graphQuantumMin, clock.duration);
        data.graphQuantumMax = std::max(data.graphQuantumMax, clock.duration);
        if (clock.rate.denom && clock.rate.num)
            deadlineNs = 1.0e9 * clock.duration * clock.rate.num / clock.rate.denom;
        bool sameClock = data.previousClock == clock.id && data.previousRate.num == clock.rate.num &&
            data.previousRate.denom == clock.rate.denom;
#ifdef SPA_IO_CLOCK_FLAG_XRUN_RECOVER
        sameClock = sameClock && data.previousCycle == clock.cycle;
        data.previousCycle = clock.cycle;
#endif
        bool discontinuity = sameClock && data.previousEnd != clock.position;
#ifdef SPA_IO_CLOCK_FLAG_DISCONT
        discontinuity = discontinuity || (clock.flags & SPA_IO_CLOCK_FLAG_DISCONT);
#endif
        if (discontinuity)
            ++data.graphDiscontinuities;
#ifdef SPA_IO_CLOCK_FLAG_XRUN_RECOVER
        if (clock.flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER)
            ++data.xrunRecoveryFlags;
#endif
        data.previousClock = clock.id;
        data.previousRate = clock.rate;
        data.previousEnd = clock.position + clock.duration;
    }
    if (auto* buffer = pw_stream_dequeue_buffer(data.stream)) {
        auto* storage = buffer->buffer;
        if (!storage || storage->n_datas != 1 || !storage->datas[0].data ||
            !storage->datas[0].chunk) {
            ++data.invalidBuffers;
            data.callbackFault.store(true, std::memory_order_relaxed);
            // Keep invalid storage out of the graph. The control loop observes
            // callbackFault and destroys the stream, reclaiming this buffer.
        } else {
            auto& plane = storage->datas[0];
            const std::uint64_t capacity = plane.maxsize / stride;
            const auto requested = buffer->requested ? buffer->requested : capacity;
            const auto frames = std::min(requested, capacity);
            data.truncatedFrames += requested - frames;
            data.framesMin = std::min(data.framesMin, frames);
            data.framesMax = std::max(data.framesMax, frames);
            if (deadlineNs == 0)
                deadlineNs = 1.0e9 * frames / RenderCore::sampleRate;
            data.renderer.render({static_cast<float*>(plane.data),
                                  static_cast<std::size_t>(frames * RenderCore::channels)});
            plane.chunk->offset = 0;
            plane.chunk->stride = stride;
            plane.chunk->size = static_cast<std::uint32_t>(frames * stride);
            buffer->size = frames;
            if (pw_stream_queue_buffer(data.stream, buffer) < 0)
                data.callbackFault.store(true, std::memory_order_relaxed);
        }
    } else {
        ++data.missingBuffers;
    }
    const auto elapsed = nowNs() - began;
    ++data.histogram[std::min<std::uint64_t>(elapsed / 1000, data.histogram.size() - 1)];
    data.maxNs = std::max(data.maxNs, elapsed);
    if (deadlineNs > 0 && static_cast<double>(elapsed) > deadlineNs)
        ++data.deadlineMisses;
    ++data.callbacks;
}

void stateChanged(void* userdata, pw_stream_state, pw_stream_state state, const char* error)
{
    auto& data = *static_cast<Probe*>(userdata);
    if (data.shuttingDown)
        return;
    if (state == PW_STREAM_STATE_STREAMING) {
        data.streaming = true;
        if (!data.startedNs)
            data.startedNs = nowNs();
    } else if (state == PW_STREAM_STATE_ERROR ||
               (data.streaming && state != PW_STREAM_STATE_STREAMING)) {
        data.failure = error ? error : "stream stopped or disconnected";
        pw_main_loop_quit(data.loop);
    }
}

void ioChanged(void* userdata, std::uint32_t id, void* area, std::uint32_t size)
{
    if (id == SPA_IO_Position)
        static_cast<Probe*>(userdata)->position.store(
            size >= sizeof(spa_io_position) ? static_cast<spa_io_position*>(area) : nullptr,
            std::memory_order_release);
}

void paramChanged(void* userdata, std::uint32_t id, const spa_pod* param)
{
    auto& data = *static_cast<Probe*>(userdata);
    if (id != SPA_PARAM_Format || !param)
        return;
    data.formatValid = spa_format_audio_raw_parse(param, &data.format) >= 0 &&
        data.format.format == SPA_AUDIO_FORMAT_F32 && data.format.rate == RenderCore::sampleRate &&
        data.format.channels == RenderCore::channels;
    if (!data.formatValid) {
        data.failure = "server did not negotiate 48000 Hz stereo interleaved F32";
        pw_main_loop_quit(data.loop);
    }
}

void tick(void* userdata, std::uint64_t)
{
    auto& data = *static_cast<Probe*>(userdata);
    const auto now = nowNs();
    pw_time timing{};
    if (data.streaming && pw_stream_get_time_n(data.stream, &timing, sizeof(timing)) == 0) {
        data.timing = timing;
        data.timeValid = true;
    }
    if (data.callbackFault.load(std::memory_order_relaxed))
        data.failure = "invalid output buffer or queue failure";
    if (!data.startedNs && now - data.connectedNs > 5000000000ULL)
        data.failure = "no streaming state within five seconds";
    std::error_code stopError;
    const bool stopRequested = !data.stopFile.empty() && std::filesystem::exists(data.stopFile, stopError);
    if (!data.failure.empty() || stopRequested ||
        (data.startedNs && now - data.startedNs >= data.seconds * 1000000000ULL))
        pw_main_loop_quit(data.loop);
}

void interrupted(void* userdata, int)
{
    auto& data = *static_cast<Probe*>(userdata);
    data.failure = "probe interrupted";
    pw_main_loop_quit(data.loop);
}

double percentile(const Probe& data, double fraction)
{
    if (!data.callbacks)
        return 0;
    const auto rank = static_cast<std::uint64_t>(std::ceil(data.callbacks * fraction));
    std::uint64_t count = 0;
    for (std::size_t i = 0; i < data.histogram.size(); ++i) {
        count += data.histogram[i];
        if (count >= rank)
            return i == data.histogram.size() - 1 ? data.maxNs / 1000.0 : i + 1.0;
    }
    return 0;
}

std::string jsonString(std::string_view value)
{
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char character : value) {
        if (character == '"' || character == '\\')
            result += '\\';
        if (character < 32) {
            result += "\\u00";
            result += hex[character >> 4];
            result += hex[character & 15];
        } else {
            result += static_cast<char>(character);
        }
    }
    return result + '"';
}

void report(const Probe& data, bool tone)
{
    std::cout << std::fixed << std::setprecision(3)
        << "{\n  \"schema\": 1,\n  \"pipewire_version\": " << jsonString(pw_get_library_version())
        << ",\n  \"status\": " << jsonString(data.failure.empty() ? "ok" : "error")
        << ",\n  \"error\": " << jsonString(data.failure)
        << ",\n  \"signal\": " << jsonString(!data.inputFile.empty() ? "local file" :
            tone ? "440 Hz stereo, amplitude 0.01" : "silence")
        << ",\n  \"requested\": {\"sample_rate\": 48000, \"channels\": 2, \"quantum_frames\": "
        << data.quantum << ", \"seconds\": " << data.seconds << "},\n"
        << "  \"negotiated\": {\"format_valid\": " << (data.formatValid ? "true" : "false")
        << ", \"sample_rate\": " << data.format.rate << ", \"channels\": " << data.format.channels
        << ", \"stream_frames_min\": " << (data.framesMax ? data.framesMin : 0)
        << ", \"stream_frames_max\": " << data.framesMax
        << ", \"graph_quantum_min\": " << (data.graphQuantumMax ? data.graphQuantumMin : 0)
        << ", \"graph_quantum_max\": " << data.graphQuantumMax << "},\n"
        << "  \"streaming_seconds\": " << (data.startedNs ? (data.stoppedNs - data.startedNs) / 1.0e9 : 0)
        << ",\n  \"streaming_started_monotonic_ns\": " << data.startedNs
        << ",\n  \"stopped_monotonic_ns\": " << data.stoppedNs
        << ",\n  \"frames_rendered\": " << data.renderer.frameClock()
        << ",\n  \"file_input\": ";
    if (data.inputFile.empty()) {
        std::cout << "null";
    } else {
        std::cout << "{\"path\": " << jsonString(data.inputFile)
            << ", \"decoded_frames\": " << data.renderer.sourceFrames()
            << ", \"decoded_bytes\": " << data.renderer.sourceFrames() * stride
            << ", \"decode_ms\": " << data.decodeMs
            << ", \"initial_source_frame\": " << data.initialSourceFrame
            << ", \"final_source_frame\": " << data.renderer.sourceFrame()
            << ", \"eof\": " << (data.renderer.sourceFrame() == data.renderer.sourceFrames() ? "true" : "false")
            << "}";
    }
    std::cout << ",\n  \"callbacks\": " << data.callbacks
        << ",\n  \"callback_elapsed_us\": {\"p50_upper_bound\": " << percentile(data, .50)
        << ", \"p95_upper_bound\": " << percentile(data, .95)
        << ", \"p99_upper_bound\": " << percentile(data, .99)
        << ", \"p99_9_upper_bound\": " << percentile(data, .999)
        << ", \"max\": " << data.maxNs / 1000.0
        << ", \"histogram_resolution\": 1, \"at_least_100ms\": " << data.histogram.back() << "},\n"
        << "  \"callback_execution_deadline_misses\": " << data.deadlineMisses
        << ",\n  \"missing_buffers\": " << data.missingBuffers
        << ",\n  \"invalid_buffers\": " << data.invalidBuffers
        << ",\n  \"truncated_frames\": " << data.truncatedFrames
        << ",\n  \"observed_graph_position_discontinuities\": " << data.graphDiscontinuities
        << ",\n  \"observed_position_clock_xrun_recovery_flags\": "
        << (xrunFlagsAvailable ? std::to_string(data.xrunRecoveryFlags) : "null")
        << ",\n  \"telemetry_capabilities\": {\"clock_cycle\": "
        << (clockCycleAvailable ? "true" : "false") << ", \"discontinuity_flag\": "
        << (discontinuityFlagAvailable ? "true" : "false") << ", \"xrun_recovery_flags\": "
        << (xrunFlagsAvailable ? "true" : "false") << ", \"graph_position_observed\": "
        << (data.graphQuantumMax ? "true" : "false") << "}"
        << ",\n  \"pipewire_time_snapshot\": {\"valid\": " << (data.timeValid ? "true" : "false")
        << ", \"now_ns\": " << data.timing.now << ", \"rate_num\": " << data.timing.rate.num
        << ", \"rate_denom\": " << data.timing.rate.denom << ", \"ticks\": " << data.timing.ticks
        << ", \"delay_ticks\": " << data.timing.delay << ", \"queued_frames\": " << data.timing.queued
        << ", \"buffered_frames\": " << data.timing.buffered << "},\n"
        << "  \"measurement_scope\": \"Callback execution and PipeWire-reported driver-position clock timing; "
           "not measured end-to-end audio latency, per-stream xrun recovery or a complete graph xrun count.\"\n}\n";
}
} // namespace

int main(int argc, char** argv)
{
    unsigned seconds = 10, quantum = 256;
    bool tone = false, startFrameSet = false;
    std::uint64_t startFrame = 0;
    std::string stopFile, inputFile;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--tone") {
            tone = true;
        } else if (argument == "--help") {
            std::cout << "Usage: conflux-audio-probe [--seconds 1..7200] [--quantum 256|512] [--tone | --input path] [--start-frame N] [--stop-file path]\n"
                         "Default output is silence. Input requires stereo 48000 Hz audio, preloaded within 64 MiB.\n"
                         "Start frame applies only to file input. EOF renders silence. Quantum is requested without forcing the graph.\n";
            return 0;
        } else if (argument == "--input" && i + 1 < argc) {
            inputFile = argv[++i];
            if (inputFile.empty()) {
                std::cerr << "Input path must not be empty\n";
                return 2;
            }
        } else if (argument == "--start-frame" && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            const auto result = std::from_chars(value.begin(), value.end(), startFrame);
            if (result.ec != std::errc{} || result.ptr != value.end()) {
                std::cerr << "Invalid value for --start-frame\n";
                return 2;
            }
            startFrameSet = true;
        } else if (argument == "--stop-file" && i + 1 < argc) {
            stopFile = argv[++i];
        } else if ((argument == "--seconds" || argument == "--quantum") && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            unsigned parsed = 0;
            const auto result = std::from_chars(value.begin(), value.end(), parsed);
            if (result.ec != std::errc{} || result.ptr != value.end() ||
                (argument == "--seconds" ? parsed < 1 || parsed > 7200 : parsed != 256 && parsed != 512)) {
                std::cerr << "Invalid value for " << argument << '\n';
                return 2;
            }
            (argument == "--seconds" ? seconds : quantum) = parsed;
        } else {
            std::cerr << "Unknown or incomplete option: " << argument << '\n';
            return 2;
        }
    }
    if ((!inputFile.empty() && tone) || (startFrameSet && inputFile.empty())) {
        std::cerr << "--input and --tone are exclusive; --start-frame requires --input\n";
        return 2;
    }
    // PCM is owned outside the callback and outlives both renderer and stream.
    std::optional<conflux::audio::DecodedAudio> decoded;
    std::unique_ptr<Probe> data;
    try {
        if (!inputFile.empty()) {
            const auto began = nowNs();
            decoded.emplace(conflux::audio::decodeAudioFile(inputFile));
            const double decodeMs = (nowNs() - began) / 1.0e6;
            data = std::make_unique<Probe>(decoded->samples());
            data->decodeMs = decodeMs;
            data->inputFile = inputFile;
            data->initialSourceFrame = startFrame;
            if (!data->renderer.seek(startFrame)) {
                std::cerr << "Start frame exceeds decoded file length\n";
                return 2;
            }
        } else {
            data = std::make_unique<Probe>(tone);
        }
    } catch (const std::exception& error) {
        std::cerr << "Cannot prepare audio: " << error.what() << '\n';
        return 1;
    }
    pw_init(nullptr, nullptr);
    data->seconds = seconds;
    data->quantum = quantum;
    data->stopFile = std::move(stopFile);
    data->loop = pw_main_loop_new(nullptr);
    if (!data->loop) {
        std::cerr << "Cannot create PipeWire main loop\n";
        pw_deinit();
        return 1;
    }
    auto* loop = pw_main_loop_get_loop(data->loop);
    const auto latency = std::to_string(quantum) + "/48000";
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = stateChanged;
    events.io_changed = ioChanged;
    events.param_changed = paramChanged;
    events.process = process;
    data->stream = pw_stream_new_simple(loop, "CONFLUX M0 audio probe",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_NAME, "conflux-audio-probe",
            PW_KEY_NODE_LATENCY, latency.c_str(), nullptr), &events, data.get());
    if (!data->stream) {
        data->failure = "cannot create PipeWire stream";
    } else {
        std::array<std::uint8_t, 1024> buffer{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
        spa_audio_info_raw format{};
        format.format = SPA_AUDIO_FORMAT_F32;
        format.rate = RenderCore::sampleRate;
        format.channels = RenderCore::channels;
        format.position[0] = SPA_AUDIO_CHANNEL_FL;
        format.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
        const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_DONT_RECONNECT);
        data->connectedNs = nowNs();
        if (pw_stream_connect(data->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
            data->failure = "cannot connect PipeWire stream";
        } else {
            auto* timer = pw_loop_add_timer(loop, tick, data.get());
            auto* interrupt = pw_loop_add_signal(loop, SIGINT, interrupted, data.get());
            auto* terminate = pw_loop_add_signal(loop, SIGTERM, interrupted, data.get());
            timespec interval{0, 100000000};
            if (!timer || !interrupt || !terminate ||
                pw_loop_update_timer(loop, timer, &interval, &interval, false) < 0)
                data->failure = "cannot create control-loop timer or signal sources";
            else if (pw_main_loop_run(data->loop) < 0)
                data->failure = "PipeWire main loop failed";
        }
    }
    data->stoppedNs = nowNs();
    data->shuttingDown = true;
    if (data->stream)
        pw_stream_destroy(data->stream);
    pw_main_loop_destroy(data->loop);
    if (data->failure.empty() && (!data->callbacks || !data->formatValid ||
        data->missingBuffers || data->invalidBuffers || data->truncatedFrames ||
        data->callbackFault.load(std::memory_order_relaxed)))
        data->failure = "probe produced no valid callbacks or observed a buffer fault";
    report(*data, tone);
    pw_deinit();
    return data->failure.empty() ? 0 : 1;
}

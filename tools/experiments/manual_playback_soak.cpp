// SPDX-License-Identifier: AGPL-3.0-or-later
// Opt-in native laptop diagnostics. Default mode accepts generated quiet fixtures only.
#include "ui/DeckController.hpp"
#include "library/LiveStemObservations.hpp"
#include "WindowEventHistory.hpp"
#include "PlaybackUiPolicy.hpp"
#include "PlaybackControlClock.hpp"
#include <sndfile.h>
#include <QAccessible>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr double maximumMasterPeak = 0.000025118864315095795; // -92 dBFS
using State = conflux::audio::LiveSession::State;
void require(bool value, const char* description)
{ if (!value) throw std::runtime_error(description); }
void save(const QString& path, const QJsonObject& value, qsizetype maximumBytes = 0)
{
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot open soak report");
    const auto data = QJsonDocument(value).toJson();
    require(!maximumBytes || data.size() <= maximumBytes, "Soak report exceeds fixed metadata bound");
    require(file.write(data) == data.size() && file.commit(), "Cannot publish soak report");
}
QJsonObject inspectAudio(const QString& path, std::uint64_t maximumFrames)
{
    SF_INFO info{};
    const auto encoded = QFile::encodeName(path);
    auto* input = sf_open(encoded.constData(), SFM_READ, &info);
    require(input, "Cannot inspect generated quiet audio");
    const auto owner = std::unique_ptr<SNDFILE, decltype(&sf_close)>(input, sf_close);
    require(info.channels == 2 && info.samplerate == 48000 && info.frames > 0 &&
            std::uint64_t(info.frames) <= maximumFrames, "Audio must be bounded stereo 48000 Hz");
    std::array<float, 8192> samples{};
    std::array<double, 2> peak{};
    sf_count_t frames = 0;
    std::uint64_t nonzero = 0;
    for (;;) {
        const auto count = sf_readf_float(input, samples.data(), samples.size() / 2);
        if (!count) break;
        for (sf_count_t frame = 0; frame < count; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto value = samples[std::size_t(frame) * 2 + channel];
                require(std::isfinite(value), "Quiet audio contains nonfinite PCM");
                peak[channel] = std::max(peak[channel], std::abs(double(value)));
                nonzero += value != 0.F;
            }
        frames += count;
    }
    require(frames == info.frames && sf_error(input) == SF_ERR_NO_ERROR, "Quiet audio read is incomplete");
    return {{"frames", qint64(frames)}, {"sample_rate", info.samplerate}, {"channels", info.channels},
            {"format", info.format}, {"peak_left", peak[0]}, {"peak_right", peak[1]}, {"nonzero_samples", qint64(nonzero)}};
}
std::uint64_t starved(const State& state) { return state.starvedFrames[0] + state.starvedFrames[1]; }
QJsonObject interval(const State& first, const State& last, const char* kind)
{
    const auto frames = last.outputFrame - first.outputFrame;
    return {{"kind", kind}, {"output_begin", qint64(first.outputFrame)}, {"output_end", qint64(last.outputFrame)},
            {"frames", qint64(frames)}, {"callbacks", qint64(last.callbacks - first.callbacks)},
            {"source_a_begin", qint64(first.sourceFrame[0])}, {"source_a_end", qint64(last.sourceFrame[0])},
            {"source_b_begin", qint64(first.sourceFrame[1])}, {"source_b_end", qint64(last.sourceFrame[1])},
            {"starved_frames", qint64(starved(last) - starved(first))},
            {"starved_a", qint64(last.starvedFrames[0] - first.starvedFrames[0])},
            {"starved_b", qint64(last.starvedFrames[1] - first.starvedFrames[1])},
            {"read_failure_observed", last.sourceReadErrorObserved[0] || last.sourceReadErrorObserved[1]}};
}
QJsonArray timing(const conflux::DeckController& controller)
{
    QJsonArray result;
    for (const auto& summary : controller.completedTiming()) {
        const auto& value = summary.counters;
        result.append(QJsonObject{{"callbacks", qint64(value.callbacks)}, {"maximum_execution_ns", qint64(value.maxExecutionNs)},
            {"execution_deadline_misses", qint64(value.deadlineMisses)}, {"execution_p999_upper_us", summary.p999UpperUs},
            {"execution_p99_upper_us", summary.p99UpperUs}, {"at_least_100ms", qint64(summary.atLeast100ms)},
            {"driver_position_observed", value.graphPositionObserved}, {"driver_position_discontinuities", qint64(value.graphDiscontinuities)},
            {"driver_recovery_flags", qint64(value.xrunRecoveryFlags)},
            {"driver_recovery_flag_mask", qint64(summary.capabilities.xrunRecoveryFlag)},
            {"driver_discontinuity_flag_mask", qint64(summary.capabilities.discontinuityFlag)},
            {"driver_cycle_available", summary.capabilities.cycle},
            {"graph_quantum_min", qint64(value.graphQuantumMin)}, {"graph_quantum_max", qint64(value.graphQuantumMax)}});
    }
    return result;
}

QQuickItem* visualItem(QQuickItem* root, const QString& name)
{
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems())
        if (auto* found = visualItem(child, name)) return found;
    return nullptr;
}

bool sameMix(const State& actual, const State& expected)
{
    return actual.deckGain == expected.deckGain && actual.eqGainDb == expected.eqGainDb &&
        actual.crossfader == expected.crossfader && actual.cueSelected == expected.cueSelected &&
        actual.masterGain == expected.masterGain && actual.cueGain == expected.cueGain &&
        actual.stemGain == expected.stemGain;
}

QJsonObject controlSnapshot(const State& s)
{
    const auto frames = [](const auto& values) {
        return QJsonArray{qint64(values[0]), qint64(values[1])};
    };
    return {{"output_frame", qint64(s.outputFrame)}, {"source_frames", frames(s.sourceFrame)},
        {"source_lengths", frames(s.sourceFrames)}, {"source_generations", frames(s.sourceGeneration)},
        {"transport_revisions", frames(s.transportRevision)}, {"cue_frames", frames(s.cueFrame)},
        {"cue_held", QJsonArray{s.cueHeld[0], s.cueHeld[1]}}, {"cue_preview", QJsonArray{s.cuePreview[0], s.cuePreview[1]}},
        {"playing", QJsonArray{s.playing[0], s.playing[1]}}, {"deck_gains", QJsonArray{s.deckGain[0], s.deckGain[1]}},
        {"eq_db", QJsonArray{QJsonArray{s.eqGainDb[0][0], s.eqGainDb[0][1], s.eqGainDb[0][2]},
            QJsonArray{s.eqGainDb[1][0], s.eqGainDb[1][1], s.eqGainDb[1][2]}}},
        {"crossfader", s.crossfader}, {"cue_selected", QJsonArray{s.cueSelected[0], s.cueSelected[1]}},
        {"master_gain", s.masterGain}, {"cue_gain", s.cueGain},
        {"stem_gains", QJsonArray{QJsonArray{s.stemGain[0][0], s.stemGain[0][1], s.stemGain[0][2], s.stemGain[0][3]},
            QJsonArray{s.stemGain[1][0], s.stemGain[1][1], s.stemGain[1][2], s.stemGain[1][3]}}},
        {"applied", qint64(s.applied)}, {"last_command_id", qint64(s.lastCommandId)},
        {"callbacks", qint64(s.callbacks)}, {"invalid_buffers", qint64(s.invalidBuffers)}, {"rejected", qint64(s.rejected)},
        {"starved", frames(s.starvedFrames)}, {"read_failure", QJsonArray{s.sourceReadErrorObserved[0], s.sourceReadErrorObserved[1]}}};
}

// Benchmark-only actual controller/service path. The private variant is never
// host qualification and requires the same isolated daemon ownership checks.
int livePlayback(QGuiApplication& app, int argc, char** argv)
{
    if (argc != 13) { std::cerr << "Usage: --live-stems|--live-stems-controls|--live-stems-controls-normal-redraw|--private-live-stems-controls|--private-live-stems-controls-normal-redraw|--live-stems-hour|--live-stems-background|--live-stems-hour-background|--live-stems-hidden|--live-stems-hidden-one-thread SOURCE PEER OUTPUT SECONDS STOP PYTHON EXPORTER WORKER SERVICE RUNTIME MODEL\n"; return 2; }
    const auto mode = QString::fromLocal8Bit(argv[1]);
    const bool privateControls = mode == "--private-live-stems-controls" ||
        mode == "--private-live-stems-controls-normal-redraw";
    const bool normalRedraw = mode == "--live-stems-controls-normal-redraw" ||
        mode == "--private-live-stems-controls-normal-redraw";
    const bool activeControls = privateControls || normalRedraw || mode == "--live-stems-controls";
    const bool hour = mode == "--live-stems-hour" || mode == "--live-stems-hour-background";
    const bool oneThread = mode == "--live-stems-hidden-one-thread";
    const bool hidden = oneThread || mode == "--live-stems-hidden";
    const int runtimeThreads = oneThread ? 1 : 2;
    const bool background = hidden || mode == "--live-stems-background" || mode == "--live-stems-hour-background";
    using UiPolicy = conflux::experiments::PlaybackUiPolicy;
    UiPolicy uiPolicy(hidden ? UiPolicy::Mode::Hidden : background ? UiPolicy::Mode::Background : UiPolicy::Mode::Visible);
    const bool functional = qEnvironmentVariable("CONFLUX_PRIVATE_ROUTING_CHECK") == "1";
    // Private model diagnostics use the host control schedule but cannot enter
    // host qualification or extend the existing generated-fixture modes.
    if (privateControls && !functional) return 2;
    const bool shortFunctional = functional && !privateControls;
    const bool fixedTwoMinute = privateControls || (normalRedraw && !functional);
    bool valid = false;
    const int seconds = QString::fromLocal8Bit(argv[5]).toInt(&valid);
    if (!valid || (hour ? seconds != 3600 : fixedTwoMinute ? seconds != 120 : shortFunctional ? seconds < 1 || seconds > 10 : seconds < 120 || seconds > 180) ||
        (!functional && qEnvironmentVariable("CONFLUX_ALLOW_HOST_SOAK") != "1")) return 2;
    const auto platform = QGuiApplication::platformName();
    if (functional ? (platform != "offscreen" && platform != "minimal") : (platform == "offscreen" || platform == "minimal")) return 2;
    const auto output = QString::fromLocal8Bit(argv[4]), stop = QString::fromLocal8Bit(argv[6]);
    const auto recordingPath = output + "/master.wav";
    if (qEnvironmentVariable("XDG_CACHE_HOME") != output + "/cache" || !QFileInfo(output + "/cache").isDir()) return 2;
    try {
        require(QFileInfo(output).isAbsolute() && QFileInfo(output).isDir() &&
            !QFileInfo::exists(recordingPath) && !QFileInfo::exists(output + "/observations.jsonl"), "Need fresh absolute live evidence paths");
        if (functional) {
            const auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR"), remote = qEnvironmentVariable("PIPEWIRE_REMOTE");
            struct stat info{};
            const auto encoded = QFile::encodeName(runtime), socket = QFile::encodeName(runtime + "/" + remote);
            require(QFileInfo(runtime).isAbsolute() && qEnvironmentVariable("PIPEWIRE_RUNTIME_DIR") == runtime &&
                remote.startsWith("conflux-private-routing-") && !remote.contains('/') &&
                ::lstat(encoded.constData(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::getuid() &&
                (info.st_mode & 0777) == 0700 && ::lstat(socket.constData(), &info) == 0 &&
                S_ISSOCK(info.st_mode) && info.st_uid == ::getuid(), "Explicit owned private PipeWire runtime/socket required");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    QQuickStyle::setStyle("Basic");
    for (const auto* font : {"Barlow-Regular.ttf", "BarlowCondensed-SemiBold.ttf", "JetBrainsMono-Regular.ttf"})
        QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/") + font);
    QTemporaryDir appData(output + "/data-XXXXXX");
    if (!appData.isValid() || !qputenv("XDG_DATA_HOME", appData.path().toUtf8())) return 2;
    conflux::DeckController::PreparationOptions options;
    conflux::library::StemEngine stem;
    stem.pythonExecutable = QString::fromLocal8Bit(argv[7]); stem.exporterPath = QString::fromLocal8Bit(argv[8]);
    stem.workerPath = QString::fromLocal8Bit(argv[9]); stem.runtimePath = QString::fromLocal8Bit(argv[11]);
    stem.modelPath = QString::fromLocal8Bit(argv[12]); stem.backend = "vulkan"; stem.threads = runtimeThreads; stem.requestTimeout = 30; options.stemEngine = stem;
    options.stemServicePath = QString::fromLocal8Bit(argv[10]);
    conflux::DeckController controller(options);
    using Observations = conflux::library::LiveStemObservations;
    using Kind = Observations::Kind;
    std::shared_ptr<Observations> observations;
    QFile trace(output + "/observations.jsonl");
    if (!trace.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
    std::uint64_t retained = 0, recordCount = 0, bytes = 0, acceptedGeneration = 0, completedGeneration = 0;
    std::uint64_t acceptedEpoch = 0, completedEpoch = 0, converterGeneration = 0, converterEpoch = 0, validatedHops = 0;
    auto write = [&](QJsonObject value) {
        auto line = QJsonDocument(value).toJson(QJsonDocument::Compact); line.append('\n');
        require(line.size() <= 4096 && retained < (hour ? 65536U : 16384U) &&
            bytes + std::uint64_t(line.size()) <= (hour ? 64ULL : 32ULL) * 1024 * 1024,
            "Live observation evidence capacity exhausted");
        require(trace.write(line) == line.size(), "Cannot write bounded live observation evidence");
        ++retained; bytes += std::uint64_t(line.size());
    };
    auto drain = [&] {
        if (!observations) return;
        Observations::Record row;
        for (std::size_t count = 0; count < Observations::capacity && observations->tryPop(row); ++count) {
            if (row.kind == Kind::CommandAccepted) { acceptedGeneration = row.workerGeneration; acceptedEpoch = row.providerEpoch; }
            if (row.kind == Kind::RawComplete) { completedGeneration = row.workerGeneration; completedEpoch = row.providerEpoch; }
            if (row.kind == Kind::ConverterComplete) { converterGeneration = row.workerGeneration; converterEpoch = row.providerEpoch; }
            if (row.kind == Kind::ChunkValidated) ++validatedHops;
            QJsonObject value{{"type", "record"}, {"kind", int(row.kind)}, {"ordinal", qint64(row.ordinal)}, {"observed_ns", qint64(row.observedNs)},
                {"epoch", qint64(row.providerEpoch)}, {"generation", qint64(row.workerGeneration)}, {"command_id", qint64(row.commandId)},
                {"request_id", qint64(row.requestId)}, {"lease_sequence", qint64(row.leaseSequence)},
                {"input_first", qint64(row.inputFirst44100)}, {"input_frames", qint64(row.inputFrames44100)},
                {"output_first", qint64(row.outputFirst48000)}, {"output_frames", qint64(row.outputFrames48000)},
                {"final", row.final}, {"validated_ns", qint64(row.validatedNs)}, {"packet_ready_ns", qint64(row.packetReadyNs)},
                {"full_observed", row.fullObserved}, {"first_full_ns", qint64(row.firstFullNs)},
                {"publish_begin_ns", qint64(row.publishBeginNs)}, {"publish_end_ns", qint64(row.publishEndNs)},
                {"worker_ready_seconds", row.workerReadySeconds}, {"service_published_seconds", row.servicePublishedSeconds},
                {"hop_service_present", row.hopServicePresent}, {"hop_service_ms", row.hopServiceMs},
                {"telemetry_dropped_present", row.serviceTelemetryDroppedPresent}, {"telemetry_dropped", qint64(row.serviceTelemetryDropped)}};
            const auto phase = [&](const char* key, const std::optional<double>& milliseconds) {
                if (milliseconds) value.insert(QLatin1String(key), *milliseconds);
            };
            phase("source_read_ms", row.sourceReadMs); phase("request_ms", row.requestMs); phase("overlap_ms", row.overlapMs);
            phase("native_elapsed_ms", row.nativeElapsedMs); phase("native_inference_wall_ms", row.nativeInferenceWallMs);
            phase("native_inference_cpu_ms", row.nativeInferenceCpuMs); phase("native_frontend_ms", row.nativeFrontendMs);
            phase("native_graph_ms", row.nativeGraphMs); phase("native_graph_upload_ms", row.nativeGraphUploadMs);
            phase("native_graph_compute_ms", row.nativeGraphComputeMs); phase("native_graph_readback_ms", row.nativeGraphReadbackMs);
            phase("native_postprocess_ms", row.nativePostprocessMs); phase("native_graph_rebuild_ms", row.nativeGraphRebuildMs);
            write(value);
            ++recordCount;
        }
    };
    const auto nowNs = []() -> qint64 { return qint64( std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
    const auto sha = [](const Observations::Sha256Hex& value) { return QString::fromLatin1(value.data(), qsizetype(value.size())); };
    QElapsedTimer clock, measure; clock.start();
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("deckController", &controller);
    engine.rootContext()->setContextProperty("initialDark", true);
    engine.rootContext()->setContextProperty("qtVersion", QString::fromUtf8(qVersion()));
    engine.rootContext()->setContextProperty("platformName", platform);
    QPointer<QQuickWindow> window; QTimer framePump;
    std::uint64_t presented = 0, eofCount = 0, replayCount = 0;
    qint64 measuredWallMs = 0;
    std::uint64_t measurementBeginOutputFrame = 0, measurementEndOutputFrame = 0, measurementBeginPeerFrame = 0;
    bool measurementIntervalsClosed = false;
    qint64 lastFrameNs = 0, maxFrameGapNs = 0, lastHeartbeatNs = 0, maxHeartbeatNs = 0, loadIssuedNs = 0, firstAdvanceNs = 0;
    QJsonArray samples, controls, peerControls, sceneErrors, intervals;
    QJsonObject setup, result{{"schema", 2}, {"mode", hour ? "live-stems-hour" : "live-stems"}, {"functional_only", functional}, {"host_audio", !functional},
        {"pid", qint64(QCoreApplication::applicationPid())}, {"requested_seconds", seconds}, {"platform", platform}, {"qt", qVersion()},
        {"ui_mode", background ? "background" : "visible"},
        {"window_policy", hidden ? "hidden" : background ? "background" : "visible"},
        {"quick_backend_environment", qEnvironmentVariable("QT_QUICK_BACKEND")}, {"source_root", QStringLiteral(CONFLUX_SOURCE_DIR)}, {"qualified", false},
        {"scope", "Short actual controller/service playback diagnostic; no sustained p99, physical latency, automatic fallback or musical-quality qualification"}};
    if (hour) result["scope"] = "Sustained two-deck actual controller/service fixed-mix evidence; wrapper qualifies only after timing/health/cleanup gates; other M2 requirements remain open";
    if (background) result["scope"] = hour
        ? "Sustained actual controller/service background audio/stem diagnostic; no visible-UI or full-player qualification"
        : "Short actual controller/service background audio/stem diagnostic; no visible-UI, sustained p99 or full-player qualification";
    if (activeControls) {
        result["mode"] = "live-stems-controls";
        result["scope"] = "Short actual-player accessible UI controls under load; no pointer/touch, physical assistive-technology, hardware-cue or sustained qualification";
    }
    result["forced_redraw_interval_ms"] = normalRedraw ? 50 : 16;
    if (normalRedraw) {
        result["mode"] = "live-stems-controls-normal-redraw";
        result["scope"] = "Explicit 50ms forced-redraw diagnostic; default16ms stress load unchanged; no sustained or normal-stress qualification";
    }
    if (privateControls) {
        result["mode"] = normalRedraw ? "private-live-stems-controls-normal-redraw" : "private-live-stems-controls";
        result["scope"] = "Two-minute actual-model/UI controls diagnostic on private virtual audio; no host, physical audio, sustained or laptop qualification";
    }
    QJsonArray manualControls;
    State mixBaseline{}, expectedMix{}, pendingMixBefore{}, pendingControlBefore{};
    QPointer<QQuickItem> pendingItem;
    int nextControl = 0;
    bool controlPending = false;
    std::uint64_t controlCommand = 0, controlSession = 0, controlPresented = 0;
    qint64 controlIssued = 0;
    double expectedUi = 0;
    bool expectedChecked = false;
    bool cancelled = false, seekIssued = false, recovering = false, replayPending = false;
    bool peerRecovering = false;
    qint64 peerIssuedNs = 0;
    std::uint64_t peerAppliedTarget = 0;
    QString failure, measurementCompletionFailure;
    State boundary{}, last{};
    State tailBegin{}, tailEnd{};
    QJsonObject diagnosticTail;
    bool tailStarted = false;
    std::uint64_t eofAtOutput = 0, recoveryTarget = 0;
    QString phase = "startup";
    auto windowSnapshot = [&](qint64 now) {
        const bool exists = bool(window), visible = window && window->isVisible(), exposed = window && window->isExposed();
        if (window) uiPolicy.observeActivity({exists, visible, exposed, presented != 0, 0});
        return QJsonObject{{"exists", exists}, {"visible", visible}, {"exposed", exposed},
            {"visibility", window ? QJsonValue(int(window->visibility())) : QJsonValue()},
            {"window_state", window ? QJsonValue(int(window->windowState())) : QJsonValue()},
            {"active", window && window->isActive()}, {"application_state", int(QGuiApplication::applicationState())},
            {"window_object_identity", window ? QJsonValue(QString::number(quintptr(window.data()), 16)) : QJsonValue()},
            {"clock_ns", now}, {"monotonic_ns", nowNs()}, {"phase", phase.left(64)}, {"last_frame_ns", lastFrameNs},
            {"frame_age_ns", lastFrameNs ? QJsonValue(now - lastFrameNs) : QJsonValue()}, {"frames_presented", qint64(presented)},
            {"last_heartbeat_ns", lastHeartbeatNs}, {"heartbeat_age_ns", lastHeartbeatNs ? QJsonValue(now - lastHeartbeatNs) : QJsonValue()},
            {"failed_predicates", QJsonObject{{"missing_window", !exists}, {"not_visible", !visible}, {"not_exposed", !exposed},
                {"frame_age_at_least_2s", now - lastFrameNs >= 2000000000LL}}}};
    };
    conflux::experiments::WindowEventHistory windowEvents([&] { return windowSnapshot(clock.nsecsElapsed()); },
        {{"elapsed_clock", "QElapsedTimer::nsecsElapsed"}, {"elapsed_clock_type", int(QElapsedTimer::clockType())},
         {"elapsed_clock_is_monotonic", QElapsedTimer::isMonotonic()}, {"anchor_clock_ns", clock.nsecsElapsed()},
         {"anchor_monotonic_ns", nowNs()}, {"anchor_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
         {"clock_anchor_scope", "Sequential GUI-thread elapsed/steady/UTC observations, not simultaneous hardware timestamps"},
         {"frame_timestamp_scope", "frameSwapped slot observation; not compositor presentation or physical scanout"},
         {"heartbeat_timestamp_scope", "Most recent measured-loop health observation after qWait"},
         {"source_scope", "event_filter_before_target is pre-handler; signal is Qt notification; snapshot is polling, not an event"},
         {"capture_end", "Before controller shutdown and window close"}, {"functional_ui_bypass", functional && !hidden},
         {"frame_age_limit_ns", 2000000000LL}, {"startup_minimum_frames", hidden ? 0 : 10}, {"startup_wait_ms", hidden ? 0 : 5000},
         {"hidden_activity_latched", hidden},
         {"activation_required", false}});
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &windowEvents, [&](Qt::ApplicationState value) {
        windowEvents.record("signal:applicationStateChanged", {{"signal_value", int(value)}});
    });
    auto checkStop = [&] { if (QFileInfo::exists(stop)) { cancelled = true; throw std::runtime_error("Stop file requested graceful live cancellation"); } };
    auto checkHidden = [&] {
        if (!hidden) return;
        const UiPolicy::State state{bool(window), window && window->isVisible(), window && window->isExposed(), presented != 0, 0};
        uiPolicy.observeActivity(state);
        require(uiPolicy.healthy(functional, state), "Controlled hidden window became visible, exposed, rendered or missing");
    };
    auto pulse = [&] {
        const auto state = controller.audioState();
        windowEvents.record("snapshot:progress");
        save(output + "/progress.json", {{"phase", phase}, {"pid", qint64(QCoreApplication::applicationPid())}, {"monotonic_ns", nowNs()},
            {"elapsed_ms", clock.elapsed()}, {"frames_presented", qint64(presented)}, {"window_exposed", window && window->isExposed()},
            {"output_ready", controller.outputReady()}, {"output_frame", qint64(state.outputFrame)}, {"callbacks", qint64(state.callbacks)},
            {"recording", QJsonObject::fromVariantMap(controller.recording())}});
    };
    auto wait = [&](const std::function<bool()>& predicate, int timeout = 10000) {
        QElapsedTimer deadline; deadline.start();
        for (;;) {
            checkStop(); drain(); checkHidden();
            if (predicate()) return;
            if (!controller.error().isEmpty()) throw std::runtime_error(controller.error().toStdString());
            require(deadline.elapsed() < timeout, "Live controller acknowledgement timed out");
            QTest::qWait(10);
        }
    };
    auto command = [&](const std::function<void()>& action) {
        const auto target = controller.submittedCommands() + 1; action();
        wait([&] { return controller.completedApplied() + controller.audioState().applied >= target; }, 5000);
        require(controller.error().isEmpty(), "Live transport submission failed");
    };
    auto appendMeasuredInterval = [&](const State& state, bool aborted = false) {
        auto value = interval(boundary, state, phase.toUtf8().constData());
        if (hour) { value["a_recovery"] = recovering; value["b_recovery"] = peerRecovering; }
        require(!hour || intervals.size() < 2048, "Hour interval capacity exhausted");
        value["aborted"] = aborted; intervals.append(value); boundary = state;
    };
    auto closeMeasurement = [&](bool aborted) {
        if (!measure.isValid() || measurementIntervalsClosed) return;
        const auto state = controller.audioState();
        appendMeasuredInterval(state, aborted); measurementEndOutputFrame = state.outputFrame;
        measurementIntervalsClosed = true;
    };
    auto beginRecovery = [&](double target, const char* kind) {
        const auto state = controller.audioState();
        appendMeasuredInterval(state);
        phase = QString::fromLatin1(kind); recovering = true;
        recoveryTarget = std::uint64_t(std::llround(target * 48000));
        controls.append(QJsonObject{{"kind", phase}, {"issued_ns", nowNs()}, {"target_frame", qint64(recoveryTarget)},
            {"output_begin", qint64(state.outputFrame)}, {"previous_generation", qint64(acceptedGeneration)},
            {"incomplete_when_last_drained", acceptedGeneration && acceptedGeneration != completedGeneration}});
        require(!hour || controls.size() <= 512, "Hour A control capacity exhausted");
        command([&] { controller.seekSeconds(0, target); });
    };
    try {
        phase = "validating"; pulse();
        const auto peer = inspectAudio(QString::fromLocal8Bit(argv[3]), 48000ULL * 600);
        require(hour ? peer["frames"].toInteger() == 48000LL * 480 :
            peer["frames"].toInteger() >= 48000LL * (seconds + (shortFunctional ? 2 : 120)), "Finite peer has wrong duration for playback mode");
        result["peer"] = peer;
        if (hidden) engine.setInitialProperties({{"visible", false}});
        engine.load(QUrl::fromLocalFile(QStringLiteral(CONFLUX_SOURCE_DIR "/qml/Main.qml")));
        require(!engine.rootObjects().isEmpty(), "Live Main.qml did not load");
        window = qobject_cast<QQuickWindow*>(engine.rootObjects().first()); require(window, "Live QML has no window");
        require(!hidden || (!window->isVisible() && !window->isExposed()), "Controlled hidden window appeared at startup");
        windowEvents.observe(window);
        QObject::connect(window, &QWindow::visibleChanged, &windowEvents, [&](bool value) {
            windowEvents.record("signal:visibleChanged", {{"signal_value", value}});
        });
        QObject::connect(window, &QWindow::visibilityChanged, &windowEvents, [&](QWindow::Visibility value) {
            windowEvents.record("signal:visibilityChanged", {{"signal_value", int(value)}});
        });
        QObject::connect(window, &QWindow::activeChanged, &windowEvents, [&] { windowEvents.record("signal:activeChanged"); });
        QObject::connect(window, &QWindow::windowStateChanged, &windowEvents, [&](Qt::WindowState value) {
            windowEvents.record("signal:windowStateChanged", {{"signal_value", int(value)}});
        });
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [&] {
            const auto now = clock.nsecsElapsed(); if (lastFrameNs) maxFrameGapNs = std::max(maxFrameGapNs, now - lastFrameNs);
            lastFrameNs = now; ++presented;
            uiPolicy.observeActivity({bool(window), window && window->isVisible(), window && window->isExposed(), true, 0});
        });
        QObject::connect(window, &QQuickWindow::sceneGraphError, &app, [&](QQuickWindow::SceneGraphError code, const QString& message) {
            if (sceneErrors.size() < 32) sceneErrors.append(QJsonObject{{"code", int(code)}, {"message", message.left(1024)}});
        });
        framePump.setInterval(normalRedraw ? 50 : 16); QObject::connect(&framePump, &QTimer::timeout, window, [window] { if (window) window->update(); }); framePump.start();
        if (uiPolicy.requiresStartupPresentation(functional)) wait([&] { return window->isExposed() && presented >= 10; }, 5000);
        if (functional) {
            QString left, right;
            controller.refreshAudioRouting();
            wait([&] {
                for (const auto& entry : controller.audioRouting()["ports"].toList()) {
                    const auto port = entry.toMap(); const auto label = port["label"].toString();
                    if (label == "conflux-short-live-test-sink · input_0 [FL]") left = port["key"].toString();
                    if (label == "conflux-short-live-test-sink · input_1 [FR]") right = port["key"].toString();
                }
                return !left.isEmpty() && !right.isEmpty();
            });
            controller.configureAudioRouting(left, right, {}, {});
        }
        phase = "loading_peer"; pulse(); controller.loadDeck(1, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[3])));
        wait([&] { return !controller.busy() && controller.outputReady(); }, 30000);
        command([&] { controller.setMasterGain(.25); });
        command([&] { controller.setPlaying(1, true); });
        controller.startRecording(QUrl::fromLocalFile(recordingPath));
        wait([&] { return controller.recording()["status"].toString() == "Recording"; });
        phase = "loading_live"; pulse(); loadIssuedNs = nowNs();
        controller.loadStemDeck(0, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[2])));
        wait([&] { return !controller.busy() && controller.outputReady() && controller.liveStemObservations(0) &&
            controller.audioState().sourceFrames[0] == controller.liveStemObservations(0)->setup().outputFrames48000; }, 90000);
        observations = controller.liveStemObservations(0);
        require(observations && !controller.liveStemObservations(1) && !controller.preparation()["cacheHit"].toBool() &&
            controller.decks()[0].toMap()["overviewStem"].toString() == "source", "Check needs a real live cache miss and finite peer");
        const auto& context = observations->setup();
        setup = {{"factory_entry_ns", qint64(context.factoryEntryNs)}, {"input_captured_ns", qint64(context.inputCapturedNs)},
            {"count_complete_ns", qint64(context.countCompleteNs)}, {"factory_committed_ns", qint64(context.factoryCommittedNs)},
            {"source_token", qint64(context.sourceToken)}, {"input_frames", qint64(context.inputFrames44100)}, {"output_frames", qint64(context.outputFrames48000)},
            {"source_sha256", sha(context.sourceSha256)}, {"input_sha256", sha(context.inputSha256)}, {"model_sha256", sha(context.modelSha256)},
            {"runtime_sha256", sha(context.runtimeSha256)}, {"service_sha256", sha(context.serviceSha256)}};
        require(context.outputFrames48000 && context.outputFrames48000 <= 48000ULL * 30 &&
            context.outputFrames48000 == controller.audioState().sourceFrames[0], "Use a bounded short source for EOF/replay");
        require(!hour || (context.inputFrames44100 == 1031940 && context.outputFrames48000 == 1123201),
            "Hour mode requires the same counted 23.4-second source geometry");
        write({{"type", "setup"}, {"context", setup}}); drain();
        command([&] { controller.setDeckGain(0, .25); }); command([&] { controller.setPlaying(0, true); });
        const auto route = controller.audioRouting();
        require(!route["cueReady"].toBool() && route["cueLeft"].toString().isEmpty() && route["cueRight"].toString().isEmpty(), "Hardware cue must remain disabled");
        mixBaseline = expectedMix = controller.audioState();
        require(!activeControls || (mixBaseline.crossfader == .5F &&
            mixBaseline.eqGainDb == std::array<std::array<float, 3>, 2>{}), "Controls check requires the original neutral EQ and centered crossfader");
        if (activeControls) result["manual_controls_baseline"] = controlSnapshot(mixBaseline);
        measure.start(); phase = "steady"; boundary = last = controller.audioState();
        measurementBeginOutputFrame = boundary.outputFrame;
        measurementBeginPeerFrame = boundary.sourceFrame[1];
        qint64 nextSample = 0, nextPulse = 0; lastHeartbeatNs = clock.nsecsElapsed();
        while (measure.elapsed() < seconds * 1000LL) {
            checkStop(); drain(); QTest::qWait(10);
            const auto now = clock.nsecsElapsed(); maxHeartbeatNs = std::max(maxHeartbeatNs, now - lastHeartbeatNs); lastHeartbeatNs = now;
            const auto state = controller.audioState();
            require(controller.outputReady() && controller.error().isEmpty() && sceneErrors.isEmpty(), "Live audio/UI error");
            require(!state.sourceReadErrorObserved[0] && !state.sourceReadErrorObserved[1] && !state.invalidBuffers && !state.rejected,
                "Live source or callback failed");
            require(controller.recording()["active"].toBool() && controller.recording()["error"].toString().isEmpty(), "Live recording stopped or failed");
            const conflux::experiments::PlaybackUiPolicy::State uiState{bool(window), window && window->isVisible(),
                window && window->isExposed(), lastFrameNs != 0, now - lastFrameNs};
            uiPolicy.observe(uiState);
            const bool windowHealthy = uiPolicy.healthy(functional, uiState);
            if (!windowHealthy) {
                result["window_failure"] = windowSnapshot(now);
                windowEvents.record("snapshot:ui_guard_failure", result["window_failure"].toObject());
            }
            require(windowHealthy, hidden ? "Controlled hidden window became visible, exposed or missing" : "Live window hidden or not presenting");
            require(controller.liveStemObservations(0) == observations, "Live playback unexpectedly changed owner/observations");
            require(state.graphRate == 48000 && state.streamFramesMin && state.streamFramesMax >= state.streamFramesMin &&
                state.masterGain == .25F && (activeControls ? (sameMix(state, expectedMix) ||
                    (controlPending && sameMix(state, pendingMixBefore))) : state.deckGain[0] == .25F && state.deckGain[1] == 1.F) && state.stemBank[0] && !state.stemBank[1] &&
                state.stemGain[0] == std::array<float, 4>{1.F, 1.F, 1.F, 1.F} && state.playing[0] && state.playing[1],
                "Negotiated audio/gain/transport configuration changed");
            require(!activeControls || (!state.cueReady && controller.audioRouting()["cueLeft"].toString().isEmpty() &&
                controller.audioRouting()["cueRight"].toString().isEmpty()), "Headphone selection unexpectedly changed hardware cue routing");
            if (controlPending) {
                require(pendingItem && controller.submittedCommands() == controlCommand && controller.completedSessions() == controlSession &&
                    state.sourceGeneration == pendingControlBefore.sourceGeneration && state.transportRevision == pendingControlBefore.transportRevision &&
                    state.cueFrame == pendingControlBefore.cueFrame && state.cueHeld == pendingControlBefore.cueHeld &&
                    state.cuePreview == pendingControlBefore.cuePreview && state.sourceFrames == pendingControlBefore.sourceFrames &&
                    state.outputFrame >= pendingControlBefore.outputFrame, "Nontransport UI control changed source/transport ownership");
                require(conflux::experiments::playbackControlClockMatches(pendingControlBefore, state),
                    "Nontransport UI control lost the exact source/output clock");
                const bool button = nextControl % 8 >= 6;
                const bool uiAck = button ? pendingItem->property("checked").toBool() == expectedChecked :
                    std::abs(pendingItem->property("value").toDouble() - expectedUi) < 1e-6;
                if (state.lastCommandId == controlCommand && controller.completedApplied() + state.applied >= controlCommand &&
                    sameMix(state, expectedMix) && uiAck && presented > controlPresented) {
                    auto row = manualControls.last().toObject(); row["acknowledged"] = true;
                    row["ack_poll_ns"] = nowNs(); row["ack"] = controlSnapshot(state);
                    row["ui_value"] = button ? QJsonValue(expectedChecked) : QJsonValue(pendingItem->property("value").toDouble());
                    row["presented_after"] = qint64(presented); row["session_after"] = qint64(controller.completedSessions());
                    manualControls[manualControls.size() - 1] = row; ++nextControl; controlPending = false;
                } else require(nowNs() - controlIssued < 5000000000LL, "Accessible UI control acknowledgement timed out");
            }
            require(state.outputFrame >= last.outputFrame && (peerRecovering || (state.sourceFrame[1] >= last.sourceFrame[1] &&
                state.sourceFrame[1] - last.sourceFrame[1] == state.outputFrame - last.outputFrame)), "Finite peer lost source/output alignment");
            if (!firstAdvanceNs && !seekIssued && state.sourceFrame[0] && !state.buffering[0]) firstAdvanceNs = nowNs();
            if (recovering && state.sourceFrame[0] > recoveryTarget && !state.buffering[0]) {
                appendMeasuredInterval(state); recovering = false; replayPending = false; phase = "steady";
                auto control = controls.last().toObject(); control["recovered_cursor_observed_ns"] = nowNs();
                control["output_end"] = qint64(state.outputFrame); controls[controls.size() - 1] = control;
            }
            if (peerRecovering && nowNs() - peerIssuedNs >= 2000000000LL && controller.completedApplied() + state.applied >= peerAppliedTarget &&
                state.sourceFrame[1] > 0 && !state.buffering[1]) {
                appendMeasuredInterval(state); peerRecovering = false;
                auto control = peerControls.last().toObject(); control["settled_ns"] = nowNs();
                control["output_end"] = qint64(state.outputFrame); control["source_after"] = qint64(state.sourceFrame[1]);
                peerControls[peerControls.size() - 1] = control;
            }
            // B has already played during preparation. Its real cursor owns the
            // rewind deadline, independently of A's live seek/replay recovery.
            if (hour && !peerRecovering && state.sourceFrame[1] >= 420ULL * 48000) {
                const auto before = controller.audioState(); appendMeasuredInterval(before); peerRecovering = true;
                peerIssuedNs = nowNs(); peerAppliedTarget = controller.submittedCommands() + 1;
                command([&] { controller.seekSeconds(1, 0); });
                const auto after = controller.audioState();
                require(peerControls.size() < 9, "Hour peer control capacity exhausted");
                peerControls.append(QJsonObject{{"kind", "peer_rewind"}, {"target_frame", 0}, {"issued_ns", peerIssuedNs},
                    {"output_begin", qint64(before.outputFrame)}, {"source_before", qint64(before.sourceFrame[1])},
                    {"ack_output_frame", qint64(after.outputFrame)}, {"ack_source_frame", qint64(after.sourceFrame[1])}});
            }
            const bool eof = state.sourceFrame[0] == state.sourceFrames[0];
            if (eof && !eofAtOutput && !replayPending) { ++eofCount; eofAtOutput = state.outputFrame; }
            if (eofAtOutput && state.outputFrame >= eofAtOutput + 1024 && !recovering && !controlPending && measure.elapsed() < seconds * 1000LL - (shortFunctional ? 200 : 5000)) {
                beginRecovery(0, "replay_recovery"); ++replayCount; replayPending = true; eofAtOutput = 0;
            } else if (replayPending && !eof) replayPending = false;
            if (!seekIssued && firstAdvanceNs && !recovering && !controlPending && !eof && measure.elapsed() < seconds * 1000LL - (shortFunctional ? 200 : 15000) && acceptedGeneration && acceptedGeneration != completedGeneration) {
                auto target = std::min<std::uint64_t>(280800, state.sourceFrames[0] / 2);
                if (target == state.sourceFrame[0]) target = 0;
                beginRecovery(double(target) / 48000, "incomplete_generation_seek"); seekIssued = true;
            }
            // Mix changes remain in the original steady interval: they never exempt starvation.
            if (activeControls && nextControl < 16 && !controlPending && !recovering && !replayPending &&
                !state.buffering[0] && !state.buffering[1] && !eof) {
                const int deck = nextControl / 8, step = nextControl % 8, control = step / 2;
                const bool restore = step % 2;
                const qint64 due = shortFunctional ? (deck ? 7000 : 500) + step * 100 : (deck ? 70000 : 10000) + step * 2000;
                if (measure.elapsed() >= due) {
                    const auto name = control == 0 ? QString("deck%1Gain").arg(deck) : control == 1 ? QString("crossfader") :
                        control == 2 ? QString("deck%1EqHI").arg(deck) : QString("deck%1CueSelect").arg(deck);
                    pendingItem = visualItem(window->contentItem(), name);
                    require(pendingItem && pendingItem->isEnabled() && pendingItem->isVisible(), "Scheduled accessible UI control is unavailable");
                    auto* accessible = QAccessible::queryAccessibleInterface(pendingItem);
                    require(accessible, "Scheduled UI control has no accessible interface");
                    pendingControlBefore = controller.audioState(); pendingMixBefore = expectedMix;
                    controlSession = controller.completedSessions(); controlCommand = controller.submittedCommands() + 1;
                    controlPresented = presented; controlIssued = nowNs();
                    if (control == 0) { expectedUi = restore ? mixBaseline.deckGain[deck] : mixBaseline.deckGain[deck] / 2.; expectedMix.deckGain[deck] = float(expectedUi); }
                    else if (control == 1) { expectedUi = restore ? mixBaseline.crossfader : deck ? .75 : .25; expectedMix.crossfader = float(expectedUi); }
                    else if (control == 2) { expectedUi = restore ? .5 : .375; expectedMix.eqGainDb[deck][2] = restore ? 0.F : -6.5F; }
                    else { expectedChecked = restore ? mixBaseline.cueSelected[deck] : !mixBaseline.cueSelected[deck]; expectedMix.cueSelected[deck] = expectedChecked; }
                    require(manualControls.size() < 16, "Manual controls evidence capacity exceeded");
                    manualControls.append(QJsonObject{{"index", nextControl}, {"deck", deck}, {"step", step}, {"object", name},
                        {"due_ms", due}, {"issued_elapsed_ms", measure.elapsed()}, {"issued_ns", controlIssued},
                        {"expected_command", qint64(controlCommand)}, {"session_before", qint64(controlSession)},
                        {"presented_before", qint64(presented)}, {"before", controlSnapshot(pendingControlBefore)},
                        {"expected", controlSnapshot(expectedMix)}, {"acknowledged", false}});
                    controlPending = true;
                    if (control == 3) {
                        auto* action = accessible->actionInterface();
                        require(action && action->actionNames().contains(QAccessibleActionInterface::pressAction()), "Headphone CUE has no accessible press action");
                        action->doAction(QAccessibleActionInterface::pressAction());
                    } else {
                        auto* numeric = accessible->valueInterface();
                        require(numeric && numeric->minimumValue().toDouble() == 0 && numeric->maximumValue().toDouble() == 1,
                            "Scheduled numeric control has unexpected accessible range");
                        numeric->setCurrentValue(expectedUi);
                    }
                    require(controller.submittedCommands() == controlCommand, "Accessible UI action did not submit exactly one command");
                }
            }
            if (measure.elapsed() >= nextSample) {
                require(!hour || samples.size() < 362, "Hour snapshot capacity exhausted");
                samples.append(QJsonObject{{"elapsed_ms", measure.elapsed()}, {"phase", phase}, {"frames_presented", qint64(presented)},
                    {"output_frame", qint64(state.outputFrame)}, {"source_a", qint64(state.sourceFrame[0])}, {"source_b", qint64(state.sourceFrame[1])},
                    {"starved_a", qint64(state.starvedFrames[0])}, {"starved_b", qint64(state.starvedFrames[1])}, {"snapshot_drops", qint64(state.snapshotDrops)},
                    {"buffering_a", state.buffering[0]}, {"graph_rate", qint64(state.graphRate)}, {"quantum_min", qint64(state.streamFramesMin)},
                    {"quantum_max", qint64(state.streamFramesMax)}, {"master_gain", state.masterGain}, {"deck_a_gain", state.deckGain[0]}, {"deck_b_gain", state.deckGain[1]}, {"master_peak", std::max(state.meters.master.peak[0], state.meters.master.peak[1])}});
                if (hour) nextSample = measure.elapsed() + 10000;
                else nextSample += 1000;
            }
            if (measure.elapsed() >= nextPulse) { pulse(); nextPulse += 2000; }
            last = state;
        }
        measuredWallMs = measure.elapsed();
        closeMeasurement(false);
        drain();
        require(!activeControls || (nextControl == 16 && !controlPending && sameMix(controller.audioState(), mixBaseline)),
            "Accessible controls sequence or baseline restoration incomplete");
        // Retain a failed boundary without cancelling its pending generation.
        // The original finite drain can still capture its slow final hops.
        if (!(firstAdvanceNs && eofCount && replayCount && !recovering && !replayPending && !peerRecovering))
            measurementCompletionFailure = "Playback/EOF/replay or last scheduled recovery incomplete";
        // The fixed measured/WAV interval ends here. Finish only this finite
        // generation; no tail replay/seek and no time-based hop filtering.
        tailStarted = true; tailBegin = tailEnd = controller.audioState();
        const auto tailEpoch = acceptedEpoch, tailGeneration = acceptedGeneration;
        const auto tailDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        QElapsedTimer tailClock; tailClock.start();
        diagnosticTail = {{"status", "WaitingForFiniteEof"}, {"epoch", qint64(tailEpoch)}, {"generation", qint64(tailGeneration)},
            {"begin_ns", nowNs()}, {"tail_budget_seconds", 60}, {"close_budget_seconds", 3}, {"measured_boundary_closed", true},
            {"measured_wall_ms_begin", measuredWallMs}, {"measured_output_end_begin", qint64(measurementEndOutputFrame)},
            {"validated_hops_begin", qint64(validatedHops)},
            {"scope", "Measured interval is closed; WAV finalized before EOF/idle-close drain, whose later audio is unrecorded; every completed hop retained"}};
        require(tailEpoch && tailGeneration, "Finite diagnostic tail has no accepted generation");
        phase = "diagnostic_tail"; pulse();
        qint64 nextTailPulse = 2000;
        auto tailProgressAt = std::chrono::steady_clock::now();
        auto tailLastOutput = tailBegin.outputFrame;
        bool tailOutputAdvanced = false;
        auto tailTick = [&] {
            checkStop(); drain(); QTest::qWait(10); drain(); checkHidden();
            const auto now = clock.nsecsElapsed(); maxHeartbeatNs = std::max(maxHeartbeatNs, now - lastHeartbeatNs); lastHeartbeatNs = now;
            tailEnd = controller.audioState();
            diagnosticTail["elapsed_ms"] = tailClock.elapsed(); diagnosticTail["end_ns"] = nowNs();
            require(std::chrono::steady_clock::now() < tailDeadline, "Finite diagnostic tail deadline expired");
            if (tailEnd.outputFrame > tailLastOutput) {
                tailLastOutput = tailEnd.outputFrame; tailProgressAt = std::chrono::steady_clock::now(); tailOutputAdvanced = true;
                diagnosticTail["last_output_advance_ns"] = nowNs();
            }
            diagnosticTail["output_progress_observed"] = tailOutputAdvanced;
            require(std::chrono::steady_clock::now() - tailProgressAt < std::chrono::seconds(10),
                "Finite diagnostic tail output did not advance for ten seconds");
            require(controller.liveStemObservations(0) == observations && acceptedGeneration == tailGeneration && acceptedEpoch == tailEpoch,
                "Finite diagnostic tail changed live owner, generation or epoch");
            require(controller.outputReady() && controller.error().isEmpty() && sceneErrors.isEmpty() &&
                !tailEnd.sourceReadErrorObserved[0] && !tailEnd.sourceReadErrorObserved[1] && !tailEnd.invalidBuffers && !tailEnd.rejected,
                "Finite diagnostic tail audio/UI/source failed");
            require(tailEnd.graphRate == 48000 && tailEnd.masterGain == .25F && tailEnd.deckGain[0] == .25F && tailEnd.deckGain[1] == 1.F &&
                tailEnd.stemBank[0] && !tailEnd.stemBank[1] && tailEnd.stemGain[0] == std::array<float, 4>{1.F, 1.F, 1.F, 1.F} &&
                tailEnd.playing[0] && tailEnd.playing[1], "Finite diagnostic tail configuration changed");
            require(tailEnd.outputFrame >= tailBegin.outputFrame && tailEnd.sourceFrame[0] >= tailBegin.sourceFrame[0] &&
                tailEnd.sourceFrame[1] >= tailBegin.sourceFrame[1] &&
                tailEnd.sourceFrame[1] - tailBegin.sourceFrame[1] == tailEnd.outputFrame - tailBegin.outputFrame,
                "Finite diagnostic tail lost source/output continuity");
            // Keep the failed audio interval, but finish the bounded finite
            // generation so a slow final hop is not censored by starvation.
            if (tailEnd.starvedFrames != tailBegin.starvedFrames || tailEnd.deadlineMisses != tailBegin.deadlineMisses) {
                diagnosticTail["audio_health_failed"] = true;
                if (!diagnosticTail.contains("first_audio_health_failure_ns"))
                    diagnosticTail["first_audio_health_failure_ns"] = nowNs();
            }
            const UiPolicy::State uiState{bool(window), window && window->isVisible(), window && window->isExposed(),
                lastFrameNs != 0, now - lastFrameNs};
            uiPolicy.observe(uiState);
            require(uiPolicy.healthy(functional, uiState), "Finite diagnostic tail window policy failed");
            if (tailClock.elapsed() >= nextTailPulse) { pulse(); nextTailPulse += 2000; }
        };
        controller.stopRecording();
        const auto recordingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (controller.recording()["active"].toBool()) {
            tailTick();
            require(std::chrono::steady_clock::now() < recordingDeadline, "Live WAV finalization timed out");
        }
        require(controller.recording()["status"].toString() == "Complete", "Live WAV did not finalize");
        diagnosticTail["WAV_finalized_before_idle_close"] = true;
        diagnosticTail["recording_written_frames_begin"] = qint64(controller.recording()["writtenFrames"].toULongLong());
        auto finiteEof = [&] {
            return completedGeneration == tailGeneration && completedEpoch == tailEpoch &&
                converterGeneration == tailGeneration && converterEpoch == tailEpoch &&
                tailEnd.sourceFrame[0] == tailEnd.sourceFrames[0];
        };
        do { tailTick(); } while (!finiteEof() || !tailOutputAdvanced);
        diagnosticTail["raw_complete"] = true; diagnosticTail["converter_complete"] = true; diagnosticTail["render_eof"] = true;
        require(observations->requestDiagnosticDrain(tailEpoch, tailGeneration, tailDeadline), "Diagnostic idle-close request rejected");
        diagnosticTail["status"] = "WaitingForIdleClose"; diagnosticTail["close_requested_ns"] = nowNs(); pulse();
        using DrainState = Observations::DiagnosticDrainState;
        while (observations->diagnosticDrainStatus().state == DrainState::Requested) tailTick();
        require(observations->diagnosticDrainStatus().state == DrainState::ReadyForStop, "Diagnostic idle close failed");
        diagnosticTail["status"] = "ReadyForStop"; diagnosticTail["end_ns"] = nowNs();
        diagnosticTail["elapsed_ms"] = tailClock.elapsed();
        phase = "finalizing";
    } catch (const std::exception& error) {
        failure = QString::fromUtf8(error.what());
        const auto detail = controller.error();
        if (!detail.isEmpty() && !failure.contains(detail))
            failure = (failure + "\n" + detail).left(qsizetype(Observations::failureDetailCapacity));
        if (measure.isValid() && !measurementIntervalsClosed) measuredWallMs = measure.elapsed();
        closeMeasurement(true);
        if (tailStarted) { diagnosticTail["status"] = "Failed"; diagnosticTail["error"] = failure; }
    }
    if (phase != "finalizing") { phase = "finalizing"; pulse(); }
    windowEvents.freeze(result);
    if (hidden && failure.isEmpty()) {
        try { checkHidden(); } catch (const std::exception& error) { failure = QString::fromUtf8(error.what()); }
    }
    result["ui_coverage"] = uiPolicy.report();
    framePump.stop(); controller.shutdown(); result["shutdown_completed"] = true; if (window) window->close();
    if (tailStarted) {
        tailEnd = controller.audioState();
        const auto delta = [](std::uint64_t after, std::uint64_t before) -> QJsonValue {
            return after >= before ? QJsonValue(qint64(after - before)) : QJsonValue{};
        };
        diagnosticTail["starved_a"] = delta(tailEnd.starvedFrames[0], tailBegin.starvedFrames[0]);
        diagnosticTail["starved_b"] = delta(tailEnd.starvedFrames[1], tailBegin.starvedFrames[1]);
        diagnosticTail["callback_deadline_misses"] = delta(tailEnd.deadlineMisses, tailBegin.deadlineMisses);
        diagnosticTail["output_begin"] = qint64(tailBegin.outputFrame); diagnosticTail["output_end"] = qint64(tailEnd.outputFrame);
        diagnosticTail["source_read_error"] = tailEnd.sourceReadErrorObserved[0] || tailEnd.sourceReadErrorObserved[1];
        diagnosticTail["invalid_buffers"] = qint64(tailEnd.invalidBuffers);
        diagnosticTail["drain_state"] = int(observations->diagnosticDrainStatus().state);
        diagnosticTail["recording_written_frames_end"] = qint64(controller.recording()["writtenFrames"].toULongLong());
        diagnosticTail["measured_wall_ms_end"] = measuredWallMs;
        diagnosticTail["measured_output_end_end"] = qint64(measurementEndOutputFrame);
    }
    try {
        drain(); require(trace.flush(), "Cannot flush live evidence"); trace.close();
        const auto counts = observations ? observations->counters() : Observations::Counters{};
        result["observation_counters"] = QJsonObject{{"attempted", qint64(counts.attemptedRecords)}, {"dropped", qint64(counts.droppedRecords)},
            {"invalid_clock_reads", qint64(counts.invalidClockReads)}, {"producer_finished", counts.producerFinished}, {"retained", qint64(recordCount)}, {"bytes", qint64(bytes)}};
        if (observations) require(counts.producerFinished && !counts.droppedRecords && !counts.invalidClockReads &&
            counts.attemptedRecords == recordCount, "Live observation interval is incomplete");
        const auto recording = controller.recording();
        if (tailStarted) {
            diagnosticTail["validated_hops_end"] = qint64(validatedHops);
            require(diagnosticTail["recording_written_frames_begin"].toInteger(-1) ==
                diagnosticTail["recording_written_frames_end"].toInteger(-2), "Recording changed during finite diagnostic drain");
        }
        require(!failure.isEmpty() || QFileInfo::exists(recordingPath), "Completed live run has no recording");
        result["recording"] = QJsonObject::fromVariantMap(recording);
        if (QFileInfo::exists(recordingPath)) {
            const auto wav = inspectAudio(recordingPath, 48000ULL * (seconds + 120)); result["recorded_audio"] = wav;
            require(recording["acceptedFrames"].toULongLong() == recording["writtenFrames"].toULongLong() &&
                recording["writtenFrames"].toULongLong() == std::uint64_t(wav["frames"].toInteger()) &&
                !recording["overflowFrames"].toULongLong() && wav["nonzero_samples"].toInteger() &&
                (wav["format"].toInt() & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV &&
                (wav["format"].toInt() & SF_FORMAT_SUBMASK) == SF_FORMAT_FLOAT, "Live recording count/format/overflow failed");
        }
        require(controller.completedApplied() == controller.submittedCommands() && !controller.completedRejected() &&
            !controller.completedInvalidBuffers() && !controller.completedReadFailures(), "Live final application counters failed");
        // Inspect and retain the finalized WAV before rejecting tail health.
        // The same zero-starvation/deadline gate still determines success.
        if (tailStarted) {
            require(diagnosticTail["starved_a"].toInteger(-1) == 0 && diagnosticTail["starved_b"].toInteger(-1) == 0 &&
                diagnosticTail["callback_deadline_misses"].toInteger(-1) == 0 && !diagnosticTail["source_read_error"].toBool() &&
                !diagnosticTail["invalid_buffers"].toInteger() && diagnosticTail["output_progress_observed"].toBool() &&
                observations->diagnosticDrainStatus().state == Observations::DiagnosticDrainState::ReadyForStop,
                "Finite diagnostic tail final health/close failed");
        }
    } catch (const std::exception& error) { if (failure.isEmpty()) failure = QString::fromUtf8(error.what()); }
    if (!measurementCompletionFailure.isEmpty()) {
        result["measurement_completion_error"] = measurementCompletionFailure;
        failure = (measurementCompletionFailure + (failure.isEmpty() ? QString{} : "\n" + failure))
            .left(qsizetype(Observations::failureDetailCapacity));
    }
    if (tailStarted) {
        if (!failure.isEmpty()) { diagnosticTail["status"] = "Failed"; diagnosticTail["error"] = failure; }
        result["diagnostic_tail"] = diagnosticTail;
    }
    std::uint64_t steadyStarved = 0, controlStarved = 0;
    std::array<std::uint64_t, 2> steadyByDeck{};
    for (const auto& entry : intervals) {
        const auto value = entry.toObject();
        if (hour) {
            for (std::size_t deck = 0; deck < 2; ++deck) {
                const auto count = std::uint64_t(value[deck ? "starved_b" : "starved_a"].toInteger());
                if (value[deck ? "b_recovery" : "a_recovery"].toBool()) controlStarved += count;
                else { steadyStarved += count; steadyByDeck[deck] += count; }
            }
        } else (value["kind"].toString() == "steady" ? steadyStarved : controlStarved) += std::uint64_t(value["starved_frames"].toInteger());
    }
    result["steady_starved_frames"] = qint64(steadyStarved); result["control_starved_frames"] = qint64(controlStarved);
    if (hour) { result["steady_starved_a"] = qint64(steadyByDeck[0]); result["steady_starved_b"] = qint64(steadyByDeck[1]); }
    const auto totalStarved = controller.completedStarvedFrames(), measuredStarved = steadyStarved + controlStarved;
    result["starved_frames_outside_measured_intervals"] = totalStarved >= measuredStarved
        ? QJsonValue(qint64(totalStarved - measuredStarved)) : QJsonValue();
    if (failure.isEmpty() && measuredStarved > totalStarved) failure = "Measured starvation exceeds completed total";
    if (failure.isEmpty() && steadyStarved) failure = "Steady playback starved; control intervals retained separately";
    if (failure.isEmpty() && !functional && controller.completedDeadlineMisses()) failure = "Application callback deadline misses observed";
    if (hour && failure.isEmpty()) {
        const auto wall = double(measuredWallMs) / 1000;
        if (std::abs(double(measurementEndOutputFrame - measurementBeginOutputFrame) / 48000 - wall) > std::max(.25, wall * .002))
            failure = "Hour rendered/output duration differs from measured wall beyond 250ms/0.2 percent";
        for (const auto& summary : controller.completedTiming())
            if (!summary.counters.graphPositionObserved || summary.counters.graphDiscontinuities || summary.counters.xrunRecoveryFlags)
                failure = "Hour driver continuity/recovery failed";
    }
    result["configuration"] = QJsonObject{{"backend", "vulkan"}, {"threads", runtimeThreads}, {"request_timeout_seconds", 30},
        {"prime_bound_seconds", 60}, {"deck_a_gain", .25}, {"deck_b_gain", 1.0}, {"master_gain", .25}, {"stem_gains", QJsonArray{1, 1, 1, 1}}};
    result["last_recovery_complete"] = !recovering && !replayPending && !peerRecovering;
    result["setup"] = setup; result["load_issued_ns"] = loadIssuedNs; result["first_advance_observed_ns"] = firstAdvanceNs;
    result["polling_consumption_scope"] = "First positive cursor observation is an upper bound; publication does not prove consumption";
    result["eof_count"] = qint64(eofCount); result["replay_count"] = qint64(replayCount); result["incomplete_generation_seek_submitted"] = seekIssued;
    result["incomplete_generation_seek_scope"] = "Accepted generation without observed RawComplete; native inference-start is not observed";
    result["measured_wall_ms"] = measuredWallMs;
    result["measurement_begin_output_frame"] = qint64(measurementBeginOutputFrame);
    if (hour) result["measurement_begin_peer_frame"] = qint64(measurementBeginPeerFrame);
    result["measurement_end_output_frame"] = qint64(measurementEndOutputFrame);
    result["measurement_intervals_closed"] = measurementIntervalsClosed;
    result["samples"] = samples; result["controls"] = controls; result["intervals"] = intervals; result["scene_errors"] = sceneErrors;
    if (activeControls) {
        result["manual_controls"] = manualControls; result["manual_controls_complete"] = nextControl == 16 && !controlPending;
        result["manual_controls_scope"] = "Actual Qt accessible value/press actions; ACK polling upper bounds. Mix controls remain steady starvation-gated; no pointer/touch or physical cue claim.";
    }
    if (hour) result["peer_controls"] = peerControls;
    result["frames_presented"] = qint64(presented); result["maximum_frame_presentation_gap_ns"] = maxFrameGapNs;
    result["maximum_gui_heartbeat_gap_ns"] = maxHeartbeatNs; result["timing"] = timing(controller);
    result["callbacks"] = qint64(controller.completedCallbacks()); result["invalid_buffers"] = qint64(controller.completedInvalidBuffers());
    result["rejected_commands"] = qint64(controller.completedRejected()); result["source_read_failures"] = qint64(controller.completedReadFailures());
    result["execution_deadline_misses"] = qint64(controller.completedDeadlineMisses());
    result["starved_frames_including_controls"] = qint64(totalStarved);
    result["cancelled"] = cancelled; result["error"] = failure; result["passed"] = failure.isEmpty() && !cancelled;
    try { save(output + "/native-result.json", result, hour ? 1024 * 1024 : 0); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return cancelled ? 2 : failure.isEmpty() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc > 1 && (QString::fromLocal8Bit(argv[1]) == "--live-stems" || QString::fromLocal8Bit(argv[1]) == "--live-stems-controls" ||
        QString::fromLocal8Bit(argv[1]) == "--live-stems-controls-normal-redraw" ||
        QString::fromLocal8Bit(argv[1]) == "--private-live-stems-controls" ||
        QString::fromLocal8Bit(argv[1]) == "--private-live-stems-controls-normal-redraw" ||
        QString::fromLocal8Bit(argv[1]) == "--live-stems-hour" ||
        QString::fromLocal8Bit(argv[1]) == "--live-stems-background" || QString::fromLocal8Bit(argv[1]) == "--live-stems-hour-background" ||
        QString::fromLocal8Bit(argv[1]) == "--live-stems-hidden" || QString::fromLocal8Bit(argv[1]) == "--live-stems-hidden-one-thread"))
        return livePlayback(app, argc, argv);
    if (argc != 6) {
        std::cerr << "Usage: manual-playback-soak QUIET_A.wav QUIET_B.wav OUTPUT_DIRECTORY SECONDS STOP_FILE\n";
        return 2;
    }
    bool durationValid = false;
    const auto seconds = QString::fromLocal8Bit(argv[4]).toInt(&durationValid);
    if (!durationValid || seconds < 60 || seconds > 3600 || qEnvironmentVariable("CONFLUX_ALLOW_HOST_SOAK") != "1")
        return 2;
    if (QGuiApplication::platformName() == "offscreen" || QGuiApplication::platformName() == "minimal")
        return 2;
    const auto output = QString::fromLocal8Bit(argv[3]);
    const auto stopFile = QString::fromLocal8Bit(argv[5]);
    const auto recordingPath = output + "/master.wav";
    QQuickStyle::setStyle("Basic");
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/Barlow-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/BarlowCondensed-SemiBold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/JetBrainsMono-Regular.ttf"));
    QTemporaryDir appData;
    if (!appData.isValid() || !qputenv("XDG_DATA_HOME", appData.path().toUtf8())) return 2;
    conflux::DeckController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("deckController", &controller);
    engine.rootContext()->setContextProperty("initialDark", true);
    engine.rootContext()->setContextProperty("qtVersion", QString::fromUtf8(qVersion()));
    engine.rootContext()->setContextProperty("platformName", QGuiApplication::platformName());
    QElapsedTimer clock;
    clock.start();
    QElapsedTimer measure;
    std::uint64_t presented = 0;
    qint64 lastPresentationNs = 0, maximumPresentationGapNs = 0;
    qint64 lastHeartbeatNs = 0, maximumHeartbeatGapNs = 0;
    QJsonArray sceneErrors, samples, intervals;
    QJsonObject result{{"schema", 1}, {"requested_seconds", seconds}, {"pid", qint64(QCoreApplication::applicationPid())},
        {"qt", qVersion()}, {"platform", QGuiApplication::platformName()},
        {"quick_backend_environment", qEnvironmentVariable("QT_QUICK_BACKEND")},
        {"wall_render_tolerance", "Greater of 250ms or 0.2 percent; duration comparison is not physical latency"},
        {"scope", "Native quiet-fixture two-deck audio/UI playback and recording; no physical latency, headphone isolation or stem qualification"}};
    QPointer<QQuickWindow> window;
    QTimer framePump;
    bool measuring = false, cancelled = false;
    QString failure;
    State measurementStart{}, measurementEnd{};
    qint64 measuredWallMs = 0;
    auto checkStop = [&] {
        if (QFileInfo::exists(stopFile)) { cancelled = true; throw std::runtime_error("Stop file requested graceful cancellation"); }
    };
    auto wait = [&](const std::function<bool()>& predicate, int timeout = 30000) {
        QElapsedTimer deadline; deadline.start();
        while (!predicate() && deadline.elapsed() < timeout) { checkStop(); QTest::qWait(10); }
        require(predicate(), "Timed out waiting for native UI/audio acknowledgement");
    };
    auto pulse = [&](const char* phase) {
        const auto state = controller.audioState();
        save(output + "/progress.json", {{"phase", phase}, {"pid", qint64(QCoreApplication::applicationPid())},
            {"monotonic_ns", qint64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count())},
            {"elapsed_ms", clock.elapsed()}, {"frames_presented", qint64(presented)},
            {"window_exposed", window && window->isExposed()}, {"output_ready", controller.outputReady()},
            {"output_frame", qint64(state.outputFrame)}, {"callbacks", qint64(state.callbacks)},
            {"recording", QJsonObject::fromVariantMap(controller.recording())}});
    };
    try {
        require(QFileInfo(output).isDir() && !QFileInfo::exists(recordingPath), "Need an output directory and a new recording path");
        pulse("validating");
        const auto a = inspectAudio(QString::fromLocal8Bit(argv[1]), 48000ULL * 600);
        const auto b = inspectAudio(QString::fromLocal8Bit(argv[2]), 48000ULL * 600);
        require(a["frames"].toInteger() >= 48000LL * 480 && b["frames"].toInteger() >= 48000LL * 480 &&
                a["frames"].toInteger() <= 48000LL * 600 && b["frames"].toInteger() <= 48000LL * 600,
                "Use generated quiet fixtures between eight and ten minutes (within the decoder budget)");
        const double peakA = std::max(a["peak_left"].toDouble(), a["peak_right"].toDouble());
        const double peakB = std::max(b["peak_left"].toDouble(), b["peak_right"].toDouble());
        require(a["nonzero_samples"].toInteger() > 0 && b["nonzero_samples"].toInteger() > 0 &&
                .5 * (peakA + peakB) < maximumMasterPeak, "Independent fixture sum exceeds -92 dBFS master bound");
        result["fixtures"] = QJsonArray{a, b};
        result["master_peak_bound"] = .5 * (peakA + peakB);
        engine.load(QUrl::fromLocalFile(QStringLiteral(CONFLUX_SOURCE_DIR "/qml/Main.qml")));
        require(!engine.rootObjects().isEmpty(), "Manual QML failed to load");
        window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        require(window, "Manual QML did not create a native window");
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [&] {
            const auto now = clock.nsecsElapsed();
            if (measuring && lastPresentationNs) maximumPresentationGapNs = std::max(maximumPresentationGapNs, now - lastPresentationNs);
            lastPresentationNs = now; ++presented;
        });
        QObject::connect(window, &QQuickWindow::sceneGraphError, &app,
            [&](QQuickWindow::SceneGraphError code, const QString& message) {
                sceneErrors.append(QJsonObject{{"code", int(code)}, {"message", message}});
            });
        framePump.setInterval(16);
        QObject::connect(&framePump, &QTimer::timeout, window, [window] { if (window) window->update(); });
        framePump.start();
        wait([&] { return window->isExposed() && presented >= 10; }, 5000);
        result["early_presented_frame_gate"] = qint64(presented);
        pulse("loading");
        controller.loadDeck(0, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
        wait([&] { return !controller.busy() && controller.outputReady(); });
        controller.loadDeck(1, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[2])));
        wait([&] { return !controller.busy() && controller.outputReady(); });
        auto command = [&](const std::function<void()>& action) {
            const auto target = controller.audioState().applied + 1;
            action();
            wait([&] { return controller.audioState().applied >= target; }, 5000);
            require(controller.error().isEmpty(), "Transport command failed");
        };
        command([&] { controller.setMasterGain(.5); });
        command([&] { controller.setCueGain(.5); });
        command([&] { controller.setCueSelected(0, true); });
        command([&] { controller.setCueSelected(1, true); });
        command([&] { controller.setPlaying(0, true); });
        command([&] { controller.setPlaying(1, true); });
        wait([&] { const auto state = controller.audioState(); return !state.buffering[0] && !state.buffering[1] &&
            state.meters.cue.peak[0] > 0 && state.meters.master.peak[0] > 0; }, 5000);
        const auto route = controller.audioRouting();
        require(!route["cueReady"].toBool() && route["cueLeft"].toString().isEmpty() && route["cueRight"].toString().isEmpty(),
                "Physical headphone cue must stay disabled for this host soak");
        controller.startRecording(QUrl::fromLocalFile(recordingPath));
        wait([&] { return controller.recording()["status"].toString() == "Recording"; }, 10000);
        measurementStart = controller.audioState();
        auto boundary = measurementStart;
        bool settling = false;
        qint64 settlingStarted = 0, nextSeek = 420000, nextSample = 10000, nextPulse = 0;
        std::uint64_t seekTarget = 0, lastSamplePresented = presented;
        measure.start();
        measuring = true; maximumPresentationGapNs = maximumHeartbeatGapNs = 0;
        lastPresentationNs = lastHeartbeatNs = clock.nsecsElapsed();
        while (measure.elapsed() < qint64(seconds) * 1000) {
            checkStop();
            QTest::qWait(20);
            const auto now = clock.nsecsElapsed(), elapsed = measure.elapsed();
            maximumHeartbeatGapNs = std::max(maximumHeartbeatGapNs, now - lastHeartbeatNs); lastHeartbeatNs = now;
            require(window && window->isVisible() && window->isExposed(), "Native soak window became hidden or unexposed");
            require(now - lastPresentationNs < 2000000000LL, "Native UI stopped presenting frames for two seconds");
            require(sceneErrors.isEmpty(), "Native scene-graph error during soak");
            require(controller.error().isEmpty() && controller.outputReady(), "Audio output failed during soak");
            require(controller.recording()["error"].toString().isEmpty() && controller.recording()["active"].toBool(),
                    "Recording failed or stopped during soak");
            const auto state = controller.audioState();
            require(state.graphRate == 48000 && state.streamFramesMin == 256 && state.streamFramesMax == 256 &&
                    !state.invalidBuffers && !state.deadlineMisses && !state.rejected, "Audio format/quantum or callback fault during soak");
            require(!state.sourceReadError[0] && !state.sourceReadError[1] &&
                    !state.sourceReadErrorObserved[0] && !state.sourceReadErrorObserved[1], "Prepared source read failed during soak");
            require(state.playing[0] && state.playing[1] && !state.cueReady, "Deck play or disabled cue routing changed");
            require(!controller.busy() && state.sourceGeneration == measurementStart.sourceGeneration &&
                    state.masterGain == .5F && state.deckGain[0] == 1.F && state.deckGain[1] == 1.F &&
                    state.cueGain == .5F && state.cueSelected[0] && state.cueSelected[1],
                    "An unscheduled source/gain/cue control changed the soak");
            if (!settling && elapsed >= nextSeek) {
                intervals.append(interval(boundary, state, "steady_no_controls"));
                boundary = state;
                seekTarget = state.applied + 2;
                controller.seekSeconds(0, 0); controller.seekSeconds(1, 0);
                require(controller.error().isEmpty(), "Periodic seek submission failed");
                settling = true; settlingStarted = elapsed; nextSeek += 420000;
            } else if (settling && state.applied >= seekTarget && !state.buffering[0] && !state.buffering[1] &&
                       elapsed - settlingStarted >= 2000) {
                intervals.append(interval(boundary, state, "seek_settling"));
                boundary = state; settling = false;
            }
            if (elapsed >= nextPulse) { pulse(settling ? "seek_settling" : "steady"); nextPulse += 2000; }
            if (elapsed >= nextSample) {
                require(presented >= lastSamplePresented + 10, "Too few actual native frames in a ten-second observation");
                samples.append(QJsonObject{{"elapsed_ms", elapsed}, {"frames_presented", qint64(presented)},
                    {"output_frame", qint64(state.outputFrame)}, {"callbacks", qint64(state.callbacks)},
                    {"source_a_frame", qint64(state.sourceFrame[0])}, {"source_b_frame", qint64(state.sourceFrame[1])},
                    {"starved_frames", qint64(starved(state))}, {"settling", settling},
                    {"master_peak", std::max(state.meters.master.peak[0], state.meters.master.peak[1])},
                    {"cue_peak", std::max(state.meters.cue.peak[0], state.meters.cue.peak[1])},
                    {"snapshot_drops", qint64(state.snapshotDrops)},
                    {"recording", QJsonObject::fromVariantMap(controller.recording())}});
                lastSamplePresented = presented; nextSample += 10000;
            }
        }
        measuredWallMs = measure.elapsed(); measurementEnd = controller.audioState();
        intervals.append(interval(boundary, measurementEnd, settling ? "seek_settling" : "steady_no_controls"));
        measuring = false;
        controller.stopRecording();
        wait([&] { return !controller.recording()["active"].toBool(); }, 20000);
        require(controller.recording()["status"].toString() == "Complete", "Recording did not finalize successfully");
    } catch (const std::exception& error) {
        failure = QString::fromUtf8(error.what());
        if (measure.isValid()) measuredWallMs = measure.elapsed();
        measurementEnd = controller.audioState();
        measuring = false;
    }
    framePump.stop();
    controller.shutdown();
    if (window) window->close();
    const auto recording = controller.recording();
    result["recording"] = QJsonObject::fromVariantMap(recording);
    result["completed_sessions"] = qint64(controller.completedSessions());
    result["submitted_commands"] = qint64(controller.submittedCommands());
    result["applied_commands"] = qint64(controller.completedApplied());
    result["callbacks"] = qint64(controller.completedCallbacks());
    result["invalid_buffers"] = qint64(controller.completedInvalidBuffers());
    result["rejected_commands"] = qint64(controller.completedRejected());
    result["execution_deadline_misses"] = qint64(controller.completedDeadlineMisses());
    result["source_read_failures"] = qint64(controller.completedReadFailures());
    result["timing"] = timing(controller);
    result["intervals"] = intervals; result["samples"] = samples; result["scene_errors"] = sceneErrors;
    result["frames_presented"] = qint64(presented);
    result["maximum_frame_presentation_gap_ns"] = maximumPresentationGapNs;
    result["maximum_gui_heartbeat_gap_ns"] = maximumHeartbeatGapNs;
    result["measured_wall_ms"] = measuredWallMs;
    result["measured_output_frames"] = qint64(measurementEnd.outputFrame - measurementStart.outputFrame);
    result["measured_output_seconds"] = double(measurementEnd.outputFrame - measurementStart.outputFrame) / 48000;
    result["frame_request_interval_ms"] = 16;
    result["cancelled"] = cancelled;
    try {
        require(controller.completedApplied() == controller.submittedCommands() && !controller.completedRejected() &&
                !controller.completedInvalidBuffers() && !controller.completedDeadlineMisses() &&
                !controller.completedReadFailures(), "Final audio/control counters failed");
        if (failure.isEmpty() && !cancelled) {
            const double renderedSeconds = double(measurementEnd.outputFrame - measurementStart.outputFrame) / 48000;
            const double wallSeconds = measuredWallMs / 1000.0;
            require(measuredWallMs >= qint64(seconds) * 1000 &&
                    std::abs(renderedSeconds - wallSeconds) <= std::max(.25, wallSeconds * .002),
                    "Wall time and rendered sample duration differ beyond the stated 0.2 percent/250ms tolerance");
        }
        for (const auto& summary : controller.completedTiming())
            require(summary.counters.graphPositionObserved && !summary.counters.graphDiscontinuities &&
                    !summary.counters.xrunRecoveryFlags && summary.counters.graphQuantumMin == 256 &&
                    summary.counters.graphQuantumMax == 256, "Observed driver-position/quantum counters failed");
        for (const auto& entry : intervals) {
            const auto value = entry.toObject();
            if (value["kind"].toString() != "steady_no_controls") continue;
            const auto frames = value["frames"].toInteger();
            require(value["starved_frames"].toInteger() == 0 &&
                    value["source_a_end"].toInteger() - value["source_a_begin"].toInteger() == frames &&
                    value["source_b_end"].toInteger() - value["source_b_begin"].toInteger() == frames,
                    "No-control steady interval lost source frames or starved");
        }
        if (QFileInfo::exists(recordingPath)) {
            const auto wav = inspectAudio(recordingPath, 48000ULL * (seconds + 60));
            result["recorded_audio"] = wav;
            require(recording["acceptedFrames"].toULongLong() == recording["writtenFrames"].toULongLong() &&
                    recording["writtenFrames"].toULongLong() == std::uint64_t(wav["frames"].toInteger()) &&
                    (wav["format"].toInt() & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV &&
                    (wav["format"].toInt() & SF_FORMAT_SUBMASK) == SF_FORMAT_FLOAT &&
                    !recording["overflowFrames"].toULongLong() && wav["nonzero_samples"].toInteger() > 0 &&
                    std::max(wav["peak_left"].toDouble(), wav["peak_right"].toDouble()) < maximumMasterPeak,
                    "Recorded WAV count, quiet peak, nonzero audio or overflow validation failed");
        } else require(cancelled, "Completed soak did not create a recording");
    } catch (const std::exception& error) {
        if (failure.isEmpty()) failure = QString::fromUtf8(error.what());
    }
    result["passed"] = failure.isEmpty() && !cancelled;
    result["error"] = failure;
    try { save(output + "/native-result.json", result); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << QJsonDocument(result).toJson().constData();
    return cancelled ? 2 : failure.isEmpty() ? 0 : 1;
}

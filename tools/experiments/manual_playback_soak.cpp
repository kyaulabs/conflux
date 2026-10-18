// SPDX-License-Identifier: AGPL-3.0-or-later
// Opt-in native laptop qualification. Only generated quiet fixtures are valid.
#include "ui/DeckController.hpp"
#include <sndfile.h>
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

namespace {
constexpr double maximumMasterPeak = 0.000025118864315095795; // -92 dBFS
using State = conflux::audio::LiveSession::State;
void require(bool value, const char* description)
{ if (!value) throw std::runtime_error(description); }
void save(const QString& path, const QJsonObject& value)
{
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot open soak report");
    const auto data = QJsonDocument(value).toJson();
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
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
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

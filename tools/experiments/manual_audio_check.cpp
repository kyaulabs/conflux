// SPDX-License-Identifier: AGPL-3.0-or-later
// Explicit host integration check; not a CI test because it needs PipeWire and
// a window system. Inputs must be generated quiet fixtures, never user music.
#include "ui/DeckController.hpp"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QTemporaryDir>
#include <QtEndian>
#include <QWheelEvent>
#include <functional>
#include <algorithm>
#include <cmath>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
QQuickItem* visualItem(QQuickItem* root, const QString& name)
{
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems())
        if (auto* found = visualItem(child, name)) return found;
    return nullptr;
}

// Independent RIFF parsing deliberately avoids the production decoder/writer.
QJsonObject readMutedRecording(const QString& path, bool requireNonempty)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024)
        throw std::runtime_error("Cannot read the bounded recording check WAV");
    const auto bytes = file.readAll();
    auto u16 = [&](qsizetype offset) { return qFromLittleEndian<quint16>(bytes.constData() + offset); };
    auto u32 = [&](qsizetype offset) { return qFromLittleEndian<quint32>(bytes.constData() + offset); };
    if (bytes.size() < 44 || bytes.first(4) != "RIFF" || bytes.mid(8, 4) != "WAVE" ||
        quint64(u32(4)) + 8 != quint64(bytes.size()))
        throw std::runtime_error("Recording has an invalid RIFF/WAVE header or final size");
    bool format = false, dataFound = false, allZero = true;
    quint64 frames = 0;
    for (qsizetype offset = 12; offset + 8 <= bytes.size();) {
        const auto length = u32(offset + 4);
        const auto begin = offset + 8;
        if (quint64(begin) + length > quint64(bytes.size()))
            throw std::runtime_error("Recording contains a truncated WAV chunk");
        const auto tag = bytes.mid(offset, 4);
        if (tag == "fmt ") {
            if (format || length < 16 || u16(begin) != 3 || u16(begin + 2) != 2 ||
                u32(begin + 4) != 48000 || u32(begin + 8) != 384000 ||
                u16(begin + 12) != 8 || u16(begin + 14) != 32)
                throw std::runtime_error("Recording is not stereo 48000 Hz float32 WAV");
            format = true;
        } else if (tag == "data") {
            if (dataFound || length % 8)
                throw std::runtime_error("Recording has duplicate or incomplete stereo data");
            dataFound = true;
            frames = length / 8;
            for (quint32 sample = 0; sample < length; sample += 4)
                allZero = allZero && std::bit_cast<float>(u32(begin + sample)) == 0.F;
        }
        offset = begin + length + (length & 1U);
    }
    if (!format || !dataFound || !allZero || (requireNonempty && !frames))
        throw std::runtime_error("Muted post-master recording is missing, empty or contains nonzero audio");
    return {{"frames", qint64(frames)}, {"channels", 2}, {"sample_rate", 48000},
            {"encoding", "IEEE float32"}, {"all_samples_zero", allZero},
            {"bytes", qint64(bytes.size())},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
}

QJsonArray timingResults(const conflux::DeckController& controller)
{
    QJsonArray result;
    for (const auto& summary : controller.completedTiming()) {
        const auto& counters = summary.counters;
        result.append(QJsonObject{
            {"session_index", int(result.size())}, {"callbacks", qint64(counters.callbacks)},
            {"maximum_execution_ns", qint64(counters.maxExecutionNs)},
            {"execution_deadline_misses", qint64(counters.deadlineMisses)},
            {"execution_p50_upper_us", summary.p50UpperUs}, {"execution_p95_upper_us", summary.p95UpperUs},
            {"execution_p99_upper_us", summary.p99UpperUs}, {"execution_p999_upper_us", summary.p999UpperUs},
            {"at_least_100ms", qint64(summary.atLeast100ms)},
            {"graph_position_observed", counters.graphPositionObserved},
            {"graph_quantum_min", qint64(counters.graphQuantumMin)},
            {"graph_quantum_max", qint64(counters.graphQuantumMax)},
            {"graph_discontinuities", qint64(counters.graphDiscontinuities)},
            {"xrun_recovery_flags", qint64(counters.xrunRecoveryFlags)},
            {"capabilities", QJsonObject{{"cycle", summary.capabilities.cycle},
                {"discontinuity_flag_mask", qint64(summary.capabilities.discontinuityFlag)},
                {"xrun_recovery_flag_mask", qint64(summary.capabilities.xrunRecoveryFlag)}}}});
    }
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 4) {
        std::cerr << "Usage: conflux-manual-audio-check QUIET_A.wav QUIET_B.wav SCREENSHOT_PREFIX\n";
        return 2;
    }
    QQuickStyle::setStyle("Basic");
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/Barlow-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/BarlowCondensed-SemiBold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/JetBrainsMono-Regular.ttf"));
    QTemporaryDir recordings;
    conflux::DeckController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("deckController", &controller);
    engine.rootContext()->setContextProperty("initialDark", true);
    engine.rootContext()->setContextProperty("qtVersion", QString::fromUtf8(qVersion()));
    engine.rootContext()->setContextProperty("platformName", QGuiApplication::platformName());
    QJsonArray checks;
    QJsonArray sceneErrors;
    QJsonObject recordingResult, steadyDelivery, routingResult, pendingOpenClose, settingsApply, initialWindow;
    QJsonArray sourceHandoffs, jogResults;
    QPointer<QQuickWindow> observedWindow;
    std::uint64_t swapped = 0;
    bool sceneError = false;
    QElapsedTimer elapsed;
    elapsed.start();
    auto require = [&](bool value, const char* description) {
        if (!value)
            throw std::runtime_error(description);
        checks.append(description);
    };
    auto wait = [&](const std::function<bool()>& predicate) {
        QElapsedTimer deadline;
        deadline.start();
        while (!predicate() && deadline.elapsed() < 8000)
            QTest::qWait(10);
        if (!predicate())
            throw std::runtime_error("Timed out waiting for decode/output/command acknowledgement");
    };
    auto windowState = [&]() -> QJsonObject {
        if (!observedWindow) return {{"available", false}};
        const auto visibility = observedWindow->visibility();
        const auto label = visibility == QWindow::Windowed ? "windowed" :
            visibility == QWindow::Minimized ? "minimized" : visibility == QWindow::Maximized ? "maximized" :
            visibility == QWindow::FullScreen ? "fullscreen" : visibility == QWindow::Hidden ? "hidden" : "automatic";
        return {{"available", true}, {"active", observedWindow->isActive()},
                {"exposed", observedWindow->isExposed()}, {"visible", observedWindow->isVisible()},
                {"visibility", int(visibility)}, {"visibility_name", label}};
    };
    try {
        engine.load(QUrl::fromLocalFile(QStringLiteral(CONFLUX_SOURCE_DIR "/qml/Main.qml")));
        require(!engine.rootObjects().isEmpty(), "Manual QML loaded");
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        require(window, "Manual window created");
        observedWindow = window;
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [&] { ++swapped; });
        QObject::connect(window, &QQuickWindow::sceneGraphError, &app,
            [&](QQuickWindow::SceneGraphError code, const QString& message) {
                sceneError = true;
                sceneErrors.append(QJsonObject{{"code", int(code)}, {"message", message}});
            });
        window->raise();
        window->requestActivate();
        wait([&] { return window->isExposed() && swapped > 0; });
        initialWindow = windowState();
        const auto a = QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1]));
        const auto b = QUrl::fromLocalFile(QString::fromLocal8Bit(argv[2]));
        controller.loadDeck(0, a);
        wait([&] { return !controller.busy() && controller.outputReady(); });
        require(controller.error().isEmpty() && !controller.audioState().playing[0] &&
                controller.audioState().sourceFrame[0] == 0, "Deck A loads paused at frame zero");
        const auto waveform = controller.decks()[0].toMap()["waveform"].toList();
        require(waveform.size() == 512 && std::all_of(waveform.begin(), waveform.end(), [](const QVariant& peak) {
                    return std::isfinite(peak.toDouble()) && peak.toDouble() >= 0 && peak.toDouble() <= 1;
                }), "Real decoded source publishes a bounded finite normalized overview");
        auto command = [&](const std::function<void()>& action) {
            const auto count = controller.audioState().applied + 1;
            action();
            wait([&] { return controller.audioState().applied >= count; });
            if (!controller.error().isEmpty())
                throw std::runtime_error(controller.error().toStdString());
        };
        auto handoff = [&](int target, const conflux::audio::LiveSession::State& before,
                           std::uint64_t sessions, const char* description) {
            const auto after = controller.audioState();
            const auto peer = std::size_t(1 - target);
            const auto outputAdvance = after.outputFrame - before.outputFrame;
            const auto peerAdvance = after.sourceFrame[peer] - before.sourceFrame[peer];
            require(controller.completedSessions() == sessions && after.outputFrame > before.outputFrame &&
                    after.sourceGeneration[target] != before.sourceGeneration[target] &&
                    after.sourceGeneration[peer] == before.sourceGeneration[peer] &&
                    !after.playing[target] && after.sourceFrame[target] == 0 && after.cueFrame[target] == 0 &&
                    !after.cueHeld[target] && !after.cuePreview[target] && !after.cueSelected[target] &&
                    after.deckGain[target] == 1 && !after.sourceReadError[target] &&
                    after.playing[peer] == before.playing[peer] && after.cueFrame[peer] == before.cueFrame[peer] &&
                    after.cueHeld[peer] == before.cueHeld[peer] && after.cuePreview[peer] == before.cuePreview[peer] &&
                    after.cueSelected[peer] == before.cueSelected[peer] && after.deckGain[peer] == before.deckGain[peer] &&
                    ((before.playing[peer] || before.cuePreview[peer]) ? peerAdvance == outputAdvance : peerAdvance == 0),
                    description);
            sourceHandoffs.append(QJsonObject{{"target_deck", target}, {"completed_sessions", qint64(sessions)},
                {"output_frame_before", qint64(before.outputFrame)}, {"output_frame_after", qint64(after.outputFrame)},
                {"target_generation_before", qint64(before.sourceGeneration[target])},
                {"target_generation_after", qint64(after.sourceGeneration[target])},
                {"peer_source_frame_before", qint64(before.sourceFrame[peer])},
                {"peer_source_frame_after", qint64(after.sourceFrame[peer])},
                {"peer_preview_preserved", after.cuePreview[peer] == before.cuePreview[peer]},
                {"scope", "Shared software session/output clock stays active; no physical continuity measurement."}});
            return after;
        };
        command([&] { controller.setPlaying(0, true); });
        wait([&] { return controller.audioState().sourceFrame[0] > 0; });
        const auto beforeLoad = controller.audioState();
        const auto loadSessions = controller.completedSessions();
        controller.loadDeck(1, b);
        wait([&] { return !controller.busy() && controller.outputReady(); });
        handoff(1, beforeLoad, loadSessions, "Loading B hands off into the existing output session and preserves advancing A");
        const auto beforeBatch = controller.audioState().applied;
        for (int index = 0; index < 80; ++index) {
            switch (index % 5) {
            case 0: command([&] { controller.setDeckGain(index % 2, 0.25 + (index % 3) * 0.25); }); break;
            case 1: command([&] { controller.setMasterGain(0.5); }); break;
            case 2: command([&] { controller.setPlaying(index % 2, false); }); break;
            case 3: command([&] { controller.seekSeconds(index % 2, 0.05); }); break;
            case 4: command([&] { controller.setPlaying(index % 2, true); }); break;
            }
        }
        require(controller.audioState().applied == beforeBatch + 80 && controller.audioState().rejected == 0,
                "80 actions acknowledged in one session without a 32-action lifetime limit");
        command([&] { controller.setPlaying(0, false); });
        command([&] { controller.setPlaying(1, false); });
        command([&] { controller.seekSeconds(0, 0.125); });
        command([&] { controller.seekSeconds(1, 0.25); });
        const auto held = controller.audioState();
        QTest::qWait(200);
        require(controller.audioState().sourceFrame == held.sourceFrame &&
                held.sourceFrame[0] == 6000 && held.sourceFrame[1] == 12000 &&
                controller.audioState().outputFrame > held.outputFrame,
                "Paused seeks hold exact source frames while output clock advances");
        for (int deck = 0; deck < 2; ++deck) {
            const auto beforeJog = controller.audioState();
            command([&] { require(controller.jogFrames(deck, 240), "Controller accepts a positive positional jog"); });
            require(controller.audioState().sourceFrame[deck] == held.sourceFrame[deck] + 240 &&
                    !controller.audioState().playing[deck] && controller.audioState().cueFrame[deck] == beforeJog.cueFrame[deck],
                    "Paused positive jog moves the exact cursor and preserves the stored cue");
            command([&] { require(controller.jogFrames(deck, -240), "Controller accepts a negative positional jog"); });
            require(controller.audioState().sourceFrame[deck] == held.sourceFrame[deck],
                    "Signed controller jog returns to the exact paused source frame");
            auto* jog = visualItem(window->contentItem(), QString("deck%1Jog").arg(deck));
            require(jog && jog->isEnabled(), "Actual deck jog control is enabled");
            command([&] {
                jog->forceActiveFocus();
                QTest::keyClick(window, Qt::Key_Right);
            });
            require(controller.audioState().sourceFrame[deck] == held.sourceFrame[deck] + 480,
                    "Actual jog Right key advances the paused cursor by 480 frames");
            command([&] {
                const auto local = jog->mapToScene(QPointF(jog->width() / 2, jog->height() / 2));
                QWheelEvent wheel(local, window->mapToGlobal(local.toPoint()), QPoint(), QPoint(0, -120),
                                  Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(window, &wheel);
            });
            const auto afterJog = controller.audioState();
            require(afterJog.sourceFrame[deck] == held.sourceFrame[deck] && !afterJog.playing[deck] &&
                    afterJog.cueFrame[deck] == beforeJog.cueFrame[deck] && afterJog.outputFrame > beforeJog.outputFrame,
                    "Actual negative jog wheel returns to the paused cursor without resetting the output clock");
            jogResults.append(QJsonObject{{"deck", deck}, {"source_frame_before", qint64(beforeJog.sourceFrame[deck])},
                {"source_frame_after", qint64(afterJog.sourceFrame[deck])}, {"controller_delta_frames", 240},
                {"keyboard_delta_frames", 480}, {"wheel_delta_frames", -480},
                {"cue_frame", qint64(afterJog.cueFrame[deck])},
                {"scope", "Positional search with paused source; no scratch, pitch bend or physical jog hardware qualification."}});
        }
        auto cuePoint = [&](int deck) {
            auto* item = window->findChild<QQuickItem*>(QString("deck%1Cue").arg(deck));
            require(item && item->isEnabled(), "Loaded deck exposes an enabled real CUE button");
            return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        };
        for (int deck = 0; deck < 2; ++deck) {
            const auto cue = held.sourceFrame[deck];
            command([&] { QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, cuePoint(deck)); });
            require(controller.audioState().cueFrame[deck] == cue && controller.audioState().cueHeld[deck] &&
                    !controller.audioState().cuePreview[deck], "Pointer CUE sets the exact paused render cursor");
            command([&] { QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, cuePoint(deck)); });
            command([&] { QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, cuePoint(deck)); });
            wait([&] { return controller.audioState().cuePreview[deck] && controller.audioState().sourceFrame[deck] > cue; });
            require(!controller.audioState().playing[deck], "Held pointer CUE auditions without changing requested play state");
            command([&] { QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, cuePoint(deck)); });
            require(controller.audioState().sourceFrame[deck] == cue && !controller.audioState().cueHeld[deck] &&
                    !controller.audioState().cuePreview[deck], "Pointer release returns preview to stored cue and pauses");
            command([&] { require(controller.cuePress(deck), "Controller accepts cue preview"); });
            command([&] { controller.setPlaying(deck, true); });
            command([&] { controller.cueRelease(deck); });
            require(controller.audioState().playing[deck] && !controller.audioState().cuePreview[deck] &&
                    controller.audioState().sourceFrame[deck] > cue, "PLAY latches preview; later CUE release keeps playing");
            command([&] { require(controller.cuePress(deck), "Controller accepts back-cue"); });
            require(!controller.audioState().playing[deck] && controller.audioState().sourceFrame[deck] == cue,
                    "CUE during playback returns to stored cue and pauses");
            command([&] { controller.cueRelease(deck); });
            command([&] { require(controller.cuePress(deck), "Controller accepts preview before seek"); });
            command([&] { controller.seekSeconds(deck, 0.5); });
            command([&] { controller.cueRelease(deck); });
            require(controller.audioState().cueFrame[deck] == cue && controller.audioState().sourceFrame[deck] == 24000 &&
                    !controller.audioState().cuePreview[deck], "Seek cancels audition and preserves stored cue");
            command([&] { controller.seekSeconds(deck, cue / 48000.0); });
        }
        // Saturate the real bounded queue while previewing. A release that
        // cannot enter immediately must survive and outrank later GUI controls.
        bool deferredRelease = false;
        for (int attempt = 0; attempt < 20 && !deferredRelease; ++attempt) {
            command([&] { require(controller.cuePress(0), "Controller accepts preview before queue pressure"); });
            for (int index = 0; index < 4096; ++index) {
                controller.setMasterGain(0.5);
                if (!controller.error().isEmpty())
                    break;
            }
            controller.cueRelease(0);
            deferredRelease = controller.error() == "Cue release is waiting for the audio control queue.";
            wait([&] { return !controller.audioState().cueHeld[0] && !controller.audioState().cuePreview[0]; });
        }
        require(deferredRelease, "Full transport queue retains CUE release until a later poll can submit it");
        command([&] { controller.setMasterGain(0.5); });
        require(controller.audioState().sourceFrame[0] == held.sourceFrame[0],
                "Queue-pressure recovery returns to the exact cue without a stuck audition");
        const auto names = controller.decks();
        controller.loadDeck(0, QUrl::fromLocalFile(QStringLiteral("/missing-conflux-manual-check.wav")));
        wait([&] { return !controller.busy(); });
        require(!controller.error().isEmpty() && controller.outputReady() && controller.decks() == names,
                "Decode failure preserves loaded decks and active output");
        command([&] { controller.setPlaying(1, true); });
        QTest::qWait(100);
        const auto beforeReplace = controller.audioState();
        const auto replaceSessions = controller.completedSessions();
        controller.loadDeck(0, a);
        wait([&] { return !controller.busy() && controller.outputReady(); });
        handoff(0, beforeReplace, replaceSessions, "Replacing A resets its source/cue while B and the shared output keep advancing");
        command([&] { require(controller.cuePress(1), "Controller accepts B back-cue before replacement check"); });
        command([&] { controller.cueRelease(1); });
        command([&] { QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, cuePoint(1)); });
        wait([&] { return controller.audioState().cuePreview[1]; });
        QTest::qWait(200);
        wait([&] { return controller.audioState().sourceFrame[1] > controller.audioState().cueFrame[1]; });
        const auto beforePreviewReplace = controller.audioState();
        const auto previewSessions = controller.completedSessions();
        controller.loadDeck(0, a);
        wait([&] { return !controller.busy() && controller.outputReady(); });
        handoff(0, beforePreviewReplace, previewSessions, "Replacing A preserves the peer's physically held audition and advancing cursor");
        command([&] { QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, cuePoint(1)); });
        require(!controller.audioState().cueHeld[1] && !controller.audioState().cuePreview[1] &&
                !controller.audioState().playing[1] && controller.audioState().sourceFrame[1] == held.sourceFrame[1] &&
                controller.audioState().cueFrame[1] == held.sourceFrame[1],
                "Physical peer CUE release after replacement returns to its preserved cue");
        command([&] { controller.setPlaying(1, true); });
        // Deliberately stall GUI snapshot consumption for longer than the
        // 32-slot queue covers. Audio must continue and later controls recover.
        QTest::qSleep(400);
        wait([&] { return controller.audioState().snapshotDrops > 0; });
        command([&] { controller.setDeckGain(1, 0.5); });
        require(controller.audioState().deckGain[1] == 0.5F,
                "Snapshot backpressure drops telemetry while later controls recover");
        require(recordings.isValid(), "Fresh temporary recording directory created");
        command([&] { controller.setMasterGain(0.0); });
        auto controlPoint = [&](const QString& name) {
            auto* item = visualItem(window->contentItem(), name);
            require(item && item->isEnabled(), "Actual UI control is enabled");
            return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        };
        for (int deck = 0; deck < 2; ++deck) {
            command([&] {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                    controlPoint(QString("deck%1CueSelect").arg(deck)));
            });
            require(controller.audioState().cueSelected[deck], "Mixer CUE selects the actual pre-fader headphone bus");
            if (deck == 0)
                command([&] { QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, controlPoint("deck0CueSelect")); });
        }
        auto* cueGain = visualItem(window->contentItem(), "cueGain");
        require(cueGain && cueGain->isEnabled(), "Actual headphone level control is enabled");
        const auto cueGainBefore = controller.cueGain();
        command([&] {
            cueGain->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Left);
        });
        require(controller.cueGain() < cueGainBefore && controller.cueGain() >= 0,
                "Real headphone level keyboard control decreases cue gain");
        wait([&] { return controller.audioState().meters.cue.peak[0] > 0 &&
                         controller.audioState().meters.master.peak[0] == 0 &&
                         controller.audioState().meters.master.peak[1] == 0; });
        const auto routing = controller.audioRouting();
        require(!controller.audioState().cueReady && !routing["cueReady"].toBool() &&
                routing["cueLeft"].toString().isEmpty() && routing["cueRight"].toString().isEmpty(),
                "Unrouted nonzero headphone cue remains disabled while master is muted");
        routingResult = QJsonObject::fromVariantMap(routing);
        recordingResult["cue_meter_nonzero_before_recording"] = true;
        recordingResult["master_meter_zero_before_recording"] = true;
        const auto recordingPath = recordings.filePath("muted-master.wav");
        controller.startRecording(QUrl::fromLocalFile(recordingPath));
        wait([&] { return controller.recording()["status"].toString() == "Recording"; });
        wait([&] { return controller.recording()["acceptedFrames"].toULongLong() >= 4800; });
        const auto beforeRecordingReplacement = controller.recording();
        const auto beforeRecordingHandoff = controller.audioState();
        const auto recordingSessions = controller.completedSessions();
        controller.loadDeck(0, a);
        wait([&] { return !controller.busy() && controller.outputReady() &&
                         controller.recording()["status"].toString() == "Recording" &&
                         controller.recording()["acceptedFrames"].toULongLong() >
                             beforeRecordingReplacement["acceptedFrames"].toULongLong(); });
        const auto afterRecordingHandoff = handoff(0, beforeRecordingHandoff, recordingSessions,
            "Recording source handoff keeps the same shared output and advancing peer clock");
        require(controller.recording()["path"].toString() == recordingPath &&
                controller.audioState().masterGain == 0 && controller.audioState().playing[1] &&
                controller.audioState().cueSelected[1],
                "Source replacement preserves the same recording path, writer prefix and peer transport");
        wait([&] { return controller.audioState().meters.cue.peak[0] > 0; });
        QTest::qWait(150);
        controller.stopRecording();
        wait([&] { return !controller.recording()["active"].toBool(); });
        const auto recorded = controller.recording();
        require(recorded["status"].toString() == "Complete" && recorded["error"].toString().isEmpty() &&
                recorded["overflowFrames"].toULongLong() == 0 &&
                recorded["acceptedFrames"].toULongLong() == recorded["writtenFrames"].toULongLong(),
                "Recording detaches, drains and finalizes without an overflow or disk error");
        const auto readback = readMutedRecording(recordingPath, true);
        require(readback["frames"].toInteger() == qint64(recorded["writtenFrames"].toULongLong()),
                "Independent nonempty stereo 48 kHz WAV readback is all zero despite nonzero cue");
        recordingResult["final_state"] = QJsonObject::fromVariantMap(recorded);
        recordingResult["readback"] = readback;
        recordingResult["replacement_before"] = QJsonObject::fromVariantMap(beforeRecordingReplacement);
        recordingResult["replacement_output_frame_before"] = qint64(beforeRecordingHandoff.outputFrame);
        recordingResult["replacement_output_frame_after"] = qint64(afterRecordingHandoff.outputFrame);
        recordingResult["replacement_policy"] = "One writer and shared output session remain active during callback-boundary target source handoff. The peer and output sample clocks continue; this is software evidence, not a physical continuity measurement.";

        // Separate steady delivery from seek/cache priming. Restore only the
        // existing quiet master level, then allow 200 ms after the last seek.
        command([&] { controller.setMasterGain(0.5); });
        command([&] { controller.seekSeconds(1, 0.25); });
        command([&] { controller.setPlaying(1, true); });
        QTest::qWait(200);
        const auto steadyBefore = controller.audioState();
        const auto steadySessions = controller.completedSessions();
        QElapsedTimer steadyElapsed;
        steadyElapsed.start();
        QTest::qWait(5000);
        const auto steadyAfter = controller.audioState();
        QJsonArray beforeStarved, afterStarved, starvationDelta, sourceBefore, sourceAfter;
        bool noNewShortage = true;
        for (std::size_t deck = 0; deck < 2; ++deck) {
            beforeStarved.append(qint64(steadyBefore.starvedFrames[deck]));
            afterStarved.append(qint64(steadyAfter.starvedFrames[deck]));
            starvationDelta.append(qint64(steadyAfter.starvedFrames[deck] - steadyBefore.starvedFrames[deck]));
            sourceBefore.append(qint64(steadyBefore.sourceFrame[deck]));
            sourceAfter.append(qint64(steadyAfter.sourceFrame[deck]));
            noNewShortage &= steadyAfter.starvedFrames[deck] == steadyBefore.starvedFrames[deck] &&
                            !steadyAfter.sourceReadError[deck];
        }
        steadyDelivery = {{"session_index", qint64(steadySessions)}, {"settling_ms", 200}, {"observation_ms", steadyElapsed.elapsed()},
            {"starved_frames_before", beforeStarved}, {"starved_frames_after", afterStarved},
            {"starved_frames_delta", starvationDelta}, {"source_frames_before", sourceBefore},
            {"source_frames_after", sourceAfter}, {"output_frame_before", qint64(steadyBefore.outputFrame)},
            {"output_frame_after", qint64(steadyAfter.outputFrame)},
            {"scope", "Five-second no-control quiet playback after 200 ms settling; separate from whole-run seek/priming counters."}};
        require(controller.completedSessions() == steadySessions && steadyBefore.playing[1] && steadyAfter.playing[1] &&
                steadyAfter.sourceFrame[1] > steadyBefore.sourceFrame[1] &&
                steadyAfter.sourceFrame[1] < steadyAfter.sourceFrames[1] && noNewShortage,
                "Five-second steady source interval advances playback without new unavailable frames or read errors");
        const auto prefix = QString::fromLocal8Bit(argv[3]);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, controlPoint("audioSettingsOpen"));
        auto* settings = window->findChild<QObject*>("audioSettingsDrawer");
        require(settings, "Actual audio settings drawer found");
        wait([&] { return settings->property("opened").toBool(); });
        const auto beforeApply = controller.audioState();
        const auto sessionsBeforeApply = controller.completedSessions();
        const auto routeBeforeApply = controller.audioRouting();
        require(routeBeforeApply["masterLeft"].toString().isEmpty() &&
                routeBeforeApply["masterRight"].toString().isEmpty() &&
                routeBeforeApply["cueLeft"].toString().isEmpty() &&
                routeBeforeApply["cueRight"].toString().isEmpty(),
                "Audio settings Apply uses only existing default master and disabled cue");
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, controlPoint("audioRoutingApply"));
        wait([&] { return controller.completedSessions() == sessionsBeforeApply + 1 &&
                         !controller.busy() && controller.outputReady(); });
        const auto afterApply = controller.audioState();
        require(controller.error().isEmpty() && afterApply.playing == beforeApply.playing &&
                afterApply.cueFrame == beforeApply.cueFrame && afterApply.cueSelected == beforeApply.cueSelected &&
                afterApply.deckGain == beforeApply.deckGain && afterApply.masterGain == beforeApply.masterGain &&
                afterApply.cueGain == beforeApply.cueGain && afterApply.sourceFrames == beforeApply.sourceFrames &&
                !afterApply.playing[0] && afterApply.sourceFrame[0] == beforeApply.sourceFrame[0] &&
                afterApply.playing[1] && afterApply.sourceFrame[1] >= beforeApply.sourceFrame[1] &&
                !afterApply.cueHeld[0] && !afterApply.cueHeld[1] && !afterApply.cueReady,
                "Real audio settings Apply reinstalls output and preserves exact cue, selection, gain and play requests");
        settingsApply = {{"completed_sessions_before", qint64(sessionsBeforeApply)},
            {"completed_sessions_after", qint64(controller.completedSessions())},
            {"paused_a_source_frame_before", qint64(beforeApply.sourceFrame[0])},
            {"paused_a_source_frame_after", qint64(afterApply.sourceFrame[0])},
            {"playing_b_source_frame_before", qint64(beforeApply.sourceFrame[1])},
            {"playing_b_source_frame_after", qint64(afterApply.sourceFrame[1])},
            {"routing_after", QJsonObject::fromVariantMap(controller.audioRouting())},
            {"preserved_transport_cue_selection_and_gains", true},
            {"scope", "Actual QML settings button applies existing default master with cue disabled; no explicit hardware or cue endpoint is selected."}};
        QTest::qWait(150);
        require(window->grabWindow().save(prefix + "-settings-default.png"), "Default-size audio settings screenshot saved");
        window->resize(1180, 760);
        QTest::qWait(200);
        require(window->grabWindow().save(prefix + "-settings-minimum.png"), "Minimum-size audio settings screenshot saved");
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, controlPoint("audioSettingsClose"));
        wait([&] { return !settings->property("visible").toBool(); });
        window->resize(1680, 1020);
        QTest::qWait(150);
        require(window->grabWindow().save(prefix + "-dark.png"), "Dark screenshot saved");
        auto* theme = window->findChild<QQuickItem*>("themeToggle");
        require(theme, "Theme control found");
        const auto point = theme->mapToScene(QPointF(theme->width() / 2, theme->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
        QTest::qWait(200);
        require(window->grabWindow().save(prefix + "-light.png"), "Light screenshot saved");
        window->resize(1180, 760);
        QTest::qWait(200);
        require(window->grabWindow().save(prefix + "-minimum.png"), "Minimum-size screenshot saved");
        require(!sceneError, "No scene-graph errors during live audio");
        require(swapped > 20, "GUI frames continued rendering alongside live audio");
        // Do not wait for a GUI acknowledgement: stop must preserve even the
        // accepted tail that has not yet reached a callback boundary.
        controller.setPlaying(1, false);
        controller.setMasterGain(0.25);
        require(controller.cuePress(0), "Tail preview press accepted before immediate shutdown");
        controller.shutdown();
        require(controller.completedApplied() == controller.submittedCommands() &&
                controller.completedRejected() == 0 && controller.completedInvalidBuffers() == 0 &&
                controller.completedDeadlineMisses() == 0 && controller.completedReadFailures() == 0,
                "All accepted controls applied; no rejections, invalid buffers or callback execution deadline misses across completed sessions");
        const auto final = controller.audioState();
        require(!final.playing[1] && final.masterGain == 0.25F && final.sourceFrame[0] == final.cueFrame[0] &&
                !final.cueHeld[0] && !final.cuePreview[0],
                "Immediate shutdown retains accepted tail controls and cancels held audition");
        // Isolated lifecycle scenario: its source stays paused, so this does
        // not add to speaker output. Close before the asynchronous opener's
        // completion can be delivered to the controller, then inspect its file.
        {
            conflux::DeckController pending;
            pending.loadDeck(0, a);
            wait([&] { return !pending.busy() && pending.outputReady(); });
            const auto pendingPath = recordings.filePath("pending-open-close.wav");
            pending.startRecording(QUrl::fromLocalFile(pendingPath));
            require(pending.recording()["status"].toString() == "Starting",
                    "Separate paused controller exposes pending recorder opener before immediate close");
            pending.shutdown();
            const auto state = pending.recording();
            const auto closedReadback = readMutedRecording(pendingPath, false);
            require(!state["active"].toBool() && state["status"].toString() == "Complete" &&
                    state["error"].toString().isEmpty() && state["acceptedFrames"].toULongLong() == 0 &&
                    state["writtenFrames"].toULongLong() == 0 && closedReadback["frames"].toInteger() == 0 &&
                    pending.completedInvalidBuffers() == 0 && pending.completedRejected() == 0,
                    "Immediate close joins pending recorder open and finalizes a valid empty WAV without attachment");
            pendingOpenClose = {{"final_state", QJsonObject::fromVariantMap(state)},
                {"readback", closedReadback}, {"session_timing", timingResults(pending)},
                {"scope", "Paused quiet source; opener completion not delivered before shutdown. Does not claim a timed disk-operation cancellation or pending-finalizer race."}};
        }
        const QJsonObject result{
            {"passed", true}, {"platform", QGuiApplication::platformName()}, {"qt", qVersion()},
            {"elapsed_ms", elapsed.elapsed()}, {"frames_swapped", qint64(swapped)},
            {"window_at_start", initialWindow}, {"window", windowState()},
            {"frame_observation_scope", "GUI-thread frameSwapped notifications; visibility state is diagnostic, not physical presentation latency."},
            {"sessions", qint64(controller.completedSessions())},
            {"submitted", qint64(controller.submittedCommands())},
            {"applied", qint64(controller.completedApplied())},
            {"rejected", qint64(controller.completedRejected())},
            {"invalid_buffers", qint64(controller.completedInvalidBuffers())},
            {"callbacks", qint64(controller.completedCallbacks())},
            {"callback_execution_deadline_misses", qint64(controller.completedDeadlineMisses())},
            {"maximum_callback_execution_ns", qint64(controller.maximumCallbackNs())},
            {"source_starved_frames_including_seek_priming", qint64(controller.completedStarvedFrames())},
            {"source_read_failures", qint64(controller.completedReadFailures())},
            {"last_session_callbacks", qint64(final.callbacks)},
            {"last_session_output_frames", qint64(final.outputFrame)},
            {"last_session_snapshot_drops", qint64(final.snapshotDrops)},
            {"last_session_frames_min", qint64(final.streamFramesMin)},
            {"last_session_frames_max", qint64(final.streamFramesMax)},
            {"session_timing", timingResults(controller)},
            {"timing_scope", "Per-session stopped execution histograms and graph-clock observations; percentiles are not combined across sessions. Unsupported flag masks mean unavailable. No scheduler-lateness, physical latency, separate-device drift or sustained qualification."},
            {"steady_source_delivery", steadyDelivery},
            {"routing", routingResult},
            {"recording", recordingResult},
            {"pending_recorder_open_close", pendingOpenClose},
            {"audio_settings_apply", settingsApply},
            {"source_handoffs", sourceHandoffs}, {"jog", jogResults},
            {"checks", checks},
            {"scope", "Short generated quiet-file UI/audio integration; no physical latency, acoustic or sustained qualification"}};
        std::cout << QJsonDocument(result).toJson().constData();
        return 0;
    } catch (const std::exception& error) {
        controller.shutdown();
        std::cerr << error.what() << '\n';
        const QJsonObject result{{"passed", false}, {"error", QString::fromUtf8(error.what())}, {"checks", checks},
            {"frames_swapped", qint64(swapped)}, {"scene_errors", sceneErrors}, {"elapsed_ms", elapsed.elapsed()},
            {"platform", QGuiApplication::platformName()}, {"qt", qVersion()},
            {"window_at_start", initialWindow}, {"window", windowState()},
            {"frame_observation_scope", "GUI-thread frameSwapped notifications; visibility state is diagnostic, not physical presentation latency."},
            {"submitted", qint64(controller.submittedCommands())}, {"applied", qint64(controller.completedApplied())},
            {"sessions", qint64(controller.completedSessions())}, {"callbacks", qint64(controller.completedCallbacks())},
            {"invalid_buffers", qint64(controller.completedInvalidBuffers())},
            {"rejected", qint64(controller.completedRejected())},
            {"source_starved_frames_including_seek_priming", qint64(controller.completedStarvedFrames())},
            {"source_read_failures", qint64(controller.completedReadFailures())},
            {"session_timing", timingResults(controller)}, {"steady_source_delivery", steadyDelivery},
            {"routing", routingResult}, {"recording", recordingResult}, {"pending_recorder_open_close", pendingOpenClose},
            {"audio_settings_apply", settingsApply}, {"source_handoffs", sourceHandoffs}, {"jog", jogResults}};
        std::cout << QJsonDocument(result).toJson().constData();
        return 1;
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// Explicit host integration check; not a CI test because it needs PipeWire and
// a window system. Inputs must be generated quiet fixtures, never user music.
#include "ui/DeckController.hpp"
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <functional>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

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
    conflux::DeckController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("deckController", &controller);
    engine.rootContext()->setContextProperty("initialDark", true);
    engine.rootContext()->setContextProperty("qtVersion", QString::fromUtf8(qVersion()));
    engine.rootContext()->setContextProperty("platformName", QGuiApplication::platformName());
    QJsonArray checks;
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
    try {
        engine.load(QUrl::fromLocalFile(QStringLiteral(CONFLUX_SOURCE_DIR "/qml/Main.qml")));
        require(!engine.rootObjects().isEmpty(), "Manual QML loaded");
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        require(window, "Manual window created");
        std::uint64_t swapped = 0;
        bool sceneError = false;
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [&] { ++swapped; });
        QObject::connect(window, &QQuickWindow::sceneGraphError, &app, [&] { sceneError = true; });
        wait([&] { return window->isExposed() && swapped > 0; });
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
        command([&] { controller.setPlaying(0, true); });
        wait([&] { return controller.audioState().sourceFrame[0] > 0; });
        const auto beforeLoad = controller.audioState().sourceFrame[0];
        controller.loadDeck(1, b);
        wait([&] { return !controller.busy() && controller.outputReady(); });
        require(controller.audioState().playing[0] && controller.audioState().sourceFrame[0] >= beforeLoad &&
                !controller.audioState().playing[1] && controller.audioState().sourceFrame[1] == 0,
                "Loading B preserves A transport and installs B paused");
        for (int index = 0; index < 80; ++index) {
            switch (index % 5) {
            case 0: command([&] { controller.setDeckGain(index % 2, 0.25 + (index % 3) * 0.25); }); break;
            case 1: command([&] { controller.setMasterGain(0.5); }); break;
            case 2: command([&] { controller.setPlaying(index % 2, false); }); break;
            case 3: command([&] { controller.seekSeconds(index % 2, 0.05); }); break;
            case 4: command([&] { controller.setPlaying(index % 2, true); }); break;
            }
        }
        require(controller.audioState().applied == 80 && controller.audioState().rejected == 0,
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
        const auto beforeReplace = controller.audioState().sourceFrame[1];
        controller.loadDeck(0, a);
        wait([&] { return !controller.busy() && controller.outputReady(); });
        require(!controller.audioState().playing[0] && controller.audioState().sourceFrame[0] == 0 &&
                controller.audioState().cueFrame[0] == 0 && controller.audioState().cueFrame[1] == held.sourceFrame[1] &&
                controller.audioState().playing[1] && controller.audioState().sourceFrame[1] >= beforeReplace,
                "Replacing A resets its cue and preserves B transport and cue after stream quiescence");
        command([&] { require(controller.cuePress(1), "Controller accepts B back-cue before replacement check"); });
        command([&] { controller.cueRelease(1); });
        command([&] { require(controller.cuePress(1), "Controller accepts B audition before replacing A"); });
        wait([&] { return controller.audioState().cuePreview[1]; });
        controller.loadDeck(0, a);
        wait([&] { return !controller.busy() && controller.outputReady(); });
        require(!controller.audioState().cueHeld[1] && !controller.audioState().cuePreview[1] &&
                !controller.audioState().playing[1] && controller.audioState().sourceFrame[1] == held.sourceFrame[1] &&
                controller.audioState().cueFrame[1] == held.sourceFrame[1],
                "Replacing a source cancels peer audition and preserves its stored cue");
        command([&] { controller.setPlaying(1, true); });
        // Deliberately stall GUI snapshot consumption for longer than the
        // 32-slot queue covers. Audio must continue and later controls recover.
        QTest::qSleep(400);
        wait([&] { return controller.audioState().snapshotDrops > 0; });
        command([&] { controller.setDeckGain(1, 0.5); });
        require(controller.audioState().deckGain[1] == 0.5F,
                "Snapshot backpressure drops telemetry while later controls recover");
        // Let normal GUI updates and audio processing overlap for five seconds.
        QTest::qWait(5000);
        const auto prefix = QString::fromLocal8Bit(argv[3]);
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
        require(!sceneError && swapped > 20, "GUI frames rendered alongside live audio without scene errors");
        // Do not wait for a GUI acknowledgement: stop must preserve even the
        // accepted tail that has not yet reached a callback boundary.
        controller.setPlaying(1, false);
        controller.setMasterGain(0.25);
        require(controller.cuePress(0), "Tail preview press accepted before immediate shutdown");
        controller.shutdown();
        require(controller.completedApplied() == controller.submittedCommands() &&
                controller.completedRejected() == 0 && controller.completedInvalidBuffers() == 0 &&
                controller.completedDeadlineMisses() == 0,
                "All accepted controls applied; no rejections, invalid buffers or callback execution deadline misses across four sessions");
        const auto final = controller.audioState();
        require(!final.playing[1] && final.masterGain == 0.25F && final.sourceFrame[0] == final.cueFrame[0] &&
                !final.cueHeld[0] && !final.cuePreview[0],
                "Immediate shutdown retains accepted tail controls and cancels held audition");
        const QJsonObject result{
            {"passed", true}, {"platform", QGuiApplication::platformName()}, {"qt", qVersion()},
            {"elapsed_ms", elapsed.elapsed()}, {"frames_swapped", qint64(swapped)},
            {"sessions", qint64(controller.completedSessions())},
            {"submitted", qint64(controller.submittedCommands())},
            {"applied", qint64(controller.completedApplied())},
            {"rejected", qint64(controller.completedRejected())},
            {"invalid_buffers", qint64(controller.completedInvalidBuffers())},
            {"callbacks", qint64(controller.completedCallbacks())},
            {"callback_execution_deadline_misses", qint64(controller.completedDeadlineMisses())},
            {"maximum_callback_execution_ns", qint64(controller.maximumCallbackNs())},
            {"last_session_callbacks", qint64(final.callbacks)},
            {"last_session_output_frames", qint64(final.outputFrame)},
            {"last_session_snapshot_drops", qint64(final.snapshotDrops)},
            {"last_session_frames_min", qint64(final.streamFramesMin)},
            {"last_session_frames_max", qint64(final.streamFramesMax)},
            {"checks", checks},
            {"scope", "Short generated quiet-file UI/audio integration; no physical latency, acoustic or sustained qualification"}};
        std::cout << QJsonDocument(result).toJson().constData();
        return 0;
    } catch (const std::exception& error) {
        controller.shutdown();
        std::cerr << error.what() << '\n';
        const QJsonObject result{{"passed", false}, {"error", QString::fromUtf8(error.what())}, {"checks", checks}};
        std::cout << QJsonDocument(result).toJson().constData();
        return 1;
    }
}

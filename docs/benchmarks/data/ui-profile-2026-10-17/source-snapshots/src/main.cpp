// SPDX-License-Identifier: AGPL-3.0-or-later
#include <QCommandLineParser>
#include <QFontDatabase>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QScreen>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <time.h>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("conflux");
    QCoreApplication::setApplicationVersion("0.1.0");
    QGuiApplication::setApplicationDisplayName("CONFLUX");
    QGuiApplication::setDesktopFileName("com.kyaulabs.conflux");
    QGuiApplication::setWindowIcon(QIcon(":/brand/assets/app-icons/conflux-app-dark.svg"));

    QCommandLineParser parser;
    parser.setApplicationDescription("CONFLUX M0 Wayland rendering harness");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"light", "Start with the light brand palette."});
    parser.addOption({"smoke-test", "Render for two seconds, report JSON and exit."});
    parser.addOption({"smoke-seconds", "Set smoke-test duration (1..7200 seconds).", "seconds", "2"});
    parser.addOption({"smoke-stop-file", "Finish the smoke test when this file appears.", "path"});
    parser.addOption({"smoke-progress-file", "Write atomic smoke-test frame progress once per second.", "path"});
    parser.addOption({"screenshot", "Save the smoke-test window image.", "path"});
    parser.process(app);
    if ((parser.isSet("screenshot") || parser.isSet("smoke-stop-file") ||
         parser.isSet("smoke-progress-file")) && !parser.isSet("smoke-test"))
        parser.showHelp(2);
    bool durationOk = false;
    const int seconds = parser.value("smoke-seconds").toInt(&durationOk);
    if (!durationOk || seconds < 1 || seconds > 7200 ||
        (parser.isSet("smoke-seconds") && !parser.isSet("smoke-test")))
        parser.showHelp(2);

    for (const auto* path : {":/brand/assets/fonts/Barlow-Regular.ttf",
                             ":/brand/assets/fonts/JetBrainsMono-Regular.ttf"}) {
        if (QFontDatabase::addApplicationFont(QString::fromUtf8(path)) < 0) {
            std::cerr << "Cannot load bundled font: " << path << '\n';
            return 1;
        }
    }

    QQuickStyle::setStyle("Basic");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("platformName", QGuiApplication::platformName());
    engine.rootContext()->setContextProperty("qtVersion", QString::fromUtf8(qVersion()));
    engine.rootContext()->setContextProperty("initialDark", !parser.isSet("light"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window)
        return 1;
    const auto nowNs = []() -> std::uint64_t {
        timespec value{};
        clock_gettime(CLOCK_MONOTONIC, &value);
        return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
    };
    const auto observedFrom = nowNs();
    std::uint64_t firstFrame = 0, lastFrame = 0, maximumFrameGap = 0;
    std::array<int, 121> frameBins{};
    int frames = 0;
    QObject::connect(window, &QQuickWindow::frameSwapped, &app, [&] {
        const auto now = nowNs();
        if (!firstFrame)
            firstFrame = now;
        if (lastFrame)
            maximumFrameGap = std::max(maximumFrameGap, now - lastFrame);
        lastFrame = now;
        ++frameBins[std::min<std::size_t>((now - observedFrom) / 60000000000ULL, frameBins.size() - 1)];
        ++frames;
    });
    QObject::connect(window, &QQuickWindow::sceneGraphError, &app,
                     [](QQuickWindow::SceneGraphError, const QString& error) {
                         std::cerr << error.toStdString() << '\n';
                         QCoreApplication::exit(1);
                     });
    if (parser.isSet("smoke-test")) {
        if (parser.isSet("smoke-progress-file")) {
            auto* progress = new QTimer(&app);
            QObject::connect(progress, &QTimer::timeout, &app, [&] {
                const QJsonObject snapshot{
                    {"pid", QCoreApplication::applicationPid()},
                    {"timestamp_monotonic_ns", static_cast<qint64>(nowNs())},
                    {"frames_swapped", frames},
                    {"last_frame_monotonic_ns", static_cast<qint64>(lastFrame)},
                    {"exposed", window->isExposed()},
                    {"scene_graph_initialized", window->isSceneGraphInitialized()}};
                const auto bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
                QSaveFile file(parser.value("smoke-progress-file"));
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
                    std::cerr << "Cannot write smoke progress: " << file.errorString().toStdString() << '\n';
                    app.exit(1);
                }
            });
            progress->start(1000);
        }
        const auto finish = [&, finished = std::make_shared<bool>(false)] {
            if (*finished)
                return;
            *finished = true;
            const bool rendered = window->isExposed() && window->isSceneGraphInitialized() && frames > 0;
            bool captured = true;
            if (parser.isSet("screenshot"))
                captured = window->grabWindow().save(parser.value("screenshot"));
            const auto observedUntil = nowNs();
            QJsonArray bins;
            const auto lastBin = std::min<std::size_t>((observedUntil - observedFrom) / 60000000000ULL,
                                                     frameBins.size() - 1);
            for (std::size_t index = 0; index <= lastBin; ++index)
                bins.append(frameBins[index]);
            const QJsonObject frameObservation{
                {"started_monotonic_ns", static_cast<qint64>(observedFrom)},
                {"stopped_monotonic_ns", static_cast<qint64>(observedUntil)},
                {"first_frame_monotonic_ns", static_cast<qint64>(firstFrame)},
                {"last_frame_monotonic_ns", static_cast<qint64>(lastFrame)},
                {"maximum_notification_gap_ms", maximumFrameGap / 1.0e6},
                {"frames_per_60_seconds", bins},
                {"scope", "GUI-thread frameSwapped notifications; not physical presentation latency"}};
            const QJsonObject result{
                {"platform", QGuiApplication::platformName()}, {"qt", qVersion()},
                {"desktop_id", QGuiApplication::desktopFileName()},
                {"graphics_api", static_cast<int>(window->rendererInterface()->graphicsApi())},
                {"frames_swapped", frames}, {"exposed", window->isExposed()},
                {"width", window->width()}, {"height", window->height()},
                {"device_pixel_ratio", window->devicePixelRatio()},
                {"rendered", rendered}, {"capture_ok", captured},
                {"frame_observation", frameObservation}};
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
            app.exit(rendered && captured ? 0 : 1);
        };
        QTimer::singleShot(seconds * 1000, &app, finish);
        if (parser.isSet("smoke-stop-file")) {
            auto* stopPoll = new QTimer(&app);
            QObject::connect(stopPoll, &QTimer::timeout, &app, [&, finish] {
                if (QFileInfo::exists(parser.value("smoke-stop-file")))
                    finish();
            });
            stopPoll->start(100);
        }
    }
    return app.exec();
}

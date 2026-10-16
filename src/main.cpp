// SPDX-License-Identifier: AGPL-3.0-or-later
#include <QCommandLineParser>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QScreen>
#include <QTimer>

#include <iostream>

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
    parser.addOption({"smoke-seconds", "Set smoke-test duration (1..3600 seconds).", "seconds", "2"});
    parser.addOption({"screenshot", "Save the smoke-test window image.", "path"});
    parser.process(app);
    if (parser.isSet("screenshot") && !parser.isSet("smoke-test"))
        parser.showHelp(2);
    bool durationOk = false;
    const int seconds = parser.value("smoke-seconds").toInt(&durationOk);
    if (!durationOk || seconds < 1 || seconds > 3600 ||
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
    int frames = 0;
    QObject::connect(window, &QQuickWindow::frameSwapped, &app, [&frames] { ++frames; });
    QObject::connect(window, &QQuickWindow::sceneGraphError, &app,
                     [](QQuickWindow::SceneGraphError, const QString& error) {
                         std::cerr << error.toStdString() << '\n';
                         QCoreApplication::exit(1);
                     });
    if (parser.isSet("smoke-test")) {
        QTimer::singleShot(seconds * 1000, &app, [&] {
            const bool rendered = window->isExposed() && window->isSceneGraphInitialized() && frames > 0;
            bool captured = true;
            if (parser.isSet("screenshot"))
                captured = window->grabWindow().save(parser.value("screenshot"));
            const QJsonObject result{
                {"platform", QGuiApplication::platformName()}, {"qt", qVersion()},
                {"desktop_id", QGuiApplication::desktopFileName()},
                {"graphics_api", static_cast<int>(window->rendererInterface()->graphicsApi())},
                {"frames_swapped", frames}, {"exposed", window->isExposed()},
                {"width", window->width()}, {"height", window->height()},
                {"device_pixel_ratio", window->devicePixelRatio()},
                {"rendered", rendered}, {"capture_ok", captured}};
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
            app.exit(rendered && captured ? 0 : 1);
        });
    }
    return app.exec();
}

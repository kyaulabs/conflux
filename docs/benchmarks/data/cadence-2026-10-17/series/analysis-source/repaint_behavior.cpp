// Temporary integration check: the application's real QML, real control input.
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QTimeLine>

#include <algorithm>
#include <cmath>
#include <iostream>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const int interval = argc > 2 ? QByteArray(argv[2]).toInt() : 33;
    if (argc < 2 || (interval != 16 && interval != 33))
        return 2;
    QQuickStyle::setStyle("Basic");
    QTimeLine timeline(3200);
    timeline.setEasingCurve(QEasingCurve::Linear);
    timeline.setLoopCount(0);
    timeline.setUpdateInterval(interval);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("platformName", QGuiApplication::platformName());
    engine.rootContext()->setContextProperty("qtVersion", QString::fromUtf8(qVersion()));
    engine.rootContext()->setContextProperty("initialDark", true);
    engine.rootContext()->setContextProperty("controlledRepaint", true);
    engine.rootContext()->setContextProperty("repaintTimeline", &timeline);
    engine.load(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    auto* window = engine.rootObjects().isEmpty() ? nullptr
        : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    QJsonObject checks{{"qml_loaded", window != nullptr}};
    QJsonObject details{{"interval_ms", interval}};
    auto finish = [&] {
        bool passed = true;
        for (auto it = checks.begin(); it != checks.end(); ++it)
            passed = passed && it.value().toBool();
        std::cout << QJsonDocument(QJsonObject{{"passed", passed}, {"checks", checks},
            {"details", details}}).toJson(QJsonDocument::Compact).constData() << '\n';
        return passed ? 0 : 1;
    };
    if (!window)
        return finish();
    QQuickItem* checkbox = nullptr;
    for (auto* object : window->findChildren<QObject*>()) {
        if (object->property("text").toString() == "Reduce motion" &&
            object->metaObject()->indexOfProperty("checked") >= 0) {
            checkbox = qobject_cast<QQuickItem*>(object);
            if (checkbox)
                break;
        }
    }
    checks["reduce_motion_control_found"] = checkbox != nullptr;
    if (!checkbox)
        return finish();
    timeline.start();
    timeline.setPaused(window->property("reducedMotion").toBool());
    auto position = [&] { return window->property("indicatorPosition").toDouble(); };
    bool bounded = true, outwardMidpoint = false, turnaround = false, returningMidpoint = false;
    double minimum = 300, maximum = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 3400) {
        QTest::qWait(40);
        const double x = position();
        const int phase = timeline.currentTime();
        bounded = bounded && std::isfinite(x) && x >= 0 && x <= 300;
        minimum = std::min(minimum, x);
        maximum = std::max(maximum, x);
        outwardMidpoint |= phase >= 700 && phase <= 900 && x >= 115 && x <= 185;
        turnaround |= phase >= 1500 && phase <= 1700 && x >= 290;
        returningMidpoint |= phase >= 2300 && phase <= 2500 && x >= 115 && x <= 185;
    }
    checks["motion_stays_inside_track"] = bounded;
    checks["full_out_and_back_motion"] = minimum < 10 && maximum > 290;
    checks["expected_cycle_checkpoints"] = outwardMidpoint && turnaround && returningMidpoint;
    while (timeline.currentTime() < 800 && elapsed.elapsed() < 4800)
        QTest::qWait(20);
    checks["control_visible_and_enabled"] = checkbox->isVisible() && checkbox->isEnabled();
    const QPoint click = checkbox->mapToScene(QPointF(checkbox->width() / 2,
                                                    checkbox->height() / 2)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, click);
    const double frozen = position();
    const int frozenPhase = timeline.currentTime();
    checks["toggle_pauses_timeline"] = checkbox->property("checked").toBool() &&
        window->property("reducedMotion").toBool() && timeline.state() == QTimeLine::Paused;
    QTest::qWait(400);
    checks["reduced_motion_freezes_position"] = position() == frozen &&
        timeline.currentTime() == frozenPhase;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, click);
    checks["toggle_resumes_without_reset_jump"] = !checkbox->property("checked").toBool() &&
        !window->property("reducedMotion").toBool() && timeline.state() == QTimeLine::Running &&
        position() == frozen && timeline.currentTime() == frozenPhase;
    QTest::qWait(250);
    checks["motion_advances_after_resume"] = std::abs(position() - frozen) > 1;
    details["minimum_x"] = minimum;
    details["maximum_x"] = maximum;
    details["paused_x"] = frozen;
    details["resumed_x"] = position();
    details["paused_phase_ms"] = frozenPhase;
    timeline.stop();
    return finish();
}

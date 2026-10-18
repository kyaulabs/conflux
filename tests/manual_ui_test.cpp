// SPDX-License-Identifier: AGPL-3.0-or-later
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>
#include <QVariantList>
#include <QtTest>

#include <memory>

namespace {

// This endpoint records calls from actual QML controls. It creates no audio
// session; decoding/backend behavior is covered by the controller/core tests.
class UiEndpoint final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList decks READ decks NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool outputReady READ outputReady NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(double masterGain READ masterGain NOTIFY stateChanged)
public:
    UiEndpoint()
    {
        for (int deck = 0; deck < 2; ++deck)
            decks_.append(QVariantMap{{"loaded", false}, {"name", ""}, {"playing", false},
                                      {"gain", .5}, {"positionSeconds", 0.0}, {"durationSeconds", 0.0}});
    }
    QVariantList decks() const { return decks_; }
    bool busy() const { return busy_; }
    bool outputReady() const { return ready_; }
    QString error() const { return error_; }
    double masterGain() const { return master_; }
    void setBusy(bool value) { busy_ = value; emit stateChanged(); }
    void setReady(bool value) { ready_ = value; emit stateChanged(); }
    void setError(const QString& value) { error_ = value; emit stateChanged(); }
    void loadState(int deck, double position = 2.0)
    {
        decks_[deck] = QVariantMap{{"loaded", true}, {"name", QString("Track %1").arg(deck)},
                                   {"playing", false}, {"gain", .5},
                                   {"positionSeconds", position}, {"durationSeconds", 10.0}};
        emit stateChanged();
    }
    void snapshotPosition(int deck, double position)
    {
        auto state = decks_[deck].toMap();
        state["positionSeconds"] = position;
        decks_[deck] = state;
        emit stateChanged();
    }
    Q_INVOKABLE void loadDeck(int deck, const QUrl& url)
    { calls.append(QVariantMap{{"action", "load"}, {"deck", deck}, {"url", url}}); }
    Q_INVOKABLE void setPlaying(int deck, bool playing)
    {
        calls.append(QVariantMap{{"action", "playing"}, {"deck", deck}, {"value", playing}});
        auto state = decks_[deck].toMap();
        state["playing"] = playing;
        decks_[deck] = state;
        emit stateChanged();
    }
    Q_INVOKABLE void seekSeconds(int deck, double seconds)
    {
        calls.append(QVariantMap{{"action", "seek"}, {"deck", deck}, {"value", seconds}});
        snapshotPosition(deck, seconds);
    }
    Q_INVOKABLE void setDeckGain(int deck, double gain)
    {
        calls.append(QVariantMap{{"action", "gain"}, {"deck", deck}, {"value", gain}});
        auto state = decks_[deck].toMap();
        state["gain"] = gain;
        decks_[deck] = state;
        emit stateChanged();
    }
    Q_INVOKABLE void setMasterGain(double gain)
    {
        calls.append(QVariantMap{{"action", "master"}, {"value", gain}});
        master_ = gain;
        emit stateChanged();
    }
    QVariantList calls;
signals:
    void stateChanged();
private:
    QVariantList decks_;
    bool busy_ = false, ready_ = false;
    QString error_;
    double master_ = .5;
};

// Repeater delegates live in the visual tree; their QObject ownership need
// not be rooted at the window. Search the actual scene used for interaction.
QQuickItem* visualItem(QQuickItem* root, const QString& name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems()) {
        if (auto* found = visualItem(child, name))
            return found;
    }
    return nullptr;
}

struct UiFixture {
    UiEndpoint endpoint;
    QQmlApplicationEngine engine;
    QQuickWindow* window = nullptr;
    QStringList warnings;

    void load()
    {
        engine.rootContext()->setContextProperty("deckController", &endpoint);
        engine.rootContext()->setContextProperty("initialDark", true);
        engine.rootContext()->setContextProperty("platformName", QGuiApplication::platformName());
        engine.rootContext()->setContextProperty("qtVersion", QString::fromUtf8(qVersion()));
        QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine,
                         [this](const QList<QQmlError>& errors) {
                             for (const auto& error : errors)
                                 warnings.append(error.toString());
                         });
        engine.load(QUrl::fromLocalFile(QStringLiteral(CONFLUX_SOURCE_DIR "/qml/Main.qml")));
        if (!engine.rootObjects().isEmpty()) {
            window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (window) {
                // Keep every tested control in the viewport. Small-window
                // scrolling is a separate visual/layout check.
                window->resize(1080, 1100);
                window->requestActivate();
            }
        }
    }
    QQuickItem* item(const char* name) const
    { return window ? visualItem(window->contentItem(), QString::fromUtf8(name)) : nullptr; }
    QPoint center(QQuickItem* item) const
    { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }
};

class ManualUiTest final : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        ui_ = std::make_unique<UiFixture>();
        ui_->load();
        QVERIFY2(ui_->window, qPrintable(ui_->warnings.join('\n')));
        QVERIFY(QTest::qWaitForWindowExposed(ui_->window));
        for (const auto* name : {"deck0Path", "deck0Load", "deck0Play", "deck0Seek", "deck0Gain",
                                 "deck1Path", "deck1Load", "deck1Play", "deck1Seek", "deck1Gain",
                                 "masterGain", "audioError"})
            QVERIFY2(ui_->item(name), name);
    }
    void cleanup()
    {
        ui_.reset();
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }

    void unloadedAndBusyControls()
    {
        for (const auto* name : {"deck0Play", "deck0Seek", "deck0Gain", "deck1Play", "deck1Seek",
                                 "deck1Gain", "masterGain"})
            QVERIFY2(!ui_->item(name)->isEnabled(), name);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck0Play")));
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QVERIFY(ui_->item("deck0Path")->setProperty("text", "/tmp/a.wav"));
        QTRY_VERIFY(ui_->item("deck0Load")->isEnabled());

        ui_->endpoint.loadState(0);
        ui_->endpoint.setReady(true);
        QTRY_VERIFY(ui_->item("deck0Play")->isEnabled());
        QVERIFY(!ui_->item("deck1Play")->isEnabled());
        ui_->endpoint.setBusy(true);
        QTRY_VERIFY(!ui_->item("deck0Load")->isEnabled());
        QVERIFY(ui_->item("deck0Play")->isEnabled());
        QVERIFY(ui_->item("deck0Seek")->isEnabled());
        QVERIFY(ui_->item("deck0Gain")->isEnabled());
        QVERIFY(ui_->item("masterGain")->isEnabled());
        ui_->endpoint.setReady(false);
        QTRY_VERIFY(!ui_->item("deck0Play")->isEnabled());
        QVERIFY(!ui_->item("masterGain")->isEnabled());
    }

    void keyboardPlayPauseRoutesBothDecks()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            const auto name = QString("deck%1Play").arg(deck).toUtf8();
            auto* button = ui_->item(name.constData());
            QTRY_VERIFY(button->isEnabled());
            button->forceActiveFocus();
            QTRY_VERIFY(button->hasActiveFocus());
            const auto before = ui_->endpoint.calls.size();
            QTest::keyClick(ui_->window, Qt::Key_Space);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            auto call = ui_->endpoint.calls.last().toMap();
            QCOMPARE(call.value("action").toString(), QString("playing"));
            QCOMPARE(call.value("deck").toInt(), deck);
            QVERIFY(call.value("value").toBool());
            QTRY_COMPARE(button->property("text").toString(), QString("Pause"));
            QTest::keyClick(ui_->window, Qt::Key_Space);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 2);
            call = ui_->endpoint.calls.last().toMap();
            QCOMPARE(call.value("deck").toInt(), deck);
            QVERIFY(!call.value("value").toBool());
            QTRY_COMPARE(button->property("text").toString(), QString("Play"));
        }
    }

    void keyboardSeekAndGainRoutes()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            for (const auto* suffix : {"Seek", "Gain"}) {
                const auto name = QString("deck%1%2").arg(deck).arg(suffix).toUtf8();
                auto* slider = ui_->item(name.constData());
                QTRY_VERIFY(slider->isEnabled());
                slider->forceActiveFocus();
                QTRY_VERIFY(slider->hasActiveFocus());
                const auto initial = slider->property("value").toDouble();
                const auto before = ui_->endpoint.calls.size();
                QTest::keyClick(ui_->window, Qt::Key_Right);
                QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
                const auto call = ui_->endpoint.calls.last().toMap();
                QCOMPARE(call.value("action").toString(), QString(suffix) == "Seek" ? QString("seek") : QString("gain"));
                QCOMPARE(call.value("deck").toInt(), deck);
                QVERIFY(call.value("value").toDouble() > initial);
                QCOMPARE(call.value("value").toDouble(), slider->property("value").toDouble());
            }
        }
        auto* master = ui_->item("masterGain");
        master->forceActiveFocus();
        QTRY_VERIFY(master->hasActiveFocus());
        const auto before = ui_->endpoint.calls.size();
        QTest::keyClick(ui_->window, Qt::Key_Right);
        QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
        const auto call = ui_->endpoint.calls.last().toMap();
        QCOMPARE(call.value("action").toString(), QString("master"));
        QCOMPARE(call.value("value").toDouble(), ui_->endpoint.masterGain());
        QVERIFY(ui_->endpoint.masterGain() > .5 && ui_->endpoint.masterGain() <= 1);
    }

    void dragSeekSubmitsOnceOnRelease()
    {
        ui_->endpoint.loadState(0, 1.0);
        ui_->endpoint.setReady(true);
        auto* seek = ui_->item("deck0Seek");
        QTRY_VERIFY(seek->isEnabled());
        const auto start = seek->mapToScene(QPointF(seek->width() * .25, seek->height() / 2)).toPoint();
        const auto end = seek->mapToScene(QPointF(seek->width() * .75, seek->height() / 2)).toPoint();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, start);
        QTRY_VERIFY(seek->property("pressed").toBool());
        QTest::mouseMove(ui_->window, end);
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, end);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.last().toMap();
        QCOMPARE(call.value("action").toString(), QString("seek"));
        QCOMPARE(call.value("deck").toInt(), 0);
        QVERIFY(call.value("value").toDouble() > 5.0);
        QCOMPARE(call.value("value").toDouble(), seek->property("value").toDouble());
    }

    void snapshotsAndErrorsDoNotSubmitCommands()
    {
        ui_->endpoint.loadState(0, 3.0);
        ui_->endpoint.setReady(true);
        auto* seek = ui_->item("deck0Seek");
        QTRY_COMPARE(seek->property("value").toDouble(), 3.0);
        ui_->endpoint.snapshotPosition(0, 7.0);
        QTRY_COMPARE(seek->property("value").toDouble(), 7.0);
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        ui_->endpoint.setError("Cannot decode this file");
        auto* error = ui_->item("audioError");
        QTRY_VERIFY(error->isVisible());
        QCOMPARE(error->property("text").toString(), QString("Cannot decode this file"));
    }

    void pathEnterRoutesLocalUrl()
    {
        auto* path = ui_->item("deck1Path");
        const QString local = "/tmp/Music Set/A #part%.wav";
        QVERIFY(path->setProperty("text", local));
        QTRY_VERIFY(ui_->item("deck1Load")->isEnabled());
        path->forceActiveFocus();
        QTRY_VERIFY(path->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Return);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.last().toMap();
        QCOMPARE(call.value("action").toString(), QString("load"));
        QCOMPARE(call.value("deck").toInt(), 1);
        const auto url = call.value("url").toUrl();
        QVERIFY(url.isLocalFile());
        QCOMPARE(url.toLocalFile(), local);
    }

private:
    std::unique_ptr<UiFixture> ui_;
};

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle("Basic");
    ManualUiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "manual_ui_test.moc"

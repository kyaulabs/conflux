// SPDX-License-Identifier: AGPL-3.0-or-later
#include <QGuiApplication>
#include <QFontDatabase>
#include <QKeyEvent>
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
                                      {"gain", .5}, {"positionSeconds", 0.0}, {"durationSeconds", 0.0},
                                      {"cueSeconds", 0.0}, {"cueHeld", false}, {"cuePreview", false},
                                      {"waveform", QVariantList{}}});
    }
    QVariantList decks() const { return decks_; }
    bool busy() const { return busy_; }
    bool outputReady() const { return ready_; }
    QString error() const { return error_; }
    double masterGain() const { return master_; }
    void setBusy(bool value) { busy_ = value; emit stateChanged(); }
    void setReady(bool value) { ready_ = value; emit stateChanged(); }
    void setError(const QString& value) { error_ = value; emit stateChanged(); }
    void setCueAccepted(bool value) { acceptCue_ = value; }
    void loadState(int deck, double position = 2.0)
    {
        decks_[deck] = QVariantMap{{"loaded", true}, {"name", QString("Track %1").arg(deck)},
                                   {"playing", false}, {"gain", .5},
                                   {"positionSeconds", position}, {"durationSeconds", 10.0},
                                   {"cueSeconds", 0.0}, {"cueHeld", false}, {"cuePreview", false},
                                   {"waveform", QVariantList{.15, .5, .9, .3, .1}}};
        emit stateChanged();
    }
    void unloadState(int deck)
    {
        auto state = decks_[deck].toMap();
        state["loaded"] = false;
        decks_[deck] = state;
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
        auto state = decks_[deck].toMap();
        calls.append(QVariantMap{{"action", "playing"}, {"deck", deck}, {"value", playing},
                                 {"previewBefore", state["cuePreview"]}});
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
    Q_INVOKABLE bool cuePress(int deck)
    {
        calls.append(QVariantMap{{"action", "cuePress"}, {"deck", deck}});
        if (!acceptCue_)
            return false;
        auto state = decks_[deck].toMap();
        state["cueHeld"] = true;
        state["cuePreview"] = true;
        decks_[deck] = state;
        emit stateChanged();
        return true;
    }
    Q_INVOKABLE void cueRelease(int deck)
    {
        calls.append(QVariantMap{{"action", "cueRelease"}, {"deck", deck}});
        auto state = decks_[deck].toMap();
        state["cueHeld"] = false;
        state["cuePreview"] = false;
        decks_[deck] = state;
        emit stateChanged();
    }
    QVariantList calls;
signals:
    void stateChanged();
private:
    QVariantList decks_;
    bool busy_ = false, ready_ = false;
    bool acceptCue_ = true;
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
                window->resize(1680, 1020);
                window->requestActivate();
            }
        }
    }
    QQuickItem* item(const char* name) const
    { return window ? visualItem(window->contentItem(), QString::fromUtf8(name)) : nullptr; }
    QObject* object(const char* name) const
    { return window ? window->findChild<QObject*>(QString::fromUtf8(name)) : nullptr; }
    QPoint center(QQuickItem* item) const
    { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

    bool openLibrary(int deck)
    {
        const auto name = QString("deck%1Browse").arg(deck).toUtf8();
        auto* browse = item(name.constData());
        auto* drawer = object("libraryDrawer");
        if (!browse || !drawer)
            return false;
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(browse));
        return QTest::qWaitFor([drawer] { return drawer->property("opened").toBool(); });
    }
    bool closeLibrary()
    {
        auto* close = item("libraryClose");
        auto* drawer = object("libraryDrawer");
        if (!close || !drawer)
            return false;
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(close));
        return QTest::qWaitFor([drawer] { return !drawer->property("visible").toBool(); });
    }
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
        for (const auto* name : {"deck0Browse", "deck0Cue", "deck0Play", "deck0Seek", "deck0Gain",
                                 "deck1Browse", "deck1Cue", "deck1Play", "deck1Seek", "deck1Gain",
                                 "masterGain", "audioError"})
            QVERIFY2(ui_->item(name), name);
        QVERIFY(ui_->object("libraryDrawer"));
    }
    void cleanup()
    {
        ui_.reset();
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }

    void unloadedAndBusyControls()
    {
        for (const auto* name : {"deck0Cue", "deck0Play", "deck0Seek", "deck0Gain", "deck1Cue", "deck1Play", "deck1Seek",
                                 "deck1Gain", "masterGain"})
            QVERIFY2(!ui_->item(name)->isEnabled(), name);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck0Play")));
        for (const auto* name : {"deck0Cue", "deck1Cue"})
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item(name)));
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QVERIFY(ui_->openLibrary(0));
        QVERIFY(ui_->item("deck0Path"));
        QVERIFY(ui_->item("deck0Load"));
        QVERIFY(ui_->item("deck0Path")->setProperty("text", "/tmp/a.wav"));
        QTRY_VERIFY(ui_->item("deck0Load")->isEnabled());

        ui_->endpoint.loadState(0);
        ui_->endpoint.setReady(true);
        QTRY_VERIFY(ui_->item("deck0Play")->isEnabled());
        QVERIFY(!ui_->item("deck1Play")->isEnabled());
        ui_->endpoint.setBusy(true);
        QTRY_VERIFY(!ui_->item("deck0Load")->isEnabled());
        QVERIFY(ui_->closeLibrary());
        QVERIFY(ui_->item("deck0Play")->isEnabled());
        QVERIFY(ui_->item("deck0Cue")->isEnabled());
        QVERIFY(ui_->item("deck0Seek")->isEnabled());
        QVERIFY(ui_->item("deck0Gain")->isEnabled());
        QVERIFY(ui_->item("masterGain")->isEnabled());
        ui_->endpoint.setReady(false);
        QTRY_VERIFY(!ui_->item("deck0Play")->isEnabled());
        QVERIFY(!ui_->item("deck0Cue")->isEnabled());
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

    void pointerCueHoldRoutesBothDecks()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            const auto name = QString("deck%1Cue").arg(deck).toUtf8();
            auto* cue = ui_->item(name.constData());
            QTRY_VERIFY(cue->isEnabled());
            const auto before = ui_->endpoint.calls.size();
            QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cue));
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            verifyCueCall(before, "cuePress", deck);
            QVERIFY(ui_->endpoint.decks()[deck].toMap()["cueHeld"].toBool());
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), before + 1);
            QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cue));
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 2);
            verifyCueCall(before + 1, "cueRelease", deck);
            QVERIFY(!ui_->endpoint.decks()[deck].toMap()["cueHeld"].toBool());
        }
    }

    void keyboardCueHold_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<int>("key");
        for (int deck = 0; deck < 2; ++deck)
            for (const auto key : {Qt::Key_Space, Qt::Key_Return, Qt::Key_Enter})
                QTest::newRow(qPrintable(QString("deck%1-key%2").arg(deck).arg(int(key)))) << deck << int(key);
    }

    void keyboardCueHold()
    {
        QFETCH(int, deck);
        QFETCH(int, key);
        ui_->endpoint.loadState(deck);
        ui_->endpoint.setReady(true);
        const auto name = QString("deck%1Cue").arg(deck).toUtf8();
        auto* cue = ui_->item(name.constData());
        QTRY_VERIFY(cue->isEnabled());
        cue->forceActiveFocus();
        QTRY_VERIFY(cue->hasActiveFocus());
        QTest::keyPress(ui_->window, Qt::Key(key));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        verifyCueCall(0, "cuePress", deck);
        // OS key repetition includes synthetic release/press pairs. Neither
        // edge may end the held preview or issue another accepted press.
        QKeyEvent repeatedRelease(QEvent::KeyRelease, key, Qt::NoModifier, QString(), true, 1);
        QKeyEvent repeatedPress(QEvent::KeyPress, key, Qt::NoModifier, QString(), true, 1);
        QCoreApplication::sendEvent(ui_->window, &repeatedRelease);
        QCoreApplication::sendEvent(ui_->window, &repeatedPress);
        QCOMPARE(ui_->endpoint.calls.size(), 1);
        QTest::keyRelease(ui_->window, Qt::Key(key));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        verifyCueCall(1, "cueRelease", deck);
        QVERIFY(!ui_->endpoint.decks()[deck].toMap()["cueHeld"].toBool());
    }

    void cueCancellation_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<QString>("reason");
        for (int deck = 0; deck < 2; ++deck)
            for (const auto* reason : {"pointer-ungrab", "keyboard-focus", "output-loss", "unload", "hidden"})
                QTest::newRow(qPrintable(QString("deck%1-%2").arg(deck).arg(reason))) << deck << QString(reason);
    }

    void cueCancellation()
    {
        QFETCH(int, deck);
        QFETCH(QString, reason);
        ui_->endpoint.loadState(deck);
        ui_->endpoint.setReady(true);
        const auto name = QString("deck%1Cue").arg(deck).toUtf8();
        auto* cue = ui_->item(name.constData());
        QTRY_VERIFY(cue->isEnabled());
        const auto point = ui_->center(cue);
        const bool keyboard = reason == "keyboard-focus";
        if (keyboard) {
            cue->forceActiveFocus();
            QTRY_VERIFY(cue->hasActiveFocus());
            QTest::keyPress(ui_->window, Qt::Key_Space);
        } else {
            QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        }
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        verifyCueCall(0, "cuePress", deck);
        if (reason == "pointer-ungrab")
            cue->ungrabMouse();
        else if (reason == "keyboard-focus")
            ui_->item("masterGain")->forceActiveFocus();
        else if (reason == "output-loss")
            ui_->endpoint.setReady(false);
        else if (reason == "unload")
            ui_->endpoint.unloadState(deck);
        else
            cue->parentItem()->setVisible(false);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        verifyCueCall(1, "cueRelease", deck);
        QVERIFY(!ui_->endpoint.decks()[deck].toMap()["cueHeld"].toBool());
        if (keyboard)
            QTest::keyRelease(ui_->window, Qt::Key_Space);
        else
            QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QCoreApplication::processEvents();
        QCOMPARE(ui_->endpoint.calls.size(), 2);
    }

    void rejectedCuePressHasNoRelease()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        ui_->endpoint.setCueAccepted(false);
        for (int deck = 0; deck < 2; ++deck) {
            const auto name = QString("deck%1Cue").arg(deck).toUtf8();
            auto* cue = ui_->item(name.constData());
            QTRY_VERIFY(cue->isEnabled());
            const auto before = ui_->endpoint.calls.size();
            QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cue));
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            verifyCueCall(before, "cuePress", deck);
            QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cue));
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), before + 1);
            QVERIFY(!ui_->endpoint.decks()[deck].toMap()["cueHeld"].toBool());
        }
    }

    void playPressPrecedesHeldCueFocusCleanup()
    {
        ui_->endpoint.loadState(0, 0);
        ui_->endpoint.loadState(1, 0);
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            const auto cueName = QString("deck%1Cue").arg(deck).toUtf8();
            const auto playName = QString("deck%1Play").arg(deck).toUtf8();
            auto* cue = ui_->item(cueName.constData());
            auto* play = ui_->item(playName.constData());
            QTRY_VERIFY(cue->isEnabled() && play->isEnabled());
            cue->forceActiveFocus();
            QTRY_VERIFY(cue->hasActiveFocus());
            const auto before = ui_->endpoint.calls.size();
            QTest::keyPress(ui_->window, Qt::Key_Space);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            verifyCueCall(before, "cuePress", deck);
            const auto playPoint = ui_->center(play);
            QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, playPoint);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 3);
            const auto playing = ui_->endpoint.calls[before + 1].toMap();
            QCOMPARE(playing["action"].toString(), QString("playing"));
            QCOMPARE(playing["deck"].toInt(), deck);
            QVERIFY(playing["value"].toBool());
            QVERIFY(playing["previewBefore"].toBool());
            verifyCueCall(before + 2, "cueRelease", deck);
            QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, playPoint);
            QTest::keyRelease(ui_->window, Qt::Key_Space);
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), before + 3);
            QVERIFY(ui_->endpoint.decks()[deck].toMap()["playing"].toBool());
            QVERIFY(!ui_->endpoint.decks()[deck].toMap()["cueHeld"].toBool());
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
                QTest::keyClick(ui_->window, QString(suffix) == "Gain" ? Qt::Key_Up : Qt::Key_Right);
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
        QVERIFY(ui_->openLibrary(1));
        auto* path = ui_->item("deck1Path");
        QVERIFY(path);
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
        QVERIFY(ui_->object("libraryDrawer")->property("opened").toBool());
        QTest::keyClick(ui_->window, Qt::Key_Escape);
        QTRY_VERIFY(!ui_->object("libraryDrawer")->property("visible").toBool());
    }

    void loadButtonRoutesLocalUrl()
    {
        QVERIFY(ui_->openLibrary(0));
        auto* path = ui_->item("deck0Path");
        auto* load = ui_->item("deck0Load");
        QVERIFY(path);
        QVERIFY(load);
        const QString local = "/tmp/Music Set/Deck A.wav";
        QVERIFY(path->setProperty("text", local));
        QTRY_VERIFY(load->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(load));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.last().toMap();
        QCOMPARE(call.value("action").toString(), QString("load"));
        QCOMPARE(call.value("deck").toInt(), 0);
        QCOMPARE(call.value("url").toUrl().toLocalFile(), local);
        QVERIFY(ui_->object("libraryDrawer")->property("opened").toBool());
        QVERIFY(ui_->closeLibrary());
    }

    void viewportControlsFit_data()
    {
        QTest::addColumn<QSize>("viewport");
        QTest::newRow("minimum") << QSize(1180, 760);
        QTest::newRow("default") << QSize(1680, 1020);
    }

    void viewportControlsFit()
    {
        QFETCH(QSize, viewport);
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        // Check usable control geometry, without constraining colors, artwork,
        // control dimensions, or decorative elements inside the deck surfaces.
        verifyControlsFit({"deck0Browse", "deck0Cue", "deck0Play", "deck0Seek", "deck0Gain",
                           "deck1Browse", "deck1Cue", "deck1Play", "deck1Seek", "deck1Gain",
                           "masterGain", "themeToggle"});
        QVERIFY(ui_->openLibrary(0));
        verifyControlsFit({"deck0Path", "deck0Load", "deck1Path", "deck1Load", "libraryClose"});
        QVERIFY(ui_->closeLibrary());
    }

private:
    void verifyCueCall(qsizetype index, const char* action, int deck)
    {
        QVERIFY(index >= 0 && index < ui_->endpoint.calls.size());
        const auto call = ui_->endpoint.calls[index].toMap();
        QCOMPARE(call.value("action").toString(), QString::fromUtf8(action));
        QCOMPARE(call.value("deck").toInt(), deck);
    }

    void verifyControlsFit(const QStringList& names)
    {
        const QRectF viewport(QPointF(0, 0), ui_->window->size());
        QList<QRectF> bounds;
        for (const auto& name : names) {
            auto* control = ui_->item(name.toUtf8().constData());
            QVERIFY2(control, qPrintable(name));
            QTRY_VERIFY2(control->isVisible(), qPrintable(name));
            QTRY_VERIFY2(control->width() > 0 && control->height() > 0, qPrintable(name));
            QTRY_VERIFY2(viewport.adjusted(-.5, -.5, .5, .5).contains(
                             control->mapRectToScene(QRectF(0, 0, control->width(), control->height()))),
                         qPrintable(name + " falls outside the viewport"));
            const auto rect = control->mapRectToScene(QRectF(0, 0, control->width(), control->height()));
            for (int previous = 0; previous < bounds.size(); ++previous)
                QVERIFY2(!rect.intersects(bounds[previous]),
                         qPrintable(name + " overlaps " + names[previous]));
            bounds.append(rect);
        }
    }

    std::unique_ptr<UiFixture> ui_;
};

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle("Basic");
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/Barlow-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/BarlowCondensed-SemiBold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/JetBrainsMono-Regular.ttf"));
    ManualUiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "manual_ui_test.moc"

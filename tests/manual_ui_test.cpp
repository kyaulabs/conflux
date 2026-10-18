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
    Q_PROPERTY(double cueGain READ cueGain NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap meters READ meters NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap audioRouting READ audioRouting NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap recording READ recording NOTIFY stateChanged)
public:
    static QVariantMap silentMeter()
    {
        return {{"peakLeft", 0.0}, {"peakRight", 0.0}, {"rmsLeft", 0.0}, {"rmsRight", 0.0},
                {"overrangeLeft", 0}, {"overrangeRight", 0}};
    }
    UiEndpoint()
    {
        for (int deck = 0; deck < 2; ++deck)
            decks_.append(QVariantMap{{"loaded", false}, {"name", ""}, {"playing", false},
                                      {"gain", .5}, {"positionSeconds", 0.0}, {"durationSeconds", 0.0},
                                      {"cueSeconds", 0.0}, {"cueHeld", false}, {"cuePreview", false},
                                      {"cueSelected", false}, {"meter", silentMeter()},
                                      {"waveform", QVariantList{}}});
        meters_ = {{"master", silentMeter()}, {"cue", silentMeter()}, {"blockFrames", 256}};
        QVariantList ports;
        for (int port = 1; port <= 4; ++port)
            ports.append(QVariantMap{{"key", QString("port%1").arg(port)},
                                      {"label", QString("Interface output %1").arg(port)}});
        routing_ = {{"ports", ports}, {"masterReady", false}, {"cueReady", false},
                    {"masterLeft", ""}, {"masterRight", ""}, {"cueLeft", ""}, {"cueRight", ""},
                    {"defaultSink", "Laptop Speakers"}, {"masterError", ""}, {"cueError", ""}};
        recording_ = {{"status", "Idle"}, {"path", ""}, {"error", ""}, {"active", false},
                      {"acceptedFrames", 0}, {"writtenFrames", 0}, {"overflowFrames", 0}};
    }
    QVariantList decks() const { return decks_; }
    bool busy() const { return busy_; }
    bool outputReady() const { return ready_; }
    QString error() const { return error_; }
    double masterGain() const { return master_; }
    double cueGain() const { return cue_; }
    QVariantMap meters() const { return meters_; }
    QVariantMap audioRouting() const { return routing_; }
    QVariantMap recording() const { return recording_; }
    void setBusy(bool value) { busy_ = value; emit stateChanged(); }
    void setReady(bool value) { ready_ = value; emit stateChanged(); }
    void setError(const QString& value) { error_ = value; emit stateChanged(); }
    void setCueAccepted(bool value) { acceptCue_ = value; }
    void setMeters(const QVariantMap& master, const QVariantMap& cue, const QVariantMap& deck)
    {
        meters_["master"] = master;
        meters_["cue"] = cue;
        auto state = decks_[0].toMap();
        state["meter"] = deck;
        decks_[0] = state;
        emit stateChanged();
    }
    void setRoutingError(const QString& error)
    { routing_["cueError"] = error; emit stateChanged(); }
    void setRecording(const QString& status, bool active, const QString& error = {})
    {
        recording_["status"] = status;
        recording_["active"] = active;
        recording_["error"] = error;
        emit stateChanged();
    }
    void loadState(int deck, double position = 2.0)
    {
        decks_[deck] = QVariantMap{{"loaded", true}, {"name", QString("Track %1").arg(deck)},
                                   {"playing", false}, {"gain", .5},
                                   {"positionSeconds", position}, {"durationSeconds", 10.0},
                                   {"cueSeconds", 0.0}, {"cueHeld", false}, {"cuePreview", false},
                                   {"cueSelected", false}, {"meter", silentMeter()},
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
    Q_INVOKABLE void setCueGain(double gain)
    {
        calls.append(QVariantMap{{"action", "cueGain"}, {"value", gain}});
        cue_ = gain;
        emit stateChanged();
    }
    Q_INVOKABLE void setCueSelected(int deck, bool selected)
    {
        calls.append(QVariantMap{{"action", "cueSelected"}, {"deck", deck}, {"value", selected}});
        auto state = decks_[deck].toMap();
        state["cueSelected"] = selected;
        decks_[deck] = state;
        emit stateChanged();
    }
    Q_INVOKABLE void refreshAudioRouting()
    { calls.append(QVariantMap{{"action", "refreshRouting"}}); }
    Q_INVOKABLE void configureAudioRouting(const QString& masterLeft, const QString& masterRight,
                                          const QString& cueLeft, const QString& cueRight)
    {
        calls.append(QVariantMap{{"action", "routing"}, {"masterLeft", masterLeft}, {"masterRight", masterRight},
                                 {"cueLeft", cueLeft}, {"cueRight", cueRight}});
        routing_["masterLeft"] = masterLeft;
        routing_["masterRight"] = masterRight;
        routing_["cueLeft"] = cueLeft;
        routing_["cueRight"] = cueRight;
        emit stateChanged();
    }
    Q_INVOKABLE void startRecording(const QUrl& url)
    {
        calls.append(QVariantMap{{"action", "recordStart"}, {"url", url}});
        recording_["path"] = url.toLocalFile();
        setRecording("Recording", true);
    }
    Q_INVOKABLE void stopRecording()
    {
        calls.append(QVariantMap{{"action", "recordStop"}});
        setRecording("Finalizing", true);
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
    QVariantMap meters_, routing_, recording_;
    bool busy_ = false, ready_ = false;
    bool acceptCue_ = true;
    QString error_;
    double master_ = .5;
    double cue_ = .5;
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
    bool openAudioSettings()
    {
        auto* open = item("audioSettingsOpen");
        auto* drawer = object("audioSettingsDrawer");
        if (!open || !drawer)
            return false;
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(open));
        return QTest::qWaitFor([drawer] { return drawer->property("opened").toBool(); });
    }
    bool closeAudioSettings()
    {
        auto* close = item("audioSettingsClose");
        auto* drawer = object("audioSettingsDrawer");
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
                                 "masterGain", "cueGain", "deck0CueSelect", "deck1CueSelect",
                                 "deck0Meter", "deck1Meter", "masterMeter", "cueMeter", "audioSettingsOpen", "audioError"})
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
                                 "deck1Gain", "masterGain", "cueGain", "deck0CueSelect", "deck1CueSelect"})
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

    void headphoneCueAndLevelRouteActualControls()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            const auto name = QString("deck%1CueSelect").arg(deck).toUtf8();
            auto* cue = ui_->item(name.constData());
            QTRY_VERIFY(cue->isEnabled());
            cue->forceActiveFocus();
            QTRY_VERIFY(cue->hasActiveFocus());
            const auto before = ui_->endpoint.calls.size();
            QTest::keyClick(ui_->window, Qt::Key_Space);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            auto call = ui_->endpoint.calls.last().toMap();
            QCOMPARE(call["action"].toString(), QString("cueSelected"));
            QCOMPARE(call["deck"].toInt(), deck);
            QVERIFY(call["value"].toBool());
            QTRY_VERIFY(cue->property("checked").toBool());
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cue));
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 2);
            QVERIFY(!ui_->endpoint.calls.last().toMap()["value"].toBool());
            QTRY_VERIFY(!cue->property("checked").toBool());
        }
        auto* gain = ui_->item("cueGain");
        gain->forceActiveFocus();
        QTRY_VERIFY(gain->hasActiveFocus());
        const auto before = ui_->endpoint.calls.size();
        QTest::keyClick(ui_->window, Qt::Key_Right);
        QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("cueGain"));
        QVERIFY(ui_->endpoint.cueGain() > .5);
        QCOMPARE(gain->property("value").toDouble(), ui_->endpoint.cueGain());
    }

    void actualMetersFollowSnapshotsAndShowOverrange()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.setReady(true);
        QVariantMap master{{"peakLeft", 1.25}, {"peakRight", .75}, {"rmsLeft", .4}, {"rmsRight", .2},
                           {"overrangeLeft", 3}, {"overrangeRight", 0}};
        QVariantMap cue{{"peakLeft", .125}, {"peakRight", .25}, {"rmsLeft", .04}, {"rmsRight", .08},
                        {"overrangeLeft", 0}, {"overrangeRight", 0}};
        QVariantMap deck{{"peakLeft", .5}, {"peakRight", .8}, {"rmsLeft", .1}, {"rmsRight", .3},
                         {"overrangeLeft", 0}, {"overrangeRight", 1}};
        ui_->endpoint.setMeters(master, cue, deck);
        auto* masterMeter = ui_->item("masterMeter");
        auto* cueMeter = ui_->item("cueMeter");
        auto* deckMeter = ui_->item("deck0Meter");
        QTRY_COMPARE(masterMeter->property("peakLeft").toDouble(), 1.25);
        QCOMPARE(masterMeter->property("rmsRight").toDouble(), .2);
        QTRY_VERIFY(masterMeter->property("overrange").toBool());
        QCOMPARE(ui_->item("masterMeterOverrange")->property("text").toString(), QString("OVR"));
        QTRY_COMPARE(cueMeter->property("peakRight").toDouble(), .25);
        QCOMPARE(cueMeter->property("rmsLeft").toDouble(), .04);
        QVERIFY(!cueMeter->property("overrange").toBool());
        QTRY_COMPARE(deckMeter->property("peakLeft").toDouble(), .5);
        QVERIFY(deckMeter->property("overrange").toBool());
        ui_->endpoint.setMeters(UiEndpoint::silentMeter(), UiEndpoint::silentMeter(), UiEndpoint::silentMeter());
        QTRY_COMPARE(masterMeter->property("peakLeft").toDouble(), 0.0);
        QVERIFY(!masterMeter->property("overrange").toBool());
        QCOMPARE(ui_->endpoint.calls.size(), 0);
    }

    void audioSettingsRejectIncompleteOrDuplicatePairsAndRouteKeys()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.setReady(true);
        QVERIFY(ui_->openAudioSettings());
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("refreshRouting"));
        ui_->endpoint.calls.clear();
        auto* apply = ui_->item("audioRoutingApply");
        QVERIFY(apply);
        QTRY_VERIFY(apply->isEnabled());
        choosePort("masterLeftPort", 1);
        QTRY_VERIFY(!apply->isEnabled());
        QVERIFY(ui_->item("routingValidation")->isVisible());
        choosePort("masterRightPort", 2);
        QTRY_VERIFY(apply->isEnabled());
        choosePort("cueLeftPort", 3);
        QTRY_VERIFY(!apply->isEnabled());
        choosePort("cueRightPort", 2);
        QVERIFY(!apply->isEnabled());
        choosePort("cueRightPort", 4);
        QTRY_VERIFY(apply->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(apply));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.last().toMap();
        QCOMPARE(call["action"].toString(), QString("routing"));
        QCOMPARE(call["masterLeft"].toString(), QString("port1"));
        QCOMPARE(call["masterRight"].toString(), QString("port2"));
        QCOMPARE(call["cueLeft"].toString(), QString("port3"));
        QCOMPARE(call["cueRight"].toString(), QString("port4"));
        ui_->endpoint.setRoutingError("Cue device disconnected");
        QTRY_COMPARE(ui_->item("routingError")->property("text").toString(), QString("Cue device disconnected"));
        QVERIFY(ui_->closeAudioSettings());
    }

    void recordingControlsUseLocalPathAndWaitForFinalization()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.setReady(true);
        QVERIFY(ui_->openAudioSettings());
        ui_->endpoint.calls.clear();
        auto* path = ui_->item("recordingPath");
        auto* start = ui_->item("recordingStart");
        auto* stop = ui_->item("recordingStop");
        QVERIFY(path && start && stop);
        QVERIFY(!start->isEnabled());
        QVERIFY(!stop->isEnabled());
        path->setProperty("text", "relative.wav");
        QVERIFY(!start->isEnabled());
        const QString file = "/tmp/Music Set/New #mix%.wav";
        path->setProperty("text", file);
        QTRY_VERIFY(start->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(start));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.last().toMap();
        QCOMPARE(call["action"].toString(), QString("recordStart"));
        QCOMPARE(call["url"].toUrl().toLocalFile(), file);
        QTRY_VERIFY(!start->isEnabled() && !path->isEnabled() && stop->isEnabled());
        QVERIFY(!ui_->item("audioRoutingApply")->isEnabled());
        QCOMPARE(ui_->item("recordingStatus")->property("text").toString(), QString("Recording: Recording"));
        QCOMPARE(ui_->item("recordingSavedPath")->property("text").toString(), file);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(stop));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("recordStop"));
        QTRY_VERIFY(!start->isEnabled() && !stop->isEnabled());
        ui_->endpoint.setRecording("Complete", false);
        QTRY_VERIFY(start->isEnabled() && path->isEnabled());
        QCOMPARE(ui_->item("recordingStatus")->property("text").toString(), QString("Recording: Complete"));
        ui_->endpoint.setRecording("Failed", false, "Cannot write WAV file: disk full");
        QTRY_VERIFY(ui_->item("recordingError")->isVisible());
        QCOMPARE(ui_->item("recordingError")->property("text").toString(), QString("Cannot write WAV file: disk full"));
        QVERIFY(ui_->closeAudioSettings());
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
                           "masterGain", "cueGain", "deck0CueSelect", "deck1CueSelect", "audioSettingsOpen", "themeToggle"});
        QVERIFY(ui_->openLibrary(0));
        verifyControlsFit({"deck0Path", "deck0Load", "deck1Path", "deck1Load", "libraryClose"});
        QVERIFY(ui_->closeLibrary());
        QVERIFY(ui_->openAudioSettings());
        verifyControlsFit({"masterLeftPort", "masterRightPort", "cueLeftPort", "cueRightPort",
                           "audioRoutingRefresh", "audioRoutingApply", "audioSettingsClose"});
        QVERIFY(ui_->closeAudioSettings());
    }

private:
    void choosePort(const char* name, int index)
    {
        auto* combo = ui_->item(name);
        QVERIFY(combo);
        combo->forceActiveFocus();
        QTRY_VERIFY(combo->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Home);
        for (int step = 0; step < index; ++step)
            QTest::keyClick(ui_->window, Qt::Key_Down);
        QTRY_COMPARE(combo->property("currentIndex").toInt(), index);
    }

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

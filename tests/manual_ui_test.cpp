// SPDX-License-Identifier: AGPL-3.0-or-later
#include <QGuiApplication>
#include <QAccessible>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>
#include <QVariantList>
#include <QWheelEvent>
#include <QtTest>

#include <memory>
#include <algorithm>
#include <array>
#include <cmath>

namespace {

// This endpoint records calls from actual QML controls. It creates no audio
// session; decoding/backend behavior is covered by the controller/core tests.
class UiEndpoint final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList decks READ decks NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool outputReady READ outputReady NOTIFY stateChanged)
    Q_PROPERTY(bool stemsAvailable READ stemsAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(double masterGain READ masterGain NOTIFY stateChanged)
    Q_PROPERTY(double cueGain READ cueGain NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap meters READ meters NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap audioRouting READ audioRouting NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap preparation READ preparation NOTIFY stateChanged)
    Q_PROPERTY(QVariantList libraryTracks READ libraryTracks NOTIFY libraryChanged)
    Q_PROPERTY(bool libraryReady READ libraryReady NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryError READ libraryError NOTIFY libraryChanged)
public:
    static QVariantMap silentMeter()
    {
        return {{"peakLeft", 0.0}, {"peakRight", 0.0}, {"rmsLeft", 0.0}, {"rmsRight", 0.0},
                {"overrangeLeft", 0}, {"overrangeRight", 0}};
    }
    UiEndpoint()
    {
        for (int deck = 0; deck < 2; ++deck)
            decks_.append(QVariantMap{{"loaded", false}, {"name", ""}, {"playing", false}, {"replacing", false},
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
        preparation_ = {{"status", "Idle"}, {"deck", -1}, {"name", ""},
                        {"cancellable", false}, {"cacheHit", false}};
    }
    QVariantList decks() const { return decks_; }
    bool busy() const { return busy_; }
    bool outputReady() const { return ready_; }
    bool stemsAvailable() const { return stemsAvailable_; }
    void setStemsAvailable(bool value) { stemsAvailable_ = value; emit stateChanged(); }
    void setOverviewStem(int deck, const QString& stem) {
        auto state = decks_[deck].toMap();
        state["overviewStem"] = stem;
        state["stemBank"] = !stem.isEmpty();
        state["stemGains"] = stem.isEmpty() ? QVariantList{0., 0., 0., 0.} : QVariantList{1., 1., 1., 1.};
        decks_[deck] = state;
        emit stateChanged();
    }
    QString error() const { return error_; }
    double masterGain() const { return master_; }
    double cueGain() const { return cue_; }
    QVariantMap meters() const { return meters_; }
    QVariantMap audioRouting() const { return routing_; }
    QVariantMap recording() const { return recording_; }
    QVariantMap preparation() const { return preparation_; }
    QVariantList libraryTracks() const { return library_; }
    bool libraryReady() const { return libraryReady_; }
    QString libraryError() const { return libraryError_; }
    void setLibrary(const QVariantList& tracks, bool ready = true, const QString& error = {})
    {
        library_ = tracks;
        libraryReady_ = ready;
        libraryError_ = error;
        emit libraryChanged();
    }
    void setPreparation(const QString& status, int deck = -1, bool cancellable = false,
                        bool cacheHit = false, const QString& name = {})
    {
        preparation_ = {{"status", status}, {"deck", deck},
                        {"name", !name.isEmpty() ? name : deck < 0 ? QString{} : QString("Pending %1.wav").arg(deck)},
                        {"cancellable", cancellable}, {"cacheHit", cacheHit}};
        busy_ = status == "Preparing" || status == "Cancelling" || status == "Installing" || status == "Saving";
        emit stateChanged();
    }
    void setBusy(bool value) { busy_ = value; emit stateChanged(); }
    void setReady(bool value) { ready_ = value; emit stateChanged(); }
    void setError(const QString& value) { error_ = value; emit stateChanged(); }
    void setCueAccepted(bool value) { acceptCue_ = value; }
    void setJogAccepted(bool value) { acceptJog_ = value; }
    void setReplacing(int deck, bool value)
    {
        auto state = decks_[deck].toMap();
        state["replacing"] = value;
        decks_[deck] = state;
        emit stateChanged();
    }
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
        decks_[deck] = QVariantMap{{"loaded", true}, {"name", QString("Track %1").arg(deck)}, {"replacing", false},
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
    Q_INVOKABLE void loadStemDeck(int deck, const QUrl& url)
    { calls.append(QVariantMap{{"action", "stemLoad"}, {"deck", deck}, {"url", url}}); }
    Q_INVOKABLE void loadLibraryStemTrack(int deck, const QString& id)
    { calls.append(QVariantMap{{"action", "libraryStemLoad"}, {"deck", deck}, {"id", id}}); }
    Q_INVOKABLE void importTrack(const QUrl& url)
    { calls.append(QVariantMap{{"action", "import"}, {"url", url}}); }
    Q_INVOKABLE void loadLibraryTrack(int deck, const QString& id)
    { calls.append(QVariantMap{{"action", "libraryLoad"}, {"deck", deck}, {"id", id}}); }
    Q_INVOKABLE bool cancelPreparation()
    {
        calls.append(QVariantMap{{"action", "cancelPreparation"}, {"deck", preparation_["deck"]}});
        if (!preparation_["cancellable"].toBool()) return false;
        preparation_["status"] = "Cancelling";
        preparation_["cancellable"] = false;
        emit stateChanged();
        return true;
    }
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
    Q_INVOKABLE bool jogFrames(int deck, qint64 frames)
    {
        calls.append(QVariantMap{{"action", "jog"}, {"deck", deck}, {"frames", frames}});
        if (!acceptJog_)
            return false;
        const auto state = decks_[deck].toMap();
        snapshotPosition(deck, std::clamp(state["positionSeconds"].toDouble() + frames / 48000.0,
                                         0.0, state["durationSeconds"].toDouble()));
        return true;
    }
    void snapshotStemGain(int deck, int stem, double gain)
    {
        auto state = decks_[deck].toMap();
        auto gains = state["stemGains"].toList();
        gains[stem] = gain;
        state["stemGains"] = gains;
        decks_[deck] = state;
        emit stateChanged();
    }
    Q_INVOKABLE void setStemGain(int deck, int stem, double gain)
    {
        calls.append(QVariantMap{{"action", "stemGain"}, {"deck", deck}, {"stem", stem}, {"value", gain}});
        snapshotStemGain(deck, stem, gain);
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
    void libraryChanged();
private:
    QVariantList decks_;
    QVariantList library_;
    QVariantMap meters_, routing_, recording_, preparation_;
    bool busy_ = false, ready_ = false, stemsAvailable_ = false;
    bool libraryReady_ = true;
    bool acceptCue_ = true, acceptJog_ = true;
    QString error_;
    QString libraryError_;
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
        for (const auto* name : {"deck0Browse", "deck0Cue", "deck0Play", "deck0Seek", "deck0Gain", "deck0Jog",
                                 "deck1Browse", "deck1Cue", "deck1Play", "deck1Seek", "deck1Gain", "deck1Jog",
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
        for (const auto* name : {"deck0Cue", "deck0Play", "deck0Seek", "deck0Gain", "deck0Jog", "deck1Cue", "deck1Play", "deck1Seek",
                                 "deck1Gain", "deck1Jog", "masterGain", "cueGain", "deck0CueSelect", "deck1CueSelect"})
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

    void libraryPreparationCancellation_data()
    {
        QTest::addColumn<bool>("keyboard");
        QTest::newRow("pointer") << false;
        QTest::newRow("keyboard") << true;
    }

    void libraryPreparationCancellation()
    {
        QFETCH(bool, keyboard);
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        QVERIFY(ui_->openLibrary(1));
        auto* cancel = ui_->item("preparationCancel");
        auto* status = ui_->item("preparationStatus");
        QVERIFY(cancel);
        QVERIFY(status);
        QVERIFY(!cancel->isVisible() || !cancel->isEnabled());
        QVERIFY(ui_->item("deck0Path")->setProperty("text", "/tmp/a.wav"));
        QVERIFY(ui_->item("deck1Path")->setProperty("text", "/tmp/b.wav"));
        ui_->endpoint.setPreparation("Preparing", 1, true);
        QTRY_VERIFY(cancel->isVisible() && cancel->isEnabled());
        QTRY_VERIFY(status->property("text").toString().contains("Preparing"));
        QVERIFY(status->property("text").toString().contains("Deck B", Qt::CaseInsensitive));
        QVERIFY(status->property("text").toString().contains("Pending 1.wav"));
        QVERIFY(!ui_->item("deck0Load")->isEnabled());
        QVERIFY(!ui_->item("deck1Load")->isEnabled());
        for (const auto* name : {"deck0Play", "deck1Play", "deck0Seek", "deck1Seek",
                                 "deck0Gain", "deck1Gain", "masterGain"})
            QVERIFY2(ui_->item(name)->isEnabled(), name);
        if (keyboard) {
            cancel->forceActiveFocus();
            QTRY_VERIFY(cancel->hasActiveFocus());
            QTest::keyClick(ui_->window, Qt::Key_Space);
        } else {
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cancel));
        }
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.first().toMap().value("action").toString(), QString("cancelPreparation"));
        QCOMPARE(ui_->endpoint.calls.first().toMap().value("deck").toInt(), 1);
        QVERIFY(ui_->endpoint.busy());
        QTRY_VERIFY(!cancel->isEnabled());
        QTRY_VERIFY(status->property("text").toString().contains("Cancelling"));
        if (cancel->isVisible())
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cancel));
        QCOMPARE(ui_->endpoint.calls.size(), 1);
        QVERIFY(!ui_->item("deck0Load")->isEnabled());
        QVERIFY(!ui_->item("deck1Load")->isEnabled());
        QVERIFY(ui_->closeLibrary());
        auto* play = ui_->item("deck0Play");
        play->forceActiveFocus();
        QTRY_VERIFY(play->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls.last().toMap().value("action").toString(), QString("playing"));
        QCOMPARE(ui_->endpoint.calls.last().toMap().value("deck").toInt(), 0);
        QVERIFY(ui_->endpoint.busy());
    }

    void libraryCloseAndInstallingDoNotCancelPreparation()
    {
        ui_->endpoint.setPreparation("Preparing", 0, true);
        QVERIFY(ui_->openLibrary(0));
        auto* cancel = ui_->item("preparationCancel");
        auto* status = ui_->item("preparationStatus");
        QVERIFY(cancel);
        QVERIFY(status);
        QTRY_VERIFY(cancel->isVisible() && cancel->isEnabled());
        QVERIFY(ui_->closeLibrary());
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QVERIFY(ui_->endpoint.preparation().value("cancellable").toBool());
        QVERIFY(ui_->openLibrary(0));
        cancel->forceActiveFocus();
        QTRY_VERIFY(cancel->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Escape);
        QTRY_VERIFY(!ui_->object("libraryDrawer")->property("visible").toBool());
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QVERIFY(ui_->endpoint.preparation().value("cancellable").toBool());
        QVERIFY(ui_->endpoint.busy());

        ui_->endpoint.setPreparation("Installing", 0, false, true);
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(!cancel->isEnabled());
        QTRY_VERIFY(status->property("text").toString().contains("Installing"));
        if (cancel->isVisible())
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cancel));
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QVERIFY(ui_->endpoint.busy());
        QCOMPARE(ui_->endpoint.preparation().value("status").toString(), QString("Installing"));
        QVERIFY(ui_->closeLibrary());
        QCOMPARE(ui_->endpoint.calls.size(), 0);
    }

    void libraryImportsLiteralPaths_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<bool>("keyboard");
        QTest::newRow("A-pointer") << 0 << false;
        QTest::newRow("B-keyboard") << 1 << true;
    }

    void libraryImportsLiteralPaths()
    {
        QFETCH(int, deck);
        QFETCH(bool, keyboard);
        QVERIFY(ui_->openLibrary(deck));
        const auto pathName = QString("deck%1Path").arg(deck).toUtf8();
        const auto importName = QString("deck%1Import").arg(deck).toUtf8();
        auto* path = ui_->item(pathName.constData());
        auto* importButton = ui_->item(importName.constData());
        QVERIFY(path);
        QVERIFY(importButton);
        const QString local = QString::fromUtf8("/tmp/Música Set/夜 #part%.wav");
        path->forceActiveFocus();
        QTRY_VERIFY(path->hasActiveFocus());
        QInputMethodEvent edit;
        edit.setCommitString(local);
        QGuiApplication::sendEvent(ui_->window, &edit);
        QTRY_COMPARE(path->property("text").toString(), local);
        QTRY_VERIFY(importButton->isEnabled());
        if (keyboard) {
            importButton->forceActiveFocus();
            QTRY_VERIFY(importButton->hasActiveFocus());
            QTest::keyClick(ui_->window, Qt::Key_Space);
        } else {
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(importButton));
        }
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.first().toMap();
        QCOMPARE(call["action"].toString(), QString("import"));
        QVERIFY(call["url"].toUrl().isLocalFile());
        QCOMPARE(call["url"].toUrl().toLocalFile(), local);
    }

    void librarySelectionLoadsByIdAndEditingRestoresRawPath()
    {
        const QString id(64, QLatin1Char('a'));
        const QString saved = QString::fromUtf8("/tmp/Música Set/Saved #%.wav");
        ui_->endpoint.setLibrary({QVariantMap{{"id", id}, {"name", "Saved track"}, {"path", saved},
                                             {"durationSeconds", 75.0}, {"available", true}}});
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        auto* row = ui_->item("libraryTrack0");
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(row));
        for (const auto* name : {"deck0Path", "deck1Path"})
            QTRY_COMPARE(ui_->item(name)->property("text").toString(), saved);
        auto* loadA = ui_->item("deck0Load");
        auto* loadB = ui_->item("deck1Load");
        QTRY_VERIFY(loadA->isEnabled() && loadB->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(loadA));
        loadB->forceActiveFocus();
        QTRY_VERIFY(loadB->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        for (int deck = 0; deck < 2; ++deck) {
            const auto call = ui_->endpoint.calls[deck].toMap();
            QCOMPARE(call["action"].toString(), QString("libraryLoad"));
            QCOMPARE(call["deck"].toInt(), deck);
            QCOMPARE(call["id"].toString(), id);
        }

        // A real edit must clear the saved ID; Enter then follows the raw path.
        auto* path = ui_->item("deck0Path");
        path->forceActiveFocus();
        QTRY_VERIFY(path->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_A, Qt::ControlModifier);
        const QString edited = QString::fromUtf8("/tmp/New 夜 #%.wav");
        QInputMethodEvent edit;
        edit.setCommitString(edited);
        QGuiApplication::sendEvent(ui_->window, &edit);
        QTRY_COMPARE(path->property("text").toString(), edited);
        QTest::keyClick(ui_->window, Qt::Key_Return);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        const auto raw = ui_->endpoint.calls.last().toMap();
        QCOMPARE(raw["action"].toString(), QString("load"));
        QCOMPARE(raw["deck"].toInt(), 0);
        QCOMPARE(raw["url"].toUrl().toLocalFile(), edited);
    }

    void libraryMissingRowsArePlainTextAndCannotLoad()
    {
        const QString untrusted = "<b>Missing & track</b>.wav";
        ui_->endpoint.setLibrary({QVariantMap{{"id", QString(64, QLatin1Char('b'))}, {"name", untrusted},
                                             {"path", "/tmp/missing.wav"}, {"durationSeconds", 12.0},
                                             {"available", false}}});
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        auto* name = ui_->item("libraryTrack0Name");
        QVERIFY(name);
        QCOMPARE(name->property("text").toString(), untrusted);
        QCOMPARE(name->property("textFormat").toInt(), int(Qt::PlainText));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        QTRY_VERIFY(!ui_->item("deck0Load")->isEnabled());
        QVERIFY(!ui_->item("deck1Load")->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck0Load")));
        ui_->item("deck1Path")->forceActiveFocus();
        QTest::keyClick(ui_->window, Qt::Key_Return);
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QVERIFY(ui_->item("libraryStatus"));
        QVERIFY(ui_->item("libraryStatus")->property("text").toString().contains("missing", Qt::CaseInsensitive));
    }

    void libraryReadinessAndBusyPreserveRawLoadingAndTransport()
    {
        const QVariantList tracks{QVariantMap{{"id", QString(64, QLatin1Char('c'))}, {"name", "Ready track"},
                                              {"path", "/tmp/ready.wav"}, {"durationSeconds", 30.0},
                                              {"available", true}}};
        ui_->endpoint.setLibrary({}, false);
        ui_->endpoint.loadState(0);
        ui_->endpoint.setReady(true);
        QVERIFY(ui_->openLibrary(0));
        auto* importButton = ui_->item("deck0Import");
        auto* status = ui_->item("libraryStatus");
        QVERIFY(importButton);
        QVERIFY(status);
        QVERIFY(ui_->item("deck0Path")->setProperty("text", "/tmp/raw.wav"));
        QTRY_VERIFY(ui_->item("deck0Load")->isEnabled());
        QVERIFY(!importButton->isEnabled());
        QVERIFY(status->property("text").toString().contains("loading", Qt::CaseInsensitive));
        ui_->endpoint.setLibrary({}, false, "Cannot read saved library: permission denied");
        QTRY_VERIFY(status->property("text").toString().contains("Cannot read saved library: permission denied"));
        QVERIFY(!importButton->isEnabled());
        QVERIFY(ui_->item("deck0Load")->isEnabled());
        ui_->endpoint.setLibrary({}, false);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck0Load")));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.first().toMap()["action"].toString(), QString("load"));
        ui_->endpoint.calls.clear();

        ui_->endpoint.setLibrary(tracks);
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        QTRY_VERIFY(ui_->item("deck0Load")->isEnabled() && importButton->isEnabled());
        ui_->endpoint.setBusy(true);
        QTRY_VERIFY(!importButton->isEnabled());
        QVERIFY(!ui_->item("deck1Import")->isEnabled());
        QVERIFY(!ui_->item("deck0Load")->isEnabled());
        QVERIFY(!ui_->item("deck1Load")->isEnabled());
        QVERIFY(ui_->closeLibrary());
        auto* play = ui_->item("deck0Play");
        play->forceActiveFocus();
        QTRY_VERIFY(play->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.first().toMap()["action"].toString(), QString("playing"));
        QVERIFY(ui_->endpoint.busy());

        ui_->endpoint.setBusy(false);
        ui_->endpoint.setLibrary({}, true, "Cannot read saved library");
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(status->property("text").toString().contains("Cannot read saved library"));
        ui_->endpoint.setLibrary({});
        QTRY_VERIFY(status->property("text").toString().contains("empty", Qt::CaseInsensitive));
    }

    void libraryImportCancellationStopsBeforeSaving()
    {
        ui_->endpoint.setPreparation("Preparing", -1, true, false, "Import #%.wav");
        QVERIFY(ui_->openLibrary(0));
        auto* cancel = ui_->item("preparationCancel");
        auto* status = ui_->item("preparationStatus");
        QVERIFY(cancel);
        QVERIFY(status);
        QTRY_VERIFY(cancel->isVisible() && cancel->isEnabled());
        QVERIFY(status->property("text").toString().contains("Library import", Qt::CaseInsensitive));
        QVERIFY(status->property("text").toString().contains("Import #%.wav"));
        // Query the interface exposed to assistive technology. Accessible is
        // a QML attached type, not a regular dotted QObject property path.
        auto* accessible = QAccessible::queryAccessibleInterface(cancel);
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), QString("Cancel library import"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cancel));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.first().toMap()["action"].toString(), QString("cancelPreparation"));
        QCOMPARE(ui_->endpoint.calls.first().toMap()["deck"].toInt(), -1);
        QVERIFY(ui_->endpoint.busy());
        QVERIFY(!cancel->isEnabled());
        ui_->endpoint.setPreparation("Saving", -1, false, false, "Import #%.wav");
        QTRY_VERIFY(status->property("text").toString().contains("Saving"));
        QVERIFY(!cancel->isEnabled());
        QVERIFY(ui_->closeLibrary());
        QCOMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.preparation()["status"].toString(), QString("Saving"));
    }

    void libraryLongListScrollsWithinDrawer()
    {
        QVariantList tracks;
        for (int index = 0; index < 40; ++index)
            tracks.append(QVariantMap{{"id", QString::number(index)}, {"name", QString("Track %1").arg(index)},
                                      {"path", QString("/tmp/track-%1.wav").arg(index)},
                                      {"durationSeconds", 60.0 + index}, {"available", true}});
        ui_->endpoint.setLibrary(tracks);
        ui_->window->resize(1180, 760);
        QVERIFY(ui_->openLibrary(0));
        auto* list = ui_->item("libraryTracks");
        QVERIFY(list);
        QTRY_VERIFY(list->isVisible() && list->height() > 0);
        QVERIFY(list->clip());
        QTRY_VERIFY(list->property("contentHeight").toDouble() > list->height());
        verifyControlsFit({"deck0Path", "deck0Import", "deck0Load", "deck1Path", "deck1Import", "deck1Load",
                           "libraryTracks", "libraryClose"});
        const auto before = list->property("contentY").toDouble();
        const auto point = ui_->center(list);
        QWheelEvent wheel(QPointF(point), QPointF(ui_->window->mapToGlobal(point)), {}, {0, -720},
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(ui_->window, &wheel);
        QTRY_VERIFY(list->property("contentY").toDouble() > before);
    }

    void jogKeyboardAndWheelUseRelativeRenderCommands()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            const auto name = QString("deck%1Jog").arg(deck).toUtf8();
            auto* jog = ui_->item(name.constData());
            QVERIFY(jog->isEnabled());
            jog->forceActiveFocus();
            QTRY_VERIFY(jog->hasActiveFocus());
            ui_->endpoint.calls.clear();
            QTest::keyClick(ui_->window, Qt::Key_Right);
            QTest::keyClick(ui_->window, Qt::Key_Left);
            QTest::keyClick(ui_->window, Qt::Key_Up, Qt::ShiftModifier);
            QTest::keyClick(ui_->window, Qt::Key_Down, Qt::ShiftModifier);
            QTRY_COMPARE(ui_->endpoint.calls.size(), 4);
            const std::array<qint64, 4> expected{480, -480, 48000, -48000};
            for (int index = 0; index < 4; ++index) {
                const auto call = ui_->endpoint.calls[index].toMap();
                QCOMPARE(call["action"].toString(), QString("jog"));
                QCOMPARE(call["deck"].toInt(), deck);
                QCOMPARE(call["frames"].toLongLong(), expected[index]);
            }
            ui_->endpoint.snapshotPosition(deck, 7.0);
            QCOMPARE(ui_->endpoint.calls.size(), 4); // Telemetry never submits a seek.
            const auto point = ui_->center(jog);
            auto wheel = [&](QPoint pixelDelta, QPoint angleDelta, Qt::KeyboardModifiers modifiers) {
                QWheelEvent event(QPointF(point), QPointF(ui_->window->mapToGlobal(point)),
                                  pixelDelta, angleDelta, Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
                QGuiApplication::sendEvent(ui_->window, &event);
                QCoreApplication::processEvents();
            };
            wheel({}, {0, 120}, Qt::NoModifier);
            wheel({}, {0, -120}, Qt::ShiftModifier);
            wheel({0, 20}, {}, Qt::NoModifier);
            QTRY_COMPARE(ui_->endpoint.calls.size(), 7);
            QCOMPARE(ui_->endpoint.calls[4].toMap()["frames"].toLongLong(), qint64(480));
            QCOMPARE(ui_->endpoint.calls[5].toMap()["frames"].toLongLong(), qint64(-48000));
            QCOMPARE(ui_->endpoint.calls[6].toMap()["frames"].toLongLong(), qint64(240));
            QVERIFY(!ui_->endpoint.decks()[deck].toMap()["playing"].toBool());
            QTest::keyClick(ui_->window, Qt::Key_Right, Qt::ControlModifier);
            QCOMPARE(ui_->endpoint.calls.size(), 7);
        }
    }

    void jogDragWrapsAngleAndStopsAfterRejectedMotion()
    {
        ui_->endpoint.loadState(0, 5.0);
        ui_->endpoint.setReady(true);
        auto* jog = ui_->item("deck0Jog");
        auto point = [jog](double angle) {
            const auto radius = jog->width() * .35;
            return jog->mapToScene(QPointF(jog->width() / 2 + std::cos(angle) * radius,
                                          jog->height() / 2 + std::sin(angle) * radius)).toPoint();
        };
        constexpr double pi = 3.14159265358979323846;
        auto totalFrames = [&] {
            qint64 frames = 0;
            for (const auto& item : ui_->endpoint.calls) {
                const auto call = item.toMap();
                if (call["action"].toString() == "jog") frames += call["frames"].toLongLong();
            }
            return frames;
        };
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point(-pi / 2));
        QTest::mouseMove(ui_->window, point(0));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(0));
        QTRY_VERIFY(!ui_->endpoint.calls.isEmpty());
        QVERIFY(std::abs(totalFrames() - 24000) < 500);
        ui_->endpoint.calls.clear();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point(pi - .15));
        QTest::mouseMove(ui_->window, point(-pi + .15));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(-pi + .15));
        QTRY_VERIFY(!ui_->endpoint.calls.isEmpty());
        QVERIFY(totalFrames() > 3000 && totalFrames() < 6000); // Crosses atan2's seam once.
        ui_->endpoint.calls.clear();
        ui_->endpoint.setJogAccepted(false);
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point(0));
        QTest::mouseMove(ui_->window, point(.3));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        ui_->endpoint.setJogAccepted(true);
        QTest::mouseMove(ui_->window, point(.6));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(.6));
        QCOMPARE(ui_->endpoint.calls.size(), 1); // A rejected drag needs a fresh press.
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(jog));
        QTest::mouseMove(ui_->window, point(0));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(0));
        QCOMPARE(ui_->endpoint.calls.size(), 1); // Leaving the dead zone never jumps.
    }

    void replacementGatesOnlyItsDeckAndCancelsJogGesture()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        auto* jog = ui_->item("deck0Jog");
        const auto start = jog->mapToScene(QPointF(jog->width() * .85, jog->height() / 2)).toPoint();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, start);
        ui_->endpoint.setReplacing(0, true);
        for (const auto* name : {"deck0Jog", "deck0Play", "deck0Seek", "deck0Gain", "deck0Cue", "deck0CueSelect"})
            QTRY_VERIFY2(!ui_->item(name)->isEnabled(), name);
        for (const auto* name : {"deck1Jog", "deck1Play", "deck1Gain", "deck1CueSelect", "masterGain", "cueGain"})
            QVERIFY2(ui_->item(name)->isEnabled(), name);
        ui_->endpoint.setReplacing(0, false);
        QTRY_VERIFY(jog->isEnabled());
        QTest::mouseMove(ui_->window, ui_->center(jog));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(jog));
        QCOMPARE(ui_->endpoint.calls.size(), 0);
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

    void preparedStemLoadingRequiresExplicitOptInAndPreservesImport()
    {
        QVERIFY(ui_->openLibrary(0));
        auto* mode = ui_->item("libraryPreparedStems");
        QVERIFY(mode);
        QVERIFY(!mode->isVisible());
        auto* path = ui_->item("deck0Path");
        auto* load = ui_->item("deck0Load");
        const QString local = QString::fromUtf8("/tmp/Stem Set/夜 #%.wav");
        QVERIFY(path->setProperty("text", local));
        QTRY_VERIFY(load->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(load));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("load"));
        ui_->endpoint.setStemsAvailable(true);
        QTRY_VERIFY(mode->isVisible() && mode->isEnabled());
        QVERIFY(!mode->property("checked").toBool());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(load));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("load"));
        mode->forceActiveFocus();
        QTRY_VERIFY(mode->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_VERIFY(mode->property("checked").toBool());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(load));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        const auto stemCall = ui_->endpoint.calls.last().toMap();
        QCOMPARE(stemCall["action"].toString(), QString("stemLoad"));
        QCOMPARE(stemCall["url"].toUrl().toLocalFile(), local);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck0Import")));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 4);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("import"));
        ui_->endpoint.setBusy(true);
        QTRY_VERIFY(!mode->isEnabled());
        ui_->endpoint.setBusy(false);
        const QString id(64, QLatin1Char('b'));
        ui_->endpoint.setLibrary({QVariantMap{{"id", id}, {"name", "Saved stems"}, {"path", local},
                                             {"durationSeconds", 60.0}, {"available", true}}});
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck1Load")));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 5);
        const auto savedCall = ui_->endpoint.calls.last().toMap();
        QCOMPARE(savedCall["action"].toString(), QString("libraryStemLoad"));
        QCOMPARE(savedCall["deck"].toInt(), 1);
        QCOMPARE(savedCall["id"].toString(), id);
        QVERIFY(ui_->closeLibrary());
        QVERIFY(ui_->warnings.isEmpty());
    }

    void stemLevelsRoutePointerKeyboardAndGateOnlyReplacementDeck()
    {
        ui_->window->resize(1180, 760);
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setOverviewStem(0, "drums");
        ui_->endpoint.setOverviewStem(1, "drums");
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck)
            for (int stem = 0; stem < 4; ++stem) {
                const auto name = QString("deck%1Stem%2Gain").arg(deck).arg(stem).toUtf8();
                auto* level = ui_->item(name.constData());
                QVERIFY(level);
                QTRY_VERIFY(level->isVisible() && level->isEnabled());
                QVERIFY(level->height() >= 44);
            }
        verifyControlsFit({"deck0Stem0Gain", "deck0Stem1Gain", "deck0Stem2Gain", "deck0Stem3Gain",
                           "deck1Stem0Gain", "deck1Stem1Gain", "deck1Stem2Gain", "deck1Stem3Gain"});
        auto* drums = ui_->item("deck0Stem0Gain");
        auto* vocals = ui_->item("deck1Stem3Gain");
        ui_->endpoint.snapshotStemGain(0, 0, .4);
        QTRY_VERIFY(std::abs(drums->property("value").toDouble() - .4) < 1e-6);
        QVERIFY(ui_->endpoint.calls.isEmpty());
        drums->forceActiveFocus();
        QTRY_VERIFY(drums->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Right);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto keyboard = ui_->endpoint.calls.last().toMap();
        QCOMPARE(keyboard["action"].toString(), QString("stemGain"));
        QCOMPARE(keyboard["deck"].toInt(), 0);
        QCOMPARE(keyboard["stem"].toInt(), 0);
        QVERIFY(std::abs(keyboard["value"].toDouble() - .41) < 1e-6);
        const auto point = vocals->mapToScene(QPointF(vocals->width() * .3, 32)).toPoint();
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_VERIFY(ui_->endpoint.calls.size() > 1);
        const auto pointer = ui_->endpoint.calls.last().toMap();
        QCOMPARE(pointer["action"].toString(), QString("stemGain"));
        QCOMPARE(pointer["deck"].toInt(), 1);
        QCOMPARE(pointer["stem"].toInt(), 3);
        QVERIFY(pointer["value"].toDouble() > .1 && pointer["value"].toDouble() < .5);
        auto* bass = ui_->item("deck0Stem1Gain");
        const auto drag = bass->mapToScene(QPointF(bass->width() * .6, 32)).toPoint();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, drag);
        QTRY_VERIFY(bass->property("pressed").toBool());
        const auto heldValue = bass->property("value").toDouble();
        ui_->endpoint.snapshotStemGain(0, 1, .1);
        QVERIFY(std::abs(bass->property("value").toDouble() - heldValue) < 1e-6);
        ui_->endpoint.setReplacing(0, true);
        for (int stem = 0; stem < 4; ++stem) {
            const auto name = QString("deck0Stem%1Gain").arg(stem).toUtf8();
            QTRY_VERIFY(!ui_->item(name.constData())->isEnabled());
        }
        QVERIFY(vocals->isEnabled());
        const auto beforeRelease = ui_->endpoint.calls.size();
        ui_->endpoint.setReplacing(0, false);
        QTest::mouseMove(ui_->window, ui_->center(bass));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(bass));
        QCOMPARE(ui_->endpoint.calls.size(), beforeRelease);
        ui_->endpoint.setOverviewStem(0, {});
        QTRY_VERIFY(!drums->isVisible());
        QVERIFY(vocals->isVisible());
        QVERIFY(ui_->warnings.isEmpty());
    }

    void stemOverviewIsLabeledWithoutChangingNormalDeckOverview()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setOverviewStem(0, "drums");
        auto* bank = ui_->item("deck0WaveformKind");
        auto* normal = ui_->item("deck1WaveformKind");
        QVERIFY(bank && normal);
        QTRY_COMPARE(bank->property("text").toString(),
                     QString("PREPARED STEMS · DRUMS WAVEFORM"));
        ui_->endpoint.setOverviewStem(0, "source");
        QTRY_COMPARE(bank->property("text").toString(),
                     QString("LIVE STEMS · SOURCE WAVEFORM"));
        auto* liveBass = ui_->item("deck0Stem1Gain");
        QVERIFY(liveBass);
        QTRY_VERIFY(liveBass->isVisible());
        QCOMPARE(normal->property("text").toString(), QString("LOCAL FILE"));
        ui_->endpoint.setOverviewStem(0, {});
        QTRY_COMPARE(bank->property("text").toString(), QString("LOCAL FILE"));
        QVERIFY(ui_->endpoint.calls.isEmpty());
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
    QAccessible::setActive(true);
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/Barlow-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/BarlowCondensed-SemiBold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(CONFLUX_SOURCE_DIR "/brand/assets/fonts/JetBrainsMono-Regular.ttf"));
    ManualUiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "manual_ui_test.moc"

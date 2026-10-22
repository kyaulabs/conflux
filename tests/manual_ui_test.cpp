// SPDX-License-Identifier: AGPL-3.0-or-later
#include <QGuiApplication>
#include <QAccessible>
#include <QFontDatabase>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QMetaProperty>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QWheelEvent>
#include <QtTest>
#include "../tools/experiments/PlaybackUiPolicy.hpp"

#include <memory>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <limits>
#include <vector>

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
    Q_PROPERTY(double crossfader READ crossfader NOTIFY stateChanged)
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
    static QVariantMap loopState(bool supported = false)
    {
        return {{"supported", supported}, {"inSet", false}, {"ready", false}, {"outSet", false},
                {"active", false}, {"cached", false}, {"inSeconds", 0.0}, {"minimumOutSeconds", 256.0 / 48000.0}, {"outSeconds", 0.0},
                {"limitSeconds", 0.0}, {"residentBytes", qulonglong(0)}, {"errorCode", 0}};
    }
    static QVariantMap tempoState(bool supported = false)
    {
        return {{"supported", supported}, {"percent", 0.0}, {"appliedPercent", 0.0},
                {"keyLock", false}, {"active", false}, {"pending", false}, {"finished", false},
                {"errorCode", 0}, {"generation", qulonglong(0)}};
    }
    static QVariantMap bpmState(double reference = 0)
    {
        return {{"estimated", reference}, {"reference", reference}, {"manual", false},
                {"status", reference > 0 ? "Estimated" : "Unavailable"}, {"error", ""}};
    }
    UiEndpoint()
    {
        for (int deck = 0; deck < 2; ++deck)
            decks_.append(QVariantMap{{"loaded", false}, {"name", ""}, {"sourceId", ""}, {"playing", false}, {"replacing", false},
                                      {"gain", .5}, {"positionSeconds", 0.0}, {"durationSeconds", 0.0},
                                      {"cueSeconds", 0.0}, {"cueHeld", false}, {"cuePreview", false},
                                      {"cueSelected", false}, {"meter", silentMeter()}, {"loop", loopState()}, {"tempo", tempoState()}, {"bpm", bpmState()},
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
    double crossfader() const { return crossfader_; }
    void snapshotCrossfader(double value) { crossfader_ = value; emit stateChanged(); }
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
        busy_ = status == "Preparing" || status == "Reading cues" || status == "Cancelling"
                || status == "Installing" || status == "Saving";
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
    void setSourceId(int deck, const QString& sourceId)
    {
        auto state = decks_[deck].toMap();
        state["sourceId"] = sourceId;
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
        decks_[deck] = QVariantMap{{"loaded", true}, {"name", QString("Track %1").arg(deck)}, {"sourceId", ""}, {"replacing", false},
                                   {"playing", false}, {"gain", .5},
                                   {"positionSeconds", position}, {"durationSeconds", 10.0},
                                   {"cueSeconds", 0.0}, {"cueHeld", false}, {"cuePreview", false},
                                   {"cueSelected", false}, {"meter", silentMeter()}, {"loop", loopState(true)}, {"tempo", tempoState(true)}, {"bpm", bpmState(120)},
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
    void snapshotDeck(int deck, const QVariantMap& fields)
    {
        auto state = decks_[deck].toMap();
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) state[it.key()] = it.value();
        decks_[deck] = state;
        emit stateChanged();
    }
    void snapshotLoop(int deck, const QVariantMap& loop)
    { snapshotDeck(deck, {{"loop", loop}}); }
    void snapshotTempo(int deck, const QVariantMap& tempo)
    { snapshotDeck(deck, {{"tempo", tempo}}); }
    void snapshotBpm(int deck, const QVariantMap& bpm)
    { snapshotDeck(deck, {{"bpm", bpm}}); }
    void snapshotSongInfo(int deck, const QString& title, const QString& artist,
                         const QString& album, const QString& artwork)
    {
        auto state = decks_[deck].toMap();
        state["title"] = title;
        state["artist"] = artist;
        state["album"] = album;
        state["artworkDataUrl"] = artwork;
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
    Q_INVOKABLE void importCueSheet(const QString& sourceId, const QUrl& url)
    { calls.append(QVariantMap{{"action", "cueImport"}, {"id", sourceId}, {"url", url}}); }
    Q_INVOKABLE bool jumpToImportedCue(int deck, const QString& sourceId, int cueIndex)
    {
        calls.append(QVariantMap{{"action", "cueJump"}, {"deck", deck}, {"id", sourceId}, {"index", cueIndex}});
        return true;
    }
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
    Q_INVOKABLE void setCrossfader(double position)
    {
        calls.append(QVariantMap{{"action", "crossfader"}, {"value", position}});
        snapshotCrossfader(position);
    }
    // Command recording deliberately does not acknowledge a loop or move the
    // cursor. Tests deliver separate snapshots, as the audio owner does.
    Q_INVOKABLE void loopIn(int deck)
    { calls.append(QVariantMap{{"action", "loopIn"}, {"deck", deck}}); }
    Q_INVOKABLE void loopOut(int deck)
    { calls.append(QVariantMap{{"action", "loopOut"}, {"deck", deck}}); }
    Q_INVOKABLE void loopToggle(int deck)
    { calls.append(QVariantMap{{"action", "loopToggle"}, {"deck", deck}}); }
    // Like loop commands, tempo requests do not mutate acknowledged snapshots.
    Q_INVOKABLE void setTempoPercent(int deck, double percent)
    { calls.append(QVariantMap{{"action", "tempo"}, {"deck", deck}, {"value", percent}}); }
    Q_INVOKABLE void setTempoBpm(int deck, double bpm)
    { calls.append(QVariantMap{{"action", "tempoBpm"}, {"deck", deck}, {"value", bpm}}); }
    Q_INVOKABLE void setBpmOverride(int deck, double bpm)
    { calls.append(QVariantMap{{"action", "bpmOverride"}, {"deck", deck}, {"value", bpm}}); }
    Q_INVOKABLE void clearBpmOverride(int deck)
    { calls.append(QVariantMap{{"action", "bpmAutomatic"}, {"deck", deck}}); }
    Q_INVOKABLE void toggleMasterTempo(int deck)
    { calls.append(QVariantMap{{"action", "masterTempo"}, {"deck", deck}}); }
    Q_INVOKABLE void resetTempo(int deck)
    { calls.append(QVariantMap{{"action", "tempoReset"}, {"deck", deck}}); }
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
    double crossfader_ = .5;
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

QString artworkDataUrl(const QColor& color)
{
    QImage cover(64, 64, QImage::Format_RGB32);
    for (int y = 0; y < cover.height(); ++y)
        for (int x = 0; x < cover.width(); ++x)
            cover.setPixelColor(x, y, color.lighter(70 + (x + y) * 80 / 126));
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !cover.save(&buffer, "PNG")) return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

struct UiFixture {
    UiEndpoint endpoint;
    QQmlApplicationEngine engine;
    QQuickWindow* window = nullptr;
    QStringList warnings;

    void load(bool visible = true)
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
        if (!visible) engine.setInitialProperties({{"visible", false}});
        engine.load(QUrl::fromLocalFile(QStringLiteral(CONFLUX_SOURCE_DIR "/qml/Main.qml")));
        if (!engine.rootObjects().isEmpty()) {
            window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (window) {
                window->resize(1680, 1020);
                if (visible) window->requestActivate();
            }
        }
    }
    QQuickItem* item(const char* name) const
    { return window ? visualItem(window->contentItem(), QString::fromUtf8(name)) : nullptr; }
    QObject* object(const char* name) const
    { return window ? window->findChild<QObject*>(QString::fromUtf8(name)) : nullptr; }
    bool useNonNativeFileDialog() const
    {
        auto* dialog = object("libraryFileDialog");
        if (!dialog) return false;
        QQmlExpression option(qmlContext(dialog), dialog, QStringLiteral("FileDialog.DontUseNativeDialog"));
        const auto value = option.evaluate();
        return !option.hasError() && value.isValid() && dialog->setProperty("options", value);
    }
    QQuickItem* fileDialogItem(const char* name) const
    {
        // Qt 6.4 uses an overlay; newer Qt can put the fallback in a popup window.
        for (auto* candidate : QGuiApplication::allWindows()) {
            auto* quickWindow = qobject_cast<QQuickWindow*>(candidate);
            if (!quickWindow || !quickWindow->isVisible()) continue;
            auto* found = visualItem(quickWindow->contentItem(), QString::fromUtf8(name));
            if (found && found->isVisible()) return found;
        }
        return nullptr;
    }
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
                                 "deck0Tempo", "deck0MasterTempo", "deck0TempoReset", "deck1Tempo", "deck1MasterTempo", "deck1TempoReset",
                                 "masterGain", "cueGain", "crossfader", "deck0CueSelect", "deck1CueSelect",
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

    void windowChromeFitsAndLeavesHeaderControlsReachable_data()
    {
        QTest::addColumn<QSize>("viewport");
        QTest::addColumn<bool>("dark");
        for (const auto viewport : {QSize(1180, 760), QSize(1280, 800), QSize(1440, 900), QSize(1680, 1020)})
            for (const bool dark : {false, true})
                QTest::newRow(qPrintable(QString("%1x%2-%3").arg(viewport.width()).arg(viewport.height()).arg(dark ? "dark" : "light")))
                    << viewport << dark;
    }

    void windowChromeFitsAndLeavesHeaderControlsReachable()
    {
        QFETCH(QSize, viewport);
        QFETCH(bool, dark);
        QVERIFY(ui_->window->flags().testFlag(Qt::FramelessWindowHint));
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        auto* theme = ui_->item("themeToggle");
        QVERIFY(theme);
        QTRY_VERIFY(ui_->center(theme).x() < viewport.width());
        if (!dark) QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(theme));
        QTRY_COMPARE(theme->property("text").toString(), dark ? QString("LIGHT") : QString("DARK"));
        verifyControlsFit({"windowDragArea", "libraryOpen", "audioSettingsOpen", "themeToggle",
                           "windowMinimize", "windowMaximize", "windowClose"});
        for (const auto& entry : {std::pair{"windowMinimize", "Minimize window"},
                                  std::pair{"windowMaximize", "Maximize window"},
                                  std::pair{"windowClose", "Close window"}}) {
            auto* item = ui_->item(entry.first);
            QVERIFY(item && item->width() >= 44 && item->height() >= 44);
            auto* accessible = QAccessible::queryAccessibleInterface(item);
            QVERIFY(accessible && accessible->role() == QAccessible::Button);
            QCOMPARE(accessible->text(QAccessible::Name), QString::fromUtf8(entry.second));
            QVERIFY(accessible->rect().contains(ui_->window->mapToGlobal(ui_->center(item))));
        }
        // Actual buttons remain hit-testable beside the drag region. System
        // move/resize behavior belongs to the compositor, not offscreen Qt.
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryOpen")));
        QTRY_VERIFY(ui_->object("libraryDrawer")->property("opened").toBool());
        QVERIFY(ui_->closeLibrary());
        QVERIFY(ui_->openAudioSettings());
        QVERIFY(ui_->closeAudioSettings());
        QCOMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.first().toMap()["action"].toString(), QString("refreshRouting"));
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void windowChromeActionsChangeActualWindowState()
    {
        auto* minimize = ui_->item("windowMinimize");
        auto* maximize = ui_->item("windowMaximize");
        auto* close = ui_->item("windowClose");
        QVERIFY(minimize && maximize && close);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(minimize));
        QTRY_COMPARE(ui_->window->visibility(), QWindow::Minimized);
        ui_->window->showNormal(); // Simulate restoring from the task switcher.
        QTRY_COMPARE(ui_->window->visibility(), QWindow::Windowed);
        QTRY_VERIFY(ui_->window->isExposed());
        maximize->forceActiveFocus();
        QTRY_VERIFY(maximize->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->window->visibility(), QWindow::Maximized);
        auto* accessible = QAccessible::queryAccessibleInterface(maximize);
        QVERIFY(accessible);
        QTRY_COMPARE(accessible->text(QAccessible::Name), QString("Restore window"));
        // The offscreen platform's virtual screen may be only 800px wide,
        // below the supported 1180px minimum. Exercise the same real action
        // by keyboard without claiming that unsupported maximized geometry.
        maximize->forceActiveFocus();
        QTRY_VERIFY(maximize->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->window->visibility(), QWindow::Windowed);
        QTRY_COMPARE(accessible->text(QAccessible::Name), QString("Maximize window"));
        QVERIFY(!QGuiApplication::quitOnLastWindowClosed()); // Test main keeps this close local to its fixture.
        QSignalSpy visible(ui_->window, &QWindow::visibleChanged);
        QTRY_VERIFY(ui_->center(close).x() < ui_->window->width());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(close));
        QTRY_VERIFY(!ui_->window->isVisible());
        QVERIFY(!visible.isEmpty());
        QVERIFY(ui_->endpoint.calls.isEmpty());
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void hiddenBenchmarkKeepsBindingsWithoutRendering()
    {
        ui_.reset();
        unsigned frames = 0;
        UiFixture hidden;
        hidden.load(false);
        QVERIFY2(hidden.window, qPrintable(hidden.warnings.join('\n')));
        QVERIFY(!hidden.window->isVisible());
        QVERIFY(!hidden.window->isExposed());
        QObject::connect(hidden.window, &QQuickWindow::frameSwapped, &hidden.engine, [&] { ++frames; });
        hidden.endpoint.setError("Hidden event delivery");
        auto* error = hidden.item("audioError");
        QVERIFY(error);
        QTRY_COMPARE(error->property("text").toString(), QString("Hidden event delivery"));
        QTest::qWait(100);
        QVERIFY(!hidden.window->isVisible());
        QVERIFY(!hidden.window->isExposed());
        QCOMPARE(frames, 0U);
        QVERIFY(hidden.warnings.isEmpty());
    }

    void hiddenBenchmarkRetainsBriefShowHide()
    {
        ui_.reset();
        using Policy = conflux::experiments::PlaybackUiPolicy;
        Policy policy(Policy::Mode::Hidden);
        UiFixture hidden;
        hidden.load(false);
        QVERIFY(hidden.window);
        auto* const window = hidden.window;
        auto state = [window] { return Policy::State{true, window->isVisible(), window->isExposed(), false, 0}; };
        QObject::connect(window, &QWindow::visibleChanged, &hidden.engine, [&policy, state](bool) { policy.observeActivity(state()); });
        QVERIFY(policy.healthy(false, state()));
        hidden.window->show();
        hidden.window->hide();
        QVERIFY(!hidden.window->isVisible());
        QVERIFY(!policy.healthy(false, state()));
        QVERIFY(!policy.healthy(true, state()));
        QVERIFY(policy.report()["hidden_policy_violated"].toBool());
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

    void libraryFileBrowseChoosesOnlyTargetPath_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<bool>("keyboard");
        QTest::newRow("A-pointer") << 0 << false;
        QTest::newRow("B-keyboard") << 1 << true;
    }

    void libraryFileBrowseChoosesOnlyTargetPath()
    {
        QFETCH(int, deck);
        QFETCH(bool, keyboard);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto local = directory.filePath(QString::fromUtf8("Música 夜 #part%23%.wav"));
        QFile file(local);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString id(64, QLatin1Char('a'));
        const QString saved = "/tmp/saved.wav";
        ui_->endpoint.setLibrary({QVariantMap{{"id", id}, {"name", "Saved track"}, {"path", saved},
                                             {"durationSeconds", 75.0}, {"available", true}}});
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto decksBefore = ui_->endpoint.decks();
        QVERIFY(ui_->openLibrary(deck));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        auto* drawer = ui_->object("libraryDrawer");
        QTRY_COMPARE(drawer->property("selectedId").toString(), id);
        QVERIFY(ui_->useNonNativeFileDialog());
        auto* dialog = ui_->object("libraryFileDialog");
        auto* browse = ui_->item(QString("deck%1FileBrowse").arg(deck).toUtf8().constData());
        QVERIFY(dialog && browse);
        QVERIFY(dialog->setProperty("currentFolder", QUrl::fromLocalFile(directory.path())));
        QSignalSpy accepted(dialog, SIGNAL(accepted()));
        QVERIFY(accepted.isValid());
        if (keyboard) {
            browse->forceActiveFocus();
            QTRY_VERIFY(browse->hasActiveFocus());
            QTest::keyClick(ui_->window, Qt::Key_Space);
        } else {
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(browse));
        }
        QTRY_VERIFY(dialog->property("visible").toBool());
        QCOMPARE(dialog->property("targetDeck").toInt(), deck);
        const auto selected = QUrl::fromLocalFile(local);
        QTRY_VERIFY(ui_->fileDialogItem("fileDialogDelegate0"));
        auto* choice = ui_->fileDialogItem("fileDialogDelegate0");
        QTRY_COMPARE(choice->property("file").toUrl(), selected);
        QTest::mouseClick(choice->window(), Qt::LeftButton, Qt::NoModifier, ui_->center(choice));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QCOMPARE(accepted.size(), 1);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        // The offscreen fallback owns another window; reactivate the test target.
        ui_->window->requestActivate();
        QTRY_VERIFY(ui_->window->isActive());
        auto* path = ui_->item(QString("deck%1Path").arg(deck).toUtf8().constData());
        const auto text = path->property("text").toString();
        QVERIFY(text.startsWith("file://"));
        QCOMPARE(QUrl(text).toLocalFile(), local);
        QCOMPARE(ui_->item(QString("deck%1Path").arg(1 - deck).toUtf8().constData())->property("text").toString(), saved);
        QCOMPARE(drawer->property("selectedId").toString(), QString{});
        QCOMPARE(ui_->endpoint.decks(), decksBefore);
        QVERIFY(ui_->endpoint.calls.isEmpty()); // Choosing a file does not load or import it.

        auto* add = ui_->item(QString("deck%1Import").arg(deck).toUtf8().constData());
        QCOMPARE(add->property("text").toString(), QString("ADD TO LIBRARY"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(add));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.first().toMap()["action"].toString(), QString("import"));
        QCOMPARE(ui_->endpoint.calls.first().toMap()["url"].toUrl().toLocalFile(), local);
        auto* load = ui_->item(QString("deck%1Load").arg(deck).toUtf8().constData());
        load->forceActiveFocus();
        QTRY_VERIFY(load->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        const auto call = ui_->endpoint.calls.last().toMap();
        QCOMPARE(call["action"].toString(), QString("load")); // Never the stale saved ID.
        QCOMPARE(call["deck"].toInt(), deck);
        QCOMPARE(call["url"].toUrl().toLocalFile(), local);
        QCOMPARE(ui_->endpoint.decks(), decksBefore);
    }

    void libraryFileBrowseCancelPreservesSelectionAndPlayback()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString id(64, QLatin1Char('b'));
        const QString saved = QString::fromUtf8("/tmp/Saved 夜 #%.wav");
        ui_->endpoint.setLibrary({QVariantMap{{"id", id}, {"name", "Saved track"}, {"path", saved},
                                             {"durationSeconds", 75.0}, {"available", true}}});
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto decksBefore = ui_->endpoint.decks();
        QVERIFY(ui_->openLibrary(1));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        QVERIFY(ui_->useNonNativeFileDialog());
        auto* dialog = ui_->object("libraryFileDialog");
        QVERIFY(dialog->setProperty("currentFolder", QUrl::fromLocalFile(directory.path())));
        QSignalSpy rejected(dialog, SIGNAL(rejected()));
        QVERIFY(rejected.isValid());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck1FileBrowse")));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(dialog->property("selectedFile").toUrl().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject", Qt::DirectConnection));
        QCOMPARE(rejected.size(), 1);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        for (const auto* name : {"deck0Path", "deck1Path"})
            QCOMPARE(ui_->item(name)->property("text").toString(), saved);
        QCOMPARE(ui_->object("libraryDrawer")->property("selectedId").toString(), id);
        QCOMPARE(ui_->endpoint.decks(), decksBefore);
        QVERIFY(ui_->endpoint.calls.isEmpty());

        // Cancelling after an actual file-list selection must preserve the same state.
        const auto unchosen = directory.filePath("unchosen.wav");
        QFile file(unchosen);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck1FileBrowse")));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(ui_->fileDialogItem("fileDialogDelegate0"));
        auto* choice = ui_->fileDialogItem("fileDialogDelegate0");
        QTRY_COMPARE(choice->property("file").toUrl(), QUrl::fromLocalFile(unchosen));
        QTest::mouseClick(choice->window(), Qt::LeftButton, Qt::NoModifier, ui_->center(choice));
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject", Qt::DirectConnection));
        QCOMPARE(rejected.size(), 2);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        for (const auto* name : {"deck0Path", "deck1Path"})
            QCOMPARE(ui_->item(name)->property("text").toString(), saved);
        QCOMPARE(ui_->object("libraryDrawer")->property("selectedId").toString(), id);
        QCOMPARE(ui_->endpoint.decks(), decksBefore);
        QVERIFY(ui_->endpoint.calls.isEmpty());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck1Load")));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.first().toMap();
        QCOMPARE(call["action"].toString(), QString("libraryLoad"));
        QCOMPARE(call["id"].toString(), id);
        QCOMPARE(call["deck"].toInt(), 1);
    }

    void libraryFileBrowseRejectsStaleResults_data()
    {
        QTest::addColumn<QString>("invalidate");
        QTest::newRow("busy") << QString("busy");
        QTest::newRow("closed") << QString("closed");
        QTest::newRow("new-saved-selection") << QString("selection");
    }

    void libraryFileBrowseRejectsStaleResults()
    {
        QFETCH(QString, invalidate);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto chosen = directory.filePath("late.wav");
        QFile file(chosen);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString oldId(64, QLatin1Char('c'));
        const QString newId(64, QLatin1Char('d'));
        const QVariantMap oldTrack{{"id", oldId}, {"name", "Old saved"}, {"path", "/tmp/old.wav"}, {"available", true}};
        const QVariantMap newTrack{{"id", newId}, {"name", "New saved"}, {"path", "/tmp/new.wav"}, {"available", true}};
        ui_->endpoint.setLibrary({oldTrack, newTrack});
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto decksBefore = ui_->endpoint.decks();
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        QVERIFY(ui_->useNonNativeFileDialog());
        auto* dialog = ui_->object("libraryFileDialog");
        auto* drawer = ui_->object("libraryDrawer");
        QVERIFY(dialog->setProperty("currentFolder", QUrl::fromLocalFile(directory.path())));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck0FileBrowse")));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(ui_->fileDialogItem("fileDialogDelegate0"));
        auto* choice = ui_->fileDialogItem("fileDialogDelegate0");
        QTRY_COMPARE(choice->property("file").toUrl(), QUrl::fromLocalFile(chosen));
        QTest::mouseClick(choice->window(), Qt::LeftButton, Qt::NoModifier, ui_->center(choice));
        if (invalidate == "busy") {
            ui_->endpoint.setBusy(true);
        } else if (invalidate == "closed") {
            QVERIFY(QMetaObject::invokeMethod(drawer, "close", Qt::DirectConnection));
        } else {
            QVERIFY(QMetaObject::invokeMethod(drawer, "selectTrack", Qt::DirectConnection,
                                              Q_ARG(QVariant, QVariant(newTrack))));
        }
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(dialog->property("targetDeck").toInt(), -1);
        const auto expectedId = invalidate == "selection" ? newId : oldId;
        const auto expectedPath = invalidate == "selection" ? QString("/tmp/new.wav") : QString("/tmp/old.wav");
        if (invalidate == "busy") {
            QVERIFY(!ui_->item("deck0FileBrowse")->isEnabled());
            QVERIFY(!ui_->item("deck1FileBrowse")->isEnabled());
            ui_->endpoint.setBusy(false); // Becoming idle must not revive an earlier selection.
        }
        if (invalidate == "closed") {
            QTRY_VERIFY(!drawer->property("visible").toBool());
            QVERIFY(ui_->openLibrary(1)); // Reopening likewise does not revive the old intent.
        }
        QSignalSpy accepted(dialog, SIGNAL(accepted()));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QCOMPARE(accepted.size(), 0); // A closed FileDialog cannot accept its earlier selection.
        QVERIFY(!dialog->property("visible").toBool());
        QCOMPARE(dialog->property("targetDeck").toInt(), -1);
        QCOMPARE(drawer->property("selectedId").toString(), expectedId);
        for (const auto* name : {"deck0Path", "deck1Path"})
            QCOMPARE(ui_->item(name)->property("text").toString(), expectedPath);
        QCOMPARE(ui_->endpoint.decks(), decksBefore);
        QVERIFY(ui_->endpoint.calls.isEmpty());
    }

    void libraryFileBrowseFitsAtLaptopSizes_data()
    {
        QTest::addColumn<QSize>("viewport");
        QTest::addColumn<bool>("dark");
        for (const auto viewport : {QSize(1180, 760), QSize(1280, 800)})
            for (const bool dark : {false, true})
                QTest::newRow(qPrintable(QString("%1x%2-%3").arg(viewport.width()).arg(viewport.height()).arg(dark ? "dark" : "light")))
                    << viewport << dark;
    }

    void libraryFileBrowseFitsAtLaptopSizes()
    {
        QFETCH(QSize, viewport);
        QFETCH(bool, dark);
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        auto* theme = ui_->item("themeToggle");
        QTRY_VERIFY(ui_->center(theme).x() < viewport.width());
        if (!dark)
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(theme));
        QTRY_COMPARE(theme->property("text").toString(), dark ? QString("LIGHT") : QString("DARK"));
        QVERIFY(ui_->openLibrary(0));
        verifyControlsFit({"deck0Path", "deck0FileBrowse", "deck0Import", "deck0Load",
                           "deck1Path", "deck1FileBrowse", "deck1Import", "deck1Load", "libraryClose"});
        for (int deck = 0; deck < 2; ++deck) {
            auto* browse = ui_->item(QString("deck%1FileBrowse").arg(deck).toUtf8().constData());
            QVERIFY(browse->width() >= 44 && browse->height() >= 44);
            auto* accessible = QAccessible::queryAccessibleInterface(browse);
            QVERIFY(accessible);
            QCOMPARE(accessible->role(), QAccessible::Button);
            QCOMPARE(accessible->text(QAccessible::Name), QString("Browse for Deck %1 audio file").arg(deck == 0 ? "A" : "B"));
            QVERIFY(accessible->text(QAccessible::Description).contains("does not load"));
            auto* add = ui_->item(QString("deck%1Import").arg(deck).toUtf8().constData());
            QCOMPARE(add->property("text").toString(), QString("ADD TO LIBRARY"));
            QVERIFY(add->implicitWidth() <= add->width() + .5);
        }
        QVERIFY(ui_->endpoint.calls.isEmpty());
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

    void selectedLibraryLevels_data()
    {
        QTest::addColumn<bool>("includeLevels");
        QTest::addColumn<QVariantMap>("levels");
        QTest::addColumn<QString>("rms");
        QTest::addColumn<QString>("peak");
        QTest::newRow("legacy-unknown") << false << QVariantMap{} << QString("unknown") << QString("unknown");
        QTest::newRow("unknown") << true << QVariantMap{} << QString("unknown") << QString("unknown");
        QTest::newRow("known") << true << QVariantMap{{"rms", .25}, {"samplePeak", .5}}
                              << QString("-12.04 dBFS") << QString("-6.02 dBFS");
        QTest::newRow("silence") << true << QVariantMap{{"rms", 0.0}, {"samplePeak", 0.0}}
                                << QString("−∞ dBFS (silence)") << QString("−∞ dBFS (silence)");
        QTest::newRow("float-headroom") << true << QVariantMap{{"rms", 1.25}, {"samplePeak", 2.0}}
                                       << QString("1.94 dBFS") << QString("6.02 dBFS");
        QTest::newRow("nonfinite-unknown") << true
            << QVariantMap{{"rms", std::numeric_limits<double>::quiet_NaN()},
                           {"samplePeak", std::numeric_limits<double>::infinity()}}
            << QString("unknown") << QString("unknown");
    }

    void selectedLibraryLevels()
    {
        QFETCH(bool, includeLevels);
        QFETCH(QVariantMap, levels);
        QFETCH(QString, rms);
        QFETCH(QString, peak);
        const QString id(64, QLatin1Char('c'));
        QVariantMap track{{"id", id}, {"name", "<b>Literal track</b>"}, {"path", "/tmp/levels.wav"},
                          {"durationSeconds", 75.0}, {"available", true}};
        if (includeLevels) track["levels"] = levels;
        ui_->endpoint.setLibrary({track});
        const auto before = ui_->endpoint.decks();
        const auto master = ui_->endpoint.masterGain(), cue = ui_->endpoint.cueGain();
        QVERIFY(ui_->openLibrary(0));
        auto* detail = ui_->item("libraryTrackLevels");
        auto* analysis = ui_->item("libraryTrackAnalysis");
        QVERIFY(detail && analysis);
        QCOMPARE(detail->property("text").toString(), QString("Whole-track RMS: unknown · Sample peak: unknown"));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        auto* row = ui_->item("libraryTrack0");
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(row));
        QTRY_COMPARE(detail->property("text").toString(), "Whole-track RMS: " + rms + " · Sample peak: " + peak);
        QCOMPARE(detail->property("textFormat").toInt(), int(Qt::PlainText));
        QCOMPARE(analysis->property("text").toString(), QString("Tempo / beat / phrase / key: unknown."));
        QCOMPARE(row->height(), 56.0);
        QCOMPARE(ui_->item("libraryTrack0Name")->property("text").toString(), QString("<b>Literal track</b>"));
        QCOMPARE(ui_->item("libraryTrack0Name")->property("textFormat").toInt(), int(Qt::PlainText));
        QVERIFY(ui_->item("deck0Load")->isEnabled() && ui_->item("deck1Load")->isEnabled());
        QVERIFY(!ui_->item("libraryCueImport")->isEnabled());
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QCOMPARE(ui_->endpoint.decks(), before);
        QCOMPARE(ui_->endpoint.masterGain(), master);
        QCOMPARE(ui_->endpoint.cueGain(), cue);

        // Both level lines must fit above the existing cue controls, including
        // silence's longer text, at the supported minimum window size.
        ui_->window->resize(1180, 760);
        auto* cuePath = ui_->item("libraryCuePath");
        auto* cueList = ui_->item("libraryCues");
        QVERIFY(cuePath && cueList);
        const auto bounds = [](QQuickItem* item) { return item->mapRectToScene(item->boundingRect()); };
        QTRY_VERIFY(detail->height() >= detail->implicitHeight() - .5);
        QTRY_VERIFY(analysis->height() >= analysis->implicitHeight() - .5);
        QTRY_VERIFY(bounds(detail).bottom() <= bounds(analysis).top() + .5);
        QTRY_VERIFY(bounds(analysis).bottom() <= bounds(cuePath).top() + .5);
        QTRY_VERIFY(bounds(cuePath).bottom() <= bounds(cueList).top() + .5);
        QTRY_VERIFY(bounds(cueList).bottom() <= ui_->window->height() + .5);
        QVERIFY(bounds(detail).left() >= 0 && bounds(detail).right() <= ui_->window->width());
    }

    void selectedLibraryLevelsRefreshForSameSource()
    {
        const QString id(64, QLatin1Char('d'));
        QVariantMap track{{"id", id}, {"name", "Reimported source"}, {"path", "/tmp/reimport.wav"},
                          {"durationSeconds", 75.0}, {"available", true},
                          {"levels", QVariantMap{{"rms", .25}, {"samplePeak", .5}}}};
        ui_->endpoint.setLibrary({track});
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        auto* detail = ui_->item("libraryTrackLevels");
        QVERIFY(detail);
        QTRY_COMPARE(detail->property("text").toString(), QString("Whole-track RMS: -12.04 dBFS · Sample peak: -6.02 dBFS"));
        const auto decks = ui_->endpoint.decks();
        const auto master = ui_->endpoint.masterGain(), cue = ui_->endpoint.cueGain();
        track["levels"] = QVariantMap{{"rms", .125}, {"samplePeak", .25}};
        ui_->endpoint.setLibrary({track}); // The selected ID remains the same during reimport/recipe updates.
        QTRY_COMPARE(detail->property("text").toString(), QString("Whole-track RMS: -18.06 dBFS · Sample peak: -12.04 dBFS"));
        QVERIFY(ui_->object("libraryDrawer")->property("opened").toBool());
        QCOMPARE(ui_->object("libraryDrawer")->property("selectedId").toString(), id);
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QCOMPARE(ui_->endpoint.decks(), decks);
        QCOMPARE(ui_->endpoint.masterGain(), master);
        QCOMPARE(ui_->endpoint.cueGain(), cue);
    }

    void cueSheetImportUsesSelectedSourceAndLiteralPath_data()
    {
        QTest::addColumn<int>("activation");
        QTest::newRow("pointer") << 0;
        QTest::newRow("button-keyboard") << 1;
        QTest::newRow("path-enter") << 2;
    }

    void cueSheetImportUsesSelectedSourceAndLiteralPath()
    {
        QFETCH(int, activation);
        const QString selectedId(64, QLatin1Char('d'));
        ui_->endpoint.setLibrary({
            QVariantMap{{"id", QString(64, QLatin1Char('a'))}, {"name", "Other source"},
                        {"path", "/tmp/other.wav"}, {"durationSeconds", 120.0}, {"available", true}},
            QVariantMap{{"id", selectedId}, {"name", "Selected source"}, {"path", "/tmp/selected.wav"},
                        {"durationSeconds", 120.0}, {"available", true}}});
        QVERIFY(ui_->openLibrary(0));
        auto* path = ui_->item("libraryCuePath");
        auto* importButton = ui_->item("libraryCueImport");
        QVERIFY(path);
        QVERIFY(importButton);
        const QString local = QString::fromUtf8("/tmp/Música Set/夜 #part%.cue");
        path->forceActiveFocus();
        QTRY_VERIFY(path->hasActiveFocus());
        QInputMethodEvent edit;
        edit.setCommitString(local);
        QGuiApplication::sendEvent(ui_->window, &edit);
        QTRY_COMPARE(path->property("text").toString(), local);
        QVERIFY(!importButton->isEnabled());
        QTest::keyClick(ui_->window, Qt::Key_Return);
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QTRY_VERIFY(ui_->item("libraryTrack1"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack1")));
        QTRY_VERIFY(importButton->isEnabled());
        if (activation == 0) {
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(importButton));
        } else {
            auto* target = activation == 1 ? importButton : path;
            target->forceActiveFocus();
            QTRY_VERIFY(target->hasActiveFocus());
            QTest::keyClick(ui_->window, activation == 1 ? Qt::Key_Space : Qt::Key_Return);
        }
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.first().toMap();
        QCOMPARE(call["action"].toString(), QString("cueImport"));
        QCOMPARE(call["id"].toString(), selectedId);
        QVERIFY(call["url"].toUrl().isLocalFile());
        QCOMPARE(call["url"].toUrl().toLocalFile(), local);
        for (const auto& state : ui_->endpoint.decks())
            QVERIFY(!state.toMap()["loaded"].toBool());
    }

    void importedCueJumpUsesSourceAndExactIndex()
    {
        const QString id(64, QLatin1Char('e'));
        const QString otherId(64, QLatin1Char('f'));
        const QString literal = "<b>Night & day</b>";
        const QVariantList cues{
            QVariantMap{{"trackNumber", 2}, {"label", "Opening"}, {"frame", qint64(0)}, {"seconds", 0.0}},
            QVariantMap{{"trackNumber", 7}, {"label", literal}, {"frame", qint64(2977920)}, {"seconds", 62.04}}};
        ui_->endpoint.setLibrary({
            QVariantMap{{"id", id}, {"name", "Cue source"}, {"path", "/tmp/cues.wav"},
                        {"durationSeconds", 120.0}, {"available", true}, {"cues", cues}},
            QVariantMap{{"id", otherId}, {"name", "Different source"}, {"path", "/tmp/different.wav"},
                        {"durationSeconds", 120.0}, {"available", true}, {"cues", cues}}});
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setSourceId(0, id);
        ui_->endpoint.setSourceId(1, otherId);
        ui_->endpoint.setReady(true);
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        QTRY_VERIFY(ui_->item("libraryCue1JumpA"));
        auto* name = ui_->item("libraryCue1Name");
        auto* jumpA = ui_->item("libraryCue1JumpA");
        auto* jumpB = ui_->item("libraryCue1JumpB");
        QVERIFY(name);
        QVERIFY(jumpB);
        QCOMPARE(name->property("text").toString(), "7. " + literal);
        QCOMPARE(name->property("textFormat").toInt(), int(Qt::PlainText));
        QCOMPARE(ui_->item("libraryCue1Time")->property("text").toString(), QString("1:02.040"));
        QTRY_VERIFY(jumpA->isEnabled());
        QVERIFY(!jumpB->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(jumpB));
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(jumpA));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        ui_->endpoint.setSourceId(1, id);
        QTRY_VERIFY(jumpB->isEnabled());
        jumpB->forceActiveFocus();
        QTRY_VERIFY(jumpB->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        for (int deck = 0; deck < 2; ++deck) {
            const auto call = ui_->endpoint.calls[deck].toMap();
            QCOMPARE(call["action"].toString(), QString("cueJump"));
            QCOMPARE(call["deck"].toInt(), deck);
            QCOMPARE(call["id"].toString(), id);
            QCOMPARE(call["index"].toInt(), 1); // Neither track number nor displayed seconds is a command.
        }
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack1")));
        QTRY_VERIFY(ui_->item("libraryCue1JumpA") && !ui_->item("libraryCue1JumpA")->isEnabled());
        QVERIFY(!ui_->item("libraryCue1JumpB")->isEnabled());
        QCOMPARE(ui_->endpoint.calls.size(), 2); // Changing selection never loads or seeks.
    }

    void cueImportAndJumpRespectBusyMissingAndLibraryReadiness()
    {
        const QString id(64, QLatin1Char('c'));
        const QVariantList cues{QVariantMap{{"trackNumber", 1}, {"label", "Start"},
                                            {"frame", qint64(0)}, {"seconds", 0.0}}};
        const QVariantList tracks{
            QVariantMap{{"id", id}, {"name", "Cue source"}, {"path", "/tmp/cues.wav"},
                        {"durationSeconds", 120.0}, {"available", true}, {"cues", cues}},
            QVariantMap{{"id", QString(64, QLatin1Char('b'))}, {"name", "Missing source"},
                        {"path", "/tmp/missing.wav"}, {"durationSeconds", 120.0},
                        {"available", false}, {"cues", cues}}};
        ui_->endpoint.setLibrary(tracks);
        ui_->endpoint.loadState(0);
        ui_->endpoint.setSourceId(0, id);
        ui_->endpoint.setReady(true);
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        auto* path = ui_->item("libraryCuePath");
        auto* importButton = ui_->item("libraryCueImport");
        QVERIFY(path);
        QVERIFY(importButton);
        QVERIFY(path->setProperty("text", "   "));
        QVERIFY(!importButton->isEnabled());
        QVERIFY(path->setProperty("text", "/tmp/track.cue"));
        QTRY_VERIFY(importButton->isEnabled());
        QTRY_VERIFY(ui_->item("libraryCue0JumpA") && ui_->item("libraryCue0JumpA")->isEnabled());
        QVERIFY(!ui_->item("libraryCue0JumpB")->isEnabled());
        auto* jumpA = ui_->item("libraryCue0JumpA");
        jumpA->forceActiveFocus();
        QTRY_VERIFY(jumpA->hasActiveFocus());
        ui_->endpoint.setReady(false);
        QTRY_VERIFY(!jumpA->isEnabled());
        QVERIFY(importButton->isEnabled()); // Cue metadata import does not require audio output.
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(jumpA));
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        ui_->endpoint.setReady(true);
        QTRY_VERIFY(jumpA->isEnabled());
        QVERIFY(importButton->isEnabled());
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        ui_->endpoint.setBusy(true);
        QTRY_VERIFY(!importButton->isEnabled());
        QVERIFY(!ui_->item("libraryCue0JumpA")->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(importButton));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryCue0JumpA")));
        path->forceActiveFocus();
        QTest::keyClick(ui_->window, Qt::Key_Return);
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        ui_->endpoint.setBusy(false);
        ui_->endpoint.setLibrary(tracks, false);
        QTRY_VERIFY(!importButton->isEnabled());
        QVERIFY(!ui_->item("libraryCue0JumpA")->isEnabled());
        ui_->endpoint.setLibrary(tracks);
        QTRY_VERIFY(importButton->isEnabled());
        ui_->endpoint.unloadState(0);
        QTRY_VERIFY(!ui_->item("libraryCue0JumpA")->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack1")));
        QTRY_VERIFY(!importButton->isEnabled());
        QTRY_VERIFY(ui_->item("libraryCue0JumpA") && !ui_->item("libraryCue0JumpA")->isEnabled());
        QVERIFY(ui_->item("libraryCueStatus")->property("text").toString().contains("missing", Qt::CaseInsensitive));
        QCOMPARE(ui_->endpoint.calls.size(), 0);
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

    void cueImportCanBeCancelledFromLibrary_data()
    {
        QTest::addColumn<bool>("keyboard");
        QTest::newRow("pointer") << false;
        QTest::newRow("keyboard") << true;
    }

    void cueImportCanBeCancelledFromLibrary()
    {
        QFETCH(bool, keyboard);
        ui_->endpoint.setPreparation("Reading cues", -1, true, false, "Album.cue");
        QVERIFY(ui_->openLibrary(0));
        auto* cancel = ui_->item("preparationCancel");
        QVERIFY(cancel);
        QTRY_VERIFY(cancel->isVisible() && cancel->isEnabled());
        QVERIFY(ui_->endpoint.busy());
        if (keyboard) {
            cancel->forceActiveFocus();
            QTRY_VERIFY(cancel->hasActiveFocus());
            QTest::keyClick(ui_->window, Qt::Key_Space);
        } else {
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cancel));
        }
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.first().toMap()["action"].toString(), QString("cancelPreparation"));
        QCOMPARE(ui_->endpoint.calls.first().toMap()["deck"].toInt(), -1);
        QCOMPARE(ui_->endpoint.preparation()["status"].toString(), QString("Cancelling"));
        QVERIFY(ui_->endpoint.busy());
        QTRY_VERIFY(!cancel->isEnabled());
    }

    void preparationTooltipShowsLiteralCueFilename()
    {
        const QString literal = "<img src='file:private'> Album & set "
                                + QString(160, QLatin1Char('x')) + ".cue";
        ui_->endpoint.setPreparation("Reading cues", -1, true, false, literal);
        QVERIFY(ui_->openLibrary(0));
        auto* status = ui_->item("preparationStatus");
        QVERIFY(status);
        QTRY_VERIFY(status->isVisible());
        const QString expected = "Reading cues · Library import · " + literal;
        QCOMPARE(status->property("text").toString(), expected);
        QCOMPARE(status->property("textFormat").toInt(), int(Qt::PlainText));
        QTest::mouseMove(ui_->window, ui_->center(status));
        // Inspect the rendered tooltip text rather than its stored source string.
        const auto findTooltipText = [status, &expected](auto&& find, QQuickItem* item) -> QQuickItem* {
            if (item != status && item->isVisible() && item->property("text").toString() == expected
                && item->property("textFormat").isValid())
                return item;
            for (auto* child : item->childItems())
                if (auto* found = find(find, child)) return found;
            return nullptr;
        };
        QQuickItem* tooltipText = nullptr;
        QTRY_VERIFY((tooltipText = findTooltipText(findTooltipText, ui_->window->contentItem())) != nullptr);
        QCOMPARE(tooltipText->property("text").toString(), expected);
        QCOMPARE(tooltipText->property("textFormat").toInt(), int(Qt::PlainText));
        QCOMPARE(ui_->endpoint.calls.size(), 0);
    }

    void libraryLongListScrollsWithinDrawer()
    {
        QVariantList cues;
        for (int index = 0; index < 40; ++index)
            cues.append(QVariantMap{{"trackNumber", index + 1}, {"label", QString("Cue %1").arg(index)},
                                    {"frame", qint64(index) * 48000}, {"seconds", double(index)}});
        QVariantList tracks;
        for (int index = 0; index < 40; ++index)
            tracks.append(QVariantMap{{"id", QString::number(index)}, {"name", QString("Track %1").arg(index)},
                                      {"path", QString("/tmp/track-%1.wav").arg(index)},
                                      {"durationSeconds", 60.0 + index}, {"available", true}, {"cues", cues}});
        ui_->endpoint.setLibrary(tracks);
        ui_->window->resize(1180, 760);
        QVERIFY(ui_->openLibrary(0));
        auto* list = ui_->item("libraryTracks");
        QVERIFY(list);
        QTRY_VERIFY(list->isVisible() && list->height() > 0);
        QVERIFY(list->clip());
        QTRY_VERIFY(list->property("contentHeight").toDouble() > list->height());
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        auto* cueList = ui_->item("libraryCues");
        QVERIFY(cueList);
        QTRY_VERIFY(cueList->isVisible() && cueList->height() > 0);
        QVERIFY(cueList->clip());
        QTRY_VERIFY(cueList->property("contentHeight").toDouble() > cueList->height());
        verifyControlsFit({"deck0Path", "deck0Import", "deck0Load", "deck1Path", "deck1Import", "deck1Load",
                           "libraryTracks", "libraryCuePath", "libraryCueImport", "libraryCues", "libraryClose"});
        const auto before = list->property("contentY").toDouble();
        const auto point = ui_->center(list);
        QWheelEvent wheel(QPointF(point), QPointF(ui_->window->mapToGlobal(point)), {}, {0, -720},
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        // Keep wheel time on QTest's mouse-event clock after selecting a row.
        // A zero timestamp uses Flickable's local timer and can precede that click.
        wheel.setTimestamp(++QTest::lastMouseTimestamp);
        QGuiApplication::sendEvent(ui_->window, &wheel);
        QTRY_VERIFY(list->property("contentY").toDouble() > before);
        const auto cueBefore = cueList->property("contentY").toDouble();
        const auto cuePoint = ui_->center(cueList);
        QWheelEvent cueWheel(QPointF(cuePoint), QPointF(ui_->window->mapToGlobal(cuePoint)), {}, {0, -720},
                             Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        cueWheel.setTimestamp(++QTest::lastMouseTimestamp);
        QGuiApplication::sendEvent(ui_->window, &cueWheel);
        QTRY_VERIFY(cueList->property("contentY").toDouble() > cueBefore);
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

    void manualLoopsRoutePressesAndWaitForAcknowledgedSnapshots()
    {
        ui_->endpoint.loadState(0, 3.0);
        ui_->endpoint.loadState(1, 3.0);
        ui_->endpoint.setReady(true);
        const auto master = ui_->endpoint.masterGain(), cue = ui_->endpoint.cueGain(), crossfader = ui_->endpoint.crossfader();
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            auto* in = ui_->item((prefix + "LoopIn").toUtf8().constData());
            auto* out = ui_->item((prefix + "LoopOut").toUtf8().constData());
            auto* toggle = ui_->item((prefix + "LoopToggle").toUtf8().constData());
            auto* status = ui_->item((prefix + "LoopStatus").toUtf8().constData());
            QVERIFY(in && out && toggle && status);
            ui_->endpoint.snapshotDeck(deck, {{"playing", true}});
            QTRY_VERIFY(in->isEnabled());
            QVERIFY(!out->isEnabled() && !toggle->isEnabled());
            const auto beforeIn = ui_->endpoint.decks();
            auto count = ui_->endpoint.calls.size();
            QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(in));
            QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1); // Dispatch belongs to press, not release.
            verifyCueCall(count, "loopIn", deck);
            QCOMPARE(ui_->endpoint.decks(), beforeIn);
            QVERIFY(!in->parentItem()->property("indicatorOn").toBool());
            QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(in));
            QCOMPARE(ui_->endpoint.calls.size(), count + 1);

            auto loop = UiEndpoint::loopState(true);
            loop["inSet"] = true; loop["inSeconds"] = 1.0; loop["minimumOutSeconds"] = (48000.0 + 256.0) / 48000.0;
            loop["limitSeconds"] = 8.0;
            ui_->endpoint.snapshotLoop(deck, loop);
            QTRY_VERIFY(status->property("text").toString().contains("PREPARING IN"));
            QVERIFY(in->parentItem()->property("indicatorOn").toBool());
            QVERIFY(!out->isEnabled() && !toggle->isEnabled());
            loop["ready"] = true; loop["residentBytes"] = qulonglong(7 * 48000 * 8);
            ui_->endpoint.snapshotLoop(deck, loop);
            QTRY_VERIFY(out->isEnabled());
            QVERIFY(status->property("text").toString().contains("OUT READY"));

            out->forceActiveFocus();
            QTRY_VERIFY(out->hasActiveFocus());
            const auto beforeOut = ui_->endpoint.decks();
            count = ui_->endpoint.calls.size();
            QTest::keyPress(ui_->window, Qt::Key_Space);
            QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1);
            verifyCueCall(count, "loopOut", deck);
            QCOMPARE(ui_->endpoint.decks(), beforeOut);
            QVERIFY(!out->parentItem()->property("indicatorOn").toBool());
            QVERIFY(!toggle->isEnabled());
            QTest::keyRelease(ui_->window, Qt::Key_Space);
            QCOMPARE(ui_->endpoint.calls.size(), count + 1);

            loop["outSet"] = true; loop["active"] = true; loop["cached"] = true; loop["outSeconds"] = 3.0;
            ui_->endpoint.snapshotLoop(deck, loop);
            QTRY_VERIFY(toggle->isEnabled());
            QVERIFY(!out->isEnabled());
            QCOMPARE(toggle->parentItem()->property("displayText").toString(), QString("EXIT"));
            QVERIFY(status->property("text").toString().startsWith("LOOP "));
            const auto beforeExit = ui_->endpoint.decks();
            count = ui_->endpoint.calls.size();
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(toggle));
            QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1);
            verifyCueCall(count, "loopToggle", deck);
            QCOMPARE(ui_->endpoint.decks(), beforeExit);
            QCOMPARE(toggle->parentItem()->property("displayText").toString(), QString("EXIT"));

            loop["active"] = false;
            ui_->endpoint.snapshotLoop(deck, loop);
            ui_->endpoint.snapshotDeck(deck, {{"playing", false}});
            QTRY_COMPARE(toggle->parentItem()->property("displayText").toString(), QString("RELOOP"));
            QVERIFY(status->property("text").toString().startsWith("RELOOP "));
            QVERIFY(toggle->isEnabled()); // Acknowledged bounds may be relooped from pause.
            toggle->forceActiveFocus();
            QTRY_VERIFY(toggle->hasActiveFocus());
            const auto beforeReloop = ui_->endpoint.decks();
            count = ui_->endpoint.calls.size();
            QTest::keyClick(ui_->window, Qt::Key_Space);
            QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1);
            verifyCueCall(count, "loopToggle", deck);
            QCOMPARE(ui_->endpoint.decks(), beforeReloop);
        }
        for (const auto& value : ui_->endpoint.calls) {
            const auto action = value.toMap()["action"].toString();
            QVERIFY(action == "loopIn" || action == "loopOut" || action == "loopToggle");
        }
        QCOMPARE(ui_->endpoint.masterGain(), master);
        QCOMPARE(ui_->endpoint.cueGain(), cue);
        QCOMPARE(ui_->endpoint.crossfader(), crossfader);
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void manualLoopAvailabilityKeepsInvalidPressesOutOfTheCommandPath()
    {
        struct Scenario {
            QString name;
            QVariantMap deck, loop;
            bool outputReady;
            std::array<bool, 3> enabled;
        };
        const std::vector<Scenario> scenarios{
            {"prepared", {}, {}, true, {true, true, false}},
            {"unloaded", {{"loaded", false}}, {}, true, {false, false, false}},
            {"output-unready", {}, {}, false, {false, false, false}},
            {"replacing", {{"replacing", true}}, {}, true, {false, false, false}},
            {"live-source", {}, {{"supported", false}}, true, {false, false, false}},
            {"cue-preview", {{"cuePreview", true}}, {}, true, {false, false, false}},
            {"source-read-error", {{"sourceReadError", true}}, {}, true, {false, false, false}},
            {"IN-pending", {}, {{"ready", false}}, true, {true, false, false}},
            {"no-IN", {}, {{"inSet", false}, {"ready", false}}, true, {true, false, false}},
            {"paused", {{"playing", false}}, {}, true, {true, false, false}},
            {"at-IN", {{"positionSeconds", 1.0}}, {}, true, {true, false, false}},
            {"255-frame-OUT", {{"positionSeconds", (48000.0 + 255.0) / 48000.0}}, {}, true, {true, false, false}},
            {"256-frame-OUT", {{"positionSeconds", (48000.0 + 256.0) / 48000.0}}, {}, true, {true, true, false}},
            {"at-limit", {{"positionSeconds", 8.0}}, {}, true, {true, true, false}},
            {"past-limit", {{"positionSeconds", 9.0}}, {}, true, {true, false, false}},
            {"EOF-past-limit", {{"positionSeconds", 10.0}}, {}, true, {false, false, false}},
            {"OUT-at-EOF", {{"positionSeconds", 10.0}}, {{"limitSeconds", 10.0}}, true, {false, true, false}},
            {"cache-error", {}, {{"ready", false}, {"errorCode", EIO}}, true, {true, false, false}},
            {"reloop-ready", {{"playing", false}}, {{"outSet", true}}, true, {true, false, true}},
            {"active-loop", {}, {{"outSet", true}, {"active", true}, {"cached", true}}, true, {true, false, true}},
        };
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        const std::array<const char*, 3> suffixes{"LoopIn", "LoopOut", "LoopToggle"};
        for (int deck = 0; deck < 2; ++deck) {
            for (const auto& scenario : scenarios) {
                QVariantMap state{{"loaded", true}, {"replacing", false}, {"playing", true}, {"cuePreview", false},
                                  {"sourceReadError", false}, {"positionSeconds", 3.0}, {"durationSeconds", 10.0}};
                auto loop = UiEndpoint::loopState(true);
                loop["inSet"] = true; loop["ready"] = true; loop["inSeconds"] = 1.0;
                loop["minimumOutSeconds"] = (48000.0 + 256.0) / 48000.0; loop["limitSeconds"] = 8.0;
                for (auto it = scenario.deck.cbegin(); it != scenario.deck.cend(); ++it) state[it.key()] = it.value();
                for (auto it = scenario.loop.cbegin(); it != scenario.loop.cend(); ++it) loop[it.key()] = it.value();
                state["loop"] = loop;
                ui_->endpoint.setReady(scenario.outputReady);
                ui_->endpoint.snapshotDeck(deck, state);
                for (std::size_t index = 0; index < suffixes.size(); ++index) {
                    const auto name = QString("deck%1%2").arg(deck).arg(suffixes[index]).toUtf8();
                    auto* control = ui_->item(name.constData());
                    QVERIFY(control);
                    QTRY_VERIFY2(control->isEnabled() == scenario.enabled[index],
                                 qPrintable(scenario.name + ": " + QString::fromUtf8(name)));
                    if (!scenario.enabled[index]) {
                        const auto count = ui_->endpoint.calls.size();
                        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(control));
                        QCOMPARE(ui_->endpoint.calls.size(), count);
                    }
                }
                const auto status = QString("deck%1LoopStatus").arg(deck).toUtf8();
                if (scenario.name == "cache-error")
                    QVERIFY(ui_->item(status.constData())->property("text").toString().contains("CACHE ERROR"));
                if (scenario.name == "past-limit")
                    QVERIFY(ui_->item(status.constData())->property("text").toString().contains("IN RANGE ENDED"));
                if (scenario.name == "at-IN" || scenario.name == "255-frame-OUT")
                    QCOMPARE(ui_->item(status.constData())->property("text").toString(), QString("IN SET / WAIT FOR OUT"));
                if (scenario.name == "256-frame-OUT") {
                    QVERIFY(ui_->item(status.constData())->property("text").toString().contains("OUT READY"));
                    const auto count = ui_->endpoint.calls.size();
                    const auto before = ui_->endpoint.decks();
                    auto* out = ui_->item(QString("deck%1LoopOut").arg(deck).toUtf8().constData());
                    QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(out));
                    QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1);
                    verifyCueCall(count, "loopOut", deck);
                    QCOMPARE(ui_->endpoint.decks(), before);
                }
            }
        }
        QCOMPARE(ui_->endpoint.calls.size(), 2); // Only the exact 256-frame boundary presses above.
    }

    void manualLoopControlsAreAccessibleAndFit_data()
    {
        QTest::addColumn<QSize>("viewport");
        QTest::addColumn<bool>("dark");
        for (const auto viewport : {QSize(1180, 760), QSize(1680, 1020)})
            for (const bool dark : {false, true})
                QTest::newRow(qPrintable(QString("%1x%2-%3").arg(viewport.width()).arg(viewport.height()).arg(dark ? "dark" : "light")))
                    << viewport << dark;
    }

    void manualLoopControlsAreAccessibleAndFit()
    {
        QFETCH(QSize, viewport);
        QFETCH(bool, dark);
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        if (!dark) QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("themeToggle")));
        ui_->endpoint.setReady(true);
        QStringList controls;
        for (int deck = 0; deck < 2; ++deck) {
            ui_->endpoint.loadState(deck);
            auto loop = UiEndpoint::loopState(true);
            loop["inSet"] = true; loop["ready"] = true; loop["outSet"] = true;
            loop["active"] = true; loop["cached"] = true; loop["inSeconds"] = 1.0;
            loop["minimumOutSeconds"] = (48000.0 + 256.0) / 48000.0; loop["outSeconds"] = 3.0;
            ui_->endpoint.snapshotLoop(deck, loop);
            for (const auto* suffix : {"LoopIn", "LoopOut", "LoopToggle", "LoopStatus", "Cue", "Play"})
                controls.append(QString("deck%1%2").arg(deck).arg(suffix));
            const auto label = deck == 0 ? QString("Deck A") : QString("Deck B");
            for (const auto& entry : {std::pair{"LoopIn", label + " loop IN"},
                                      std::pair{"LoopOut", label + " loop OUT"},
                                      std::pair{"LoopToggle", label + " loop EXIT"}}) {
                auto* button = ui_->item(QString("deck%1%2").arg(deck).arg(entry.first).toUtf8().constData());
                auto* accessible = QAccessible::queryAccessibleInterface(button);
                QVERIFY(accessible);
                QCOMPARE(accessible->role(), QAccessible::Button);
                QCOMPARE(accessible->text(QAccessible::Name), entry.second);
                const auto description = QString::fromUtf8(entry.first) == "LoopIn" ? "64 seconds" :
                                         QString::fromUtf8(entry.first) == "LoopOut" ? "loop end" : "without jumping";
                QVERIFY(accessible->text(QAccessible::Description).contains(description));
            }
            auto* status = ui_->item(QString("deck%1LoopStatus").arg(deck).toUtf8().constData());
            auto* accessible = QAccessible::queryAccessibleInterface(status);
            QVERIFY(accessible);
            QVERIFY(accessible->text(QAccessible::Name).contains(label));
            QVERIFY(accessible->text(QAccessible::Name).contains(status->property("text").toString()));
        }
        verifyControlsFit(controls);
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void manualLoopsDoNotEnableUnimplementedControls()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        unsigned hotCues = 0, equalizers = 0, memoryCalls = 0;
        const auto inspect = [&](auto&& self, QQuickItem* item) -> void {
            const auto reason = item->property("unavailableReason").toString();
            if (reason.contains("Hot cues") || reason.contains("Memory cue")) {
                QVERIFY(!item->property("available").toBool());
                hotCues += reason.contains("Hot cues");
                memoryCalls += reason.contains("Memory cue");
            }
            if (item->property("helpText").toString() == "Channel equalization is not implemented.") {
                QVERIFY(!item->property("activeControl").toBool());
                ++equalizers;
            }
            for (auto* child : item->childItems()) self(self, child);
        };
        inspect(inspect, ui_->window->contentItem());
        QVERIFY(hotCues > 0 && equalizers > 0 && memoryCalls > 0);
        QVERIFY(ui_->endpoint.calls.isEmpty());
    }

    void tempoCommandsWaitForAcknowledgedDeckSnapshots()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto master = ui_->endpoint.masterGain(), cue = ui_->endpoint.cueGain(), crossfader = ui_->endpoint.crossfader();
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            auto* rail = ui_->item((prefix + "Tempo").toUtf8().constData());
            auto* lock = ui_->item((prefix + "MasterTempo").toUtf8().constData());
            auto* reset = ui_->item((prefix + "TempoReset").toUtf8().constData());
            auto* value = ui_->item((prefix + "TempoValue").toUtf8().constData());
            auto* status = ui_->item((prefix + "TempoStatus").toUtf8().constData());
            QVERIFY(rail && lock && reset && value && status);
            auto tempo = UiEndpoint::tempoState(true);
            tempo["percent"] = -2.5; tempo["appliedPercent"] = -3.0; tempo["active"] = true;
            ui_->endpoint.snapshotTempo(deck, tempo);
            QTRY_COMPARE(rail->property("value").toDouble(), 117.0);
            QCOMPARE(value->property("text").toString(), QString("117.0"));
            QVERIFY(!lock->parentItem()->property("indicatorOn").toBool());
            QVERIFY(reset->isEnabled());
            const auto before = ui_->endpoint.decks();
            auto count = ui_->endpoint.calls.size();
            rail->forceActiveFocus();
            QTRY_VERIFY(rail->hasActiveFocus());
            QTest::keyClick(ui_->window, Qt::Key_Down);
            QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1);
            verifyCueCall(count, "tempoBpm", deck);
            QVERIFY(std::abs(ui_->endpoint.calls.last().toMap()["value"].toDouble() - 117.1) < 1e-9);
            QCOMPARE(ui_->endpoint.decks(), before);
            QCOMPARE(rail->property("value").toDouble(), 117.0);

            count = ui_->endpoint.calls.size();
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(lock));
            QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1);
            verifyCueCall(count, "masterTempo", deck);
            QVERIFY(!lock->parentItem()->property("indicatorOn").toBool());
            QCOMPARE(ui_->endpoint.decks(), before);
            tempo["percent"] = -2.0; tempo["keyLock"] = true; tempo["pending"] = true;
            ui_->endpoint.snapshotTempo(deck, tempo);
            QTRY_COMPARE(rail->property("value").toDouble(), 117.6);
            QVERIFY(lock->parentItem()->property("indicatorOn").toBool());
            QCOMPARE(status->property("text").toString(), QString("PENDING"));
            QVERIFY(rail->isEnabled() && lock->isEnabled()); // Pending input configuration accepts another request.
            rail->forceActiveFocus();
            count = ui_->endpoint.calls.size();
            QTest::keyClick(ui_->window, Qt::Key_Up, Qt::ShiftModifier);
            QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1);
            verifyCueCall(count, "tempoBpm", deck);
            QVERIFY(std::abs(ui_->endpoint.calls.last().toMap()["value"].toDouble() - 116.6) < 1e-9);
            QCOMPARE(rail->property("value").toDouble(), 117.6);

            count = ui_->endpoint.calls.size();
            const auto beforeReset = ui_->endpoint.decks();
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(reset));
            QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1);
            verifyCueCall(count, "tempoReset", deck);
            QCOMPARE(ui_->endpoint.decks(), beforeReset);
            QVERIFY(reset->isEnabled());
            tempo = UiEndpoint::tempoState(true); tempo["keyLock"] = true;
            ui_->endpoint.snapshotTempo(deck, tempo);
            QTRY_VERIFY(!reset->isEnabled());
            QCOMPARE(rail->property("value").toDouble(), 120.0);
            QCOMPARE(value->property("text").toString(), QString("120.0"));
            QVERIFY(lock->parentItem()->property("indicatorOn").toBool());
            QCOMPARE(status->property("text").toString(), QString("ORIGINAL"));
            QCOMPARE(ui_->endpoint.decks()[deck].toMap()["bpm"], beforeReset[deck].toMap()["bpm"]);
        }
        for (const auto& value : ui_->endpoint.calls) {
            const auto action = value.toMap()["action"].toString();
            QVERIFY(action == "tempoBpm" || action == "masterTempo" || action == "tempoReset");
        }
        QCOMPARE(ui_->endpoint.masterGain(), master);
        QCOMPARE(ui_->endpoint.cueGain(), cue);
        QCOMPARE(ui_->endpoint.crossfader(), crossfader);
    }

    void tempoPointerDirectionAndRejectedRequestRestore()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            auto* rail = ui_->item(QString("deck%1Tempo").arg(deck).toUtf8().constData());
            QVERIFY(rail);
            auto tempo = UiEndpoint::tempoState(true); tempo["active"] = true;
            ui_->endpoint.snapshotTempo(deck, tempo);
            QTRY_VERIFY(rail->isEnabled());
            // Exercise physical scene directions, independently of the
            // slider's local transform and ascending accessible range.
            const auto physical = rail->mapRectToScene(QRectF(0, 0, rail->width(), rail->height()));
            const auto middle = physical.center().toPoint();
            const auto bottom = QPointF(physical.center().x(), physical.bottom() - 1).toPoint();
            const auto top = QPointF(physical.center().x(), physical.top() + 1).toPoint();
            QVERIFY(top.y() < middle.y() && middle.y() < bottom.y());
            QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, middle);
            QTRY_VERIFY(rail->property("pressed").toBool());
            const auto held = rail->property("value").toDouble();
            const auto count = ui_->endpoint.calls.size();
            tempo["percent"] = 1.0;
            ui_->endpoint.snapshotTempo(deck, tempo);
            QCoreApplication::processEvents();
            QCOMPARE(rail->property("value").toDouble(), held);
            QCOMPARE(ui_->endpoint.calls.size(), count);
            QTest::mouseMove(ui_->window, bottom);
            QTRY_VERIFY(ui_->endpoint.calls.size() > count);
            verifyCueCall(ui_->endpoint.calls.size() - 1, "tempoBpm", deck);
            QCOMPARE(ui_->endpoint.calls.last().toMap()["value"].toDouble(), 127.2);
            QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, bottom);
            QTRY_COMPARE(rail->property("value").toDouble(), 121.2); // No command acknowledgement was delivered.
            const auto prior = ui_->endpoint.calls.size();
            QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, top);
            QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, top);
            QTRY_VERIFY(ui_->endpoint.calls.size() > prior);
            verifyCueCall(ui_->endpoint.calls.size() - 1, "tempoBpm", deck);
            QCOMPARE(ui_->endpoint.calls.last().toMap()["value"].toDouble(), 112.8);
            QTRY_COMPARE(rail->property("value").toDouble(), 121.2);
            rail->forceActiveFocus();
            QTest::keyClick(ui_->window, Qt::Key_Home);
            QCOMPARE(ui_->endpoint.calls.last().toMap()["value"].toDouble(), 112.8);
            QTest::keyClick(ui_->window, Qt::Key_End);
            QCOMPARE(ui_->endpoint.calls.last().toMap()["value"].toDouble(), 127.2);
        }
        for (const auto& value : ui_->endpoint.calls) {
            const auto call = value.toMap();
            QCOMPARE(call["action"].toString(), QString("tempoBpm"));
            QVERIFY(call["value"].toDouble() >= 112.8 - 1e-9 && call["value"].toDouble() <= 127.2 + 1e-9);
        }
    }

    void tempoAvailabilityAndFailureReset()
    {
        struct Scenario {
            const char* label;
            QVariantMap deck, tempo, loop;
            bool ready, controls, reset;
        };
        const std::array scenarios{
            Scenario{"ready", {}, {}, {}, true, true, false},
            Scenario{"unloaded", {{"loaded", false}}, {{"active", true}}, {}, true, false, false},
            Scenario{"output-unready", {}, {{"active", true}}, {}, false, false, false},
            Scenario{"unsupported", {}, {{"supported", false}, {"active", true}}, {}, true, false, false},
            Scenario{"replacing", {{"replacing", true}}, {{"active", true}}, {}, true, false, false},
            Scenario{"preview", {{"cuePreview", true}}, {{"active", true}}, {}, true, false, false},
            Scenario{"linear-read-error", {{"sourceReadError", true}}, {}, {}, true, false, false},
            Scenario{"worker-error-reset", {{"sourceReadError", true}}, {{"active", true}, {"errorCode", EIO}}, {}, true, false, true},
            Scenario{"early-worker-error", {}, {{"active", true}, {"errorCode", EIO}}, {}, true, false, true},
            Scenario{"loop-in-pending", {}, {}, {{"inSet", true}}, true, false, false},
            Scenario{"loop-out-stored", {}, {}, {{"outSet", true}}, true, false, false},
            Scenario{"loop-exit-remainder", {}, {}, {{"cached", true}}, true, false, false},
            Scenario{"pending", {}, {{"active", true}, {"pending", true}}, {}, true, true, true},
            Scenario{"processed-zero", {}, {{"active", true}, {"percent", 0.0}}, {}, true, true, true}
        };
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            auto* rail = ui_->item((prefix + "Tempo").toUtf8().constData());
            auto* lock = ui_->item((prefix + "MasterTempo").toUtf8().constData());
            auto* reset = ui_->item((prefix + "TempoReset").toUtf8().constData());
            auto* in = ui_->item((prefix + "LoopIn").toUtf8().constData());
            QVERIFY(rail && lock && reset && in);
            for (const auto& scenario : scenarios) {
                ui_->endpoint.loadState(deck);
                ui_->endpoint.setReady(scenario.ready);
                auto tempo = UiEndpoint::tempoState(true), loop = UiEndpoint::loopState(true);
                for (auto it = scenario.tempo.cbegin(); it != scenario.tempo.cend(); ++it) tempo[it.key()] = it.value();
                for (auto it = scenario.loop.cbegin(); it != scenario.loop.cend(); ++it) loop[it.key()] = it.value();
                ui_->endpoint.snapshotDeck(deck, scenario.deck);
                ui_->endpoint.snapshotTempo(deck, tempo);
                ui_->endpoint.snapshotLoop(deck, loop);
                QTRY_COMPARE(rail->isEnabled(), scenario.controls);
                QCOMPARE(lock->isEnabled(), scenario.controls);
                QCOMPARE(reset->isEnabled(), scenario.reset);
                if (tempo["active"].toBool()) QVERIFY2(!in->isEnabled(), scenario.label);
                const auto before = ui_->endpoint.calls.size();
                if (!scenario.controls) {
                    QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(rail));
                    QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(lock));
                    QCOMPARE(ui_->endpoint.calls.size(), before);
                }
                if (scenario.reset) {
                    QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(reset));
                    QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
                    verifyCueCall(before, "tempoReset", deck);
                }
            }
        }
    }

    void tempoAccessibleNumericEntryWaitsForAcknowledgement()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            auto* rail = ui_->item((prefix + "Tempo").toUtf8().constData());
            auto* label = ui_->item((prefix + "TempoValue").toUtf8().constData());
            QVERIFY(rail && label);
            auto tempo = UiEndpoint::tempoState(true);
            tempo["active"] = true; tempo["percent"] = -2.5;
            ui_->endpoint.snapshotTempo(deck, tempo);
            QTRY_COMPARE(rail->property("value").toDouble(), 117.0);
            auto* accessible = QAccessible::queryAccessibleInterface(rail);
            QVERIFY(accessible && accessible->valueInterface());
            auto* numeric = accessible->valueInterface();
            const auto before = ui_->endpoint.calls.size();
            const auto accepted = ui_->endpoint.decks();
            numeric->setCurrentValue(126.0); // Actual Qt path: setValue, no moved signal.
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            verifyCueCall(before, "tempoBpm", deck);
            QCOMPARE(ui_->endpoint.calls.last().toMap()["value"].toDouble(), 126.0);
            QCOMPARE(ui_->endpoint.decks(), accepted);
            QTRY_COMPARE(numeric->currentValue().toDouble(), 117.0);
            QCOMPARE(label->property("text").toString(), QString("117.0"));
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), before + 1);

            // A later engine snapshot is the only source of the visible value.
            tempo["percent"] = 5.0; tempo["pending"] = true;
            ui_->endpoint.snapshotTempo(deck, tempo);
            QTRY_COMPARE(numeric->currentValue().toDouble(), 126.0);
            QCOMPARE(label->property("text").toString(), QString("126.0"));
            QCOMPARE(ui_->endpoint.calls.size(), before + 1);
            numeric->setCurrentValue(123.0);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 2);
            verifyCueCall(before + 1, "tempoBpm", deck);
            QCOMPARE(ui_->endpoint.calls.last().toMap()["value"].toDouble(), 123.0);
            QTRY_COMPARE(numeric->currentValue().toDouble(), 126.0);

            // Qt still writes a disabled item's value directly. The QML gate
            // must reject it and restore acknowledgement, without a command.
            ui_->endpoint.setReady(false);
            QTRY_VERIFY(!rail->isEnabled());
            numeric->setCurrentValue(124.0);
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), before + 2);
            QTRY_COMPARE(numeric->currentValue().toDouble(), 126.0);
            ui_->endpoint.setReady(true);
            tempo["percent"] = -1.0; tempo["pending"] = false;
            ui_->endpoint.snapshotTempo(deck, tempo);
            QTRY_COMPARE(numeric->currentValue().toDouble(), 118.8);
            QCOMPARE(label->property("text").toString(), QString("118.8"));
            QCOMPARE(ui_->endpoint.calls.size(), before + 2); // Binding survived restoration.
        }
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void bpmReferenceChangesAndUnknownStateDoNotSubmitTempo()
    {
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            ui_->endpoint.loadState(deck);
            const auto prefix = QString("deck%1").arg(deck);
            auto* rail = ui_->item((prefix + "Tempo").toUtf8().constData());
            auto* lock = ui_->item((prefix + "MasterTempo").toUtf8().constData());
            auto* reset = ui_->item((prefix + "TempoReset").toUtf8().constData());
            auto* value = ui_->item((prefix + "TempoValue").toUtf8().constData());
            auto* reference = ui_->item((prefix + "BpmReference").toUtf8().constData());
            QVERIFY(rail && lock && reset && value && reference);
            auto tempo = UiEndpoint::tempoState(true);
            tempo["active"] = true; tempo["percent"] = 5.0; tempo["keyLock"] = true;
            ui_->endpoint.snapshotTempo(deck, tempo);
            QTRY_COMPARE(rail->property("value").toDouble(), 126.0);
            const auto before = ui_->endpoint.calls.size();
            ui_->endpoint.snapshotBpm(deck, UiEndpoint::bpmState());
            QTRY_VERIFY(!rail->isEnabled());
            QTRY_COMPARE(value->property("text").toString(), QString("—"));
            QVERIFY(reference->property("text").toString().contains("UNKNOWN"));
            QVERIFY(lock->isEnabled() && reset->isEnabled()); // Only the BPM rail needs a reference.
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), before);

            auto manual = UiEndpoint::bpmState(128);
            manual["estimated"] = 120.0; manual["manual"] = true;
            ui_->endpoint.snapshotBpm(deck, manual);
            QTRY_VERIFY(rail->isEnabled());
            QTRY_COMPARE(rail->property("value").toDouble(), 134.4);
            QCOMPARE(value->property("text").toString(), QString("134.4"));
            QVERIFY(reference->property("text").toString().contains("MANUAL"));
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), before); // Range clamps must not become commands.
            QCOMPARE(ui_->endpoint.decks()[deck].toMap()["tempo"].toMap(), tempo);
            // A failed automatic estimate does not erase an accepted manual reference.
            manual["estimated"] = 0.0; manual["status"] = "Failed"; manual["error"] = "No reliable estimate";
            ui_->endpoint.snapshotBpm(deck, manual);
            QTRY_VERIFY(rail->isEnabled());
            QCOMPARE(ui_->endpoint.calls.size(), before);

            ui_->endpoint.unloadState(deck);
            QTRY_VERIFY(!reference->isEnabled() && !rail->isEnabled());
            QTRY_COMPARE(value->property("text").toString(), QString("—"));
            ui_->endpoint.loadState(deck);
            ui_->endpoint.snapshotBpm(deck, UiEndpoint::bpmState());
            QTRY_VERIFY(!rail->isEnabled()); // A new unknown track cannot inherit the old reference.
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), before);
        }
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void bpmEditorUsesValidatedReferenceWithoutChangingAudio()
    {
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            auto* reference = ui_->item((prefix + "BpmReference").toUtf8().constData());
            QVERIFY(reference && !reference->isEnabled());
            ui_->endpoint.loadState(deck);
            // Reference editing needs a loaded track, not an active output.
            QVERIFY(reference->isEnabled());
            auto* referenceA11y = QAccessible::queryAccessibleInterface(reference);
            QVERIFY(referenceA11y && referenceA11y->role() == QAccessible::Button);
            QVERIFY(referenceA11y->text(QAccessible::Description).contains("not the audio"));
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(reference));
            auto* popup = ui_->object((prefix + "BpmEditor").toUtf8().constData());
            QVERIFY(popup);
            QTRY_VERIFY(popup->property("opened").toBool());
            auto* input = ui_->item((prefix + "BpmInput").toUtf8().constData());
            auto* apply = ui_->item((prefix + "BpmApply").toUtf8().constData());
            auto* automatic = ui_->item((prefix + "BpmAutomatic").toUtf8().constData());
            QVERIFY(input && apply && automatic);
            auto* inputA11y = QAccessible::queryAccessibleInterface(input);
            QVERIFY(inputA11y && inputA11y->text(QAccessible::Name).contains("manual reference BPM"));
            QVERIFY(!automatic->isEnabled());
            const auto before = ui_->endpoint.calls.size();
            const auto accepted = ui_->endpoint.decks();
            for (const auto* invalid : {"", "19.9", "400.1", "128.25", "nan", "inf"}) {
                QVERIFY(input->setProperty("text", QString::fromUtf8(invalid)));
                QTRY_VERIFY(!apply->isEnabled());
                QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(apply));
                QCOMPARE(ui_->endpoint.calls.size(), before);
                QCOMPARE(ui_->endpoint.decks(), accepted);
            }
            input->forceActiveFocus();
            QTest::keyClick(ui_->window, Qt::Key_A, Qt::ControlModifier);
            for (const auto key : {Qt::Key_1, Qt::Key_2, Qt::Key_8, Qt::Key_Period, Qt::Key_0})
                QTest::keyClick(ui_->window, key);
            QTRY_VERIFY(apply->isEnabled());
            if (deck == 0) QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(apply));
            else QTest::keyClick(ui_->window, Qt::Key_Return);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            verifyCueCall(before, "bpmOverride", deck);
            QCOMPARE(ui_->endpoint.calls.last().toMap()["value"].toDouble(), 128.0);
            QTRY_VERIFY(!popup->property("visible").toBool());
            QCOMPARE(ui_->endpoint.decks(), accepted); // The editor has not fabricated acknowledgement.

            auto manual = UiEndpoint::bpmState(128);
            manual["estimated"] = 120.0; manual["manual"] = true;
            ui_->endpoint.snapshotBpm(deck, manual);
            QTRY_VERIFY(reference->property("text").toString().contains("128.0 BPM · MANUAL"));
            auto loop = UiEndpoint::loopState(true); loop["inSet"] = true;
            ui_->endpoint.snapshotLoop(deck, loop);
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(reference));
            QTRY_VERIFY(popup->property("opened").toBool()); // Editing reference is allowed during a loop.
            QTRY_VERIFY(automatic->isEnabled());
            const auto manualAccepted = ui_->endpoint.decks();
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(automatic));
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 2);
            verifyCueCall(before + 1, "bpmAutomatic", deck);
            QTRY_VERIFY(!popup->property("visible").toBool());
            QCOMPARE(ui_->endpoint.decks(), manualAccepted);
            ui_->endpoint.snapshotBpm(deck, UiEndpoint::bpmState(120));
            QTRY_VERIFY(reference->property("text").toString().contains("120.0 BPM · ESTIMATED"));
            QCOMPARE(ui_->endpoint.calls.size(), before + 2);

            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(reference));
            QTRY_VERIFY(popup->property("opened").toBool());
            ui_->endpoint.setReplacing(deck, true);
            QTRY_VERIFY(!popup->property("visible").toBool());
            QVERIFY(!reference->isEnabled());
            QCOMPARE(ui_->endpoint.calls.size(), before + 2);
            ui_->endpoint.setReplacing(deck, false);
            ui_->endpoint.unloadState(deck);
            QTRY_VERIFY(!reference->isEnabled());
        }
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void bpmEditorDoesNotTransferDraftAcrossAtomicSourceReplacement()
    {
        for (int deck = 0; deck < 2; ++deck) {
            ui_->endpoint.loadState(deck);
            const auto oldId = QString("original-%1").arg(deck);
            const auto newId = QString("replacement-%1").arg(deck);
            ui_->endpoint.setSourceId(deck, oldId);
            auto manual = UiEndpoint::bpmState(128);
            manual["manual"] = true; manual["estimated"] = 120.0;
            ui_->endpoint.snapshotBpm(deck, manual);
            const auto prefix = QString("deck%1").arg(deck);
            auto* reference = ui_->item((prefix + "BpmReference").toUtf8().constData());
            QVERIFY(reference);
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(reference));
            auto* popup = ui_->object((prefix + "BpmEditor").toUtf8().constData());
            QVERIFY(popup);
            QTRY_VERIFY(popup->property("opened").toBool());
            QCOMPARE(popup->property("openedSourceId").toString(), oldId);
            auto* input = ui_->item((prefix + "BpmInput").toUtf8().constData());
            auto* apply = ui_->item((prefix + "BpmApply").toUtf8().constData());
            auto* automatic = ui_->item((prefix + "BpmAutomatic").toUtf8().constData());
            QVERIFY(input && apply && automatic);
            QVERIFY(input->setProperty("text", QString("132.0")));
            QTRY_VERIFY(apply->isEnabled() && automatic->isEnabled());
            const auto before = ui_->endpoint.calls.size();
            const auto peer = ui_->endpoint.decks()[1 - deck];
            // A no-session accepted load can replace the complete source in
            // one snapshot without any intermediate unloaded/replacing state.
            ui_->endpoint.snapshotDeck(deck, {{"sourceId", newId}, {"bpm", UiEndpoint::bpmState(100)}});
            QVERIFY(ui_->endpoint.decks()[deck].toMap()["loaded"].toBool());
            QVERIFY(!ui_->endpoint.decks()[deck].toMap()["replacing"].toBool());
            QTRY_VERIFY(!popup->property("visible").toBool());
            QVERIFY(!popup->property("sameSource").toBool());
            // Exercise both action guards even if an old action was queued
            // before the popup closed, independently of disabled hit testing.
            QVERIFY(QMetaObject::invokeMethod(popup, "applyReference", Qt::DirectConnection));
            QVERIFY(QMetaObject::invokeMethod(automatic, "clicked", Qt::DirectConnection));
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), before);
            QCOMPARE(ui_->endpoint.decks()[deck].toMap()["bpm"].toMap(), UiEndpoint::bpmState(100));
            QCOMPARE(ui_->endpoint.decks()[1 - deck], peer);
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(reference));
            QTRY_VERIFY(popup->property("opened").toBool());
            QCOMPARE(popup->property("openedSourceId").toString(), newId);
            QTRY_COMPARE(input->property("text").toString(), QString("100.0"));
            QTest::keyClick(ui_->window, Qt::Key_Escape);
            QTRY_VERIFY(!popup->property("visible").toBool());
            QCOMPARE(ui_->endpoint.calls.size(), before);
        }
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void tempoAccessibleAtLaptopSizes_data() { crossfaderAccessibleAtLaptopSizes_data(); }
    void tempoAccessibleAtLaptopSizes()
    {
        QFETCH(QSize, viewport);
        QFETCH(bool, dark);
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        auto* theme = ui_->item("themeToggle");
        QVERIFY(theme);
        QTRY_VERIFY(ui_->center(theme).x() < viewport.width());
        if (!dark) QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(theme));
        QTRY_COMPARE(theme->property("text").toString(), dark ? QString("LIGHT") : QString("DARK"));
        ui_->endpoint.setReady(true);
        QStringList controls;
        for (int deck = 0; deck < 2; ++deck) {
            ui_->endpoint.loadState(deck);
            auto tempo = UiEndpoint::tempoState(true); tempo["active"] = true; tempo["percent"] = -1.25;
            ui_->endpoint.snapshotTempo(deck, tempo);
            const auto prefix = QString("deck%1").arg(deck);
            for (const auto* suffix : {"Tempo", "MasterTempo", "TempoReset", "BpmReference", "Jog", "Cue", "Play", "LoopIn", "LoopOut", "LoopToggle"})
                controls.append(prefix + QString::fromUtf8(suffix));
            auto* bpmReference = ui_->item((prefix + "BpmReference").toUtf8().constData());
            QVERIFY(bpmReference && bpmReference->width() >= 44 && bpmReference->height() >= 44);
            auto* rail = ui_->item((prefix + "Tempo").toUtf8().constData());
            QVERIFY(rail && rail->width() >= 44 && rail->height() >= 44);
            auto* accessible = QAccessible::queryAccessibleInterface(rail);
            QVERIFY(accessible);
            QCOMPARE(accessible->role(), QAccessible::Slider);
            QCOMPARE(accessible->text(QAccessible::Name), deck == 0 ? QString("Deck A tempo BPM") : QString("Deck B tempo BPM"));
            QVERIFY(accessible->rect().contains(ui_->window->mapToGlobal(ui_->center(rail))));
            QVERIFY(accessible->text(QAccessible::Description).contains("Down increases speed"));
            QVERIFY(accessible->valueInterface());
            QCOMPARE(accessible->valueInterface()->currentValue().toDouble(), 118.5);
            QVERIFY(accessible->valueInterface()->minimumValue().toDouble() <= 112.8);
            QVERIFY(accessible->valueInterface()->maximumValue().toDouble() >= 127.2);
            QVERIFY(accessible->actionInterface());
            auto before = ui_->endpoint.calls.size();
            accessible->actionInterface()->doAction(QAccessibleActionInterface::increaseAction());
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            verifyCueCall(before, "tempoBpm", deck);
            QVERIFY(std::abs(ui_->endpoint.calls.last().toMap()["value"].toDouble() - 118.6) < 1e-9);
            QCOMPARE(rail->property("value").toDouble(), 118.5);
            auto* reset = ui_->item((prefix + "TempoReset").toUtf8().constData());
            auto* resetAccessible = QAccessible::queryAccessibleInterface(reset);
            QVERIFY(resetAccessible && resetAccessible->text(QAccessible::Name).contains("reset tempo"));
            rail->forceActiveFocus();
            QTRY_VERIFY(rail->hasActiveFocus());
            before = ui_->endpoint.calls.size();
            QTest::keyClick(ui_->window, Qt::Key_Down);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            verifyCueCall(before, "tempoBpm", deck);
            QTest::keyClick(ui_->window, Qt::Key_Tab);
            QTRY_VERIFY(!rail->hasActiveFocus());
        }
        verifyControlsFit(controls);
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier,
                              ui_->center(ui_->item((prefix + "BpmReference").toUtf8().constData())));
            auto* popup = ui_->object((prefix + "BpmEditor").toUtf8().constData());
            QVERIFY(popup);
            QTRY_VERIFY(popup->property("opened").toBool());
            verifyControlsFit({prefix + "BpmInput", prefix + "BpmApply", prefix + "BpmAutomatic"});
            QTest::keyClick(ui_->window, Qt::Key_Escape);
            QTRY_VERIFY(!popup->property("visible").toBool());
        }
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void crossfaderSnapshotsAndOutputGating()
    {
        auto* fader = ui_->item("crossfader");
        QVERIFY(fader);
        QCOMPARE(fader->property("value").toDouble(), .5);
        QVERIFY(!fader->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(fader));
        QVERIFY(ui_->endpoint.calls.isEmpty());

        // It is a global master-bus control, usable with one loaded deck and
        // during another deck's preparation/replacement, like MASTER LEVEL.
        ui_->endpoint.loadState(0);
        ui_->endpoint.setReady(true);
        QTRY_VERIFY(fader->isEnabled());
        ui_->endpoint.setBusy(true);
        ui_->endpoint.setReplacing(0, true);
        QVERIFY(fader->isEnabled());
        ui_->endpoint.snapshotCrossfader(.75);
        QTRY_COMPARE(fader->property("value").toDouble(), .75);
        ui_->endpoint.loadState(1);
        ui_->endpoint.snapshotPosition(1, 4.0);
        ui_->endpoint.setError("A source is preparing");
        QCoreApplication::processEvents();
        QCOMPARE(fader->property("value").toDouble(), .75);
        QVERIFY(ui_->endpoint.calls.isEmpty());
        ui_->endpoint.setReady(false);
        QTRY_VERIFY(!fader->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier,
                          fader->mapToScene(QPointF(1, fader->height() / 2)).toPoint());
        QVERIFY(ui_->endpoint.calls.isEmpty());
    }

    void crossfaderRoutesKeyboardAndPointerWithoutOtherCommands()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto decks = ui_->endpoint.decks();
        const auto master = ui_->endpoint.masterGain(), cue = ui_->endpoint.cueGain();
        auto* fader = ui_->item("crossfader");
        QVERIFY(fader);
        fader->forceActiveFocus();
        QTRY_VERIFY(fader->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Left);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QVERIFY(std::abs(ui_->endpoint.crossfader() - .49) < 1e-6);
        QTest::keyClick(ui_->window, Qt::Key_Right);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QVERIFY(std::abs(ui_->endpoint.crossfader() - .5) < 1e-6);

        const auto left = fader->mapToScene(QPointF(1, 34)).toPoint();
        const auto right = fader->mapToScene(QPointF(fader->width() - 1, 34)).toPoint();
        const auto middle = fader->mapToScene(QPointF(fader->width() / 2, 34)).toPoint();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, middle);
        QTRY_VERIFY(fader->property("pressed").toBool());
        const auto held = fader->property("value").toDouble();
        const auto beforeSnapshot = ui_->endpoint.calls.size();
        ui_->endpoint.snapshotCrossfader(.1);
        QCoreApplication::processEvents();
        QCOMPARE(fader->property("value").toDouble(), held);
        QCOMPARE(ui_->endpoint.calls.size(), beforeSnapshot);
        QTest::mouseMove(ui_->window, left);
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, left);
        QTRY_COMPARE(ui_->endpoint.crossfader(), 0.0);
        QTRY_COMPARE(fader->property("value").toDouble(), 0.0);
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, left);
        QTest::mouseMove(ui_->window, right);
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, right);
        QTRY_COMPARE(ui_->endpoint.crossfader(), 1.0);
        QTRY_COMPARE(fader->property("value").toDouble(), 1.0);
        for (const auto& entry : ui_->endpoint.calls) {
            const auto call = entry.toMap();
            QCOMPARE(call["action"].toString(), QString("crossfader"));
            QVERIFY(call["value"].toDouble() >= 0 && call["value"].toDouble() <= 1);
        }
        QCOMPARE(ui_->endpoint.decks(), decks);
        QCOMPARE(ui_->endpoint.masterGain(), master);
        QCOMPARE(ui_->endpoint.cueGain(), cue);
        QTest::keyClick(ui_->window, Qt::Key_Tab);
        QTRY_VERIFY(!fader->hasActiveFocus());
        QVERIFY(ui_->window->activeFocusItem());
    }

    void crossfaderAccessibleAtLaptopSizes_data()
    {
        QTest::addColumn<QSize>("viewport");
        QTest::addColumn<bool>("dark");
        for (const auto viewport : {QSize(1180, 760), QSize(1680, 1020)})
            for (const bool dark : {false, true})
                QTest::newRow(qPrintable(QString("%1x%2-%3").arg(viewport.width()).arg(viewport.height()).arg(dark ? "dark" : "light")))
                    << viewport << dark;
    }

    void crossfaderAccessibleAtLaptopSizes()
    {
        QFETCH(QSize, viewport);
        QFETCH(bool, dark);
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        if (!dark)
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("themeToggle")));
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        ui_->endpoint.snapshotCrossfader(.75);
        auto* fader = ui_->item("crossfader");
        QVERIFY(fader);
        QTRY_COMPARE(fader->property("value").toDouble(), .75);
        QVERIFY(fader->height() >= 44);
        verifyControlsFit({"crossfader", "deck0Gain", "deck1Gain", "masterGain", "cueGain",
                           "deck0CueSelect", "deck1CueSelect"});
        auto* accessible = QAccessible::queryAccessibleInterface(fader);
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::Slider);
        QCOMPARE(accessible->text(QAccessible::Name), QString("Crossfader A to B"));
        QVERIFY(accessible->text(QAccessible::Description).contains("both channel levels"));
        QVERIFY(accessible->valueInterface());
        QCOMPARE(accessible->valueInterface()->currentValue().toDouble(), .75);
        fader->forceActiveFocus();
        QTRY_VERIFY(fader->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Left);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("crossfader"));
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
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
        const QString literal = "CUE command <b>invalid & unsafe</b> <img src='file:///tmp/private'> "
                                + QString(256, QLatin1Char('x'));
        ui_->endpoint.setError(literal);
        QTRY_COMPARE(error->property("text").toString(), literal);
        QCOMPARE(error->property("textFormat").toInt(), int(Qt::PlainText));
        QTest::mouseMove(ui_->window, ui_->center(error));
        auto* tooltip = ui_->object("audioErrorTooltip");
        QVERIFY(tooltip);
        QTRY_VERIFY(tooltip->property("visible").toBool());
        auto* tooltipText = ui_->item("audioErrorTooltipText");
        QVERIFY(tooltipText);
        QVERIFY(tooltipText->isVisible());
        QCOMPARE(tooltipText->property("text").toString(), literal);
        QCOMPARE(tooltipText->property("textFormat").toInt(), int(Qt::PlainText));
        QCOMPARE(ui_->endpoint.calls.size(), 0);
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
        const QString cueError = "CUE command <b>invalid & unsafe</b> <img src='file:///tmp/private'>";
        ui_->endpoint.setError(cueError);
        auto* routingError = ui_->item("routingError");
        QTRY_COMPARE(routingError->property("text").toString(), "Cue device disconnected\n" + cueError);
        QCOMPARE(routingError->property("textFormat").toInt(), int(Qt::PlainText));
        QCOMPARE(ui_->endpoint.calls.size(), 1); // Error snapshots never submit another routing command.
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

    void deckLcdShowsLiteralSongInfoAndStableArtwork()
    {
        const auto artwork = artworkDataUrl(QColor("#48b8a4"));
        QVERIFY(!artwork.isEmpty());
        const QString title = "<b>Night & day</b>";
        const QString artist = "Artist <img src='https://example.invalid/private'>";
        const QString album = "Album & <i>version</i>";
        for (int deck = 0; deck < 2; ++deck) {
            ui_->endpoint.loadState(deck);
            ui_->endpoint.snapshotSongInfo(deck, title, artist, album, artwork);
            const auto prefix = QString("deck%1").arg(deck);
            auto* image = ui_->item((prefix + "Artwork").toUtf8().constData());
            QVERIFY(image);
            QTRY_COMPARE(image->property("status").toInt(), 1); // QML Image.Ready
            QCOMPARE(image->property("source").toUrl(), QUrl(artwork));
            QVERIFY(!ui_->item((prefix + "ArtworkPlaceholder").toUtf8().constData())->isVisible());
            for (const auto& entry : {std::pair{"Title", title}, {"Artist", artist}, {"Album", album}}) {
                auto* label = ui_->item((prefix + entry.first).toUtf8().constData());
                QVERIFY(label);
                QCOMPARE(label->property("text").toString(), entry.second);
                QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
                auto* accessible = QAccessible::queryAccessibleInterface(label);
                QVERIFY(accessible);
                QVERIFY(accessible->text(QAccessible::Name).contains(entry.second));
            }
        }
        auto* image = ui_->item("deck0Artwork");
        const auto sourceProperty = image->metaObject()->property(image->metaObject()->indexOfProperty("source"));
        QSignalSpy sourceChanges(image, sourceProperty.notifySignal());
        QVERIFY(sourceChanges.isValid());
        for (int index = 0; index < 8; ++index)
            ui_->endpoint.snapshotPosition(0, index / 2.0);
        QCoreApplication::processEvents();
        QCOMPARE(sourceChanges.size(), 0); // Poll snapshots do not reload an unchanged cover.
        const auto replacementArtwork = artworkDataUrl(QColor("#c49855"));
        QVERIFY(!replacementArtwork.isEmpty());
        ui_->endpoint.snapshotSongInfo(0, "New cover", "New artist", "New album", replacementArtwork);
        QTRY_COMPARE(image->property("status").toInt(), 1);
        QCOMPARE(image->property("source").toUrl(), QUrl(replacementArtwork));
        QCOMPARE(sourceChanges.size(), 1);
        ui_->endpoint.snapshotSongInfo(0, "Replacement", "New artist", "New album", {});
        QTRY_COMPARE(ui_->item("deck0Title")->property("text").toString(), QString("Replacement"));
        QTRY_VERIFY(image->property("source").toUrl().isEmpty());
        QVERIFY(ui_->item("deck0ArtworkPlaceholder")->isVisible());
        QCOMPARE(ui_->item("deck1Title")->property("text").toString(), title);
        QCOMPARE(ui_->item("deck1Artwork")->property("source").toUrl(), QUrl(artwork));
        QVERIFY(ui_->endpoint.calls.isEmpty());
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void deckLcdMissingArtAndUnloadedMetadataStayExplicit()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.snapshotSongInfo(0, {}, {}, {}, "https://example.invalid/cover.png");
        auto* title = ui_->item("deck0Title");
        auto* artwork = ui_->item("deck0Artwork");
        auto* placeholder = ui_->item("deck0ArtworkPlaceholder");
        QVERIFY(title && artwork && placeholder);
        QTRY_COMPARE(title->property("text").toString(), QString("Track 0"));
        QCOMPARE(ui_->item("deck0Artist")->property("text").toString(), QString("Artist unknown"));
        QCOMPARE(ui_->item("deck0Album")->property("text").toString(), QString("Album unknown"));
        QVERIFY(artwork->property("source").toUrl().isEmpty());
        QVERIFY(placeholder->isVisible());
        ui_->endpoint.snapshotSongInfo(0, "Old title", "Old artist", "Old album", {});
        ui_->endpoint.unloadState(0);
        QTRY_COMPARE(title->property("text").toString(), QString("Browse a local track"));
        QVERIFY(!ui_->item("deck0Artist")->property("text").toString().contains("Old artist"));
        QVERIFY(!ui_->item("deck0Album")->property("text").toString().contains("Old album"));
        QVERIFY(artwork->property("source").toUrl().isEmpty());
        QVERIFY(placeholder->isVisible());
        QVERIFY(ui_->endpoint.calls.isEmpty());
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void deckLcdMetadataFitsWithTransport_data()
    {
        QTest::addColumn<QSize>("viewport");
        QTest::addColumn<bool>("dark");
        for (const auto viewport : {QSize(1180, 760), QSize(1680, 1020)})
            for (const bool dark : {false, true})
                QTest::newRow(qPrintable(QString("%1x%2-%3").arg(viewport.width()).arg(viewport.height()).arg(dark ? "dark" : "light")))
                    << viewport << dark;
    }

    void deckLcdMetadataFitsWithTransport()
    {
        QFETCH(QSize, viewport);
        QFETCH(bool, dark);
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        QTRY_VERIFY(ui_->center(ui_->item("themeToggle")).x() < viewport.width());
        if (!dark)
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("themeToggle")));
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            const auto artwork = artworkDataUrl(QColor(deck == 0 ? "#38bba8" : "#bf8a56"));
            QVERIFY(!artwork.isEmpty());
            ui_->endpoint.snapshotSongInfo(deck, QString(240, 'T'), QString(240, 'A'), QString(240, 'B'), artwork);
            QTRY_COMPARE(ui_->item(QString("deck%1Artwork").arg(deck).toUtf8().constData())->property("status").toInt(), 1);
            const auto prefix = QString("deck%1").arg(deck);
            QStringList labels;
            for (const auto* suffix : {"ArtworkFrame", "Title", "Artist", "Album", "Elapsed", "Remaining"})
                labels.append(prefix + suffix);
            verifyControlsFit(labels);
            auto* display = ui_->item((prefix + "Display").toUtf8().constData());
            QVERIFY(display);
            const auto displayRect = [display] {
                return display->mapRectToScene(QRectF(0, 0, display->width(), display->height()));
            };
            for (const auto& name : labels) {
                auto* item = ui_->item(name.toUtf8().constData());
                QTRY_VERIFY2(displayRect().adjusted(-.5, -.5, .5, .5).contains(
                                 item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))), qPrintable(name));
                if (item->property("textFormat").isValid())
                    QVERIFY2(item->implicitHeight() <= item->height() + .5,
                             qPrintable(QString("%1 text height %2 exceeds %3").arg(name).arg(item->implicitHeight()).arg(item->height())));
            }
            auto* seek = ui_->item((prefix + "Seek").toUtf8().constData());
            QTRY_VERIFY(displayRect().adjusted(-.5, -.5, .5, .5).contains(
                            seek->mapRectToScene(QRectF(0, 0, seek->width(), seek->height()))));
            seek->forceActiveFocus();
            QTRY_VERIFY(seek->hasActiveFocus());
            const auto count = ui_->endpoint.calls.size();
            QTest::keyClick(ui_->window, Qt::Key_Right);
            QTRY_COMPARE(ui_->endpoint.calls.size(), count + 1);
            QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("seek"));
            QCOMPARE(ui_->endpoint.calls.last().toMap()["deck"].toInt(), deck);
        }
        verifyControlsFit({"deck0Cue", "deck0Play", "deck0Seek", "deck0Gain", "deck1Cue", "deck1Play", "deck1Seek", "deck1Gain"});
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
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

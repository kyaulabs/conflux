// SPDX-License-Identifier: AGPL-3.0-or-later
#include <QGuiApplication>
#include <QAccessible>
#include <QFontDatabase>
#include <QBuffer>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QMetaProperty>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QPointingDevice>
#include <QPointer>
#include <QStyleHints>
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
    Q_PROPERTY(bool residentHoldAvailable READ residentHoldAvailable NOTIFY stateChanged)
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
    static QVariantMap scratchState(bool supported = false)
    {
        return {{"supported", supported}, {"active", false}, {"pending", false},
                {"errorCode", 0}, {"generation", qulonglong(0)}};
    }
    static QVariantMap bpmState(double reference = 0)
    {
        return {{"estimated", reference}, {"reference", reference}, {"manual", false},
                {"status", reference > 0 ? "Estimated" : "Unavailable"}, {"error", ""}};
    }
    static QVariantMap savedCueState(bool catalogued = false, bool saved = false,
                                     const QString& frame = {}, double seconds = 0)
    {
        const QString add = "Add this source to the library before saving or recalling a cue.";
        return {{"saveAvailable", catalogued}, {"recallAvailable", catalogued && saved},
                {"saveReason", catalogued ? QString{} : add},
                {"recallReason", !catalogued ? add : saved ? QString{} : QString("No saved cue for this source.")},
                {"saved", saved}, {"frame", frame}, {"seconds", seconds}};
    }
    UiEndpoint()
    {
        for (int deck = 0; deck < 2; ++deck)
            decks_.append(QVariantMap{{"loaded", false}, {"name", ""}, {"sourceId", ""}, {"playing", false}, {"replacing", false},
                                      {"gain", .5}, {"eqGainDb", QVariantList{0., 0., 0.}},
                                      {"positionSeconds", 0.0}, {"durationSeconds", 0.0},
                                      {"cueSeconds", 0.0}, {"cueHeld", false}, {"cuePreview", false},
                                      {"cueSelected", false}, {"meter", silentMeter()}, {"loop", loopState()}, {"tempo", tempoState()}, {"bpm", bpmState()},
                                      {"scratch", scratchState()}, {"savedCue", savedCueState()}, {"sourceGeneration", qulonglong(0)},
                                      {"waveform", QVariantList{}}, {"loading", false}, {"pendingName", ""}, {"pendingPath", ""},
                                      {"pendingPresentation", QVariantMap{}}, {"pendingWaveform", QVariantList{}},
                                      {"loadPhase", ""}, {"loadPhaseText", ""}, {"loadCompletedStages", 0},
                                      {"loadTotalStages", 0}, {"loadProgress", 0.0}});
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
    bool residentHoldAvailable() const { return residentHoldAvailable_; }
    void setResidentHoldAvailable(bool value) { residentHoldAvailable_ = value; emit stateChanged(); }
    void setOverviewStem(int deck, const QString& stem) {
        auto state = decks_[deck].toMap();
        state["overviewStem"] = stem;
        state["liveStems"] = stem == "source";
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
    void snapshotMasterGain(double value) { master_ = value; emit stateChanged(); }
    void snapshotCueGain(double value) { cue_ = value; emit stateChanged(); }
    void setNumericAcknowledgement(bool value) { acknowledgeNumeric_ = value; }
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
    void setScratchAccepted(bool begin, bool move = true)
    { acceptScratchBegin_ = begin; acceptScratchMove_ = move; }
    void setScratchInvalidateOnBegin(const QString& reason) { invalidateScratchBegin_ = reason; }
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
                                   {"playing", false}, {"gain", .5}, {"eqGainDb", QVariantList{0., 0., 0.}},
                                   {"positionSeconds", position}, {"durationSeconds", 10.0},
                                   {"cueSeconds", 0.0}, {"cueHeld", false}, {"cuePreview", false},
                                   {"cueSelected", false}, {"meter", silentMeter()}, {"loop", loopState(true)}, {"tempo", tempoState(true)}, {"bpm", bpmState(120)},
                                   {"scratch", scratchState(true)}, {"savedCue", savedCueState()}, {"sourceGeneration", qulonglong(1)},
                                   {"waveform", QVariantList{.15, .5, .9, .3, .1}}, {"loading", false},
                                   {"pendingName", ""}, {"pendingPath", ""}, {"pendingPresentation", QVariantMap{}},
                                   {"pendingWaveform", QVariantList{}}, {"loadPhase", ""}, {"loadPhaseText", ""},
                                   {"loadCompletedStages", 0}, {"loadTotalStages", 0}, {"loadProgress", 0.0}};
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
    Q_INVOKABLE void loadLibraryStemTrackWithRecoveryRange(int deck, const QString& id,
                                                         double inSeconds, double outSeconds)
    { calls.append(QVariantMap{{"action", "libraryStemRangeLoad"}, {"deck", deck}, {"id", id},
                               {"inSeconds", inSeconds}, {"outSeconds", outSeconds}}); }
    Q_INVOKABLE bool authorizeResidentHold(int deck, const QString& id,
                                          const QString& outputSession, const QString& rangeId,
                                          const QString& generation, const QString& revision)
    { return residentCall("residentArm", deck, id, outputSession, rangeId, generation, revision); }
    Q_INVOKABLE bool cancelResidentHold(int deck, const QString& id,
                                       const QString& outputSession, const QString& rangeId,
                                       const QString& generation, const QString& revision)
    { return residentCall("residentCancel", deck, id, outputSession, rangeId, generation, revision); }
    Q_INVOKABLE bool exitResidentHold(int deck, const QString& id,
                                     const QString& outputSession, const QString& rangeId,
                                     const QString& generation, const QString& revision)
    { return residentCall("residentExit", deck, id, outputSession, rangeId, generation, revision); }
    Q_INVOKABLE void importTrack(const QUrl& url)
    { calls.append(QVariantMap{{"action", "import"}, {"url", url}}); }
    Q_INVOKABLE void importCueSheet(const QString& sourceId, const QUrl& url)
    { calls.append(QVariantMap{{"action", "cueImport"}, {"id", sourceId}, {"url", url}}); }
    Q_INVOKABLE bool jumpToImportedCue(int deck, const QString& sourceId, int cueIndex)
    {
        calls.append(QVariantMap{{"action", "cueJump"}, {"deck", deck}, {"id", sourceId}, {"index", cueIndex}});
        return true;
    }
    // Routing only: tests deliver saved metadata snapshots independently.
    Q_INVOKABLE bool saveDeckCue(int deck)
    { calls.append(QVariantMap{{"action", "saveDeckCue"}, {"deck", deck}}); return true; }
    Q_INVOKABLE bool recallDeckCue(int deck)
    { calls.append(QVariantMap{{"action", "recallDeckCue"}, {"deck", deck}}); return true; }
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
        if (acknowledgeNumeric_) snapshotPosition(deck, seconds);
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
    // This endpoint records routing only. It neither produces scratch PCM nor
    // changes acknowledged position/play state; native/controller tests own that.
    Q_INVOKABLE QString beginScratch(int deck)
    {
        if (deck < 0 || deck > 1) return {};
        const auto token = acceptScratchBegin_
            ? QString::number(++scratchSerial_, 16).rightJustified(16, '0') : QString{};
        calls.append(QVariantMap{{"action", "scratchBegin"}, {"deck", deck}, {"token", token}});
        if (token.isEmpty()) return {};
        scratchTokens_[deck] = token;
        // Simulate a synchronous source/enable notification before Begin returns.
        if (invalidateScratchBegin_ == "generation")
            snapshotDeck(deck, {{"sourceGeneration",
                decks_[deck].toMap()["sourceGeneration"].toULongLong() + 1}});
        else if (invalidateScratchBegin_ == "replacing")
            setReplacing(deck, true);
        return token;
    }
    Q_INVOKABLE bool moveScratch(int deck, const QString& token, qint64 cumulativeFrames)
    {
        calls.append(QVariantMap{{"action", "scratchMove"}, {"deck", deck}, {"token", token},
                                 {"frames", cumulativeFrames}});
        return deck >= 0 && deck < 2 && token == scratchTokens_[deck] && acceptScratchMove_;
    }
    Q_INVOKABLE bool endScratch(int deck, const QString& token)
    {
        calls.append(QVariantMap{{"action", "scratchEnd"}, {"deck", deck}, {"token", token}});
        if (deck < 0 || deck > 1 || token != scratchTokens_[deck]) return false;
        scratchTokens_[deck].clear();
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
        if (acknowledgeNumeric_) snapshotStemGain(deck, stem, gain);
    }
    void snapshotEqGain(int deck, int band, double db)
    {
        auto state = decks_[deck].toMap();
        auto gains = state["eqGainDb"].toList();
        gains[band] = db;
        state["eqGainDb"] = gains;
        decks_[deck] = state;
        emit stateChanged();
    }
    Q_INVOKABLE void setEqGainDb(int deck, int band, double db)
    {
        calls.append(QVariantMap{{"action", "eq"}, {"deck", deck}, {"band", band}, {"value", db}});
        if (acknowledgeNumeric_) snapshotEqGain(deck, band, db);
    }
    Q_INVOKABLE void setDeckGain(int deck, double gain)
    {
        calls.append(QVariantMap{{"action", "gain"}, {"deck", deck}, {"value", gain}});
        if (!acknowledgeNumeric_) return;
        auto state = decks_[deck].toMap();
        state["gain"] = gain;
        decks_[deck] = state;
        emit stateChanged();
    }
    Q_INVOKABLE void setMasterGain(double gain)
    {
        calls.append(QVariantMap{{"action", "master"}, {"value", gain}});
        if (!acknowledgeNumeric_) return;
        master_ = gain;
        emit stateChanged();
    }
    Q_INVOKABLE void setCueGain(double gain)
    {
        calls.append(QVariantMap{{"action", "cueGain"}, {"value", gain}});
        if (!acknowledgeNumeric_) return;
        cue_ = gain;
        emit stateChanged();
    }
    Q_INVOKABLE void setCrossfader(double position)
    {
        calls.append(QVariantMap{{"action", "crossfader"}, {"value", position}});
        if (acknowledgeNumeric_) snapshotCrossfader(position);
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
    Q_INVOKABLE bool hotCue(int deck, int slot)
    { calls.append(QVariantMap{{"action", "hotCue"}, {"deck", deck}, {"slot", slot}}); return true; }
    Q_INVOKABLE bool clearHotCue(int deck, int slot)
    { calls.append(QVariantMap{{"action", "clearHotCue"}, {"deck", deck}, {"slot", slot}}); return true; }
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
    bool residentCall(const QString& action, int deck, const QString& id,
                      const QString& outputSession, const QString& rangeId,
                      const QString& generation, const QString& revision)
    {
        calls.append(QVariantMap{{"action", action}, {"deck", deck}, {"id", id},
                                 {"outputSession", outputSession}, {"rangeId", rangeId},
                                 {"generation", generation}, {"revision", revision}});
        return true;
    }
    QVariantList decks_;
    QVariantList library_;
    QVariantMap meters_, routing_, recording_, preparation_;
    bool busy_ = false, ready_ = false, stemsAvailable_ = false;
    bool residentHoldAvailable_ = false;
    bool libraryReady_ = true;
    bool acceptCue_ = true, acceptJog_ = true;
    bool acknowledgeNumeric_ = true;
    bool acceptScratchBegin_ = true, acceptScratchMove_ = true;
    quint64 scratchSerial_ = 0;
    std::array<QString, 2> scratchTokens_{};
    QString invalidateScratchBegin_;
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
    bool useNonNativeFileDialog(const char* name = "libraryFileDialog") const
    {
        auto* dialog = object(name);
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
        if (touchActive_ && ui_ && ui_->window) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
            QTest::touchEvent(ui_->window, touchDevice_.get(), false).cancel();
#else
            // Retire the known contact through Qt's input path before the
            // window is destroyed; Qt6.4 has no public platform-cancel helper.
            QTest::touchEvent(ui_->window, touchDevice_.get(), false)
                .release(0, QPoint(0, 0), ui_->window).commit();
#endif
            touchActive_ = false;
        }
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

    void keyboardTraversalReachesEnabledControlsWithoutCommands_data()
    {
        QTest::addColumn<int>("fontPixels");
        QTest::newRow("minimum-15px") << 15;
        QTest::newRow("minimum-19px") << 19;
    }

    void keyboardTraversalReachesEnabledControlsWithoutCommands()
    {
        QFETCH(int, fontPixels);
        const auto originalFont = QGuiApplication::font();
        const auto restoreFont = qScopeGuard([this, originalFont] {
            ui_.reset();
            QGuiApplication::setFont(originalFont);
        });
        ui_.reset();
        auto platformFont = originalFont;
        platformFont.setPixelSize(fontPixels);
        QGuiApplication::setFont(platformFont);
        ui_ = std::make_unique<UiFixture>();
        ui_->load();
        QVERIFY2(ui_->window, qPrintable(ui_->warnings.join('\n')));
        QVERIFY(QTest::qWaitForWindowExposed(ui_->window));
        ui_->window->resize(1180, 760);
        QTRY_COMPARE(ui_->window->size(), QSize(1180, 760));
        ui_->endpoint.loadState(0); ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        ui_->window->contentItem()->polish(); ui_->window->update();
        QTRY_VERIFY(polished.count() > 0);

        QStringList candidates{"libraryOpen", "audioSettingsOpen", "themeToggle",
                               "masterGain", "cueGain", "crossfader"};
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            for (const auto* suffix : {"Browse", "BpmReference", "Cue", "Play", "Seek", "Jog",
                                      "Tempo", "MasterTempo", "TempoReset", "LoopIn", "LoopOut",
                                      "LoopToggle", "Gain", "CueSelect", "EqHI", "EqMID", "EqLOW"})
                candidates.append(prefix + QString::fromLatin1(suffix));
        }
        for (const auto& name : candidates)
            QVERIFY2(ui_->item(name.toUtf8().constData()), qPrintable(name));
        const auto ownsFocus = [](QQuickItem* control, QQuickItem* focus) {
            for (auto* item = focus; item; item = item->parentItem())
                if (item == control) return true;
            return false;
        };
        QString failure;
        const auto walk = [&](QQuickItem* start, bool backwards) {
            // Item order is deliberately unspecified. Require a bounded cycle,
            // reachable enabled controls, and no hidden/disabled focus trap.
            QList<QQuickItem*> visited;
            QStringList reached;
            for (int step = 0; step < 128; ++step) {
                auto* current = ui_->window->activeFocusItem();
                if (!current || !current->isVisible() || !current->isEnabled()) {
                    failure = "Keyboard focus left visible enabled content";
                    return false;
                }
                if (visited.contains(current)) {
                    if (current != start) {
                        failure = "Keyboard traversal repeated before returning to its start";
                        return false;
                    }
                    for (const auto& name : candidates) {
                        auto* control = ui_->item(name.toUtf8().constData());
                        if (control && control->isVisible() && control->isEnabled() && !reached.contains(name)) {
                            failure = "Keyboard traversal missed " + name;
                            return false;
                        }
                    }
                    return true;
                }
                visited.append(current);
                for (const auto& name : candidates) {
                    auto* control = ui_->item(name.toUtf8().constData());
                    if (control && ownsFocus(control, current)) reached.append(name);
                }
                QTest::keyClick(ui_->window, backwards ? Qt::Key_Backtab : Qt::Key_Tab);
                if (!QTest::qWaitFor([&] { return ui_->window->activeFocusItem() != current; }, 500)) {
                    failure = "Tab/Backtab did not advance from " + current->objectName();
                    return false;
                }
            }
            failure = "Keyboard traversal exceeded its128-step bound";
            return false;
        };
        for (int phase = 0; phase < 3; ++phase) {
            if (phase == 1) {
                auto tempo = UiEndpoint::tempoState(true);
                tempo["active"] = true; tempo["percent"] = 1.0;
                ui_->endpoint.snapshotTempo(0, tempo); ui_->endpoint.snapshotTempo(1, tempo);
                QTRY_VERIFY(ui_->item("deck0TempoReset")->isEnabled());
                QTRY_VERIFY(ui_->item("deck1TempoReset")->isEnabled());
            } else if (phase == 2) {
                ui_->endpoint.setReplacing(0, true);
                for (const auto* name : {"deck0Play", "deck0Cue", "deck0Seek", "deck0Jog", "deck0Tempo", "deck0Gain"})
                    QTRY_VERIFY(!ui_->item(name)->isEnabled());
                QVERIFY(ui_->item("deck1Play")->isEnabled());
                QVERIFY(ui_->item("crossfader")->isEnabled());
            }
            auto* start = ui_->item(phase == 2 ? "deck1Play" : "deck0Play");
            start->forceActiveFocus();
            QTRY_VERIFY(start->hasActiveFocus());
            QVERIFY2(walk(start, false), qPrintable(failure));
            QVERIFY2(walk(start, true), qPrintable(failure));
            QVERIFY(ui_->endpoint.calls.isEmpty());
        }
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
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

    void keyboardShortcutsAndModalFocusReturnWithoutAudioCommands()
    {
        ui_->endpoint.loadState(0); ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto decks = ui_->endpoint.decks();
        const auto ownsFocus = [this](QQuickItem* control) {
            for (auto* item = ui_->window->activeFocusItem(); item; item = item->parentItem())
                if (item == control) return true;
            return false;
        };
        const auto content = [](QObject* popup) {
            return popup->property("contentItem").value<QQuickItem*>();
        };
        auto* library = ui_->object("libraryDrawer");
        auto* libraryOpen = ui_->item("libraryOpen");
        QVERIFY(library && libraryOpen);
        libraryOpen->forceActiveFocus();
        QTRY_VERIFY(libraryOpen->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_O, Qt::ControlModifier);
        QTRY_VERIFY(library->property("opened").toBool());
        QTRY_VERIFY(ownsFocus(ui_->item("deck0Path")));
        QVERIFY(content(library));
        QTest::keyClick(ui_->window, Qt::Key_Tab);
        QTRY_VERIFY(ownsFocus(content(library)));
        QTest::keyClick(ui_->window, Qt::Key_Backtab);
        QTRY_VERIFY(ownsFocus(content(library)));
        QTest::keyClick(ui_->window, Qt::Key_Escape);
        QTRY_VERIFY(!library->property("visible").toBool());
        QTRY_VERIFY(libraryOpen->hasActiveFocus());
        QVERIFY(ui_->endpoint.calls.isEmpty());

        auto* reference = ui_->item("deck0BpmReference");
        auto* editor = ui_->object("deck0BpmEditor");
        QVERIFY(reference && editor);
        reference->forceActiveFocus();
        QTRY_VERIFY(reference->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_VERIFY(editor->property("opened").toBool());
        QTRY_VERIFY(ownsFocus(ui_->item("deck0BpmInput")));
        QVERIFY(content(editor));
        QTest::keyClick(ui_->window, Qt::Key_Tab);
        QTRY_VERIFY(ownsFocus(content(editor)));
        QTest::keyClick(ui_->window, Qt::Key_Backtab);
        QTRY_VERIFY(ownsFocus(content(editor)));
        QTest::keyClick(ui_->window, Qt::Key_Escape);
        QTRY_VERIFY(!editor->property("visible").toBool());
        QTRY_VERIFY(reference->hasActiveFocus());
        QVERIFY(ui_->endpoint.calls.isEmpty());

        const auto focusDescription = [this] {
            QStringList chain;
            for (auto* item = ui_->window->activeFocusItem(); item; item = item->parentItem())
                chain.append(item->objectName() + " (" + QString::fromLatin1(item->metaObject()->className()) + ")");
            return chain.join(" <- ");
        };
        auto* audio = ui_->object("audioSettingsDrawer");
        auto* audioOpen = ui_->item("audioSettingsOpen");
        QVERIFY(audio && audioOpen);
        audioOpen->forceActiveFocus();
        QTRY_VERIFY(audioOpen->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_VERIFY(audio->property("opened").toBool());
        QVERIFY(content(audio));
        // Native Popup may initially focus its container rather than a child.
        QTRY_VERIFY2(audio->property("activeFocus").toBool(),
                     qPrintable(QStringLiteral("Audio drawer focus remains at ") + focusDescription()));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls.first().toMap()["action"].toString(), QString("refreshRouting"));
        QTest::keyClick(ui_->window, Qt::Key_Tab);
        QTRY_VERIFY(ownsFocus(content(audio)));
        QTest::keyClick(ui_->window, Qt::Key_Backtab);
        QTRY_VERIFY2(audio->property("activeFocus").toBool(), qPrintable(focusDescription()));
        QTest::keyClick(ui_->window, Qt::Key_Tab);
        QTRY_VERIFY(ownsFocus(content(audio)));
        QTest::keyClick(ui_->window, Qt::Key_Escape);
        QTRY_VERIFY(!audio->property("visible").toBool());
        QTRY_VERIFY(audioOpen->hasActiveFocus());

        auto* theme = ui_->item("themeToggle");
        QVERIFY(theme);
        const auto originalText = theme->property("text").toString();
        QTest::keyClick(ui_->window, Qt::Key_F6);
        QTRY_VERIFY(theme->property("text").toString() != originalText);
        QTest::keyClick(ui_->window, Qt::Key_F6);
        QTRY_COMPARE(theme->property("text").toString(), originalText);
        QCOMPARE(ui_->endpoint.calls.size(), 1); // Only opening Audio requested discovery.
        QCOMPARE(ui_->endpoint.decks(), decks);
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
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

    void deckPendingPresentationIsIsolatedAndRestoresActiveSource_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<QString>("outcome");
        for (int deck = 0; deck < 2; ++deck)
            for (const auto* outcome : {"Cancelled", "Failed"})
                QTest::newRow(qPrintable(QString("deck%1-%2").arg(deck).arg(outcome))) << deck << QString::fromLatin1(outcome);
    }

    void deckPendingPresentationIsIsolatedAndRestoresActiveSource()
    {
        QFETCH(int, deck);
        QFETCH(QString, outcome);
        for (int current = 0; current < 2; ++current) {
            ui_->endpoint.loadState(current);
            ui_->endpoint.setSourceId(current, QString(64, QChar('a' + current)));
            ui_->endpoint.snapshotSongInfo(current, QString("Active title %1").arg(current),
                                          "Active artist", "Active album", artworkDataUrl(QColor("#528ba1")));
        }
        ui_->endpoint.setReady(true);
        const auto accepted = ui_->endpoint.decks();
        const auto peer = accepted[1 - deck];
        const auto prefix = QString("deck%1").arg(deck);
        auto* title = ui_->item((prefix + "Title").toUtf8().constData());
        auto* artist = ui_->item((prefix + "Artist").toUtf8().constData());
        auto* image = ui_->item((prefix + "Artwork").toUtf8().constData());
        auto* seek = ui_->item((prefix + "Seek").toUtf8().constData());
        auto* reference = ui_->item((prefix + "BpmReference").toUtf8().constData());
        auto* feedback = ui_->item((prefix + "LoadFeedback").toUtf8().constData());
        auto* progress = ui_->item((prefix + "LoadProgress").toUtf8().constData());
        auto* status = ui_->item((prefix + "LoadStatus").toUtf8().constData());
        auto* display = ui_->item((prefix + "Display").toUtf8().constData());
        QVERIFY(title && artist && image && seek && reference && feedback && progress && status && display);
        auto* surface = display->parentItem();
        const auto oldArtwork = image->property("source").toUrl();
        QQmlExpression waveform(qmlContext(surface), surface, "JSON.stringify(cachedWaveform)");
        const auto oldWaveform = waveform.evaluate().toString();
        QVERIFY(!waveform.hasError());
        const QString pending = QString::fromUtf8("Incoming 夜 #%.wav");
        ui_->endpoint.setPreparation("Preparing", deck, true, false, pending);
        ui_->endpoint.snapshotDeck(deck, {{"loading", true}, {"pendingName", pending}, {"pendingPath", "/tmp/incoming.wav"},
                                           {"loadPhase", "decode-cache"}, {"loadPhaseText", "Preparing source"},
                                           {"loadCompletedStages", 0}, {"loadTotalStages", 4}, {"loadProgress", 0.0}});
        QTRY_COMPARE(title->property("text").toString(), pending);
        QTRY_VERIFY(feedback->isVisible());
        QTRY_VERIFY(!seek->isVisible()); // New overview must never seek the still-active source.
        QVERIFY(!reference->isEnabled());
        QCOMPARE(progress->width(), 0.0);
        QVERIFY(status->property("text").toString().contains("0/4 STAGES"));
        QVERIFY(!status->property("text").toString().contains('%'));
        QCOMPARE(ui_->endpoint.decks()[deck].toMap()["sourceId"], accepted[deck].toMap()["sourceId"]);
        QCOMPARE(ui_->endpoint.decks()[1 - deck], peer);
        for (int current = 0; current < 2; ++current)
            QVERIFY(ui_->item(QString("deck%1Play").arg(current).toUtf8().constData())->isEnabled());
        const QVariantList peaks{.05, .8, .4, .6};
        const auto pendingArtwork = artworkDataUrl(QColor("#ba7938"));
        ui_->endpoint.snapshotDeck(deck, {{"pendingPresentation", QVariantMap{{"title", "Incoming title"}, {"artist", "Incoming artist"},
                                                                             {"album", "Incoming album"}, {"artworkDataUrl", pendingArtwork}}},
                                           {"pendingWaveform", peaks}, {"loadPhase", "bpm"}, {"loadPhaseText", "Analyzing BPM"},
                                           {"loadCompletedStages", 1}, {"loadProgress", .25}});
        QTRY_COMPARE(title->property("text").toString(), QString("Incoming title"));
        QCOMPARE(artist->property("text").toString(), QString("Incoming artist"));
        QCOMPARE(image->property("source").toUrl(), QUrl(pendingArtwork));
        QTRY_COMPARE(waveform.evaluate().toString(), QString("[0.05,0.8,0.4,0.6]"));
        QVERIFY(!waveform.hasError());
        QTRY_VERIFY(progress->width() > 0 && progress->width() < progress->parentItem()->width());
        const auto decodedWidth = progress->width();
        QVERIFY(status->property("text").toString().contains("1/4 STAGES"));
        auto* accessible = QAccessible::queryAccessibleInterface(feedback);
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::ProgressBar);
        QVERIFY(accessible->text(QAccessible::Description).contains("Completed stages: 1 of 4"));
        ui_->endpoint.snapshotDeck(deck, {{"loadPhase", "prime"}, {"loadPhaseText", "Priming audio"},
                                           {"loadCompletedStages", 2}, {"loadProgress", .5}});
        QTRY_VERIFY(progress->width() > decodedWidth && progress->width() < progress->parentItem()->width());
        QVERIFY(status->property("text").toString().contains("2/4 STAGES"));
        QVERIFY(!status->property("text").toString().contains('%'));
        QCOMPARE(ui_->endpoint.decks()[1 - deck], peer);
        QVERIFY(ui_->endpoint.calls.isEmpty());
        if (outcome == "Cancelled") {
            ui_->endpoint.snapshotDeck(deck, {{"loadPhase", "cancel"}, {"loadPhaseText", "Cancelling preparation"}});
            QTRY_COMPARE(status->property("text").toString(), QString("CANCELLING…"));
        }
        ui_->endpoint.snapshotDeck(deck, {{"loading", false}, {"pendingName", ""}, {"pendingPath", ""},
                                           {"pendingPresentation", QVariantMap{}}, {"pendingWaveform", QVariantList{}},
                                           {"loadPhase", ""}, {"loadPhaseText", ""}, {"loadCompletedStages", 0},
                                           {"loadTotalStages", 0}, {"loadProgress", 0.0}});
        ui_->endpoint.setPreparation(outcome, deck);
        QTRY_VERIFY(!feedback->isVisible());
        QTRY_COMPARE(title->property("text").toString(), QString("Active title %1").arg(deck));
        QCOMPARE(artist->property("text").toString(), QString("Active artist"));
        QCOMPARE(image->property("source").toUrl(), oldArtwork);
        QTRY_COMPARE(waveform.evaluate().toString(), oldWaveform);
        QVERIFY(seek->isVisible() && seek->isEnabled());
        QVERIFY(reference->isEnabled());
        QCOMPARE(ui_->endpoint.decks(), accepted);
        QVERIFY(ui_->endpoint.calls.isEmpty());
    }

    void acceptedWaveformRevisionSkipsPayloadAndPreservesInvalidation()
    {
        QQmlComponent component(&ui_->engine,
            QUrl::fromLocalFile(QStringLiteral(CONFLUX_SOURCE_DIR "/tests/waveform_cache_fixture.qml")));
        std::unique_ptr<QObject> fixture(component.create());
        QVERIFY2(fixture, qPrintable(component.errorString()));
        QQmlExpression check(qmlContext(fixture.get()), fixture.get(), QStringLiteral("run()"));
        const auto passed = check.evaluate();
        QVERIFY2(!check.hasError(), qPrintable(check.error().toString()));
        QVERIFY(passed.toBool());
    }

    void deckFocusUsesItsOwnColorWithoutSubmittingCommands()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        std::array<QColor, 2> colors;
        for (int deck = 0; deck < 2; ++deck) {
            auto* display = ui_->item(QString("deck%1Display").arg(deck).toUtf8().constData());
            QVERIFY(display);
            colors[deck] = display->parentItem()->property("deckColor").value<QColor>();
            QVERIFY(colors[deck].isValid());
            for (const auto* suffix : {"Play", "Seek"}) {
                auto* control = ui_->item(QString("deck%1%2").arg(deck).arg(suffix).toUtf8().constData());
                control->forceActiveFocus();
                QTRY_VERIFY(control->hasActiveFocus());
                QQmlExpression border(qmlContext(control), control, "background.border.color");
                QTRY_COMPARE(border.evaluate().value<QColor>(), colors[deck]);
                QVERIFY(!border.hasError());
            }
        }
        QVERIFY(colors[0] != colors[1]);
        QVERIFY(ui_->endpoint.calls.isEmpty());
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
                           "deck1Path", "deck1FileBrowse", "deck1Import", "deck1Load", "libraryClose",
                           "libraryCuePath", "libraryCueBrowse", "libraryCueImport"});
        auto* cueBrowse = ui_->item("libraryCueBrowse");
        QVERIFY(cueBrowse->width() >= 44 && cueBrowse->height() >= 44);
        auto* cueAccessible = QAccessible::queryAccessibleInterface(cueBrowse);
        QVERIFY(cueAccessible);
        QCOMPARE(cueAccessible->role(), QAccessible::Button);
        QVERIFY(cueAccessible->text(QAccessible::Description).contains("without importing"));
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

    void cueSheetBrowseBeforeSelectingSource_data()
    {
        QTest::addColumn<bool>("keyboard");
        QTest::newRow("pointer") << false;
        QTest::newRow("keyboard") << true;
    }

    void cueSheetBrowseBeforeSelectingSource()
    {
        QFETCH(bool, keyboard);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto local = directory.filePath(QString::fromUtf8("Club 夜 #%.cue"));
        QFile file(local);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString id(64, QLatin1Char('c'));
        ui_->endpoint.setLibrary({QVariantMap{{"id", id}, {"name", "Club XL"},
            {"path", "/tmp/club.flac"}, {"available", true}}});
        ui_->endpoint.loadState(0); ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto decksBefore = ui_->endpoint.decks();
        QVERIFY(ui_->openLibrary(0));
        auto* drawer = ui_->object("libraryDrawer");
        auto* browse = ui_->item("libraryCueBrowse");
        auto* path = ui_->item("libraryCuePath");
        auto* importButton = ui_->item("libraryCueImport");
        auto* status = ui_->item("libraryCueStatus");
        QVERIFY(drawer && browse && path && importButton && status);
        QCOMPARE(drawer->property("selectedId").toString(), QString{});
        QVERIFY2(browse->isEnabled(), "Choosing a sheet must not require selecting its import target first");
        QVERIFY(!importButton->isEnabled());
        QVERIFY(importButton->opacity() < .5);
        QVERIFY(status->property("text").toString().contains("select", Qt::CaseInsensitive));
        QVERIFY(path->setProperty("text", "/tmp/previous.cue"));
        QVERIFY(ui_->useNonNativeFileDialog("libraryCueFileDialog"));
        auto* dialog = ui_->object("libraryCueFileDialog");
        QVERIFY(dialog->setProperty("currentFolder", QUrl::fromLocalFile(directory.path())));
        auto openChooser = [&] {
            QTRY_VERIFY(browse->isEnabled());
            if (keyboard) {
                browse->forceActiveFocus();
                QTRY_VERIFY(browse->hasActiveFocus());
                QTest::keyClick(ui_->window, Qt::Key_Space);
            } else {
                QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(browse));
            }
        };
        openChooser();
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(ui_->fileDialogItem("fileDialogDelegate0"));
        auto* cancelledChoice = ui_->fileDialogItem("fileDialogDelegate0");
        QTest::mouseClick(cancelledChoice->window(), Qt::LeftButton, Qt::NoModifier, ui_->center(cancelledChoice));
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject", Qt::DirectConnection));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        // A late native callback after cancellation is not a fresh unbound pick.
        QVERIFY(dialog->setProperty("selectedFile", QUrl::fromLocalFile(local)));
        QCOMPARE(dialog->property("selectedFile").toUrl(), QUrl::fromLocalFile(local));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accepted", Qt::DirectConnection));
        QCOMPARE(path->property("text").toString(), QString("/tmp/previous.cue"));
        QVERIFY(ui_->endpoint.calls.isEmpty());
        ui_->window->requestActivate();
        QTRY_VERIFY(ui_->window->isActive());
        openChooser();
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(ui_->fileDialogItem("fileDialogDelegate0"));
        auto* choice = ui_->fileDialogItem("fileDialogDelegate0");
        QTRY_COMPARE(choice->property("file").toUrl(), QUrl::fromLocalFile(local));
        QTest::mouseClick(choice->window(), Qt::LeftButton, Qt::NoModifier, ui_->center(choice));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_COMPARE(QUrl(path->property("text").toString()).toLocalFile(), local);
        QCOMPARE(drawer->property("selectedId").toString(), QString{});
        QVERIFY(!importButton->isEnabled());
        QVERIFY(ui_->endpoint.calls.isEmpty());
        QCOMPARE(ui_->endpoint.decks(), decksBefore);
        ui_->window->requestActivate();
        QTRY_VERIFY(ui_->window->isActive());
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        QTRY_VERIFY(importButton->isEnabled());
        QCOMPARE(importButton->opacity(), 1.0);
        QVERIFY(status->property("text").toString().contains("Club XL"));
        QCOMPARE(QUrl(path->property("text").toString()).toLocalFile(), local);
        if (keyboard) {
            importButton->forceActiveFocus();
            QTest::keyClick(ui_->window, Qt::Key_Space);
        } else {
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(importButton));
        }
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.first().toMap();
        QCOMPARE(call["action"].toString(), QString("cueImport"));
        QCOMPARE(call["id"].toString(), id);
        QCOMPARE(call["url"].toUrl().toLocalFile(), local);
        QCOMPARE(ui_->endpoint.decks(), decksBefore);
    }

    void cueSheetBrowseChoosesOnlyPathForSelectedSource_data()
    {
        QTest::addColumn<bool>("keyboard");
        QTest::newRow("pointer") << false;
        QTest::newRow("keyboard") << true;
    }

    void cueSheetBrowseChoosesOnlyPathForSelectedSource()
    {
        QFETCH(bool, keyboard);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto local = directory.filePath(QString::fromUtf8("Música 夜 #part%23%.cue"));
        QFile file(local);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString id(64, QLatin1Char('c')), otherId(64, QLatin1Char('d'));
        ui_->endpoint.setLibrary({
            QVariantMap{{"id", id}, {"name", "Selected source"}, {"path", "/tmp/selected.wav"}, {"available", true}},
            QVariantMap{{"id", otherId}, {"name", "Other source"}, {"path", "/tmp/other.wav"}, {"available", true}}});
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto decksBefore = ui_->endpoint.decks();
        QVERIFY(ui_->openLibrary(0));
        auto* browse = ui_->item("libraryCueBrowse");
        auto* path = ui_->item("libraryCuePath");
        auto* importButton = ui_->item("libraryCueImport");
        QVERIFY(browse && path && importButton);
        QVERIFY(browse->isEnabled());
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        QTRY_VERIFY(browse->isEnabled());
        QVERIFY(ui_->useNonNativeFileDialog("libraryCueFileDialog"));
        auto* dialog = ui_->object("libraryCueFileDialog");
        QVERIFY(dialog);
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
        QTRY_VERIFY(ui_->fileDialogItem("fileDialogDelegate0"));
        auto* choice = ui_->fileDialogItem("fileDialogDelegate0");
        QTRY_COMPARE(choice->property("file").toUrl(), QUrl::fromLocalFile(local));
        QTest::mouseClick(choice->window(), Qt::LeftButton, Qt::NoModifier, ui_->center(choice));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QCOMPARE(accepted.size(), 1);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        ui_->window->requestActivate();
        QTRY_VERIFY(ui_->window->isActive());
        QTRY_COMPARE(QUrl(path->property("text").toString()).toLocalFile(), local);
        QCOMPARE(ui_->object("libraryDrawer")->property("selectedId").toString(), id);
        for (const auto* name : {"deck0Path", "deck1Path"})
            QCOMPARE(ui_->item(name)->property("text").toString(), QString("/tmp/selected.wav"));
        QCOMPARE(ui_->endpoint.decks(), decksBefore);
        QVERIFY(ui_->endpoint.calls.isEmpty());
        QTRY_VERIFY(importButton->isEnabled());
        if (keyboard) {
            path->forceActiveFocus();
            QTRY_VERIFY(path->hasActiveFocus());
            QTest::keyClick(ui_->window, Qt::Key_Return);
        } else {
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(importButton));
        }
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls.first().toMap();
        QCOMPARE(call["action"].toString(), QString("cueImport"));
        QCOMPARE(call["id"].toString(), id);
        QCOMPARE(call["url"].toUrl().toLocalFile(), local);
        QCOMPARE(ui_->endpoint.decks(), decksBefore);

        // A chosen sheet belongs to its source, not the next selected track.
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack1")));
        QTRY_VERIFY(path->property("text").toString().isEmpty());
        QVERIFY(!importButton->isEnabled());
        QCOMPARE(ui_->endpoint.calls.size(), 1);
    }

    void cueSheetBrowseCancelPreservesSelectionPathAndPlayback()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto local = directory.filePath("unchosen.cue");
        QFile file(local);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString id(64, QLatin1Char('e'));
        ui_->endpoint.setLibrary({QVariantMap{{"id", id}, {"name", "Saved source"},
                                             {"path", "/tmp/source.wav"}, {"available", true}}});
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto decksBefore = ui_->endpoint.decks();
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        auto* path = ui_->item("libraryCuePath");
        const QString original = "/tmp/previous.cue";
        QVERIFY(path->setProperty("text", original));
        QVERIFY(ui_->useNonNativeFileDialog("libraryCueFileDialog"));
        auto* dialog = ui_->object("libraryCueFileDialog");
        QVERIFY(dialog->setProperty("currentFolder", QUrl::fromLocalFile(directory.path())));
        QSignalSpy rejected(dialog, SIGNAL(rejected()));
        for (bool selectFile : {false, true}) {
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryCueBrowse")));
            QTRY_VERIFY(dialog->property("visible").toBool());
            if (selectFile) {
                QTRY_VERIFY(ui_->fileDialogItem("fileDialogDelegate0"));
                auto* choice = ui_->fileDialogItem("fileDialogDelegate0");
                QTRY_COMPARE(choice->property("file").toUrl(), QUrl::fromLocalFile(local));
                QTest::mouseClick(choice->window(), Qt::LeftButton, Qt::NoModifier, ui_->center(choice));
            } else {
                QVERIFY(dialog->property("selectedFile").toUrl().isEmpty());
            }
            QVERIFY(QMetaObject::invokeMethod(dialog, "reject", Qt::DirectConnection));
            QTRY_VERIFY(!dialog->property("visible").toBool());
            ui_->window->requestActivate();
            QTRY_VERIFY(ui_->window->isActive());
            QCOMPARE(path->property("text").toString(), original);
            QCOMPARE(ui_->object("libraryDrawer")->property("selectedId").toString(), id);
            QCOMPARE(ui_->endpoint.decks(), decksBefore);
            QVERIFY(ui_->endpoint.calls.isEmpty());
        }
        QCOMPARE(rejected.size(), 2);
    }

    void cueSheetBrowseRejectsStaleSourceIntent_data()
    {
        QTest::addColumn<QString>("invalidate");
        for (const auto* reason : {"selection", "busy", "closed", "source-missing"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }

    void cueSheetBrowseRejectsStaleSourceIntent()
    {
        QFETCH(QString, invalidate);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto local = directory.filePath("late.cue");
        QFile file(local);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString id(64, QLatin1Char('e')), otherId(64, QLatin1Char('f'));
        QVariantMap track{{"id", id}, {"name", "Old source"}, {"path", "/tmp/old.wav"}, {"available", true}};
        const QVariantMap other{{"id", otherId}, {"name", "New source"}, {"path", "/tmp/new.wav"}, {"available", true}};
        ui_->endpoint.setLibrary({track, other});
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        const auto decksBefore = ui_->endpoint.decks();
        QVERIFY(ui_->openLibrary(0));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        auto* drawer = ui_->object("libraryDrawer");
        auto* path = ui_->item("libraryCuePath");
        QVERIFY(path->setProperty("text", "/tmp/previous.cue"));
        QVERIFY(ui_->useNonNativeFileDialog("libraryCueFileDialog"));
        auto* dialog = ui_->object("libraryCueFileDialog");
        QVERIFY(dialog->setProperty("currentFolder", QUrl::fromLocalFile(directory.path())));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryCueBrowse")));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(ui_->fileDialogItem("fileDialogDelegate0"));
        auto* choice = ui_->fileDialogItem("fileDialogDelegate0");
        QTRY_COMPARE(choice->property("file").toUrl(), QUrl::fromLocalFile(local));
        QTest::mouseClick(choice->window(), Qt::LeftButton, Qt::NoModifier, ui_->center(choice));
        if (invalidate == "selection") {
            QVERIFY(QMetaObject::invokeMethod(drawer, "selectTrack", Qt::DirectConnection, Q_ARG(QVariant, QVariant(other))));
        } else if (invalidate == "busy") {
            ui_->endpoint.setBusy(true);
        } else if (invalidate == "closed") {
            QVERIFY(QMetaObject::invokeMethod(drawer, "close", Qt::DirectConnection));
        } else {
            track["available"] = false;
            ui_->endpoint.setLibrary({track, other});
        }
        QSignalSpy accepted(dialog, SIGNAL(accepted()));
        if (invalidate == "source-missing") {
            // Framework acceptance is valid; the source-binding guard must reject it.
            QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
            QCOMPARE(accepted.size(), 1);
        } else {
            QTRY_VERIFY(!dialog->property("visible").toBool());
            QCOMPARE(dialog->property("targetSourceId").toString(), QString{});
            if (invalidate == "busy") ui_->endpoint.setBusy(false);
            if (invalidate == "closed") {
                QTRY_VERIFY(!drawer->property("visible").toBool());
                QVERIFY(ui_->openLibrary(1));
            }
            QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
            QCOMPARE(accepted.size(), 0);
        }
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(path->property("text").toString(), QString("/tmp/previous.cue"));
        QCOMPARE(drawer->property("selectedId").toString(), invalidate == "selection" ? otherId : id);
        QCOMPARE(ui_->endpoint.decks(), decksBefore);
        QVERIFY(ui_->endpoint.calls.isEmpty());
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

    void savedDeckCueRoutesIndependentlyOfLibrarySelection()
    {
        const QString idA(64, QLatin1Char('a')), idB(64, QLatin1Char('b'));
        const QString unrelated(64, QLatin1Char('c'));
        const QVariantMap other{{"id", unrelated}, {"name", "Unrelated source"},
                                {"path", "/tmp/unrelated.wav"}, {"durationSeconds", 120.0}, {"available", true}};
        ui_->endpoint.setLibrary({other});
        ui_->endpoint.loadState(0, 62.04);
        ui_->endpoint.loadState(1, 17.125);
        for (int deck = 0; deck < 2; ++deck) {
            auto tempo = UiEndpoint::tempoState(true);
            tempo["percent"] = deck == 0 ? 7.5 : -3.0;
            tempo["appliedPercent"] = tempo["percent"];
            tempo["keyLock"] = true;
            ui_->endpoint.snapshotDeck(deck, {{"durationSeconds", 120.0}, {"playing", true},
                {"gain", deck == 0 ? .375 : .625}, {"eqGainDb", QVariantList{1., -2., 3.}},
                {"cueSelected", deck == 1}, {"tempo", tempo}});
        }
        ui_->endpoint.snapshotMasterGain(.625);
        ui_->endpoint.snapshotCueGain(.375);
        ui_->endpoint.snapshotCrossfader(.25);
        ui_->endpoint.setReady(true);
        QVERIFY(ui_->openLibrary(0));
        auto* drawer = ui_->object("libraryDrawer");
        QVERIFY(drawer);
        QCOMPARE(drawer->property("selectedId").toString(), QString{});
        auto* saveA = ui_->item("deck0SaveCue");
        auto* recallA = ui_->item("deck0RecallCue");
        auto* saveB = ui_->item("deck1SaveCue");
        auto* recallB = ui_->item("deck1RecallCue");
        auto* statusA = ui_->item("deck0SavedCueStatus");
        auto* statusB = ui_->item("deck1SavedCueStatus");
        for (auto* control : {saveA, recallA, saveB, recallB, statusA, statusB}) {
            QVERIFY(control);
            QTRY_VERIFY(control->isVisible() && control->width() > 0 && control->height() > 0);
        }
        QCOMPARE(saveA->property("text").toString(), QString("Save CUE A"));
        QCOMPARE(saveB->property("text").toString(), QString("Save CUE B"));
        QCOMPARE(recallA->property("text").toString(), QString("Recall A"));
        QCOMPARE(recallB->property("text").toString(), QString("Recall B"));
        for (auto* control : {saveA, recallA, saveB, recallB}) {
            QVERIFY(!control->isEnabled());
            auto* accessible = QAccessible::queryAccessibleInterface(control);
            QVERIFY(accessible);
            QVERIFY(accessible->text(QAccessible::Description).contains("Add this source to the library"));
        }
        for (auto* status : {statusA, statusB}) {
            QCOMPARE(status->property("textFormat").toInt(), int(Qt::PlainText));
            QVERIFY(!status->property("text").toString().contains("0:00.000"));
        }
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(saveA));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(recallB));
        QVERIFY(ui_->endpoint.calls.isEmpty()); // Raw loaded paths have no catalog cue.

        ui_->endpoint.setLibrary({other,
            QVariantMap{{"id", idA}, {"name", "Deck A source"}, {"path", "/tmp/a.wav"},
                        {"durationSeconds", 120.0}, {"available", true}},
            QVariantMap{{"id", idB}, {"name", "Deck B source"}, {"path", "/tmp/b.wav"},
                        {"durationSeconds", 120.0}, {"available", true}}});
        ui_->endpoint.setSourceId(0, idA);
        ui_->endpoint.setSourceId(1, idB);
        ui_->endpoint.snapshotDeck(0, {{"savedCue", UiEndpoint::savedCueState(true)}});
        ui_->endpoint.snapshotDeck(1, {{"savedCue", UiEndpoint::savedCueState(true, true, "168000", 3.5)}});
        QTRY_VERIFY(saveA->isEnabled() && saveB->isEnabled() && recallB->isEnabled());
        QVERIFY(!recallA->isEnabled());
        QCOMPARE(drawer->property("selectedId").toString(), QString{});
        QTRY_VERIFY(statusB->property("text").toString().contains("0:03.500"));
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        QTRY_COMPARE(drawer->property("selectedId").toString(), unrelated);
        QVERIFY(saveA->isEnabled() && saveB->isEnabled() && recallB->isEnabled());
        QVERIFY(!recallA->isEnabled());
        QVERIFY(ui_->endpoint.calls.isEmpty());
        const auto before = ui_->endpoint.decks();

        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(saveA));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        ui_->endpoint.snapshotDeck(0, {{"savedCue", UiEndpoint::savedCueState(true, true, "2977920", 62.04)}});
        QTRY_VERIFY(statusA->property("text").toString().contains("1:02.040"));
        QVERIFY(statusB->property("text").toString().contains("0:03.500"));
        QTRY_VERIFY(recallA->isEnabled());
        saveB->forceActiveFocus();
        QTRY_VERIFY(saveB->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        ui_->endpoint.snapshotDeck(1, {{"savedCue", UiEndpoint::savedCueState(true, true, "822000", 17.125)}});
        QTRY_VERIFY(statusB->property("text").toString().contains("0:17.125"));
        QVERIFY(statusA->property("text").toString().contains("1:02.040"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(recallB));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        recallA->forceActiveFocus();
        QTRY_VERIFY(recallA->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 4);
        const std::array<QString, 4> actions{"saveDeckCue", "saveDeckCue", "recallDeckCue", "recallDeckCue"};
        const std::array<int, 4> decks{0, 1, 1, 0};
        for (int index = 0; index < 4; ++index) {
            const auto call = ui_->endpoint.calls[index].toMap();
            QCOMPARE(call["action"].toString(), actions[index]);
            QCOMPARE(call["deck"].toInt(), decks[index]);
        }
        for (int deck = 0; deck < 2; ++deck) {
            const auto expected = before[deck].toMap(), actual = ui_->endpoint.decks()[deck].toMap();
            for (const auto* field : {"gain", "eqGainDb", "tempo", "playing", "positionSeconds", "cueSeconds",
                                      "cueSelected", "sourceId", "sourceGeneration"})
                QCOMPARE(actual.value(field), expected.value(field));
        }
        QCOMPARE(ui_->endpoint.masterGain(), .625);
        QCOMPARE(ui_->endpoint.cueGain(), .375);
        QCOMPARE(ui_->endpoint.crossfader(), .25);
        QCOMPARE(drawer->property("selectedId").toString(), unrelated);
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

    void jogAccessibleActionsRouteBothDirectionsAndRespectAvailability_data()
    {
        QTest::addColumn<int>("deck");
        QTest::newRow("deckA") << 0;
        QTest::newRow("deckB") << 1;
    }

    void jogAccessibleActionsRouteBothDirectionsAndRespectAvailability()
    {
        QFETCH(int, deck);
        ui_->endpoint.loadState(0, 5); ui_->endpoint.loadState(1, 5);
        ui_->endpoint.setReady(true);
        auto* jog = ui_->item(QString("deck%1Jog").arg(deck).toUtf8().constData());
        QVERIFY(jog && jog->isEnabled());
        auto* accessible = QAccessible::queryAccessibleInterface(jog);
        QVERIFY(accessible && accessible->actionInterface());
        QCOMPARE(accessible->role(), QAccessible::Dial);
        QVERIFY(accessible->text(QAccessible::Name).contains(deck == 0 ? "Deck A" : "Deck B"));
        QVERIFY(accessible->text(QAccessible::Description).contains("ten milliseconds"));
        auto* actions = accessible->actionInterface();
        QVERIFY(actions->actionNames().contains(QAccessibleActionInterface::increaseAction()));
        QVERIFY(actions->actionNames().contains(QAccessibleActionInterface::decreaseAction()));
        const auto peer = ui_->endpoint.decks()[1 - deck];
        const auto master = ui_->endpoint.masterGain(), cue = ui_->endpoint.cueGain(), crossfader = ui_->endpoint.crossfader();
        actions->doAction(QAccessibleActionInterface::increaseAction());
        actions->doAction(QAccessibleActionInterface::decreaseAction());
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        for (int index = 0; index < 2; ++index) {
            verifyCueCall(index, "jog", deck);
            QCOMPARE(ui_->endpoint.calls[index].toMap()["frames"].toLongLong(), index == 0 ? qint64(480) : qint64(-480));
        }
        for (const auto* gate : {"replacing", "unloaded", "output-unready"}) {
            if (QString::fromLatin1(gate) == "replacing") ui_->endpoint.setReplacing(deck, true);
            else if (QString::fromLatin1(gate) == "unloaded") ui_->endpoint.unloadState(deck);
            else ui_->endpoint.setReady(false);
            QTRY_VERIFY(!jog->isEnabled());
            actions->doAction(QAccessibleActionInterface::increaseAction());
            actions->doAction(QAccessibleActionInterface::decreaseAction());
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), 2);
            ui_->endpoint.loadState(deck, 5); ui_->endpoint.setReady(true);
            QTRY_VERIFY(jog->isEnabled());
        }
        actions->doAction(QAccessibleActionInterface::increaseAction());
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        verifyCueCall(2, "jog", deck);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["frames"].toLongLong(), qint64(480));
        QCOMPARE(ui_->endpoint.decks()[1 - deck], peer);
        QCOMPARE(ui_->endpoint.masterGain(), master);
        QCOMPARE(ui_->endpoint.cueGain(), cue);
        QCOMPARE(ui_->endpoint.crossfader(), crossfader);
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void heldVinylSearchEndsBeforeJog_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<QString>("search");
        for (int deck = 0; deck < 2; ++deck)
            for (const auto* search : {"arrow", "shift-arrow", "wheel", "accessible"})
                QTest::newRow(qPrintable(QString("deck%1-%2").arg(deck).arg(search)))
                    << deck << QString::fromLatin1(search);
    }

    void heldVinylSearchEndsBeforeJog()
    {
        QFETCH(int, deck); QFETCH(QString, search);
        ui_->endpoint.loadState(0, 5); ui_->endpoint.loadState(1, 5);
        ui_->endpoint.snapshotDeck(deck, {{"playing", true}});
        ui_->endpoint.setReady(true);
        auto* jog = ui_->item(QString("deck%1Jog").arg(deck).toUtf8().constData());
        QVERIFY(jog);
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update(); QTRY_VERIFY(polished.count() > 0);
        const auto peer = ui_->endpoint.decks()[1 - deck];
        const auto point = jog->mapToScene(QPointF(jog->width() * .85, jog->height() / 2)).toPoint();
        bool pointerHeld = true;
        const auto releaseOnFailure = qScopeGuard([&] {
            if (pointerHeld)
                QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        });
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto token = ui_->endpoint.calls.first().toMap()["token"].toString();
        QVERIFY(!token.isEmpty());
        QTRY_VERIFY(jog->hasActiveFocus());
        qint64 expected = 480;
        if (search == "accessible") {
            auto* accessible = QAccessible::queryAccessibleInterface(jog);
            QVERIFY(accessible && accessible->actionInterface());
            accessible->actionInterface()->doAction(QAccessibleActionInterface::increaseAction());
        } else if (search == "wheel") {
            QWheelEvent wheel(QPointF(point), QPointF(ui_->window->mapToGlobal(point)),
                              {}, {0, 120}, Qt::LeftButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QGuiApplication::sendEvent(ui_->window, &wheel);
        } else {
            const bool shift = search == "shift-arrow";
            expected = shift ? -48000 : 480;
            QTest::keyClick(ui_->window, shift ? Qt::Key_Left : Qt::Key_Right,
                            shift ? Qt::ShiftModifier : Qt::NoModifier);
        }
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        verifyCueCall(0, "scratchBegin", deck);
        verifyCueCall(1, "scratchEnd", deck);
        verifyCueCall(2, "jog", deck);
        QCOMPARE(ui_->endpoint.calls[1].toMap()["token"].toString(), token);
        QCOMPARE(ui_->endpoint.calls[2].toMap()["frames"].toLongLong(), expected);
        QCOMPARE(ui_->endpoint.decks()[1 - deck], peer);
        QVERIFY(ui_->endpoint.decks()[deck].toMap()["playing"].toBool());
        // The original held pointer cannot submit more movement or another End.
        QTest::mouseMove(ui_->window, ui_->center(jog));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(jog));
        pointerHeld = false;
        QCoreApplication::processEvents();
        QCOMPARE(ui_->endpoint.calls.size(), 3);
        pointerHeld = true;
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 4);
        const auto fresh = ui_->endpoint.calls.last().toMap()["token"].toString();
        QVERIFY(!fresh.isEmpty() && fresh != token);
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        pointerHeld = false;
        QTRY_COMPARE(ui_->endpoint.calls.size(), 5);
        verifyCueCall(4, "scratchEnd", deck);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["token"].toString(), fresh);
        QCOMPARE(ui_->endpoint.decks()[1 - deck], peer);
    }

    void vinylPointerUsesCumulativeTokenWithoutTransportMutation_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<bool>("playing");
        QTest::addColumn<bool>("processed");
        QTest::addColumn<double>("percent");
        QTest::newRow("deck-a-paused") << 0 << false << false << 0.;
        QTest::newRow("deck-a-playing") << 0 << true << false << 0.;
        QTest::newRow("deck-b-paused") << 1 << false << false << 0.;
        QTest::newRow("deck-b-playing") << 1 << true << false << 0.;
        QTest::newRow("deck-a-active-zero-paused") << 0 << false << true << 0.;
        QTest::newRow("deck-b-active-zero-paused") << 1 << false << true << 0.;
        QTest::newRow("deck-a-active-six-playing") << 0 << true << true << 6.;
        QTest::newRow("deck-b-active-six-playing") << 1 << true << true << 6.;
    }

    void vinylPointerUsesCumulativeTokenWithoutTransportMutation()
    {
        QFETCH(int, deck); QFETCH(bool, playing); QFETCH(bool, processed); QFETCH(double, percent);
        ui_->endpoint.loadState(0, 5.0);
        ui_->endpoint.loadState(1, 5.0);
        ui_->endpoint.snapshotDeck(deck, {{"playing", playing}});
        if (processed) {
            auto tempo = UiEndpoint::tempoState(true);
            tempo["active"] = true; tempo["percent"] = percent; tempo["appliedPercent"] = percent;
            tempo["keyLock"] = true; tempo["generation"] = qulonglong(42);
            ui_->endpoint.snapshotDeck(deck, {{"tempo", tempo}});
        }
        ui_->endpoint.setReady(true);
        auto* jog = ui_->item(QString("deck%1Jog").arg(deck).toUtf8().constData());
        QVERIFY(jog);
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update();
        QTRY_VERIFY(polished.count() > 0);
        constexpr double pi = 3.14159265358979323846;
        auto point = [jog](double angle) {
            const auto radius = jog->width() * .35;
            return jog->mapToScene(QPointF(jog->width() / 2 + std::cos(angle) * radius,
                                          jog->height() / 2 + std::sin(angle) * radius)).toPoint();
        };
        const auto stateBefore = ui_->endpoint.decks();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point(-pi / 2));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto token = ui_->endpoint.calls.first().toMap()["token"].toString();
        QVERIFY(!token.isEmpty());
        QCOMPARE(ui_->endpoint.calls.first().toMap()["action"].toString(), QString("scratchBegin"));
        QTest::mouseMove(ui_->window, point(0));
        QTRY_COMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("scratchMove"));
        QTRY_VERIFY(std::abs(ui_->endpoint.calls.last().toMap()["frames"].toLongLong() - 24000) < 500);
        QTest::mouseMove(ui_->window, point(pi / 2));
        QTRY_VERIFY(std::abs(ui_->endpoint.calls.last().toMap()["frames"].toLongLong() - 48000) < 500);
        QTest::mouseMove(ui_->window, point(0));
        QTRY_VERIFY(std::abs(ui_->endpoint.calls.last().toMap()["frames"].toLongLong() - 24000) < 500);
        QCOMPARE(ui_->endpoint.decks(), stateBefore); // Pointer contact has no speculative seek/play ACK.
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(0));
        QTRY_COMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("scratchEnd"));
        int begins = 0, moves = 0, ends = 0;
        for (const auto& item : ui_->endpoint.calls) {
            const auto call = item.toMap();
            const auto action = call["action"].toString();
            QCOMPARE(call["deck"].toInt(), deck);
            QCOMPARE(call["token"].toString(), token);
            if (action == "scratchBegin") ++begins;
            else if (action == "scratchMove") ++moves;
            else if (action == "scratchEnd") ++ends;
            else QFAIL("Vinyl pointer must not submit jog, seek or play commands");
        }
        QCOMPARE(begins, 1); QVERIFY(moves >= 3); QCOMPARE(ends, 1);
        QCOMPARE(ui_->endpoint.decks(), stateBefore);
    }

    void vinylSeamDeadZoneAndRejectedMotionNeedFreshPress()
    {
        ui_->endpoint.loadState(0, 5.0); ui_->endpoint.setReady(true);
        auto* jog = ui_->item("deck0Jog");
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update(); QTRY_VERIFY(polished.count() > 0);
        constexpr double pi = 3.14159265358979323846;
        auto point = [jog](double angle) {
            const auto radius = jog->width() * .35;
            return jog->mapToScene(QPointF(jog->width() / 2 + std::cos(angle) * radius,
                                          jog->height() / 2 + std::sin(angle) * radius)).toPoint();
        };
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point(pi - .15));
        QTest::mouseMove(ui_->window, point(-pi + .15));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        const auto seam = ui_->endpoint.calls.last().toMap()["frames"].toLongLong();
        QVERIFY(seam > 3000 && seam < 6000); // Short angular seam, not almost a full revolution.
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(-pi + .15));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        ui_->endpoint.calls.clear();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(jog));
        QTest::mouseMove(ui_->window, point(0));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(0));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls[0].toMap()["action"].toString(), QString("scratchBegin"));
        QCOMPARE(ui_->endpoint.calls[1].toMap()["action"].toString(), QString("scratchEnd"));
        ui_->endpoint.calls.clear();
        ui_->endpoint.setScratchAccepted(false);
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point(0));
        QTest::mouseMove(ui_->window, point(.3));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(.3));
        QCOMPARE(ui_->endpoint.calls.size(), 1);
        QVERIFY(ui_->endpoint.calls.first().toMap()["token"].toString().isEmpty());
        ui_->endpoint.calls.clear();
        ui_->endpoint.setScratchAccepted(true, false);
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point(0));
        QTest::mouseMove(ui_->window, point(.3));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3); // Begin, failed Move, exactly one End.
        const auto oldToken = ui_->endpoint.calls.first().toMap()["token"].toString();
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("scratchEnd"));
        ui_->endpoint.setScratchAccepted(true);
        QTest::mouseMove(ui_->window, point(.6));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(.6));
        QCOMPARE(ui_->endpoint.calls.size(), 3);
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point(0));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 4);
        QVERIFY(ui_->endpoint.calls.last().toMap()["token"].toString() != oldToken);
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point(0));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 5);
    }

    void vinylCancellationPairsExactlyOneOldSourceToken_data()
    {
        QTest::addColumn<int>("deck"); QTest::addColumn<QString>("reason");
        for (const auto* reason : {"ungrab", "focus", "hidden", "window", "output", "escape"})
            QTest::newRow(reason) << 0 << QString::fromLatin1(reason);
        QTest::newRow("peer-deck-ungrab") << 1 << QString("ungrab");
        QTest::newRow("source-swap") << 1 << QString("source");
        QTest::newRow("same-id-new-generation") << 1 << QString("generation");
        QTest::newRow("replacement") << 1 << QString("replacement");
    }

    void vinylCancellationPairsExactlyOneOldSourceToken()
    {
        QFETCH(int, deck); QFETCH(QString, reason);
        ui_->endpoint.loadState(0); ui_->endpoint.loadState(1); ui_->endpoint.setReady(true);
        auto* jog = ui_->item(QString("deck%1Jog").arg(deck).toUtf8().constData());
        auto* input = ui_->item(QString("deck%1ScratchInput").arg(deck).toUtf8().constData());
        QVERIFY(jog && input);
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update(); QTRY_VERIFY(polished.count() > 0);
        const auto point = jog->mapToScene(QPointF(jog->width() * .85, jog->height() / 2)).toPoint();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto token = ui_->endpoint.calls.first().toMap()["token"].toString();
        QVERIFY(!token.isEmpty());
        if (reason == "ungrab") input->ungrabMouse();
        else if (reason == "focus") ui_->item("masterGain")->forceActiveFocus();
        else if (reason == "hidden") jog->setVisible(false);
        else if (reason == "window") ui_->window->hide();
        else if (reason == "output") ui_->endpoint.setReady(false);
        else if (reason == "escape") QTest::keyClick(ui_->window, Qt::Key_Escape);
        else if (reason == "source") ui_->endpoint.setSourceId(deck, "new-source");
        else if (reason == "generation") ui_->endpoint.snapshotDeck(deck, {{"sourceGeneration", qulonglong(2)}});
        else if (reason == "replacement") ui_->endpoint.setReplacing(deck, true);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("scratchEnd"));
        QCOMPARE(ui_->endpoint.calls.last().toMap()["token"].toString(), token);
        QCOMPARE(input->property("scratchToken").toString(), QString{});
        ui_->endpoint.setReady(true); ui_->endpoint.setReplacing(deck, false); jog->setVisible(true);
        if (reason == "window") {
            ui_->window->show(); ui_->window->requestActivate();
            QTRY_VERIFY(ui_->window->isActive());
        }
        // Retire the original mouse contact, without submitting another End to
        // a new source or resurrecting the cancelled transaction.
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(ui_->endpoint.calls.size(), 2);
    }

    void vinylReentrantBeginStillPairsItsAcceptedToken_data()
    {
        QTest::addColumn<QString>("reason");
        QTest::newRow("same-source-new-generation") << QString("generation");
        QTest::newRow("becomes-replacing") << QString("replacing");
    }

    void vinylReentrantBeginStillPairsItsAcceptedToken()
    {
        QFETCH(QString, reason);
        ui_->endpoint.loadState(0); ui_->endpoint.setReady(true);
        ui_->endpoint.setScratchInvalidateOnBegin(reason);
        auto* jog = ui_->item("deck0Jog");
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update(); QTRY_VERIFY(polished.count() > 0);
        const auto point = jog->mapToScene(QPointF(jog->width() * .85, jog->height() / 2)).toPoint();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls[0].toMap()["action"].toString(), QString("scratchBegin"));
        QCOMPARE(ui_->endpoint.calls[1].toMap()["action"].toString(), QString("scratchEnd"));
        QCOMPARE(ui_->endpoint.calls[0].toMap()["token"], ui_->endpoint.calls[1].toMap()["token"]);
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(ui_->endpoint.calls.size(), 2);
    }

    void vinylModeGatesPointerWhileKeyboardStillSearches()
    {
        ui_->endpoint.loadState(0, 5); ui_->endpoint.setReady(true);
        auto* jog = ui_->item("deck0Jog");
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update(); QTRY_VERIFY(polished.count() > 0);
        auto baseline = ui_->endpoint.decks()[0].toMap();
        baseline["liveStems"] = false; // Storage mode is independent of waveform provenance.
        const auto point = jog->mapToScene(QPointF(jog->width() * .85, jog->height() / 2)).toPoint();
        for (const auto* reason : {"unsupported", "live", "cue", "cue-held", "loop-in", "loop-cached", "read-error"}) {
            auto fields = baseline;
            if (QString(reason) == "unsupported") fields["scratch"] = UiEndpoint::scratchState(false);
            else if (QString(reason) == "live") fields["liveStems"] = true;
            else if (QString(reason) == "cue") fields["cuePreview"] = true;
            else if (QString(reason) == "cue-held") {
                // Holding CUE may set a cue point without temporary preview.
                // That distinct state must also reject pointer scratch.
                fields["cueHeld"] = true; fields["cuePreview"] = false;
            }
            else if (QString(reason).startsWith("loop")) {
                auto loop = UiEndpoint::loopState(true);
                loop[QString(reason) == "loop-in" ? "inSet" : "cached"] = true; fields["loop"] = loop;
            } else {
                auto scratch = UiEndpoint::scratchState(true); scratch["errorCode"] = EIO; fields["scratch"] = scratch;
            }
            ui_->endpoint.snapshotDeck(0, fields);
            ui_->endpoint.calls.clear();
            QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
            QVERIFY2(ui_->endpoint.calls.isEmpty(), reason);
            jog->forceActiveFocus(); QTRY_VERIFY(jog->hasActiveFocus());
            QTest::keyClick(ui_->window, Qt::Key_Right);
            QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
            QCOMPARE(ui_->endpoint.calls[0].toMap()["action"].toString(), QString("jog"));
            QCOMPARE(ui_->endpoint.calls[0].toMap()["frames"].toLongLong(), qint64(480));
        }
    }

    void vinylTouchDragReleaseAndGrabLoss_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<bool>("cancelGrab");
        for (int deck = 0; deck < 2; ++deck)
            for (const bool cancel : {false, true})
                QTest::newRow(qPrintable(QString("deck%1-%2").arg(deck).arg(cancel ? "grab-loss" : "release")))
                    << deck << cancel;
    }

    void vinylTouchDragReleaseAndGrabLoss()
    {
        QFETCH(int, deck); QFETCH(bool, cancelGrab);
        ui_->window->resize(1180, 760);
        QTRY_COMPARE(ui_->window->size(), QSize(1180, 760));
        ui_->endpoint.loadState(0, 5); ui_->endpoint.loadState(1, 5);
        ui_->endpoint.setReady(true);
        auto* jog = ui_->item(QString("deck%1Jog").arg(deck).toUtf8().constData());
        auto* input = ui_->item(QString("deck%1ScratchInput").arg(deck).toUtf8().constData());
        QVERIFY(jog && input);
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update(); QTRY_VERIFY(polished.count() > 0);
        constexpr double pi = 3.14159265358979323846;
        auto point = [jog](double angle) {
            const auto radius = jog->width() * .35;
            return jog->mapToScene(QPointF(jog->width() / 2 + std::cos(angle) * radius,
                                          jog->height() / 2 + std::sin(angle) * radius)).toPoint();
        };
        const auto before = ui_->endpoint.decks();
        auto touch = QTest::touchEvent(ui_->window, touchDevice_.get(), false);
        touchActive_ = true;
        touch.press(0, point(-pi / 2), ui_->window).commit();
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto token = ui_->endpoint.calls.first().toMap()["token"].toString();
        QVERIFY(!token.isEmpty());
        for (const auto& step : {std::pair{0., qint64(24000)}, std::pair{pi / 2, qint64(48000)},
                                 std::pair{0., qint64(24000)}}) {
            touch.move(0, point(step.first), ui_->window).commit();
            QTRY_COMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("scratchMove"));
            QTRY_VERIFY(std::abs(ui_->endpoint.calls.last().toMap()["frames"].toLongLong() - step.second) < 500);
        }
        QCOMPARE(ui_->endpoint.decks(), before); // No optimistic source/play acknowledgement.
        if (cancelGrab) {
            // Public on Qt6.4 too: lose the actual synthesized-touch mouse grab.
            // This is not a platform TouchCancel; the separate version-gated
            // platform-cancel case retains that explicitly narrower scope.
            input->ungrabMouse();
            QTRY_VERIFY(!input->property("pressed").toBool());
            QTRY_COMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("scratchEnd"));
            const auto ended = ui_->endpoint.calls.size();
            touch.move(0, point(-pi / 2), ui_->window).commit();
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), ended);
        }
        touch.release(0, point(0), ui_->window).commit(); touchActive_ = false;
        QTRY_COMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("scratchEnd"));
        int begins = 0, moves = 0, ends = 0;
        for (const auto& item : ui_->endpoint.calls) {
            const auto call = item.toMap();
            QCOMPARE(call["deck"].toInt(), deck);
            QCOMPARE(call["token"].toString(), token);
            const auto action = call["action"].toString();
            if (action == "scratchBegin") ++begins;
            else if (action == "scratchMove") ++moves;
            else if (action == "scratchEnd") ++ends;
            else QFAIL("Touch vinyl must not submit search, seek or play commands");
        }
        QCOMPARE(begins, 1); QVERIFY(moves >= 3); QCOMPARE(ends, 1);
        QCOMPARE(ui_->endpoint.decks(), before);
        const auto oldCalls = ui_->endpoint.calls.size();
        touchActive_ = true;
        touch.press(0, point(0), ui_->window).commit();
        QTRY_COMPARE(ui_->endpoint.calls.size(), oldCalls + 1);
        const auto fresh = ui_->endpoint.calls.last().toMap()["token"].toString();
        QVERIFY(!fresh.isEmpty() && fresh != token);
        touch.release(0, point(0), ui_->window).commit(); touchActive_ = false;
        QTRY_COMPARE(ui_->endpoint.calls.size(), oldCalls + 2);
        verifyCueCall(oldCalls + 1, "scratchEnd", deck);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["token"].toString(), fresh);
        QCOMPARE(ui_->endpoint.decks(), before);
    }

    void vinylPlatformTouchCancelEndsOnceAndAllowsFreshContact()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        ui_->endpoint.loadState(0); ui_->endpoint.setReady(true);
        auto* jog = ui_->item("deck0Jog");
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update(); QTRY_VERIFY(polished.count() > 0);
        const auto point = jog->mapToScene(QPointF(jog->width() * .85, jog->height() / 2)).toPoint();
        auto touch = QTest::touchEvent(ui_->window, touchDevice_.get(), false);
        touchActive_ = true;
        touch.press(0, point, ui_->window).commit();
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto first = ui_->endpoint.calls.first().toMap()["token"].toString();
        QVERIFY(touch.cancel()); touchActive_ = false;
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("scratchEnd"));
        touchActive_ = true;
        touch.press(0, point, ui_->window).commit();
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        QVERIFY(ui_->endpoint.calls.last().toMap()["token"].toString() != first);
        touch.release(0, point, ui_->window).commit(); touchActive_ = false;
        QTRY_COMPARE(ui_->endpoint.calls.size(), 4);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("scratchEnd"));
#else
        QSKIP("Qt6.4 has no public platform-cancel helper; actual mouse-grab loss is covered separately");
#endif
    }

    void replacementGatesOnlyItsDeckAndCancelsJogGesture()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        auto* jog = ui_->item("deck0Jog");
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update(); QTRY_VERIFY(polished.count() > 0);
        const auto start = jog->mapToScene(QPointF(jog->width() * .85, jog->height() / 2)).toPoint();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, start);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls[0].toMap()["action"].toString(), QString("scratchBegin"));
        ui_->endpoint.setReplacing(0, true);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls[1].toMap()["action"].toString(), QString("scratchEnd"));
        for (const auto* name : {"deck0Jog", "deck0Play", "deck0Seek", "deck0Gain", "deck0Cue", "deck0CueSelect"})
            QTRY_VERIFY2(!ui_->item(name)->isEnabled(), name);
        for (const auto* name : {"deck1Jog", "deck1Play", "deck1Gain", "deck1CueSelect", "masterGain", "cueGain"})
            QVERIFY2(ui_->item(name)->isEnabled(), name);
        ui_->endpoint.setReplacing(0, false);
        QTRY_VERIFY(jog->isEnabled());
        QTest::mouseMove(ui_->window, ui_->center(jog));
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(jog));
        QCOMPARE(ui_->endpoint.calls.size(), 2); // Replacement cannot revive the pointer token.
    }

    void hotCuePadsWaitForAudioAcknowledgement_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<bool>("dark");
        for (int deck = 0; deck < 2; ++deck)
            for (bool dark : {false, true})
                QTest::newRow(qPrintable(QString("deck%1-%2").arg(deck).arg(dark))) << deck << dark;
    }

    void hotCuePadsWaitForAudioAcknowledgement()
    {
        QFETCH(int, deck);
        QFETCH(bool, dark);
        if (!dark) QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("themeToggle")));
        const auto prefix = QString("deck%1HotCue").arg(deck);
        auto* pad = ui_->item((prefix + "7").toUtf8().constData());
        auto* remove = ui_->item((prefix + "Delete").toUtf8().constData());
        QVERIFY(pad && remove);
        QVERIFY(!pad->isEnabled() && !remove->isEnabled());
        ui_->endpoint.loadState(deck);
        ui_->endpoint.setReady(true);
        QTRY_VERIFY(pad->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(pad));
        QCOMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls[0].toMap(), (QVariantMap{{"action", "hotCue"}, {"deck", deck}, {"slot", 7}}));
        QVERIFY(!pad->parentItem()->property("indicatorOn").toBool());
        QVariantList points;
        for (int index = 0; index < 8; ++index)
            points.append(QVariantMap{{"set", index == 7}, {"seconds", index == 7 ? 1.25 : 0.}});
        ui_->endpoint.snapshotDeck(deck, {{"hotCues", points}});
        QTRY_VERIFY(pad->parentItem()->property("indicatorOn").toBool());
        auto* accessible = QAccessible::queryAccessibleInterface(pad);
        QVERIFY(accessible);
        QVERIFY(accessible->text(QAccessible::Name).contains("hot cue H"));
        QVERIFY(accessible->text(QAccessible::Description).contains("1.250"));
        pad->forceActiveFocus();
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls[1], ui_->endpoint.calls[0]);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(remove));
        QTRY_VERIFY(remove->parentItem()->property("indicatorOn").toBool());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(pad));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        QCOMPARE(ui_->endpoint.calls[2].toMap(), (QVariantMap{{"action", "clearHotCue"}, {"deck", deck}, {"slot", 7}}));
        QVERIFY(pad->parentItem()->property("indicatorOn").toBool()); // Wait for native ACK.
        QTRY_VERIFY(!remove->parentItem()->property("indicatorOn").toBool());
        points[7] = QVariantMap{{"set", false}, {"seconds", 0.}};
        ui_->endpoint.snapshotDeck(deck, {{"hotCues", points}});
        QTRY_VERIFY(!pad->parentItem()->property("indicatorOn").toBool());
        ui_->endpoint.setReplacing(deck, true);
        QTRY_VERIFY(!pad->isEnabled() && !remove->isEnabled());
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void hotCueCaptureIsDisabledDuringLoopButRecallRemainsAvailable()
    {
        ui_->endpoint.loadState(0);
        ui_->endpoint.setReady(true);
        QVariantList points;
        for (int index = 0; index < 8; ++index)
            points.append(QVariantMap{{"set", index == 0}, {"seconds", 0.}});
        auto loop = UiEndpoint::loopState(true);
        loop["active"] = true;
        ui_->endpoint.snapshotDeck(0, {{"hotCues", points}, {"loop", loop}});
        QTRY_VERIFY(ui_->item("deck0HotCue0")->isEnabled());
        QVERIFY(!ui_->item("deck0HotCue1")->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck0HotCue0")));
        QCOMPARE(ui_->endpoint.calls.size(), 1);
        QCOMPARE(ui_->endpoint.calls[0].toMap()["action"].toString(), QString("hotCue"));
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
        unsigned trims = 0, colors = 0, activeEq = 0, reservedEq = 0, memoryCalls = 0;
        const auto inspect = [&](auto&& self, QQuickItem* item) -> void {
            const auto reason = item->property("unavailableReason").toString();
            if (reason.contains("Memory cue")) {
                QVERIFY(!item->property("available").toBool());
                ++memoryCalls;
            }
            const auto help = item->property("helpText").toString();
            if (help.startsWith("Input trim is not implemented.") ||
                    help.startsWith("Channel filter and color effects are not implemented.")) {
                QVERIFY(!item->property("activeControl").toBool());
                trims += help.startsWith("Input trim");
                colors += help.startsWith("Channel filter");
            }
            const auto caption = item->property("caption").toString();
            if (caption == "HI" || caption == "MID" || caption == "LOW") {
                if (item->property("activeControl").toBool()) ++activeEq;
                else ++reservedEq;
            }
            for (auto* child : item->childItems()) self(self, child);
        };
        inspect(inspect, ui_->window->contentItem());
        QVERIFY(trims == 4 && colors == 4 && memoryCalls > 0);
        QCOMPARE(activeEq, 6U);
        QCOMPARE(reservedEq, 6U); // C and D remain reserved.
        QVERIFY(ui_->endpoint.calls.isEmpty());
    }

    void channelEqAccessibleNumericWaitsForAcknowledgement_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<int>("band");
        QTest::addColumn<bool>("dark");
        for (int deck = 0; deck < 2; ++deck)
            for (int band = 0; band < 3; ++band)
                for (bool dark : {false, true})
                    QTest::newRow(qPrintable(QString("deck%1-band%2-%3").arg(deck).arg(band).arg(dark ? "dark" : "light")))
                        << deck << band << dark;
    }

    void channelEqAccessibleNumericWaitsForAcknowledgement()
    {
        QFETCH(int, deck); QFETCH(int, band); QFETCH(bool, dark);
        ui_->window->resize(1280, 800);
        QTRY_COMPARE(ui_->window->size(), QSize(1280, 800));
        verifyControlsFit({"themeToggle"});
        if (!dark) QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("themeToggle")));
        ui_->endpoint.loadState(0); ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        ui_->endpoint.setNumericAcknowledgement(false);
        ui_->endpoint.snapshotEqGain(deck, band, -6.5);
        const std::array names{"LOW", "MID", "HI"};
        auto* dial = ui_->item(QString("deck%1Eq%2").arg(deck).arg(names[band]).toUtf8().constData());
        QVERIFY(dial);
        QTRY_VERIFY(dial->isEnabled());
        auto* accessible = QAccessible::queryAccessibleInterface(dial);
        QVERIFY(accessible && accessible->valueInterface());
        auto* numeric = accessible->valueInterface();
        QCOMPARE(numeric->minimumValue().toDouble(), 0.0);
        QCOMPARE(numeric->maximumValue().toDouble(), 1.0);
        QTRY_COMPARE(numeric->currentValue().toDouble(), .375);
        const auto description = accessible->text(QAccessible::Description);
        QVERIFY(description.contains("dB") && description.contains("cut", Qt::CaseInsensitive) &&
                description.contains("boost", Qt::CaseInsensitive));
        QVERIFY(accessible->text(QAccessible::Name).contains(deck == 0 ? "Deck A" : "Deck B"));
        const auto accepted = ui_->endpoint.decks();
        const auto master = ui_->endpoint.masterGain(), cue = ui_->endpoint.cueGain(), crossfader = ui_->endpoint.crossfader();
        QVERIFY(ui_->endpoint.calls.isEmpty());
        // Independent endpoint/quarter/neutral oracles, not a copy of the QML conversion.
        for (const auto [position, db] : {std::pair{0., -26.}, {.25, -13.}, {.5, 0.}, {.75, 3.}, {1., 6.}}) {
            const auto before = ui_->endpoint.calls.size();
            numeric->setCurrentValue(position);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            const auto call = ui_->endpoint.calls.last().toMap();
            QCOMPARE(call["action"].toString(), QString("eq"));
            QCOMPARE(call["deck"].toInt(), deck);
            QCOMPARE(call["band"].toInt(), band);
            QCOMPARE(call["value"].toDouble(), db);
            QTRY_COMPARE(numeric->currentValue().toDouble(), .375);
            QCOMPARE(ui_->endpoint.decks(), accepted);
        }
        const auto count = ui_->endpoint.calls.size();
        ui_->endpoint.snapshotEqGain(deck, band, 1.5);
        QTRY_COMPARE(numeric->currentValue().toDouble(), .625);
        QVERIFY(accessible->text(QAccessible::Description).contains("1.5"));
        QCOMPARE(ui_->endpoint.calls.size(), count); // ACK is never an input request.
        for (const auto& gate : {QString("output"), QString("unloaded"), QString("replacing")}) {
            if (gate == "output") ui_->endpoint.setReady(false);
            else ui_->endpoint.snapshotDeck(deck, {{gate == "unloaded" ? "loaded" : "replacing", gate != "unloaded"}});
            QTRY_VERIFY(!dial->isEnabled());
            numeric->setCurrentValue(.9); // Qt's actual value interface bypasses disabled pointer input.
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), count);
            QTRY_COMPARE(numeric->currentValue().toDouble(), .625);
            ui_->endpoint.setReady(true);
            ui_->endpoint.snapshotDeck(deck, {{"loaded", true}, {"replacing", false}});
            QTRY_VERIFY(dial->isEnabled());
        }
        // EQ belongs to the channel. Replacement snapshots may retain or update
        // targets, but neither a source change nor that ACK submits a command.
        ui_->endpoint.snapshotDeck(deck, {{"sourceId", "replacement-eq-source"}, {"sourceGeneration", qulonglong(2)}});
        ui_->endpoint.snapshotEqGain(deck, band, -13.);
        QTRY_COMPARE(numeric->currentValue().toDouble(), .25);
        QCOMPARE(ui_->endpoint.calls.size(), count);
        QCOMPARE(ui_->endpoint.decks()[1 - deck], accepted[1 - deck]);
        QCOMPARE(ui_->endpoint.masterGain(), master);
        QCOMPARE(ui_->endpoint.cueGain(), cue);
        QCOMPARE(ui_->endpoint.crossfader(), crossfader);
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void channelEqPointerAndKeyboardKeepHeldAcknowledgement_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<bool>("touch");
        QTest::addColumn<bool>("dark");
        for (int deck = 0; deck < 2; ++deck)
            for (bool touch : {false, true})
                for (bool dark : {false, true})
                    QTest::newRow(qPrintable(QString("deck%1-%2-%3").arg(deck).arg(touch ? "touch" : "mouse").arg(dark ? "dark" : "light")))
                        << deck << touch << dark;
    }

    void channelEqPointerAndKeyboardKeepHeldAcknowledgement()
    {
        QFETCH(int, deck); QFETCH(bool, touch); QFETCH(bool, dark);
        ui_->window->resize(1180, 760);
        QTRY_COMPARE(ui_->window->size(), QSize(1180, 760));
        verifyControlsFit({"themeToggle"});
        if (!dark) QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("themeToggle")));
        ui_->endpoint.loadState(0); ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        ui_->endpoint.setNumericAcknowledgement(false);
        auto* dial = ui_->item(QString("deck%1EqHI").arg(deck).toUtf8().constData());
        auto* peer = ui_->item(QString("deck%1EqHI").arg(1 - deck).toUtf8().constData());
        QVERIFY(dial && peer);
        QTRY_VERIFY(dial->isEnabled() && peer->isEnabled());
        verifyControlsFit({"deck0EqHI", "deck0EqMID", "deck0EqLOW", "deck1EqHI", "deck1EqMID", "deck1EqLOW"});
        const auto peerBefore = ui_->endpoint.decks()[1 - deck];
        const auto start = ui_->center(dial);
        const auto end = start - QPoint(0, std::max(24, QGuiApplication::styleHints()->startDragDistance() + 8));
        bool mouseHeld = false;
        const auto releaseMouse = qScopeGuard([&] {
            if (mouseHeld) QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, end);
        });
        auto contact = QTest::touchEvent(ui_->window, touchDevice_.get(), false);
        if (touch) { touchActive_ = true; contact.press(0, start, ui_->window).commit(); }
        else { mouseHeld = true; QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, start); }
        QTRY_VERIFY(dial->property("pressed").toBool());
        if (touch) contact.move(0, end, ui_->window).commit();
        else QTest::mouseMove(ui_->window, end);
        QTRY_VERIFY(!ui_->endpoint.calls.isEmpty());
        const auto held = dial->property("value").toDouble();
        QVERIFY(held > .5);
        const auto beforeAck = ui_->endpoint.calls.size();
        ui_->endpoint.snapshotEqGain(deck, 2, -13.);
        QTRY_COMPARE(dial->property("value").toDouble(), held);
        ui_->endpoint.snapshotEqGain(1 - deck, 2, 3.);
        QTRY_COMPARE(peer->property("value").toDouble(), .75);
        QCOMPARE(ui_->endpoint.calls.size(), beforeAck);
        if (touch) { contact.release(0, end, ui_->window).commit(); touchActive_ = false; }
        else { QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, end); mouseHeld = false; }
        QTRY_VERIFY(!dial->property("pressed").toBool());
        QTRY_COMPARE(dial->property("value").toDouble(), .25);
        for (const auto& entry : ui_->endpoint.calls) {
            const auto call = entry.toMap();
            QCOMPARE(call["action"].toString(), QString("eq"));
            QCOMPARE(call["deck"].toInt(), deck);
            QCOMPARE(call["band"].toInt(), 2);
            QVERIFY(call["value"].toDouble() >= -26. && call["value"].toDouble() <= 6.);
        }
        ui_->endpoint.snapshotEqGain(deck, 2, 0.);
        QTRY_COMPARE(dial->property("value").toDouble(), .5);
        auto count = ui_->endpoint.calls.size();
        dial->forceActiveFocus();
        QTRY_VERIFY(dial->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Right);
        ++count;
        QTRY_COMPARE(ui_->endpoint.calls.size(), count);
        QVERIFY(std::abs(ui_->endpoint.calls.last().toMap()["value"].toDouble() - .12) < 1e-9);
        QTRY_COMPARE(dial->property("value").toDouble(), .5);
        QTest::keyClick(ui_->window, Qt::Key_Home);
        ++count;
        QTRY_COMPARE(ui_->endpoint.calls.size(), count);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["value"].toDouble(), -26.);
        QTest::keyClick(ui_->window, Qt::Key_End);
        ++count;
        QTRY_COMPARE(ui_->endpoint.calls.size(), count);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["value"].toDouble(), 6.);
        QTRY_COMPARE(dial->property("value").toDouble(), .5);
        // Only the explicit peer snapshot above changes the other strip.
        auto expectedPeer = peerBefore.toMap();
        expectedPeer["eqGainDb"] = QVariantList{0., 0., 3.};
        QCOMPARE(ui_->endpoint.decks()[1 - deck].toMap(), expectedPeer);
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
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

    void existingControlsAccessibleNumericWaitsForAcknowledgement_data()
    {
        QTest::addColumn<QString>("objectName");
        QTest::addColumn<QString>("action");
        QTest::addColumn<int>("deck");
        QTest::addColumn<int>("stem");
        QTest::newRow("crossfader") << QString("crossfader") << QString("crossfader") << -1 << -1;
        QTest::newRow("master") << QString("masterGain") << QString("master") << -1 << -1;
        QTest::newRow("headphone") << QString("cueGain") << QString("cueGain") << -1 << -1;
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            QTest::newRow(qPrintable(prefix + "-gain")) << prefix + "Gain" << QString("gain") << deck << -1;
            QTest::newRow(qPrintable(prefix + "-seek")) << prefix + "Seek" << QString("seek") << deck << -1;
            for (int stem = 0; stem < 4; ++stem)
                QTest::newRow(qPrintable(prefix + QString("-stem%1").arg(stem)))
                    << prefix + QString("Stem%1Gain").arg(stem) << QString("stemGain") << deck << stem;
        }
    }

    void existingControlsAccessibleNumericWaitsForAcknowledgement()
    {
        QFETCH(QString, objectName); QFETCH(QString, action); QFETCH(int, deck); QFETCH(int, stem);
        ui_->endpoint.loadState(0); ui_->endpoint.loadState(1);
        ui_->endpoint.setOverviewStem(0, "drums"); ui_->endpoint.setOverviewStem(1, "drums");
        ui_->endpoint.setReady(true);
        ui_->endpoint.setNumericAcknowledgement(false);
        auto* control = ui_->item(objectName.toUtf8().constData());
        QVERIFY(control);
        QTRY_VERIFY(control->isEnabled());
        auto* accessible = QAccessible::queryAccessibleInterface(control);
        QVERIFY(accessible && accessible->valueInterface());
        auto* numeric = accessible->valueInterface();
        QVERIFY(numeric->minimumValue().isValid());
        QVERIFY(numeric->maximumValue().isValid());
        QCOMPARE(numeric->minimumValue().toDouble(), 0.0);
        QCOMPARE(numeric->maximumValue().toDouble(), action == "seek" ? 10.0 : 1.0);
        const auto acceptedValue = numeric->currentValue().toDouble();
        const auto acceptedDecks = ui_->endpoint.decks();
        const auto master = ui_->endpoint.masterGain(), cue = ui_->endpoint.cueGain(), crossfader = ui_->endpoint.crossfader();
        const double requested = action == "seek" ? 4.0 : .75;
        const double acknowledged = action == "seek" ? 6.25 : .35;
        auto publish = [&](double value) {
            if (action == "seek") ui_->endpoint.snapshotPosition(deck, value);
            else if (action == "gain") ui_->endpoint.snapshotDeck(deck, {{"gain", value}});
            else if (action == "stemGain") ui_->endpoint.snapshotStemGain(deck, stem, value);
            else if (action == "master") ui_->endpoint.snapshotMasterGain(value);
            else if (action == "cueGain") ui_->endpoint.snapshotCueGain(value);
            else ui_->endpoint.snapshotCrossfader(value);
        };
        QVERIFY(ui_->endpoint.calls.isEmpty()); // Initial snapshot binding is not an input request.
        numeric->setCurrentValue(requested); // Actual Qt value setter, not a moved signal or QML function call.
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto call = ui_->endpoint.calls[0].toMap();
        QCOMPARE(call["action"].toString(), action);
        QCOMPARE(call["value"].toDouble(), requested);
        if (deck >= 0) QCOMPARE(call["deck"].toInt(), deck);
        if (stem >= 0) QCOMPARE(call["stem"].toInt(), stem);
        QCOMPARE(ui_->endpoint.decks(), acceptedDecks);
        QCOMPARE(ui_->endpoint.masterGain(), master);
        QCOMPARE(ui_->endpoint.cueGain(), cue);
        QCOMPARE(ui_->endpoint.crossfader(), crossfader);
        QTRY_COMPARE(numeric->currentValue().toDouble(), acceptedValue);
        QCoreApplication::processEvents();
        QCOMPARE(ui_->endpoint.calls.size(), 1); // Restoration must not submit a second command.

        publish(acknowledged); // Only a later engine snapshot changes the displayed accepted value.
        QTRY_COMPARE(numeric->currentValue().toDouble(), acknowledged);
        QCOMPARE(ui_->endpoint.calls.size(), 1);
        const auto acknowledgedDecks = ui_->endpoint.decks();
        const double nextRequest = action == "seek" ? 3.0 : .65;
        numeric->setCurrentValue(nextRequest);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        const auto next = ui_->endpoint.calls[1].toMap();
        QCOMPARE(next["action"].toString(), action);
        QCOMPARE(next["value"].toDouble(), nextRequest);
        if (deck >= 0) QCOMPARE(next["deck"].toInt(), deck);
        if (stem >= 0) QCOMPARE(next["stem"].toInt(), stem);
        QCOMPARE(ui_->endpoint.decks(), acknowledgedDecks);
        QTRY_COMPARE(numeric->currentValue().toDouble(), acknowledged);

        // Global master/cue/crossfader controls deliberately do not require a
        // loaded deck or stop during replacement. Per-deck controls do.
        QStringList gates{"output-unready"};
        if (deck >= 0) gates << "unloaded" << "replacing";
        for (const auto& gate : gates) {
            if (gate == "output-unready") ui_->endpoint.setReady(false);
            else if (gate == "unloaded") ui_->endpoint.snapshotDeck(deck, {{"loaded", false}});
            else ui_->endpoint.setReplacing(deck, true);
            QTRY_VERIFY(!control->isEnabled());
            const auto guardedDecks = ui_->endpoint.decks();
            const auto guardedMaster = ui_->endpoint.masterGain(), guardedCue = ui_->endpoint.cueGain(),
                       guardedCrossfader = ui_->endpoint.crossfader();
            numeric->setCurrentValue(action == "seek" ? 4.5 : .8);
            QCoreApplication::processEvents();
            QCOMPARE(ui_->endpoint.calls.size(), 2);
            QTRY_COMPARE(numeric->currentValue().toDouble(), acknowledged);
            QCOMPARE(ui_->endpoint.decks(), guardedDecks);
            QCOMPARE(ui_->endpoint.masterGain(), guardedMaster);
            QCOMPARE(ui_->endpoint.cueGain(), guardedCue);
            QCOMPARE(ui_->endpoint.crossfader(), guardedCrossfader);
            ui_->endpoint.setReady(true);
            if (deck >= 0) ui_->endpoint.snapshotDeck(deck, {{"loaded", true}, {"replacing", false}});
            QTRY_VERIFY(control->isEnabled());
            QCOMPARE(ui_->endpoint.calls.size(), 2);
        }
        publish(action == "seek" ? 5.5 : .45); // Binding remains alive after rejected direct writes.
        QTRY_COMPARE(numeric->currentValue().toDouble(), action == "seek" ? 5.5 : .45);
        QCOMPARE(ui_->endpoint.calls.size(), 2);
        if (action == "seek") {
            // A shorter replacement clamps the old range. That metadata
            // change must not become an input command for the new source.
            ui_->endpoint.snapshotDeck(deck, {{"sourceId", "shorter-accessible-source"},
                {"durationSeconds", 2.0}, {"positionSeconds", 1.25}});
            QTRY_COMPARE(numeric->currentValue().toDouble(), 1.25);
            QCOMPARE(numeric->maximumValue().toDouble(), 2.0);
            QCOMPARE(ui_->endpoint.calls.size(), 2);
        }
        if (deck >= 0) QCOMPARE(ui_->endpoint.decks()[1 - deck], acceptedDecks[1 - deck]);
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
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

    void localTimingDisplayDoesNotSubmitTempoOrChangeReference()
    {
        ui_->endpoint.setReady(true);
        for (int deck = 0; deck < 2; ++deck) {
            ui_->endpoint.loadState(deck);
            auto manual = UiEndpoint::bpmState(128);
            manual["manual"] = true;
            auto tempo = UiEndpoint::tempoState(true);
            tempo["active"] = true; tempo["percent"] = 6.; tempo["keyLock"] = true;
            ui_->endpoint.snapshotBpm(deck, manual);
            ui_->endpoint.snapshotTempo(deck, tempo);
            auto local = QVariantMap{{"status", "Estimated"}, {"confidence", "Unknown"}, {"bpm", 160.},
                {"positionKnown", true}, {"sourceFrame", qulonglong(48000)},
                {"firstFrame", qulonglong(48000)}, {"endFrame", qulonglong(66000)}};
            ui_->endpoint.snapshotDeck(deck, {{"localTiming", local}});
            auto* label = ui_->item(QString("deck%1LocalTempo").arg(deck).toUtf8().constData());
            auto* rail = ui_->item(QString("deck%1Tempo").arg(deck).toUtf8().constData());
            QVERIFY(label && rail);
            QTRY_COMPARE(label->property("text").toString(), QString("LOCAL SOURCE EST. 160.0 BPM"));
            QTRY_VERIFY(std::abs(rail->property("value").toDouble() - 135.68) < 1e-8);
            const auto before = ui_->endpoint.calls.size();
            local["bpm"] = 180.;
            ui_->endpoint.snapshotDeck(deck, {{"localTiming", local}, {"playing", true}});
            QTRY_COMPARE(label->property("text").toString(), QString("LOCAL SOURCE EST. 180.0 BPM"));
            ui_->endpoint.snapshotDeck(deck, {{"scratch", QVariantMap{{"active", true}}}, {"playing", false}});
            QTRY_COMPARE(label->property("text").toString(), QString("LOCAL SOURCE EST. 180.0 BPM"));
            local["status"] = "Unknown"; local["bpm"] = 0.;
            ui_->endpoint.snapshotDeck(deck, {{"localTiming", local}});
            QTRY_COMPARE(label->property("text").toString(), QString("LOCAL UNKNOWN"));
            // Replacement preparation and unloading never display retained old analysis.
            local["status"] = "Estimated"; local["bpm"] = 160.;
            ui_->endpoint.snapshotDeck(deck, {{"loading", true}, {"localTiming", local}});
            QTRY_COMPARE(label->property("text").toString(), QString("LOCAL PREPARING"));
            ui_->endpoint.snapshotDeck(deck, {{"loading", false}, {"loaded", false}});
            QTRY_COMPARE(label->property("text").toString(), QString("LOCAL UNKNOWN"));
            QCOMPARE(ui_->endpoint.decks()[deck].toMap()["bpm"].toMap(), manual);
            QCOMPARE(ui_->endpoint.decks()[deck].toMap()["tempo"].toMap(), tempo);
            QCOMPARE(ui_->endpoint.calls.size(), before);
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

    void touchSeekCancellationRestoresAcknowledgedPosition_data()
    {
        QTest::addColumn<int>("deck");
        QTest::addColumn<bool>("platformCancel");
        QTest::newRow("deck-a-grab-loss") << 0 << false;
        QTest::newRow("deck-b-grab-loss") << 1 << false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        QTest::newRow("deck-a-platform-cancel") << 0 << true;
        QTest::newRow("deck-b-platform-cancel") << 1 << true;
#endif
    }

    void touchSeekCancellationRestoresAcknowledgedPosition()
    {
        QFETCH(int, deck);
        QFETCH(bool, platformCancel);
        ui_->window->resize(1180, 760);
        QTRY_COMPARE(ui_->window->size(), QSize(1180, 760));
        ui_->endpoint.loadState(0, 1.0);
        ui_->endpoint.loadState(1, 1.0);
        ui_->endpoint.setReady(true);
        auto* seek = ui_->item(QString("deck%1Seek").arg(deck).toUtf8().constData());
        auto* peer = ui_->item(QString("deck%1Seek").arg(1 - deck).toUtf8().constData());
        auto* input = ui_->item(QString("deck%1SeekInput").arg(deck).toUtf8().constData());
        QVERIFY(seek && peer && input && seek->isEnabled() && peer->isEnabled());
        QSignalSpy polishedFrame(ui_->window, &QQuickWindow::frameSwapped);
        seek->polish();
        peer->polish();
        ui_->window->update();
        QTRY_VERIFY(polishedFrame.count() > 0);
        const auto peerState = ui_->endpoint.decks()[1 - deck];
        const auto start = seek->mapToScene(QPointF(seek->width() * .25, seek->height() / 2)).toPoint();
        const auto end = seek->mapToScene(QPointF(seek->width() * .75, seek->height() / 2)).toPoint();
        auto touch = QTest::touchEvent(ui_->window, touchDevice_.get(), false);
        touchActive_ = true;
        touch.press(0, start, ui_->window).commit();
        QTRY_VERIFY(seek->property("pressed").toBool());
        touch.move(0, end, ui_->window).commit();
        QTRY_VERIFY(seek->property("value").toDouble() > 5.0);
        QVERIFY(ui_->endpoint.calls.isEmpty());
        ui_->endpoint.snapshotPosition(deck, 2.5);
        QVERIFY(seek->property("value").toDouble() > 5.0); // A held preview ignores telemetry.

        if (platformCancel) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
            // This clears Qt's platform input state as well as the sequence's
            // remembered points. A real cancel is followed by a fresh press.
            QVERIFY(touch.cancel());
            touchActive_ = false;
#else
            QFAIL("Platform-cancel rows require Qt6.12 or later");
#endif
        } else {
            // MouseArea owns Qt's synthesized single-touch mouse grab. Losing
            // that actual grab must cancel; the contact is still down.
            input->ungrabMouse();
        }
        QTRY_VERIFY(!input->property("pressed").toBool());
        QTRY_VERIFY(!seek->property("pressed").toBool());
        QCOMPARE(ui_->endpoint.calls.size(), 0);
        QTRY_COMPARE(seek->property("value").toDouble(), 2.5);
        QCOMPARE(ui_->endpoint.decks()[1 - deck], peerState);
        QCOMPARE(peer->property("value").toDouble(), 1.0);
        if (!platformCancel) {
            // Release the still-active contact after grab loss. It must not
            // commit the invalid preview or affect the other deck.
            touch.release(0, end, ui_->window).commit();
            touchActive_ = false;
        }
        QCOMPARE(ui_->endpoint.calls.size(), 0);

        touchActive_ = true;
        touch.press(0, start, ui_->window).commit();
        QTRY_VERIFY(seek->property("pressed").toBool());
        touch.move(0, end, ui_->window).commit();
        QVERIFY(ui_->endpoint.calls.isEmpty());
        touch.release(0, end, ui_->window).commit();
        touchActive_ = false;
        QTRY_VERIFY(!seek->property("pressed").toBool());
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        verifyCueCall(0, "seek", deck);
        const auto accepted = ui_->endpoint.calls[0].toMap()["value"].toDouble();
        QVERIFY(accepted > 5.0);
        QCOMPARE(seek->property("value").toDouble(), accepted);
        QCOMPARE(ui_->endpoint.decks()[1 - deck], peerState);
        QCOMPARE(peer->property("value").toDouble(), 1.0);
    }

    void touchChannelGainKeepsHeldValueUntilRelease_data()
    {
        QTest::addColumn<int>("deck");
        QTest::newRow("deck-a") << 0;
        QTest::newRow("deck-b") << 1;
    }

    void touchChannelGainKeepsHeldValueUntilRelease()
    {
        QFETCH(int, deck);
        ui_->window->resize(1180, 760);
        QTRY_COMPARE(ui_->window->size(), QSize(1180, 760));
        ui_->endpoint.loadState(0);
        ui_->endpoint.loadState(1);
        ui_->endpoint.setReady(true);
        auto* gain = ui_->item(QString("deck%1Gain").arg(deck).toUtf8().constData());
        auto* peer = ui_->item(QString("deck%1Gain").arg(1 - deck).toUtf8().constData());
        QVERIFY(gain && peer && gain->isEnabled() && peer->isEnabled());
        auto* handle = qvariant_cast<QQuickItem*>(gain->property("handle"));
        QVERIFY(handle);
        QSignalSpy polishedFrame(ui_->window, &QQuickWindow::frameSwapped);
        gain->polish();
        handle->polish();
        ui_->window->update();
        QTRY_VERIFY(polishedFrame.count() > 0);
        QVERIFY(handle->width() > 0 && handle->height() > 0);
        const auto localStart = handle->mapToItem(gain, QPointF(handle->width() / 2, handle->height() / 2));
        const auto configuredThreshold = gain->property("touchDragThreshold").toDouble();
        const auto threshold = configuredThreshold < 0 ? QGuiApplication::styleHints()->startDragDistance()
                                                      : qRound(configuredThreshold);
        QVERIFY2(localStart.y() > threshold + 2, "Fader handle has insufficient room for a native upward touch drag");
        const auto start = gain->mapToScene(localStart).toPoint();
        const auto first = gain->mapToScene(QPointF(localStart.x(), localStart.y() - threshold - 2)).toPoint();
        const auto end = gain->mapToScene(QPointF(localStart.x(),
            std::max(0.0, localStart.y() - threshold - 2 - std::max(8, threshold)))).toPoint();
        const auto gestureState = [&] {
            return QString("value=%1 pressed=%2 keepTouchGrab=%3 size=%4x%5 threshold=%6 start=(%7,%8) first=(%9,%10) end=(%11,%12)")
                .arg(gain->property("value").toDouble()).arg(gain->property("pressed").toBool())
                .arg(gain->keepTouchGrab()).arg(gain->width()).arg(gain->height()).arg(threshold)
                .arg(start.x()).arg(start.y()).arg(first.x()).arg(first.y()).arg(end.x()).arg(end.y());
        };
        auto touch = QTest::touchEvent(ui_->window, touchDevice_.get(), false);
        touchActive_ = true;
        touch.press(0, start, ui_->window).commit();
        QTRY_VERIFY(gain->property("pressed").toBool());
        touch.move(0, first, ui_->window).commit();
        QTRY_VERIFY2_WITH_TIMEOUT(gain->keepTouchGrab(), qPrintable(gestureState()), 3000);
        const auto firstValue = gain->property("value").toDouble();
        touch.move(0, end, ui_->window).commit();
        QTRY_VERIFY2_WITH_TIMEOUT(gain->property("value").toDouble() > std::max(.5, firstValue),
                                 qPrintable(gestureState()), 3000);
        QVERIFY(!ui_->endpoint.calls.isEmpty());
        const auto held = gain->property("value").toDouble();
        const auto count = ui_->endpoint.calls.size();
        for (const auto& value : ui_->endpoint.calls) {
            QCOMPARE(value.toMap()["action"].toString(), QString("gain"));
            QCOMPARE(value.toMap()["deck"].toInt(), deck);
        }
        ui_->endpoint.snapshotDeck(deck, {{"gain", .13}});
        QVERIFY(std::abs(gain->property("value").toDouble() - held) < 1e-6);
        ui_->endpoint.snapshotDeck(1 - deck, {{"gain", .8}});
        QTRY_COMPARE(peer->property("value").toDouble(), .8);
        QVERIFY(std::abs(gain->property("value").toDouble() - held) < 1e-6);
        QCOMPARE(ui_->endpoint.calls.size(), count); // Snapshots cannot issue gain commands.
        touch.release(0, end, ui_->window).commit();
        touchActive_ = false;
        QTRY_VERIFY(!gain->property("pressed").toBool());
        // Native Slider release may reassert the final finger value before it
        // clears pressed. That real input supersedes the earlier telemetry ACK.
        const auto releasedCount = ui_->endpoint.calls.size();
        QVERIFY(releasedCount >= count && releasedCount <= count + 1);
        if (releasedCount == count + 1) {
            const auto call = ui_->endpoint.calls.last().toMap();
            QCOMPARE(call["action"].toString(), QString("gain"));
            QCOMPARE(call["deck"].toInt(), deck);
            QVERIFY(std::abs(call["value"].toDouble() - held) < 1e-6);
            QCOMPARE(ui_->endpoint.decks()[deck].toMap()["gain"].toDouble(), call["value"].toDouble());
        } else {
            QCOMPARE(ui_->endpoint.decks()[deck].toMap()["gain"].toDouble(), .13);
        }
        QTRY_COMPARE(gain->property("value").toDouble(), ui_->endpoint.decks()[deck].toMap()["gain"].toDouble());
        ui_->endpoint.snapshotDeck(deck, {{"gain", .21}});
        QTRY_COMPARE(gain->property("value").toDouble(), .21);
        QCOMPARE(ui_->endpoint.calls.size(), releasedCount); // A fresh ACK is never an input command.
        QCOMPARE(ui_->endpoint.decks()[1 - deck].toMap()["gain"].toDouble(), .8);
        QCOMPARE(peer->property("value").toDouble(), .8);
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

        // Live level updates must update the existing channels rather than
        // destroying/recreating their scene objects at each audio snapshot.
        QObject* channels = nullptr;
        for (auto* child : masterMeter->findChildren<QObject*>())
            if (child->inherits("QQuickRepeater")) { channels = child; break; }
        QVERIFY(channels);
        QQuickItem* left = nullptr;
        QQuickItem* right = nullptr;
        QVERIFY(QMetaObject::invokeMethod(channels, "itemAt", Q_RETURN_ARG(QQuickItem*, left), Q_ARG(int, 0)));
        QVERIFY(QMetaObject::invokeMethod(channels, "itemAt", Q_RETURN_ARG(QQuickItem*, right), Q_ARG(int, 1)));
        QVERIFY(left && right);
        const QPointer<QQuickItem> originalLeft(left), originalRight(right);
        QVERIFY(left->childItems().size() >= 3 && right->childItems().size() >= 3);
        QTRY_COMPARE(left->childItems()[0]->height(), left->height()); // Overrange peak clamps at the top.
        QVERIFY(right->childItems()[0]->height() > 0 && right->childItems()[0]->height() < right->height());
        QVERIFY(left->childItems()[1]->height() > right->childItems()[1]->height()); // Independent RMS levels.

        ui_->endpoint.setMeters(UiEndpoint::silentMeter(), UiEndpoint::silentMeter(), UiEndpoint::silentMeter());
        QTRY_COMPARE(masterMeter->property("peakLeft").toDouble(), 0.0);
        QVERIFY(!masterMeter->property("overrange").toBool());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY2(originalLeft && originalRight, "A level snapshot recreated the meter channels");
        QQuickItem* updatedLeft = nullptr;
        QQuickItem* updatedRight = nullptr;
        QVERIFY(QMetaObject::invokeMethod(channels, "itemAt", Q_RETURN_ARG(QQuickItem*, updatedLeft), Q_ARG(int, 0)));
        QVERIFY(QMetaObject::invokeMethod(channels, "itemAt", Q_RETURN_ARG(QQuickItem*, updatedRight), Q_ARG(int, 1)));
        QCOMPARE(updatedLeft, originalLeft.data());
        QCOMPARE(updatedRight, originalRight.data());
        QTRY_COMPARE(updatedLeft->childItems()[0]->height(), 0.0);
        QTRY_COMPARE(updatedRight->childItems()[0]->height(), 0.0);
        QCOMPARE(updatedLeft->childItems()[1]->height(), 0.0);
        QCOMPARE(updatedRight->childItems()[1]->height(), 0.0);
        QVERIFY(!updatedLeft->childItems()[2]->isVisible() && !updatedRight->childItems()[2]->isVisible());
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

    void recordingHeaderIndicatorIsCentered_data()
    {
        QTest::addColumn<QSize>("viewport");
        QTest::addColumn<bool>("dark");
        for (const auto viewport : {QSize(1180, 760), QSize(1280, 800)})
            for (const bool dark : {false, true})
                QTest::newRow(qPrintable(QString("%1x%2-%3").arg(viewport.width()).arg(viewport.height())
                                        .arg(dark ? "dark" : "light"))) << viewport << dark;
    }

    void recordingHeaderIndicatorIsCentered()
    {
        QFETCH(QSize, viewport);
        QFETCH(bool, dark);
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        auto* theme = ui_->item("themeToggle");
        QTRY_VERIFY(ui_->center(theme).x() < viewport.width());
        if (!dark) QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(theme));
        QTRY_COMPARE(theme->property("text").toString(), dark ? QString("LIGHT") : QString("DARK"));
        auto* button = ui_->item("audioSettingsOpen");
        auto* label = ui_->item("audioSettingsRecordingLabel");
        auto* dot = ui_->item("audioSettingsRecordingIndicator");
        QVERIFY(button && label && dot);
        QCOMPARE(button->property("text").toString(), QString("AUDIO / REC"));
        QVERIFY(!dot->isVisible());
        ui_->endpoint.setRecording("Recording", true);
        QTRY_VERIFY(dot->isVisible());
        // Row repositions a newly visible child during the next polish pass.
        QTRY_VERIFY(dot->mapRectToScene(QRectF(0, 0, dot->width(), dot->height())).left()
                    > label->mapRectToScene(QRectF(0, 0, label->width(), label->height())).right());
        QTRY_VERIFY(std::abs(dot->mapRectToScene(QRectF(0, 0, dot->width(), dot->height())).center().y()
                            - label->mapRectToScene(QRectF(0, 0, label->width(), label->height())).center().y()) <= .5);
        QCOMPARE(dot->width(), dot->height());
        QVERIFY(dot->width() > 0);
        QCOMPARE(dot->property("radius").toDouble(), dot->width() / 2);
        const auto dotRect = dot->mapRectToScene(QRectF(0, 0, dot->width(), dot->height()));
        const auto labelRect = label->mapRectToScene(QRectF(0, 0, label->width(), label->height()));
        const auto buttonRect = button->mapRectToScene(QRectF(0, 0, button->width(), button->height()));
        QVERIFY(buttonRect.contains(dotRect));
        QVERIFY(buttonRect.contains(labelRect));
        QVERIFY(dotRect.left() > labelRect.right());
        QVERIFY(std::abs(dotRect.center().y() - buttonRect.center().y()) <= .5);
        auto* accessible = QAccessible::queryAccessibleInterface(button);
        QVERIFY(accessible);
        QVERIFY(accessible->text(QAccessible::Description).contains("recording is active"));
        ui_->endpoint.setRecording("Finalizing", true);
        QVERIFY(dot->isVisible());
        ui_->endpoint.setRecording("Complete", false);
        QTRY_VERIFY(!dot->isVisible());
        QCOMPARE(button->property("text").toString(), QString("AUDIO / REC"));
        QVERIFY(ui_->endpoint.calls.isEmpty());
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

    void recoveryRangeRequiresFreshConsentAndStaysBoundToSelectedSource()
    {
        QVERIFY(ui_->openLibrary(0));
        auto* range = ui_->item("libraryRecoveryRange");
        QVERIFY(range && !range->isVisible());
        ui_->endpoint.setStemsAvailable(true);
        ui_->endpoint.setResidentHoldAvailable(true);
        const QString id(64, QLatin1Char('b')), other(64, QLatin1Char('c'));
        ui_->endpoint.setLibrary({
            QVariantMap{{"id", id}, {"name", "Approved source"}, {"path", "/tmp/one.wav"},
                        {"frames", 48000 * 60}, {"durationSeconds", 60.0}, {"available", true}},
            QVariantMap{{"id", other}, {"name", "Different source"}, {"path", "/tmp/two.wav"},
                        {"frames", 48000 * 60}, {"durationSeconds", 60.0}, {"available", true}}});
        QTRY_VERIFY(ui_->item("libraryTrack0"));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack0")));
        auto* stems = ui_->item("libraryPreparedStems");
        QVERIFY(stems); QTRY_VERIFY(stems->isVisible() && stems->isEnabled());
        stems->forceActiveFocus();
        QTRY_VERIFY(stems->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_VERIFY(stems->property("checked").toBool());
        QTRY_VERIFY(range->isVisible() && range->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(range));
        auto* dialog = ui_->object("libraryRecoveryRangeDialog");
        auto* start = ui_->item("libraryRecoveryStart");
        auto* end = ui_->item("libraryRecoveryEnd");
        auto* consent = ui_->item("libraryRecoveryConsent");
        auto* approve = ui_->item("libraryRecoveryApprove");
        QVERIFY(dialog && start && end && consent && approve);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(start->property("text").toString().isEmpty() && end->property("text").toString().isEmpty());
        QVERIFY(!consent->property("checked").toBool() && !approve->isEnabled());
        start->setProperty("text", "1.25"); end->setProperty("text", "2.75");
        QTRY_VERIFY(consent->isEnabled());
        QVERIFY(!approve->isEnabled());
        auto* accessible = QAccessible::queryAccessibleInterface(consent);
        QVERIFY(accessible && !accessible->text(QAccessible::Name).isEmpty());
        consent->forceActiveFocus(); QTRY_VERIFY(consent->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_VERIFY(approve->isEnabled());
        approve->forceActiveFocus(); QTRY_VERIFY(approve->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(ui_->endpoint.calls.isEmpty());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck0Load")));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        const auto approved = ui_->endpoint.calls.last().toMap();
        QCOMPARE(approved["action"].toString(), QString("libraryStemRangeLoad"));
        QCOMPARE(approved["id"].toString(), id); QCOMPARE(approved["deck"].toInt(), 0);
        QCOMPARE(approved["inSeconds"].toDouble(), 1.25); QCOMPARE(approved["outSeconds"].toDouble(), 2.75);

        // Opening the editor cannot reuse the checkbox. An actual text edit
        // also revokes the previously approved load intent immediately.
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(range));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(!consent->property("checked").toBool());
        end->forceActiveFocus(); QTRY_VERIFY(end->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(ui_->window, Qt::Key_Backspace); end->setProperty("text", "2.8");
        QTRY_VERIFY(!consent->property("checked").toBool() && !approve->isEnabled());
        QTest::keyClick(ui_->window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck0Load")));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QCOMPARE(ui_->endpoint.calls.last().toMap()["action"].toString(), QString("libraryStemLoad"));

        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(range));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(start->property("text").toString().isEmpty() && end->property("text").toString().isEmpty());
        start->setProperty("text", "1.25"); end->setProperty("text", "2.75");
        QTRY_VERIFY(consent->isEnabled());
        consent->forceActiveFocus(); QTRY_VERIFY(consent->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_VERIFY(approve->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(approve));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("libraryTrack1")));
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(ui_->item("deck1Load")));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        const auto changed = ui_->endpoint.calls.last().toMap();
        QCOMPARE(changed["action"].toString(), QString("libraryStemLoad"));
        QCOMPARE(changed["id"].toString(), other); QCOMPARE(changed["deck"].toInt(), 1);
        QVERIFY(ui_->closeLibrary());
        QVERIFY(ui_->warnings.isEmpty());
    }

    void recoveryControlsRouteExactTicketsAndCloseOnSourceChange()
    {
        const QString id(64, QLatin1Char('b'));
        const QString outputSession("9007199254740997"), rangeId("18446744073709551613");
        const QString generation("9007199254740993"), revision("18446744073709551614");
        ui_->endpoint.loadState(0); ui_->endpoint.setReady(true);
        QVariantMap hold{{"configured", true}, {"ready", true}, {"armed", false}, {"active", false},
                         {"exitPending", false}, {"status", "Cancelled"}, {"text", "Recovery off"},
                         {"detail", "Only the approved section may repeat."}, {"inSeconds", 1.25},
                         {"outSeconds", 2.75}, {"outputSession", outputSession}, {"rangeId", rangeId},
                         {"sourceGeneration", generation},
                         {"transportRevision", revision}, {"errorCode", 0}};
        ui_->endpoint.snapshotDeck(0, {{"sourceId", id}, {"liveStems", true}, {"residentHold", hold}});
        auto* open = ui_->item("deck0Recovery");
        QVERIFY(open); QTRY_VERIFY(open->isEnabled());
        open->forceActiveFocus(); QTRY_VERIFY(open->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        auto* dialog = ui_->object("deck0RecoveryDialog");
        QVERIFY(dialog); QTRY_VERIFY(dialog->property("visible").toBool());
        auto* arm = ui_->item("deck0RecoveryArm");
        auto* cancel = ui_->item("deck0RecoveryCancel");
        auto* exit = ui_->item("deck0RecoveryExit");
        QVERIFY(arm && cancel && exit); QVERIFY(arm->isEnabled() && cancel->isEnabled() && !exit->isEnabled());
        arm->forceActiveFocus(); QTRY_VERIFY(arm->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Space);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        hold["active"] = true; hold["armed"] = true; hold["status"] = "Active";
        ui_->endpoint.snapshotDeck(0, {{"residentHold", hold}});
        QTRY_VERIFY(!arm->isEnabled() && exit->isEnabled());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(exit));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cancel));
        QTRY_COMPARE(ui_->endpoint.calls.size(), 3);
        const QStringList actions{"residentArm", "residentExit", "residentCancel"};
        for (qsizetype index = 0; index < ui_->endpoint.calls.size(); ++index) {
            const auto call = ui_->endpoint.calls[index].toMap();
            QCOMPARE(call["action"].toString(), actions[index]);
            QCOMPARE(call["deck"].toInt(), 0); QCOMPARE(call["id"].toString(), id);
            QCOMPARE(call["outputSession"].toString(), outputSession); QCOMPARE(call["rangeId"].toString(), rangeId);
            QCOMPARE(call["generation"].toString(), generation); QCOMPARE(call["revision"].toString(), revision);
        }
        // Values outside JavaScript's exact integer range must remain strings;
        // output restart or a new range on the same SHA also invalidates an
        // already open action panel, even when generation/revision are reused.
        hold["outputSession"] = "9007199254740998";
        ui_->endpoint.snapshotDeck(0, {{"residentHold", hold}});
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(open));
        QTRY_VERIFY(dialog->property("visible").toBool());
        hold["rangeId"] = "18446744073709551614";
        ui_->endpoint.snapshotDeck(0, {{"residentHold", hold}});
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(open));
        QTRY_VERIFY(dialog->property("visible").toBool());
        hold["sourceGeneration"] = "9007199254740994";
        ui_->endpoint.snapshotDeck(0, {{"residentHold", hold}});
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(open));
        QTRY_VERIFY(dialog->property("visible").toBool());
        ui_->endpoint.setSourceId(0, QString(64, QLatin1Char('c')));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(ui_->endpoint.calls.size(), 3);
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

    void savedContinuationKeepsSourceWaveformAndRestoresVinylControl()
    {
        ui_->endpoint.loadState(0, 5); ui_->endpoint.setReady(true);
        ui_->endpoint.setOverviewStem(0, "source");
        auto* kind = ui_->item("deck0WaveformKind");
        auto* title = ui_->item("deck0Title");
        auto* jog = ui_->item("deck0Jog");
        QVERIFY(kind && title && jog);
        const auto titleBefore = title->property("text");
        const auto waveformBefore = ui_->endpoint.decks()[0].toMap()["waveform"];
        ui_->endpoint.snapshotDeck(0, {{"continuation", QVariantMap{
            {"status", "Checking"}, {"text", "Checking saved stems"}, {"detail", ""}}}});
        QTRY_COMPARE(kind->property("text").toString(), QString("CHECKING SAVED STEMS · SOURCE WAVEFORM"));
        QVERIFY(!ui_->endpoint.busy());
        QCOMPARE(title->property("text"), titleBefore);
        ui_->endpoint.snapshotDeck(0, {{"liveStems", false}, {"sourceGeneration", qulonglong(2)},
            {"continuation", QVariantMap{{"status", "Active"}, {"text", "Using saved stems"}, {"detail", ""}}}});
        QTRY_COMPARE(kind->property("text").toString(), QString("USING SAVED STEMS · SOURCE WAVEFORM"));
        QCOMPARE(ui_->endpoint.decks()[0].toMap()["waveform"], waveformBefore);
        QCOMPARE(title->property("text"), titleBefore);
        QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
        jog->polish(); ui_->window->update(); QTRY_VERIFY(polished.count() > 0);
        const auto point = jog->mapToScene(QPointF(jog->width() * .85, jog->height() / 2)).toPoint();
        ui_->endpoint.calls.clear();
        QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        verifyCueCall(0, "scratchBegin", 0);
        QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 2);
        verifyCueCall(1, "scratchEnd", 0);
        // Cache/source errors can contain markup-like filenames: show literal detail.
        const QString detail = "No bank for <b>track</b>.";
        ui_->endpoint.snapshotDeck(0, {{"liveStems", true}, {"continuation", QVariantMap{
            {"status", "Unavailable"}, {"text", "Saved stem continuation unavailable"}, {"detail", detail}}}});
        QTRY_VERIFY(kind->property("text").toString().startsWith("SAVED STEM CONTINUATION UNAVAILABLE"));
        QTest::mouseMove(ui_->window, ui_->center(kind));
        QTRY_VERIFY(ui_->item("deck0ContinuationTooltipText"));
        auto* tooltip = ui_->item("deck0ContinuationTooltipText");
        QTRY_VERIFY(tooltip->isVisible());
        QTRY_COMPARE(tooltip->property("text").toString(), detail);
        QCOMPARE(tooltip->property("textFormat").toInt(), int(Qt::PlainText));
        auto* accessible = QAccessible::queryAccessibleInterface(kind);
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Description), detail);
        QCOMPARE(accessible->text(QAccessible::Name), QString("Deck A: ") + kind->property("text").toString());
        QVERIFY(ui_->warnings.isEmpty());
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

    void deckSurfacesGrowAndRemainUsableAcrossResize_data()
    {
        QTest::addColumn<QSize>("viewport");
        QTest::addColumn<bool>("dark");
        for (const auto viewport : {QSize(2560, 1600), QSize(1920, 1080), QSize(1180, 760),
                                    QSize(2560, 900), QSize(1920, 1600)})
            for (const bool dark : {false, true})
                QTest::newRow(qPrintable(QString("%1x%2-%3").arg(viewport.width()).arg(viewport.height()).arg(dark ? "dark" : "light")))
                    << viewport << dark;
    }

    void deckSurfacesGrowAndRemainUsableAcrossResize()
    {
        QFETCH(QSize, viewport);
        QFETCH(bool, dark);
        const QSize initialSize(1680, 1020);
        QTRY_COMPARE(ui_->window->size(), initialSize);
        auto* theme = ui_->item("themeToggle");
        QVERIFY(theme);
        if (!dark)
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(theme));
        QTRY_COMPARE(theme->property("text").toString(), dark ? QString("LIGHT") : QString("DARK"));
        ui_->endpoint.loadState(0, 5.0);
        ui_->endpoint.loadState(1, 5.0);
        ui_->endpoint.setReady(true);
        std::array<qreal, 2> initialJog{}, initialDisplay{}, initialSeek{};
        QStringList outerControls{"deck0Gain", "deck1Gain", "masterGain", "cueGain", "crossfader",
                                  "deck0CueSelect", "deck1CueSelect"};
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            auto tempo = UiEndpoint::tempoState(true);
            tempo["active"] = true;
            tempo["percent"] = deck == 0 ? 1.0 : -2.0;
            ui_->endpoint.snapshotTempo(deck, tempo);
            auto* jog = ui_->item((prefix + "Jog").toUtf8().constData());
            auto* display = ui_->item((prefix + "Display").toUtf8().constData());
            auto* seek = ui_->item((prefix + "Seek").toUtf8().constData());
            QVERIFY(jog && display && seek);
            initialJog[deck] = jog->width();
            initialDisplay[deck] = display->height();
            initialSeek[deck] = seek->height();
            for (const auto* suffix : {"Display", "Browse", "Jog", "Cue", "Play", "Tempo", "TempoReset",
                                       "MasterTempo", "LoopIn", "LoopOut", "LoopToggle"})
                outerControls.append(prefix + QString::fromUtf8(suffix));
        }
        const auto acceptedBeforeResize = ui_->endpoint.decks();
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        const bool hasMoreSpace = viewport.width() > initialSize.width() && viewport.height() > initialSize.height();
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            auto* jog = ui_->item((prefix + "Jog").toUtf8().constData());
            auto* display = ui_->item((prefix + "Display").toUtf8().constData());
            auto* seek = ui_->item((prefix + "Seek").toUtf8().constData());
            if (hasMoreSpace) {
                QTRY_VERIFY(jog->width() > initialJog[deck]);
                QVERIFY(jog->width() > 390); // Extra space must no longer stop at the old cap.
                QTRY_VERIFY(display->height() > initialDisplay[deck]);
                QTRY_VERIFY(seek->height() > initialSeek[deck]);
            } else {
                // A short window must release vertical space even when it is wide.
                QTRY_VERIFY(jog->width() < initialJog[deck]);
            }
            QCOMPARE(jog->width(), jog->height());
            const auto displayRect = display->mapRectToScene(QRectF(0, 0, display->width(), display->height()));
            const auto seekRect = seek->mapRectToScene(QRectF(0, 0, seek->width(), seek->height()));
            QVERIFY(displayRect.adjusted(-.5, -.5, .5, .5).contains(seekRect));
            QStringList insideControls;
            for (const auto* suffix : {"ArtworkFrame", "Title", "Artist", "Album", "Elapsed", "BpmReference", "Seek"})
                insideControls.append(prefix + QString::fromUtf8(suffix));
            verifyControlsFit(insideControls); // Waveform interaction stays below the LCD metadata.
            for (const auto& name : insideControls) {
                auto* item = ui_->item(name.toUtf8().constData());
                QVERIFY(displayRect.adjusted(-.5, -.5, .5, .5).contains(
                    item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))));
            }
        }
        verifyControlsFit(outerControls); // In particular, jog, transport and tempo have separate lanes.
        QCOMPARE(ui_->endpoint.decks(), acceptedBeforeResize);
        QVERIFY(ui_->endpoint.calls.isEmpty());

        // The expanded waveform remains a real seek target, independently of its paint size.
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            auto* seek = ui_->item((prefix + "Seek").toUtf8().constData());
            const auto peerBefore = ui_->endpoint.decks()[1 - deck];
            const auto point = seek->mapToScene(QPointF(seek->width() * .75, seek->height() * .75)).toPoint();
            const auto before = ui_->endpoint.calls.size();
            QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, point);
            QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
            verifyCueCall(before, "seek", deck);
            const auto seconds = ui_->endpoint.calls.last().toMap()["value"].toDouble();
            QVERIFY(seconds > 6 && seconds < 9);
            QCOMPARE(ui_->endpoint.decks()[1 - deck], peerBefore);
        }

        if (viewport == QSize(2560, 1600)) {
            // Hit the newly enlarged ring beyond the former 390px dial, on both decks.
            for (int deck = 0; deck < 2; ++deck) {
                const auto prefix = QString("deck%1").arg(deck);
                auto* jog = ui_->item((prefix + "Jog").toUtf8().constData());
                const auto tempoBefore = ui_->endpoint.decks()[deck].toMap()["tempo"].toMap();
                // Explicitly acknowledge TEMPO RESET before the pointer-only
                // vinyl path; the layout above still checks active tempo.
                ui_->endpoint.snapshotTempo(deck, UiEndpoint::tempoState(true));
                const auto radius = jog->width() * .43;
                QVERIFY(radius > 195);
                const auto top = jog->mapToScene(QPointF(jog->width() / 2, jog->height() / 2 - radius)).toPoint();
                const auto right = jog->mapToScene(QPointF(jog->width() / 2 + radius, jog->height() / 2)).toPoint();
                const auto peerBefore = ui_->endpoint.decks()[1 - deck];
                auto before = ui_->endpoint.calls.size();
                QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, top);
                QTest::mouseMove(ui_->window, right);
                QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, right);
                QTRY_VERIFY(ui_->endpoint.calls.size() >= before + 3);
                verifyCueCall(before, "scratchBegin", deck);
                verifyCueCall(ui_->endpoint.calls.size() - 1, "scratchEnd", deck);
                for (auto index = before + 1; index + 1 < ui_->endpoint.calls.size(); ++index) {
                    verifyCueCall(index, "scratchMove", deck);
                    QVERIFY(ui_->endpoint.calls[index].toMap()["frames"].toLongLong() > 0);
                }
                QCOMPARE(ui_->endpoint.decks()[1 - deck], peerBefore);
                ui_->endpoint.snapshotTempo(deck, tempoBefore);
                auto* play = ui_->item((prefix + "Play").toUtf8().constData());
                before = ui_->endpoint.calls.size();
                QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(play));
                QTRY_COMPARE(ui_->endpoint.calls.size(), before + 1);
                verifyCueCall(before, "playing", deck);
                QVERIFY(ui_->endpoint.calls.last().toMap()["value"].toBool());
                QTest::mouseClick(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(play));
                QTRY_COMPARE(ui_->endpoint.calls.size(), before + 2);
                verifyCueCall(before + 1, "playing", deck);
                QVERIFY(!ui_->endpoint.calls.last().toMap()["value"].toBool());
                auto* cue = ui_->item((prefix + "Cue").toUtf8().constData());
                QTest::mousePress(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cue));
                QTest::mouseRelease(ui_->window, Qt::LeftButton, Qt::NoModifier, ui_->center(cue));
                QTRY_COMPARE(ui_->endpoint.calls.size(), before + 4);
                verifyCueCall(before + 2, "cuePress", deck);
                verifyCueCall(before + 3, "cueRelease", deck);
                QCOMPARE(ui_->endpoint.decks()[1 - deck], peerBefore);
            }
        }

        const auto acceptedBeforeReturn = ui_->endpoint.decks();
        const auto callsBeforeReturn = ui_->endpoint.calls.size();
        ui_->window->resize(initialSize);
        QTRY_COMPARE(ui_->window->size(), initialSize);
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            auto* jog = ui_->item((prefix + "Jog").toUtf8().constData());
            QTRY_COMPARE(jog->width(), initialJog[deck]);
            QCOMPARE(jog->height(), initialJog[deck]);
            QTRY_COMPARE(ui_->item((prefix + "Display").toUtf8().constData())->height(), initialDisplay[deck]);
            QTRY_COMPARE(ui_->item((prefix + "Seek").toUtf8().constData())->height(), initialSeek[deck]);
        }
        verifyControlsFit(outerControls);
        QCOMPARE(ui_->endpoint.decks(), acceptedBeforeReturn);
        QCOMPARE(ui_->endpoint.calls.size(), callsBeforeReturn);
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
    }

    void platformTextSizeGrowsWithoutScalingDeckGeometry_data()
    {
        QTest::addColumn<QSize>("viewport");
        QTest::newRow("minimum") << QSize(1180, 760);
        QTest::newRow("normal") << QSize(1680, 1020);
    }

    void platformTextSizeGrowsWithoutScalingDeckGeometry()
    {
        QFETCH(QSize, viewport);
        const auto originalFont = QGuiApplication::font();
        const auto restoreFont = qScopeGuard([this, originalFont] {
            ui_.reset();
            QGuiApplication::setFont(originalFont);
        });
        // Qt.application.font is constant for an engine. Recreate the fixture
        // after changing the application font, including the engine from init().
        ui_.reset();
        auto platformFont = originalFont;
        platformFont.setPixelSize(15);
        QGuiApplication::setFont(platformFont);
        ui_ = std::make_unique<UiFixture>();
        ui_->load();
        QVERIFY2(ui_->window, qPrintable(ui_->warnings.join('\n')));
        QVERIFY(QTest::qWaitForWindowExposed(ui_->window));
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        for (int deck = 0; deck < 2; ++deck) {
            ui_->endpoint.loadState(deck);
            ui_->endpoint.snapshotSongInfo(deck, "Title", "Artist", "Album", {});
            ui_->endpoint.snapshotDeck(deck, {{"localTiming", QVariantMap{{"status", "Estimated"},
                {"bpm", 400.}, {"firstFrame", qulonglong(0)}, {"endFrame", qulonglong(7200)}}}});
        }
        ui_->endpoint.setReady(true);
        {
            QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
            ui_->window->contentItem()->polish();
            ui_->window->update();
            QTRY_VERIFY(polished.count() > 0);
        }
        const auto cueText = [this](int deck) -> QQuickItem* {
            auto* cue = ui_->item(QString("deck%1Cue").arg(deck).toUtf8().constData());
            if (!cue) return nullptr;
            auto* content = cue->property("contentItem").value<QQuickItem*>();
            if (!content) return nullptr;
            for (auto* child : content->childItems())
                if (child->property("text").toString() == "CUE" && child->property("font").isValid())
                    return child;
            return nullptr;
        };
        std::array<std::array<int, 3>, 2> baselinePixels{};
        std::array<std::array<QString, 3>, 2> baselineFamilies{};
        std::array<std::array<QSizeF, 3>, 2> baselineGeometry{};
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            const std::array<QQuickItem*, 3> text{
                ui_->item((prefix + "Ownership").toUtf8().constData()),
                ui_->item((prefix + "Title").toUtf8().constData()), cueText(deck)};
            for (std::size_t index = 0; index < text.size(); ++index) {
                QVERIFY(text[index]);
                const auto font = text[index]->property("font").value<QFont>();
                QVERIFY(font.pixelSize() > 0);
                baselinePixels[deck][index] = font.pixelSize();
                baselineFamilies[deck][index] = font.family();
            }
            std::size_t index = 0;
            for (const auto* suffix : {"Display", "Jog", "Cue"}) {
                auto* item = ui_->item((prefix + suffix).toUtf8().constData());
                QVERIFY(item);
                baselineGeometry[deck][index++] = item->size();
            }
        }
        QVERIFY(ui_->endpoint.calls.isEmpty());
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));

        ui_.reset();
        platformFont.setPixelSize(19);
        QGuiApplication::setFont(platformFont);
        ui_ = std::make_unique<UiFixture>();
        ui_->load();
        QVERIFY2(ui_->window, qPrintable(ui_->warnings.join('\n')));
        QVERIFY(QTest::qWaitForWindowExposed(ui_->window));
        ui_->window->resize(viewport);
        QTRY_COMPARE(ui_->window->size(), viewport);
        for (int deck = 0; deck < 2; ++deck) {
            ui_->endpoint.loadState(deck);
            ui_->endpoint.snapshotSongInfo(deck, "Title", "Artist", "Album", {});
            ui_->endpoint.snapshotDeck(deck, {{"localTiming", QVariantMap{{"status", "Estimated"},
                {"bpm", 400.}, {"firstFrame", qulonglong(0)}, {"endFrame", qulonglong(7200)}}}});
        }
        ui_->endpoint.setReady(true);
        {
            QSignalSpy polished(ui_->window, &QQuickWindow::frameSwapped);
            ui_->window->contentItem()->polish();
            ui_->window->update();
            QTRY_VERIFY(polished.count() > 0);
        }
        for (int deck = 0; deck < 2; ++deck) {
            const auto prefix = QString("deck%1").arg(deck);
            const std::array<QQuickItem*, 3> text{
                ui_->item((prefix + "Ownership").toUtf8().constData()),
                ui_->item((prefix + "Title").toUtf8().constData()), cueText(deck)};
            for (std::size_t index = 0; index < text.size(); ++index) {
                QVERIFY(text[index]);
                QTRY_VERIFY2(text[index]->property("font").value<QFont>().pixelSize() > baselinePixels[deck][index],
                             qPrintable(prefix + " text did not grow with the independently enlarged platform font"));
                QCOMPARE(text[index]->property("font").value<QFont>().family(), baselineFamilies[deck][index]);
                QTRY_VERIFY2(text[index]->implicitHeight() <= text[index]->height() + .5,
                             qPrintable(prefix + " enlarged text is clipped vertically"));
                QTRY_VERIFY2(text[index]->implicitWidth() <= text[index]->width() + .5,
                             qPrintable(prefix + " short enlarged label is clipped horizontally"));
            }
            auto* display = ui_->item((prefix + "Display").toUtf8().constData());
            auto* jog = ui_->item((prefix + "Jog").toUtf8().constData());
            auto* cue = ui_->item((prefix + "Cue").toUtf8().constData());
            QVERIFY(display && jog && cue);
            // Text growth may add LCD rows and consume vertical platter room;
            // it must not magnify the deck width or the transport hit area.
            QCOMPARE(display->width(), baselineGeometry[deck][0].width());
            QVERIFY(display->height() >= baselineGeometry[deck][0].height());
            QCOMPARE(cue->size(), baselineGeometry[deck][2]);
            QVERIFY(jog->width() >= 220 && jog->width() <= baselineGeometry[deck][1].width());
            QCOMPARE(jog->height(), jog->width());
            auto* ownership = text[0];
            auto* accessible = QAccessible::queryAccessibleInterface(ownership);
            QVERIFY(accessible);
            QCOMPARE(accessible->text(QAccessible::Name),
                     deck == 0 ? QString("Deck A control ownership: Manual") : QString("Deck B control ownership: Manual"));
            QVERIFY(accessible->rect().contains(ui_->window->mapToGlobal(ui_->center(ownership))));
            QStringList lcd;
            for (const auto* suffix : {"WaveformKind", "ArtworkFrame", "Title", "Artist", "Album", "Elapsed",
                                       "BpmCurrent", "BpmStatus", "LocalTempo", "Remaining"})
                lcd.append(prefix + suffix);
            auto* status = ui_->item((prefix + "TransportStatus").toUtf8().constData());
            QVERIFY(status);
            if (status->isVisible()) lcd.append(prefix + "TransportStatus");
            verifyControlsFit(lcd);
            const auto displayRect = display->mapRectToScene(QRectF(QPointF(0, 0), display->size()));
            auto* overview = ui_->item((prefix + "Seek").toUtf8().constData());
            QVERIFY(overview);
            const auto seekRect = overview->mapRectToScene(QRectF(QPointF(0, 0), overview->size()));
            QVERIFY(displayRect.adjusted(-.5, -.5, .5, .5).contains(seekRect));
            for (const auto& name : lcd) {
                auto* item = ui_->item(name.toUtf8().constData());
                QVERIFY(item);
                QTRY_VERIFY2(displayRect.adjusted(-.5, -.5, .5, .5).contains(
                    item->mapRectToScene(QRectF(QPointF(0, 0), item->size()))), qPrintable(name));
                if (item->property("textFormat").isValid())
                    QTRY_VERIFY2(item->implicitHeight() <= item->height() + .5, qPrintable(name + " text is clipped"));
                QVERIFY2(item->mapRectToScene(QRectF(QPointF(0, 0), item->size())).bottom() <= seekRect.top() + .5,
                         qPrintable(name + " overlaps the overview seek area"));
            }
            auto* local = ui_->item((prefix + "LocalTempo").toUtf8().constData());
            QVERIFY(local);
            QCOMPARE(local->property("text").toString(), QString("LOCAL SOURCE EST. 400.0 BPM"));
            QVERIFY(local->implicitWidth() <= local->width() + .5);
            auto* reference = ui_->item((prefix + "BpmReference").toUtf8().constData());
            QVERIFY(reference);
            const auto bpmRect = reference->mapRectToScene(QRectF(QPointF(0, 0), reference->size()));
            QVERIFY(displayRect.adjusted(-.5, -.5, .5, .5).contains(bpmRect));
            for (const auto* suffix : {"BpmCurrent", "BpmStatus", "Remaining"}) {
                auto* label = ui_->item((prefix + suffix).toUtf8().constData());
                QVERIFY(label);
                QVERIFY2(label->implicitWidth() <= label->width() + .5, suffix);
                QVERIFY2(label->implicitHeight() <= label->height() + .5, suffix);
                QVERIFY(bpmRect.adjusted(-.5, -.5, .5, .5).contains(
                    label->mapRectToScene(QRectF(QPointF(0, 0), label->size()))));
            }
        }
        verifyControlsFit({"deck0Cue", "deck0Play", "deck0Seek", "deck0Gain", "deck0Ownership", "deck0Jog",
                           "deck1Cue", "deck1Play", "deck1Seek", "deck1Gain", "deck1Ownership", "deck1Jog",
                           "masterGain", "cueGain", "crossfader", "audioSettingsOpen", "themeToggle"});
        QVERIFY(ui_->endpoint.calls.isEmpty()); // Font/layout updates are never input requests.
        const auto captureDirectory = qEnvironmentVariable("CONFLUX_UI_CAPTURE_DIR");
        if (!captureDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(captureDirectory));
            const auto image = ui_->window->grabWindow();
            QVERIFY(!image.isNull());
            QVERIFY(image.save(QDir(captureDirectory).filePath(
                QString("text-19px-%1x%2.png").arg(viewport.width()).arg(viewport.height()))));
        }
        const auto peer = ui_->endpoint.decks()[1];
        auto* seek = ui_->item("deck0Seek");
        QVERIFY(seek);
        auto* accessible = QAccessible::queryAccessibleInterface(seek);
        QVERIFY(accessible && accessible->valueInterface());
        QVERIFY(accessible->text(QAccessible::Name).contains("Deck A"));
        QVERIFY(accessible->rect().contains(ui_->window->mapToGlobal(ui_->center(seek))));
        seek->forceActiveFocus();
        QTRY_VERIFY(seek->hasActiveFocus());
        QTest::keyClick(ui_->window, Qt::Key_Right);
        QTRY_COMPARE(ui_->endpoint.calls.size(), 1);
        verifyCueCall(0, "seek", 0);
        QCOMPARE(ui_->endpoint.decks()[1], peer);
        QCoreApplication::processEvents();
        QCOMPARE(ui_->endpoint.calls.size(), 1);
        QVERIFY2(ui_->warnings.isEmpty(), qPrintable(ui_->warnings.join('\n')));
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
                           "masterGain", "cueGain", "deck0CueSelect", "deck1CueSelect", "audioSettingsOpen", "themeToggle",
                           "deck0Ownership", "deck1Ownership"});
        for (int deck = 0; deck < 2; ++deck) {
            auto* ownership = ui_->item(QString("deck%1Ownership").arg(deck).toUtf8().constData());
            QVERIFY(ownership);
            const auto expected = deck == 0 ? QString("DECK A / MANUAL") : QString("DECK B / MANUAL");
            QTRY_COMPARE(ownership->property("text").toString(), expected);
            auto* accessible = QAccessible::queryAccessibleInterface(ownership);
            QVERIFY(accessible);
            QCOMPARE(accessible->text(QAccessible::Name),
                     deck == 0 ? QString("Deck A control ownership: Manual") : QString("Deck B control ownership: Manual"));
            QVERIFY(ownership->implicitWidth() <= ownership->width() + .5);
            QVERIFY(ownership->implicitHeight() <= ownership->height() + .5);
        }
        QVERIFY(ui_->endpoint.calls.isEmpty());
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

    // QtTest retains registered touch devices; share one across methods and
    // delete it only when the test object is destroyed.
    std::unique_ptr<QPointingDevice> touchDevice_{QTest::createTouchDevice()};
    std::unique_ptr<UiFixture> ui_;
    bool touchActive_ = false;
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

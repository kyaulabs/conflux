// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioFile.hpp"
#include "audio/LiveSession.hpp"
#include "audio/AudioRouting.hpp"
#include "audio/StereoRecorder.hpp"
#include "library/LibraryCatalog.hpp"
#include "library/PreparedTrackStore.hpp"
#include "library/BpmAnalyzer.hpp"
#include "library/HarmonyAnalyzer.hpp"

#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace conflux {

// GUI-thread owner. One preparation job feeds a bounded callback-boundary
// source handoff; retired readers are joined outside the GUI/render threads.
class DeckController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList decks READ decks NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool outputReady READ outputReady NOTIFY stateChanged)
    Q_PROPERTY(bool stemsAvailable READ stemsAvailable CONSTANT)
    Q_PROPERTY(bool residentHoldAvailable READ residentHoldAvailable CONSTANT)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(double masterGain READ masterGain NOTIFY stateChanged)
    Q_PROPERTY(double crossfader READ crossfader NOTIFY stateChanged)
    Q_PROPERTY(double cueGain READ cueGain NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap meters READ meters NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap audioRouting READ audioRouting NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap preparation READ preparation NOTIFY stateChanged)
    Q_PROPERTY(QVariantList libraryTracks READ libraryTracks NOTIFY libraryChanged)
    Q_PROPERTY(bool libraryReady READ libraryReady NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryError READ libraryError NOTIFY libraryChanged)
public:
    struct PreparationOptions {
        std::optional<library::StemEngine> stemEngine;
        library::PreparedTrackStore::Limits limits;
        // Empty keeps complete offline preparation; explicit service mode first
        // looks for the same validated bank, then prepares a live owner on miss.
        QString stemServicePath;
        std::optional<library::BpmEngine> bpmEngine;
        std::optional<library::HarmonyEngine> harmonyEngine;
    };
    explicit DeckController(QObject* parent = nullptr);
    explicit DeckController(PreparationOptions options, QObject* parent = nullptr);
    ~DeckController() override;
    QVariantList decks() const;
    bool busy() const { return busy_; }
    bool outputReady() const { return ready_; }
    // Configuration presence only; dependency/identity checks run on preparation.
    bool stemsAvailable() const { return preparationOptions_.stemEngine.has_value(); }
    bool residentHoldAvailable() const { return stemsAvailable() && !preparationOptions_.stemServicePath.isEmpty(); }
    // Distinct PCM owners, including a worker retirement not yet completed.
    std::uint64_t retainedPcmBytes() const;
    // GUI/control owner only; the harness is the sole observation consumer.
    std::shared_ptr<library::LiveStemObservations> liveStemObservations(int deck) const;
    QString error() const { return error_; }
    double masterGain() const { return state_.masterGain; }
    double crossfader() const { return state_.crossfader; }
    double cueGain() const { return state_.cueGain; }
    QVariantMap meters() const;
    QVariantMap audioRouting() const;
    QVariantMap recording() const;
    QVariantMap preparation() const;
    QVariantList libraryTracks() const;
    bool libraryReady() const;
    QString libraryError() const;
    Q_INVOKABLE void loadDeck(int deck, const QUrl& url);
    Q_INVOKABLE void loadStemDeck(int deck, const QUrl& url);
    Q_INVOKABLE void importTrack(const QUrl& url);
    Q_INVOKABLE void importCueSheet(const QString& sourceId, const QUrl& url);
    Q_INVOKABLE bool jumpToImportedCue(int deck, const QString& sourceId, int cueIndex);
    Q_INVOKABLE bool saveDeckCue(int deck);
    Q_INVOKABLE bool recallDeckCue(int deck);
    Q_INVOKABLE void loadLibraryTrack(int deck, const QString& sourceId);
    Q_INVOKABLE void loadLibraryStemTrack(int deck, const QString& sourceId);
    Q_INVOKABLE void loadLibraryStemTrackWithRecoveryRange(int deck, const QString& sourceId,
                                                          double inSeconds, double outSeconds);
    // Tickets are decimal strings so QML never rounds uint64 identities.
    Q_INVOKABLE bool authorizeResidentHold(int deck, const QString& sourceId,
                                          const QString& outputSession, const QString& rangeId,
                                          const QString& generation, const QString& revision);
    Q_INVOKABLE bool cancelResidentHold(int deck, const QString& sourceId,
                                       const QString& outputSession, const QString& rangeId,
                                       const QString& generation, const QString& revision);
    Q_INVOKABLE bool exitResidentHold(int deck, const QString& sourceId,
                                     const QString& outputSession, const QString& rangeId,
                                     const QString& generation, const QString& revision);
    Q_INVOKABLE bool cancelPreparation();
    Q_INVOKABLE void setPlaying(int deck, bool playing);
    Q_INVOKABLE void seekSeconds(int deck, double seconds);
    Q_INVOKABLE bool jogFrames(int deck, qint64 deltaFrames);
    Q_INVOKABLE QString beginScratch(int deck);
    Q_INVOKABLE bool moveScratch(int deck, const QString& token, qint64 cumulativeFrames);
    Q_INVOKABLE bool endScratch(int deck, const QString& token);
    Q_INVOKABLE void setDeckGain(int deck, double gain);
    Q_INVOKABLE void setEqGainDb(int deck, int band, double db);
    Q_INVOKABLE void setStemGain(int deck, int stem, double gain);
    Q_INVOKABLE void setMasterGain(double gain);
    Q_INVOKABLE void setCrossfader(double position);
    Q_INVOKABLE void loopIn(int deck);
    Q_INVOKABLE void loopOut(int deck);
    Q_INVOKABLE void loopToggle(int deck);
    Q_INVOKABLE void setTempoPercent(int deck, double percent);
    Q_INVOKABLE void setTempoBpm(int deck, double bpm);
    Q_INVOKABLE void setBpmOverride(int deck, double bpm);
    Q_INVOKABLE void clearBpmOverride(int deck);
    Q_INVOKABLE void toggleMasterTempo(int deck);
    Q_INVOKABLE void resetTempo(int deck);
    Q_INVOKABLE void setCueGain(double gain);
    Q_INVOKABLE void setCueSelected(int deck, bool selected);
    Q_INVOKABLE void refreshAudioRouting();
    // Empty master keys select the current default; empty cue keys disable cue.
    Q_INVOKABLE void configureAudioRouting(const QString& masterLeft, const QString& masterRight,
                                          const QString& cueLeft, const QString& cueRight);
    Q_INVOKABLE void startRecording(const QUrl& url);
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE bool hotCue(int deck, int slot);
    Q_INVOKABLE bool clearHotCue(int deck, int slot);
    Q_INVOKABLE bool cuePress(int deck);
    Q_INVOKABLE void cueRelease(int deck);
    // Called after output has stopped by the host integration check as well as
    // destruction. Repeated shutdown is safe; decoded owners stay alive.
    void shutdown();
    audio::LiveSession::State audioState() const { return state_; }
    std::uint64_t submittedCommands() const { return submitted_; }
    std::uint64_t completedSessions() const { return completedSessions_; }
    std::uint64_t completedApplied() const { return completedApplied_; }
    std::uint64_t completedRejected() const { return completedRejected_; }
    std::uint64_t completedInvalidBuffers() const { return completedInvalidBuffers_; }
    std::uint64_t completedCallbacks() const { return completedCallbacks_; }
    std::uint64_t completedDeadlineMisses() const { return completedDeadlineMisses_; }
    std::uint64_t maximumCallbackNs() const { return maximumCallbackNs_; }
    std::uint64_t completedStarvedFrames() const { return completedStarvedFrames_; }
    std::uint64_t completedReadFailures() const { return completedReadFailures_; }
    const std::vector<audio::CallbackTiming::Summary>& completedTiming() const { return completedTiming_; }
signals:
    void stateChanged();
    void libraryChanged();
private:
    // Immutable finite leaves or live source metadata. A live slot retains no
    // complete PCM and cannot manufacture another producer; its whole playback
    // owns the charged job through callback handoff and worker retirement.
    struct PreparedPcm {
        explicit PreparedPcm(audio::PreparedAudio audio);
        explicit PreparedPcm(std::array<audio::PreparedAudio, 4> stems);
        PreparedPcm(std::uint64_t frames, std::vector<float> sourcePeaks, QString sourceSha256,
                    std::shared_ptr<library::LiveStemObservations> observations,
                    QString sourcePath, QString artifactKey,
                    std::optional<audio::AudioResidentHoldRange> residentHold = {});
        bool live() const noexcept { return liveFrames != 0; }
        std::uint64_t frames() const noexcept { return live() ? liveFrames : audioLeaves.front().frames(); }
        std::uint64_t bytes() const noexcept;
        bool stemBank() const noexcept { return live() || audioLeaves.size() == 4; }
        std::span<const float> peaks() const noexcept
        { return live() ? std::span<const float>(livePeaks) : audioLeaves.front().peaks(); }
        std::unique_ptr<audio::PreparedPlayback> makePlayback() const;
        std::vector<audio::PreparedAudio> audioLeaves;
        std::uint64_t liveFrames = 0;
        std::vector<float> livePeaks;
        QString liveSourceSha256;
        std::shared_ptr<library::LiveStemObservations> observations;
        QString sourcePath, artifactKey;
        std::optional<audio::AudioResidentHoldRange> residentHold;
    };
    struct PreparedLoad {
        std::shared_ptr<const PreparedPcm> audio;
        QVariantList waveform;
        std::unique_ptr<audio::PreparedPlayback> playback;
        bool cacheHit = false;
        QString sourceId;
        library::TrackPresentation presentation;
        library::BpmEstimate bpm;
        library::HarmonyEstimate harmony;
    };
    struct LoadResult {
        std::shared_ptr<PreparedLoad> prepared;
        QString error;
        std::optional<library::LibraryTrack> imported;
        bool cacheHit = false;
        bool cueImport = false;
    };
    enum class LoaderPurpose { None, Preparation, Continuation };
    struct ForegroundPreparation {
        enum class Kind { Track, CueImport };
        Kind kind = Kind::Track;
        int deck = 0;
        QUrl url;
        bool importOnly = false, stems = false;
        QString expectedSource;
        std::optional<library::LibraryTrack> cueTrack;
        std::optional<library::LiveStemResidentHold> residentHold;
    };
    struct ResidentAuthorization {
        enum class Phase { None, NextPlay, PlayPending, ArmPending, Armed, Cancelled };
        const PreparedPcm* clip = nullptr;
        const audio::LiveSession* session = nullptr;
        std::uint64_t outputSession = 0, generation = 0, revision = 0, commandId = 0;
        Phase phase = Phase::None;
        QString detail;
    };
    struct ContinuationEpisode {
        const PreparedPcm* clip = nullptr;
        std::uint64_t sourceGeneration = 0, revision = 0, frame = 0, generation = 0, attempts = 0, residentHoldRangeId = 0;
        QElapsedTimer stalled;
        bool attempted = false;
        QString status = "Idle", detail;
    };
    struct ContinuationAttempt {
        std::size_t deck = 0;
        std::shared_ptr<const PreparedPcm> original;
        const audio::LiveSession* session = nullptr;
        std::uint64_t sourceGeneration = 0, revision = 0, frame = 0, generation = 0, residentHoldRangeId = 0;
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<PreparedLoad> prepared;
        std::unique_ptr<audio::PreparedPlayback> retired;
        std::optional<audio::LiveSession::State> snapshot;
        bool revoked = false, published = false, acknowledged = false;
    };
    struct CatalogResult {
        std::vector<library::LibraryTrack> tracks;
        QString error;
        bool saving = false;
    };
    struct InstallResult {
        std::unique_ptr<audio::LiveSession> session;
        QString error;
        std::array<bool, audio::RenderCore::deckCount> lostLiveOwners{};
    };
    struct RecordingStart {
        std::shared_ptr<audio::StereoRecorder> writer;
        QString error;
    };
    struct RecordingCompletion {
        QString error;
        std::uint64_t accepted = 0, written = 0, overflows = 0, overflowFrames = 0;
    };
    void loaded();
    void catalogFinished();
    QString deckCueUnavailable(int deck, bool saving) const;
    QVariantMap savedDeckCue(std::size_t deck) const;
    void beginPreparation(int deck, const QUrl& url, bool importOnly,
                          const QString& expectedSource = {}, bool stems = false,
                          std::optional<library::LiveStemResidentHold> residentHold = {});
    bool preparationActive() const;
    void updatePreparation(std::uint64_t serial, QString phase, int completedStages,
                           std::optional<library::TrackPresentation> presentation = {},
                           QVariantList waveform = {});
    void loadLibrary(int deck, const QString& sourceId, bool stems);
    void installed();
    void installSession(std::unique_ptr<audio::PreparedPlayback> retiredPlayback = {},
                        std::shared_ptr<const PreparedPcm> retiredClip = {});
    void publishReplacement();
    void collectReplacement();
    void acceptReplacement(std::optional<audio::LiveSession::SourceReplacementResult> result);
    void settleReplacement(bool stopped = false);
    bool deferForegroundPreparation(ForegroundPreparation request);
    void drainForegroundPreparation();
    void startCueImport(library::LibraryTrack selected, const QUrl& url);
    void resetResidentAuthorization(std::size_t deck);
    void revokeResidentAuthorization(std::size_t deck, const QString& reason);
    void pollResidentAuthorization();
    bool residentTicket(int deck, const QString& sourceId, const QString& outputSession,
                        const QString& rangeId, const QString& generation, const QString& revision) const;
    bool queueResidentArm(std::size_t deck);
    std::uint64_t continuationResidentRangeId(std::size_t deck) const;
    bool continuationEligible(std::size_t deck) const;
    bool continuationCurrent() const;
    void considerContinuation();
    void continuationLoaded(const LoadResult& result);
    void cancelContinuation(std::size_t deck, const QString& reason);
    void settleContinuation(bool stopped = false);
    void retirePlayback(std::unique_ptr<audio::PreparedPlayback> playback,
                      std::shared_ptr<const PreparedPcm> clip = {});
    struct RetirementGroup {
        std::unique_ptr<audio::PreparedPlayback> playback;
        std::shared_ptr<const PreparedPcm> clip;
    };
    struct RetirementBatch {
        std::array<RetirementGroup, 3> groups{};
        std::size_t count = 0;
    };
    void startRetirement(RetirementBatch batch);
    void retired();
    void recordingStarted();
    void recordingFinished();
    void pollRecording();
    void finalizeRecording();
    void poll();
    bool submit(audio::RenderCore::Command command);
    bool flushCueReleases();
    bool submitHotCue(int deck, int slot, bool clear);
    struct ScratchIdentity {
        QString token, sourceId;
        std::uint64_t nativeToken = 0, sourceGeneration = 0;
    };
    struct ScratchGesture {
        ScratchIdentity identity;
        QElapsedTimer interval;
    };
    bool sameScratchSource(std::size_t deck, const ScratchIdentity& identity) const;
    bool canScratch(std::size_t deck) const;
    bool scratchBusy(std::size_t deck) const;
    void queueScratchEnd(std::size_t deck);
    bool flushScratchEnds();
    void stopSession();
    void notifyStateChanged();
    void fail(const QString& error);
    std::array<std::shared_ptr<const PreparedPcm>, 2> clips_{};
    PreparationOptions preparationOptions_;
    // Active worker charges remain until its whole batch has been destroyed.
    // One bounded deferred batch can wait behind an ordinary retirement; never
    // overwrite its watcher/future or drop a waiting owner's charge.
    std::array<std::pair<const PreparedPcm*, std::uint64_t>, 3> retiringPcm_{};
    RetirementBatch deferredRetirement_{};
    bool retirementPending_ = false;
    std::stop_source loadCancellation_, installCancellation_;
    LoaderPurpose loaderPurpose_ = LoaderPurpose::None;
    std::optional<ForegroundPreparation> foregroundPreparation_;
    bool foregroundWaiting_ = false;
    std::array<ResidentAuthorization, 2> residentAuthorizations_{};
    std::array<ContinuationEpisode, 2> continuationEpisodes_{};
    std::optional<ContinuationAttempt> continuation_;
    // Exact quiesced owners, empty while the session/installer owns them.
    audio::LiveSession::PlaybackSources stoppedPlaybacks_{};
    std::array<QString, 2> names_{};
    std::array<QString, 2> sourceIds_{};
    std::array<library::TrackPresentation, 2> presentations_{};
    std::array<library::BpmEstimate, 2> bpmEstimates_{};
    std::array<library::HarmonyEstimate, 2> harmonyEstimates_{};
    std::array<std::optional<double>, 2> bpmOverrides_{};
    std::array<QVariantList, 2> waveforms_{};
    std::array<std::uint64_t, 2> waveformRevisions_{};
    std::array<bool, 2> cueDown_{}, cueReleasePending_{};
    // GUI ownership only, not optimistic render state. One pending release per
    // deck blocks a new gesture until queued or the old source is retired.
    std::array<std::optional<ScratchGesture>, 2> scratchGestures_{};
    std::array<std::optional<ScratchIdentity>, 2> scratchEnds_{};
    std::array<bool, 2> replacing_{};
    std::shared_ptr<PreparedLoad> pendingLoad_;
    std::unique_ptr<audio::PreparedPlayback> pendingRetired_;
    std::optional<audio::LiveSession::State> replacementSnapshot_;
    bool replacementPublished_ = false, replacementAcknowledged_ = false;
    std::uint64_t nextSourceGeneration_ = 0, pendingGeneration_ = 0;
    std::unique_ptr<audio::LiveSession> session_;
    std::unique_ptr<audio::LiveSession> discovery_;
    audio::RoutingConfig routingConfig_;
    audio::RoutingSnapshot routingSnapshot_;
    audio::LiveSession::State state_{};
    mutable std::optional<QVariantList> deckSnapshot_;
    std::uint64_t completedStarvedFrames_ = 0, completedReadFailures_ = 0;
    std::vector<audio::CallbackTiming::Summary> completedTiming_;
    QFutureWatcher<LoadResult> loader_;
    QFutureWatcher<CatalogResult> catalogJob_;
    std::vector<library::LibraryTrack> libraryEntries_;
    QVariantList libraryTracks_;
    QString libraryPath_, libraryError_;
    bool libraryReady_ = false;
    QFutureWatcher<std::shared_ptr<InstallResult>> installer_;
    QFutureWatcher<void> retirer_;
    QFutureWatcher<RecordingStart> recordingStarter_;
    QFutureWatcher<RecordingCompletion> recordingFinalizer_;
    std::shared_ptr<audio::StereoRecorder> recorder_;
    RecordingCompletion lastRecording_;
    QString recordingPath_, recordingStatus_ = "Idle", recordingError_;
    bool recordingStopRequested_ = false;
    bool recordingStartPending_ = false, recordingFinalizationPending_ = false;
    QTimer pollTimer_;
    QString error_;
    QString pendingName_;
    QString pendingPath_, preparationPhase_;
    library::TrackPresentation pendingPresentation_;
    QVariantList pendingWaveform_;
    std::uint64_t preparationSerial_ = 0;
    int preparationCompletedStages_ = 0, preparationTotalStages_ = 0;
    QString preparationStatus_ = "Idle";
    bool preparationCacheHit_ = false;
    bool pendingImport_ = false, pendingStems_ = false;
    int pendingDeck_ = 0;
    bool busy_ = false;
    bool ready_ = false;
    bool installationPending_ = false;
    bool closing_ = false;
    std::uint64_t submitted_ = 0;
    // Command IDs are cumulative; a rebuilt native session starts at zero.
    std::uint64_t sessionCommandBase_ = 0;
    std::uint64_t completedSessions_ = 0;
    std::uint64_t completedApplied_ = 0;
    std::uint64_t completedRejected_ = 0;
    std::uint64_t completedInvalidBuffers_ = 0;
    std::uint64_t completedCallbacks_ = 0;
    std::uint64_t completedDeadlineMisses_ = 0;
    std::uint64_t maximumCallbackNs_ = 0;
};
} // namespace conflux

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioFile.hpp"
#include "audio/LiveSession.hpp"
#include "audio/AudioRouting.hpp"
#include "audio/StereoRecorder.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <array>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace conflux {

// GUI-thread owner. One preparation job feeds a bounded callback-boundary
// source handoff; retired readers are joined outside the GUI/render threads.
class DeckController final : public QObject {
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
    Q_PROPERTY(QVariantMap preparation READ preparation NOTIFY stateChanged)
public:
    explicit DeckController(QObject* parent = nullptr);
    ~DeckController() override;
    QVariantList decks() const;
    bool busy() const { return busy_; }
    bool outputReady() const { return ready_; }
    QString error() const { return error_; }
    double masterGain() const { return state_.masterGain; }
    double cueGain() const { return state_.cueGain; }
    QVariantMap meters() const;
    QVariantMap audioRouting() const;
    QVariantMap recording() const;
    QVariantMap preparation() const;
    Q_INVOKABLE void loadDeck(int deck, const QUrl& url);
    Q_INVOKABLE bool cancelPreparation();
    Q_INVOKABLE void setPlaying(int deck, bool playing);
    Q_INVOKABLE void seekSeconds(int deck, double seconds);
    Q_INVOKABLE bool jogFrames(int deck, qint64 deltaFrames);
    Q_INVOKABLE void setDeckGain(int deck, double gain);
    Q_INVOKABLE void setMasterGain(double gain);
    Q_INVOKABLE void setCueGain(double gain);
    Q_INVOKABLE void setCueSelected(int deck, bool selected);
    Q_INVOKABLE void refreshAudioRouting();
    // Empty master keys select the current default; empty cue keys disable cue.
    Q_INVOKABLE void configureAudioRouting(const QString& masterLeft, const QString& masterRight,
                                          const QString& cueLeft, const QString& cueRight);
    Q_INVOKABLE void startRecording(const QUrl& url);
    Q_INVOKABLE void stopRecording();
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
private:
    struct PreparedLoad {
        std::shared_ptr<const audio::PreparedAudio> audio;
        QVariantList waveform;
        std::unique_ptr<audio::PcmReadAhead> reader;
        bool cacheHit = false;
    };
    struct LoadResult {
        std::shared_ptr<PreparedLoad> prepared;
        QString error;
    };
    struct InstallResult {
        std::unique_ptr<audio::LiveSession> session;
        QString error;
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
    void installed();
    void installSession();
    void publishReplacement();
    void collectReplacement();
    void settleReplacement(bool stopped = false);
    void retireReader(std::unique_ptr<audio::PcmReadAhead> reader,
                      std::shared_ptr<const audio::PreparedAudio> clip = {});
    void retired();
    void recordingStarted();
    void recordingFinished();
    void pollRecording();
    void finalizeRecording();
    void poll();
    bool submit(audio::RenderCore::Command command);
    bool flushCueReleases();
    void stopSession();
    void fail(const QString& error);
    std::array<std::shared_ptr<const audio::PreparedAudio>, 2> clips_{};
    std::stop_source loadCancellation_;
    std::array<QString, 2> names_{};
    std::array<QVariantList, 2> waveforms_{};
    std::array<bool, 2> cueDown_{}, cueReleasePending_{};
    std::array<bool, 2> replacing_{};
    std::shared_ptr<PreparedLoad> pendingLoad_;
    std::unique_ptr<audio::PcmReadAhead> pendingRetired_;
    std::optional<audio::LiveSession::State> replacementSnapshot_;
    bool replacementPublished_ = false, replacementAcknowledged_ = false;
    std::uint64_t nextSourceGeneration_ = 0, pendingGeneration_ = 0;
    std::unique_ptr<audio::LiveSession> session_;
    std::unique_ptr<audio::LiveSession> discovery_;
    audio::RoutingConfig routingConfig_;
    audio::RoutingSnapshot routingSnapshot_;
    audio::LiveSession::State state_{};
    std::uint64_t completedStarvedFrames_ = 0, completedReadFailures_ = 0;
    std::vector<audio::CallbackTiming::Summary> completedTiming_;
    QFutureWatcher<LoadResult> loader_;
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
    QString preparationStatus_ = "Idle";
    bool preparationCacheHit_ = false;
    int pendingDeck_ = 0;
    bool busy_ = false;
    bool ready_ = false;
    bool closing_ = false;
    std::uint64_t submitted_ = 0;
    std::uint64_t completedSessions_ = 0;
    std::uint64_t completedApplied_ = 0;
    std::uint64_t completedRejected_ = 0;
    std::uint64_t completedInvalidBuffers_ = 0;
    std::uint64_t completedCallbacks_ = 0;
    std::uint64_t completedDeadlineMisses_ = 0;
    std::uint64_t maximumCallbackNs_ = 0;
};
} // namespace conflux

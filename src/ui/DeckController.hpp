// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "audio/AudioFile.hpp"
#include "audio/LiveSession.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <array>
#include <memory>

namespace conflux {

// GUI-thread owner. Decoding uses one background job; PCM replacement happens
// only after LiveSession::stop has quiesced the borrowed spans.
class DeckController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList decks READ decks NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool outputReady READ outputReady NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(double masterGain READ masterGain NOTIFY stateChanged)
public:
    explicit DeckController(QObject* parent = nullptr);
    ~DeckController() override;
    QVariantList decks() const;
    bool busy() const { return busy_; }
    bool outputReady() const { return ready_; }
    QString error() const { return error_; }
    double masterGain() const { return state_.masterGain; }
    Q_INVOKABLE void loadDeck(int deck, const QUrl& url);
    Q_INVOKABLE void setPlaying(int deck, bool playing);
    Q_INVOKABLE void seekSeconds(int deck, double seconds);
    Q_INVOKABLE void setDeckGain(int deck, double gain);
    Q_INVOKABLE void setMasterGain(double gain);
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
signals:
    void stateChanged();
private:
    struct LoadResult {
        std::shared_ptr<const audio::DecodedAudio> audio;
        QString error;
        QVariantList waveform;
    };
    void loaded();
    void poll();
    bool submit(audio::RenderCore::Command command);
    bool flushCueReleases();
    void stopSession();
    void fail(const QString& error);
    std::array<std::shared_ptr<const audio::DecodedAudio>, 2> clips_{};
    std::array<QString, 2> names_{};
    std::array<QVariantList, 2> waveforms_{};
    std::array<bool, 2> cueDown_{}, cueReleasePending_{};
    std::unique_ptr<audio::LiveSession> session_;
    audio::LiveSession::State state_{};
    QFutureWatcher<LoadResult> loader_;
    QTimer pollTimer_;
    QString error_;
    QString pendingName_;
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

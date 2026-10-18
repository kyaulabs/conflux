// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui/DeckController.hpp"

#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace conflux {

DeckController::DeckController(QObject* parent) : QObject(parent)
{
    connect(&loader_, &QFutureWatcher<LoadResult>::finished, this, &DeckController::loaded);
    connect(&installer_, &QFutureWatcher<std::shared_ptr<InstallResult>>::finished, this, &DeckController::installed);
    connect(&pollTimer_, &QTimer::timeout, this, &DeckController::poll);
    pollTimer_.start(50);
}

DeckController::~DeckController()
{
    shutdown();
}

void DeckController::shutdown()
{
    if (closing_)
        return;
    closing_ = true;
    pollTimer_.stop();
    disconnect(&loader_, nullptr, this, nullptr);
    disconnect(&installer_, nullptr, this, nullptr);
    loadCancellation_.request_stop();
    stopSession();
    // Preparation checks cancellation between bounded reads/conversion/writes.
    // Join before releasing the controller and decoded owners.
    loader_.waitForFinished();
    installer_.waitForFinished();
    if (installer_.future().resultCount() > 0) {
        const auto pending = installer_.result();
        if (pending && pending->session) pending->session->stop();
    }
    busy_ = false;
}

QVariantList DeckController::decks() const
{
    QVariantList result;
    for (std::size_t deck = 0; deck < clips_.size(); ++deck) {
        result.append(QVariantMap{
            {"name", names_[deck]}, {"loaded", bool(clips_[deck])},
            {"waveform", waveforms_[deck]},
            {"playing", state_.playing[deck]},
            {"cueSeconds", state_.cueFrame[deck] / double(audio::RenderCore::sampleRate)},
            {"cueHeld", state_.cueHeld[deck]}, {"cuePreview", state_.cuePreview[deck]},
            {"buffering", state_.buffering[deck]}, {"sourceReadError", state_.sourceReadError[deck]},
            {"starvedFrames", QVariant::fromValue<qulonglong>(state_.starvedFrames[deck])},
            {"positionSeconds", state_.sourceFrame[deck] / double(audio::RenderCore::sampleRate)},
            {"durationSeconds", state_.sourceFrames[deck] / double(audio::RenderCore::sampleRate)},
            {"gain", state_.deckGain[deck]}});
    }
    return result;
}

void DeckController::fail(const QString& error)
{
    error_ = error;
    emit stateChanged();
}

void DeckController::loadDeck(int deck, const QUrl& url)
{
    if (closing_)
        return;
    if (deck < 0 || deck >= int(clips_.size())) {
        fail("Invalid deck.");
        return;
    }
    if (busy_) {
        fail("A file is already loading. Wait for it to finish.");
        return;
    }
    if (!url.isLocalFile() || (!url.host().isEmpty() && url.host() != "localhost") ||
        !QFileInfo(url.toLocalFile()).isAbsolute()) {
        fail("Enter an absolute local file path.");
        return;
    }
    pendingDeck_ = deck;
    const auto path = url.toLocalFile();
    pendingName_ = QFileInfo(path).fileName();
    busy_ = true;
    error_.clear();
    emit stateChanged();
    loadCancellation_ = std::stop_source{};
    const auto baseCache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (baseCache.isEmpty()) {
        busy_ = false;
        fail("Cannot find a local cache directory for audio preparation.");
        return;
    }
    const auto cache = baseCache + "/pcm";
    loader_.setFuture(QtConcurrent::run([path, cache, stop = loadCancellation_.get_token()]() -> LoadResult {
        try {
            // QFile::encodeName preserves the platform's filesystem encoding.
            const auto encoded = QFile::encodeName(path);
            const auto encodedCache = QFile::encodeName(cache);
            auto decoded = audio::prepareAudioFile(std::filesystem::path(encoded.constData()),
                audio::PreparedAudio::defaultMaxPreparedBytes, stop, std::filesystem::path(encodedCache.constData()));
            QVariantList waveform;
            waveform.reserve(qsizetype(decoded.peaks().size()));
            for (const auto peak : decoded.peaks()) waveform.append(peak);
            return {std::make_shared<const audio::PreparedAudio>(std::move(decoded)), {}, waveform};
        } catch (const std::exception& error) {
            return {{}, QString::fromUtf8(error.what()), {}};
        } catch (...) {
            return {{}, "Audio decoding failed.", {}};
        }
    }));
}

void DeckController::stopSession()
{
    if (session_) {
        session_->stop();
        state_ = session_->stoppedState();
        ++completedSessions_;
        completedApplied_ += state_.applied;
        completedRejected_ += state_.rejected;
        completedInvalidBuffers_ += state_.invalidBuffers;
        completedCallbacks_ += state_.callbacks;
        completedDeadlineMisses_ += state_.deadlineMisses;
        maximumCallbackNs_ = std::max(maximumCallbackNs_, state_.maxExecutionNs);
        for (std::size_t deck = 0; deck < clips_.size(); ++deck) {
            completedStarvedFrames_ += state_.starvedFrames[deck];
            completedReadFailures_ += state_.sourceReadErrorObserved[deck] || state_.sourceReadError[deck] ? 1 : 0;
        }
        session_.reset();
    }
    ready_ = false;
    cueDown_.fill(false);
    cueReleasePending_.fill(false);
}

void DeckController::loaded()
{
    if (closing_)
        return;
    const auto result = loader_.result();
    if (!result.audio) {
        busy_ = false;
        fail(result.error);
        return;
    }
    stopSession();
    // No callback can refer to the old owner after stopSession returns.
    clips_[pendingDeck_] = result.audio;
    names_[pendingDeck_] = pendingName_;
    waveforms_[pendingDeck_] = result.waveform;
    state_.sourceFrame[pendingDeck_] = 0;
    state_.cueFrame[pendingDeck_] = 0;
    state_.cueHeld[pendingDeck_] = state_.cuePreview[pendingDeck_] = false;
    state_.buffering[pendingDeck_] = state_.sourceReadError[pendingDeck_] = false;
    state_.sourceReadErrorObserved[pendingDeck_] = false;
    state_.starvedFrames[pendingDeck_] = 0;
    state_.sourceFrames[pendingDeck_] = result.audio->frames();
    state_.playing[pendingDeck_] = false;
    state_.deckGain[pendingDeck_] = 1.0F;
    audio::LiveSession::InitialState initial;
    initial.sourceFrame = state_.sourceFrame;
    initial.cueFrame = state_.cueFrame;
    initial.playing = state_.playing;
    initial.deckGain = state_.deckGain;
    initial.masterGain = state_.masterGain;
    // Capture the exact quiesced cursor first. Priming can involve slow storage,
    // so keep the installing stage busy and construct its readers off the GUI.
    installer_.setFuture(QtConcurrent::run([clips = clips_, initial]() {
        auto result = std::make_shared<InstallResult>();
        try {
            result->session = std::make_unique<audio::LiveSession>(
                audio::LiveSession::PreparedSources{clips[0].get(), clips[1].get()}, initial);
        } catch (const std::exception& error) {
            result->error = QString::fromUtf8(error.what());
        } catch (...) {
            result->error = "Cannot install prepared audio";
        }
        return result;
    }));
    emit stateChanged();
}

void DeckController::installed()
{
    if (closing_) return;
    const auto result = installer_.result();
    busy_ = false;
    if (!result->session) {
        fail(result->error);
        return;
    }
    try {
        session_ = std::move(result->session);
        state_ = session_->stoppedState();
        std::string error;
        if (!session_->start(error)) {
            stopSession();
            fail(QString::fromStdString(error));
            return;
        }
    } catch (const std::exception& error) {
        stopSession();
        fail(QString::fromUtf8(error.what()));
        return;
    }
    error_.clear();
    emit stateChanged();
}

void DeckController::poll()
{
    if (!session_)
        return;
    flushCueReleases();
    audio::LiveSession::State snapshot;
    // The queue is bounded; GUI delay can drop intermediate snapshots. Drain at
    // most its capacity, keeping the newest complete published state.
    for (std::size_t index = 0; index < 32 && session_->pollState(snapshot); ++index)
        state_ = snapshot;
    const auto error = session_->error();
    if (!error.empty()) {
        stopSession();
        fail(QString::fromStdString(error));
        return;
    }
    ready_ = session_->ready();
    emit stateChanged();
}

bool DeckController::submit(audio::RenderCore::Command command)
{
    if (closing_ || !ready_ || !session_) {
        fail("Audio output is not ready.");
        return false;
    }
    // Release has priority over later slider/play events. Never lose a release
    // just because the bounded transport queue was full at pointer-up.
    if (!flushCueReleases()) {
        fail("Cue release is waiting for the audio control queue.");
        return false;
    }
    if (!session_->submit(command)) {
        fail("Audio control queue is full. Try the control again.");
        return false;
    }
    ++submitted_;
    error_.clear();
    return true;
}

bool DeckController::flushCueReleases()
{
    for (std::size_t deck = 0; deck < cueReleasePending_.size(); ++deck) {
        if (!cueReleasePending_[deck])
            continue;
        audio::RenderCore::Command command;
        command.type = audio::RenderCore::Command::Type::CueRelease;
        command.deck = std::uint32_t(deck);
        if (!session_ || !session_->submit(command))
            return false;
        cueReleasePending_[deck] = false;
        ++submitted_;
    }
    if (error_ == "Cue release is waiting for the audio control queue.")
        error_.clear();
    return true;
}

bool DeckController::cuePress(int deck)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck]) {
        fail("Load this deck before using cue.");
        return false;
    }
    if (cueDown_[deck])
        return false;
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::CuePress;
    command.deck = std::uint32_t(deck);
    if (!submit(command))
        return false;
    cueDown_[deck] = true;
    return true;
}

void DeckController::cueRelease(int deck)
{
    if (deck < 0 || deck >= int(cueDown_.size())) {
        fail("Invalid cue deck.");
        return;
    }
    if (!cueDown_[deck])
        return;
    cueDown_[deck] = false;
    cueReleasePending_[deck] = true;
    if (!flushCueReleases())
        fail("Cue release is waiting for the audio control queue.");
}

void DeckController::setPlaying(int deck, bool playing)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck]) {
        fail("Load this deck before playing.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = playing ? audio::RenderCore::Command::Type::Play : audio::RenderCore::Command::Type::Pause;
    command.deck = std::uint32_t(deck);
    submit(command);
}

void DeckController::seekSeconds(int deck, double seconds)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] ||
        !std::isfinite(seconds) || seconds < 0 ||
        seconds > clips_[deck]->frames() / double(audio::RenderCore::sampleRate)) {
        fail("Seek is outside the loaded track.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::Seek;
    command.deck = std::uint32_t(deck);
    command.frame = std::min<std::uint64_t>(clips_[deck]->frames(),
        std::uint64_t(std::llround(seconds * audio::RenderCore::sampleRate)));
    submit(command);
}

void DeckController::setDeckGain(int deck, double gain)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] ||
        !std::isfinite(gain) || gain < 0 || gain > 1) {
        fail("Deck gain must be between zero and one for a loaded deck.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::DeckGain;
    command.deck = std::uint32_t(deck);
    command.gain = float(gain);
    submit(command);
}

void DeckController::setMasterGain(double gain)
{
    if (!std::isfinite(gain) || gain < 0 || gain > 1) {
        fail("Master gain must be between zero and one.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::MasterGain;
    command.gain = float(gain);
    submit(command);
}
} // namespace conflux

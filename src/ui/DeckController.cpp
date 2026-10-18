// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui/DeckController.hpp"

#include <QFileInfo>
#include <QFile>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace conflux {

DeckController::DeckController(QObject* parent) : QObject(parent)
{
    connect(&loader_, &QFutureWatcher<LoadResult>::finished, this, &DeckController::loaded);
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
    // QtConcurrent::run cannot cancel a running decoder. The decoder accepts
    // regular files and bounds its PCM allocation; wait before releasing state.
    loader_.waitForFinished();
    busy_ = false;
    stopSession();
}

QVariantList DeckController::decks() const
{
    QVariantList result;
    for (std::size_t deck = 0; deck < clips_.size(); ++deck) {
        result.append(QVariantMap{
            {"name", names_[deck]}, {"loaded", bool(clips_[deck])},
            {"playing", state_.playing[deck]},
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
    loader_.setFuture(QtConcurrent::run([path]() -> LoadResult {
        try {
            // QFile::encodeName preserves the platform's filesystem encoding.
            const auto encoded = QFile::encodeName(path);
            auto decoded = audio::decodeAudioFile(std::filesystem::path(encoded.constData()));
            return {std::make_shared<const audio::DecodedAudio>(std::move(decoded)), {}};
        } catch (const std::exception& error) {
            return {{}, QString::fromUtf8(error.what())};
        } catch (...) {
            return {{}, "Audio decoding failed."};
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
        session_.reset();
    }
    ready_ = false;
}

void DeckController::loaded()
{
    if (closing_)
        return;
    const auto result = loader_.result();
    busy_ = false;
    if (!result.audio) {
        fail(result.error);
        return;
    }
    stopSession();
    // No callback can refer to the old owner after stopSession returns.
    clips_[pendingDeck_] = result.audio;
    names_[pendingDeck_] = pendingName_;
    state_.sourceFrame[pendingDeck_] = 0;
    state_.sourceFrames[pendingDeck_] = result.audio->frames();
    state_.playing[pendingDeck_] = false;
    state_.deckGain[pendingDeck_] = 1.0F;
    const auto span = [&](std::size_t deck) {
        return clips_[deck] ? clips_[deck]->samples() : std::span<const float>{};
    };
    audio::LiveSession::InitialState initial;
    initial.sourceFrame = state_.sourceFrame;
    initial.playing = state_.playing;
    initial.deckGain = state_.deckGain;
    initial.masterGain = state_.masterGain;
    try {
        session_ = std::make_unique<audio::LiveSession>(span(0), span(1), initial);
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

void DeckController::submit(audio::RenderCore::Command command)
{
    if (closing_ || !ready_ || !session_) {
        fail("Audio output is not ready.");
        return;
    }
    if (!session_->submit(command)) {
        fail("Audio control queue is full. Try the control again.");
        return;
    }
    ++submitted_;
    error_.clear();
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

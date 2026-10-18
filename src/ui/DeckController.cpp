// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui/DeckController.hpp"
#include "library/PreparedTrackStore.hpp"

#include <QFileInfo>
#include <QFile>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace conflux {
namespace {
QString portKey(const audio::PortIdentity& port)
{
    return QString::number(port.globalId) + ":" + QString::number(port.serial) + ":" +
        QString::number(port.nodeId) + ":" + QString::number(port.nodeSerial);
}

QVariantMap meterMap(const audio::RenderCore::StereoMeter& meter)
{
    return {{"peakLeft", meter.peak[0]}, {"peakRight", meter.peak[1]},
            {"rmsLeft", meter.rms[0]}, {"rmsRight", meter.rms[1]},
            {"overrangeLeft", QVariant::fromValue<qulonglong>(meter.overrangeSamples[0])},
            {"overrangeRight", QVariant::fromValue<qulonglong>(meter.overrangeSamples[1])}};
}
} // namespace

DeckController::DeckController(QObject* parent) : QObject(parent)
{
    connect(&loader_, &QFutureWatcher<LoadResult>::finished, this, &DeckController::loaded);
    connect(&catalogJob_, &QFutureWatcher<CatalogResult>::finished, this, &DeckController::catalogFinished);
    connect(&installer_, &QFutureWatcher<std::shared_ptr<InstallResult>>::finished, this, &DeckController::installed);
    connect(&retirer_, &QFutureWatcher<void>::finished, this, &DeckController::retired);
    connect(&recordingStarter_, &QFutureWatcher<RecordingStart>::finished, this, &DeckController::recordingStarted);
    connect(&recordingFinalizer_, &QFutureWatcher<RecordingCompletion>::finished, this, &DeckController::recordingFinished);
    connect(&pollTimer_, &QTimer::timeout, this, &DeckController::poll);
    pollTimer_.start(50);
    const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    libraryPath_ = data.isEmpty() ? QString{} : data + "/library.json";
    catalogJob_.setFuture(QtConcurrent::run([path = libraryPath_]() -> CatalogResult {
        try {
            if (path.isEmpty()) throw std::runtime_error("Cannot find the local library directory");
            return {library::LibraryCatalog(path).read(), {}, false};
        } catch (const std::exception& error) {
            return {{}, QString::fromUtf8(error.what()), false};
        }
    }));
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
    disconnect(&catalogJob_, nullptr, this, nullptr);
    disconnect(&installer_, nullptr, this, nullptr);
    disconnect(&retirer_, nullptr, this, nullptr);
    disconnect(&recordingStarter_, nullptr, this, nullptr);
    disconnect(&recordingFinalizer_, nullptr, this, nullptr);
    loadCancellation_.request_stop();
    stopSession();
    discovery_.reset();
    // Preparation checks cancellation between bounded reads/conversion/writes.
    // Join before releasing the controller and decoded owners.
    loader_.waitForFinished();
    catalogJob_.waitForFinished();
    installer_.waitForFinished();
    retirer_.waitForFinished();
    if (loader_.future().resultCount() > 0) {
        const auto loaded = loader_.result();
        if (loaded.prepared && loaded.prepared->reader) {
            auto reader = std::move(loaded.prepared->reader);
            auto cleanup = QtConcurrent::run([reader = std::move(reader)]() { reader->stop(); });
            cleanup.waitForFinished();
        }
    }
    pendingLoad_.reset();
    pendingRetired_.reset();
    if (installer_.future().resultCount() > 0) {
        const auto pending = installer_.result();
        if (pending && pending->session) pending->session->stop();
    }
    // stopSession() acknowledged detachment after callback quiescence. Neither
    // a pending open nor a finalizer may outlive shutdown or its writer owner.
    recordingStarter_.waitForFinished();
    if (recordingStartPending_ && recordingStarter_.future().resultCount() > 0) {
        const auto result = recordingStarter_.result();
        recorder_ = result.writer;
        if (!recorder_) {
            recordingError_ = result.error;
            recordingStatus_ = "Failed";
        }
        recordingStartPending_ = false;
    }
    if (recorder_) {
        recorder_->stop();
        lastRecording_ = {QString::fromStdString(recorder_->error()), recorder_->acceptedFrames(),
            recorder_->writtenFrames(), recorder_->overflowCount(), recorder_->overflowFrames()};
        recorder_.reset();
        recordingError_ = lastRecording_.error;
        recordingStatus_ = recordingError_.isEmpty() ? "Complete" : "Failed";
    }
    recordingFinalizer_.waitForFinished();
    if (recordingFinalizationPending_ && recordingFinalizer_.future().resultCount() > 0) {
        lastRecording_ = recordingFinalizer_.result();
        recordingError_ = lastRecording_.error;
        recordingStatus_ = recordingError_.isEmpty() ? "Complete" : "Failed";
        recordingFinalizationPending_ = false;
    }
    busy_ = false;
    preparationStatus_ = "Idle";
    preparationCacheHit_ = false;
}

QVariantList DeckController::decks() const
{
    QVariantList result;
    for (std::size_t deck = 0; deck < clips_.size(); ++deck) {
        result.append(QVariantMap{
            {"name", names_[deck]}, {"loaded", bool(clips_[deck])},
            {"replacing", replacing_[deck]},
            {"sourceGeneration", QVariant::fromValue<qulonglong>(state_.sourceGeneration[deck])},
            {"waveform", waveforms_[deck]},
            {"playing", state_.playing[deck]},
            {"cueSelected", state_.cueSelected[deck]},
            {"meter", meterMap(state_.meters.deck[deck])},
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

QVariantMap DeckController::meters() const
{
    return {{"master", meterMap(state_.meters.master)}, {"cue", meterMap(state_.meters.cue)},
            {"blockFrames", QVariant::fromValue<qulonglong>(state_.meters.blockFrames)}};
}

QVariantMap DeckController::audioRouting() const
{
    QVariantList ports;
    for (const auto& port : routingSnapshot_.playbackPorts) {
        if (!audio::isPlaybackPort(port)) continue;
        ports.append(QVariantMap{{"key", portKey(port)},
            {"label", QString::fromStdString(port.nodeName + " · " + port.portName + " [" + port.channel + "]")}});
    }
    return {{"ports", ports}, {"masterReady", ready_ && routingSnapshot_.masterReady},
        {"cueReady", ready_ && state_.cueReady},
        {"masterLeft", routingConfig_.master ? portKey(routingConfig_.master->left) : QString{}},
        {"masterRight", routingConfig_.master ? portKey(routingConfig_.master->right) : QString{}},
        {"cueLeft", routingConfig_.cue ? portKey(routingConfig_.cue->left) : QString{}},
        {"cueRight", routingConfig_.cue ? portKey(routingConfig_.cue->right) : QString{}},
        {"defaultSink", QString::fromStdString(routingSnapshot_.defaultSinkName)},
        {"masterError", QString::fromStdString(routingSnapshot_.masterError)},
        {"cueError", QString::fromStdString(routingSnapshot_.cueError)}};
}

QVariantMap DeckController::recording() const
{
    return {{"status", recordingStatus_}, {"path", recordingPath_}, {"error", recordingError_},
        {"active", bool(recorder_) || recordingStatus_ == "Starting" || recordingStatus_ == "Stopping" ||
                   recordingStatus_ == "Finalizing"},
        {"acceptedFrames", QVariant::fromValue<qulonglong>(recorder_ ? recorder_->acceptedFrames() : lastRecording_.accepted)},
        {"writtenFrames", QVariant::fromValue<qulonglong>(recorder_ ? recorder_->writtenFrames() : lastRecording_.written)},
        {"overflowFrames", QVariant::fromValue<qulonglong>(recorder_ ? recorder_->overflowFrames() : lastRecording_.overflowFrames)}};
}

void DeckController::fail(const QString& error)
{
    error_ = error;
    emit stateChanged();
}

QVariantMap DeckController::preparation() const
{
    const bool idle = preparationStatus_ == "Idle";
    return {{"status", preparationStatus_}, {"deck", idle || pendingImport_ ? -1 : pendingDeck_},
            {"name", idle ? QString{} : pendingName_},
            {"cancellable", !closing_ && preparationStatus_ == "Preparing"},
            {"cacheHit", preparationCacheHit_}};
}

QVariantList DeckController::libraryTracks() const { return libraryTracks_; }
bool DeckController::libraryReady() const { return libraryReady_; }
QString DeckController::libraryError() const { return libraryError_; }

void DeckController::catalogFinished()
{
    if (closing_) return;
    auto result = catalogJob_.result();
    libraryError_ = result.error;
    if (result.error.isEmpty()) {
        libraryEntries_ = std::move(result.tracks);
        libraryTracks_.clear();
        libraryTracks_.reserve(qsizetype(libraryEntries_.size()));
        for (const auto& track : libraryEntries_)
            libraryTracks_.append(QVariantMap{{"id", track.sourceSha256}, {"name", track.name},
                {"path", track.sourcePath}, {"durationSeconds", track.frames / 48000.0},
                {"available", track.available}});
        libraryReady_ = true;
    }
    // Startup reads do not own transport/preparation state. Only the save of
    // an accepted import owns busy and its terminal preparation phase.
    if (result.saving) {
        busy_ = false;
        preparationStatus_ = result.error.isEmpty() ? "Complete" : "Failed";
        error_ = result.error;
    }
    emit libraryChanged();
    if (!closing_) emit stateChanged();
}

void DeckController::importTrack(const QUrl& url)
{
    if (closing_) return;
    if (!libraryReady_) {
        fail(libraryError_.isEmpty() ? "Wait for the library to finish opening."
                                     : "Library unavailable: " + libraryError_);
        return;
    }
    // The live session still polls handoffs during metadata-only import.
    // Keep its internal deck index valid; expose -1 through preparation().
    beginPreparation(0, url, true);
}

void DeckController::loadLibraryTrack(int deck, const QString& sourceId)
{
    if (closing_) return;
    if (!libraryReady_) {
        fail("The local library is not available.");
        return;
    }
    const auto found = std::ranges::find_if(libraryEntries_, [&](const auto& track) {
        return track.sourceSha256 == sourceId;
    });
    if (found == libraryEntries_.end()) {
        fail("This track is not in the local library.");
        return;
    }
    beginPreparation(deck, QUrl::fromLocalFile(found->sourcePath), false, found->sourceSha256);
}

bool DeckController::cancelPreparation()
{
    if (closing_ || preparationStatus_ != "Preparing") return false;
    preparationStatus_ = "Cancelling";
    loadCancellation_.request_stop();
    emit stateChanged();
    return true;
}

void DeckController::loadDeck(int deck, const QUrl& url)
{
    beginPreparation(deck, url, false);
}

void DeckController::beginPreparation(int deck, const QUrl& url, bool importOnly, const QString& expectedSource)
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
        !QFileInfo(url.toLocalFile()).isAbsolute() || url.toLocalFile().contains(QChar(u'\0'))) {
        fail("Enter an absolute local file path.");
        return;
    }
    pendingDeck_ = deck;
    const auto path = url.toLocalFile();
    pendingName_ = QFileInfo(path).fileName();
    const auto baseCache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (baseCache.isEmpty()) {
        preparationStatus_ = "Failed";
        fail("Cannot find a local cache directory for audio preparation.");
        return;
    }
    const auto cache = baseCache + "/prepared/v1";
    busy_ = true;
    error_.clear();
    loadCancellation_ = std::stop_source{};
    preparationStatus_ = "Preparing";
    preparationCacheHit_ = false;
    pendingImport_ = importOnly;
    loader_.setFuture(QtConcurrent::run([path, cache, importOnly, expectedSource, stop = loadCancellation_.get_token()]() -> LoadResult {
        try {
            library::PreparedTrackStore store(cache);
            auto track = store.prepare(path, stop);
            if (stop.stop_requested()) throw std::runtime_error("Audio preparation cancelled");
            if (!expectedSource.isEmpty() && track.sourceSha256 != expectedSource)
                throw std::runtime_error("Source changed; import this file again before loading its library entry.");
            if (importOnly) {
                library::LibraryTrack entry{track.sourceSha256, path, QFileInfo(path).fileName(),
                    track.artifactKey, track.audio.frames(), true};
                return {nullptr, {}, std::move(entry), track.cacheHit};
            }
            QVariantList waveform;
            waveform.reserve(qsizetype(track.audio.peaks().size()));
            for (const auto peak : track.audio.peaks()) waveform.append(peak);
            auto prepared = std::make_shared<PreparedLoad>();
            prepared->cacheHit = track.cacheHit;
            prepared->audio = std::make_shared<const audio::PreparedAudio>(std::move(track.audio));
            prepared->waveform = std::move(waveform);
            prepared->reader = std::make_unique<audio::PcmReadAhead>(prepared->audio->descriptor(), prepared->audio->frames());
            if (!prepared->reader->prime(0, std::chrono::seconds(2)))
                throw std::runtime_error("Cannot prime the prepared track: " + prepared->reader->error());
            if (stop.stop_requested()) throw std::runtime_error("Audio preparation cancelled");
            return {std::move(prepared), {}, {}, track.cacheHit};
        } catch (const std::exception& error) {
            return {{}, QString::fromUtf8(error.what()), {}, false};
        } catch (...) {
            return {{}, "Audio decoding failed.", {}, false};
        }
    }));
    // A direct observer may cancel or shut down from stateChanged. Publish
    // cancellability only after this job owns its new token and future.
    emit stateChanged();
}

void DeckController::stopSession()
{
    // Retirement can complete synchronously. Observers must see teardown only
    // after the stopped session's accounting and ownership are consistent.
    const QSignalBlocker notifications(this);
    if (session_) {
        session_->stop();
        state_ = session_->stoppedState();
        collectReplacement();
        settleReplacement(true);
        completedTiming_.push_back(session_->stoppedTimingSummary());
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
    if (loadCancellation_.stop_requested()) {
        error_.clear();
        preparationCacheHit_ = false;
        std::unique_ptr<audio::PcmReadAhead> reader;
        std::shared_ptr<const audio::PreparedAudio> clip;
        if (result.prepared) {
            reader = std::move(result.prepared->reader);
            clip = std::move(result.prepared->audio);
        }
        // The watcher shares PreparedLoad with this result. Remove both owners
        // from it, then join/close on the existing retirement worker.
        retireReader(std::move(reader), std::move(clip));
        return;
    }
    if (result.imported) {
        preparationStatus_ = "Saving";
        preparationCacheHit_ = result.cacheHit;
        // Own the write before a direct observer can shut down from Saving.
        // Cancellation has ended; shutdown joins this accepted atomic save.
        catalogJob_.setFuture(QtConcurrent::run([path = libraryPath_, track = *result.imported]() -> CatalogResult {
            try {
                return {library::LibraryCatalog(path).upsert(track), {}, true};
            } catch (const std::exception& error) {
                return {{}, QString::fromUtf8(error.what()), true};
            }
        }));
        emit stateChanged();
        return;
    }
    if (!result.prepared) {
        busy_ = false;
        preparationStatus_ = "Failed";
        fail(result.error);
        return;
    }
    // This is the cancellation cutoff, before metadata or live handoff changes.
    preparationStatus_ = "Installing";
    preparationCacheHit_ = result.prepared->cacheHit;
    emit stateChanged();
    if (closing_) return;
    pendingLoad_ = result.prepared;
    if (session_) {
        replacing_[pendingDeck_] = true;
        pendingGeneration_ = ++nextSourceGeneration_;
        // End the old gesture before publication. A queue-full release is
        // retried while only this deck's ordinary controls are blocked.
        cueRelease(pendingDeck_);
        publishReplacement();
        emit stateChanged();
        return;
    }
    stopSession();
    // No callback can refer to the old owner after stopSession returns.
    clips_[pendingDeck_] = result.prepared->audio;
    names_[pendingDeck_] = pendingName_;
    waveforms_[pendingDeck_] = result.prepared->waveform;
    state_.sourceFrame[pendingDeck_] = 0;
    state_.cueFrame[pendingDeck_] = 0;
    state_.cueHeld[pendingDeck_] = state_.cuePreview[pendingDeck_] = false;
    state_.buffering[pendingDeck_] = state_.sourceReadError[pendingDeck_] = false;
    state_.sourceReadErrorObserved[pendingDeck_] = false;
    state_.starvedFrames[pendingDeck_] = 0;
    state_.sourceFrames[pendingDeck_] = result.prepared->audio->frames();
    state_.sourceGeneration[pendingDeck_] = 0;
    state_.playing[pendingDeck_] = false;
    state_.deckGain[pendingDeck_] = 1.0F;
    state_.cueSelected[pendingDeck_] = false;
    installSession();
}

void DeckController::installSession()
{
    audio::LiveSession::InitialState initial;
    initial.sourceFrame = state_.sourceFrame;
    initial.cueFrame = state_.cueFrame;
    initial.playing = state_.playing;
    initial.deckGain = state_.deckGain;
    initial.masterGain = state_.masterGain;
    initial.cueSelected = state_.cueSelected;
    initial.cueGain = state_.cueGain;
    // Capture the exact quiesced cursor first. Priming can involve slow storage,
    // so keep the installing stage busy and construct its readers off the GUI.
    std::unique_ptr<audio::PcmReadAhead> unusedReader;
    if (pendingLoad_) unusedReader = std::move(pendingLoad_->reader);
    pendingLoad_.reset();
    installer_.setFuture(QtConcurrent::run([clips = clips_, initial, unusedReader = std::move(unusedReader)]() mutable {
        auto result = std::make_shared<InstallResult>();
        try {
            // First-load/reconfiguration constructors prime their own readers.
            // Dispose of an unused candidate on this same worker, not the GUI.
            unusedReader.reset();
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

void DeckController::publishReplacement()
{
    if (!pendingLoad_ || !replacing_[pendingDeck_] || replacementPublished_ ||
        !session_ || !session_->ready()) return;
    if (!flushCueReleases()) return;
    replacementPublished_ = session_->requestSourceReplacement(std::size_t(pendingDeck_),
        pendingLoad_->reader, pendingGeneration_);
}

void DeckController::collectReplacement()
{
    if (!session_) return;
    auto result = session_->takeSourceReplacementResult();
    if (!result) return;
    if (result->completion.installed) {
        completedStarvedFrames_ += result->completion.retired.starvedFrames;
        completedReadFailures_ += result->completion.retired.readFailed ? 1 : 0;
        pendingRetired_ = std::move(result->retiredReader);
        replacementAcknowledged_ = true;
    } else {
        auto clip = pendingLoad_ ? pendingLoad_->audio : nullptr;
        pendingLoad_.reset();
        replacing_[pendingDeck_] = false;
        replacementPublished_ = replacementAcknowledged_ = false;
        pendingGeneration_ = 0;
        replacementSnapshot_.reset();
        preparationStatus_ = "Failed";
        retireReader(std::move(result->retiredReader), std::move(clip));
        if (!closing_ && session_->ready())
            fail("The prepared track could not be installed; the previous track remains loaded.");
    }
}

void DeckController::settleReplacement(bool stopped)
{
    if (!replacing_[pendingDeck_] || !pendingLoad_) return;
    if (!replacementPublished_ && stopped) {
        auto reader = std::move(pendingLoad_->reader);
        auto clip = pendingLoad_->audio;
        pendingLoad_.reset();
        replacing_[pendingDeck_] = false;
        pendingGeneration_ = 0;
        replacementSnapshot_.reset();
        preparationStatus_ = "Failed";
        retireReader(std::move(reader), std::move(clip));
        return;
    }
    if (!replacementAcknowledged_ || state_.sourceGeneration[pendingDeck_] != pendingGeneration_) return;
    auto oldClip = std::move(clips_[pendingDeck_]);
    clips_[pendingDeck_] = pendingLoad_->audio;
    names_[pendingDeck_] = pendingName_;
    waveforms_[pendingDeck_] = pendingLoad_->waveform;
    pendingLoad_.reset();
    replacing_[pendingDeck_] = false;
    cueDown_[pendingDeck_] = cueReleasePending_[pendingDeck_] = false;
    replacementPublished_ = replacementAcknowledged_ = false;
    pendingGeneration_ = 0;
    replacementSnapshot_.reset();
    retireReader(std::move(pendingRetired_), std::move(oldClip));
}

void DeckController::retireReader(std::unique_ptr<audio::PcmReadAhead> reader,
                                std::shared_ptr<const audio::PreparedAudio> clip)
{
    if (!reader && !clip) { retired(); return; }
    retirer_.setFuture(QtConcurrent::run([reader = std::move(reader), clip = std::move(clip)]() mutable {
        reader.reset(); // stop/join and close the duplicated FD on this worker
        clip.reset();
    }));
}

void DeckController::retired()
{
    if (!retirer_.isRunning() && !pendingLoad_ &&
        !replacing_[0] && !replacing_[1]) {
        busy_ = false;
        if (preparationStatus_ == "Cancelling") preparationStatus_ = "Cancelled";
        else if (preparationStatus_ == "Installing") preparationStatus_ = "Complete";
    }
    if (!closing_) emit stateChanged();
}

void DeckController::installed()
{
    if (closing_) return;
    const auto result = installer_.result();
    busy_ = false;
    const bool installingPreparation = preparationStatus_ == "Installing";
    if (installingPreparation) preparationStatus_ = "Complete";
    if (!result->session) {
        if (installingPreparation) preparationStatus_ = "Failed";
        recordingStopRequested_ = true;
        pollRecording();
        fail(result->error);
        return;
    }
    try {
        discovery_.reset();
        session_ = std::move(result->session);
        state_ = session_->stoppedState();
        std::string error;
        if (recorder_ && !recordingStopRequested_)
            session_->requestRecorder(recorder_.get());
        if (!session_->start(error, routingConfig_)) {
            if (installingPreparation) preparationStatus_ = "Failed";
            stopSession();
            recordingStopRequested_ = true;
            pollRecording();
            fail(QString::fromStdString(error));
            return;
        }
    } catch (const std::exception& error) {
        if (installingPreparation) preparationStatus_ = "Failed";
        stopSession();
        recordingStopRequested_ = true;
        pollRecording();
        fail(QString::fromUtf8(error.what()));
        return;
    }
    error_.clear();
    emit stateChanged();
}

void DeckController::poll()
{
    if (!session_) {
        if (discovery_) {
            routingSnapshot_ = discovery_->routingSnapshot();
            const auto error = discovery_->error();
            if (!error.empty()) routingSnapshot_.masterError = error;
            emit stateChanged();
        }
        pollRecording();
        return;
    }
    flushCueReleases();
    audio::LiveSession::State snapshot;
    // The queue is bounded; GUI delay can drop intermediate snapshots. Drain at
    // most its capacity, keeping the newest complete published state.
    for (std::size_t index = 0; index < 32 && session_->pollState(snapshot); ++index) {
        if (replacing_[pendingDeck_] && snapshot.sourceGeneration[pendingDeck_] == pendingGeneration_)
            replacementSnapshot_ = snapshot;
        else state_ = snapshot;
    }
    collectReplacement();
    if (closing_ || !session_) return;
    if (replacementAcknowledged_ && replacementSnapshot_) state_ = *replacementSnapshot_;
    settleReplacement();
    // Completion can notify a direct observer that shuts down this controller.
    if (closing_ || !session_) return;
    const auto error = session_->error();
    if (!error.empty()) {
        stopSession();
        recordingStopRequested_ = true;
        pollRecording();
        fail(QString::fromStdString(error));
        return;
    }
    ready_ = session_->ready();
    publishReplacement();
    routingSnapshot_ = session_->routingSnapshot();
    pollRecording();
    emit stateChanged();
}

bool DeckController::submit(audio::RenderCore::Command command)
{
    if (closing_ || !ready_ || !session_) {
        fail("Audio output is not ready.");
        return false;
    }
    const bool global = command.type == audio::RenderCore::Command::Type::MasterGain ||
                        command.type == audio::RenderCore::Command::Type::CueGain;
    if (!global && command.deck < replacing_.size() && replacing_[command.deck]) {
        fail("Wait for this deck's prepared track to finish installing.");
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

bool DeckController::jogFrames(int deck, qint64 deltaFrames)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck]) {
        fail("Load this deck before using the jog wheel.");
        return false;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::Jog;
    command.deck = std::uint32_t(deck);
    command.deltaFrames = deltaFrames;
    return submit(command);
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

void DeckController::setCueGain(double gain)
{
    if (!std::isfinite(gain) || gain < 0 || gain > 1) {
        fail("Headphone cue level must be between zero and one.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::CueGain;
    command.gain = float(gain);
    submit(command);
}

void DeckController::setCueSelected(int deck, bool selected)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck]) {
        fail("Load this deck before selecting headphone cue.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = selected ? audio::RenderCore::Command::Type::CueOn : audio::RenderCore::Command::Type::CueOff;
    command.deck = std::uint32_t(deck);
    submit(command);
}

void DeckController::refreshAudioRouting()
{
    if (closing_) return;
    if (session_) {
        routingSnapshot_ = session_->routingSnapshot();
    } else {
        try {
            if (discovery_ && !discovery_->error().empty()) discovery_.reset();
            if (!discovery_) {
                discovery_ = std::make_unique<audio::LiveSession>();
                std::string error;
                if (!discovery_->startDiscovery(error)) {
                    routingSnapshot_.masterError = error;
                    discovery_.reset();
                }
            }
            if (discovery_) routingSnapshot_ = discovery_->routingSnapshot();
        } catch (const std::exception& error) {
            routingSnapshot_.masterError = error.what();
            discovery_.reset();
        }
    }
    emit stateChanged();
}

void DeckController::configureAudioRouting(const QString& masterLeft, const QString& masterRight,
                                         const QString& cueLeft, const QString& cueRight)
{
    if (closing_) return;
    if (busy_) {
        fail("Wait for preparation before applying audio outputs.");
        return;
    }
    if (session_) routingSnapshot_ = session_->routingSnapshot();
    else if (discovery_) routingSnapshot_ = discovery_->routingSnapshot();
    auto find = [&](const QString& key) -> std::optional<audio::PortIdentity> {
        for (const auto& port : routingSnapshot_.playbackPorts)
            if (audio::isPlaybackPort(port) && portKey(port) == key) return port;
        return std::nullopt;
    };
    audio::RoutingConfig config;
    auto pair = [&](const QString& left, const QString& right, std::optional<audio::RoutePair>& output) {
        if (left.isEmpty() && right.isEmpty()) return true;
        const auto l = find(left), r = find(right);
        if (!l || !r) return false;
        output = audio::RoutePair{*l, *r};
        return true;
    };
    if (!pair(masterLeft, masterRight, config.master) || !pair(cueLeft, cueRight, config.cue)) {
        fail("An output port is missing. Refresh and select both channels again.");
        return;
    }
    const auto resolved = audio::resolveAudioRouting(routingSnapshot_, config);
    if (!resolved.master || (config.cue && !resolved.cue)) {
        fail(QString::fromStdString(!resolved.master ? resolved.masterError : resolved.cueError));
        return;
    }
    // Runtime identities protect against ID reuse. Reconfiguration intentionally
    // interrupts output, then restores the exact stopped transport state.
    routingConfig_ = std::move(config);
    if (!clips_[0] && !clips_[1]) {
        error_.clear();
        emit stateChanged();
        return;
    }
    busy_ = true;
    stopSession();
    discovery_.reset();
    installSession();
}

void DeckController::startRecording(const QUrl& url)
{
    if (closing_) return;
    if (!ready_ || !session_) {
        recordingError_ = "Audio output must be ready before recording.";
        emit stateChanged();
        return;
    }
    if (recorder_ || recordingStartPending_ || recordingFinalizationPending_) {
        recordingError_ = "Wait for the current recording to finish.";
        emit stateChanged();
        return;
    }
    if (!url.isLocalFile() || (!url.host().isEmpty() && url.host() != "localhost") ||
        !QFileInfo(url.toLocalFile()).isAbsolute()) {
        recordingError_ = "Enter a new absolute local WAV path.";
        emit stateChanged();
        return;
    }
    recordingPath_ = url.toLocalFile();
    recordingError_.clear();
    recordingStatus_ = "Starting";
    recordingStopRequested_ = false;
    recordingStartPending_ = true;
    lastRecording_ = {};
    const auto path = recordingPath_;
    recordingStarter_.setFuture(QtConcurrent::run([path]() -> RecordingStart {
        try {
            const auto encoded = QFile::encodeName(path);
            return {std::make_shared<audio::StereoRecorder>(std::filesystem::path(encoded.constData())), {}};
        } catch (const std::exception& error) {
            return {{}, QString::fromUtf8(error.what())};
        } catch (...) { return {{}, "Cannot create a recording."}; }
    }));
    emit stateChanged();
}

void DeckController::recordingStarted()
{
    if (closing_) return;
    const auto result = recordingStarter_.result();
    recordingStartPending_ = false;
    recorder_ = result.writer;
    if (!recorder_) {
        recordingError_ = result.error;
        recordingStatus_ = "Failed";
    } else {
        if (!session_ || !ready_) recordingStopRequested_ = true;
        pollRecording();
    }
    emit stateChanged();
}

void DeckController::stopRecording()
{
    if (closing_) return;
    if (!recorder_ && !recordingStartPending_) return;
    recordingStopRequested_ = true;
    recordingStatus_ = "Stopping";
    pollRecording();
    emit stateChanged();
}

void DeckController::pollRecording()
{
    if (!recorder_) return;
    if (recorder_->failed()) {
        recordingError_ = QString::fromStdString(recorder_->error());
        recordingStopRequested_ = true;
    }
    if (recordingStopRequested_) {
        recordingStatus_ = "Stopping";
        if (!session_) {
            finalizeRecording();
        } else if (!session_->recorderChangePending()) {
            if (session_->recorderAttached()) session_->requestRecorder(nullptr);
            else finalizeRecording();
        }
    } else if (session_ && !session_->recorderChangePending()) {
        if (session_->recorderAttached()) recordingStatus_ = "Recording";
        else session_->requestRecorder(recorder_.get());
    }
}

void DeckController::finalizeRecording()
{
    // The old session is quiescent, or a detach has completed at callback exit.
    // Joining the disk writer belongs to a worker, never the GUI/audio thread.
    auto writer = std::move(recorder_);
    if (!writer) return;
    recordingStatus_ = "Finalizing";
    recordingFinalizationPending_ = true;
    recordingFinalizer_.setFuture(QtConcurrent::run([writer = std::move(writer)]() -> RecordingCompletion {
        writer->stop();
        return {QString::fromStdString(writer->error()), writer->acceptedFrames(), writer->writtenFrames(),
                writer->overflowCount(), writer->overflowFrames()};
    }));
}

void DeckController::recordingFinished()
{
    if (closing_) return;
    recordingFinalizationPending_ = false;
    lastRecording_ = recordingFinalizer_.result();
    recordingError_ = lastRecording_.error;
    recordingStatus_ = recordingError_.isEmpty() ? "Complete" : "Failed";
    emit stateChanged();
}
} // namespace conflux

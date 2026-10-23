// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeckController.hpp"
#include "library/PreparedTrackStore.hpp"
#include "library/CueSheet.hpp"

#include <QFileInfo>
#include <QFile>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>
#include <stdexcept>
#include <thread>

namespace conflux {
namespace {
// Process-lifetime identity, separate from any controller/session generation.
// Saturate before wrap so even a failed preparation never permits token reuse.
std::uint64_t nextLiveSourceToken()
{
    static std::atomic<std::uint64_t> last{0};
    auto current = last.load(std::memory_order_relaxed);
    for (;;) {
        if (current == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Live stem source identity exhausted");
        if (last.compare_exchange_weak(current, current + 1, std::memory_order_relaxed))
            return current + 1;
    }
}

QString withLiveStemFailure(QString error, const std::shared_ptr<library::LiveStemObservations>& observations)
{
    if (observations) {
        const auto detail = observations->failureDetail();
        if (!detail.empty()) error += "; " + QString::fromStdString(detail);
    }
    return error;
}

bool nextScratchToken(std::uint64_t& result) noexcept
{
    static std::atomic<std::uint64_t> last{0};
    auto current = last.load(std::memory_order_relaxed);
    for (;;) {
        if (current == std::numeric_limits<std::uint64_t>::max()) return false;
        if (last.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
            result = current + 1;
            return true;
        }
    }
}

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

QString preparationPhaseText(const QString& phase)
{
    if (phase == "decode-cache") return "Decoding / validating audio cache";
    if (phase == "stems") return "Preparing requested stems";
    if (phase == "bpm-prepare") return "Preparing original mix for BPM analysis";
    if (phase == "bpm") return "Analyzing original-mix BPM";
    if (phase == "prime") return "Priming playback";
    if (phase == "install") return "Installing prepared track";
    if (phase == "levels") return "Measuring audio levels";
    if (phase == "cues") return "Reading cue sheet";
    if (phase == "library") return "Saving library entry";
    if (phase == "cancel") return "Cancelling preparation";
    if (phase == "continuation-cleanup") return "Waiting for the saved-stem check to finish";
    return {};
}

QString continuationStatusText(const QString& status)
{
    if (status == "Checking") return "Checking saved stems";
    if (status == "Installing") return "Continuing with saved stems";
    if (status == "Active") return "Using saved stems";
    if (status == "Unavailable") return "Saved stem continuation unavailable";
    if (status == "Cancelled") return "Saved stem check cancelled";
    return {};
}

QVariantMap presentationMap(const library::TrackPresentation& presentation)
{
    return {{"title", presentation.title}, {"artist", presentation.artist},
            {"album", presentation.album}, {"artworkDataUrl", presentation.artworkDataUrl}};
}
} // namespace

DeckController::PreparedPcm::PreparedPcm(audio::PreparedAudio audio)
{
    audioLeaves.push_back(std::move(audio));
}

DeckController::PreparedPcm::PreparedPcm(std::array<audio::PreparedAudio, 4> stems)
{
    const auto commonFrames = stems.front().frames();
    if (!commonFrames || commonFrames > audio::PreparedAudio::defaultMaxPreparedBytes / 8)
        throw std::invalid_argument("Invalid prepared stem geometry");
    audioLeaves.reserve(stems.size());
    for (auto& stem : stems) {
        if (stem.frames() != commonFrames)
            throw std::invalid_argument("Prepared stem frame counts differ");
        audioLeaves.push_back(std::move(stem));
    }
}

DeckController::PreparedPcm::PreparedPcm(std::uint64_t frames, std::vector<float> sourcePeaks, QString sourceSha256,
    std::shared_ptr<library::LiveStemObservations> observations, QString sourcePath, QString artifactKey,
    std::optional<audio::AudioResidentHoldRange> residentHold)
    : liveFrames(frames), livePeaks(std::move(sourcePeaks)), liveSourceSha256(std::move(sourceSha256)),
      observations(std::move(observations)), sourcePath(std::move(sourcePath)), artifactKey(std::move(artifactKey)),
      residentHold(std::move(residentHold))
{
    if (!liveFrames || livePeaks.empty() || liveSourceSha256.isEmpty() ||
        std::ranges::any_of(livePeaks, [](float peak) { return !std::isfinite(peak) || peak < 0.F || peak > 1.F; }))
        throw std::invalid_argument("Invalid live stem source metadata");
    if (this->residentHold) {
        const auto& range = *this->residentHold;
        if (!range.id || range.provenance != audio::AudioResidentHoldRange::Provenance::DjRange ||
            range.inFrame >= range.outFrame || range.outFrame > liveFrames ||
            range.outFrame - range.inFrame < audio::AudioResidentHoldRange::minimumFrames ||
            range.outFrame - range.inFrame > audio::AudioResidentHoldRange::maximumFrames ||
            range.admittedBytes < (range.outFrame - range.inFrame) * audio::AudioResidentHoldRange::bytesPerFrame)
            throw std::invalid_argument("Invalid retained live recovery range metadata");
    }
}

std::uint64_t DeckController::PreparedPcm::bytes() const noexcept
{
    // Live input/slots/spool are already charged once by the persistent store
    // reservation, including while this metadata is pending or retiring.
    // A terminal producer can clean its disk job while this resident payload
    // still belongs to a pending/active/retiring playback. Keep that owner
    // charged here too; active reservation + metadata conservatively overcount.
    return live() ? (residentHold ? (residentHold->outFrame - residentHold->inFrame) *
        audio::AudioResidentHoldRange::bytesPerFrame : 0) : frames() * 8 * audioLeaves.size();
}

std::unique_ptr<audio::PreparedPlayback> DeckController::PreparedPcm::makePlayback() const
{
    if (live()) throw std::logic_error("Live stem owner was lost; reload this deck.");
    if (!stemBank())
        return std::make_unique<audio::PreparedPlayback>(audioLeaves.front().descriptor(), frames());
    std::array<int, 4> descriptors{};
    for (std::size_t stem = 0; stem < descriptors.size(); ++stem)
        descriptors[stem] = audioLeaves[stem].descriptor();
    return std::make_unique<audio::PreparedPlayback>(descriptors, frames());
}

DeckController::DeckController(QObject* parent) : DeckController(PreparationOptions{}, parent) {}

DeckController::DeckController(PreparationOptions options, QObject* parent)
    : QObject(parent), preparationOptions_(std::move(options))
{
    // Constructor validates caller-tightened limits without file/process work.
    static_cast<void>(library::PreparedTrackStore(QStringLiteral("configuration"), preparationOptions_.limits));
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
    foregroundPreparation_.reset();
    foregroundWaiting_ = false;
    pollTimer_.stop();
    disconnect(&loader_, nullptr, this, nullptr);
    disconnect(&catalogJob_, nullptr, this, nullptr);
    disconnect(&installer_, nullptr, this, nullptr);
    disconnect(&retirer_, nullptr, this, nullptr);
    disconnect(&recordingStarter_, nullptr, this, nullptr);
    disconnect(&recordingFinalizer_, nullptr, this, nullptr);
    loadCancellation_.request_stop();
    installCancellation_.request_stop();
    // Qt's unfinished-future wait may steal its QRunnable onto this caller.
    // Never execute queued preparation, owner joins or writer cleanup on GUI.
    // Shutdown may block, but does not pump events or borrow another scheduler.
    const auto waitForWorker = [](auto& watcher) {
        while (!watcher.isFinished()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        watcher.waitForFinished(); // Already complete: exception/result semantics only.
    };
    stopSession();
    discovery_.reset();
    // Both jobs own cancellation independently. An old cancelled load token
    // must not cancel a later routing rebuild. Join before gathering results.
    waitForWorker(loader_);
    waitForWorker(catalogJob_);
    waitForWorker(installer_);
    installationPending_ = false;
    if (installer_.future().resultCount() > 0) {
        const auto pending = installer_.result();
        if (pending && pending->session) {
            auto stopped = pending->session->stopAndTakePlaybacks();
            // An unstarted installer cannot have accepted a replacement.
            if (stopped.replacement) throw std::logic_error("Unexpected installer replacement owner");
            for (std::size_t deck = 0; deck < stoppedPlaybacks_.size(); ++deck) {
                if (stoppedPlaybacks_[deck]) throw std::logic_error("Playback owner already parked");
                stoppedPlaybacks_[deck] = std::move(stopped.active[deck]);
            }
            pending->session.reset(); // Detached; no producer stop/join here.
        }
    }
    // Finish the prior ordinary/deferred retirement before the final bounded
    // teardown. Each completion can launch at most one fixed deferred batch.
    while (retirementPending_) {
        waitForWorker(retirer_);
        retired();
    }
    if (loader_.future().resultCount() > 0) {
        const auto loaded = loader_.result();
        if (loaded.prepared)
            retirePlayback(std::move(loaded.prepared->playback), std::move(loaded.prepared->audio));
    }
    // The loader result and pendingLoad_ may be the same shared PreparedLoad;
    // moving its fields above leaves these empty rather than duplicating them.
    if (pendingLoad_)
        retirePlayback(std::move(pendingLoad_->playback), std::move(pendingLoad_->audio));
    pendingLoad_.reset();
    if (continuation_) {
        if (continuation_->prepared)
            retirePlayback(std::move(continuation_->prepared->playback), std::move(continuation_->prepared->audio));
        retirePlayback(std::move(continuation_->retired));
        continuation_.reset();
    }
    loaderPurpose_ = LoaderPurpose::None;
    retirePlayback(std::move(pendingRetired_));
    for (std::size_t deck = 0; deck < stoppedPlaybacks_.size(); ++deck)
        if (stoppedPlaybacks_[deck])
            retirePlayback(std::move(stoppedPlaybacks_[deck]), clips_[deck]);
    while (retirementPending_) {
        waitForWorker(retirer_);
        retired();
    }
    // stopSession() acknowledged detachment after callback quiescence. Neither
    // a pending open nor a finalizer may outlive shutdown or its writer owner.
    waitForWorker(recordingStarter_);
    if (recordingStartPending_ && recordingStarter_.future().resultCount() > 0) {
        const auto result = recordingStarter_.result();
        recorder_ = result.writer;
        if (!recorder_) {
            recordingError_ = result.error;
            recordingStatus_ = "Failed";
        }
        recordingStartPending_ = false;
    }
    if (recorder_) finalizeRecording();
    waitForWorker(recordingFinalizer_);
    if (recordingFinalizationPending_ && recordingFinalizer_.future().resultCount() > 0) {
        lastRecording_ = recordingFinalizer_.result();
        recordingError_ = lastRecording_.error;
        recordingStatus_ = recordingError_.isEmpty() ? "Complete" : "Failed";
        recordingFinalizationPending_ = false;
    }
    busy_ = false;
    preparationStatus_ = "Idle";
    preparationCacheHit_ = false;
    pendingPresentation_ = {};
    pendingWaveform_.clear();
    preparationPhase_.clear();
    preparationCompletedStages_ = preparationTotalStages_ = 0;
}

QVariantList DeckController::decks() const
{
    QVariantList result;
    for (std::size_t deck = 0; deck < clips_.size(); ++deck) {
        const bool loading = preparationActive() && !pendingImport_ && int(deck) == pendingDeck_;
        result.append(QVariantMap{
            {"name", names_[deck]}, {"loaded", bool(clips_[deck])},
            {"sourceId", sourceIds_[deck]},
            {"title", presentations_[deck].title}, {"artist", presentations_[deck].artist},
            {"album", presentations_[deck].album}, {"artworkDataUrl", presentations_[deck].artworkDataUrl},
            {"bpm", QVariantMap{
                {"estimated", bpmEstimates_[deck].bpm.value_or(0.)},
                {"reference", bpmOverrides_[deck].value_or(bpmEstimates_[deck].bpm.value_or(0.))},
                {"manual", bpmOverrides_[deck].has_value()},
                {"status", bpmEstimates_[deck].status}, {"error", bpmEstimates_[deck].error}}},
            {"replacing", replacing_[deck]},
            {"residentHold", [&]() -> QVariantMap {
                const auto& authorization = residentAuthorizations_[deck];
                const bool configured = clips_[deck] && clips_[deck]->live() && clips_[deck]->residentHold.has_value();
                // Metadata can commit before installation; never label a new
                // selection with the stopped previous owner's ready/active bits.
                const auto hold = configured && state_.residentHold[deck].rangeId == clips_[deck]->residentHold->id ?
                    state_.residentHold[deck] : audio::AudioResidentHoldState{};
                QString status, text;
                if (!configured) { status = "Unavailable"; text = "No recovery section selected"; }
                else if (hold.active) { status = hold.exitPending ? "ExitPending" : "Active";
                    text = hold.exitPending ? "Recovery loop: waiting for stems" : "Recovery loop playing"; }
                else if (hold.armed) { status = hold.ready ? "Armed" : "Capturing";
                    text = hold.ready ? "Recovery ready" : "Recovery on; waiting for this section"; }
                else if (authorization.phase == ResidentAuthorization::Phase::NextPlay) {
                    status = "NextPlay"; text = "Recovery on next Play"; }
                else if (authorization.phase == ResidentAuthorization::Phase::PlayPending ||
                         authorization.phase == ResidentAuthorization::Phase::ArmPending) {
                    status = "Pending"; text = "Enabling recovery"; }
                else { status = "Cancelled"; text = "Recovery off"; }
                return {{"configured", configured}, {"status", status}, {"text", text},
                    {"detail", authorization.detail}, {"ready", hold.ready}, {"armed", hold.armed},
                    {"active", hold.active}, {"exitPending", hold.exitPending}, {"errorCode", hold.errorCode},
                    // Unconfigured decks have no recovery action ticket. An
                    // unrelated output rebuild must not change their metadata.
                    {"outputSession", configured ? QString::number(completedSessions_) : QString{}},
                    {"sourceGeneration", configured ? QString::number(state_.sourceGeneration[deck]) : QString{}},
                    {"transportRevision", configured ? QString::number(state_.transportRevision[deck]) : QString{}},
                    {"rangeId", configured ? QString::number(clips_[deck]->residentHold->id) : QString{}},
                    {"inFrame", configured ? QString::number(clips_[deck]->residentHold->inFrame) : QString{}},
                    {"outFrame", configured ? QString::number(clips_[deck]->residentHold->outFrame) : QString{}},
                    {"inSeconds", configured ? clips_[deck]->residentHold->inFrame / 48000.0 : 0.},
                    {"outSeconds", configured ? clips_[deck]->residentHold->outFrame / 48000.0 : 0.},
                    {"linearFrame", QString::number(hold.linearFrame)}, {"audibleFrame", QString::number(hold.audibleFrame)},
                    {"residentBytes", QVariant::fromValue<qulonglong>(hold.residentBytes)}};
            }()},
            {"continuation", QVariantMap{
                {"status", continuationEpisodes_[deck].status}, {"detail", continuationEpisodes_[deck].detail},
                {"text", continuationStatusText(continuationEpisodes_[deck].status)},
                {"pending", continuation_ && continuation_->deck == deck},
                {"attempts", QVariant::fromValue<qulonglong>(continuationEpisodes_[deck].attempts)},
                {"sourceFrame", QVariant::fromValue<qulonglong>(continuationEpisodes_[deck].frame)},
                {"sourceGeneration", QVariant::fromValue<qulonglong>(continuationEpisodes_[deck].sourceGeneration)},
                {"generation", QVariant::fromValue<qulonglong>(continuationEpisodes_[deck].generation)}}},
            {"scratch", QVariantMap{
                {"position", state_.scratch[deck].position},
                {"generation", QVariant::fromValue<qulonglong>(state_.scratch[deck].generation)},
                {"errorCode", state_.scratch[deck].errorCode},
                {"supported", state_.scratch[deck].supported}, {"active", state_.scratch[deck].active},
                {"pending", state_.scratch[deck].pending},
                {"scratchToken", state_.scratch[deck].scratchToken ?
                    QString::number(state_.scratch[deck].scratchToken, 16).rightJustified(16, QChar(u'0')) : QString{}}}},
            {"loading", loading},
            {"pendingName", loading ? pendingName_ : QString{}},
            {"pendingPath", loading ? pendingPath_ : QString{}},
            {"pendingPresentation", presentationMap(loading ? pendingPresentation_ : library::TrackPresentation{})},
            {"pendingWaveform", loading ? pendingWaveform_ : QVariantList{}},
            {"loadPhase", loading ? preparationPhase_ : QString{}},
            {"loadPhaseText", loading ? preparationPhaseText(preparationPhase_) : QString{}},
            {"loadCompletedStages", loading ? preparationCompletedStages_ : 0},
            {"loadTotalStages", loading ? preparationTotalStages_ : 0},
            {"loadProgress", loading && preparationTotalStages_ ?
                preparationCompletedStages_ / double(preparationTotalStages_) : 0.},
            {"sourceGeneration", QVariant::fromValue<qulonglong>(state_.sourceGeneration[deck])},
            {"waveform", waveforms_[deck]},
            {"overviewStem", clips_[deck] && clips_[deck]->observations ? "source" :
                             clips_[deck] && clips_[deck]->stemBank() ? "drums" : ""},
            {"stemBank", state_.stemBank[deck]},
            {"liveStems", clips_[deck] && clips_[deck]->live()},
            {"tempo", QVariantMap{
                {"supported", state_.tempo[deck].supported}, {"percent", state_.tempo[deck].percent},
                {"appliedPercent", state_.tempo[deck].appliedPercent}, {"keyLock", state_.tempo[deck].keyLock},
                {"active", state_.tempo[deck].active}, {"pending", state_.tempo[deck].pending},
                {"finished", state_.tempo[deck].finished}, {"errorCode", state_.tempo[deck].errorCode},
                {"generation", QVariant::fromValue<qulonglong>(state_.tempo[deck].generation)}}},
            {"loop", QVariantMap{
                {"supported", state_.loops[deck].supported}, {"inSet", state_.loops[deck].inSet},
                {"ready", state_.loops[deck].ready}, {"outSet", state_.loops[deck].outSet},
                {"active", state_.loops[deck].active}, {"cached", state_.loops[deck].cached},
                {"inSeconds", state_.loops[deck].inFrame / double(audio::RenderCore::sampleRate)},
                {"minimumOutSeconds", (state_.loops[deck].inFrame + audio::AudioLoopState::minimumFrames)
                    / double(audio::RenderCore::sampleRate)},
                {"outSeconds", state_.loops[deck].outFrame / double(audio::RenderCore::sampleRate)},
                {"limitSeconds", state_.loops[deck].limitFrame / double(audio::RenderCore::sampleRate)},
                {"residentBytes", QVariant::fromValue<qulonglong>(state_.loops[deck].residentBytes)},
                {"errorCode", state_.loops[deck].errorCode}}},
            {"stemGains", QVariantList{state_.stemGain[deck][0], state_.stemGain[deck][1],
                                       state_.stemGain[deck][2], state_.stemGain[deck][3]}},
            {"eqGainDb", QVariantList{state_.eqGainDb[deck][0], state_.eqGainDb[deck][1],
                                      state_.eqGainDb[deck][2]}},
            {"playing", state_.playing[deck]},
            {"cueSelected", state_.cueSelected[deck]},
            {"meter", meterMap(state_.meters.deck[deck])},
            {"cueSeconds", state_.cueFrame[deck] / double(audio::RenderCore::sampleRate)},
            {"cueHeld", state_.cueHeld[deck]}, {"cuePreview", state_.cuePreview[deck]},
            {"buffering", state_.buffering[deck]}, {"sourceReadError", state_.sourceReadError[deck]},
            {"starvedFrames", QVariant::fromValue<qulonglong>(state_.starvedFrames[deck])},
            {"positionSeconds", state_.sourcePosition[deck] / double(audio::RenderCore::sampleRate)},
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

std::shared_ptr<library::LiveStemObservations> DeckController::liveStemObservations(int deck) const
{
    if (deck < 0 || std::size_t(deck) >= clips_.size() || !clips_[std::size_t(deck)])
        return {};
    return clips_[std::size_t(deck)]->observations;
}

std::uint64_t DeckController::retainedPcmBytes() const
{
    // Bound raw references before dedup: clips2 + pending normal/continuation1 + ready loader1 +
    // active retirement3 + deferred retirement3. Distinct physical owners are
    // fewer; repeated references must not be allowed to overrun this storage.
    std::array<std::pair<const PreparedPcm*, std::uint64_t>, 10> owners{};
    std::size_t count = 0;
    auto append = [&](const PreparedPcm* identity, std::uint64_t bytes) {
        if (!identity) return true;
        for (std::size_t index = 0; index < count; ++index)
            if (owners[index].first == identity) {
                // A completed worker's charge snapshot can outlive its clip
                // until the GUI slot runs. Address reuse must overcount safely.
                owners[index].second = std::max(owners[index].second, bytes);
                return true;
            }
        if (count == owners.size()) return false;
        owners[count++] = {identity, bytes};
        return true;
    };
    for (const auto& clip : clips_)
        if (clip && !append(clip.get(), clip->bytes())) return std::numeric_limits<std::uint64_t>::max();
    if (pendingLoad_ && pendingLoad_->audio &&
        !append(pendingLoad_->audio.get(), pendingLoad_->audio->bytes()))
        return std::numeric_limits<std::uint64_t>::max();
    if (continuation_ && continuation_->prepared && continuation_->prepared->audio &&
        !append(continuation_->prepared->audio.get(), continuation_->prepared->audio->bytes()))
        return std::numeric_limits<std::uint64_t>::max();
    if (loader_.future().resultCount() > 0) {
        const auto result = loader_.result();
        if (result.prepared && result.prepared->audio &&
            !append(result.prepared->audio.get(), result.prepared->audio->bytes()))
            return std::numeric_limits<std::uint64_t>::max();
    }
    for (const auto& [identity, bytes] : retiringPcm_)
        if (!append(identity, bytes)) return std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < deferredRetirement_.count; ++index) {
        const auto& clip = deferredRetirement_.groups[index].clip;
        if (clip && !append(clip.get(), clip->bytes())) return std::numeric_limits<std::uint64_t>::max();
    }
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (owners[index].second > std::numeric_limits<std::uint64_t>::max() - total)
            return std::numeric_limits<std::uint64_t>::max();
        total += owners[index].second;
    }
    return total;
}

QVariantMap DeckController::preparation() const
{
    const bool idle = preparationStatus_ == "Idle";
    const bool active = preparationActive();
    return {{"status", preparationStatus_}, {"deck", idle || pendingImport_ ? -1 : pendingDeck_},
            {"name", idle ? QString{} : pendingName_},
            {"active", active}, {"sourcePath", idle ? QString{} : pendingPath_},
            {"phase", idle ? QString{} : preparationPhase_},
            {"phaseText", idle ? QString{} : preparationPhaseText(preparationPhase_)},
            // A fraction of completed stages, never an elapsed-time or byte estimate.
            {"completedStages", preparationCompletedStages_}, {"totalStages", preparationTotalStages_},
            {"progress", preparationTotalStages_ ? preparationCompletedStages_ / double(preparationTotalStages_) : 0.},
            {"indeterminate", active},
            {"cancellable", !closing_ && (preparationStatus_ == "Preparing" || preparationStatus_ == "Reading cues")},
            {"cacheHit", preparationCacheHit_},
            {"stems", !idle && pendingStems_},
            {"retainedPcmBytes", QVariant::fromValue<qulonglong>(retainedPcmBytes())}};
}

bool DeckController::preparationActive() const
{
    return !closing_ && busy_ && (preparationStatus_ == "Preparing" || preparationStatus_ == "Reading cues" ||
        preparationStatus_ == "Cancelling" || preparationStatus_ == "Installing" || preparationStatus_ == "Saving");
}

void DeckController::updatePreparation(std::uint64_t serial, QString phase, int completedStages,
                                      std::optional<library::TrackPresentation> presentation, QVariantList waveform)
{
    // Worker notifications own metadata only. Cancelled/completed or superseded
    // jobs cannot repaint a deck, including when their queued events arrive late.
    if (closing_ || serial != preparationSerial_ || loadCancellation_.stop_requested() ||
        preparationStatus_ != "Preparing" || completedStages < preparationCompletedStages_ ||
        completedStages >= preparationTotalStages_)
        return;
    preparationPhase_ = std::move(phase);
    preparationCompletedStages_ = completedStages;
    if (presentation) {
        pendingPresentation_ = std::move(*presentation);
        pendingWaveform_ = std::move(waveform);
    }
    emit stateChanged();
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
        for (const auto& track : libraryEntries_) {
            QVariantMap levels;
            if (track.levels)
                levels = {{"rms", track.levels->rms}, {"samplePeak", track.levels->samplePeak}};
            QVariantList cues;
            cues.reserve(qsizetype(track.cues.size()));
            for (const auto& cue : track.cues)
                cues.append(QVariantMap{{"trackNumber", cue.trackNumber}, {"label", cue.label},
                    {"frame", QVariant::fromValue<qulonglong>(cue.frame)},
                    {"seconds", cue.frame / double(audio::RenderCore::sampleRate)}});
            libraryTracks_.append(QVariantMap{{"id", track.sourceSha256}, {"name", track.name},
                {"path", track.sourcePath}, {"frames", QVariant::fromValue<qulonglong>(track.frames)},
                {"durationSeconds", track.frames / 48000.0},
                {"available", track.available}, {"cues", cues}, {"levels", levels}});
        }
        libraryReady_ = true;
    }
    // Startup reads do not own transport/preparation state. Only the save of
    // an accepted import owns busy and its terminal preparation phase.
    if (result.saving) {
        busy_ = false;
        preparationStatus_ = result.error.isEmpty() ? "Complete" : "Failed";
        if (result.error.isEmpty()) preparationCompletedStages_ = preparationTotalStages_;
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

void DeckController::importCueSheet(const QString& sourceId, const QUrl& url)
{
    if (closing_) return;
    if (busy_) { fail("Wait for the current operation to finish."); return; }
    if (!libraryReady_) { fail("The local library is not available."); return; }
    const auto found = std::ranges::find_if(libraryEntries_, [&](const auto& track) {
        return track.sourceSha256 == sourceId;
    });
    if (found == libraryEntries_.end()) { fail("Select a saved library track before importing cues."); return; }
    if (!url.isLocalFile() || (!url.host().isEmpty() && url.host() != "localhost") ||
        !QFileInfo(url.toLocalFile()).isAbsolute() || url.toLocalFile().contains(QChar(u'\0'))) {
        fail("Enter an absolute local CUE-sheet path.");
        return;
    }
    ForegroundPreparation request;
    request.kind = ForegroundPreparation::Kind::CueImport;
    request.url = url; request.cueTrack = *found;
    if (deferForegroundPreparation(std::move(request))) return;
    startCueImport(*found, url);
}

void DeckController::startCueImport(library::LibraryTrack selected, const QUrl& url)
{
    pendingDeck_ = 0;
    pendingImport_ = true;
    pendingStems_ = false;
    pendingName_ = QFileInfo(url.toLocalFile()).fileName();
    pendingPath_ = url.toLocalFile();
    pendingPresentation_ = {};
    pendingWaveform_.clear();
    ++preparationSerial_;
    preparationPhase_ = "cues";
    preparationCompletedStages_ = 0;
    preparationTotalStages_ = 2;
    preparationCacheHit_ = false;
    preparationStatus_ = "Reading cues";
    busy_ = true;
    error_.clear();
    loadCancellation_ = std::stop_source{};
    loaderPurpose_ = LoaderPurpose::Preparation;
    loader_.setFuture(QtConcurrent::run([path = url.toLocalFile(), selected = std::move(selected),
                                       stop = loadCancellation_.get_token()]() mutable -> LoadResult {
        try {
            auto result = library::readCueSheet(path, selected, stop);
            if (result.cancelled || !result.error.isEmpty())
                return {{}, result.cancelled ? QStringLiteral("CUE import cancelled.") : result.error, {}, false};
            selected.cues = std::move(result.cues);
            return {{}, {}, std::move(selected), false, true};
        } catch (const std::exception& error) {
            return {{}, QString::fromUtf8(error.what()), {}, false};
        } catch (...) {
            return {{}, "CUE import failed.", {}, false};
        }
    }));
    // Publish cancellation only after the worker owns its token and future.
    emit stateChanged();
}

bool DeckController::jumpToImportedCue(int deck, const QString& sourceId, int cueIndex)
{
    if (closing_) return false;
    if (busy_ || deck < 0 || deck >= int(clips_.size()) || !clips_[deck] ||
        sourceIds_[deck] != sourceId) {
        fail("Load the selected cue source on this deck before jumping.");
        return false;
    }
    const auto found = std::ranges::find_if(libraryEntries_, [&](const auto& track) {
        return track.sourceSha256 == sourceId;
    });
    if (found == libraryEntries_.end() || cueIndex < 0 ||
        std::size_t(cueIndex) >= found->cues.size() || found->cues[std::size_t(cueIndex)].frame >= clips_[deck]->frames()) {
        fail("This imported cue is outside the loaded track.");
        return false;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::Seek;
    command.deck = std::uint32_t(deck);
    command.frame = found->cues[std::size_t(cueIndex)].frame;
    return submit(command);
}

void DeckController::loadLibraryTrack(int deck, const QString& sourceId)
{ loadLibrary(deck, sourceId, false); }

void DeckController::loadLibraryStemTrack(int deck, const QString& sourceId)
{ loadLibrary(deck, sourceId, true); }

void DeckController::loadLibraryStemTrackWithRecoveryRange(int deck, const QString& sourceId,
                                                            double inSeconds, double outSeconds)
{
    if (closing_) return;
    if (!libraryReady_) { fail("The local library is not available."); return; }
    const auto found = std::ranges::find_if(libraryEntries_, [&](const auto& track) {
        return track.sourceSha256 == sourceId;
    });
    if (found == libraryEntries_.end() || !found->available || !preparationOptions_.stemEngine ||
        preparationOptions_.stemServicePath.isEmpty()) {
        fail("Choose an available library source with live stem preparation configured."); return;
    }
    if (!std::isfinite(inSeconds) || !std::isfinite(outSeconds) || inSeconds < 0 ||
        outSeconds <= inSeconds || outSeconds > found->frames / 48000.0) {
        fail("Recovery bounds must lie inside the selected source."); return;
    }
    library::LiveStemResidentHold request;
    request.sourceSha256 = found->sourceSha256;
    auto& range = request.range;
    range.inFrame = std::uint64_t(std::llround(inSeconds * 48000.0));
    range.outFrame = std::uint64_t(std::llround(outSeconds * 48000.0));
    if (range.inFrame >= range.outFrame || range.outFrame > found->frames ||
        range.outFrame - range.inFrame < audio::AudioResidentHoldRange::minimumFrames ||
        range.outFrame - range.inFrame > audio::AudioResidentHoldRange::maximumFrames) {
        fail("Choose a recovery section between 0.042667 and 8 seconds."); return;
    }
    range.admittedBytes = (range.outFrame - range.inFrame) * audio::AudioResidentHoldRange::bytesPerFrame;
    range.provenance = audio::AudioResidentHoldRange::Provenance::DjRange;
    try { range.id = nextLiveSourceToken(); }
    catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); return; }
    beginPreparation(deck, QUrl::fromLocalFile(found->sourcePath), false, found->sourceSha256, true, request);
}

void DeckController::loadLibrary(int deck, const QString& sourceId, bool stems)
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
    beginPreparation(deck, QUrl::fromLocalFile(found->sourcePath), false, found->sourceSha256, stems);
}

bool DeckController::cancelPreparation()
{
    if (closing_ || (preparationStatus_ != "Preparing" && preparationStatus_ != "Reading cues")) return false;
    preparationStatus_ = "Cancelling";
    preparationPhase_ = "cancel";
    foregroundPreparation_.reset();
    loadCancellation_.request_stop();
    drainForegroundPreparation();
    emit stateChanged();
    return true;
}

void DeckController::loadDeck(int deck, const QUrl& url)
{
    beginPreparation(deck, url, false);
}

void DeckController::loadStemDeck(int deck, const QUrl& url)
{ beginPreparation(deck, url, false, {}, true); }

void DeckController::beginPreparation(int deck, const QUrl& url, bool importOnly,
                                      const QString& expectedSource, bool stems,
                                      std::optional<library::LiveStemResidentHold> residentHold)
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
    if (stems && !preparationOptions_.stemEngine) {
        fail("Prepared stems require a configured local stem engine. Normal loading is available.");
        return;
    }
    if (residentHold) {
        const auto& range = residentHold->range;
        // Reject malformed optional intent before it can reserve the shared
        // foreground worker or cancel an otherwise valid automatic probe.
        if (importOnly || !stems || expectedSource.isEmpty() || residentHold->sourceSha256 != expectedSource ||
            !range.id || range.provenance != audio::AudioResidentHoldRange::Provenance::DjRange ||
            range.inFrame >= range.outFrame ||
            range.outFrame - range.inFrame < audio::AudioResidentHoldRange::minimumFrames ||
            range.outFrame - range.inFrame > audio::AudioResidentHoldRange::maximumFrames) {
            fail("A recovery range must belong to this selected library stem source and have valid bounds.");
            return;
        }
        const auto bytes = (range.outFrame - range.inFrame) * audio::AudioResidentHoldRange::bytesPerFrame;
        if (bytes > preparationOptions_.limits.maxPreparedBytes || range.admittedBytes < bytes) {
            fail("The selected recovery range exceeds the admitted PCM byte budget.");
            return;
        }
    }
    ForegroundPreparation request;
    request.residentHold = residentHold;
    request.deck = deck; request.url = url; request.importOnly = importOnly;
    request.expectedSource = expectedSource; request.stems = stems;
    if (deferForegroundPreparation(std::move(request))) return;
    auto limits = preparationOptions_.limits;
    const auto retained = retainedPcmBytes();
    // Reserve all retained PCM, including still-linked owners. This intentionally
    // double-counts linked active leaves, covering corrupt-entry unlink/rebuild.
    if (retained >= limits.maxCacheBytes) {
        fail("Retained playback leaves no local preparation byte budget.");
        return;
    }
    limits.maxCacheBytes -= retained;
    pendingDeck_ = deck;
    const auto path = url.toLocalFile();
    pendingName_ = QFileInfo(path).fileName();
    pendingPath_ = path;
    const auto baseCache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (baseCache.isEmpty()) {
        preparationStatus_ = "Failed";
        fail("Cannot find a local cache directory for audio preparation.");
        return;
    }
    const auto cache = baseCache + "/prepared/v1";
    const auto bpmCache = baseCache + "/analysis/bpm-v1";
    busy_ = true;
    error_.clear();
    loadCancellation_ = std::stop_source{};
    preparationStatus_ = "Preparing";
    preparationCacheHit_ = false;
    pendingImport_ = importOnly;
    pendingStems_ = stems;
    const auto engine = preparationOptions_.stemEngine;
    const auto service = preparationOptions_.stemServicePath;
    const auto bpmEngine = preparationOptions_.bpmEngine;
    pendingPresentation_ = {};
    pendingWaveform_.clear();
    const auto serial = ++preparationSerial_;
    preparationPhase_ = stems ? "stems" : "decode-cache";
    preparationCompletedStages_ = 0;
    preparationTotalStages_ = importOnly ? 3 : 3 + int(bpmEngine.has_value());
    const auto report = [this, serial](QString phase, int completedStages,
        std::optional<library::TrackPresentation> presentation = {}, QVariantList waveform = {}) {
        QMetaObject::invokeMethod(this, [this, serial, phase = std::move(phase), completedStages,
            presentation = std::move(presentation), waveform = std::move(waveform)]() mutable {
            updatePreparation(serial, std::move(phase), completedStages,
                              std::move(presentation), std::move(waveform));
        }, Qt::QueuedConnection);
    };
    loaderPurpose_ = LoaderPurpose::Preparation;
    if (!importOnly) revokeResidentAuthorization(std::size_t(deck), "A new track preparation was accepted.");
    loader_.setFuture(QtConcurrent::run([path, cache, bpmCache, importOnly, expectedSource, stems, engine, service, bpmEngine, limits, residentHold,
                                       report, stop = loadCancellation_.get_token()]() -> LoadResult {
        try {
            library::PreparedTrackStore store(cache, limits);
            auto prepared = std::make_shared<PreparedLoad>();
            int completedStages = 0;
            const auto preview = [&](const library::TrackPresentation& presentation, std::span<const float> peaks) {
                prepared->waveform.reserve(qsizetype(peaks.size()));
                for (const auto peak : peaks) prepared->waveform.append(peak);
                report(stems ? "stems" : "decode-cache", ++completedStages, presentation, prepared->waveform);
            };
            QString sourceId;
            if (stems) {
                auto bank = service.isEmpty() ?
                    std::optional<library::PreparedStemTrack>(store.prepareStemBank(path, *engine, stop)) :
                    store.findPreparedStemBank(path, *engine, stop);
                if (bank) {
                    sourceId = bank->sourceSha256;
                    prepared->cacheHit = bank->cacheHit;
                    prepared->presentation = std::move(bank->presentation);
                    auto pcm = std::make_shared<PreparedPcm>(std::move(bank->stems));
                    pcm->sourcePath = path; pcm->artifactKey = bank->artifactKey;
                    prepared->audio = std::move(pcm);
                } else {
                    // Only a genuine lookup miss reaches the service. A valid
                    // bank never inspects this otherwise unused sixth asset.
                    auto track = store.prepareLiveStem(path, *engine, service, nextLiveSourceToken(), stop, residentHold);
                    sourceId = track.sourceSha256;
                    prepared->presentation = std::move(track.presentation);
                    prepared->audio = std::make_shared<const PreparedPcm>(track.outputFrames,
                        std::move(track.sourceOverview), track.sourceSha256, std::move(track.observations),
                        path, track.artifactKey, track.residentHold);
                    prepared->playback = std::move(track.playback);
                }
                preview(prepared->presentation, prepared->audio->peaks());
                QString analyzedSource;
                if (bpmEngine) {
                    // Timing always uses the original mix, never a drums leaf
                    // or the downsampled overview. Admit playback first so an
                    // optional analysis cache entry cannot exhaust its budget.
                    try {
                        report("bpm-prepare", completedStages);
                        auto original = store.prepare(path, stop);
                        analyzedSource = original.sourceSha256;
                        report("bpm", completedStages);
                        prepared->bpm = library::analyzeBpmCached(original.audio, *bpmEngine, bpmCache, stop);
                    } catch (const library::BpmSourceError&) {
                        throw;
                    } catch (const std::exception&) {
                        if (stop.stop_requested()) throw;
                        prepared->bpm = {{}, "Failed", "Original-mix BPM analysis could not complete."};
                    }
                    ++completedStages;
                }
                if (!analyzedSource.isEmpty() && analyzedSource != sourceId)
                    prepared->bpm = {{}, "Failed", "Source changed during BPM analysis."};
            } else {
                auto track = store.prepare(path, stop);
                sourceId = track.sourceSha256;
                prepared->cacheHit = track.cacheHit;
                prepared->presentation = std::move(track.presentation);
                preview(prepared->presentation, track.audio.peaks());
                if (importOnly) {
                    report("levels", completedStages);
                    const auto levels = audio::measurePreparedLevels(track.audio, stop);
                    if (stop.stop_requested()) throw std::runtime_error("Audio preparation cancelled");
                    library::LibraryTrack entry{track.sourceSha256, path, QFileInfo(path).fileName(),
                        track.artifactKey, track.audio.frames(), true, {}, levels};
                    return {nullptr, {}, std::move(entry), track.cacheHit};
                }
                if (bpmEngine) {
                    report("bpm", completedStages);
                    prepared->bpm = library::analyzeBpmCached(track.audio, *bpmEngine, bpmCache, stop);
                    ++completedStages;
                }
                prepared->audio = std::make_shared<const PreparedPcm>(std::move(track.audio));
            }
            if (stop.stop_requested()) throw std::runtime_error("Audio preparation cancelled");
            if (!expectedSource.isEmpty() && sourceId != expectedSource)
                throw std::runtime_error("Source changed; import this file again before loading its library entry.");
            prepared->sourceId = sourceId;
            report("prime", completedStages);
            if (!prepared->playback) prepared->playback = prepared->audio->makePlayback();
            const auto primeWait = prepared->audio->live() ? audio::PreparedPlayback::liveWait : std::chrono::seconds(2);
            if (!prepared->playback->prime(0, primeWait, stop))
                throw std::runtime_error(withLiveStemFailure(
                    QString::fromStdString("Cannot prime the prepared track: " + prepared->playback->error()),
                    prepared->audio->observations).toStdString());
            if (stop.stop_requested()) throw std::runtime_error("Audio preparation cancelled");
            report("prime", ++completedStages);
            const auto hit = prepared->cacheHit;
            return {std::move(prepared), {}, {}, hit};
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

bool DeckController::deferForegroundPreparation(ForegroundPreparation request)
{
    if (loaderPurpose_ != LoaderPurpose::Continuation && !continuation_ &&
        !retirementPending_ && !deferredRetirement_.count) return false;
    if (foregroundWaiting_ || foregroundPreparation_) throw std::logic_error("Foreground preparation slot already reserved");
    // Inputs were structurally validated by the public entry point. Reserve the
    // one accepted user request BEFORE cancellation or a direct GUI observer.
    // Keep the old job's token/future until its finished slot consumes it.
    pendingDeck_ = request.deck;
    pendingImport_ = request.kind == ForegroundPreparation::Kind::CueImport || request.importOnly;
    pendingStems_ = request.stems;
    pendingPath_ = request.url.toLocalFile(); pendingName_ = QFileInfo(pendingPath_).fileName();
    pendingPresentation_ = {}; pendingWaveform_.clear();
    ++preparationSerial_;
    preparationStatus_ = request.kind == ForegroundPreparation::Kind::CueImport ? "Reading cues" : "Preparing";
    preparationPhase_ = "continuation-cleanup";
    preparationCompletedStages_ = 0;
    preparationTotalStages_ = request.kind == ForegroundPreparation::Kind::CueImport ? 2 :
        request.importOnly ? 3 : 3 + int(preparationOptions_.bpmEngine.has_value());
    preparationCacheHit_ = false;
    if (request.kind == ForegroundPreparation::Kind::Track && !request.importOnly)
        revokeResidentAuthorization(std::size_t(request.deck), "A new track preparation was accepted.");
    foregroundPreparation_ = std::move(request);
    foregroundWaiting_ = true; busy_ = true; error_.clear();
    if (continuation_) cancelContinuation(continuation_->deck, "A new preparation was accepted.");
    emit stateChanged();
    return true;
}

void DeckController::drainForegroundPreparation()
{
    if (closing_ || !foregroundWaiting_ || loaderPurpose_ != LoaderPurpose::None || continuation_ ||
        retirementPending_ || deferredRetirement_.count || installationPending_ || pendingLoad_ ||
        replacing_[0] || replacing_[1]) return;
    foregroundWaiting_ = false;
    if (!foregroundPreparation_) {
        // A cancelled lookup/miss may return no owner, so no retirement watcher
        // will finish. The loader-consumption path must settle this reservation.
        busy_ = false; preparationStatus_ = "Cancelled"; preparationPhase_ = "cancel";
        emit stateChanged();
        return;
    }
    auto request = std::move(*foregroundPreparation_);
    foregroundPreparation_.reset();
    // No signal or event pumping between releasing this private reservation and
    // normal launch. Observers see either the accepted request still busy or its
    // actual terminal error; never a false idle window that drops the request.
    busy_ = false; preparationStatus_ = "Failed"; preparationPhase_.clear();
    if (request.kind == ForegroundPreparation::Kind::CueImport) {
        if (!request.cueTrack) throw std::logic_error("Queued CUE import lost its selected track");
        startCueImport(std::move(*request.cueTrack), request.url);
    } else beginPreparation(request.deck, request.url, request.importOnly, request.expectedSource,
        request.stems, std::move(request.residentHold));
}

void DeckController::resetResidentAuthorization(std::size_t deck)
{
    auto& authorization = residentAuthorizations_[deck]; authorization = {};
    if (clips_[deck] && clips_[deck]->live() && clips_[deck]->residentHold) {
        authorization.clip = clips_[deck].get(); authorization.phase = ResidentAuthorization::Phase::NextPlay;
        authorization.detail = "Approved for the next Play. Playback controls turn recovery off.";
    }
}

void DeckController::revokeResidentAuthorization(std::size_t deck, const QString& reason)
{
    if (deck >= residentAuthorizations_.size() || !clips_[deck] || !clips_[deck]->residentHold) return;
    auto& authorization = residentAuthorizations_[deck]; authorization.phase = ResidentAuthorization::Phase::Cancelled;
    authorization.detail = reason.left(1024);
}

bool DeckController::residentTicket(int deck, const QString& sourceId, const QString& outputSession,
                                    const QString& rangeId, const QString& generation, const QString& revision) const
{
    return !closing_ && ready_ && session_ && deck >= 0 && deck < int(clips_.size()) &&
        clips_[deck] && clips_[deck]->live() && clips_[deck]->residentHold && !replacing_[deck] &&
        completedSessions_ != std::numeric_limits<std::uint64_t>::max() &&
        outputSession == QString::number(completedSessions_) && rangeId == QString::number(clips_[deck]->residentHold->id) &&
        sourceId == sourceIds_[deck] && generation == QString::number(state_.sourceGeneration[deck]) &&
        revision == QString::number(state_.transportRevision[deck]) &&
        state_.transportRevision[deck] < std::numeric_limits<std::uint64_t>::max() - 1 &&
        state_.residentHold[deck].supported && state_.residentHold[deck].rangeId == clips_[deck]->residentHold->id;
}

bool DeckController::queueResidentArm(std::size_t deck)
{
    if (!session_ || !ready_ || !clips_[deck] || !clips_[deck]->residentHold ||
        submitted_ >= UINT32_MAX || completedSessions_ == std::numeric_limits<std::uint64_t>::max() ||
        state_.transportRevision[deck] >= std::numeric_limits<std::uint64_t>::max() - 1) return false;
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::ResidentHoldArm;
    command.deck = std::uint32_t(deck); command.sourceGeneration = state_.sourceGeneration[deck];
    command.transportRevision = state_.transportRevision[deck]; command.residentHoldToken = clips_[deck]->residentHold->id;
    if (!submit(command)) return false;
    cancelContinuation(deck, "The approved recovery section was enabled.");
    auto& authorization = residentAuthorizations_[deck];
    authorization.clip = clips_[deck].get(); authorization.session = session_.get();
    authorization.outputSession = completedSessions_;
    authorization.generation = state_.sourceGeneration[deck]; authorization.revision = state_.transportRevision[deck] + 1;
    authorization.commandId = submitted_; authorization.phase = ResidentAuthorization::Phase::ArmPending;
    authorization.detail = "Enabling the approved recovery section.";
    return true;
}

bool DeckController::authorizeResidentHold(int deck, const QString& sourceId,
                                           const QString& outputSession, const QString& rangeId,
                                           const QString& generation, const QString& revision)
{
    if (!residentTicket(deck,sourceId,outputSession,rangeId,generation,revision) || state_.residentHold[deck].active ||
        submitted_ >= std::uint64_t(UINT32_MAX) - 1) return false;
    auto& authorization = residentAuthorizations_[deck];
    if (authorization.phase == ResidentAuthorization::Phase::PlayPending ||
        authorization.phase == ResidentAuthorization::Phase::ArmPending) return false;
    if (state_.playing[deck] || state_.cuePreview[deck]) return queueResidentArm(std::size_t(deck));
    authorization.clip = clips_[deck].get(); authorization.phase = ResidentAuthorization::Phase::NextPlay;
    authorization.detail = "Enabled for the next Play. Playback controls turn recovery off.";
    cancelContinuation(std::size_t(deck), "Recovery was enabled for the next Play.");
    emit stateChanged(); return true;
}

bool DeckController::cancelResidentHold(int deck, const QString& sourceId,
                                        const QString& outputSession, const QString& rangeId,
                                        const QString& generation, const QString& revision)
{
    if (!residentTicket(deck,sourceId,outputSession,rangeId,generation,revision)) return false;
    const auto pending = residentAuthorizations_[deck];
    const auto hold = state_.residentHold[deck];
    // A queued Arm is FIFO-before this Cancel. Use its exact expected result
    // revision, never an unguarded cancellation of a later source or transport.
    const auto expected = pending.phase == ResidentAuthorization::Phase::ArmPending ? pending.revision :
        state_.transportRevision[deck];
    if (!hold.armed && !hold.active && pending.phase != ResidentAuthorization::Phase::ArmPending) {
        revokeResidentAuthorization(std::size_t(deck), "You turned recovery off.");
        emit stateChanged(); return true;
    }
    audio::RenderCore::Command command; command.type = audio::RenderCore::Command::Type::ResidentHoldCancel;
    command.deck = std::uint32_t(deck); command.sourceGeneration = state_.sourceGeneration[deck];
    command.transportRevision = expected; command.residentHoldToken = clips_[deck]->residentHold->id;
    if (!submit(command)) return false; // Preserve current authorization on queue failure.
    revokeResidentAuthorization(std::size_t(deck), "Recovery off requested.");
    return true;
}

bool DeckController::exitResidentHold(int deck, const QString& sourceId,
                                      const QString& outputSession, const QString& rangeId,
                                      const QString& generation, const QString& revision)
{
    if (!residentTicket(deck,sourceId,outputSession,rangeId,generation,revision) || !state_.residentHold[deck].active) return false;
    audio::RenderCore::Command command; command.type = audio::RenderCore::Command::Type::ResidentHoldExit;
    command.deck = std::uint32_t(deck); command.sourceGeneration = state_.sourceGeneration[deck];
    command.transportRevision = state_.transportRevision[deck]; command.residentHoldToken = clips_[deck]->residentHold->id;
    if (!submit(command)) return false;
    residentAuthorizations_[deck].detail = "Resume requested for the end of this loop, when stems are ready.";
    return true;
}

void DeckController::pollResidentAuthorization()
{
    for (std::size_t deck = 0; deck < clips_.size(); ++deck) {
        auto& authorization = residentAuthorizations_[deck];
        using Phase = ResidentAuthorization::Phase;
        const auto& hold = state_.residentHold[deck];
        if (authorization.phase == Phase::Armed && !hold.armed && !hold.active) {
            authorization.phase = Phase::Cancelled;
            authorization.detail = "Live stems resumed. Recovery is off.";
        } else if (authorization.phase == Phase::Cancelled && !hold.armed && !hold.active &&
                   authorization.detail == "Recovery off requested.") {
            authorization.detail = "You turned recovery off.";
        }
        if (authorization.phase != Phase::PlayPending && authorization.phase != Phase::ArmPending) continue;
        if (authorization.clip != clips_[deck].get() || authorization.session != session_.get() ||
            authorization.outputSession != completedSessions_ || authorization.generation != state_.sourceGeneration[deck]) {
            revokeResidentAuthorization(deck, "The track or audio output changed. Enable recovery again when ready."); continue;
        }
        if (state_.lastCommandId < authorization.commandId) continue;
        if (state_.transportRevision[deck] != authorization.revision ||
            (!state_.playing[deck] && !state_.cuePreview[deck])) {
            revokeResidentAuthorization(deck, "Playback changed before recovery was enabled. Enable it again when ready."); continue;
        }
        if (authorization.phase == Phase::PlayPending) {
            if (!queueResidentArm(deck)) revokeResidentAuthorization(deck, "Recovery could not be enabled. Try again from the current position.");
        } else if (state_.residentHold[deck].armed) {
            authorization.phase = Phase::Armed; authorization.detail = "Only your approved section will repeat if stems become unavailable.";
        } else revokeResidentAuthorization(deck, "Recovery could not be enabled. Try again from the current position.");
    }
}

bool DeckController::continuationEligible(std::size_t deck) const
{
    if (closing_ || !ready_ || !session_ || !session_->ready() || deck >= clips_.size() ||
        !clips_[deck] || !clips_[deck]->live() || !preparationOptions_.stemEngine ||
        (!state_.playing[deck] && !state_.cuePreview[deck]) || !state_.buffering[deck] ||
        state_.sourceFrames[deck] != clips_[deck]->frames() || state_.sourceFrame[deck] >= clips_[deck]->frames() ||
        state_.sourcePosition[deck] != double(state_.sourceFrame[deck]) ||
        state_.transportRevision[deck] == std::numeric_limits<std::uint64_t>::max()) return false;
    const auto& tempo = state_.tempo[deck];
    const auto& loop = state_.loops[deck];
    return !tempo.active && !tempo.pending && !tempo.keyLock && tempo.percent == 0 && tempo.appliedPercent == 0 &&
        !state_.scratch[deck].active && !loop.inSet && !loop.outSet && !loop.active && !loop.cached;
}

bool DeckController::continuationCurrent() const
{
    if (!continuation_ || continuation_->revoked) return false;
    const auto& attempt = *continuation_;
    const auto deck = attempt.deck;
    return session_.get() == attempt.session && clips_[deck] == attempt.original && continuationEligible(deck) &&
        sourceIds_[deck] == attempt.original->liveSourceSha256 &&
        state_.sourceGeneration[deck] == attempt.sourceGeneration && state_.transportRevision[deck] == attempt.revision &&
        state_.sourceFrame[deck] == attempt.frame;
}

void DeckController::cancelContinuation(std::size_t deck, const QString& reason)
{
    if (!continuation_ || continuation_->deck != deck) return;
    continuation_->revoked = true;
    if (loaderPurpose_ == LoaderPurpose::Continuation) loadCancellation_.request_stop();
    auto& episode = continuationEpisodes_[deck];
    episode.status = "Cancelled"; episode.detail = reason.left(1024);
    // A published owner belongs to the callback mailbox until acquired, even
    // after a newer human intent. A checking worker retains its own cancellation
    // token/result until loaded() consumes it; neither may be overwritten here.
}

void DeckController::considerContinuation()
{
    if (continuation_ && !continuation_->published && !continuationCurrent())
        cancelContinuation(continuation_->deck, "The live source or transport changed during the check.");
    for (std::size_t deck = 0; deck < clips_.size(); ++deck) {
        if (continuation_ && continuation_->deck == deck) continue;
        auto& episode = continuationEpisodes_[deck];
        if (!clips_[deck] || !clips_[deck]->live()) continue;
        if (!continuationEligible(deck)) {
            // Healthy progress or a deliberate non-rendering state ends this
            // stall. An unchanged failed/buffering source does not rearm a miss.
            if (!state_.buffering[deck] || state_.sourceFrame[deck] != episode.frame ||
                state_.transportRevision[deck] != episode.revision) {
                episode.attempted = false; episode.stalled.invalidate();
                if (!state_.buffering[deck] && (state_.playing[deck] || state_.cuePreview[deck]) &&
                    state_.sourceFrame[deck] > episode.frame) {
                    episode.status = "Idle"; episode.detail.clear();
                }
            }
            continue;
        }
        if (episode.clip != clips_[deck].get() || episode.sourceGeneration != state_.sourceGeneration[deck] ||
            episode.revision != state_.transportRevision[deck] || episode.frame != state_.sourceFrame[deck]) {
            const auto attempts = episode.clip == clips_[deck].get() ? episode.attempts : 0;
            episode = {};
            episode.clip = clips_[deck].get(); episode.sourceGeneration = state_.sourceGeneration[deck];
            episode.revision = state_.transportRevision[deck]; episode.frame = state_.sourceFrame[deck];
            episode.attempts = attempts; episode.stalled.start();
        }
        if (!episode.stalled.isValid()) episode.stalled.start();
        if (episode.attempted || (!state_.sourceReadError[deck] &&
            (!episode.stalled.isValid() || episode.stalled.elapsed() < 4095))) continue;
        if (busy_ || loaderPurpose_ != LoaderPurpose::None || continuation_ || installationPending_ || pendingLoad_ ||
            replacing_[0] || replacing_[1] || retirementPending_ || deferredRetirement_.count ||
            session_->sourceReplacementPending()) continue;
        episode.attempted = true;
        if (episode.attempts != std::numeric_limits<std::uint64_t>::max()) ++episode.attempts;
        episode.status = "Unavailable";
        const auto& original = clips_[deck];
        const auto digest = [](const QString& value) {
            return value.size() == 64 && std::all_of(value.cbegin(), value.cend(), [](QChar c) {
                return (c >= QChar(u'0') && c <= QChar(u'9')) || (c >= QChar(u'a') && c <= QChar(u'f'));
            });
        };
        const auto baseCache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        auto limits = preparationOptions_.limits;
        const auto retained = retainedPcmBytes();
        if (baseCache.isEmpty() || original->sourcePath.isEmpty() || !digest(original->artifactKey) ||
            !digest(original->liveSourceSha256) || sourceIds_[deck] != original->liveSourceSha256) {
            episode.detail = "The loaded source has no exact prepared-bank identity."; continue;
        }
        if (retained >= limits.maxCacheBytes) {
            episode.detail = "Retained playback leaves no prepared continuation byte budget."; continue;
        }
        if (nextSourceGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
            episode.detail = "Prepared continuation source identity exhausted."; continue;
        }
        limits.maxCacheBytes -= retained;
        ContinuationAttempt attempt;
        attempt.deck = deck; attempt.original = original; attempt.session = session_.get();
        attempt.sourceGeneration = state_.sourceGeneration[deck]; attempt.revision = state_.transportRevision[deck];
        attempt.frame = state_.sourceFrame[deck]; attempt.generation = ++nextSourceGeneration_;
        attempt.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        episode.generation = attempt.generation;
        const auto frame = attempt.frame;
        const auto deadline = attempt.deadline;
        continuation_ = std::move(attempt);
        episode.status = "Checking"; episode.detail.clear();
        loadCancellation_ = std::stop_source{};
        loaderPurpose_ = LoaderPurpose::Continuation;
        loader_.setFuture(QtConcurrent::run([original, frame, deadline, limits,
            cache = baseCache + "/prepared/v1", engine = *preparationOptions_.stemEngine,
            parentStop = loadCancellation_.get_token()]() -> LoadResult {
            try {
                // One absolute admission cutoff includes queue delay, lookup,
                // construction and priming. Existing cooperative process cleanup
                // may finish after cancellation; no late result is publishable.
                std::stop_source admission;
                std::stop_callback cancelled(parentStop, [&] { admission.request_stop(); });
                std::mutex mutex;
                std::condition_variable_any condition;
                std::jthread expiry([&](std::stop_token stop) {
                    std::unique_lock lock(mutex);
                    (void)condition.wait_until(lock, stop, deadline, [] { return false; });
                    if (!stop.stop_requested()) admission.request_stop();
                });
                const auto check = [&] {
                    if (admission.stop_requested() || std::chrono::steady_clock::now() >= deadline)
                        throw std::runtime_error("Prepared continuation admission cancelled or timed out.");
                };
                check();
                // Cache-only discovery validates existing files. Its bounded
                // --describe is not an export or a second separator instance.
                auto bank = library::PreparedTrackStore(cache, limits).findPreparedStemBank(
                    original->sourcePath, engine, admission.get_token());
                check();
                if (!bank) return {{}, "No complete compatible prepared stem bank is available.", {}, false};
                if (bank->sourceSha256 != original->liveSourceSha256 || bank->artifactKey != original->artifactKey ||
                    std::any_of(bank->stems.begin(), bank->stems.end(), [&](const auto& leaf) {
                        return leaf.frames() != original->frames();
                    }) || frame >= original->frames())
                    throw std::runtime_error("Prepared bank identity or exact source geometry differs from the live source.");
                auto pcm = std::make_shared<PreparedPcm>(std::move(bank->stems));
                pcm->sourcePath = original->sourcePath; pcm->artifactKey = original->artifactKey;
                pcm->liveSourceSha256 = original->liveSourceSha256; pcm->observations = original->observations;
                auto prepared = std::make_shared<PreparedLoad>();
                prepared->audio = std::move(pcm); prepared->sourceId = bank->sourceSha256; prepared->cacheHit = true;
                prepared->playback = prepared->audio->makePlayback();
                check();
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0 || !prepared->playback->prime(frame,
                    std::min(std::chrono::milliseconds(2000), remaining), admission.get_token()) ||
                    !prepared->playback->primedForContinuation(frame))
                    throw std::runtime_error("Prepared continuation could not prime its exact source cursor.");
                check();
                return {std::move(prepared), {}, {}, true};
            } catch (const std::exception& error) {
                return {{}, QString::fromUtf8(error.what()).left(1024), {}, false};
            } catch (...) { return {{}, "Prepared continuation discovery failed.", {}, false}; }
        }));
        return;
    }
}

void DeckController::continuationLoaded(const LoadResult& result)
{
    if (!continuation_) throw std::logic_error("Continuation worker lost its request identity");
    auto& attempt = *continuation_;
    auto& episode = continuationEpisodes_[attempt.deck];
    const bool current = !loadCancellation_.stop_requested() && continuationCurrent() &&
        std::chrono::steady_clock::now() < attempt.deadline;
    const bool compatible = result.prepared && result.prepared->audio && result.prepared->playback &&
        !result.prepared->audio->live() && result.prepared->audio->stemBank() &&
        result.prepared->audio->frames() == attempt.original->frames() &&
        result.prepared->playback->frames() == attempt.original->frames() &&
        result.prepared->sourceId == attempt.original->liveSourceSha256 &&
        result.prepared->audio->artifactKey == attempt.original->artifactKey &&
        result.prepared->audio->observations == attempt.original->observations;
    if (!current || !compatible) {
        const bool cancelled = attempt.revoked || loadCancellation_.stop_requested();
        episode.status = cancelled ? "Cancelled" : "Unavailable";
        if (!cancelled) episode.detail = current ? (result.prepared ?
            QStringLiteral("The prepared continuation identity or geometry changed.") : result.error) :
            QStringLiteral("The source changed or admission expired before publication.");
        auto playback = result.prepared ? std::move(result.prepared->playback) : nullptr;
        auto clip = result.prepared ? std::move(result.prepared->audio) : nullptr;
        continuation_.reset();
        if (playback || clip) retirePlayback(std::move(playback), std::move(clip));
        emit stateChanged();
        return;
    }
    attempt.prepared = result.prepared;
    // Control publication and the callback both require the unused exact prime.
    // No seek/reset/stop or gain retarget is applied to this candidate here.
    attempt.published = session_->requestSourceContinuation(attempt.deck, attempt.prepared->playback,
        attempt.sourceGeneration, attempt.revision, attempt.frame, attempt.generation);
    if (!attempt.published) {
        episode.status = "Unavailable"; episode.detail = "The exact prepared continuation could not be published.";
        auto playback = std::move(attempt.prepared->playback);
        auto clip = std::move(attempt.prepared->audio);
        continuation_.reset();
        retirePlayback(std::move(playback), std::move(clip));
    } else episode.status = "Installing";
    emit stateChanged();
}

void DeckController::settleContinuation(bool stopped)
{
    if (!continuation_) return;
    auto& attempt = *continuation_;
    if (!attempt.published) {
        if (stopped) cancelContinuation(attempt.deck, "The audio session stopped during the check.");
        return;
    }
    if (!attempt.acknowledged || state_.sourceGeneration[attempt.deck] != attempt.generation) return;
    const auto deck = attempt.deck;
    auto oldClip = std::move(clips_[deck]);
    clips_[deck] = std::move(attempt.prepared->audio);
    resetResidentAuthorization(deck);
    auto retired = std::move(attempt.retired);
    auto& episode = continuationEpisodes_[deck];
    episode.status = "Active"; episode.detail.clear();
    // Same Deck, same metadata and cumulative health. Only finite storage and
    // the callback's acknowledged source generation changed. In particular do
    // not add retired health again or reset CUE/gains/EQ/BPM/waveform/recording.
    continuation_.reset();
    retirePlayback(std::move(retired), std::move(oldClip));
}

void DeckController::stopSession()
{
    if (session_)
        for (std::size_t deck = 0; deck < clips_.size(); ++deck)
            revokeResidentAuthorization(deck, "Audio output stopped. Enable recovery again when ready.");
    // Retirement can complete synchronously. Observers must see teardown only
    // after the stopped session's accounting and ownership are consistent.
    const QSignalBlocker notifications(this);
    if (continuation_) cancelContinuation(continuation_->deck, "The audio session stopped during the check.");
    if (session_) {
        auto stopped = session_->stopAndTakePlaybacks();
        state_ = session_->stoppedState();
        acceptReplacement(std::move(stopped.replacement));
        settleContinuation(true);
        settleReplacement(true);
        // Replacement metadata has now committed against the exact stopped
        // generation. Park precisely those corresponding active whole owners.
        for (std::size_t deck = 0; deck < stoppedPlaybacks_.size(); ++deck) {
            if (stoppedPlaybacks_[deck]) throw std::logic_error("Playback owner already parked");
            stoppedPlaybacks_[deck] = std::move(stopped.active[deck]);
        }
        completedTiming_.push_back(session_->stoppedTimingSummary());
        if (completedSessions_ != std::numeric_limits<std::uint64_t>::max()) ++completedSessions_;
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
    // The native owner is quiesced. Old GUI releases must never enter a later
    // session's queue, even when that session restores the same source/frame.
    scratchGestures_.fill({});
    scratchEnds_.fill({});
    for (std::size_t deck = 0; deck < clips_.size(); ++deck) {
        if (clips_[deck] && clips_[deck]->live()) {
            continuationEpisodes_[deck].attempted = false;
            continuationEpisodes_[deck].stalled.invalidate();
        }
    }
}

void DeckController::loaded()
{
    if (closing_)
        return;
    const auto result = loader_.result();
    const auto purpose = std::exchange(loaderPurpose_, LoaderPurpose::None);
    if (purpose == LoaderPurpose::Continuation) {
        continuationLoaded(result);
        drainForegroundPreparation();
        return;
    }
    if (loadCancellation_.stop_requested()) {
        error_.clear();
        preparationCacheHit_ = false;
        std::unique_ptr<audio::PreparedPlayback> playback;
        std::shared_ptr<const PreparedPcm> clip;
        if (result.prepared) {
            playback = std::move(result.prepared->playback);
            clip = std::move(result.prepared->audio);
        }
        // The watcher shares PreparedLoad with this result. Remove both owners
        // from it, then join/close on the existing retirement worker.
        retirePlayback(std::move(playback), std::move(clip));
        return;
    }
    if (result.imported) {
        preparationStatus_ = "Saving";
        preparationPhase_ = "library";
        preparationCompletedStages_ = preparationTotalStages_ - 1;
        preparationCacheHit_ = result.cacheHit;
        // Own the write before a direct observer can shut down from Saving.
        // Cancellation has ended; shutdown joins this accepted atomic save.
        catalogJob_.setFuture(QtConcurrent::run([path = libraryPath_, track = *result.imported,
                                               cues = result.cueImport]() -> CatalogResult {
            try {
                library::LibraryCatalog catalog(path);
                return {cues ? catalog.updateCues(track.sourceSha256, track.frames, track.cues) :
                               catalog.upsert(track), {}, true};
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
    preparationPhase_ = "install";
    preparationCompletedStages_ = preparationTotalStages_ - 1;
    preparationCacheHit_ = result.prepared->cacheHit;
    emit stateChanged();
    if (closing_) return;
    pendingLoad_ = result.prepared;
    if (session_) {
        queueScratchEnd(std::size_t(pendingDeck_));
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
    auto oldClip = std::move(clips_[pendingDeck_]);
    clips_[pendingDeck_] = std::move(result.prepared->audio);
    resetResidentAuthorization(std::size_t(pendingDeck_));
    continuationEpisodes_[pendingDeck_] = {};
    names_[pendingDeck_] = pendingName_;
    if (sourceIds_[pendingDeck_] != result.prepared->sourceId) bpmOverrides_[pendingDeck_].reset();
    sourceIds_[pendingDeck_] = result.prepared->sourceId;
    presentations_[pendingDeck_] = std::move(result.prepared->presentation);
    bpmEstimates_[pendingDeck_] = std::move(result.prepared->bpm);
    waveforms_[pendingDeck_] = result.prepared->waveform;
    state_.sourceFrame[pendingDeck_] = 0;
    state_.sourcePosition[pendingDeck_] = 0;
    state_.cueFrame[pendingDeck_] = 0;
    state_.cueHeld[pendingDeck_] = state_.cuePreview[pendingDeck_] = false;
    state_.buffering[pendingDeck_] = state_.sourceReadError[pendingDeck_] = false;
    state_.sourceReadErrorObserved[pendingDeck_] = false;
    state_.starvedFrames[pendingDeck_] = 0;
    state_.sourceFrames[pendingDeck_] = clips_[pendingDeck_]->frames();
    state_.stemBank[pendingDeck_] = clips_[pendingDeck_]->stemBank();
    state_.loops[pendingDeck_] = {};
    state_.tempo[pendingDeck_] = {};
    state_.scratch[pendingDeck_] = {};
    state_.stemGain[pendingDeck_].fill(state_.stemBank[pendingDeck_] ? 1.0F : 0.0F);
    state_.sourceGeneration[pendingDeck_] = 0;
    state_.playing[pendingDeck_] = false;
    state_.deckGain[pendingDeck_] = 1.0F;
    state_.cueSelected[pendingDeck_] = false;
    installSession(std::move(stoppedPlaybacks_[pendingDeck_]), std::move(oldClip));
}

void DeckController::installSession(std::unique_ptr<audio::PreparedPlayback> retiredPlayback,
                                    std::shared_ptr<const PreparedPcm> retiredClip)
{
    installationPending_ = true;
    audio::LiveSession::InitialState initial;
    initial.sourceFrame = state_.sourceFrame;
    initial.cueFrame = state_.cueFrame;
    initial.playing = state_.playing;
    initial.deckGain = state_.deckGain;
    initial.eqGainDb = state_.eqGainDb;
    initial.masterGain = state_.masterGain;
    initial.crossfader = state_.crossfader;
    initial.cueSelected = state_.cueSelected;
    initial.cueGain = state_.cueGain;
    for (std::size_t deck = 0; deck < initial.stemGain.size(); ++deck)
        if (state_.stemBank[deck]) initial.stemGain[deck] = state_.stemGain[deck];
    // Reuse both exact stopped owners and the prepared first-load candidate.
    // Reset/prime belongs to LiveSession's existing worker-side constructor.
    auto playbacks = std::move(stoppedPlaybacks_);
    if (pendingLoad_) {
        if (playbacks[pendingDeck_]) throw std::logic_error("Candidate deck still has a parked owner");
        playbacks[pendingDeck_] = std::move(pendingLoad_->playback);
    }
    pendingLoad_.reset();
    installCancellation_ = std::stop_source{};
    installer_.setFuture(QtConcurrent::run([clips = clips_, initial, tempo = state_.tempo, playbacks = std::move(playbacks),
                                           stop = installCancellation_.get_token()]() mutable {
        auto result = std::make_shared<InstallResult>();
        try {
            // A prior constructor/prime exception may have consumed its owners
            // on this worker. Only finite leaves can be recreated. Live source
            // metadata requires the existing whole owner or an explicit reload.
            for (std::size_t deck = 0; deck < clips.size(); ++deck) {
                if (!clips[deck] || playbacks[deck]) continue;
                playbacks[deck] = clips[deck]->makePlayback();
                // A failed earlier installation may have consumed a finite
                // owner. Preserve accepted logical settings on its replacement
                // instead of silently reverting a nonzero tempo or key lock.
                if (tempo[deck].percent != 0 || tempo[deck].keyLock) {
                    const auto source = playbacks[deck]->sourceRef();
                    if (!source.tempoControl || !source.tempoControl(source.context, audio::AudioTempoCommand::Set,
                            tempo[deck].percent, tempo[deck].keyLock, initial.sourceFrame[deck]))
                        throw std::runtime_error("Cannot restore prepared tempo after playback installation failure");
                }
            }
            result->session = std::make_unique<audio::LiveSession>(std::move(playbacks), initial, stop);
        } catch (const std::exception& error) {
            result->error = QString::fromUtf8(error.what());
        } catch (...) {
            result->error = "Cannot install prepared audio";
        }
        if (!result->session)
            for (std::size_t deck = 0; deck < clips.size(); ++deck) {
                result->lostLiveOwners[deck] = clips[deck] && clips[deck]->live();
                if (result->lostLiveOwners[deck])
                    result->error = withLiveStemFailure(std::move(result->error), clips[deck]->observations);
            }
        return result;
    }));
    // Establish installer ownership before allowing a completion notification.
    if (retiredPlayback || retiredClip)
        retirePlayback(std::move(retiredPlayback), std::move(retiredClip));
    emit stateChanged();
}

void DeckController::publishReplacement()
{
    if (!pendingLoad_ || !replacing_[pendingDeck_] || replacementPublished_ ||
        !session_ || !session_->ready()) return;
    if (!flushScratchEnds() || !flushCueReleases()) return;
    replacementPublished_ = session_->requestSourceReplacement(std::size_t(pendingDeck_),
        pendingLoad_->playback, pendingGeneration_);
}

void DeckController::collectReplacement()
{
    if (!session_) return;
    acceptReplacement(session_->takeSourceReplacementResult());
}

void DeckController::acceptReplacement(std::optional<audio::LiveSession::SourceReplacementResult> result)
{
    if (!result) return;
    if (result->completion.kind == audio::SourceHandoff::Kind::Continuation) {
        if (!continuation_ || result->completion.deck != continuation_->deck ||
            result->completion.generation != continuation_->generation)
            throw std::logic_error("Continuation completion lost its request identity");
        if (result->completion.installed) {
            continuation_->retired = std::move(result->retiredPlayback);
            continuation_->acknowledged = true;
        } else {
            auto& episode = continuationEpisodes_[continuation_->deck];
            episode.status = continuation_->revoked ? "Cancelled" : "Unavailable";
            if (!continuation_->revoked) episode.detail = "The source or transport no longer matched the prepared continuation.";
            auto clip = std::move(continuation_->prepared->audio);
            continuation_.reset();
            retirePlayback(std::move(result->retiredPlayback), std::move(clip));
        }
        return; // Same-deck cumulative faults remain in RenderCore, counted once.
    }
    if (result->completion.installed) {
        completedStarvedFrames_ += result->completion.retired.starvedFrames;
        completedReadFailures_ += result->completion.retired.readFailed ? 1 : 0;
        pendingRetired_ = std::move(result->retiredPlayback);
        replacementAcknowledged_ = true;
    } else {
        auto clip = pendingLoad_ ? std::move(pendingLoad_->audio) : nullptr;
        pendingLoad_.reset();
        replacing_[pendingDeck_] = false;
        replacementPublished_ = replacementAcknowledged_ = false;
        pendingGeneration_ = 0;
        replacementSnapshot_.reset();
        preparationStatus_ = "Failed";
        retirePlayback(std::move(result->retiredPlayback), std::move(clip));
        if (!closing_ && session_->ready())
            fail("The prepared track could not be installed; the previous track remains loaded.");
    }
}

void DeckController::settleReplacement(bool stopped)
{
    if (!replacing_[pendingDeck_] || !pendingLoad_) return;
    if (!replacementPublished_ && stopped) {
        auto playback = std::move(pendingLoad_->playback);
        auto clip = std::move(pendingLoad_->audio);
        pendingLoad_.reset();
        replacing_[pendingDeck_] = false;
        pendingGeneration_ = 0;
        replacementSnapshot_.reset();
        preparationStatus_ = "Failed";
        retirePlayback(std::move(playback), std::move(clip));
        return;
    }
    if (!replacementAcknowledged_ || state_.sourceGeneration[pendingDeck_] != pendingGeneration_) return;
    auto oldClip = std::move(clips_[pendingDeck_]);
    clips_[pendingDeck_] = std::move(pendingLoad_->audio);
    resetResidentAuthorization(std::size_t(pendingDeck_));
    continuationEpisodes_[pendingDeck_] = {};
    names_[pendingDeck_] = pendingName_;
    if (sourceIds_[pendingDeck_] != pendingLoad_->sourceId) bpmOverrides_[pendingDeck_].reset();
    sourceIds_[pendingDeck_] = pendingLoad_->sourceId;
    scratchGestures_[pendingDeck_].reset();
    scratchEnds_[pendingDeck_].reset();
    presentations_[pendingDeck_] = std::move(pendingLoad_->presentation);
    bpmEstimates_[pendingDeck_] = std::move(pendingLoad_->bpm);
    waveforms_[pendingDeck_] = pendingLoad_->waveform;
    pendingLoad_.reset();
    replacing_[pendingDeck_] = false;
    cueDown_[pendingDeck_] = cueReleasePending_[pendingDeck_] = false;
    replacementPublished_ = replacementAcknowledged_ = false;
    pendingGeneration_ = 0;
    replacementSnapshot_.reset();
    retirePlayback(std::move(pendingRetired_), std::move(oldClip));
}

void DeckController::retirePlayback(std::unique_ptr<audio::PreparedPlayback> playback,
                                std::shared_ptr<const PreparedPcm> clip)
{
    if (!playback && !clip) { retired(); return; }
    if (retirementPending_) {
        if (deferredRetirement_.count == deferredRetirement_.groups.size())
            throw std::logic_error("Playback retirement batch capacity exceeded");
        deferredRetirement_.groups[deferredRetirement_.count++] = {std::move(playback), std::move(clip)};
        return;
    }
    RetirementBatch batch;
    batch.groups[0] = {std::move(playback), std::move(clip)};
    batch.count = 1;
    startRetirement(std::move(batch));
}

void DeckController::startRetirement(RetirementBatch batch)
{
    if (retirementPending_ || !batch.count || batch.count > batch.groups.size())
        throw std::logic_error("Invalid playback retirement batch");
    retiringPcm_.fill({});
    for (std::size_t index = 0; index < batch.count; ++index) {
        const auto& clip = batch.groups[index].clip;
        retiringPcm_[index] = {clip.get(), clip ? clip->bytes() : 0};
    }
    retirementPending_ = true;
    retirer_.setFuture(QtConcurrent::run([batch = std::move(batch)]() mutable {
        for (std::size_t index = 0; index < batch.count; ++index) {
            batch.groups[index].playback.reset(); // Whole stop/join on this worker.
            batch.groups[index].clip.reset();
        }
    }));
}

void DeckController::retired()
{
    if (retirementPending_ && retirer_.isFinished()) {
        retirementPending_ = false;
        retiringPcm_.fill({});
        if (deferredRetirement_.count) {
            auto batch = std::move(deferredRetirement_);
            deferredRetirement_ = {};
            startRetirement(std::move(batch));
        }
    }
    if (!retirementPending_ && !deferredRetirement_.count && !installationPending_ && !pendingLoad_ &&
        !replacing_[0] && !replacing_[1] && !foregroundWaiting_) {
        busy_ = false;
        if (preparationStatus_ == "Cancelling") preparationStatus_ = "Cancelled";
        else if (preparationStatus_ == "Installing") {
            preparationStatus_ = "Complete";
            preparationCompletedStages_ = preparationTotalStages_;
        }
    }
    drainForegroundPreparation();
    if (!closing_) emit stateChanged();
}

void DeckController::installed()
{
    if (closing_) return;
    const auto result = installer_.result();
    installationPending_ = false;
    busy_ = retirementPending_ || deferredRetirement_.count != 0;
    const bool installingPreparation = preparationStatus_ == "Installing";
    if (installingPreparation && !busy_) preparationStatus_ = "Complete";
    if (!result->session) {
        // Constructor/prime failure destroys moved owners on the installer
        // worker. Clear only live rows that can no longer recreate an owner,
        // so either deck can be explicitly reloaded after losing both. Output
        // start failure below instead parks its healthy exact owners unchanged.
        for (std::size_t deck = 0; deck < clips_.size(); ++deck) {
            if (!result->lostLiveOwners[deck]) continue;
            clips_[deck].reset();
            resetResidentAuthorization(deck);
            continuationEpisodes_[deck] = {};
            names_[deck].clear();
            sourceIds_[deck].clear();
            presentations_[deck] = {};
            bpmEstimates_[deck] = {};
            bpmOverrides_[deck].reset();
            waveforms_[deck].clear();
            cueDown_[deck] = cueReleasePending_[deck] = false;
            state_.sourceFrame[deck] = state_.sourceFrames[deck] = state_.cueFrame[deck] = 0;
            state_.sourcePosition[deck] = 0;
            state_.sourceGeneration[deck] = state_.starvedFrames[deck] = 0;
            state_.playing[deck] = state_.cueSelected[deck] = false;
            state_.cueHeld[deck] = state_.cuePreview[deck] = false;
            state_.buffering[deck] = state_.sourceReadError[deck] = state_.sourceReadErrorObserved[deck] = false;
            state_.stemBank[deck] = false;
            state_.loops[deck] = {};
            state_.tempo[deck] = {};
            state_.scratch[deck] = {};
            state_.stemGain[deck].fill(0.F);
            state_.deckGain[deck] = 1.F;
            state_.meters.deck[deck] = {};
        }
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
        for (std::size_t deck = 0; deck < clips_.size(); ++deck)
            if (residentAuthorizations_[deck].phase != ResidentAuthorization::Phase::NextPlay)
                revokeResidentAuthorization(deck, "Audio output changed. Enable recovery again when ready.");
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
    if (installingPreparation && !busy_) preparationCompletedStages_ = preparationTotalStages_;
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
    flushScratchEnds();
    flushCueReleases();
    audio::LiveSession::State snapshot;
    // The queue is bounded; GUI delay can drop intermediate snapshots. Drain at
    // most its capacity, keeping the newest complete published state.
    for (std::size_t index = 0; index < 32 && session_->pollState(snapshot); ++index) {
        if (continuation_ && continuation_->published &&
            snapshot.sourceGeneration[continuation_->deck] == continuation_->generation)
            continuation_->snapshot = snapshot;
        else if (replacing_[pendingDeck_] && snapshot.sourceGeneration[pendingDeck_] == pendingGeneration_)
            replacementSnapshot_ = snapshot;
        else state_ = snapshot;
    }
    collectReplacement();
    if (closing_ || !session_) return;
    if (continuation_ && continuation_->acknowledged && continuation_->snapshot) state_ = *continuation_->snapshot;
    settleContinuation();
    if (closing_ || !session_) return;
    if (replacementAcknowledged_ && replacementSnapshot_) state_ = *replacementSnapshot_;
    settleReplacement();
    // Completion can notify a direct observer that shuts down this controller.
    if (closing_ || !session_) return;
    const auto error = session_->error();
    if (!error.empty()) {
        auto detail = QString::fromStdString(error);
        for (const auto& clip : clips_)
            if (clip && clip->live()) detail = withLiveStemFailure(std::move(detail), clip->observations);
        stopSession();
        recordingStopRequested_ = true;
        pollRecording();
        fail(detail);
        return;
    }
    ready_ = session_->ready();
    publishReplacement();
    routingSnapshot_ = session_->routingSnapshot();
    pollRecording();
    pollResidentAuthorization();
    considerContinuation();
    emit stateChanged();
}

bool DeckController::submit(audio::RenderCore::Command command)
{
    using Type = audio::RenderCore::Command::Type;
    const bool transportEndsScratch = command.deck < scratchGestures_.size() &&
        (command.type == Type::Play || command.type == Type::Pause || command.type == Type::Seek ||
         command.type == Type::Jog || command.type == Type::CuePress || command.type == Type::RetrySource);
    const auto failedTransport = [&] {
        if (transportEndsScratch) {
            queueScratchEnd(command.deck);
            (void)flushScratchEnds();
        }
    };
    if (closing_ || !ready_ || !session_) {
        failedTransport();
        fail("Audio output is not ready.");
        return false;
    }
    const bool global = command.type == audio::RenderCore::Command::Type::MasterGain ||
                        command.type == audio::RenderCore::Command::Type::Crossfader ||
                        command.type == audio::RenderCore::Command::Type::CueGain;
    if (!global && command.deck < replacing_.size() && replacing_[command.deck]) {
        failedTransport();
        fail("Wait for this deck's prepared track to finish installing.");
        return false;
    }
    const bool modeChange = command.type == Type::LoopIn || command.type == Type::LoopOut ||
        command.type == Type::LoopToggle || command.type == Type::TempoPercent ||
        command.type == Type::MasterTempo || command.type == Type::TempoReset;
    if (modeChange && command.deck < scratchGestures_.size() && scratchBusy(command.deck)) {
        fail("End the vinyl gesture before changing loops or tempo.");
        return false;
    }
    // Release has priority over later slider/play events. Never lose a release
    // just because the bounded transport queue was full at pointer-up.
    if (!flushScratchEnds()) {
        failedTransport();
        fail("Scratch release is waiting for the audio control queue.");
        return false;
    }
    if (!flushCueReleases()) {
        failedTransport();
        fail("Cue release is waiting for the audio control queue.");
        return false;
    }
    // IDs are unique until saturation. Manual controls remain available after
    // that; no new automatic acknowledgment chain may use a saturated ID.
    command.id = std::uint32_t(std::min(submitted_, std::uint64_t(UINT32_MAX) - 1) + 1);
    if (!session_->submit(command)) {
        failedTransport();
        fail("Audio control queue is full. Try the control again.");
        return false;
    }
    // The queued native transport command cancels scratch itself. Do not add a
    // redundant End+seek; source/token guards reject a later old-pointer Move.
    if (transportEndsScratch) scratchGestures_[command.deck].reset();
    const bool transportIntent = transportEndsScratch || modeChange || command.type == Type::CueRelease ||
        command.type == Type::ScratchBegin || command.type == Type::ScratchMove || command.type == Type::ScratchEnd;
    if (transportIntent) {
        revokeResidentAuthorization(command.deck, "Playback controls turned recovery off.");
        cancelContinuation(command.deck, "A newer human transport command was accepted.");
    }
    if (submitted_ != std::numeric_limits<std::uint64_t>::max()) ++submitted_;
    error_.clear();
    return true;
}

bool DeckController::sameScratchSource(std::size_t deck, const ScratchIdentity& identity) const
{
    return deck < clips_.size() && clips_[deck] && sourceIds_[deck] == identity.sourceId &&
        state_.sourceGeneration[deck] == identity.sourceGeneration;
}

bool DeckController::canScratch(std::size_t deck) const
{
    return !closing_ && ready_ && session_ && deck < clips_.size() && clips_[deck] &&
        !clips_[deck]->live() && !replacing_[deck] && !sourceIds_[deck].isEmpty() &&
        state_.scratch[deck].supported && !state_.scratch[deck].errorCode &&
        !state_.sourceReadError[deck] && !state_.cueHeld[deck] && !state_.cuePreview[deck] && !cueDown_[deck] &&
        !state_.loops[deck].inSet && !state_.loops[deck].outSet &&
        !state_.loops[deck].cached && !state_.loops[deck].active;
}

bool DeckController::scratchBusy(std::size_t deck) const
{
    return scratchGestures_[deck].has_value() || scratchEnds_[deck].has_value() || state_.scratch[deck].active;
}

void DeckController::queueScratchEnd(std::size_t deck)
{
    if (!scratchGestures_[deck]) return;
    // Revoke first: direct GUI observers and duplicate pointer-up cannot move
    // this accepted gesture while its release waits for bounded queue space.
    auto identity = std::move(scratchGestures_[deck]->identity);
    scratchGestures_[deck].reset();
    if (sameScratchSource(deck, identity)) scratchEnds_[deck] = std::move(identity);
}

bool DeckController::flushScratchEnds()
{
    for (std::size_t deck = 0; deck < scratchEnds_.size(); ++deck) {
        if (!scratchEnds_[deck]) continue;
        const auto& identity = *scratchEnds_[deck];
        if (!sameScratchSource(deck, identity)) { scratchEnds_[deck].reset(); continue; }
        if (!session_) return false;
        audio::RenderCore::Command command;
        command.type = audio::RenderCore::Command::Type::ScratchEnd;
        command.deck = std::uint32_t(deck);
        command.scratchToken = identity.nativeToken;
        command.sourceGeneration = identity.sourceGeneration;
        command.id = std::uint32_t(std::min(submitted_, std::uint64_t(UINT32_MAX) - 1) + 1);
        if (!session_->submit(command)) return false;
        revokeResidentAuthorization(deck, "Vinyl controls turned recovery off.");
        cancelContinuation(deck, "A newer scratch release was accepted.");
        scratchEnds_[deck].reset();
        if (submitted_ != std::numeric_limits<std::uint64_t>::max()) ++submitted_;
    }
    if (error_ == "Scratch release is waiting for the audio control queue.") error_.clear();
    return true;
}

QString DeckController::beginScratch(int deck)
{
    if (deck < 0 || deck >= int(clips_.size()) || !canScratch(std::size_t(deck))) return {};
    if (!flushScratchEnds() || scratchBusy(std::size_t(deck))) return {};
    std::uint64_t nativeToken = 0;
    if (!nextScratchToken(nativeToken)) { fail("Vinyl gesture identity exhausted."); return {}; }
    ScratchGesture gesture;
    gesture.identity = {QString::number(nativeToken, 16).rightJustified(16, QChar(u'0')),
                        sourceIds_[deck], nativeToken, state_.sourceGeneration[deck]};
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::ScratchBegin;
    command.deck = std::uint32_t(deck);
    command.scratchToken = nativeToken;
    command.sourceGeneration = gesture.identity.sourceGeneration;
    if (!submit(command)) return {};
    gesture.interval.start();
    scratchGestures_[deck] = std::move(gesture);
    return scratchGestures_[deck]->identity.token;
}

bool DeckController::moveScratch(int deck, const QString& token, qint64 cumulativeFrames)
{
    if (deck < 0 || deck >= int(clips_.size()) || !scratchGestures_[deck] ||
        token != scratchGestures_[deck]->identity.token) return false;
    auto& gesture = *scratchGestures_[deck];
    if (!sameScratchSource(std::size_t(deck), gesture.identity)) {
        scratchGestures_[deck].reset();
        return false;
    }
    const qint64 elapsed = gesture.interval.nsecsElapsed();
    if (!canScratch(std::size_t(deck)) || elapsed <= 0) {
        queueScratchEnd(std::size_t(deck));
        (void)flushScratchEnds();
        return false;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::ScratchMove;
    command.deck = std::uint32_t(deck);
    command.scratchToken = gesture.identity.nativeToken;
    command.sourceGeneration = gesture.identity.sourceGeneration;
    command.deltaFrames = cumulativeFrames;
    // A stationary contact remains valid. After the source's 30 ms motion
    // watchdog has stopped it, treat the next event as a fresh motion burst
    // rather than averaging its velocity over the entire stationary hold.
    // The source still bounds speed and consumes only the requested distance.
    command.scratchIntervalSeconds = double(std::min<qint64>(elapsed, 30000000LL)) / 1000000000.;
    if (!submit(command)) {
        queueScratchEnd(std::size_t(deck));
        (void)flushScratchEnds();
        return false;
    }
    gesture.interval.start();
    return true;
}

bool DeckController::endScratch(int deck, const QString& token)
{
    if (deck < 0 || deck >= int(clips_.size()) || !scratchGestures_[deck] ||
        token != scratchGestures_[deck]->identity.token) return false;
    queueScratchEnd(std::size_t(deck));
    if (!flushScratchEnds()) fail("Scratch release is waiting for the audio control queue.");
    return true; // Accepted locally; the public map still waits for native ACK.
}

bool DeckController::flushCueReleases()
{
    if (!flushScratchEnds()) return false;
    for (std::size_t deck = 0; deck < cueReleasePending_.size(); ++deck) {
        if (!cueReleasePending_[deck])
            continue;
        audio::RenderCore::Command command;
        command.type = audio::RenderCore::Command::Type::CueRelease;
        command.deck = std::uint32_t(deck);
        if (!session_) return false;
        command.id = std::uint32_t(std::min(submitted_, std::uint64_t(UINT32_MAX) - 1) + 1);
        if (!session_->submit(command)) return false;
        revokeResidentAuthorization(deck, "CUE turned recovery off.");
        cancelContinuation(deck, "A newer CUE release was accepted.");
        cueReleasePending_[deck] = false;
        if (submitted_ != std::numeric_limits<std::uint64_t>::max()) ++submitted_;
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
    const auto authorized = playing && residentAuthorizations_[deck].clip == clips_[deck].get() &&
        residentAuthorizations_[deck].phase == ResidentAuthorization::Phase::NextPlay;
    const auto generation = state_.sourceGeneration[deck], revision = state_.transportRevision[deck];
    if (submit(command) && authorized && submitted_ < UINT32_MAX &&
        completedSessions_ != std::numeric_limits<std::uint64_t>::max() && revision < std::numeric_limits<std::uint64_t>::max() - 1) {
        auto& authorization = residentAuthorizations_[deck];
        authorization.clip = clips_[deck].get(); authorization.session = session_.get();
        authorization.outputSession = completedSessions_;
        authorization.generation = generation; authorization.revision = revision + 1;
        authorization.commandId = submitted_; authorization.phase = ResidentAuthorization::Phase::PlayPending;
        authorization.detail = "Enabling recovery for this Play.";
    }
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

void DeckController::setEqGainDb(int deck, int band, double db)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] ||
        band < 0 || band >= int(audio::RenderCore::eqBandCount) || !std::isfinite(db) ||
        db < audio::ChannelEq::minGainDb || db > audio::ChannelEq::maxGainDb) {
        fail("Channel EQ must be between -26 and +6 dB for a loaded deck.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::EqGain;
    command.deck = std::uint32_t(deck);
    command.band = std::uint32_t(band);
    command.eqGainDb = float(db);
    submit(command);
}

void DeckController::setStemGain(int deck, int stem, double gain)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || !clips_[deck]->stemBank() ||
        stem < 0 || stem >= int(audio::RenderCore::stemCount) ||
        !std::isfinite(gain) || gain < 0 || gain > 1) {
        fail("Stem gain must be between zero and one for a loaded stem bank.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::StemGain;
    command.deck = std::uint32_t(deck);
    command.stem = std::uint32_t(stem);
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

void DeckController::setCrossfader(double position)
{
    if (!std::isfinite(position) || position < 0 || position > 1) {
        fail("Crossfader position must be between zero and one.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::Crossfader;
    command.gain = float(position);
    submit(command);
}

void DeckController::setTempoPercent(int deck, double percent)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || clips_[deck]->live() ||
        !std::isfinite(percent) || percent < -6.0 || percent > 6.0) {
        fail("Tempo requires prepared audio and a value between -6 and +6 percent.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::TempoPercent;
    command.deck = std::uint32_t(deck);
    command.tempoPercent = percent;
    submit(command);
}

void DeckController::setTempoBpm(int deck, double bpm)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || replacing_[deck]) {
        fail("Load a track before changing BPM.");
        return;
    }
    const auto reference = bpmOverrides_[deck].value_or(bpmEstimates_[deck].bpm.value_or(0.));
    const auto percent = library::tempoPercentForBpm(reference, bpm);
    if (!percent) {
        fail("BPM needs a valid original reference and a target within the tempo range.");
        return;
    }
    setTempoPercent(deck, *percent);
}

void DeckController::setBpmOverride(int deck, double bpm)
{
    if (closing_ || deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || replacing_[deck] ||
        !std::isfinite(bpm) || bpm < 20. || bpm > 400.) {
        fail("Original BPM must be between 20 and 400 for a loaded track.");
        return;
    }
    // A source reference is metadata. Changing it never changes playback speed
    // or issues a render command; the acknowledged ratio remains authoritative.
    bpmOverrides_[deck] = bpm;
    emit stateChanged();
}

void DeckController::clearBpmOverride(int deck)
{
    if (closing_ || deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || replacing_[deck]) {
        fail("Load a track before selecting automatic BPM.");
        return;
    }
    bpmOverrides_[deck].reset();
    emit stateChanged();
}

void DeckController::toggleMasterTempo(int deck)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || clips_[deck]->live()) {
        fail("Master Tempo requires prepared audio.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::MasterTempo;
    command.deck = std::uint32_t(deck);
    submit(command);
}

void DeckController::resetTempo(int deck)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || clips_[deck]->live()) {
        fail("Tempo reset requires prepared audio.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::TempoReset;
    command.deck = std::uint32_t(deck);
    submit(command);
}

void DeckController::loopIn(int deck)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || clips_[deck]->live()) {
        fail("Load prepared audio before using manual loops.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::LoopIn;
    command.deck = std::uint32_t(deck);
    submit(command);
}

void DeckController::loopOut(int deck)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || clips_[deck]->live()) {
        fail("Load prepared audio before using manual loops.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::LoopOut;
    command.deck = std::uint32_t(deck);
    submit(command);
}

void DeckController::loopToggle(int deck)
{
    if (deck < 0 || deck >= int(clips_.size()) || !clips_[deck] || clips_[deck]->live()) {
        fail("Load prepared audio before using manual loops.");
        return;
    }
    audio::RenderCore::Command command;
    command.type = audio::RenderCore::Command::Type::LoopToggle;
    command.deck = std::uint32_t(deck);
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

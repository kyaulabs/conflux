// SPDX-License-Identifier: AGPL-3.0-or-later
// Functional controller integration on the private_routing_check.py daemon.
// Generated audio and cache stay inside its private runtime; no host output.
#include "ui/DeckController.hpp"
#include "../../tests/StemExportFixture.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtEndian>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t fixtureFrames = 60 * 48000;
constexpr std::array<float, 2> expected{.125F, -.0625F};

template<class Predicate>
void wait(Predicate&& predicate, const char* message, bool processGui = true,
          std::chrono::seconds timeout = std::chrono::seconds(8))
{
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error(message);
        if (processGui) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
}

bool save(const QString& path, const QJsonObject& value)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.commit();
}

void writeFixture(const QString& path, qint16 left, qint16 right, std::uint32_t totalFrames = fixtureFrames)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot create private PCM fixture");
    QDataStream header(&file);
    header.setByteOrder(QDataStream::LittleEndian);
    header.writeRawData("RIFF", 4);
    header << quint32(36 + totalFrames * 4);
    header.writeRawData("WAVEfmt ", 8);
    header << quint32(16) << quint16(1) << quint16(2) << quint32(48000)
           << quint32(192000) << quint16(4) << quint16(16);
    header.writeRawData("data", 4);
    header << quint32(totalFrames * 4);
    std::array<char, 4096 * 4> block{};
    for (std::size_t frame = 0; frame < block.size() / 4; ++frame) {
        qToLittleEndian<qint16>(left, block.data() + frame * 4);
        qToLittleEndian<qint16>(right, block.data() + frame * 4 + 2);
    }
    for (std::uint32_t first = 0; first < totalFrames;) {
        const auto frames = std::min<std::uint32_t>(4096, totalFrames - first);
        if (file.write(block.data(), frames * 4) != qint64(frames * 4))
            throw std::runtime_error("Cannot write private PCM fixture");
        first += frames;
    }
    if (header.status() != QDataStream::Ok || !file.flush())
        throw std::runtime_error("Cannot finish private PCM fixture");
}

class Sink {
public:
    explicit Sink(std::string name, float tolerance = 0.F) : name_(std::move(name)), tolerance_(tolerance)
    {
        loop_ = pw_thread_loop_new(name_.c_str(), nullptr);
        if (!loop_) throw std::runtime_error("Cannot create private sink loop");
        filter_ = pw_filter_new_simple(pw_thread_loop_get_loop(loop_), name_.c_str(),
            pw_properties_new(PW_KEY_NODE_NAME, name_.c_str(), PW_KEY_MEDIA_CLASS, "Audio/Sink",
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                PW_KEY_NODE_AUTOCONNECT, "false", "node.always-process", "true",
                PW_KEY_NODE_LATENCY, "256/48000", PW_KEY_NODE_RATE, "1/48000", nullptr),
            &events(), this);
        if (!filter_) { stop(); throw std::runtime_error("Cannot create private sink filter"); }
        for (std::size_t channel = 0; channel < ports_.size(); ++channel) {
            const auto name = "input_" + std::to_string(channel);
            ports_[channel] = pw_filter_add_port(filter_, PW_DIRECTION_INPUT,
                PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
                pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME,
                    name.c_str(), PW_KEY_AUDIO_CHANNEL, channel ? "FR" : "FL", nullptr), nullptr, 0);
            if (!ports_[channel]) { stop(); throw std::runtime_error("Cannot create private sink input"); }
        }
        if (pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0 || pw_thread_loop_start(loop_) < 0) {
            stop(); throw std::runtime_error("Cannot connect private sink");
        }
        started_ = true;
    }
    ~Sink() { stop(); }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    const std::string& name() const { return name_; }
    void validate(bool value) { armed_.store(value, std::memory_order_release); }
    std::uint64_t received(std::size_t channel) const { return received_[channel].load(); }
    std::uint64_t wrong() const { return wrong_.load(); }
    std::uint64_t zeros() const { return zeros_.load(); }
private:
    void stop() noexcept
    {
        if (!loop_) return;
        if (started_) pw_thread_loop_lock(loop_);
        if (filter_) { pw_filter_disconnect(filter_); pw_filter_destroy(filter_); filter_ = nullptr; }
        if (started_) { pw_thread_loop_unlock(loop_); pw_thread_loop_stop(loop_); }
        pw_thread_loop_destroy(loop_); loop_ = nullptr; started_ = false;
    }
    static void process(void* data, spa_io_position* position) noexcept
    {
        auto& self = *static_cast<Sink*>(data);
        if (!position || position->clock.duration > 65536) return;
        const auto frames = std::uint32_t(position->clock.duration);
        const bool armed = self.armed_.load(std::memory_order_acquire);
        for (std::size_t channel = 0; channel < self.ports_.size(); ++channel) {
            const auto* samples = static_cast<const float*>(pw_filter_get_dsp_buffer(self.ports_[channel], frames));
            if (!samples || !armed) continue;
            std::uint64_t wrong = 0, zeros = 0;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                zeros += samples[frame] == 0.F;
                wrong += !std::isfinite(samples[frame]) || std::abs(samples[frame] - expected[channel]) > self.tolerance_;
            }
            self.received_[channel].fetch_add(frames, std::memory_order_relaxed);
            self.wrong_.fetch_add(wrong, std::memory_order_relaxed);
            self.zeros_.fetch_add(zeros, std::memory_order_relaxed);
        }
    }
    static const pw_filter_events& events()
    {
        static const auto value = [] { pw_filter_events e{}; e.version = PW_VERSION_FILTER_EVENTS; e.process = process; return e; }();
        return value;
    }
    std::string name_;
    const float tolerance_;
    std::array<void*, 2> ports_{};
    std::array<std::atomic<std::uint64_t>, 2> received_{};
    std::atomic<std::uint64_t> wrong_{0}, zeros_{0};
    std::atomic<bool> armed_{false};
    pw_thread_loop* loop_ = nullptr;
    pw_filter* filter_ = nullptr;
    bool started_ = false;
};

QJsonObject inspectStemRecording(const QString& path, std::uint64_t prefixFrames, std::uint64_t expectedFrames)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 44 || file.size() > 128 * 1024 * 1024)
        throw std::runtime_error("Cannot read bounded generated stem recording");
    const auto header = file.read(12);
    if (header.size() != 12 || header.first(4) != "RIFF" || header.sliced(8, 4) != "WAVE")
        throw std::runtime_error("Stem recording is not a RIFF WAVE file");
    bool format = false;
    qint64 dataOffset = -1;
    std::uint64_t bytes = 0;
    for (qint64 position = 12; position + 8 <= file.size();) {
        if (!file.seek(position)) throw std::runtime_error("Cannot seek generated WAV metadata");
        const auto chunk = file.read(8);
        if (chunk.size() != 8) throw std::runtime_error("Truncated generated WAV metadata");
        const auto count = qFromLittleEndian<quint32>(chunk.constData() + 4);
        if (std::uint64_t(count) > std::uint64_t(file.size() - position - 8))
            throw std::runtime_error("Generated WAV chunk exceeds its regular file");
        if (chunk.first(4) == "fmt ") {
            const auto fmt = file.read(std::min<quint32>(count, 64));
            format = fmt.size() >= 16 && qFromLittleEndian<quint16>(fmt.constData()) == 3 &&
                qFromLittleEndian<quint16>(fmt.constData() + 2) == 2 &&
                qFromLittleEndian<quint32>(fmt.constData() + 4) == 48000 &&
                qFromLittleEndian<quint16>(fmt.constData() + 12) == 8 &&
                qFromLittleEndian<quint16>(fmt.constData() + 14) == 32;
        } else if (chunk.first(4) == "data") {
            if (dataOffset >= 0) throw std::runtime_error("Generated WAV has multiple data chunks");
            dataOffset = position + 8;
            bytes = count;
        }
        position += 8 + qint64(count) + (count & 1U);
    }
    const auto frames = bytes / 8;
    // A failure diagnostic checks the complete finalized recording after the
    // controller has unwound; normal assertions supply their exact counts.
    if (!expectedFrames) expectedFrames = frames;
    if (!prefixFrames) prefixFrames = frames;
    if (!format || dataOffset < 0 || !frames || bytes % 8 || frames != expectedFrames ||
        prefixFrames < 1024 || prefixFrames > frames || frames < 1024 || !file.seek(dataOffset))
        throw std::runtime_error("Generated WAV is not complete stereo 48000 Hz float PCM");
    std::array<char, 4096 * 8> block{};
    float maximumError = 0;
    std::uint64_t checkedFrames = 0;
    for (std::uint64_t first = 0; first < frames;) {
        const auto count = std::min<std::uint64_t>(4096, frames - first);
        if (file.read(block.data(), qint64(count * 8)) != qint64(count * 8))
            throw std::runtime_error("Generated WAV PCM ended early");
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto frame = first + index;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto bits = qFromLittleEndian<quint32>(block.data() + index * 8 + channel * 4);
                const auto value = std::bit_cast<float>(std::uint32_t(bits));
                if (!std::isfinite(value) || std::abs(value) > std::abs(expected[channel]) + 1e-6F)
                    throw std::runtime_error("Stem recording contains nonfinite or unexpected master audio");
                if (frame < prefixFrames || frame + 1024 >= frames)
                    maximumError = std::max(maximumError, std::abs(value - expected[channel]));
            }
            checkedFrames += frame < prefixFrames || frame + 1024 >= frames;
        }
        first += count;
    }
    if (maximumError > 1e-6F) throw std::runtime_error("Stem recording lost its independent master prefix or tail");
    return {{"frames", qint64(frames)}, {"prefix_frames", qint64(prefixFrames)},
        {"checked_prefix_or_tail_frames", qint64(checkedFrames)}, {"maximum_prefix_or_tail_error", maximumError},
        {"scope", "Generated master prefix and tail checked at 1e-6 tolerance; explicit routing rebuild may omit wall time and ramp between them."}};
}

template<class Check>
void runStemController(const std::filesystem::path& output, const QString& fixtures,
                       const QString& a, const QString& b, const QString& libraryId,
                       bool functionalOnly, Check&& require, QJsonObject& observations)
{
    auto engine = conflux::test::makeStemEngineFixture(fixtures);
    engine.maxSeconds = 30;
    conflux::DeckController::PreparationOptions options;
    options.stemEngine = engine;
    conflux::DeckController controller(options);
    Sink sink("conflux-stem-controller-master-" + std::to_string(::getpid()), 1e-6F);
    // Instrumented functional checks still inspect every steady recorded frame.
    // They report downstream virtual-sink discrepancies separately: maintaining
    // that graph's wall-clock deadlines is a different qualification.
    const auto sinkMatches = [&] { return functionalOnly || (!sink.wrong() && !sink.zeros()); };
    controller.refreshAudioRouting();
    QString left, right;
    wait([&] {
        for (const auto& value : controller.audioRouting()["ports"].toList()) {
            const auto port = value.toMap();
            const auto prefix = QString::fromStdString(sink.name()) + " · input_";
            if (port["label"].toString() == prefix + "0 [FL]") left = port["key"].toString();
            if (port["label"].toString() == prefix + "1 [FR]") right = port["key"].toString();
        }
        return !left.isEmpty() && !right.isEmpty() && controller.libraryReady();
    }, "Stem controller private routing/library discovery timed out");
    controller.configureAudioRouting(left, right, {}, {});
    require(controller.stemsAvailable() && controller.error().isEmpty(),
            "Explicit local stem engine is available on the private master-only pair");
    controller.loadStemDeck(0, QUrl::fromLocalFile(a));
    wait([&] {
        if (!controller.busy() && !controller.error().isEmpty())
            throw std::runtime_error("Initial bank A failed: " + controller.error().toStdString());
        return !controller.busy() && controller.outputReady();
    }, "Initial bank A did not become ready", true, std::chrono::seconds(120));
    const auto loadedA = controller.audioState();
    const auto aMap = controller.decks()[0].toMap();
    require(controller.error().isEmpty() && loadedA.stemBank[0] && !loadedA.playing[0] &&
            aMap["stemBank"].toBool() && aMap["overviewStem"].toString() == "drums" &&
            aMap["stemGains"].toList().size() == 4 && aMap["waveform"].toList().size() == 512 &&
            controller.retainedPcmBytes() == loadedA.sourceFrames[0] * 8 * 4,
            "Initial prepared bank retains four aligned owners and labels its drums overview");
    constexpr std::array<float, 4> selected{0.F, 0.F, 0.F, 1.F};
    for (int stem = 0; stem < 3; ++stem) controller.setStemGain(0, stem, 0.0);
    wait([&] { return controller.audioState().stemGain[0] == selected; }, "Initial selective stem gains were not acknowledged");
    controller.setPlaying(0, true);
    wait([&] { return controller.audioState().playing[0] && controller.audioState().sourceFrame[0] >= 4096; },
         "Selected bank A did not advance through its startup ramp");
    sink.validate(true);
    wait([&] { return sink.received(0) >= 1024 && sink.received(1) >= 1024; }, "Selected bank A master samples did not arrive");
    require(sinkMatches(), "Selected bank A reaches the private sink; sink sample gate applies outside functional-only mode");
    const auto recordingPath = QString::fromStdString((output / "generated-stem-controller.wav").string());
    controller.startRecording(QUrl::fromLocalFile(recordingPath));
    wait([&] { return controller.recording()["status"].toString() == "Recording" &&
                      controller.recording()["acceptedFrames"].toULongLong() >= 1024; },
         "Generated bank recording did not attach");

    // Keep a long advancing peer while the replacement is small enough for
    // instrumented cold preparation to finish before that peer's natural EOF.
    const auto stemTarget = QDir(fixtures).filePath("stem-controller-target.wav");
    constexpr std::uint32_t targetFrames = 4 * 48000;
    writeFixture(stemTarget, 2048, 3072, targetFrames);
    controller.importTrack(QUrl::fromLocalFile(stemTarget));
    wait([&] { return !controller.busy(); }, "Short stem target import did not finish");
    QString stemTargetId;
    for (const auto& value : controller.libraryTracks()) {
        const auto track = value.toMap();
        if (track["path"].toString() == stemTarget) stemTargetId = track["id"].toString();
    }
    require(controller.error().isEmpty() && !stemTargetId.isEmpty(),
            "Short replacement source is imported while the long prepared bank continues");

    controller.loadDeck(1, QUrl::fromLocalFile(b));
    wait([&] { return !controller.busy() && controller.audioState().sourceFrames[1] != 0; }, "Normal peer B did not load");
    controller.setDeckGain(1, 0.0);
    wait([&] { return controller.audioState().deckGain[1] == 0.F; }, "Normal B mute target was not acknowledged");
    const auto mutedAt = controller.audioState().outputFrame;
    wait([&] { return controller.audioState().outputFrame >= mutedAt + 1024; }, "Normal B mute ramp did not settle");
    controller.setPlaying(1, true);
    wait([&] {
        const auto state = controller.audioState();
        return state.playing[1] && state.deckGain[1] == 0.F && state.sourceFrame[1] >= 4096;
    }, "Muted normal B peer did not advance");
    const auto beforeCancel = controller.audioState();
    const auto sessions = controller.completedSessions();
    const auto retainedBefore = controller.retainedPcmBytes();
    const auto recordedBefore = controller.recording()["acceptedFrames"].toULongLong();
    const auto blockedFrom = std::chrono::steady_clock::now();
    const auto sinkWrongBefore = sink.wrong(), sinkZerosBefore = sink.zeros();
    controller.loadLibraryStemTrack(1, stemTargetId);
    require(controller.busy() && controller.preparation()["cancellable"].toBool(),
            "Library bank B preparation starts without replacing the advancing sources");
    require(QThreadPool::globalInstance()->waitForDone(60000),
            "Bank B worker completes while GUI acceptance remains queued");
    const auto recordedAtCompletion = controller.recording()["acceptedFrames"].toULongLong();
    require(controller.cancelPreparation() && controller.busy(),
            "Queued complete-bank cancellation remains accepted before GUI installation");
    // A blocked GUI can retain old queued snapshots. Recorded frames establish
    // a lower bound for a fresh output clock without relying on wall time.
    wait([&] { return !controller.busy() && controller.audioState().outputFrame >=
                         beforeCancel.outputFrame + recordedAtCompletion - recordedBefore; },
         "Queued bank cancellation did not finish cleanup");
    const auto cancelled = controller.audioState();
    const auto stateDetail = [](const auto& value) {
        QJsonArray decks;
        for (std::size_t deck = 0; deck < 2; ++deck) {
            QJsonArray gains;
            for (const auto gain : value.stemGain[deck]) gains.append(gain);
            decks.append(QJsonObject{{"frame", qint64(value.sourceFrame[deck])},
                {"frames", qint64(value.sourceFrames[deck])}, {"playing", value.playing[deck]},
                {"generation", qint64(value.sourceGeneration[deck])}, {"bank", value.stemBank[deck]},
                {"stem_gains", gains}, {"deck_gain", value.deckGain[deck]},
                {"starved", qint64(value.starvedFrames[deck])}, {"read_error", value.sourceReadError[deck]}});
        }
        return QJsonObject{{"output", qint64(value.outputFrame)}, {"decks", decks}};
    };
    observations["stem_cancel_detail"] = QJsonObject{{"before", stateDetail(beforeCancel)},
        {"after", stateDetail(cancelled)}, {"status", controller.preparation()["status"].toString()},
        {"ready", controller.outputReady()}, {"sessions_before", qint64(sessions)},
        {"sessions_after", qint64(controller.completedSessions())},
        {"retained_before", qint64(retainedBefore)}, {"retained_after", qint64(controller.retainedPcmBytes())},
        {"recorded_before", qint64(recordedBefore)}, {"recording", QJsonObject::fromVariantMap(controller.recording())},
        {"recorded_at_worker_completion", qint64(recordedAtCompletion)},
        {"sink_wrong_before", qint64(sinkWrongBefore)}, {"sink_wrong_after", qint64(sink.wrong())},
        {"sink_zeros_before", qint64(sinkZerosBefore)}, {"sink_zeros_after", qint64(sink.zeros())},
        {"elapsed_seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - blockedFrom).count()}};
    require(controller.preparation()["status"].toString() == "Cancelled" && controller.outputReady() &&
            controller.completedSessions() == sessions && cancelled.sourceGeneration == beforeCancel.sourceGeneration &&
            cancelled.stemBank[0] && !cancelled.stemBank[1] && cancelled.stemGain[0] == selected &&
            cancelled.playing[0] && cancelled.playing[1] && cancelled.deckGain[1] == 0.F &&
            cancelled.sourceFrame[0] - beforeCancel.sourceFrame[0] == cancelled.outputFrame - beforeCancel.outputFrame &&
            cancelled.sourceFrame[1] - beforeCancel.sourceFrame[1] == cancelled.outputFrame - beforeCancel.outputFrame &&
            controller.retainedPcmBytes() == retainedBefore && controller.recording()["active"].toBool() &&
            controller.recording()["acceptedFrames"].toULongLong() > recordedBefore && sinkMatches(),
            "Queued bank cancellation retires its whole candidate while peer clocks, selected stems and recorder continue");

    const auto beforeBank = controller.audioState();
    controller.loadLibraryStemTrack(1, stemTargetId);
    wait([&] { return !controller.busy() && controller.audioState().stemBank[1]; }, "Cached complete bank B did not install");
    const auto bank = controller.audioState();
    constexpr std::array<float, 4> unity{1.F, 1.F, 1.F, 1.F};
    require(controller.preparation()["cacheHit"].toBool() && controller.completedSessions() == sessions &&
            bank.sourceGeneration[1] > beforeBank.sourceGeneration[1] && !bank.playing[1] && bank.sourceFrame[1] == 0 &&
            bank.stemGain[1] == unity && bank.deckGain[1] == 1.F && bank.playing[0] && bank.stemGain[0] == selected &&
            bank.sourceFrame[0] - beforeBank.sourceFrame[0] == bank.outputFrame - beforeBank.outputFrame &&
            controller.retainedPcmBytes() == (bank.sourceFrames[0] + bank.sourceFrames[1]) * 8 * 4 &&
            controller.recording()["active"].toBool() && sinkMatches(),
            "Cached bank handoff resets only its target and retains both complete owners without restarting A or recording");

    const auto beforeNormal = controller.audioState();
    controller.loadLibraryTrack(1, libraryId);
    wait([&] { return !controller.busy() && !controller.audioState().stemBank[1]; }, "Bank B did not retire into normal playback");
    const auto normal = controller.audioState();
    require(controller.completedSessions() == sessions && normal.sourceGeneration[1] > beforeNormal.sourceGeneration[1] &&
            !normal.playing[1] && normal.sourceFrame[1] == 0 && normal.playing[0] && normal.stemGain[0] == selected &&
            normal.sourceFrame[0] - beforeNormal.sourceFrame[0] == normal.outputFrame - beforeNormal.outputFrame &&
            controller.retainedPcmBytes() == normal.sourceFrames[0] * 8 * 4 + normal.sourceFrames[1] * 8 &&
            controller.recording()["active"].toBool() && sinkMatches(),
            "Bank-to-normal handoff retires all four target PCM owners and preserves the audible bank peer and writer");
    controller.setDeckGain(1, 0.0);
    wait([&] { return controller.audioState().deckGain[1] == 0.F; }, "Reloaded normal B mute target was not acknowledged");
    const auto mutedAgainAt = controller.audioState().outputFrame;
    wait([&] { return controller.audioState().outputFrame >= mutedAgainAt + 1024; }, "Reloaded normal B mute ramp did not settle");
    controller.setPlaying(1, true);
    wait([&] { return controller.audioState().playing[1] && controller.audioState().deckGain[1] == 0.F; },
         "Normal B did not restart muted before routing rebuild");
    const auto beforeRejectedRoute = controller.audioState();
    const auto retainedBeforeRoute = controller.retainedPcmBytes();
    const auto routingBefore = controller.audioRouting();
    const auto recordedBeforeRoute = controller.recording()["acceptedFrames"].toULongLong();
    controller.configureAudioRouting("missing", "missing", {}, {});
    wait([&] { return controller.audioState().outputFrame >= beforeRejectedRoute.outputFrame + 1024; },
         "Rejected routing interrupted the existing output");
    const auto rejectedRoute = controller.audioState();
    const auto routingAfter = controller.audioRouting();
    require(!controller.error().isEmpty() && !controller.busy() && controller.outputReady() &&
            controller.completedSessions() == sessions && rejectedRoute.playing[0] && rejectedRoute.playing[1] &&
            rejectedRoute.sourceGeneration == beforeRejectedRoute.sourceGeneration &&
            rejectedRoute.stemGain[0] == selected && rejectedRoute.deckGain[1] == 0.F &&
            rejectedRoute.sourceFrame[0] - beforeRejectedRoute.sourceFrame[0] ==
                rejectedRoute.outputFrame - beforeRejectedRoute.outputFrame &&
            rejectedRoute.sourceFrame[1] - beforeRejectedRoute.sourceFrame[1] ==
                rejectedRoute.outputFrame - beforeRejectedRoute.outputFrame &&
            routingAfter["masterLeft"] == routingBefore["masterLeft"] &&
            routingAfter["masterRight"] == routingBefore["masterRight"] &&
            controller.retainedPcmBytes() == retainedBeforeRoute && controller.recording()["active"].toBool() &&
            controller.recording()["path"].toString() == recordingPath &&
            controller.recording()["acceptedFrames"].toULongLong() > recordedBeforeRoute,
            "Rejected routing retains advancing sources, selected stems, output pair, PCM charge and original writer");
    const auto beforeRoute = controller.audioState();
    const auto prefixFrames = controller.recording()["acceptedFrames"].toULongLong();
    sink.validate(false); // Explicit routing rebuild may omit wall time and ramp.
    controller.configureAudioRouting(left, right, {}, {});
    wait([&] {
        return !controller.busy() && controller.outputReady() &&
            controller.audioState().sourceFrame[0] >= beforeRoute.sourceFrame[0] + 4096;
    }, "Bank routing rebuild did not restore output and advance");
    const auto routed = controller.audioState();
    require(controller.error().isEmpty() && controller.completedSessions() == sessions + 1 && routed.stemBank[0] &&
            !routed.stemBank[1] && routed.stemGain[0] == selected && routed.playing[0] && routed.playing[1] &&
            routed.deckGain[1] == 0.F && routed.sourceFrame[1] >= beforeRoute.sourceFrame[1] &&
            controller.retainedPcmBytes() == routed.sourceFrames[0] * 8 * 4 + routed.sourceFrames[1] * 8 &&
            controller.recording()["active"].toBool() && controller.recording()["path"].toString() == recordingPath &&
            controller.recording()["acceptedFrames"].toULongLong() > prefixFrames,
            "Explicit routing rebuild retains the bank, acknowledged stem gains, peer transport and original recording writer");
    const auto wrongBefore = sink.wrong(), zerosBefore = sink.zeros(), receivedBefore = sink.received(0);
    sink.validate(true);
    wait([&] { return sink.received(0) >= receivedBefore + 1024; }, "Post-rebuild bank samples did not arrive");
    require(functionalOnly || (sink.wrong() == wrongBefore && sink.zeros() == zerosBefore),
            "Restored bank reaches the private sink; sink sample gate applies outside functional-only mode");
    controller.stopRecording();
    wait([&] { return !controller.recording()["active"].toBool(); }, "Generated stem recording did not finalize");
    const auto recording = controller.recording();
    require(recording["status"].toString() == "Complete" && recording["error"].toString().isEmpty() &&
            recording["acceptedFrames"] == recording["writtenFrames"] && recording["overflowFrames"].toULongLong() == 0,
            "One bank-session writer finalizes every accepted stereo frame without overflow");
    const auto wav = inspectStemRecording(recordingPath, prefixFrames, recording["writtenFrames"].toULongLong());
    require(!routed.invalidBuffers && !routed.rejected && !routed.sourceReadErrorObserved[0] &&
            !routed.sourceReadErrorObserved[1] && !routed.sourceReadError[0] && !routed.sourceReadError[1],
            "Prepared bank integration reports no invalid buffers, rejected commands or source read failures");
    const auto final = controller.audioState();
    sink.validate(false);
    controller.shutdown();
    require(controller.completedSessions() == sessions + 2 && !controller.outputReady() && !controller.busy(),
            "Only explicit routing rebuild and shutdown complete the stem controller's sessions");
    observations["stem_controller"] = QJsonObject{{"recording", wav},
        {"virtual_sink_sample_gate", !functionalOnly},
        {"virtual_sink_wrong_samples", qint64(sink.wrong())}, {"virtual_sink_zero_samples", qint64(sink.zeros())},
        {"replacement_fixture_frames", qint64(targetFrames)},
        {"initial_bank_frames", qint64(loadedA.sourceFrames[0])}, {"bank_handoff_generation", qint64(bank.sourceGeneration[1])},
        {"normal_handoff_generation", qint64(normal.sourceGeneration[1])},
        {"completed_sessions", qint64(controller.completedSessions())}, {"final_callbacks", qint64(final.callbacks)},
        {"reported_starved_a", qint64(final.starvedFrames[0])}, {"reported_starved_b", qint64(final.starvedFrames[1])},
        {"steady_sample_tolerance", 1e-6}, {"scope", "Generated fake separator, actual cache/controller/render/PipeWire paths. No model inference, physical routing, latency, sustained or musical qualification."}};
}

void validatePrivate(const std::filesystem::path& runtime, const std::filesystem::path& output)
{
    const auto env = [](const char* name) { const auto* value = std::getenv(name); return value ? std::string(value) : std::string{}; };
    const auto remote = env("PIPEWIRE_REMOTE");
    if (env("CONFLUX_PRIVATE_ROUTING_CHECK") != "1" || !runtime.is_absolute() ||
        env("XDG_RUNTIME_DIR") != runtime.string() || env("PIPEWIRE_RUNTIME_DIR") != runtime.string() ||
        !remote.starts_with("conflux-private-routing-"))
        throw std::runtime_error("Explicit private PipeWire runtime is required");
    struct stat info{};
    if (::lstat(runtime.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != ::getuid() ||
        (info.st_mode & 0777) != 0700)
        throw std::runtime_error("Private runtime must be owned and mode 0700");
    if (::lstat((runtime / remote).c_str(), &info) || !S_ISSOCK(info.st_mode) || info.st_uid != ::getuid())
        throw std::runtime_error("Private daemon socket is missing or not owned");
    if (!output.is_absolute() || !std::filesystem::is_directory(output) || !std::filesystem::is_empty(output))
        throw std::runtime_error("Fresh absolute recording directory is required");
}
} // namespace

int main(int argc, char** argv)
{
    QJsonArray checks;
    QJsonObject observations;
    bool passed = false;
    QString error;
    auto require = [&](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
        checks.append(message);
    };
    try {
        if (argc != 3 && (argc != 4 || std::string_view(argv[3]) != "--functional-only"))
            throw std::runtime_error("Usage: preparation-controller-check PRIVATE_RUNTIME EMPTY_RECORDING_DIR [--functional-only]");
        const rlimit noCore{0, 0};
        require(::setrlimit(RLIMIT_CORE, &noCore) == 0 && ::prctl(PR_SET_DUMPABLE, 0) == 0,
                "Expected failure cannot create a core dump");
        const std::filesystem::path runtime(argv[1]), output(argv[2]);
        validatePrivate(runtime, output);
        require(qputenv("XDG_CACHE_HOME", QByteArray::fromStdString((runtime / "cache").string())),
                "Preparation cache is confined to the private runtime");
        require(qputenv("XDG_DATA_HOME", QByteArray::fromStdString((runtime / "data").string())),
                "Persistent library data is confined to the private runtime");
        QCoreApplication app(argc, argv);
        app.setOrganizationName("CONFLUX");
        app.setApplicationName("preparation-controller-check");
        QTemporaryDir fixtures(QString::fromStdString((runtime / "controller-fixtures-XXXXXX").string()));
        require(fixtures.isValid(), "Generated fixtures have a private temporary owner");
        const auto a = fixtures.filePath("generated-a.wav"), b = fixtures.filePath("generated-b.wav");
        writeFixture(a, 4096, -2048);
        writeFixture(b, 2048, 3072);
        pw_init(nullptr, nullptr);
        Sink sink("conflux-preparation-controller-master-" + std::to_string(::getpid()));
        conflux::DeckController controller;
        controller.refreshAudioRouting();
        QString left, right;
        wait([&] {
            for (const auto& value : controller.audioRouting()["ports"].toList()) {
                const auto port = value.toMap();
                const auto label = port["label"].toString();
                const auto prefix = QString::fromStdString(sink.name()) + " · input_";
                if (label == prefix + "0 [FL]") left = port["key"].toString();
                if (label == prefix + "1 [FR]") right = port["key"].toString();
            }
            return !left.isEmpty() && !right.isEmpty();
        }, "Private stereo sink discovery timed out");
        controller.configureAudioRouting(left, right, {}, {});
        require(controller.error().isEmpty(), "Exact private master pair is configured with cue disabled");
        controller.loadDeck(0, QUrl::fromLocalFile(a));
        wait([&] { return !controller.busy() && controller.outputReady(); }, "Initial prepared A output did not become ready");
        require(controller.error().isEmpty() && controller.decks()[0].toMap()["loaded"].toBool(),
                "Generated A loads on the private output");
        controller.setPlaying(0, true);
        wait([&] { return controller.audioState().playing[0] && controller.audioState().sourceFrame[0] >= 4096; },
             "A transport did not advance");
        sink.validate(true);
        wait([&] { return sink.received(0) >= 1024 && sink.received(1) >= 1024; }, "Steady private samples did not arrive");
        require(!sink.wrong() && !sink.zeros(), "Steady A matches independent asymmetric PCM samples");
        wait([&] { return controller.libraryReady(); }, "Private library startup did not finish");
        require(controller.libraryError().isEmpty() && controller.libraryTracks().isEmpty(),
                "Library startup finishes empty without disturbing active A playback");
        const auto catalogPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/library.json";
        const auto catalogSnapshot = [&] {
            QFile file(catalogPath);
            if (!file.exists()) return std::pair{false, QByteArray{}};
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read the private library catalog");
            const auto bytes = file.readAll();
            if (file.error() != QFileDevice::NoError) throw std::runtime_error("Cannot read the private library catalog");
            return std::pair{true, bytes};
        };
        const auto catalogBefore = catalogSnapshot();
        const auto importBefore = controller.audioState();
        const auto importSessions = controller.completedSessions();
        const auto importCommands = controller.submittedCommands();
        const auto importReceived = sink.received(0);
        QLockFile importGate(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                                 .filePath("prepared/v1/.lock"));
        require(importGate.tryLock(0), "Private cache lock gates import while GUI polling stays active");
        controller.importTrack(QUrl::fromLocalFile(b));
        require(controller.busy() && controller.preparation()["deck"].toInt() == -1,
                "Library import starts without targeting a playback deck");
        wait([&] {
            if (!controller.busy() || controller.preparation()["status"].toString() != "Preparing")
                throw std::runtime_error("Import did not remain Preparing behind the private cache lock");
            return controller.audioState().outputFrame >= importBefore.outputFrame + 1024;
        }, "A snapshot did not advance while import waited on the private cache lock");
        const auto gatedImport = controller.audioState();
        require(controller.busy() && controller.preparation()["status"].toString() == "Preparing" &&
                controller.preparation()["cancellable"].toBool() && controller.outputReady() &&
                controller.completedSessions() == importSessions && controller.submittedCommands() == importCommands &&
                gatedImport.playing[0] && gatedImport.sourceGeneration == importBefore.sourceGeneration &&
                gatedImport.sourceFrame[0] - importBefore.sourceFrame[0] == gatedImport.outputFrame - importBefore.outputFrame &&
                !gatedImport.invalidBuffers && !gatedImport.rejected && !gatedImport.sourceReadErrorObserved[0] &&
                !gatedImport.sourceReadError[0] && !gatedImport.starvedFrames[0] && !sink.wrong() && !sink.zeros(),
                "GUI polling during pending import preserves the same healthy advancing A session");
        importGate.unlock();
        require(QThreadPool::globalInstance()->waitForDone(8000),
                "B import preparation finishes while GUI completion remains queued");
        wait([&] { return sink.received(0) >= importReceived + 1024; },
             "A did not render while import completion was withheld", false);
        require(controller.preparation()["cancellable"].toBool() && controller.libraryTracks().isEmpty() &&
                !controller.decks()[1].toMap()["loaded"].toBool(),
                "Completed import remains cancellable before any catalog or deck acceptance");
        require(controller.cancelPreparation(), "Queued B import cancellation is accepted");
        require(controller.busy(), "Import cancellation stays busy until candidate cleanup");
        wait([&] { return !controller.busy() && controller.audioState().outputFrame >= importBefore.outputFrame + 1024; },
             "Cancelled import cleanup or advancing A snapshot timed out");
        const auto cancelledImport = controller.audioState();
        require(controller.preparation()["status"].toString() == "Cancelled" && controller.libraryTracks().isEmpty() &&
                catalogSnapshot() == catalogBefore && !controller.decks()[1].toMap()["loaded"].toBool() &&
                controller.outputReady() && controller.completedSessions() == importSessions &&
                controller.submittedCommands() == importCommands && cancelledImport.playing[0] &&
                cancelledImport.sourceGeneration == importBefore.sourceGeneration &&
                cancelledImport.sourceFrame[0] > importBefore.sourceFrame[0] &&
                cancelledImport.sourceFrame[0] - importBefore.sourceFrame[0] == cancelledImport.outputFrame - importBefore.outputFrame,
                "Cancelling queued import leaves catalog and decks unchanged on the same advancing A session");
        require(!cancelledImport.invalidBuffers && !cancelledImport.rejected &&
                !cancelledImport.sourceReadErrorObserved[0] && !cancelledImport.sourceReadError[0] && !cancelledImport.starvedFrames[0] &&
                !sink.wrong() && !sink.zeros(),
                "Import cancellation preserves exact A samples without source or output failures");
        controller.importTrack(QUrl::fromLocalFile(b));
        wait([&] { return !controller.busy() && controller.audioState().outputFrame >= cancelledImport.outputFrame + 1024; },
             "Retried B import saving or advancing A snapshot timed out");
        const auto imported = controller.audioState();
        require(controller.preparation()["status"].toString() == "Complete" &&
                controller.libraryError().isEmpty() && controller.libraryTracks().size() == 1,
                "Retried import completes one persistent library record");
        const auto row = controller.libraryTracks()[0].toMap();
        const auto libraryId = row["id"].toString();
        const auto idBytes = libraryId.toLatin1();
        const auto savedCatalog = catalogSnapshot();
        require(idBytes.size() == 64 && std::ranges::all_of(idBytes, [](char value) {
                    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                }) && row["path"].toString() == b && row["name"].toString() == "generated-b.wav" &&
                row["durationSeconds"].toDouble() == 60.0 && row["available"].toBool() &&
                savedCatalog.first && savedCatalog != catalogBefore,
                "Saved B row exposes its content id, local path, duration and availability");
        require(!controller.decks()[1].toMap()["loaded"].toBool() && controller.outputReady() &&
                controller.completedSessions() == importSessions && controller.submittedCommands() == importCommands &&
                imported.playing[0] && imported.sourceGeneration == importBefore.sourceGeneration &&
                imported.sourceFrame[0] > cancelledImport.sourceFrame[0] &&
                imported.sourceFrame[0] - cancelledImport.sourceFrame[0] == imported.outputFrame - cancelledImport.outputFrame &&
                !imported.invalidBuffers && !imported.rejected && !imported.sourceReadErrorObserved[0] &&
                !imported.sourceReadError[0] && !imported.starvedFrames[0] && !sink.wrong() && !sink.zeros(),
                "Import saves metadata without installing B or changing commands, exact A samples or the shared session");
        observations["library_import"] = QJsonObject{
            {"gated_at_output_frame", qint64(gatedImport.outputFrame)},
            {"cancelled_at_output_frame", qint64(cancelledImport.outputFrame)},
            {"saved_at_output_frame", qint64(imported.outputFrame)}, {"id", libraryId},
            {"catalog_records", qint64(controller.libraryTracks().size())}, {"b_loaded", false},
            {"commands_before", qint64(importCommands)}, {"commands_after", qint64(controller.submittedCommands())}};
        const auto before = controller.audioState();
        const auto sessions = controller.completedSessions();
        const auto received = sink.received(0);
        controller.loadLibraryTrack(1, libraryId);
        require(QThreadPool::globalInstance()->waitForDone(8000), "B preparation finishes while GUI completion remains queued");
        wait([&] { return sink.received(0) >= received + 1024; }, "A did not render while GUI completion was withheld", false);
        require(controller.preparation()["cancellable"].toBool() && !controller.decks()[1].toMap()["loaded"].toBool(),
                "Completed B preparation remains cancellable before GUI installation");
        require(controller.cancelPreparation(), "Queued B preparation cancellation is accepted");
        require(controller.busy(), "Cancellation keeps ownership busy until cleanup");
        wait([&] { return !controller.busy() && controller.audioState().outputFrame >= before.outputFrame + 1024; },
             "Cancelled preparation cleanup or advancing A snapshot timed out");
        const auto cancelled = controller.audioState();
        require(controller.preparation()["status"].toString() == "Cancelled" &&
                !controller.decks()[1].toMap()["loaded"].toBool() && controller.outputReady() &&
                controller.libraryTracks().size() == 1 && controller.libraryTracks()[0].toMap()["id"].toString() == libraryId &&
                catalogSnapshot() == savedCatalog &&
                controller.completedSessions() == sessions && cancelled.playing[0] &&
                cancelled.sourceFrame[0] > before.sourceFrame[0] &&
                cancelled.sourceFrame[0] - before.sourceFrame[0] == cancelled.outputFrame - before.outputFrame,
                "Cancelling queued B preserves unloaded B and the same advancing A/output clock");
        require(!cancelled.invalidBuffers && !cancelled.rejected &&
                !cancelled.sourceReadErrorObserved[0] && !cancelled.sourceReadError[0] && !cancelled.starvedFrames[0] &&
                !cancelled.sourceReadErrorObserved[1] && !cancelled.sourceReadError[1] && !cancelled.starvedFrames[1] &&
                !sink.wrong() && !sink.zeros(), "Cancellation preserves exact A samples without invalid buffers or source failures");
        observations["cancelled_at_output_frame"] = qint64(cancelled.outputFrame);
        const auto phasePath = QString::fromStdString((output.parent_path() / "controller-preparation-phase.json").string());
        bool shutdownCalled = false, shutdownReturned = false, phaseSaved = true;
        conflux::audio::LiveSession::State atComplete{};
        QJsonObject completion;
        QObject::connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            const auto preparation = controller.preparation();
            if (shutdownCalled || preparation["status"].toString() != "Complete" || preparation["deck"].toInt() != 1)
                return;
            atComplete = controller.audioState();
            completion = {{"stage", "before-direct-complete-shutdown"}, {"cache_hit", preparation["cacheHit"].toBool()},
                {"checks", checks}, {"execution_deadline_gate", false},
                {"cancelled_at_output_frame", qint64(cancelled.outputFrame)},
                {"output_frame", qint64(atComplete.outputFrame)}, {"a_source_frame", qint64(atComplete.sourceFrame[0])},
                {"b_source_generation", qint64(atComplete.sourceGeneration[1])}, {"b_playing", atComplete.playing[1]},
                {"b_source_frame", qint64(atComplete.sourceFrame[1])}, {"busy", controller.busy()},
                {"checked_samples_left", qint64(sink.received(0))}, {"checked_samples_right", qint64(sink.received(1))},
                {"wrong_samples", qint64(sink.wrong())}, {"zero_samples", qint64(sink.zeros())}};
            phaseSaved = save(phasePath, completion);
            shutdownCalled = true;
            if (!phaseSaved) return;
            sink.validate(false); // Intentional shutdown silence is outside the checked prefix.
            controller.shutdown();
            shutdownReturned = true;
            completion["stage"] = "direct-shutdown-returned-awaiting-poll-return";
            completion["shutdown_returned"] = true;
            phaseSaved = save(phasePath, completion) && phaseSaved;
        }, Qt::DirectConnection);
        controller.loadLibraryTrack(1, libraryId);
        // On the unfixed controller this delivers first-B Complete inside
        // poll()->settleReplacement()->retired(), then dereferences session_
        // after the direct shutdown cleared it. The wrapper preserves RED.
        wait([&] { return shutdownReturned || !phaseSaved; }, "Cached B completion/direct shutdown was not observed");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        require(phaseSaved && shutdownCalled && !controller.outputReady() && !controller.busy(),
                "Direct Complete shutdown returns through the enclosing poll safely");
        require(completion["cache_hit"].toBool() && atComplete.sourceGeneration[1] > 0 &&
                !atComplete.playing[1] && atComplete.sourceFrame[1] == 0 &&
                atComplete.playing[0] && atComplete.sourceFrame[0] > cancelled.sourceFrame[0] &&
                atComplete.sourceFrame[0] - cancelled.sourceFrame[0] == atComplete.outputFrame - cancelled.outputFrame &&
                !atComplete.invalidBuffers && !atComplete.rejected && !atComplete.starvedFrames[0] &&
                !atComplete.sourceReadError[0] && !atComplete.sourceReadErrorObserved[0] &&
                !atComplete.starvedFrames[1] && !atComplete.sourceReadError[1] && !atComplete.sourceReadErrorObserved[1] &&
                completion["wrong_samples"].toInteger() == 0 && completion["zero_samples"].toInteger() == 0,
                "Retry reuses persisted B preparation while A remains exact and B installs paused on the same clock");
        require(controller.completedSessions() == sessions + 1, "Only explicit shutdown completes the shared playback session");
        require(catalogSnapshot() == savedCatalog,
                "Loading and shutting down preserve the persistent imported library record");
        observations["completion"] = completion;
        observations["fixture_frames_per_track"] = qint64(fixtureFrames);
        runStemController(output, fixtures.path(), a, b, libraryId, argc == 4, require, observations);
        passed = true;
    } catch (const std::exception& failure) {
        error = QString::fromUtf8(failure.what());
        if (argc >= 3) {
            const auto path = QDir(QString::fromLocal8Bit(argv[2])).filePath("generated-stem-controller.wav");
            if (QFile::exists(path)) {
                try {
                    observations["failed_stem_recording"] = inspectStemRecording(path, 0, 0);
                } catch (const std::exception& diagnostic) {
                    observations["failed_stem_recording_error"] = QString::fromUtf8(diagnostic.what());
                }
            }
        }
    }
    const QJsonObject result{{"passed", passed}, {"error", error}, {"checks", checks},
        {"execution_deadline_gate", false}, {"observations", observations},
        {"scope", "Private virtual PipeWire graph; generated PCM and fake separator, private cache/library data. Functional import/preparation/stem bank/gain/cancellation/recording/reentrancy checks; no model, hardware or latency qualification."}};
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    return passed ? 0 : 1;
}

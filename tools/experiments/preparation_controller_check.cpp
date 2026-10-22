// SPDX-License-Identifier: AGPL-3.0-or-later
// Functional controller integration on the private_routing_check.py daemon.
// Generated audio and cache stay inside its private runtime; no host output.
#include "ui/DeckController.hpp"
#include "library/LiveStemObservations.hpp"
#include "../../tests/StemExportFixture.hpp"
#include "../../tests/StemServiceFixture.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
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
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <exception>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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

void writeFixture(const QString& path, qint16 left, qint16 right, std::uint32_t totalFrames = fixtureFrames,
                  std::uint32_t sampleRate = 48000)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot create private PCM fixture");
    QDataStream header(&file);
    header.setByteOrder(QDataStream::LittleEndian);
    header.writeRawData("RIFF", 4);
    header << quint32(36 + totalFrames * 4);
    header.writeRawData("WAVEfmt ", 8);
    header << quint32(16) << quint16(1) << quint16(2) << quint32(sampleRate)
           << quint32(sampleRate * 4) << quint16(4) << quint16(16);
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

QJsonObject inspectStemRecording(const QString& path, std::uint64_t prefixFrames, std::uint64_t expectedFrames,
                                 float expectedGain = 1.F)
{
    if (!std::isfinite(expectedGain) || expectedGain < 0.F || expectedGain > 1.F)
        throw std::runtime_error("Invalid generated master recording oracle gain");
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
                const auto target = expected[channel] * expectedGain;
                if (!std::isfinite(value) || std::abs(value) > std::abs(target) + 1e-6F)
                    throw std::runtime_error("Stem recording contains nonfinite or unexpected master audio");
                if (frame < prefixFrames || frame + 1024 >= frames)
                    maximumError = std::max(maximumError, std::abs(value - target));
            }
            checkedFrames += frame < prefixFrames || frame + 1024 >= frames;
        }
        first += count;
    }
    if (maximumError > 1e-6F) throw std::runtime_error("Stem recording lost its independent master prefix or tail");
    return {{"frames", qint64(frames)}, {"prefix_frames", qint64(prefixFrames)},
        {"expected_master_multiplier", expectedGain},
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

    // Crossfader is global even while A (the command's default deck index) is
    // replacing. B stays muted but advancing, so its independent transport and
    // cue state can be checked without changing the recording oracle.
    sink.validate(false); // Intentional gain ramps and target replacement follow.
    const auto beforeCrossfader = controller.audioState();
    bool crossfaderRequestedWhileReplacing = false, crossfaderCommandAccepted = false;
    bool deckScopedCommandRejected = false;
    std::uint64_t crossfaderCommandsBefore = 0;
    const auto crossfaderConnection = QObject::connect(&controller, &conflux::DeckController::stateChanged,
        &controller, [&] {
            if (crossfaderRequestedWhileReplacing || !controller.decks()[0].toMap()["replacing"].toBool()) return;
            crossfaderRequestedWhileReplacing = true; // Setter failure also emits stateChanged.
            const auto deckCommandsBefore = controller.submittedCommands();
            controller.setDeckGain(0, .5);
            deckScopedCommandRejected = controller.submittedCommands() == deckCommandsBefore &&
                !controller.error().isEmpty();
            crossfaderCommandsBefore = controller.submittedCommands();
            controller.setCrossfader(.75);
            crossfaderCommandAccepted = controller.submittedCommands() == crossfaderCommandsBefore + 1 &&
                controller.error().isEmpty();
        }, Qt::DirectConnection);
    try {
        controller.loadDeck(0, QUrl::fromLocalFile(a));
        wait([&] { return !controller.busy() && !controller.audioState().stemBank[0]; },
             "Crossfader target A replacement did not finish");
    } catch (...) {
        QObject::disconnect(crossfaderConnection);
        throw;
    }
    QObject::disconnect(crossfaderConnection);
    const auto crossfaderInstalled = controller.audioState();
    require(crossfaderRequestedWhileReplacing && deckScopedCommandRejected && crossfaderCommandAccepted &&
            controller.error().isEmpty() &&
            crossfaderInstalled.crossfader == .75F && controller.crossfader() == .75 &&
            crossfaderInstalled.applied > beforeCrossfader.applied &&
            crossfaderInstalled.sourceGeneration[0] > beforeCrossfader.sourceGeneration[0] &&
            !crossfaderInstalled.playing[0] && !crossfaderInstalled.sourceFrame[0] &&
            crossfaderInstalled.sourceGeneration[1] == beforeCrossfader.sourceGeneration[1] &&
            crossfaderInstalled.playing[1] && crossfaderInstalled.deckGain[1] == 0.F &&
            crossfaderInstalled.sourceFrame[1] - beforeCrossfader.sourceFrame[1] ==
                crossfaderInstalled.outputFrame - beforeCrossfader.outputFrame &&
            crossfaderInstalled.cueFrame[1] == beforeCrossfader.cueFrame[1] &&
            crossfaderInstalled.cueSelected == beforeCrossfader.cueSelected &&
            crossfaderInstalled.cueGain == beforeCrossfader.cueGain &&
            controller.completedSessions() == sessions + 1,
            "Target deck controls stay blocked while global crossfader applies during A replacement and survives the swap without resetting peer or cue state");

    // Known A samples at x=.75 have a .5 send; master .5 must therefore reach
    // the recorder at .25 of the original amplitude. Attach after all ramps,
    // then check every recorded frame rather than guessing callback boundaries.
    controller.setPlaying(0, true);
    controller.setMasterGain(.5);
    wait([&] {
        const auto state = controller.audioState();
        return state.playing[0] && state.sourceFrame[0] >= 4096 && state.crossfader == .75F && state.masterGain == .5F;
    }, "Crossfader/master targets or restarted A were not acknowledged");
    const auto crossfaderSettledAt = controller.audioState().outputFrame;
    wait([&] { return controller.audioState().outputFrame >= crossfaderSettledAt + 1024; },
         "Crossfader/master ramps did not settle before recording");
    const auto crossfaderPath = QString::fromStdString((output / "generated-crossfader-controller.wav").string());
    controller.startRecording(QUrl::fromLocalFile(crossfaderPath));
    wait([&] { return controller.recording()["status"].toString() == "Recording" &&
                      controller.recording()["acceptedFrames"].toULongLong() >= 2048; },
         "Post-master crossfader recording did not attach");
    controller.stopRecording();
    wait([&] { return !controller.recording()["active"].toBool(); }, "Crossfader recording did not finalize");
    const auto crossfaderRecording = controller.recording();
    require(crossfaderRecording["status"].toString() == "Complete" &&
            crossfaderRecording["error"].toString().isEmpty() &&
            crossfaderRecording["acceptedFrames"] == crossfaderRecording["writtenFrames"] &&
            crossfaderRecording["overflowFrames"].toULongLong() == 0,
            "Crossfader recorder finalizes all accepted post-master frames without overflow");
    const auto crossfaderFrames = crossfaderRecording["writtenFrames"].toULongLong();
    const auto crossfaderWav = inspectStemRecording(crossfaderPath, crossfaderFrames, crossfaderFrames, .25F);
    const auto afterCrossfader = controller.audioState();
    require(afterCrossfader.crossfader == .75F && afterCrossfader.masterGain == .5F &&
            !afterCrossfader.invalidBuffers && !afterCrossfader.rejected &&
            !afterCrossfader.sourceReadError[0] && !afterCrossfader.sourceReadError[1] &&
            !afterCrossfader.sourceReadErrorObserved[0] && !afterCrossfader.sourceReadErrorObserved[1] &&
            controller.completedSessions() == sessions + 1,
            "Crossfader recording preserves its applied global targets and healthy shared session");

    // Exercise queued controls on the already installed normal A. Constant PCM
    // supplies an independent all-frame amplitude oracle; exact cursor deltas
    // separately check wraps and linear EXIT, not arbitrary-wave alignment.
    using LoopSnapshot = conflux::audio::LiveSession::State;
    constexpr std::uint64_t loopInFrame = 2400, maximumLoopLength = 48000 / 2;
    constexpr std::uint64_t normalLoopByteBudget = 64ULL * 48000 * 8;
    const auto loopSessions = controller.completedSessions();
    QJsonArray loopSnapshots;
    QJsonObject loopDetail{{"scope", "Generated constant prepared PCM, queued controller controls, cursor accounting and every recorded master frame. No physical, musical, delivery-latency or sustained qualification."},
        {"normal_cache_payload_budget_bytes", qint64(normalLoopByteBudget)},
        {"maximum_fixture_loop_frames", qint64(maximumLoopLength)}};
    const auto loopDetailPath = QString::fromStdString((output / "generated-loop-controller-detail.json").string());
    auto loopHealth = [&] {
        if (!controller.error().isEmpty()) throw std::runtime_error("Manual loop controller error: " + controller.error().toStdString());
        const auto state = controller.audioState();
        if (!controller.outputReady() || controller.completedSessions() != loopSessions ||
            state.sourceGeneration != afterCrossfader.sourceGeneration || state.sourceFrames != afterCrossfader.sourceFrames ||
            state.invalidBuffers != afterCrossfader.invalidBuffers || state.rejected != afterCrossfader.rejected ||
            state.sourceReadError[0] || state.sourceReadError[1] || state.sourceReadErrorObserved[0] || state.sourceReadErrorObserved[1] ||
            state.loops[0].errorCode || !state.playing[1] || state.sourceFrame[1] >= state.sourceFrames[1] ||
            state.deckGain != afterCrossfader.deckGain || state.stemGain != afterCrossfader.stemGain ||
            state.masterGain != afterCrossfader.masterGain || state.crossfader != afterCrossfader.crossfader ||
            state.cueGain != afterCrossfader.cueGain || state.cueFrame != afterCrossfader.cueFrame ||
            state.cueSelected != afterCrossfader.cueSelected || state.cueHeld != afterCrossfader.cueHeld ||
            state.cuePreview != afterCrossfader.cuePreview)
            throw std::runtime_error("Manual loop changed shared source/peer/cue/gain health or exhausted the normal peer");
        return state;
    };
    auto noteLoop = [&](const char* stage, const LoopSnapshot& state) {
        if (loopSnapshots.size() >= 20) throw std::runtime_error("Manual-loop snapshot evidence bound exceeded");
        const auto& loop = state.loops[0];
        loopSnapshots.append(QJsonObject{{"stage", stage}, {"output_frame", qint64(state.outputFrame)},
            {"source_frames", QJsonArray{qint64(state.sourceFrame[0]), qint64(state.sourceFrame[1])}},
            {"source_generations", QJsonArray{qint64(state.sourceGeneration[0]), qint64(state.sourceGeneration[1])}},
            {"callbacks", qint64(state.callbacks)}, {"submitted", qint64(controller.submittedCommands())},
            {"applied", qint64(state.applied)}, {"rejected", qint64(state.rejected)}, {"last_command_id", qint64(state.lastCommandId)},
            {"A_starved_frames", qint64(state.starvedFrames[0])}, {"B_starved_frames", qint64(state.starvedFrames[1])},
            {"A_buffering", state.buffering[0]}, {"B_buffering", state.buffering[1]},
            {"loop", QJsonObject{{"IN", qint64(loop.inFrame)}, {"OUT", qint64(loop.outFrame)},
                {"limit", qint64(loop.limitFrame)}, {"ready", loop.ready}, {"in_set", loop.inSet},
                {"out_set", loop.outSet}, {"active", loop.active}, {"cached", loop.cached},
                {"resident_bytes", qint64(loop.residentBytes)}, {"error_code", loop.errorCode}}},
            {"acknowledged_deck_map", QJsonObject::fromVariantMap(controller.decks()[0].toMap()["loop"].toMap())}});
    };
    auto advancingPeer = [](const LoopSnapshot& before, const LoopSnapshot& after) {
        return after.outputFrame >= before.outputFrame && after.sourceFrame[1] >= before.sourceFrame[1] &&
            after.sourceFrame[1] - before.sourceFrame[1] == after.outputFrame - before.outputFrame;
    };
    try {
        noteLoop("before-pause", loopHealth());
        auto before = controller.audioState();
        auto submitted = controller.submittedCommands();
        controller.setPlaying(0, false);
        require(controller.submittedCommands() == submitted + 1 && controller.error().isEmpty(), "Manual-loop pause is queued on the existing controller session");
        wait([&] { const auto state = loopHealth(); return !state.playing[0] && state.applied == before.applied + 1; }, "Manual-loop pause was not acknowledged");
        noteLoop("paused", controller.audioState());
        before = controller.audioState(); submitted = controller.submittedCommands();
        controller.seekSeconds(0, loopInFrame / 48000.0);
        require(controller.submittedCommands() == submitted + 1 && controller.error().isEmpty(), "Paused loop IN cursor seek is queued");
        wait([&] { const auto state = loopHealth(); return !state.playing[0] && state.sourceFrame[0] == loopInFrame && state.applied == before.applied + 1; }, "Paused loop IN cursor was not acknowledged exactly");
        noteLoop("paused-IN-cursor", controller.audioState());
        before = controller.audioState(); submitted = controller.submittedCommands();
        controller.loopIn(0);
        require(controller.submittedCommands() == submitted + 1 && controller.error().isEmpty() &&
                !controller.audioState().loops[0].inSet && controller.audioState().sourceFrame[0] == loopInFrame &&
                controller.audioState().applied == before.applied,
                "IN queues without speculative loop state before callback acknowledgement");
        wait([&] { const auto state = loopHealth(); return state.applied == before.applied + 1 && state.loops[0].inSet && state.loops[0].ready; }, "Queued paused IN did not prepare its bounded PCM cache");
        const auto preparedIn = loopHealth();
        const auto inMap = controller.decks()[0].toMap()["loop"].toMap();
        require(!preparedIn.playing[0] && preparedIn.sourceFrame[0] == loopInFrame && preparedIn.loops[0].inFrame == loopInFrame &&
                !preparedIn.loops[0].outSet && !preparedIn.loops[0].active && inMap["inSet"].toBool() && inMap["ready"].toBool() &&
                inMap["inSeconds"].toDouble() == loopInFrame / 48000.0 && preparedIn.loops[0].residentBytes > 0 &&
                preparedIn.loops[0].residentBytes <= normalLoopByteBudget,
                "Paused IN captures exact frame2400 and exposes only its acknowledged bounded normal-PCM cache");
        noteLoop("IN-ready", preparedIn);
        before = controller.audioState(); submitted = controller.submittedCommands();
        controller.setPlaying(0, true);
        require(controller.submittedCommands() == submitted + 1 && controller.error().isEmpty(), "Loop target restart is queued without changing peer or gains");
        wait([&] { const auto state = loopHealth(); return state.applied == before.applied + 1 && state.playing[0] && state.sourceFrame[0] >= loopInFrame + 2048; }, "Loop target did not pass its existing transport ramp and OUT margin");
        const auto beforeOut = loopHealth();
        submitted = controller.submittedCommands();
        controller.loopOut(0);
        require(controller.submittedCommands() == submitted + 1 && controller.error().isEmpty() &&
                !controller.audioState().loops[0].outSet && controller.audioState().sourceFrame[0] == beforeOut.sourceFrame[0] &&
                controller.audioState().applied == beforeOut.applied,
                "OUT queues without a speculative cursor jump or loop activation");
        wait([&] { const auto state = loopHealth(); return state.applied == beforeOut.applied + 1 && state.loops[0].ready && state.loops[0].outSet && state.loops[0].active; }, "Queued OUT did not enter its prepared loop");
        const auto outAck = loopHealth();
        const auto in = outAck.loops[0].inFrame, out = outAck.loops[0].outFrame, length = out - in;
        require(in == loopInFrame && out >= beforeOut.sourceFrame[0] && length >= 2048 && length <= maximumLoopLength &&
                out <= outAck.loops[0].limitFrame && outAck.sourceFrame[0] >= in && outAck.sourceFrame[0] < out &&
                outAck.loops[0].cached && advancingPeer(preparedIn, outAck),
                "Callback-captured OUT has a short valid range and rewinds only A while the muted peer clock stays exact");
        loopDetail["IN_frame"] = qint64(in); loopDetail["OUT_frame"] = qint64(out); loopDetail["length_frames"] = qint64(length);
        noteLoop("OUT-active", outAck);

        const auto loopPath = QString::fromStdString((output / "generated-loop-controller.wav").string());
        controller.startRecording(QUrl::fromLocalFile(loopPath));
        wait([&] { (void)loopHealth(); return controller.recording()["status"].toString() == "Recording" && controller.recording()["acceptedFrames"].toULongLong() >= 1024; }, "Loop recording did not attach after settled OUT");
        const auto first = loopHealth(); noteLoop("recording-first-anchor", first);
        wait([&] { return loopHealth().outputFrame >= first.outputFrame + 3 * length; }, "Active loop did not render three complete wrap intervals");
        const auto repeated = loopHealth(); noteLoop("after-three-wrap-intervals", repeated);
        const auto repeatedExpected = in + (first.sourceFrame[0] - in + repeated.outputFrame - first.outputFrame) % length;
        require(repeated.sourceFrame[0] == repeatedExpected && advancingPeer(first, repeated) &&
                repeated.starvedFrames == first.starvedFrames && !repeated.buffering[0] && !repeated.buffering[1],
                "Repeated loop cursor follows exact modulo output frames while the peer advances once and neither deck starves");
        submitted = controller.submittedCommands();
        controller.loopToggle(0);
        require(controller.submittedCommands() == submitted + 1 && controller.error().isEmpty() && controller.audioState().loops[0].active,
                "EXIT is queued without speculative loop state");
        wait([&] { const auto state = loopHealth(); return state.applied == repeated.applied + 1 && !state.loops[0].active; }, "Queued EXIT was not acknowledged");
        const auto exited = loopHealth(); noteLoop("EXIT-acknowledged", exited);
        require(exited.loops[0].outSet && exited.loops[0].ready && exited.sourceFrame[0] >= in &&
                (exited.sourceFrame[0] - in) % length ==
                    (repeated.sourceFrame[0] - in + exited.outputFrame - repeated.outputFrame) % length &&
                advancingPeer(repeated, exited), "EXIT preserves the running loop phase modulo its pre-acknowledgement wraps");
        loopDetail["cached_remainder_observed_at_EXIT_ack"] = exited.loops[0].cached;
        wait([&] { const auto state = loopHealth(); return !state.loops[0].cached && state.sourceFrame[0] >= out + 512; }, "EXIT did not consume the cached remainder and resume past the parked OUT");
        const auto linear = loopHealth(); noteLoop("EXIT-linear-past-OUT", linear);
        require(!linear.loops[0].active && linear.loops[0].outSet &&
                linear.sourceFrame[0] - exited.sourceFrame[0] == linear.outputFrame - exited.outputFrame &&
                advancingPeer(exited, linear) && linear.starvedFrames == first.starvedFrames,
                "After EXIT acknowledgement A continues linearly without a cursor jump or lost samples and B remains independent");
        submitted = controller.submittedCommands();
        controller.loopToggle(0);
        require(controller.submittedCommands() == submitted + 1 && controller.error().isEmpty() && !controller.audioState().loops[0].active,
                "RELOOP after linear playback is queued without optimistic activation");
        wait([&] { const auto state = loopHealth(); return state.applied == linear.applied + 1 && state.loops[0].active && state.loops[0].cached; }, "Queued RELOOP did not return to the retained prepared range");
        const auto relooped = loopHealth(); noteLoop("RELOOP-acknowledged", relooped);
        require(relooped.sourceFrame[0] >= in && relooped.sourceFrame[0] < out && relooped.loops[0].inFrame == in &&
                relooped.loops[0].outFrame == out && relooped.playing[0] && advancingPeer(linear, relooped),
                "RELOOP returns the playing target to its original cached bounds without disturbing the peer");
        wait([&] { return loopHealth().outputFrame >= relooped.outputFrame + 2 * length; }, "RELOOP did not render two complete wrap intervals");
        const auto twice = loopHealth(); noteLoop("RELOOP-after-two-wrap-intervals", twice);
        require(twice.sourceFrame[0] == in + (relooped.sourceFrame[0] - in + twice.outputFrame - relooped.outputFrame) % length &&
                advancingPeer(relooped, twice) && twice.starvedFrames == first.starvedFrames && !twice.buffering[0] && !twice.buffering[1],
                "RELOOP repeats exact modulo clocks with no cache or peer starvation");
        controller.stopRecording();
        wait([&] { (void)loopHealth(); return !controller.recording()["active"].toBool(); }, "Loop recording did not finalize before further transport changes");
        const auto loopRecording = controller.recording();
        const auto loopFrames = loopRecording["writtenFrames"].toULongLong();
        require(loopRecording["status"].toString() == "Complete" && loopRecording["error"].toString().isEmpty() &&
                loopRecording["acceptedFrames"] == loopRecording["writtenFrames"] && loopFrames >= 5 * length &&
                loopRecording["overflowFrames"].toULongLong() == 0,
                "Loop recorder drains every accepted frame across wraps, EXIT and RELOOP without overflow");
        const auto loopWav = inspectStemRecording(loopPath, loopFrames, loopFrames, .25F);
        require(loopWav["checked_prefix_or_tail_frames"].toInteger() == qint64(loopFrames),
                "Every recorded loop/EXIT/RELOOP frame matches the independent post-master quarter-amplitude oracle");
        loopDetail["recording"] = loopWav;
        loopDetail["every_recorded_frame_checked"] = true;
        loopDetail["recorded_frames"] = qint64(loopFrames);

        before = loopHealth(); submitted = controller.submittedCommands();
        controller.setPlaying(0, false);
        require(controller.submittedCommands() == submitted + 1 && controller.error().isEmpty(), "Recorded loop pauses before the same-frame clear check");
        wait([&] { const auto state = loopHealth(); return !state.playing[0] && state.applied == before.applied + 1; }, "Recorded loop pause was not acknowledged");
        const auto paused = loopHealth(); noteLoop("paused-before-same-frame-clear", paused);
        submitted = controller.submittedCommands();
        controller.seekSeconds(0, paused.sourceFrame[0] / 48000.0);
        require(controller.submittedCommands() == submitted + 1 && controller.error().isEmpty(), "Same-frame paused seek queues explicit loop invalidation");
        wait([&] { const auto state = loopHealth(); return state.applied == paused.applied + 1 && !state.loops[0].inSet &&
                      !state.loops[0].outSet && !state.loops[0].cached && !state.loops[0].active && !state.loops[0].residentBytes; },
             "Same-frame paused seek did not clear bounds and release the optional cache");
        const auto cleared = loopHealth(); noteLoop("same-frame-cleared", cleared);
        require(cleared.sourceFrame[0] == paused.sourceFrame[0] && !cleared.playing[0] && advancingPeer(paused, cleared),
                "Same-frame seek clears loop storage without moving its paused target or resetting the advancing peer");
        loopDetail["passed"] = true;
        loopDetail["snapshots"] = loopSnapshots;
        require(save(loopDetailPath, loopDetail), "Bounded queued-loop controller and all-frame recording evidence is retained");
    } catch (const std::exception& error) {
        loopDetail["passed"] = false; loopDetail["error"] = error.what();
        try { noteLoop("failure-current", controller.audioState()); } catch (...) {}
        loopDetail["recording_at_failure"] = QJsonObject::fromVariantMap(controller.recording());
        loopDetail["snapshots"] = loopSnapshots;
        (void)save(loopDetailPath, loopDetail);
        throw;
    }

    const auto final = controller.audioState();
    sink.validate(false);
    controller.shutdown();
    require(controller.completedSessions() == sessions + 2 && !controller.outputReady() && !controller.busy(),
            "Only explicit routing rebuild and shutdown complete the stem controller's sessions");
    observations["stem_controller"] = QJsonObject{{"recording", wav},
        {"manual_loop", loopDetail},
        {"crossfader", QJsonObject{{"submitted_while_A_replacing", crossfaderRequestedWhileReplacing},
            {"deck_scoped_command_rejected", deckScopedCommandRejected},
            {"command_accepted", crossfaderCommandAccepted}, {"commands_before", qint64(crossfaderCommandsBefore)},
            {"position", afterCrossfader.crossfader}, {"master_gain", afterCrossfader.masterGain},
            {"A_generation_before", qint64(beforeCrossfader.sourceGeneration[0])},
            {"A_generation_after", qint64(crossfaderInstalled.sourceGeneration[0])},
            {"recording", crossfaderWav}}},
        {"virtual_sink_sample_gate", !functionalOnly},
        {"virtual_sink_wrong_samples", qint64(sink.wrong())}, {"virtual_sink_zero_samples", qint64(sink.zeros())},
        {"replacement_fixture_frames", qint64(targetFrames)},
        {"initial_bank_frames", qint64(loadedA.sourceFrames[0])}, {"bank_handoff_generation", qint64(bank.sourceGeneration[1])},
        {"normal_handoff_generation", qint64(normal.sourceGeneration[1])},
        {"completed_sessions", qint64(controller.completedSessions())}, {"final_callbacks", qint64(final.callbacks)},
        {"reported_starved_a", qint64(final.starvedFrames[0])}, {"reported_starved_b", qint64(final.starvedFrames[1])},
        {"steady_sample_tolerance", 1e-6}, {"scope", "Generated fake separator, actual cache/controller/render/PipeWire paths. No model inference, physical routing, latency, sustained or musical qualification."}};
}


template<class Check>
void runScratchController(const std::filesystem::path& output, const QString& a,
                          const QString& b, Check&& require, QJsonObject& observations)
{
    // Separate from every recorded forward-playback oracle above: a held
    // scratch intentionally produces silence and may move the source backwards.
    conflux::DeckController controller;
    Sink sink("conflux-scratch-controller-master-" + std::to_string(::getpid()));
    controller.refreshAudioRouting();
    QString left, right;
    wait([&] {
        for (const auto& value : controller.audioRouting()["ports"].toList()) {
            const auto port = value.toMap();
            const auto prefix = QString::fromStdString(sink.name()) + " · input_";
            if (port["label"].toString() == prefix + "0 [FL]") left = port["key"].toString();
            if (port["label"].toString() == prefix + "1 [FR]") right = port["key"].toString();
        }
        return !left.isEmpty() && !right.isEmpty();
    }, "Scratch controller private routing discovery timed out");
    controller.configureAudioRouting(left, right, {}, {});
    controller.loadDeck(0, QUrl::fromLocalFile(a));
    wait([&] { return !controller.busy() && controller.outputReady(); }, "Scratch target did not load");
    controller.loadDeck(1, QUrl::fromLocalFile(b));
    wait([&] { return !controller.busy() && controller.decks()[1].toMap()["loaded"].toBool(); }, "Scratch peer did not load");
    controller.setDeckGain(1, 0.0);
    controller.setPlaying(1, true);
    controller.seekSeconds(0, 1.0);
    wait([&] { const auto state = controller.audioState(); return state.playing[1] && !state.playing[0] &&
                      state.deckGain[1] == 0.F && state.sourceFrame[0] == 48000 && state.scratch[0].supported; },
         "Scratch target/advancing muted peer did not acknowledge initial controls");
    const auto initial = controller.audioState();
    const auto sessions = controller.completedSessions();
    QJsonArray snapshots;
    QJsonObject detail{{"scope", "Generated stereo PCM, actual controller/native acknowledgements and private virtual PipeWire only. No musical scratch, physical output, sample-amplitude or latency qualification."}};
    const auto detailPath = QString::fromStdString((output / "generated-scratch-controller-detail.json").string());
    auto healthy = [&] {
        const auto state = controller.audioState();
        if (!controller.error().isEmpty() || !controller.outputReady() || controller.completedSessions() != sessions ||
            state.sourceGeneration[1] != initial.sourceGeneration[1] || state.sourceFrames[1] != initial.sourceFrames[1] ||
            !state.playing[1] || state.sourceFrame[1] >= state.sourceFrames[1] ||
            state.invalidBuffers != initial.invalidBuffers || state.rejected != initial.rejected ||
            state.sourceReadError[0] || state.sourceReadError[1] ||
            state.sourceReadErrorObserved[0] || state.sourceReadErrorObserved[1] ||
            state.deckGain != initial.deckGain || state.stemGain != initial.stemGain ||
            state.masterGain != initial.masterGain || state.crossfader != initial.crossfader ||
            state.cueGain != initial.cueGain || state.cueFrame != initial.cueFrame ||
            state.cueSelected != initial.cueSelected || state.cueHeld != initial.cueHeld || state.cuePreview != initial.cuePreview)
            throw std::runtime_error("Scratch changed the shared peer/gain/cue/session health");
        return state;
    };
    auto peerAdvanced = [&](const auto& before, const auto& after) {
        require(after.outputFrame > before.outputFrame && after.sourceFrame[1] >= before.sourceFrame[1] &&
                after.sourceFrame[1] - before.sourceFrame[1] == after.outputFrame - before.outputFrame,
                "Scratch leaves the playing peer on the exact shared output clock");
    };
    auto note = [&](const char* stage) {
        const auto state = healthy();
        if (snapshots.size() >= 16) throw std::runtime_error("Scratch snapshot evidence bound exceeded");
        snapshots.append(QJsonObject{{"stage", stage}, {"output_frame", qint64(state.outputFrame)},
            {"source_frames", QJsonArray{qint64(state.sourceFrame[0]), qint64(state.sourceFrame[1])}},
            {"source_generations", QJsonArray{qint64(state.sourceGeneration[0]), qint64(state.sourceGeneration[1])}},
            {"applied", qint64(state.applied)}, {"submitted", qint64(controller.submittedCommands())},
            {"scratch", QJsonObject::fromVariantMap(controller.decks()[0].toMap()["scratch"].toMap())}});
    };
    auto begin = [&] {
        const auto token = controller.beginScratch(0);
        require(!token.isEmpty(), "Prepared scratch Begin is admitted through the actual controller");
        const auto nativeToken = token.toULongLong(nullptr, 16);
        wait([&] { const auto state = healthy(); return state.scratch[0].active && !state.scratch[0].pending &&
                          state.scratch[0].scratchToken == nativeToken; }, "Scratch Begin did not receive its matching native acknowledgement");
        require(controller.decks()[0].toMap()["scratch"].toMap()["scratchToken"].toString() == token,
                "Published scratch identity is the actual native acknowledgement");
        return token;
    };
    try {
        const auto pausedToken = begin();
        const auto held = healthy();
        require(!held.playing[0] && held.sourceFrame[0] == 48000, "Paused Begin preserves its captured source frame");
        note("paused-held");
        const auto holdStart = Clock::now();
        wait([&] { const auto state = healthy(); return Clock::now() - holdStart >= std::chrono::milliseconds(1100) &&
                          state.outputFrame >= held.outputFrame + 1024; }, "Stationary scratch contact did not survive over one second");
        const auto afterHold = healthy();
        require(afterHold.scratch[0].active && afterHold.sourceFrame[0] == held.sourceFrame[0] &&
                afterHold.scratch[0].position == held.scratch[0].position,
                "A stationary contact holds the paused source while callbacks and the peer continue");
        detail["stationary_hold_milliseconds"] = qint64(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - holdStart).count());
        peerAdvanced(held, afterHold); note("after-long-hold");
        require(controller.moveScratch(0, pausedToken, 960), "The same scratch token accepts motion after a stationary hold longer than one second");
        wait([&] { const auto state = healthy(); return state.scratch[0].position > afterHold.scratch[0].position; },
             "Resumed scratch motion did not advance actual consumed source position");
        const auto moved = healthy();
        require(!moved.playing[0] && moved.scratch[0].position <= afterHold.scratch[0].position + 960,
                "Paused scratch consumes bounded requested motion without changing play state");
        note("paused-moved");
        require(controller.endScratch(0, pausedToken), "Paused scratch End is admitted once");
        wait([&] { return !healthy().scratch[0].active; }, "Paused scratch End was not acknowledged");
        const auto pausedEnd = healthy();
        require(!pausedEnd.playing[0] && pausedEnd.sourceFrame[0] == std::uint64_t(std::floor(pausedEnd.scratch[0].position)) &&
                !controller.endScratch(0, pausedToken) && !controller.moveScratch(0, pausedToken, 1920),
                "End restores the paused linear cursor at actual scratch position and revokes the old token");
        note("paused-End");

        controller.setPlaying(0, true);
        wait([&] { return healthy().playing[0]; }, "Scratch target did not acknowledge Play");
        const auto playingToken = begin();
        const auto playingHeld = healthy();
        const auto submitted = controller.submittedCommands();
        require(playingToken != pausedToken && !controller.endScratch(0, pausedToken) &&
                !controller.moveScratch(0, pausedToken, -960) && controller.submittedCommands() == submitted && healthy().scratch[0].active,
                "Old Move/End cannot submit commands against a newer playing gesture");
        require(controller.moveScratch(0, playingToken, -960), "Playing scratch accepts reverse motion");
        wait([&] { return healthy().scratch[0].position < playingHeld.scratch[0].position; }, "Playing scratch did not consume reverse motion");
        require(healthy().playing[0], "Scratch motion preserves the acknowledged playing transport");
        note("playing-reverse");
        require(controller.endScratch(0, playingToken), "Playing scratch End is admitted");
        wait([&] { const auto state = healthy(); return !state.scratch[0].active && state.playing[0]; }, "Playing scratch End did not resume linear playback");
        const auto resumed = healthy();
        wait([&] { return healthy().sourceFrame[0] >= resumed.sourceFrame[0] + 1024; }, "Linear playback did not advance after scratch release");
        peerAdvanced(playingHeld, healthy()); note("playing-End-linear");

        const auto replacedToken = begin();
        const auto replacedGeneration = healthy().sourceGeneration[0];
        controller.loadDeck(0, QUrl::fromLocalFile(b));
        wait([&] { const auto state = healthy(); return !controller.busy() && state.sourceGeneration[0] != replacedGeneration &&
                          !state.scratch[0].active && !state.playing[0]; }, "Replacement did not release scratch and install the new paused source");
        require(!controller.decks()[0].toMap()["sourceId"].toString().isEmpty() &&
                controller.decks()[0].toMap()["sourceId"] == controller.decks()[1].toMap()["sourceId"],
                "Scratch replacement installs the same generated B content identity as the preserved peer");
        const auto replacementToken = begin();
        const auto beforeStale = controller.submittedCommands();
        require(replacementToken != replacedToken && !controller.endScratch(0, replacedToken) &&
                !controller.moveScratch(0, replacedToken, 480) && controller.submittedCommands() == beforeStale && healthy().scratch[0].active,
                "Replaced-source tokens cannot move or release the new source's gesture");
        note("replacement-new-gesture");
        const auto sameFrame = healthy().sourceFrame[0];
        controller.seekSeconds(0, sameFrame / 48000.0);
        wait([&] { return !healthy().scratch[0].active; }, "Same-frame explicit seek did not revoke active scratch");
        require(!controller.moveScratch(0, replacementToken, 960) && !controller.endScratch(0, replacementToken) &&
                healthy().sourceFrame[0] == sameFrame, "Same-frame transport retires the controller token without moving the paused cursor");
        peerAdvanced(initial, healthy()); note("same-frame-seek-retired");
        detail["passed"] = true;
        detail["snapshots"] = snapshots;
        require(save(detailPath, detail), "Actual controller scratch acknowledgements are retained separately from forward audio oracles");
        controller.shutdown();
        require(controller.completedSessions() == sessions + 1 && !controller.outputReady() && !controller.busy(),
                "Scratch controller shuts down its sole private session and all optional workers");
        observations["scratch_controller"] = detail;
    } catch (const std::exception& error) {
        detail["passed"] = false; detail["error"] = error.what(); detail["snapshots"] = snapshots;
        (void)save(detailPath, detail);
        controller.shutdown();
        throw;
    }
}


// The fake service has no native descendants in these modes. Bind every
// observed leader while alive; pidfd fallback is test safety after controller
// unwind, never a replacement for the product cleanup assertions below.
class ServiceTrace {
public:
    explicit ServiceTrace(conflux::test::StemServiceFixture fixture) : fixture_(std::move(fixture)) {}
    ~ServiceTrace()
    {
        try { capture(); } catch (...) {} // Bind any still-live failed-setup child before fallback.
        for (const auto& child : children_) {
            if (!exited(child)) {
                (void)::syscall(SYS_pidfd_send_signal, child.fd, SIGKILL, nullptr, 0);
                pollfd event{child.fd, POLLIN, 0};
                (void)::poll(&event, 1, 2000);
            }
            siginfo_t status{};
            (void)::waitid(P_PIDFD, id_t(child.fd), &status, WEXITED | WNOHANG);
            ::close(child.fd);
        }
    }
    std::vector<QJsonObject> rows() const
    {
        QFile file(fixture_.marker(".trace"));
        if (!file.exists()) return {};
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
            throw std::runtime_error("Cannot read bounded service identity trace");
        const auto bytes = file.readAll();
        const auto end = bytes.lastIndexOf('\n');
        std::vector<QJsonObject> result;
        for (const auto& line : bytes.first(end + 1).split('\n')) {
            if (line.isEmpty()) continue;
            const auto document = QJsonDocument::fromJson(line);
            if (!document.isObject()) throw std::runtime_error("Invalid complete fixture trace row");
            result.push_back(document.object());
        }
        return result; // Only a still-unwritten trailing row may be retried.
    }
    void capture()
    {
        for (const auto& row : rows()) {
            if (row["fixture"].toString() != "started") continue;
            const auto pid = pid_t(row["fixture_pid"].toInt());
            const auto ticks = row["fixture_start_ticks"].toString().toLatin1();
            if (pid <= 1 || ticks.isEmpty()) throw std::runtime_error("Invalid fixture process identity");
            if (std::ranges::any_of(children_, [&](const auto& child) { return child.pid == pid && child.ticks == ticks; })) continue;
            const auto fd = int(::syscall(SYS_pidfd_open, pid, 0));
            if (fd < 0 && errno == ESRCH) continue; // Already exited before observation; never signal its PID.
            QFile stat(QString("/proc/%1/stat").arg(pid));
            if (fd < 0) throw std::runtime_error("Cannot retain fixture process identity");
            if (!stat.open(QIODevice::ReadOnly)) {
                pollfd event{fd, POLLIN, 0};
                const bool alreadyExited = ::poll(&event, 1, 0) > 0 && (event.revents & POLLIN);
                if (fd >= 0) ::close(fd);
                if (alreadyExited) continue;
                throw std::runtime_error("Cannot bind live fixture process identity");
            }
            const auto raw = stat.readAll();
            const auto fields = raw.mid(raw.lastIndexOf(") ") + 2).split(' ');
            if (fields.size() <= 19 || fields[19] != ticks) {
                ::close(fd); throw std::runtime_error("Fixture PID identity changed before capture");
            }
            const auto serviceJob = row["fixture_job"].toString();
            const auto job = QFileInfo(serviceJob).absolutePath();
            if (QFileInfo(serviceJob).fileName() != "service" || !QFileInfo(job).fileName().startsWith(".live-")) {
                ::close(fd); throw std::runtime_error("Fixture did not identify its private live job");
            }
            children_.push_back({pid, fd, ticks, job});
        }
    }
    std::size_t size() const { return children_.size(); }
    bool alive(std::size_t index) const { return !exited(children_.at(index)); }
    bool gone(std::size_t index) const
    {
        const auto& child = children_.at(index);
        if (!exited(child) || QFileInfo::exists(child.job)) return false;
        QFile stat(QString("/proc/%1/stat").arg(child.pid));
        if (!stat.exists()) return true;
        if (!stat.open(QIODevice::ReadOnly)) return false;
        const auto raw = stat.readAll();
        const auto fields = raw.mid(raw.lastIndexOf(") ") + 2).split(' ');
        return fields.size() > 19 && fields[19] != child.ticks; // A reused PID is a different process.
    }
    bool charged(std::size_t index) const
    { return QFileInfo::exists(QDir(children_.at(index).job).filePath("reservation.json")); }
    std::size_t count(std::size_t index, const char* key, const char* value) const
    {
        const auto& child = children_.at(index);
        const auto values = rows();
        return std::size_t(std::ranges::count_if(values, [&](const auto& row) {
            return row["fixture_pid"].toInt() == child.pid && row["fixture_start_ticks"].toString().toLatin1() == child.ticks &&
                row[key].toString() == value;
        }));
    }
    QJsonArray identities() const
    {
        QJsonArray result;
        for (const auto& child : children_) result.append(QJsonObject{{"pid", child.pid},
            {"start_ticks", QString::fromLatin1(child.ticks)}, {"job", child.job}});
        return result;
    }
private:
    struct Child { pid_t pid; int fd; QByteArray ticks; QString job; };
    static bool exited(const Child& child)
    { pollfd event{child.fd, POLLIN, 0}; return ::poll(&event, 1, 0) > 0 && (event.revents & POLLIN); }
    conflux::test::StemServiceFixture fixture_;
    std::vector<Child> children_;
};

// A separate local pool would not block controller QtConcurrent work. This
// scoped barrier occupies the GLOBAL pool only after its loader is finished.
class RetirementGate {
public:
    RetirementGate() : previous_(QThreadPool::globalInstance()->maxThreadCount()), state_(std::make_shared<State>())
    {
        QThreadPool::globalInstance()->setMaxThreadCount(1);
        try {
            future_ = QtConcurrent::run([state = state_] {
                std::unique_lock lock(state->mutex);
                state->entered = true;
                state->changed.notify_all();
                state->changed.wait(lock, [&] { return state->released; });
            });
        } catch (...) {
            QThreadPool::globalInstance()->setMaxThreadCount(previous_);
            throw;
        }
        std::unique_lock lock(state_->mutex);
        if (!state_->changed.wait_for(lock, std::chrono::seconds(8), [&] { return state_->entered; })) {
            state_->released = true; state_->changed.notify_all();
            QThreadPool::globalInstance()->setMaxThreadCount(previous_);
            throw std::runtime_error("Cannot occupy the global retirement worker");
        }
    }
    ~RetirementGate() { release(); }
    void release()
    {
        if (!state_) return;
        { std::lock_guard lock(state_->mutex); state_->released = true; }
        state_->changed.notify_all();
        // Never waitForFinished on an unfinished Qt runnable from the GUI.
        while (!future_.isFinished()) QThread::msleep(1);
        future_.waitForFinished();
        QThreadPool::globalInstance()->setMaxThreadCount(previous_);
        state_.reset();
    }
private:
    struct State { std::mutex mutex; std::condition_variable changed; bool entered = false, released = false; };
    int previous_;
    std::shared_ptr<State> state_;
    QFuture<void> future_;
};

template<class Check>
void privateLivePair(conflux::DeckController& controller, Sink& sink, QString& left, QString& right, Check&& require)
{
    controller.refreshAudioRouting();
    wait([&] {
        for (const auto& value : controller.audioRouting()["ports"].toList()) {
            const auto port = value.toMap();
            const auto prefix = QString::fromStdString(sink.name()) + " · input_";
            if (port["label"].toString() == prefix + "0 [FL]") left = port["key"].toString();
            if (port["label"].toString() == prefix + "1 [FR]") right = port["key"].toString();
        }
        return !left.isEmpty() && !right.isEmpty() && controller.libraryReady();
    }, "Live controller private routing/library did not become ready");
    controller.configureAudioRouting(left, right, {}, {});
    require(controller.error().isEmpty(), "Live controller uses the exact private master pair with cue output disabled");
}

template<class Check>
void waitLiveLoad(conflux::DeckController& controller, int deck, Check&& require)
{
    wait([&] {
        if (!controller.busy() && !controller.error().isEmpty())
            throw std::runtime_error("Live controller load failed: " + controller.error().toStdString());
        return !controller.busy() && controller.outputReady() && controller.audioState().sourceFrames[deck];
    }, "Live controller load did not complete", true, std::chrono::seconds(60));
    const auto map = controller.decks()[deck].toMap();
    require(map["stemBank"].toBool() && map["overviewStem"].toString() == "source" &&
            !map["waveform"].toList().isEmpty() && !controller.preparation()["cacheHit"].toBool(),
            "A genuine stem cache miss installs a live bank with its source waveform");
}

template<class Check>
void runLiveStemController(const std::filesystem::path& output, const QString& parent,
                           const QString& normal, bool functionalOnly, Check&& require, QJsonObject& observations)
{
    QTemporaryDir directory(QDir(parent).filePath("live-controller-XXXXXX"));
    require(directory.isValid(), "Live controller fixtures have a separate private owner");
    const auto fixture = conflux::test::makeStemServiceFixture(directory.path());
    ServiceTrace trace(fixture); // Outlives the controller for failed-test safety.
    conflux::DeckController::PreparationOptions options;
    options.stemEngine = fixture.engine;
    options.stemServicePath = fixture.service;
    conflux::DeckController controller(options);
    Sink sink("conflux-live-controller-master-" + std::to_string(::getpid()), 1e-6F);
    const auto sinkMatches = [&] { return functionalOnly || (!sink.wrong() && !sink.zeros()); };
    QString left, right;
    privateLivePair(controller, sink, left, right, require);
    const auto source = directory.filePath("live-bass-source.wav");
    constexpr std::uint32_t liveFrames = 30 * 48000;
    writeFixture(source, 8192, -4096, 30 * 44100, 44100);
    controller.loadStemDeck(0, QUrl::fromLocalFile(source));
    waitLiveLoad(controller, 0, require);
    trace.capture();
    const auto loaded = controller.audioState();
    auto liveObservations = controller.liveStemObservations(0);
    require(bool(liveObservations) && !controller.liveStemObservations(1), "Only the installed live deck exposes its observation handle");
    require(trace.size() == 1 && trace.alive(0) && trace.count(0, "command", "load") == 1 &&
            trace.count(0, "command", "seek") == 0 && trace.count(0, "fixture", "runtime") == 1 &&
            loaded.sourceFrames[0] >= liveFrames - 1 && loaded.sourceFrames[0] <= liveFrames + 1 &&
            controller.decks()[0].toMap()["name"].toString() == "live-bass-source.wav" &&
            controller.retainedPcmBytes() == 0 && !QFile::exists(fixture.engine.exporterPath.chopped(3) + ".calls"),
            "Initial live install uses one service load/runtime, no constructor seek or offline PCM export");
    constexpr std::array<float, 4> bass{0.F, 1.F, 0.F, 0.F};
    for (int role = 0; role < 4; ++role) controller.setStemGain(0, role, role == 1 ? 1.0 : 0.0);
    wait([&] { return controller.audioState().stemGain[0] == bass; }, "Live bass targets were not acknowledged");
    controller.setPlaying(0, true);
    wait([&] { return controller.audioState().sourceFrame[0] >= 4096; }, "Live bass did not render a nonempty settled prefix");
    controller.setPlaying(0, false);
    wait([&] { return !controller.audioState().playing[0]; }, "Live bass pause was not acknowledged");
    require(controller.cuePress(0), "Paused live deck accepts storing the advanced cue");
    wait([&] { return controller.audioState().cueHeld[0] && controller.audioState().cueFrame[0] >= 4096; },
         "Live cue was not stored at the advanced cursor");
    controller.cueRelease(0);
    controller.setCueSelected(0, true);
    controller.setPlaying(0, true);
    const auto cue = controller.audioState().cueFrame[0];
    wait([&] { return controller.audioState().playing[0] && !controller.audioState().cueHeld[0] &&
                      controller.audioState().sourceFrame[0] >= cue + 1024; }, "Live bass restart did not settle");
    sink.validate(true);
    wait([&] { return sink.received(0) >= 1024 && sink.received(1) >= 1024; }, "Exact bass samples did not reach the private sink");
    require(sinkMatches(), "Bass-only output matches the independent half-amplitude oracle, not the original mix");
    const auto recordingPath = QString::fromStdString((output / "generated-live-controller.wav").string());
    controller.startRecording(QUrl::fromLocalFile(recordingPath));
    wait([&] { return controller.recording()["status"].toString() == "Recording" &&
                      controller.recording()["acceptedFrames"].toULongLong() >= 1024; }, "Live recording did not attach");
    controller.loadDeck(1, QUrl::fromLocalFile(normal));
    wait([&] { return !controller.busy() && controller.audioState().sourceFrames[1]; }, "Normal muted live peer did not load");
    controller.setDeckGain(1, 0.0);
    wait([&] { return controller.audioState().deckGain[1] == 0.F; }, "Normal peer mute was not acknowledged");
    const auto mutedAt = controller.audioState().outputFrame;
    wait([&] { return controller.audioState().outputFrame >= mutedAt + 1024; }, "Normal peer mute did not settle");
    controller.setPlaying(1, true);
    wait([&] { return controller.audioState().playing[1] && controller.audioState().sourceFrame[1] >= 1024; },
         "Muted normal peer did not advance");
    const auto shortSource = directory.filePath("cancel-live-target.wav");
    writeFixture(shortSource, 2048, 3072, 211);
    const auto shortFrames = [&] {
        QFile source(shortSource);
        if (!source.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read tiny live duration oracle");
        const std::filesystem::path spool(QFile::encodeName(directory.path()).constData());
        auto input = conflux::audio::prepareStemInput(source.handle(), 65536, {}, spool);
        auto output = conflux::audio::prepareStemOutput(input.descriptor(), input.frames(), 65536, {}, spool);
        return output.frames();
    }(); // Independent complete-file conversion, just 211 original frames.
    const auto before = controller.audioState();
    const auto beforeMaps = controller.decks();
    const auto sessions = controller.completedSessions(), retained = controller.retainedPcmBytes();
    const auto recordedBefore = controller.recording()["acceptedFrames"].toULongLong();
    controller.loadStemDeck(1, QUrl::fromLocalFile(shortSource));
    require(QThreadPool::globalInstance()->waitForDone(60000), "Live candidate worker finishes before queued GUI acceptance");
    trace.capture();
    require(trace.size() == 2 && trace.alive(1) && controller.cancelPreparation(),
            "The complete queued live candidate is still owned and cancellable");
    const auto recordedAtCompletion = controller.recording()["acceptedFrames"].toULongLong();
    {
        RetirementGate gate;
        bool heartbeat = false;
        QMetaObject::invokeMethod(&controller, [&] { heartbeat = true; }, Qt::QueuedConnection);
        wait([&] { return heartbeat && controller.preparation()["status"].toString() == "Cancelling" &&
                          controller.audioState().outputFrame > before.outputFrame; }, "GUI did not progress with retirement held");
        require(controller.busy() && trace.alive(1) && trace.charged(1) &&
                controller.audioState().sourceGeneration == before.sourceGeneration &&
                controller.recording()["active"].toBool(),
                "Queued live retirement remains charged and owned while the GUI and existing writer advance");
        gate.release();
    }
    wait([&] { return !controller.busy() && trace.gone(1) && controller.audioState().outputFrame >=
                         before.outputFrame + recordedAtCompletion - recordedBefore + 1024; },
         "Cancelled live candidate did not retire on its worker");
    const auto cancelled = controller.audioState();
    require(controller.preparation()["status"].toString() == "Cancelled" && controller.completedSessions() == sessions &&
            cancelled.sourceGeneration == before.sourceGeneration && cancelled.stemGain[0] == bass &&
            cancelled.cueFrame[0] == cue && cancelled.cueSelected[0] && cancelled.playing[0] && cancelled.playing[1] &&
            cancelled.deckGain[1] == 0.F && !cancelled.buffering[0] && !cancelled.buffering[1] &&
            !cancelled.sourceReadError[0] && !cancelled.sourceReadError[1] &&
            !cancelled.sourceReadErrorObserved[0] && !cancelled.sourceReadErrorObserved[1] &&
            !cancelled.starvedFrames[0] && !cancelled.starvedFrames[1] &&
            controller.decks()[0].toMap()["name"] == beforeMaps[0].toMap()["name"] &&
            controller.decks()[1].toMap()["name"] == beforeMaps[1].toMap()["name"] &&
            controller.decks()[0].toMap()["waveform"] == beforeMaps[0].toMap()["waveform"] &&
            controller.decks()[1].toMap()["waveform"] == beforeMaps[1].toMap()["waveform"] &&
            controller.recording()["path"].toString() == recordingPath &&
            cancelled.sourceFrame[0] < cancelled.sourceFrames[0] && cancelled.sourceFrame[1] < cancelled.sourceFrames[1] &&
            cancelled.sourceFrame[0] - before.sourceFrame[0] == cancelled.outputFrame - before.outputFrame &&
            cancelled.sourceFrame[1] - before.sourceFrame[1] == cancelled.outputFrame - before.outputFrame &&
            controller.retainedPcmBytes() == retained && sinkMatches(),
            "Queued live cancellation preserves both advancing clocks, selected bass, cue, peer and writer");
    controller.loadStemDeck(1, QUrl::fromLocalFile(shortSource));
    waitLiveLoad(controller, 1, require);
    trace.capture();
    require(trace.size() == 3 && trace.alive(2) && !controller.audioState().playing[1] &&
            controller.audioState().sourceFrame[1] == 0 && controller.audioState().sourceFrames[1] == shortFrames &&
            controller.decks()[1].toMap()["name"].toString() == QFileInfo(shortSource).fileName() &&
            controller.decks()[1].toMap()["durationSeconds"].toDouble() == shortFrames / 48000.0 &&
            controller.completedSessions() == sessions &&
            controller.retainedPcmBytes() == 0 && trace.alive(0),
            "Paused live replacement retires the normal peer without restarting its audible live owner");
    controller.loadDeck(1, QUrl::fromLocalFile(normal));
    wait([&] { return !controller.busy() && !controller.audioState().stemBank[1] && trace.gone(2); },
         "Live-to-normal target replacement did not finish whole-owner cleanup");
    require(controller.audioState().sourceFrames[1] == fixtureFrames &&
            controller.decks()[1].toMap()["name"].toString() == QFileInfo(normal).fileName() &&
            controller.completedSessions() == sessions && trace.alive(0) && controller.recording()["active"].toBool() &&
            controller.recording()["path"].toString() == recordingPath && sinkMatches(),
            "Live-to-normal replacement preserves the audible peer service, shared session and original recorder");
    const auto beforeRoute = controller.audioState();
    const auto prefixFrames = controller.recording()["acceptedFrames"].toULongLong();
    const auto seeks = trace.count(0, "command", "seek");
    sink.validate(false); // Explicit rebuild/start ramps and omitted wall time are excluded.
    controller.configureAudioRouting(left, right, {}, {});
    wait([&] {
        if (!controller.busy() && !controller.error().isEmpty()) throw std::runtime_error(controller.error().toStdString());
        return !controller.busy() && controller.outputReady() && controller.audioState().sourceFrame[0] >= beforeRoute.sourceFrame[0] + 4096;
    }, "Consumed live owner did not rebuild on the same service", true, std::chrono::seconds(60));
    trace.capture();
    const auto routed = controller.audioState();
    require(controller.liveStemObservations(0) == liveObservations && !controller.liveStemObservations(1),
            "Consumed route rebuild retains its live observation handle and a null finite peer handle");
    require(trace.size() == 3 && trace.alive(0) && trace.count(0, "command", "load") == 1 &&
            trace.count(0, "fixture", "runtime") == 1 && trace.count(0, "command", "seek") == seeks + 1 &&
            routed.stemGain[0] == bass && routed.playing[0] && routed.cueFrame[0] == cue && routed.cueSelected[0] &&
            !routed.playing[1] && !routed.stemBank[1] && routed.sourceFrame[0] < routed.sourceFrames[0] &&
            controller.completedSessions() == sessions + 1 && controller.recording()["active"].toBool() &&
            controller.recording()["path"].toString() == recordingPath,
            "Consumed routing rebuild retains one service/runtime with one necessary seek and captured bass/cue/transport targets");
    const auto received = sink.received(0), wrong = sink.wrong(), zeros = sink.zeros();
    sink.validate(true);
    wait([&] { return sink.received(0) >= received + 1024; }, "Rebuilt live bass samples did not arrive");
    require(functionalOnly || (sink.wrong() == wrong && sink.zeros() == zeros),
            "Rebuilt bass reaches the virtual sink with the existing normal-versus-functional sample gate");
    controller.stopRecording();
    wait([&] { return !controller.recording()["active"].toBool(); }, "Live recording did not finalize");
    const auto recording = controller.recording();
    require(recording["status"].toString() == "Complete" && recording["acceptedFrames"] == recording["writtenFrames"] &&
            recording["overflowFrames"].toULongLong() == 0 && !routed.invalidBuffers && !routed.rejected &&
            !routed.sourceReadErrorObserved[0] && !routed.sourceReadError[0] && !routed.starvedFrames[0],
            "Live controller finalizes every accepted master frame with healthy live source and no overflow");
    const auto wav = inspectStemRecording(recordingPath, prefixFrames, recording["writtenFrames"].toULongLong());
    sink.validate(false);
    controller.shutdown();
    require(trace.gone(0) && trace.gone(1) && trace.gone(2) && !controller.busy() && !controller.outputReady(),
            "Shutdown reaps all three identity-bound service children and removes their charged jobs");
    require(liveObservations->counters().producerFinished, "Retained diagnostics do not keep the shut down producer/job alive");
    observations["live_controller"] = QJsonObject{{"recording", wav}, {"children", trace.identities()},
        {"virtual_sink_sample_gate", !functionalOnly}, {"virtual_sink_wrong_samples", qint64(sink.wrong())},
        {"virtual_sink_zero_samples", qint64(sink.zeros())}, {"source_frames", qint64(loaded.sourceFrames[0])},
        {"scope", "Generated 30-second input and stdlib service, selected bass samples, private PipeWire controller/handoff/rebuild/recording. Rebuild gaps/ramps excluded; no model, physical, latency or sustained qualification."}};
}

template<class Check>
void runLostLiveOwners(const QString& parent, Check&& require, QJsonObject& observations)
{
    QTemporaryDir directory(QDir(parent).filePath("lost-live-controller-XXXXXX"));
    require(directory.isValid(), "Constructor-loss fixtures have an independent private owner");
    auto fixture = conflux::test::makeStemServiceFixture(directory.path(), "stale-after-seek");
    ServiceTrace trace(fixture);
    conflux::DeckController::PreparationOptions options;
    options.stemEngine = fixture.engine; options.stemServicePath = fixture.service;
    conflux::DeckController controller(options);
    Sink sink("conflux-lost-live-master-" + std::to_string(::getpid()));
    QString left, right;
    privateLivePair(controller, sink, left, right, require);
    const auto a = directory.filePath("lost-live-a.wav"), b = directory.filePath("lost-live-b.wav");
    writeFixture(a, 4096, -2048, 30 * 44100, 44100); writeFixture(b, 2048, 3072, 30 * 44100, 44100);
    for (int deck = 0; deck < 2; ++deck) {
        controller.loadStemDeck(deck, QUrl::fromLocalFile(deck ? b : a));
        waitLiveLoad(controller, deck, require); trace.capture();
        controller.setDeckGain(deck, 0.0);
        wait([&] { return controller.audioState().deckGain[deck] == 0.F; }, "Lost-owner fixture mute was not acknowledged");
        const auto mutedAt = controller.audioState().outputFrame;
        wait([&] { return controller.audioState().outputFrame >= mutedAt + 1024; }, "Lost-owner fixture mute did not settle");
        controller.setPlaying(deck, true);
        wait([&] { return controller.audioState().sourceFrame[deck] >= 1024; }, "Lost-owner fixture did not consume PCM");
        controller.setPlaying(deck, false);
        wait([&] { return !controller.audioState().playing[deck]; }, "Lost-owner fixture pause was not acknowledged");
    }
    require(trace.size() == 2 && trace.alive(0) && trace.alive(1) &&
            trace.count(0, "command", "load") == 1 && trace.count(1, "command", "load") == 1,
            "Two independent live services each render before the constructor-failure route rebuild");
    controller.configureAudioRouting(left, right, {}, {});
    wait([&] { return !controller.busy() && !controller.error().isEmpty() && trace.gone(0) && trace.gone(1); },
         "Stale re-prime did not fail and retire both consumed owners", true, std::chrono::seconds(60));
    const auto lost = controller.decks();
    require(!controller.outputReady() && !lost[0].toMap()["loaded"].toBool() && !lost[1].toMap()["loaded"].toBool() &&
            lost[0].toMap()["name"].toString().isEmpty() && lost[1].toMap()["name"].toString().isEmpty() &&
            lost[0].toMap()["waveform"].toList().isEmpty() && lost[1].toMap()["waveform"].toList().isEmpty() &&
            controller.audioState().sourceFrames[0] == 0 && controller.audioState().sourceFrames[1] == 0 &&
            controller.retainedPcmBytes() == 0,
            "Two-owner constructor loss clears both consumed live metadata rows and retains its explicit failure");
    // Both old services are proved gone before changing any pinned fixture bytes.
    fixture = conflux::test::makeStemServiceFixture(directory.path());
    for (int deck = 0; deck < 2; ++deck) {
        controller.loadStemDeck(deck, QUrl::fromLocalFile(deck ? b : a));
        waitLiveLoad(controller, deck, require); trace.capture();
        const auto state = controller.audioState();
        require(!state.playing[deck] && state.sourceFrame[deck] == 0 && state.stemBank[deck] &&
                controller.decks()[deck].toMap()["name"].toString() == (deck ? "lost-live-b.wav" : "lost-live-a.wav") &&
                state.sourceFrames[deck] >= 30 * 48000 - 1 && state.sourceFrames[deck] <= 30 * 48000 + 1 &&
                (deck || (!state.sourceFrames[1] && !controller.decks()[1].toMap()["loaded"].toBool())),
                "A consumed-owner failure permits sequential explicit reload without recreating its still-empty peer");
    }
    require(trace.size() == 4 && trace.alive(2) && trace.alive(3), "Sequential reload owns exactly two new service identities");
    controller.shutdown();
    require(trace.gone(2) && trace.gone(3) && !controller.busy() && !controller.outputReady(),
            "Reloaded whole owners shut down and remove both fresh jobs");
    observations["lost_live_owners"] = QJsonObject{{"children", trace.identities()},
        {"scope", "Actual private callback consumption, deterministic stale-generation constructor failure, metadata clearing and sequential reload; no model, physical output or delivery qualification."}};
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

// Separate CLI mode keeps these measured scenarios out of the existing90s run.
namespace liveFailure {
using State = conflux::audio::LiveSession::State;
constexpr std::uint64_t MiB = 1024 * 1024;
constexpr std::uint64_t totalBytesLimit = 512 * MiB;
constexpr std::uint64_t fileBytesLimit = 64 * MiB;
constexpr std::uint64_t holdFrames = 8192;
constexpr std::array<float, 4> bassOnly{0.F, 1.F, 0.F, 0.F};
constexpr std::array<float, 2> aBass{.125F, -.0625F}, bBass{.0625F, -.03125F};
constexpr std::array<float, 2> bothBass{.1875F, -.09375F};
volatile std::sig_atomic_t stopOwner = 0;
void requestStop(int) { stopOwner = 1; }
Clock::time_point ownerDeadline;
QJsonArray assertions;
void insist(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
    if (assertions.size() == 256) throw std::runtime_error("Assertion evidence bound exceeded");
    assertions.append(message);
}
void current()
{
    if (stopOwner || Clock::now() >= ownerDeadline)
        throw std::runtime_error("Owned fixture interrupted or original75s owner deadline expired");
}
QString digest(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || std::uint64_t(file.size()) > fileBytesLimit)
        throw std::runtime_error("Cannot hash bounded generated file");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) throw std::runtime_error("Generated file hash failed");
    return QString::fromLatin1(hash.result().toHex());
}
void directory(const QString& path)
{
    if (QFileInfo::exists(path) || !QDir().mkdir(path) || ::chmod(QFile::encodeName(path).constData(), 0700))
        throw std::runtime_error("Fresh owned private directory required");
}
std::uint64_t treeBytes(const QString& path)
{
    std::uint64_t bytes = 0, files = 0, directories = 0;
    const std::filesystem::path root(QFile::encodeName(path).constData());
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        struct stat st{};
        if (::lstat(item.path().c_str(), &st)) {
            if (errno == ENOENT) continue; // A worker may atomically retire its own stage.
            throw std::runtime_error("Cannot inspect private resource bounds");
        }
        if (st.st_uid != ::geteuid() || S_ISLNK(st.st_mode))
            throw std::runtime_error("Unexpected owner or symlink in private fixtures/cache");
        if (S_ISDIR(st.st_mode)) {
            if (++directories > 128) throw std::runtime_error("Private directory count exceeded");
        } else if (S_ISREG(st.st_mode)) {
            if (++files > 256 || st.st_size < 0 || std::uint64_t(st.st_size) > fileBytesLimit ||
                st.st_nlink != 1 || std::uint64_t(st.st_size) > totalBytesLimit - bytes)
                throw std::runtime_error("Private file/count/byte bound exceeded");
            bytes += std::uint64_t(st.st_size);
        } else throw std::runtime_error("Unexpected special file in private fixtures/cache");
    }
    return bytes;
}
QJsonObject snapshot(const State& s)
{
    return {{"output_frame", qint64(s.outputFrame)}, {"a_cursor", qint64(s.sourceFrame[0])},
        {"b_cursor", qint64(s.sourceFrame[1])}, {"a_frames", qint64(s.sourceFrames[0])},
        {"b_frames", qint64(s.sourceFrames[1])}, {"a_generation", qint64(s.sourceGeneration[0])},
        {"b_generation", qint64(s.sourceGeneration[1])}, {"a_buffering", s.buffering[0]},
        {"b_buffering", s.buffering[1]}, {"a_error", s.sourceReadError[0]},
        {"b_error", s.sourceReadError[1]}, {"a_render_error", s.sourceReadErrorObserved[0]},
        {"b_render_error", s.sourceReadErrorObserved[1]}, {"a_starved", qint64(s.starvedFrames[0])},
        {"b_starved", qint64(s.starvedFrames[1])}, {"a_playing", s.playing[0]},
        {"b_playing", s.playing[1]}, {"callbacks", qint64(s.callbacks)},
        {"invalid_buffers", qint64(s.invalidBuffers)}, {"rejected", qint64(s.rejected)},
        {"snapshot_drops", qint64(s.snapshotDrops)}, {"ready", s.ready}};
}
struct Evidence {
    QString name, output, cache;
    conflux::DeckController* controller = nullptr;
    std::shared_ptr<conflux::library::LiveStemObservations> live;
    QJsonArray snapshots, records;
    QJsonObject actions;
    std::uint64_t ordinal = 0, lastOutput = UINT64_MAX, maximumBytes = 0;
    bool saved = false;
    Evidence(QString nameValue, QString outputValue, QString cacheValue)
        : name(std::move(nameValue)), output(std::move(outputValue)), cache(std::move(cacheValue)) {}
    void drain()
    {
        if (live) {
            conflux::library::LiveStemObservations::Record r;
            while (live->tryPop(r)) {
                if (r.ordinal != ++ordinal || records.size() >= 2048)
                    throw std::runtime_error("Live observation ordinal or evidence bound failed");
                records.append(QJsonObject{{"ordinal", qint64(r.ordinal)}, {"kind", int(r.kind)},
                    {"observed_ns", qint64(r.observedNs)}, {"epoch", qint64(r.providerEpoch)},
                    {"generation", qint64(r.workerGeneration)}, {"command_id", qint64(r.commandId)},
                    {"request_id", qint64(r.requestId)}, {"lease_sequence", qint64(r.leaseSequence)},
                    {"input_first", qint64(r.inputFirst44100)}, {"input_frames", qint64(r.inputFrames44100)},
                    {"output_first", qint64(r.outputFirst48000)}, {"output_frames", qint64(r.outputFrames48000)}});
            }
        }
        if (controller) {
            const auto s = controller->audioState();
            if (s.outputFrame != lastOutput) {
                if (snapshots.size() >= 2048) throw std::runtime_error("Snapshot evidence bound exceeded");
                auto row = snapshot(s);
                row["recording"] = QJsonObject::fromVariantMap(controller->recording());
                snapshots.append(row); lastOutput = s.outputFrame;
                // Staging/retirement directories may be atomically removed while busy.
                // Hard store admission and per-file limits still apply during those workers.
                if (!controller->busy()) {
                    maximumBytes = std::max(maximumBytes, treeBytes(output) + treeBytes(cache));
                    if (maximumBytes > totalBytesLimit) throw std::runtime_error("Combined output/cache byte ceiling exceeded");
                }
            }
        }
    }
    void write(bool passed, const QString& error = {})
    {
        if (saved) return;
        drain();
        QJsonObject counters;
        if (live) {
            const auto c = live->counters();
            counters = {{"attempted", qint64(c.attemptedRecords)}, {"dropped", qint64(c.droppedRecords)},
                {"invalid_clock_reads", qint64(c.invalidClockReads)}, {"producer_finished", c.producerFinished},
                {"drained", qint64(ordinal)}, {"source_token", qint64(live->setup().sourceToken)}};
        }
        QJsonObject value{{"schema", 1}, {"scenario", name}, {"passed", passed}, {"qualified", false},
            {"error", error}, {"actions", actions}, {"snapshots", snapshots}, {"live_records", records},
            {"live_counters", counters}, {"maximum_observed_output_plus_cache_bytes", qint64(maximumBytes)}};
        if (controller) value["recording_final"] = QJsonObject::fromVariantMap(controller->recording());
        const auto bytes = QJsonDocument(value).toJson();
        if (bytes.size() > 8 * qint64(MiB) || !save(QDir(output).filePath(name + "-evidence.json"), value))
            throw std::runtime_error("Cannot retain bounded private scenario evidence");
        saved = true;
    }
};
template<class Predicate>
void await(Evidence& e, Predicate&& predicate, const char* message, std::chrono::seconds limit = std::chrono::seconds(8))
{
    const auto end = std::min(ownerDeadline, Clock::now() + limit);
    for (;;) {
        current(); e.drain();
        if (predicate()) return;
        if (Clock::now() >= end) throw std::runtime_error(message);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
}
conflux::library::PreparedTrackStore::Limits tightLimits()
{
    conflux::library::PreparedTrackStore::Limits value;
    value.maxPreparedBytes = 16 * MiB; // Each30s stereo F32 leaf is11.52MB.
    value.maxSourceBytes = 8 * MiB; // Each generated PCM16 source is<5.77MB.
    value.maxCacheBytes = 256 * MiB;
    value.maxEntries = 8;
    return value;
}
void seedBank(const QString& source, const QString& cache, const conflux::library::StemEngine& engine, Evidence& e)
{
    std::stop_source cancellation;
    auto future = QtConcurrent::run([source, cache, engine, stop = cancellation.get_token()] {
        try {
            auto bank = conflux::library::PreparedTrackStore(cache, tightLimits()).prepareStemBank(source, engine, stop);
            if (bank.cacheHit || bank.stems.front().frames() < 12 * 48000 - 1 ||
                bank.stems.front().frames() > 12 * 48000 + 1)
                throw std::runtime_error("Generated12s prepared bank has unexpected geometry/cache identity");
            return QString{};
        } catch (const std::exception& error) {
            return QString::fromUtf8(error.what()); // Preserve the worker error before Qt wraps exceptions.
        } catch (...) { return QStringLiteral("Unknown prepared fixture worker failure"); }
    });
    try {
        await(e, [&] { return future.isFinished(); }, "Prepared fixture bank did not complete", std::chrono::seconds(20));
    } catch (...) {
        const auto primary = std::current_exception();
        cancellation.request_stop();
        // Finish cancellation without replacing the original await/interruption failure.
        while (!future.isFinished()) QThread::msleep(2);
        try {
            future.waitForFinished();
            e.actions["seed_worker_error_during_cleanup"] = future.result();
        } catch (const std::exception& secondary) {
            e.actions["seed_secondary_cleanup_exception"] = secondary.what();
        } catch (...) { e.actions["seed_secondary_cleanup_exception"] = "Unknown Qt cleanup exception"; }
        std::rethrow_exception(primary);
    }
    future.waitForFinished();
    const auto error = future.result();
    if (!error.isEmpty()) throw std::runtime_error("Prepared fixture worker failed: " + error.toStdString());
}
class BoundService {
public:
    explicit BoundService(const QJsonObject& identity)
    {
        pid_ = identity["pid"].toInt(); ticks_ = identity["start_ticks"].toString().toLatin1();
        fd_ = int(::syscall(SYS_pidfd_open, pid_, 0));
        QFile stat(QString("/proc/%1/stat").arg(pid_));
        if (pid_ <= 1 || ticks_.isEmpty() || fd_ < 0 || !stat.open(QIODevice::ReadOnly)) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Cannot bind exact live fixture process before induced exit");
        }
        const auto bytes = stat.readAll();
        const auto fields = bytes.mid(bytes.lastIndexOf(") ") + 2).split(' ');
        if (fields.size() <= 19 || fields[19] != ticks_ || ::getpgid(pid_) != pid_) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("Fixture leader identity/group changed before induced exit");
        }
    }
    ~BoundService() { if (fd_ >= 0) ::close(fd_); }
    void terminateUnexpectedly()
    {
        if (::syscall(SYS_pidfd_send_signal, fd_, SIGKILL, nullptr, 0))
            throw std::runtime_error("Identity-bound induced fixture exit failed");
    }
    bool groupGone() const { errno = 0; return ::kill(-pid_, 0) == -1 && errno == ESRCH; }
private:
    int fd_ = -1;
    pid_t pid_ = 0;
    QByteArray ticks_;
};
void continuousPeer(const State& before, const State& after)
{
    if (after.outputFrame <= before.outputFrame || after.sourceFrame[1] < before.sourceFrame[1] ||
        after.sourceFrame[1] - before.sourceFrame[1] != after.outputFrame - before.outputFrame ||
        after.sourceGeneration[1] != before.sourceGeneration[1] || !after.playing[1] ||
        after.buffering[1] || after.sourceReadError[1] || after.sourceReadErrorObserved[1] ||
        after.starvedFrames[1] != 0 || after.invalidBuffers || after.rejected || after.snapshotDrops)
        throw std::runtime_error("Prepared bass peer/output atomic clocks or health changed");
}
std::array<float, 2> ramp(bool rising, std::uint32_t step)
{
    std::array<float, 2> value{};
    const double fraction = double(step) / 240;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto a = static_cast<float>((rising ? fraction : 1 - fraction) * aBass[channel]);
        value[channel] = static_cast<float>(double(a) + bBass[channel]);
    }
    return value;
}
QJsonObject inspectCompleteRecording(const QString& path, std::uint64_t expectedFrames, bool recovery)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 44 || std::uint64_t(file.size()) > fileBytesLimit)
        throw std::runtime_error("Cannot read complete bounded generated recording");
    const auto head = file.read(12);
    if (head.size() != 12 || head.first(4) != "RIFF" || head.sliced(8, 4) != "WAVE" ||
        qFromLittleEndian<quint32>(head.constData() + 4) != std::uint64_t(file.size() - 8))
        throw std::runtime_error("Final RIFF geometry is inconsistent");
    qint64 offset = -1; std::uint64_t pcmBytes = 0, factFrames = UINT64_MAX;
    bool format = false;
    for (qint64 pos = 12; pos + 8 <= file.size();) {
        if (!file.seek(pos)) throw std::runtime_error("Cannot seek RIFF metadata");
        const auto chunk = file.read(8);
        if (chunk.size() != 8) throw std::runtime_error("Incomplete RIFF chunk header");
        const auto count = qFromLittleEndian<quint32>(chunk.constData() + 4);
        if (std::uint64_t(count) > std::uint64_t(file.size() - pos - 8))
            throw std::runtime_error("RIFF chunk exceeds final file");
        if (chunk.first(4) == "fmt ") {
            std::array<char, 64> f{};
            const auto read = file.read(f.data(), std::min<quint32>(count, f.size()));
            format = read >= 16 && qFromLittleEndian<quint16>(f.data()) == 3 &&
                qFromLittleEndian<quint16>(f.data() + 2) == 2 &&
                qFromLittleEndian<quint32>(f.data() + 4) == 48000 &&
                qFromLittleEndian<quint32>(f.data() + 8) == 384000 &&
                qFromLittleEndian<quint16>(f.data() + 12) == 8 &&
                qFromLittleEndian<quint16>(f.data() + 14) == 32;
        } else if (chunk.first(4) == "fact") {
            const auto f = file.read(4);
            if (count < 4 || f.size() != 4 || factFrames != UINT64_MAX) throw std::runtime_error("Invalid fact chunk");
            factFrames = qFromLittleEndian<quint32>(f.constData());
        } else if (chunk.first(4) == "data") {
            if (offset >= 0) throw std::runtime_error("Duplicate PCM chunk");
            offset = pos + 8; pcmBytes = count;
        }
        pos += 8 + qint64(count) + (count & 1U);
    }
    if (!format || offset < 0 || pcmBytes % 8 || pcmBytes / 8 != expectedFrames ||
        factFrames != expectedFrames || expectedFrames < 2 * holdFrames || !file.seek(offset))
        throw std::runtime_error("WAV format/fact/accepted/written frame mismatch");
    enum Phase { Both, FadeOut, Peer, FadeIn, Recovered } phase = Both;
    std::uint32_t step = 0;
    std::array<std::uint64_t, 5> counts{};
    std::array<char, 4096 * 8> block{};
    float maxError = 0;
    const auto error = [&](const std::array<float, 2>& got, const std::array<float, 2>& wanted) {
        return std::max(std::abs(got[0] - wanted[0]), std::abs(got[1] - wanted[1]));
    };
    for (std::uint64_t first = 0; first < expectedFrames;) {
        const auto count = std::min<std::uint64_t>(4096, expectedFrames - first);
        if (file.read(block.data(), qint64(count * 8)) != qint64(count * 8))
            throw std::runtime_error("Complete generated PCM truncated");
        for (std::uint64_t i = 0; i < count; ++i) {
            std::array<float, 2> got{};
            for (std::size_t c = 0; c < 2; ++c) {
                got[c] = std::bit_cast<float>(std::uint32_t(qFromLittleEndian<quint32>(block.data() + i * 8 + c * 4)));
                if (!std::isfinite(got[c])) throw std::runtime_error("Nonfinite generated master sample");
            }
            std::array<float, 2> wanted{};
            if (phase == Both) {
                if (error(got, bothBass) <= 1e-6F) wanted = bothBass;
                else { phase = FadeOut; step = 1; wanted = ramp(false, step); }
            } else if (phase == FadeOut) wanted = ramp(false, ++step);
            else if (phase == Peer) {
                if (error(got, bBass) <= 1e-6F) wanted = bBass;
                else { phase = FadeIn; step = 1; wanted = ramp(true, step); }
            } else if (phase == FadeIn) wanted = ramp(true, ++step);
            else wanted = bothBass;
            const auto deviation = error(got, wanted);
            if (deviation > 1e-6F) throw std::runtime_error("Every-frame bass/peer/240-frame ramp oracle failed");
            maxError = std::max(maxError, deviation); ++counts[phase];
            if (phase == FadeOut && step == 240) phase = Peer;
            if (phase == FadeIn && step == 240) phase = Recovered;
        }
        first += count;
    }
    if (counts[Both] < 1024 || counts[FadeOut] != 240 || counts[Peer] < holdFrames ||
        (recovery ? (phase != Recovered || counts[FadeIn] != 240 || counts[Recovered] < 1024) :
                    (phase != Peer || counts[FadeIn] || counts[Recovered])))
        throw std::runtime_error("Required recorded baseline/held/recovered phases incomplete");
    return {{"frames", qint64(expectedFrames)}, {"bytes", file.size()}, {"fact_frames", qint64(factFrames)},
        {"sha256", digest(path)}, {"all_samples_finite", true}, {"every_frame_checked", true},
        {"baseline_frames", qint64(counts[Both])}, {"fade_out_frames", qint64(counts[FadeOut])},
        {"peer_only_frames", qint64(counts[Peer])}, {"fade_in_frames", qint64(counts[FadeIn])},
        {"recovered_frames", qint64(counts[Recovered])}, {"maximum_oracle_error", maxError},
        {"amplitude_tolerance", 1e-6}, {"unclassified_or_omitted_frames", 0}};
}
void scenario(const QString& output, const QString& privateRoot, bool exitScenario, QJsonArray& results)
{
    const auto name = exitScenario ? QStringLiteral("active-exit") : QStringLiteral("active-stall");
    const auto local = QDir(output).filePath(name), cacheHome = QDir(privateRoot).filePath(name);
    directory(local); directory(cacheHome);
    Evidence evidence{name, output, privateRoot};
    QJsonObject report{{"scenario", name}, {"qualified", false}};
    const auto a = QDir(local).filePath("live-a.wav"), b = QDir(local).filePath("prepared-b.wav");
    // Paused replacement reuses the distinct suitable B bank; no third bank or implicit mix fallback.
    writeFixture(a, 8192, -4096, 30 * 44100, 44100);
    writeFixture(b, 4096, -2048, 12 * 48000, 48000);
    const auto fixture = conflux::test::makeStemServiceFixture(local, exitScenario ? "good" : "gate-after-first");
    ServiceTrace services(fixture);
    insist(qputenv("XDG_CACHE_HOME", QFile::encodeName(cacheHome)) &&
           qputenv("XDG_DATA_HOME", QFile::encodeName(QDir(cacheHome).filePath("data"))),
           "Scenario cache and library remain under the isolated private owner");
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
    try { seedBank(b, cache, fixture.engine, evidence); }
    catch (const std::exception& seedError) {
        // Seeding precedes controller construction; retain its precise failure too.
        try { evidence.write(false, QString::fromUtf8(seedError.what())); } catch (...) {}
        throw;
    }
    conflux::DeckController::PreparationOptions options;
    options.stemEngine = fixture.engine; options.stemServicePath = fixture.service; options.limits = tightLimits();
    conflux::DeckController controller(options);
    evidence.controller = &controller;
    Sink sink("conflux-stall-continuity-" + name.toStdString() + "-" + std::to_string(::getpid()));
    try {
        QString left, right;
        privateLivePair(controller, sink, left, right, insist);
        controller.loadStemDeck(1, QUrl::fromLocalFile(b));
        await(evidence, [&] { return !controller.busy() && controller.outputReady() && controller.audioState().sourceFrames[1]; },
            "Prepared bass peer did not install", std::chrono::seconds(15));
        insist(controller.error().isEmpty() && controller.preparation()["cacheHit"].toBool() && !controller.liveStemObservations(1),
            "Peer is an actual compatible complete prepared stem bank, not a full-track or live substitute");
        controller.loadStemDeck(0, QUrl::fromLocalFile(a));
        await(evidence, [&] {
            if (!controller.busy() && !controller.error().isEmpty()) throw std::runtime_error(controller.error().toStdString());
            return !controller.busy() && controller.outputReady() && controller.audioState().sourceFrames[0];
        }, "Genuine live A did not install", std::chrono::seconds(20));
        evidence.live = controller.liveStemObservations(0); services.capture();
        insist(evidence.live && !controller.preparation()["cacheHit"].toBool() && services.size() == 1 &&
            services.alive(0) && services.count(0, "command", "load") == 1 && services.count(0, "command", "seek") == 0 &&
            services.count(0, "fixture", "runtime") == 1,
            "One genuine cache-miss live owner installs with one runtime/load and no constructor seek");
        BoundService bound(services.identities()[0].toObject());
        const auto handle = evidence.live; const auto token = handle->setup().sourceToken;
        for (int deck = 0; deck < 2; ++deck)
            for (int role = 0; role < 4; ++role) controller.setStemGain(deck, role, role == 1 ? 1.0 : 0.0);
        await(evidence, [&] { const auto s = controller.audioState(); return s.stemGain[0] == bassOnly && s.stemGain[1] == bassOnly; },
            "Both exact bass-only gain targets were not acknowledged");
        controller.setPlaying(0, true); controller.setPlaying(1, true);
        await(evidence, [&] { const auto s = controller.audioState(); return s.playing[0] && s.playing[1] &&
            s.sourceFrame[0] >= 8192 && s.sourceFrame[1] >= 8192 && !s.buffering[0] && !s.buffering[1]; },
            "Both generated selected-bass sources did not settle");
        const auto baseline = controller.audioState(); const auto sessions = controller.completedSessions();
        insist(baseline.deckGain == std::array<float, 2>{1.F, 1.F} && baseline.masterGain == 1.F,
            "The independent selected-bass sample oracle uses acknowledged unity deck/master gains");
        const auto wav = QDir(local).filePath("master.wav");
        controller.startRecording(QUrl::fromLocalFile(wav));
        await(evidence, [&] { return controller.recording()["status"].toString() == "Recording" &&
            controller.recording()["acceptedFrames"].toULongLong() >= holdFrames; }, "Continuous recorder did not attach");
        const auto before = controller.audioState();
        evidence.actions["before"] = snapshot(before);
        if (exitScenario) {
            insist(services.alive(0) && services.count(0, "fixture", "complete") == 0 &&
                before.sourceFrame[0] > 0 && before.sourceFrame[0] < before.sourceFrames[0],
                "Induced exit targets a live incomplete producer during active playback, not an already-complete source");
            bound.terminateUnexpectedly();
            evidence.actions["induced_exit"] = "SIGKILL through retained exact-process pidfd; fixture has no descendant mode";
            await(evidence, [&] { const auto s = controller.audioState(); return s.sourceReadError[0] &&
                s.sourceReadErrorObserved[0] && s.buffering[0]; }, "Unexpected active producer exit was not exposed per deck");
            insist(services.count(0, "fixture", "complete") == 0,
                "The killed active fixture did not complete the source before its induced exit");
        } else {
            await(evidence, [&] { const auto s = controller.audioState(); return s.buffering[0] &&
                s.sourceFrame[0] >= conflux::audio::PreparedPlayback::liveReserveFrames &&
                s.sourceFrame[0] < s.sourceFrames[0]; }, "Active first-hop reserve did not naturally drain", std::chrono::seconds(12));
        }
        const auto held = controller.audioState();
        const auto recordedHeld = controller.recording()["acceptedFrames"].toULongLong();
        evidence.actions["held"] = snapshot(held);
        await(evidence, [&] { const auto s = controller.audioState();
            if (s.sourceFrame[0] != held.sourceFrame[0]) throw std::runtime_error("Held live A cursor advanced without supply");
            // Cover a full240-frame availability fade plus8192 stable peer-only frames.
            return s.outputFrame >= held.outputFrame + holdFrames + 512 &&
                controller.recording()["acceptedFrames"].toULongLong() >= recordedHeld + holdFrames + 512;
        }, "Prepared peer/output did not continue through the held A interval");
        const auto continued = controller.audioState();
        continuousPeer(before, continued); continuousPeer(held, continued);
        insist(continued.sourceFrame[0] == held.sourceFrame[0] &&
            continued.starvedFrames[0] >= held.starvedFrames[0] + holdFrames &&
            controller.completedSessions() == sessions && controller.outputReady() && controller.error().isEmpty() &&
            controller.recording()["active"].toBool() && controller.recording()["path"].toString() == wav &&
            controller.recording()["acceptedFrames"].toULongLong() >= recordedHeld + holdFrames &&
            controller.decks()[0].toMap()["sourceReadError"].toBool() == exitScenario,
            "Exact held A cursor coexists with advancing suitable peer/output/recorder and truthful per-deck health");
        evidence.actions["continued"] = snapshot(continued);
        if (exitScenario) {
            controller.loadStemDeck(0, QUrl::fromLocalFile(b)); // Explicit compatible prepared replacement; remains paused.
            await(evidence, [&] { const auto s = controller.audioState(); return !controller.busy() &&
                s.sourceGeneration[0] != held.sourceGeneration[0] && !controller.liveStemObservations(0) && services.gone(0); },
                "Explicit prepared replacement did not retire the failed live owner", std::chrono::seconds(15));
            const auto installed = controller.audioState();
            insist(controller.preparation()["cacheHit"].toBool() && installed.stemBank[0] &&
                !installed.playing[0] && installed.sourceFrame[0] == 0 && !installed.sourceReadError[0] &&
                !installed.sourceReadErrorObserved[0] && !installed.buffering[0] && !installed.starvedFrames[0] &&
                bound.groupGone() && services.size() == 1 && controller.completedSessions() == sessions,
                "Explicit paused complete-bank replacement clears only the new generation and reaps old service/group/job");
            evidence.actions["replacement"] = snapshot(installed);
            await(evidence, [&] { return controller.audioState().outputFrame >= installed.outputFrame + holdFrames; },
                "Prepared peer/recorder did not continue after explicit replacement");
            continuousPeer(continued, controller.audioState());
        } else {
            // The marker is exclusive, inside the owned fixture directory, and not an arbitrary user path.
            QFile marker(fixture.marker(".release"));
            insist(marker.open(QIODevice::WriteOnly | QIODevice::NewOnly) && marker.write("release\n") == 8 && marker.flush(),
                "Only the owned gate-after-first fixture receives its exclusive release marker");
            marker.close();
            await(evidence, [&] { const auto s = controller.audioState(); return !s.buffering[0] &&
                s.sourceFrame[0] >= held.sourceFrame[0] + holdFrames; }, "Held source did not recover after same-owner release");
            const auto recovered = controller.audioState();
            continuousPeer(continued, recovered);
            insist(controller.liveStemObservations(0) == handle && handle->setup().sourceToken == token &&
                !recovered.sourceReadError[0] && !recovered.sourceReadErrorObserved[0] &&
                services.size() == 1 && services.alive(0) && services.count(0, "command", "load") == 1 &&
                services.count(0, "command", "seek") == 0 && services.count(0, "fixture", "runtime") == 1 &&
                controller.completedSessions() == sessions,
                "Natural reserve recovery keeps the exact service/source owner with no new load, runtime, seek or session");
            evidence.actions["recovered"] = snapshot(recovered);
        }
        insist(controller.recording()["active"].toBool() && controller.recording()["path"].toString() == wav,
            "Both actions preserve the same continuous master recorder");
        controller.stopRecording();
        await(evidence, [&] { return !controller.recording()["active"].toBool(); }, "Generated recording did not finalize");
        const auto recording = controller.recording();
        insist(recording["status"].toString() == "Complete" && recording["acceptedFrames"] == recording["writtenFrames"] &&
            recording["overflowFrames"].toULongLong() == 0,
            "Every accepted master frame finalizes without writer overflow");
        current();
        report["wav"] = inspectCompleteRecording(wav, recording["writtenFrames"].toULongLong(), !exitScenario);
        current();
        report["source_sha256"] = digest(a); report["peer_sha256"] = digest(b);
        report["children"] = services.identities(); report["final_audio"] = snapshot(controller.audioState());
        controller.shutdown(); evidence.drain();
        const auto c = handle->counters();
        insist(services.gone(0) && bound.groupGone() && c.producerFinished && !c.droppedRecords &&
            !c.invalidClockReads && c.attemptedRecords == evidence.ordinal && !controller.busy() && !controller.outputReady(),
            "Shutdown proves exact service/group/job retirement and complete loss-free local observations before safety cleanup");
        std::optional<std::uint64_t> firstEpoch;
        for (const auto& row : evidence.records) {
            const auto epoch = row.toObject()["epoch"].toVariant().toULongLong();
            if (!epoch) continue; // Setup/old-context rows can legitimately have no accepted epoch.
            if (!firstEpoch) firstEpoch = epoch;
            if (epoch != *firstEpoch) throw std::runtime_error("Unexpected new provider epoch in no-seek scenario");
        }
        insist(firstEpoch.has_value(), "Actual positive provider epoch is observed and unchanged through recovery/retirement");
        evidence.actions["same_owner_provider_epoch"] = qint64(*firstEpoch);
        const auto trace = QFileInfo(fixture.marker(".trace"));
        insist(trace.size() <= 1024 * 1024, "Generated fixture process trace remains bounded");
        report["passed"] = true;
        evidence.write(true);
        evidence.controller = nullptr;
        insist(QDir(cacheHome).removeRecursively() && !QFileInfo::exists(cacheHome),
            "Only the fully retired owned scenario cache is removed before the next scenario");
        results.append(report);
    } catch (const std::exception& error) {
        controller.shutdown();
        try { evidence.write(false, QString::fromUtf8(error.what())); } catch (...) {}
        evidence.controller = nullptr;
        report["passed"] = false; report["error"] = error.what(); report["children"] = services.identities();
        results.append(report);
        throw;
    }
}

int run(int argc, char** argv, bool functionalOnly)
{
    QJsonArray results;
    QString error;
    bool passed = false;
    const auto begin = Clock::now(); ownerDeadline = begin + std::chrono::seconds(75);
    try {
        const std::filesystem::path runtime(argv[1]), output(argv[2]);
        validatePrivate(runtime, output);
        const rlimit noCore{0, 0}, fileLimit{fileBytesLimit, fileBytesLimit};
        insist(::setrlimit(RLIMIT_CORE, &noCore) == 0 && ::setrlimit(RLIMIT_FSIZE, &fileLimit) == 0 &&
            ::prctl(PR_SET_DUMPABLE, 0) == 0, "Owned generated fixtures have no core dumps and a hard64MiB per-file limit");
        std::signal(SIGINT, requestStop); std::signal(SIGTERM, requestStop);
        QCoreApplication app(argc, argv);
        app.setOrganizationName("CONFLUX"); app.setApplicationName("preparation-controller-check");
        const auto privateRoot = QString::fromStdString((runtime / "live-stall-owner").string());
        directory(privateRoot);
        pw_init(nullptr, nullptr);
        scenario(QString::fromStdString(output.string()), privateRoot, false, results);
        scenario(QString::fromStdString(output.string()), privateRoot, true, results);
        insist(QDir(privateRoot).removeRecursively() && !QFileInfo::exists(privateRoot),
            "All retired scenario cache/library trees are gone before private-daemon graph inspection");
        current(); passed = true;
    } catch (const std::exception& caught) { error = QString::fromUtf8(caught.what()); }
    QJsonObject final{{"schema", 1}, {"passed", passed}, {"qualified", false}, {"checks", assertions},
        {"error", error}, {"scenarios", results}, {"elapsed_seconds", std::chrono::duration<double>(Clock::now() - begin).count()},
        {"limits", QJsonObject{{"owner_seconds", 75}, {"private_runner_seconds", functionalOnly ? 240 : 90},
          {"per_file_hard_bytes", qint64(fileBytesLimit)}, {"prepared_cache_admission_bytes", qint64(256 * MiB)},
          {"prepared_leaf_bytes", qint64(16 * MiB)}, {"source_bytes", qint64(8 * MiB)},
          {"combined_observed_bytes", qint64(totalBytesLimit)}, {"files_per_tree", 256}, {"directories_per_tree", 128}}},
        {"scope", "Generated virtual controller/recorder support check only. Explicit replacement remains paused. No physical/model/GPU, sustained, musical, hard-cancel or4095ms latency qualification."}};
    const auto bytes = QJsonDocument(final).toJson(QJsonDocument::Indented);
    if (bytes.size() > 8 * qint64(MiB)) return 1;
    std::cout << bytes.constData();
    return passed ? 0 : 1;
}

} // namespace liveFailure
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
        bool functionalOnly = false, liveFailureOnly = false;
        const auto usage = "Usage: preparation-controller-check PRIVATE_RUNTIME EMPTY_RECORDING_DIR [--functional-only] [--live-failure-only]";
        if (argc < 3) throw std::runtime_error(usage);
        for (int argument = 3; argument < argc; ++argument) {
            const std::string_view option(argv[argument]);
            if (option == "--functional-only") functionalOnly = true;
            else if (option == "--live-failure-only") liveFailureOnly = true;
            else throw std::runtime_error(usage);
        }
        if (liveFailureOnly) return liveFailure::run(argc, argv, functionalOnly);
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
        auto savedCatalog = catalogSnapshot();
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
        const auto cuePath = fixtures.filePath("generated-b.cue");
        {
            QFile cue(cuePath);
            const QByteArray sheet = "FILE \"generated-b.wav\" WAVE\n"
                " TRACK 01 AUDIO\n TITLE \"First tick\"\n INDEX 01 00:00:01\n"
                " TRACK 02 AUDIO\n TITLE \"Last tick of second\"\n INDEX 01 00:00:74\n";
            require(cue.open(QIODevice::WriteOnly) && cue.write(sheet) == sheet.size() && cue.flush(),
                    "Private CUE sheet contains known 75-frame timestamps");
        }
        const auto cueImportBefore = controller.audioState();
        controller.importCueSheet(libraryId, QUrl::fromLocalFile(cuePath));
        require(controller.busy() && controller.preparation()["cancellable"].toBool(),
                "CUE parsing owns an asynchronous cancellable operation");
        wait([&] { return !controller.busy() && controller.audioState().outputFrame >= cueImportBefore.outputFrame + 1024; },
             "CUE metadata saving or advancing A snapshot timed out");
        const auto cueImported = controller.audioState();
        const auto cueRows = controller.libraryTracks()[0].toMap()["cues"].toList();
        require(controller.error().isEmpty() && cueRows.size() == 2 &&
                cueRows[0].toMap()["frame"].toULongLong() == 640 &&
                cueRows[1].toMap()["frame"].toULongLong() == 47360 &&
                !controller.decks()[1].toMap()["loaded"].toBool() &&
                controller.submittedCommands() == importCommands && controller.completedSessions() == importSessions &&
                cueImported.playing[0] && cueImported.sourceGeneration == cueImportBefore.sourceGeneration &&
                cueImported.sourceFrame[0] - cueImportBefore.sourceFrame[0] ==
                    cueImported.outputFrame - cueImportBefore.outputFrame && !sink.wrong() && !sink.zeros(),
                "CUE import persists exact source frames without installing B or disturbing advancing A");
        savedCatalog = catalogSnapshot();
        observations["cue_import"] = QJsonObject{{"count", 2}, {"first_frame", 640}, {"second_frame", 47360},
            {"a_advancing", cueImported.sourceFrame[0] > cueImportBefore.sourceFrame[0]}, {"b_loaded", false}};
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
        controller.loadLibraryTrack(1, libraryId);
        wait([&] { return !controller.busy() && controller.decks()[1].toMap()["loaded"].toBool() && controller.outputReady(); },
             "B did not load for imported cue jumps");
        const auto cueJumpBefore = controller.audioState();
        require(controller.decks()[1].toMap()["sourceId"].toString() == libraryId &&
                !cueJumpBefore.playing[1] && !controller.jumpToImportedCue(0, libraryId, 0),
                "Imported cue jumps require the exact loaded source identity");
        require(controller.jumpToImportedCue(1, libraryId, 0), "First imported cue submits an exact-frame seek");
        wait([&] { return controller.audioState().sourceFrame[1] == 640; }, "First imported cue did not land at frame640");
        require(controller.jumpToImportedCue(1, libraryId, 1), "Second imported cue submits an exact-frame seek");
        wait([&] { return controller.audioState().sourceFrame[1] == 47360; }, "Second imported cue did not land at frame47360");
        const auto cueJumped = controller.audioState();
        require(!cueJumped.playing[1] && cueJumped.cueFrame[1] == cueJumpBefore.cueFrame[1] &&
                cueJumped.playing[0] && cueJumped.sourceGeneration[0] == cueJumpBefore.sourceGeneration[0] &&
                cueJumped.sourceFrame[0] - cueJumpBefore.sourceFrame[0] == cueJumped.outputFrame - cueJumpBefore.outputFrame &&
                !cueJumped.invalidBuffers && !cueJumped.rejected && !sink.wrong() && !sink.zeros(),
                "Exact cue jumps preserve paused B, its temporary CUE and the advancing peer clock/samples");
        observations["cue_jump"] = QJsonObject{{"landed_frame", qint64(cueJumped.sourceFrame[1])},
            {"b_paused", !cueJumped.playing[1]}, {"temporary_cue_unchanged", true}, {"peer_clock_unchanged", true}};
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
        runStemController(output, fixtures.path(), a, b, libraryId, functionalOnly, require, observations);
        runScratchController(output, a, b, require, observations);
        runLiveStemController(output, fixtures.path(), a, functionalOnly, require, observations);
        runLostLiveOwners(fixtures.path(), require, observations);
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

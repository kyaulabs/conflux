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
#include <QProcess>
#include <QProcessEnvironment>
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
#include <limits>
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
    bool observingValidation() const { return observedArmed_.load(std::memory_order_acquire); }
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
        std::array<std::uint64_t, 2> received{};
        for (std::size_t channel = 0; channel < self.ports_.size(); ++channel) {
            const auto* samples = static_cast<const float*>(pw_filter_get_dsp_buffer(self.ports_[channel], frames));
            if (!samples || !armed) continue;
            std::uint64_t wrong = 0, zeros = 0;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                zeros += samples[frame] == 0.F;
                wrong += !std::isfinite(samples[frame]) || std::abs(samples[frame] - expected[channel]) > self.tolerance_;
            }
            self.wrong_.fetch_add(wrong, std::memory_order_relaxed);
            self.zeros_.fetch_add(zeros, std::memory_order_relaxed);
            received[channel] = frames;
        }
        // Counts acknowledge both channels' completed sample/error checks.
        // Disarming is observed only after any previously armed work finishes.
        self.observedArmed_.store(armed, std::memory_order_release);
        for (std::size_t channel = 0; channel < received.size(); ++channel)
            self.received_[channel].fetch_add(received[channel], std::memory_order_release);
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
    std::atomic<bool> observedArmed_{false};
    pw_thread_loop* loop_ = nullptr;
    pw_filter* filter_ = nullptr;
    bool started_ = false;
};

QJsonObject inspectStemRecording(const QString& path, std::uint64_t prefixFrames, std::uint64_t expectedFrames,
                                 float expectedGain = 1.F, bool excludeRebuildMiddle = false,
                                 const std::array<float, 2>& expectedSamples = expected)
{
    if (!std::isfinite(expectedGain) || expectedGain < 0.F || expectedGain > 1.F ||
        std::ranges::any_of(expectedSamples, [](float sample) { return !std::isfinite(sample); }))
        throw std::runtime_error("Invalid generated master recording oracle gain/samples");
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
                const auto target = expectedSamples[channel] * expectedGain;
                const bool steady = frame < prefixFrames || frame + 1024 >= frames;
                if (!std::isfinite(value) || ((!excludeRebuildMiddle || steady) &&
                        std::abs(value) > std::abs(target) + 1e-6F))
                    throw std::runtime_error("Stem recording contains nonfinite or unexpected master audio");
                if (steady)
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
        {"transient_middle_frames_excluded", qint64(excludeRebuildMiddle ? frames - checkedFrames : 0)},
        {"scope", excludeRebuildMiddle
            ? "Generated steady master prefix/tail checked at 1e-6; explicit rebuild middle checked only for finiteness, with no continuous-delivery claim."
            : "Generated master prefix and tail checked at 1e-6 tolerance; explicit routing rebuild may omit wall time and ramp between them."}};
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
        // Replacement and global controls use independent mailboxes. Observe
        // both acknowledgements before checking their shared invariants.
        wait([&] {
            const auto state = controller.audioState();
            return !controller.busy() && !state.stemBank[0] && state.crossfader == .75F;
        }, "Crossfader acknowledgement and target A replacement did not finish");
    } catch (...) {
        QObject::disconnect(crossfaderConnection);
        throw;
    }
    QObject::disconnect(crossfaderConnection);
    const auto crossfaderInstalled = controller.audioState();
    if (!(crossfaderRequestedWhileReplacing && deckScopedCommandRejected && crossfaderCommandAccepted &&
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
            controller.completedSessions() == sessions + 1)) {
        const auto audio = [](const auto& state) {
            return QJsonObject{
                {"applied", qint64(state.applied)}, {"output_frame", qint64(state.outputFrame)},
                {"source_generations", QJsonArray{qint64(state.sourceGeneration[0]), qint64(state.sourceGeneration[1])}},
                {"source_frames", QJsonArray{qint64(state.sourceFrame[0]), qint64(state.sourceFrame[1])}},
                {"source_total_frames", QJsonArray{qint64(state.sourceFrames[0]), qint64(state.sourceFrames[1])}},
                {"playing", QJsonArray{state.playing[0], state.playing[1]}},
                {"deck_gain", QJsonArray{state.deckGain[0], state.deckGain[1]}},
                {"cue_frames", QJsonArray{qint64(state.cueFrame[0]), qint64(state.cueFrame[1])}},
                {"cue_selected", QJsonArray{state.cueSelected[0], state.cueSelected[1]}},
                {"cue_gain", state.cueGain}, {"crossfader", state.crossfader}};
        };
        observations["crossfader_replacement_failure"] = QJsonObject{
            {"requested_while_replacing", crossfaderRequestedWhileReplacing},
            {"deck_scoped_command_rejected", deckScopedCommandRejected},
            {"crossfader_command_accepted", crossfaderCommandAccepted},
            {"crossfader_commands_before", qint64(crossfaderCommandsBefore)},
            {"submitted_commands", qint64(controller.submittedCommands())},
            {"controller_error", controller.error()}, {"controller_crossfader", controller.crossfader()},
            {"completed_sessions", qint64(controller.completedSessions())},
            {"sessions_before_rebuild", qint64(sessions)}, {"expected_completed_sessions", qint64(sessions + 1)},
            {"before_crossfader", audio(beforeCrossfader)}, {"crossfader_installed", audio(crossfaderInstalled)},
            {"clauses", QJsonObject{
                {"requested_while_replacing", crossfaderRequestedWhileReplacing},
                {"deck_scoped_command_rejected", deckScopedCommandRejected},
                {"crossfader_command_accepted", crossfaderCommandAccepted},
                {"controller_error_empty", controller.error().isEmpty()},
                {"audio_crossfader_075", crossfaderInstalled.crossfader == .75F},
                {"controller_crossfader_075", controller.crossfader() == .75},
                {"applied_advanced", crossfaderInstalled.applied > beforeCrossfader.applied},
                {"A_generation_advanced", crossfaderInstalled.sourceGeneration[0] > beforeCrossfader.sourceGeneration[0]},
                {"A_paused", !crossfaderInstalled.playing[0]}, {"A_cursor_zero", !crossfaderInstalled.sourceFrame[0]},
                {"B_generation_unchanged", crossfaderInstalled.sourceGeneration[1] == beforeCrossfader.sourceGeneration[1]},
                {"B_playing", crossfaderInstalled.playing[1]}, {"B_gain_zero", crossfaderInstalled.deckGain[1] == 0.F},
                {"B_source_delta_equals_output_delta", crossfaderInstalled.sourceFrame[1] - beforeCrossfader.sourceFrame[1] ==
                    crossfaderInstalled.outputFrame - beforeCrossfader.outputFrame},
                {"B_cue_frame_unchanged", crossfaderInstalled.cueFrame[1] == beforeCrossfader.cueFrame[1]},
                {"cue_selection_unchanged", crossfaderInstalled.cueSelected == beforeCrossfader.cueSelected},
                {"cue_gain_unchanged", crossfaderInstalled.cueGain == beforeCrossfader.cueGain},
                {"completed_sessions_expected", controller.completedSessions() == sessions + 1}}},
            {"scope", "Failure-only public snapshots and exact assertion operands; no coherent callback-time or root-cause inference."}};
    }
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

template<class Check>
void runEqController(const std::filesystem::path& output, const QString& a, const QString& b,
                     Check&& require, QJsonObject& observations)
{
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    auto await = [&](auto&& predicate, const char* message) {
        wait([&] {
            if (Clock::now() >= deadline) throw std::runtime_error("Generated EQ phase exceeded its bounded wait");
            return predicate();
        }, message);
    };
    conflux::DeckController controller;
    Sink master("conflux-eq-master-" + std::to_string(::getpid()));
    Sink cue("conflux-eq-cue-" + std::to_string(::getpid()), 1e-6F);
    controller.refreshAudioRouting();
    std::array<QString, 4> ports;
    await([&] {
        for (const auto& value : controller.audioRouting()["ports"].toList()) {
            const auto port = value.toMap();
            for (std::size_t bus = 0; bus < 2; ++bus)
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    const auto label = QString::fromStdString(bus ? cue.name() : master.name()) +
                        QString(" · input_%1 [%2]").arg(int(channel)).arg(channel ? "FR" : "FL");
                    if (port["label"].toString() == label) ports[bus * 2 + channel] = port["key"].toString();
                }
        }
        return std::all_of(ports.begin(), ports.end(), [](const auto& port) { return !port.isEmpty(); });
    }, "EQ master/cue private pairs were not discovered");
    controller.configureAudioRouting(ports[0], ports[1], ports[2], ports[3]);
    controller.loadDeck(0, QUrl::fromLocalFile(a));
    await([&] { return !controller.busy() && controller.outputReady(); }, "EQ A did not load");
    controller.loadDeck(1, QUrl::fromLocalFile(b));
    await([&] { return !controller.busy() && controller.decks()[1].toMap()["loaded"].toBool(); }, "EQ B did not load");
    constexpr std::array<float, 3> cut{-26.F, 3.F, -7.F}, peer{6.F, -9.F, 4.F};
    auto target = cut;
    for (int band = 0; band < 3; ++band) {
        controller.setEqGainDb(0, band, target[band]); controller.setEqGainDb(1, band, peer[band]);
    }
    const auto boost = std::pow(10., 6. / 20.);
    controller.setDeckGain(0, .5); controller.setDeckGain(1, 0.);
    controller.setMasterGain(.5); controller.setCrossfader(.75);
    controller.setCueGain(1. / boost); controller.setCueSelected(0, true);
    controller.setPlaying(0, true); controller.setPlaying(1, true);
    await([&] { const auto s = controller.audioState(); return s.eqGainDb[0] == target && s.eqGainDb[1] == peer &&
        s.playing[0] && s.playing[1] && s.deckGain[0] == .5F && s.deckGain[1] == 0.F &&
        s.masterGain == .5F && s.crossfader == .75F && s.cueSelected[0] && s.cueReady; }, "EQ targets/routes were not acknowledged");
    const auto initial = controller.audioState();
    const auto sessions = controller.completedSessions(), submitted = controller.submittedCommands();
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), -26.1, 6.1}) controller.setEqGainDb(0, 0, bad);
    controller.setEqGainDb(2, 0, 1.); controller.setEqGainDb(0, 3, 1.);
    require(controller.submittedCommands() == submitted && controller.audioState().eqGainDb == initial.eqGainDb,
            "Invalid real-controller EQ requests do not enter the queue or mutate targets");
    controller.setEqGainDb(0, 0, -26.); // Clear the expected validation error through a valid request.
    auto settle = [&] {
        const auto first = controller.audioState().outputFrame;
        await([&] { return controller.audioState().outputFrame >= first + 4096; }, "EQ ramps/DC filter state did not settle");
    };
    auto finishRecording = [&](const QString& path, std::uint64_t prefix, float multiplier, bool rebuild) {
        controller.stopRecording();
        await([&] { return !controller.recording()["active"].toBool(); }, "EQ recording did not finalize");
        const auto r = controller.recording(); const auto frames = r["writtenFrames"].toULongLong();
        require(r["status"].toString() == "Complete" && r["error"].toString().isEmpty() &&
                r["acceptedFrames"] == r["writtenFrames"] && r["overflowFrames"].toULongLong() == 0,
                "EQ master recorder drains exactly its accepted frames without overflow");
        return inspectStemRecording(path, prefix ? prefix : frames, frames, multiplier, rebuild);
    };
    const auto detailPath = QString::fromStdString((output / "generated-eq-controller-detail.json").string());
    QJsonObject detail{{"scope", "Generated constant PCM; queued EQ ACK, source replacement, routing restore, analytic DC master and prefader cue samples. No musical/frequency-response, continuous-delivery, hardware or latency qualification."}};
    try {
        settle();
        const auto cutPath = QString::fromStdString((output / "generated-eq-cut.wav").string());
        controller.startRecording(QUrl::fromLocalFile(cutPath));
        await([&] { return controller.recording()["status"].toString() == "Recording" &&
            controller.recording()["acceptedFrames"].toULongLong() >= 2048; }, "Cut EQ recording did not attach");
        detail["cut_recording"] = finishRecording(cutPath, 0, float(std::pow(10., -26. / 20.) * .125), false);
        target[0] = 6.F; controller.setEqGainDb(0, 0, target[0]);
        await([&] { return controller.audioState().eqGainDb[0] == target; }, "Boost EQ was not acknowledged");
        const auto beforeReplace = controller.audioState();
        controller.loadDeck(0, QUrl::fromLocalFile(a));
        await([&] { return !controller.busy() && controller.audioState().sourceGeneration[0] > beforeReplace.sourceGeneration[0]; }, "EQ source replacement did not finish");
        const auto installed = controller.audioState();
        require(installed.eqGainDb[0] == target && installed.eqGainDb[1] == peer && !installed.playing[0] &&
                installed.playing[1] && installed.sourceGeneration[1] == beforeReplace.sourceGeneration[1] &&
                installed.sourceFrame[1] - beforeReplace.sourceFrame[1] == installed.outputFrame - beforeReplace.outputFrame &&
                installed.deckGain[1] == 0.F && installed.masterGain == .5F && installed.crossfader == .75F &&
                controller.completedSessions() == sessions, "Source handoff preserves channel EQ and the advancing muted peer/globals");
        controller.setDeckGain(0, .5); controller.setCueSelected(0, true); controller.setPlaying(0, true);
        await([&] { const auto s = controller.audioState(); return s.playing[0] && s.deckGain[0] == .5F && s.cueSelected[0]; }, "Replaced EQ A did not restart");
        settle();
        auto checkCue = [&] {
            const auto left = cue.received(0), right = cue.received(1);
            cue.validate(true);
            await([&] { return cue.received(0) >= left + 1024 && cue.received(1) >= right + 1024; }, "Fresh equalized cue samples did not arrive");
            QJsonArray checks = detail["cue_checks"].toArray();
            checks.append(QJsonObject{{"received_left_before", qint64(left)}, {"received_right_before", qint64(right)},
                {"received_left_after", qint64(cue.received(0))}, {"received_right_after", qint64(cue.received(1))},
                {"wrong_samples", qint64(cue.wrong())}, {"zero_samples", qint64(cue.zeros())},
                {"validation_acknowledged", cue.observingValidation()}, {"cue_ready", controller.audioState().cueReady},
                {"output_frame", qint64(controller.audioState().outputFrame)}});
            detail["cue_checks"] = checks;
            require(!cue.wrong() && !cue.zeros(), "Prefader cue contains analytic boosted PCM independently of channel/master/crossfader gains");
        };
        checkCue();
        const auto boostPath = QString::fromStdString((output / "generated-eq-boost.wav").string());
        controller.startRecording(QUrl::fromLocalFile(boostPath));
        await([&] { return controller.recording()["status"].toString() == "Recording" &&
            controller.recording()["acceptedFrames"].toULongLong() >= 2048; }, "Boost EQ recording did not attach");
        const auto prefix = controller.recording()["acceptedFrames"].toULongLong();
        const auto beforeRoute = controller.audioState(); const auto sourceIds = controller.decks(); cue.validate(false);
        // checkCue above proved a completed armed callback, so this false ACK
        // cannot be the initial default or precede in-flight armed sample work.
        await([&] { return !cue.observingValidation(); }, "Cue validation did not acknowledge disarm before rebuild");
        // Exercise an old GUI checkpoint while the current session keeps advancing.
        // It must not later be mistaken for settled output from the rebuilt session.
        await([&] { return controller.audioState().outputFrame >= beforeRoute.outputFrame + 8192; },
              "Old EQ session did not advance beyond the captured routing checkpoint");
        controller.configureAudioRouting(ports[0], ports[1], ports[2], ports[3]);
        await([&] { return !controller.busy() && controller.outputReady() && controller.completedSessions() == sessions + 1 &&
            controller.audioState().cueReady &&
            controller.audioState().sourceFrame[0] >= beforeRoute.sourceFrame[0] + 4096; }, "EQ routing rebuild did not restore cue readiness");
        const auto restored = controller.audioState();
        require(restored.eqGainDb == beforeRoute.eqGainDb && restored.playing == beforeRoute.playing &&
                restored.deckGain == beforeRoute.deckGain && restored.masterGain == beforeRoute.masterGain &&
                restored.crossfader == beforeRoute.crossfader && restored.cueGain == beforeRoute.cueGain &&
                restored.cueSelected == beforeRoute.cueSelected && restored.cueFrame == beforeRoute.cueFrame &&
                restored.sourceFrames == beforeRoute.sourceFrames && restored.sourceFrame[1] >= beforeRoute.sourceFrame[1] &&
                controller.decks()[0].toMap()["sourceId"] == sourceIds[0].toMap()["sourceId"] &&
                controller.decks()[1].toMap()["sourceId"] == sourceIds[1].toMap()["sourceId"] && controller.recording()["active"].toBool() &&
                controller.recording()["path"].toString() == boostPath,
                "Explicit routing rebuild restores all EQ targets, transport, cue and gains with the same active writer");
        // The pre-rebuild source checkpoint can already be old. Measure settling
        // from this ready session's output clock, not that earlier source cursor.
        detail["rebuild_cue_ready_output_frame"] = qint64(controller.audioState().outputFrame);
        settle();
        detail["rebuild_settled_output_frame"] = qint64(controller.audioState().outputFrame);
        checkCue();
        detail["boost_rebuild_recording"] = finishRecording(boostPath, prefix, float(boost * .125), true);
        const auto final = controller.audioState();
        require(controller.error().isEmpty() && !final.invalidBuffers && !final.rejected &&
                !final.sourceReadError[0] && !final.sourceReadError[1] && !final.sourceReadErrorObserved[0] &&
                !final.sourceReadErrorObserved[1] && !final.starvedFrames[0] && !final.starvedFrames[1], "EQ phase retains healthy prepared sources and queue");
        detail["eq_a"] = QJsonArray{target[0], target[1], target[2]}; detail["eq_b"] = QJsonArray{peer[0], peer[1], peer[2]};
        detail["cue_checked_left_frames"] = qint64(cue.received(0)); detail["cue_checked_right_frames"] = qint64(cue.received(1));
        detail["cue_gain"] = final.cueGain; detail["passed"] = true;
        detail["replacement_generations"] = QJsonArray{qint64(beforeReplace.sourceGeneration[0]), qint64(installed.sourceGeneration[0])};
        detail["peer_replacement_advance"] = qint64(installed.sourceFrame[1] - beforeReplace.sourceFrame[1]);
        detail["rebuild_source_frames_before"] = QJsonArray{qint64(beforeRoute.sourceFrame[0]), qint64(beforeRoute.sourceFrame[1])};
        detail["rebuild_source_frames_after"] = QJsonArray{qint64(restored.sourceFrame[0]), qint64(restored.sourceFrame[1])};
        require(save(detailPath, detail), "Bounded EQ controller/sample evidence is retained");
        controller.shutdown(); observations["eq_controller"] = detail;
    } catch (const std::exception& error) {
        detail["passed"] = false; detail["error"] = error.what();
        detail["recording_at_failure"] = QJsonObject::fromVariantMap(controller.recording());
        (void)save(detailPath, detail); controller.shutdown(); throw;
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
constexpr std::uint32_t recordingRampFrames = 240;
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
    bool saved = false, inspectResources = true;
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
                if (inspectResources && !controller->busy()) {
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
    // Paused setup may drop GUI reports without losing audio. Require no new
    // dropped snapshots inside this playing interval; retain cumulative counts.
    if (after.outputFrame <= before.outputFrame || after.sourceFrame[1] < before.sourceFrame[1] ||
        after.sourceFrame[1] - before.sourceFrame[1] != after.outputFrame - before.outputFrame ||
        after.sourceGeneration[1] != before.sourceGeneration[1] || !after.playing[1] ||
        after.buffering[1] || after.sourceReadError[1] || after.sourceReadErrorObserved[1] ||
        after.starvedFrames[1] != 0 || after.invalidBuffers || after.rejected ||
        after.snapshotDrops != before.snapshotDrops)
        throw std::runtime_error("Prepared bass peer/output atomic clocks or health changed: " +
            QJsonDocument(QJsonObject{{"before", snapshot(before)}, {"after", snapshot(after)}})
                .toJson(QJsonDocument::Compact).toStdString());
}
std::array<float, 2> ramp(bool rising, std::uint32_t step)
{
    std::array<float, 2> value{};
    const double fraction = double(step) / recordingRampFrames;
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
            if (phase == FadeOut && step == recordingRampFrames) phase = Peer;
            if (phase == FadeIn && step == recordingRampFrames) phase = Recovered;
        }
        first += count;
    }
    if (counts[Both] < 1024 || counts[FadeOut] != recordingRampFrames || counts[Peer] < holdFrames ||
        (recovery ? (phase != Recovered || counts[FadeIn] != recordingRampFrames || counts[Recovered] < 1024) :
                    (phase != Peer || counts[FadeIn] || counts[Recovered])))
        throw std::runtime_error("Required recorded baseline/held/recovered phases incomplete: phase=" +
            std::to_string(int(phase)) + ", recovery=" + std::to_string(recovery) +
            ", baseline=" + std::to_string(counts[Both]) + " (min1024)" +
            ", fade_out=" + std::to_string(counts[FadeOut]) + " (exact" + std::to_string(recordingRampFrames) + ")" +
            ", peer_only=" + std::to_string(counts[Peer]) + " (min" + std::to_string(holdFrames) + ")" +
            ", fade_in=" + std::to_string(counts[FadeIn]) + " (recovery exact" + std::to_string(recordingRampFrames) + "; otherwise0)" +
            ", recovered=" + std::to_string(counts[Recovered]) + " (recovery min1024; otherwise0)");
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
        report["failure_context"] = QJsonObject{{"audio", snapshot(controller.audioState())},
            {"recording", QJsonObject::fromVariantMap(controller.recording())}, {"actions", evidence.actions}};
        controller.shutdown();
        try { evidence.write(false, QString::fromUtf8(error.what())); } catch (...) {}
        evidence.controller = nullptr;
        report["passed"] = false; report["error"] = error.what(); report["children"] = services.identities();
        results.append(report);
        throw;
    }
}

void residentScenario(const QString& output, const QString& privateRoot, QJsonArray& results,
                      std::string_view continuationMode = {})
{
    const bool takeover = continuationMode == "takeover", cancelLookup = continuationMode == "cancel";
    const bool preparedContinuation = takeover || cancelLookup;
    const auto name = QStringLiteral("resident-") + (takeover ? "takeover" : cancelLookup ? "cancel" : "hold");
    const auto local = QDir(output).filePath(name), cacheHome = QDir(privateRoot).filePath(name);
    directory(local); directory(cacheHome);
    Evidence evidence{name, output, privateRoot};
    evidence.inspectResources = false; // Same quiescent-only accounting as continuation cases.
    QJsonObject report{{"scenario", name}, {"qualified", false}, {"filesystem_observations", "quiescent-only"}};
    const auto a = QDir(local).filePath("live-a.wav"), b = QDir(local).filePath("normal-b.wav");
    writeFixture(a, 8192, -4096, 12 * 44100, 44100);
    writeFixture(b, 2048, -1024, 30 * 48000, 48000); // Exact bBass, no extra exporter/prepared-bank seed.
    const auto fixture = conflux::test::makeStemServiceFixture(local, "gate-after-first");
    ServiceTrace services(fixture);
    insist(qputenv("XDG_CACHE_HOME", QFile::encodeName(cacheHome)) &&
        qputenv("XDG_DATA_HOME", QFile::encodeName(QDir(cacheHome).filePath("data"))),
        "Resident range cache/library stay under the private owner");
    conflux::DeckController::PreparationOptions options;
    options.stemEngine = fixture.engine; options.stemServicePath = fixture.service; options.limits = tightLimits();
    conflux::DeckController controller(options);
    evidence.controller = &controller;
    Sink sink("conflux-resident-" + name.toStdString() + "-" + std::to_string(::getpid()));
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
    QLockFile gate(QDir(cache).filePath(".lock"));
    bool locked = false;
    try {
        QString left, right;
        privateLivePair(controller, sink, left, right, insist);
        controller.importTrack(QUrl::fromLocalFile(a));
        await(evidence, [&] { return !controller.busy(); }, "Resident source import did not finish", std::chrono::seconds(15));
        insist(controller.error().isEmpty() && controller.libraryTracks().size() == 1,
            "Recovery range uses the real catalog source, not a fabricated source ID");
        const auto id = controller.libraryTracks().front().toMap()["id"].toString();
        controller.loadLibraryStemTrackWithRecoveryRange(0, id, 1.001, 1.065);
        await(evidence, [&] {
            if (!controller.busy() && !controller.error().isEmpty()) throw std::runtime_error(controller.error().toStdString());
            return !controller.busy() && controller.outputReady() && controller.audioState().sourceFrames[0];
        }, "Opted-in genuine live source did not install", std::chrono::seconds(20));
        evidence.live = controller.liveStemObservations(0); services.capture();
        const auto handle = evidence.live;
        insist(handle && !controller.preparation()["cacheHit"].toBool() && services.size() == 1 && services.alive(0),
            "Explicit range is attached to one real live cache-miss owner");
        BoundService bound(services.identities()[0].toObject());
        const auto token = handle->setup().sourceToken;
        const auto hold = [&] { return controller.decks()[0].toMap()["residentHold"].toMap(); };
        const auto arm = [&](const QVariantMap& t) {
            return controller.authorizeResidentHold(0, id, t["outputSession"].toString(), t["rangeId"].toString(),
                t["sourceGeneration"].toString(), t["transportRevision"].toString());
        };
        const auto cancel = [&](const QVariantMap& t) {
            return controller.cancelResidentHold(0, id, t["outputSession"].toString(), t["rangeId"].toString(),
                t["sourceGeneration"].toString(), t["transportRevision"].toString());
        };
        const auto exit = [&](const QVariantMap& t) {
            return controller.exitResidentHold(0, id, t["outputSession"].toString(), t["rangeId"].toString(),
                t["sourceGeneration"].toString(), t["transportRevision"].toString());
        };
        const auto initial = hold();
        insist(initial["configured"].toBool() && initial["ready"].toBool() && !initial["armed"].toBool() &&
            initial["status"] == "NextPlay" && initial["inFrame"].toString() == "48048" &&
            initial["outFrame"].toString() == "51120" && initial["residentBytes"].toULongLong() == 98304,
            "Routine millisecond input resolves once to an exact3072-frame captured unarmed range");
        controller.loadDeck(1, QUrl::fromLocalFile(b));
        await(evidence, [&] { return !controller.busy() && controller.audioState().sourceFrames[1]; },
            "Independent finite bass peer did not install", std::chrono::seconds(15));
        insist(controller.error().isEmpty() && !controller.decks()[1].toMap()["liveStems"].toBool(),
            "Peer uses its existing finite owner and no second live service");
        // Explicit test setup AFTER the genuine initial live miss. The product
        // continuation is cache-only and must never generate this bank itself.
        if (preparedContinuation) {
            seedBank(a, cache, fixture.engine, evidence);
            insist(gate.tryLock(0), "Existing cache lock delays only the automatic resident lookup");
            locked = true;
        }
        const auto exporterCalls = fixture.engine.exporterPath.chopped(3) + ".calls";
        const auto seededCalls = preparedContinuation ? digest(exporterCalls) : QString{};
        const auto originalMetadata = controller.decks()[0].toMap();
        const auto retainedBefore = controller.retainedPcmBytes();
        const auto beforePreparation = controller.preparation();
        for (int role = 0; role < 4; ++role) controller.setStemGain(0, role, role == 1 ? 1. : 0.);
        await(evidence, [&] { return controller.audioState().stemGain[0] == bassOnly; }, "Bass selection was not acknowledged");
        const auto beforePlay = controller.audioState(); const auto sessions = controller.completedSessions();
        controller.setPlaying(0, true); const auto playId = controller.submittedCommands();
        insist(hold()["status"] == "Pending", "First accepted Play waits for a real callback acknowledgement");
        controller.setPlaying(1, true);
        await(evidence, [&] { const auto s = controller.audioState(); return s.residentHold[0].armed &&
            s.sourceFrame[0] >= 8192 && s.sourceFrame[1] >= 8192; }, "First Play did not arm the approved range on the actual callback");
        const auto armed = controller.audioState();
        insist(armed.transportRevision[0] == beforePlay.transportRevision[0] + 2 && armed.lastCommandId > playId &&
            armed.sourceGeneration[0] == beforePlay.sourceGeneration[0] && armed.residentHold[0].rangeId == initial["rangeId"].toULongLong() &&
            !armed.residentHold[0].active && !armed.buffering[0] && !armed.sourceReadError[0] && !armed.starvedFrames[0],
            "Exactly one accepted Play then guarded Arm is observed with unchanged source identity and healthy linear audio");
        evidence.actions["first_play_armed"] = QJsonObject::fromVariantMap(hold());
        const auto wav = QDir(local).filePath("master.wav");
        controller.startRecording(QUrl::fromLocalFile(wav));
        await(evidence, [&] { return controller.recording()["status"] == "Recording" &&
            controller.recording()["acceptedFrames"].toULongLong() >= holdFrames; }, "Resident master recorder did not attach");
        const auto beforeHold = controller.audioState();
        await(evidence, [&] { return controller.audioState().residentHold[0].active; },
            "The armed range did not enter naturally at the owned service shortage", std::chrono::seconds(12));
        const auto held = controller.audioState(); const auto recordedAtHold = controller.recording()["acceptedFrames"].toULongLong();
        await(evidence, [&] { const auto s = controller.audioState();
            if (s.sourceFrame[0] != held.sourceFrame[0]) throw std::runtime_error("Resident loop advanced the parked linear F");
            return s.outputFrame >= held.outputFrame + holdFrames &&
                controller.recording()["acceptedFrames"].toULongLong() >= recordedAtHold + holdFrames;
        }, "Resident loop/peer/recorder did not span multiple actual callbacks");
        const auto looping = controller.audioState(); continuousPeer(beforeHold, looping); continuousPeer(held, looping);
        insist(looping.residentHold[0].active && !looping.buffering[0] && !looping.sourceReadError[0] && !looping.starvedFrames[0] &&
            looping.sourcePosition[0] == double(held.sourceFrame[0]) && looping.residentHold[0].audibleFrame >= 48048 &&
            // The reported next cursor may equal OUT between reads, before wrapping.
            looping.residentHold[0].audibleFrame <= 51120 && controller.completedSessions() == sessions,
            "Shortage repeats only the approved resident range while F is parked, peer/output continue and no A frame is starved");
        evidence.actions["held"] = snapshot(held); evidence.actions["looping"] = QJsonObject::fromVariantMap(hold());
        const auto continuation = [&] { return controller.decks()[0].toMap()["continuation"].toMap(); };
        if (preparedContinuation) {
            await(evidence, [&] { return continuation()["status"] == "Checking" &&
                continuation()["pending"].toBool(); }, "Active unbuffered resident range did not start a cache-only lookup");
            const auto checking = controller.audioState(); const auto attempt = continuation();
            insist(checking.residentHold[0].active && !checking.buffering[0] && !checking.starvedFrames[0] &&
                attempt["attempts"].toULongLong() == 1 && attempt["sourceFrame"].toULongLong() == held.sourceFrame[0] &&
                attempt["sourceGeneration"].toULongLong() == held.sourceGeneration[0] &&
                attempt["residentHoldRangeId"].toString() == initial["rangeId"].toString() && !controller.busy() &&
                controller.preparation() == beforePreparation,
                "One exact range-bound attempt starts while approved audio plays, without buffering or a normal load");
            evidence.actions["continuation_capture"] = QJsonObject::fromVariantMap(attempt);
            if (cancelLookup) {
                insist(cancel(hold()), "An accepted exact resident Cancel revokes the in-flight lookup immediately");
                insist(continuation()["status"] == "Cancelled", "Accepted Cancel wins before its callback snapshot");
                controller.setPlaying(0, false);
                controller.seekSeconds(0, (held.sourceFrame[0]+2048)/48000.);
                controller.seekSeconds(0, held.sourceFrame[0]/48000.);
            }
            gate.unlock(); locked = false;
            await(evidence, [&] { const auto state = controller.audioState();
                return !continuation()["pending"].toBool() && (cancelLookup ?
                    (continuation()["status"] == "Cancelled" && !state.playing[0] &&
                     state.sourceFrame[0] == held.sourceFrame[0] && state.transportRevision[0] > held.transportRevision[0]) :
                    (continuation()["status"] == "Active" && state.sourceGeneration[0] != held.sourceGeneration[0] &&
                     !state.buffering[0] && state.sourceFrame[0] >= held.sourceFrame[0]+1024));
            }, "Resident continuation did not settle the exact hit or manual cancellation");
            const auto settled = controller.audioState(); continuousPeer(looping, settled);
            insist(continuation()["attempts"].toULongLong() == 1 && controller.completedSessions() == sessions &&
                controller.liveStemObservations(0) == handle && handle->setup().sourceToken == token &&
                (cancelLookup || !settled.starvedFrames[0]) && !settled.starvedFrames[1] && !settled.sourceReadErrorObserved[0] &&
                !settled.sourceReadError[0] && controller.recording()["active"].toBool() &&
                !controller.recording()["overflowFrames"].toULongLong(),
                "Settling retains the exact episode, observations, peer, recorder and source health");
            if (takeover) {
                const auto mapped = controller.decks()[0].toMap();
                insist(!mapped["liveStems"].toBool() && !mapped["residentHold"].toMap()["configured"].toBool() &&
                    settled.sourceGeneration[0] == continuation()["generation"].toULongLong() &&
                    settled.transportRevision[0] == held.transportRevision[0]+1 && settled.sourceFrames[0] == held.sourceFrames[0] &&
                    settled.sourcePosition[0] == double(settled.sourceFrame[0]) && settled.stemGain == looping.stemGain &&
                    settled.deckGain == looping.deckGain && settled.eqGainDb == looping.eqGainDb &&
                    settled.cueFrame == looping.cueFrame && settled.cueSelected == looping.cueSelected &&
                    settled.masterGain == looping.masterGain && settled.crossfader == looping.crossfader &&
                    settled.sourceFrame[0]-held.sourceFrame[0] <= settled.outputFrame-looping.outputFrame,
                    "Automatic range-bound OUT takeover preserves finite F direction, exact geometry, controls and source metadata");
                for (const auto* key : {"name", "sourceId", "title", "artist", "album", "artworkDataUrl", "bpm", "waveform", "overviewStem"})
                    if (mapped[key] != originalMetadata[key]) throw std::runtime_error("Resident continuation changed original metadata");
                const auto expectedRetained = retainedBefore-initial["residentBytes"].toULongLong()+settled.sourceFrames[0]*32;
                await(evidence, [&] { return services.gone(0) && bound.groupGone() &&
                    controller.retainedPcmBytes() == expectedRetained; }, "Acquired old live/range ownership did not retire off-thread");
                controller.stopRecording();
                await(evidence, [&] { return !controller.recording()["active"].toBool(); }, "Resident continuation recording did not drain");
                const auto recording = controller.recording(); const auto frames = recording["writtenFrames"].toULongLong();
                insist(recording["status"] == "Complete" && recording["acceptedFrames"] == recording["writtenFrames"] &&
                    !recording["overflowFrames"].toULongLong(), "Resident takeover retains every accepted recorded frame");
                current(); report["wav"] = inspectStemRecording(wav, frames, frames, 1.F, false, bothBass); current();
                insist(report["wav"].toObject()["checked_prefix_or_tail_frames"].toVariant().toULongLong() == frames,
                    "Every stereo sample through live, held and finite OUT takeover matches the selected-bass oracle");
            } else {
                insist(settled.sourceGeneration[0] == held.sourceGeneration[0] &&
                    controller.decks()[0].toMap()["liveStems"].toBool() && !settled.residentHold[0].armed &&
                    !settled.residentHold[0].active && services.alive(0) &&
                    settled.starvedFrames[0] >= checking.starvedFrames[0] &&
                    settled.starvedFrames[0]-checking.starvedFrames[0] <= settled.outputFrame-checking.outputFrame,
                    "Cancel and seek-away/back never install a stale candidate; any intentional A buffering is output-bounded");
                // Cancel may reach a callback before Pause. That intentional
                // control interval is not continuous-A or zero-starvation proof.
                report["manual_cancel_starved_frames"] = qint64(settled.starvedFrames[0]-checking.starvedFrames[0]);
                report["manual_cancel_interval_output_frames"] = qint64(settled.outputFrame-checking.outputFrame);
                report["affected_deck_audio_scope"] = "Accepted Cancel/Pause/seeks may deliberately buffer A; peer and recorder remain strict.";
            }
            services.capture(); const auto all = [&](const char* key, const char* value) {
                return std::ranges::count_if(services.rows(), [&](const auto& row) { return row[key].toString() == value; });
            };
            insist(digest(exporterCalls) == seededCalls && services.size() == 1 && all("fixture", "started") == 1 &&
                all("command", "load") == 1 && all("fixture", "runtime") == 1,
                "Resident continuation adds no export, second live service, load or runtime");
            report["settled"] = snapshot(settled); report["continuation_final"] = QJsonObject::fromVariantMap(continuation());
            controller.shutdown(); evidence.inspectResources = true; evidence.lastOutput = UINT64_MAX; evidence.drain();
            const auto counters = handle->counters(); const auto stoppedAudio = controller.audioState();
            const auto finalRecording = controller.recording();
            insist(services.gone(0) && bound.groupGone() && counters.producerFinished && !counters.droppedRecords &&
                !counters.invalidClockReads && counters.attemptedRecords == evidence.ordinal &&
                !controller.busy() && !controller.outputReady() && controller.completedSessions() == sessions+1 &&
                !stoppedAudio.starvedFrames[1] && controller.completedStarvedFrames() == stoppedAudio.starvedFrames[0] &&
                (cancelLookup ? stoppedAudio.starvedFrames[0] == settled.starvedFrames[0] : !stoppedAudio.starvedFrames[0]) &&
                controller.completedReadFailures() == 0 && controller.completedRejected() == 0 && controller.completedInvalidBuffers() == 0 &&
                finalRecording["status"] == "Complete" && finalRecording["acceptedFrames"] == finalRecording["writtenFrames"] &&
                !finalRecording["overflowFrames"].toULongLong(),
                "Both outcomes retire exact owned jobs, drain every recorded frame and count observed health exactly once");
            report["passed"] = true; evidence.write(true); evidence.controller = nullptr;
            insist(QDir(cacheHome).removeRecursively() && !QFileInfo::exists(cacheHome), "Only retired resident continuation cache is removed");
            results.append(report); return;
        }
        // A genuine miss is checked once while buffering stays false; verify
        // unchanged native range/cursor/owner and real recorded/output progress,
        // not just an unavailable label that could hide a silent failed source.
        await(evidence, [&] { return continuation()["status"] == "Unavailable" &&
            !continuation()["pending"].toBool(); }, "Missing resident bank did not settle one cache-only attempt");
        const auto missed = controller.audioState(); const auto missedRecording = controller.recording()["acceptedFrames"].toULongLong();
        await(evidence, [&] { return controller.audioState().outputFrame >= missed.outputFrame+holdFrames &&
            controller.recording()["acceptedFrames"].toULongLong() >= missedRecording+holdFrames; },
            "Unchanged resident miss did not retain audible/recorded progress");
        const auto afterMiss = controller.audioState(); continuousPeer(missed, afterMiss);
        insist(continuation()["attempts"].toULongLong() == 1 && !continuation()["detail"].toString().isEmpty() &&
            continuation()["residentHoldRangeId"].toString() == initial["rangeId"].toString() &&
            afterMiss.residentHold[0].active && afterMiss.residentHold[0].armed && !afterMiss.buffering[0] &&
            afterMiss.sourceFrame[0] == held.sourceFrame[0] && afterMiss.sourceGeneration[0] == held.sourceGeneration[0] &&
            !afterMiss.starvedFrames[0] && !afterMiss.sourceReadError[0] && services.alive(0),
            "A missing bank never rearms from buffering=false or replaces the approved resident audio");
        const auto currentTicket = hold(); const auto commandsBeforeStale = controller.submittedCommands();
        for (const auto* key : {"outputSession", "rangeId", "sourceGeneration", "transportRevision"}) {
            auto stale = currentTicket; stale[key] = "18446744073709551616";
            insist(!cancel(stale) && !exit(stale), "Malformed/stale exact tickets cannot enqueue Cancel or Exit on a live hold");
        }
        insist(!controller.cancelResidentHold(0, QString(64, QLatin1Char('0')), currentTicket["outputSession"].toString(),
            currentTicket["rangeId"].toString(), currentTicket["sourceGeneration"].toString(), currentTicket["transportRevision"].toString()) &&
            controller.submittedCommands() == commandsBeforeStale, "Wrong SHA and stale hold tickets leave the native queue untouched");
        insist(exit(currentTicket), "Fresh exact ticket accepts same-owner resume-at-OUT intent");
        await(evidence, [&] { return controller.audioState().residentHold[0].exitPending; }, "Exit was not acknowledged while the service is held");
        QFile release(fixture.marker(".release"));
        insist(release.open(QIODevice::WriteOnly | QIODevice::NewOnly) && release.write("release\n") == 8 && release.flush(),
            "Only the existing owned service release marker permits recovery"); release.close();
        await(evidence, [&] { const auto s = controller.audioState();
            if (s.sourceFrame[0] < held.sourceFrame[0]) throw std::runtime_error("Exit rewound the parked linear F");
            return !s.residentHold[0].active && !s.residentHold[0].armed && s.sourceFrame[0] >= held.sourceFrame[0] + 1024; },
            "Approved hold did not resume from F after the unchanged live reserve", std::chrono::seconds(10));
        const auto resumed = controller.audioState(); continuousPeer(looping, resumed);
        insist(resumed.sourceGeneration[0] == held.sourceGeneration[0] &&
            resumed.sourcePosition[0] == double(resumed.sourceFrame[0]) &&
            resumed.sourceFrame[0] - held.sourceFrame[0] <= resumed.outputFrame - looping.outputFrame &&
            !resumed.sourceReadError[0] && !resumed.starvedFrames[0] &&
            hold()["status"] == "Cancelled" && controller.liveStemObservations(0) == handle && handle->setup().sourceToken == token &&
            services.size() == 1 && services.count(0, "command", "load") == 1 && services.count(0, "command", "seek") == 0,
            "Exit completes on the original source without staleArmed UI, replacement, extra load, seek or session");
        evidence.actions["resumed"] = snapshot(resumed);
        controller.stopRecording();
        await(evidence, [&] { return !controller.recording()["active"].toBool(); }, "Resident recording did not finalize");
        const auto recorded = controller.recording(); const auto frames = recorded["writtenFrames"].toULongLong();
        insist(recorded["status"] == "Complete" && recorded["acceptedFrames"] == recorded["writtenFrames"] &&
            !recorded["overflowFrames"].toULongLong(), "Every accepted resident master frame is finalized without overflow");
        current(); report["wav"] = inspectStemRecording(wav, frames, frames, 1.F, false, bothBass); current();
        insist(report["wav"].toObject()["checked_prefix_or_tail_frames"].toVariant().toULongLong() == frames,
            "Every recorded stereo frame across linear, held wraps and Exit matches the unchanged constantboth-bass oracle");
        auto wavProof = report["wav"].toObject();
        wavProof["scope"] = "Every recorded stereo frame checked against constant selected-bass A plus finite B at the existing1e-6 tolerance; no omitted frames.";
        wavProof["expected_left"] = bothBass[0]; wavProof["expected_right"] = bothBass[1];
        wavProof["unclassified_or_omitted_frames"] = 0; report["wav"] = wavProof;
        // Subsequent deliberate transport is outside that complete continuous WAV.
        controller.setPlaying(0, false);
        await(evidence, [&] { return !controller.audioState().playing[0]; }, "Pause before stale-arm test was not acknowledged");
        insist(arm(hold()), "A fresh paused ticket explicitly approves the next Play");
        const auto staleBeforeReplay = hold();
        controller.setPlaying(0, true);
        controller.seekSeconds(0, .25); controller.seekSeconds(0, 0.); controller.setPlaying(0, true);
        insist(hold()["status"] == "Cancelled", "Accepted seek/replay immediately revokes the old automatic arming action");
        await(evidence, [&] { const auto s = controller.audioState(); return s.playing[0] && !s.buffering[0] &&
            s.sourceFrame[0] >= 4096 && !s.residentHold[0].armed && !s.residentHold[0].active; },
            "Real callbacks did not settle replay without reusing the cancelled Arm");
        const auto beforeStaleArm = controller.submittedCommands();
        insist(!arm(staleBeforeReplay) && controller.submittedCommands() == beforeStaleArm,
            "Old revision cannot explicitly arm after seek-away/back replay");
        insist(arm(hold()), "Fresh playing ticket queues explicit rearm");
        await(evidence, [&] { return controller.audioState().residentHold[0].armed; }, "Explicit rearm was not acknowledged");
        const auto beforeCancel = hold(); insist(cancel(beforeCancel), "Fresh exact ticket queues resident cancellation");
        await(evidence, [&] { return !controller.audioState().residentHold[0].armed && !controller.audioState().residentHold[0].active; },
            "Explicit Cancel did not revoke the actual callback's authorization");
        controller.setPlaying(0, false); controller.setPlaying(1, false);
        await(evidence, [&] { const auto s = controller.audioState(); return !s.playing[0] && !s.playing[1]; },
            "Both exact owners did not pause before deterministic routing reconstruction");
        const auto oldSession = hold()["outputSession"].toString();
        const auto beforeRebuild = controller.audioState();
        controller.configureAudioRouting(left, right, {}, {});
        await(evidence, [&] {
            if (!controller.busy() && !controller.error().isEmpty()) throw std::runtime_error(controller.error().toStdString());
            return !controller.busy() && controller.outputReady();
        }, "Same-owner output reconstruction did not finish", std::chrono::seconds(15));
        const auto rebuilt = controller.audioState(); const auto rebuiltTicket = hold();
        insist(rebuilt.sourceFrame == beforeRebuild.sourceFrame && rebuilt.stemGain == beforeRebuild.stemGain &&
            !rebuilt.playing[0] && !rebuilt.playing[1] && rebuiltTicket["rangeId"] == initial["rangeId"] &&
            rebuiltTicket["outputSession"].toString() != oldSession && controller.completedSessions() == sessions + 1 &&
            controller.liveStemObservations(0) == handle && !rebuilt.residentHold[0].armed,
            "Output rebuild retains exact paused owners/range and revokes authorization with a new session ticket");
        auto staleSession = rebuiltTicket; staleSession["outputSession"] = oldSession;
        const auto beforeOldSession = controller.submittedCommands();
        insist(!arm(staleSession) && !cancel(staleSession) && !exit(staleSession) &&
            controller.submittedCommands() == beforeOldSession,
            "Old output-session ticket rejects even with current SHA/range/generation/revision");
        insist(arm(rebuiltTicket) && hold()["status"] == "NextPlay" && cancel(hold()),
            "Fresh rebuilt-session ticket can approve then cancel a purely unqueued nextPlay");
        services.capture();
        const auto rows = services.rows();
        const auto all = [&](const char* key, const char* value) { return std::ranges::count_if(rows,
            [&](const auto& row) { return row[key].toString() == value; }); };
        insist(services.size() == 1 && all("fixture", "started") == 1 && all("command", "load") == 1 &&
            all("fixture", "runtime") == 1 && !QFileInfo::exists(fixture.engine.exporterPath.chopped(3) + ".calls") &&
            controller.liveStemObservations(0) == handle && handle->setup().sourceToken == token,
            "All transport/rebuild actions retain one service/runtime/load with no hidden exporter or original-mix substitute");
        report["source_sha256"] = digest(a); report["peer_sha256"] = digest(b);
        report["children"] = services.identities(); report["rebuild"] = snapshot(rebuilt);
        report["same_owner_range"] = QJsonObject::fromVariantMap(rebuiltTicket);
        controller.shutdown(); evidence.inspectResources = true; evidence.lastOutput = UINT64_MAX; evidence.drain();
        const auto counters = handle->counters();
        insist(services.gone(0) && bound.groupGone() && counters.producerFinished && !counters.droppedRecords &&
            !counters.invalidClockReads && counters.attemptedRecords == evidence.ordinal &&
            !controller.busy() && !controller.outputReady() && controller.completedRejected() == 0 &&
            controller.completedInvalidBuffers() == 0,
            "Shutdown proves exact service/group/job cleanup, complete observations and zero native rejected/invalid commands");
        report["passed"] = true; evidence.write(true); evidence.controller = nullptr;
        insist(QDir(cacheHome).removeRecursively() && !QFileInfo::exists(cacheHome), "Only retired owned resident cache/library is removed");
        results.append(report);
    } catch (const std::exception& error) {
        report["failure_context"] = QJsonObject{{"audio", snapshot(controller.audioState())},
            {"recording", QJsonObject::fromVariantMap(controller.recording())}, {"actions", evidence.actions}};
        if (locked) gate.unlock();
        controller.shutdown(); evidence.inspectResources = true; evidence.lastOutput = UINT64_MAX;
        try { evidence.write(false, QString::fromUtf8(error.what())); } catch (...) {}
        evidence.controller = nullptr; report["passed"] = false; report["error"] = error.what();
        report["children"] = services.identities(); results.append(report); throw;
    }
}

enum class ContinuationCase { Takeover, Miss, Cancel, Stop, Foreground, ResidentHold, ResidentTakeover, ResidentCancel };
void continuationScenario(const QString& output, const QString& privateRoot,
                          ContinuationCase kind, QJsonArray& results)
{
    const auto name = QStringLiteral("continuation-") + (kind == ContinuationCase::Takeover ? "takeover" :
        kind == ContinuationCase::Miss ? "miss" : kind == ContinuationCase::Cancel ? "cancel" :
        kind == ContinuationCase::Stop ? "stop" : "foreground");
    const auto local = QDir(output).filePath(name), cacheHome = QDir(privateRoot).filePath(name);
    directory(local); directory(cacheHome);
    Evidence evidence{name, output, privateRoot};
    // A continuation lookup intentionally leaves busy() false. Avoid walking
    // directories concurrently with its atomic staging/retirement operations.
    // The same bounded scan runs after shutdown; file/cache admission limits
    // remain enforced throughout. These observations do not measure peak use.
    evidence.inspectResources = false;
    evidence.actions["filesystem_observations"] = "quiescent-only; not a peak measurement";
    QJsonObject report{{"scenario", name}, {"qualified", false},
        {"filesystem_observations", "quiescent-only"}};
    const auto a = QDir(local).filePath("live-a.wav"), b = QDir(local).filePath("prepared-b.wav");
    writeFixture(a, 8192, -4096, 12 * 44100, 44100);
    writeFixture(b, 4096, -2048, 12 * 48000, 48000);
    const auto fixture = conflux::test::makeStemServiceFixture(local, "gate-after-first");
    ServiceTrace services(fixture); // Retained exact-process safety outlives the controller.
    insist(qputenv("XDG_CACHE_HOME", QFile::encodeName(cacheHome)) &&
        qputenv("XDG_DATA_HOME", QFile::encodeName(QDir(cacheHome).filePath("data"))),
        "Continuation cache/library remain inside the isolated private owner");
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
    try { seedBank(b, cache, fixture.engine, evidence); }
    catch (const std::exception& error) {
        try { evidence.write(false, QString::fromUtf8(error.what())); } catch (...) {}
        throw;
    }
    conflux::DeckController::PreparationOptions options;
    options.stemEngine = fixture.engine; options.stemServicePath = fixture.service; options.limits = tightLimits();
    conflux::DeckController controller(options);
    evidence.controller = &controller;
    Sink sink("conflux-continuation-" + name.toStdString() + "-" + std::to_string(::getpid()));
    QLockFile gate(QDir(cache).filePath(".lock"));
    bool locked = false, stopped = false;
    try {
        QString left, right;
        privateLivePair(controller, sink, left, right, insist);
        controller.loadStemDeck(1, QUrl::fromLocalFile(b));
        await(evidence, [&] { return !controller.busy() && controller.outputReady() && controller.audioState().sourceFrames[1]; },
            "Prepared continuation peer did not install", std::chrono::seconds(15));
        insist(controller.error().isEmpty() && controller.preparation()["cacheHit"].toBool() &&
            !controller.liveStemObservations(1), "Peer starts as a genuine compatible complete stem bank");
        const auto exporterCalls = fixture.engine.exporterPath.chopped(3) + ".calls";
        const auto peerCalls = digest(exporterCalls);
        controller.loadStemDeck(0, QUrl::fromLocalFile(a));
        await(evidence, [&] {
            if (!controller.busy() && !controller.error().isEmpty()) throw std::runtime_error(controller.error().toStdString());
            return !controller.busy() && controller.outputReady() && controller.audioState().sourceFrames[0];
        }, "Genuine cache-miss continuation source did not install", std::chrono::seconds(20));
        evidence.live = controller.liveStemObservations(0); services.capture();
        insist(evidence.live && controller.decks()[0].toMap()["liveStems"].toBool() &&
            !controller.preparation()["cacheHit"].toBool() && digest(exporterCalls) == peerCalls &&
            services.size() == 1 && services.alive(0) && services.count(0, "command", "load") == 1 &&
            services.count(0, "fixture", "runtime") == 1,
            "A genuine miss installs one live owner before any matching A bank exists, without exporting A");
        BoundService bound(services.identities()[0].toObject());
        const auto handle = evidence.live; const auto token = handle->setup().sourceToken;
        for (int deck = 0; deck < 2; ++deck)
            for (int role = 0; role < 4; ++role) controller.setStemGain(deck, role, role == 1 ? 1.0 : 0.0);
        controller.setBpmOverride(0, 142.);
        controller.setPlaying(0, true);
        await(evidence, [&] { const auto s = controller.audioState(); return s.sourceFrame[0] >= 8192 &&
            s.stemGain[0] == bassOnly && s.stemGain[1] == bassOnly && !s.buffering[0]; },
            "Selected live bass did not settle before storing a nonzero cue");
        controller.setPlaying(0, false);
        await(evidence, [&] { return !controller.audioState().playing[0]; }, "Live pause did not settle before cache seeding");
        insist(controller.cuePress(0), "Paused live source accepts a nonzero cue before continuation");
        await(evidence, [&] { const auto s = controller.audioState(); return s.cueHeld[0] && s.cueFrame[0] >= 8192; },
            "Nonzero cue was not acknowledged before continuation");
        controller.cueRelease(0); controller.setCueSelected(0, true);
        await(evidence, [&] { const auto s = controller.audioState(); return !s.cueHeld[0] && s.cueSelected[0]; },
            "Cue release/selection did not settle before continuation");
        // This is explicit fixture setup after live installation, never a
        // product fallback export or a parallel second separator.
        if (kind != ContinuationCase::Miss && kind != ContinuationCase::Foreground)
            seedBank(a, cache, fixture.engine, evidence);
        const auto callsBeforeFailure = digest(exporterCalls);
        const auto descriptorMarker = fixture.engine.exporterPath.chopped(3) + ".prefix";
        if (kind == ContinuationCase::Miss)
            insist(QFile::remove(descriptorMarker), "Initial explicit fixture preparation ran the descriptor probe");
        const auto metadata = controller.decks()[0].toMap();
        const auto preparation = controller.preparation();
        const auto sessions = controller.completedSessions();
        const auto priorStarved = controller.completedStarvedFrames(), priorFailures = controller.completedReadFailures();
        const auto retainedBefore = controller.retainedPcmBytes();
        std::uint64_t retiredStarved = 0, retiredFailures = 0;
        controller.setPlaying(0, true); controller.setPlaying(1, true);
        await(evidence, [&] { const auto s = controller.audioState(); return s.playing[0] && s.playing[1] &&
            s.sourceFrame[1] >= holdFrames && !s.buffering[0] && !s.buffering[1]; },
            "Both selected-bass sources did not settle before the induced failure");
        const auto before = controller.audioState();
        insist(before.deckGain == std::array<float, 2>{1.F, 1.F} && before.masterGain == 1.F &&
            !before.sourceReadError[0] && !before.sourceReadErrorObserved[0],
            "Continuation recording starts with healthy unity-gain selected bass, not the full original mix");
        const auto wav = QDir(local).filePath("master.wav");
        if (kind == ContinuationCase::Takeover || kind == ContinuationCase::Foreground) {
            controller.startRecording(QUrl::fromLocalFile(wav));
            await(evidence, [&] { return controller.recording()["status"].toString() == "Recording" &&
                controller.recording()["acceptedFrames"].toULongLong() >= holdFrames; },
                "Continuation master recorder did not attach before failure");
        }
        if (kind != ContinuationCase::Miss) {
            insist(gate.tryLock(0), "Owned prepared-cache lock gates only the automatic lookup after setup");
            locked = true;
        }
        insist(services.alive(0) && services.count(0, "fixture", "complete") == 0,
            "Induced failure targets the exact incomplete active fixture service");
        bound.terminateUnexpectedly();
        evidence.actions["induced_exit"] = "SIGKILL through retained exact-process pidfd; no descendant fixture";
        const auto continuation = [&] { return controller.decks()[0].toMap()["continuation"].toMap(); };
        const auto status = [&] { return continuation()["status"].toString(); };
        await(evidence, [&] { const auto s = controller.audioState(); return s.sourceReadError[0] &&
            s.sourceReadErrorObserved[0] && s.buffering[0] &&
            status() == (kind == ContinuationCase::Miss ? "Unavailable" : "Checking"); },
            "Terminal live failure did not expose the expected automatic cache-only continuation status");
        const auto held = controller.audioState();
        const auto captured = continuation();
        evidence.actions["before"] = snapshot(before); evidence.actions["held"] = snapshot(held);
        evidence.actions["continuation_capture"] = QJsonObject::fromVariantMap(captured);
        insist(captured["attempts"].toULongLong() == 1 &&
            captured["sourceFrame"].toULongLong() == held.sourceFrame[0] &&
            captured["sourceGeneration"].toULongLong() == held.sourceGeneration[0] &&
            held.sourcePosition[0] == double(held.sourceFrame[0]) && !controller.busy() &&
            !controller.decks()[0].toMap()["replacing"].toBool() &&
            controller.preparation()["status"] == preparation["status"] &&
            controller.preparation()["cacheHit"] == preparation["cacheHit"],
            "Automatic lookup captures the exact linear cursor/generation without becoming a normal load");
        if (kind == ContinuationCase::Takeover) {
            const auto recordingAtHold = controller.recording()["acceptedFrames"].toULongLong();
            await(evidence, [&] { const auto s = controller.audioState();
                if (s.sourceFrame[0] != held.sourceFrame[0]) throw std::runtime_error("Locked continuation advanced the failed source cursor");
                return s.starvedFrames[0] >= held.starvedFrames[0] + holdFrames + 512 &&
                    controller.recording()["acceptedFrames"].toULongLong() >= recordingAtHold + holdFrames + 512;
            }, "Selected peer-only interval did not persist before automatic recovery");
            gate.unlock(); locked = false;
            // Source advance includes the availability fade. Record that fade
            // plus the oracle's 1024 fully recovered frames before detach.
            await(evidence, [&] { const auto s = controller.audioState(); return status() == "Active" &&
                !continuation()["pending"].toBool() && s.sourceGeneration[0] != held.sourceGeneration[0] &&
                !s.buffering[0] && s.sourceFrame[0] >= held.sourceFrame[0] + recordingRampFrames + 1024; },
                "Compatible prepared bank did not continue automatically at the captured source cursor");
            const auto recovered = controller.audioState(); const auto recoveredMap = controller.decks()[0].toMap();
            continuousPeer(before, recovered); continuousPeer(held, recovered);
            insist(recovered.sourceGeneration[0] == continuation()["generation"].toULongLong() &&
                recovered.sourceFrames[0] == held.sourceFrames[0] &&
                recovered.sourceFrame[0] <= held.sourceFrame[0] + recovered.outputFrame - held.outputFrame &&
                recovered.sourcePosition[0] == double(recovered.sourceFrame[0]) &&
                recovered.transportRevision[0] == held.transportRevision[0] + 1 && recovered.playing[0] &&
                recovered.stemBank[0] && recovered.stemGain == before.stemGain &&
                recovered.deckGain == before.deckGain && recovered.masterGain == before.masterGain &&
                recovered.eqGainDb == before.eqGainDb && recovered.crossfader == before.crossfader &&
                recovered.cueFrame == before.cueFrame && recovered.cueSelected == before.cueSelected &&
                !recovered.sourceReadError[0] && recovered.sourceReadErrorObserved[0] &&
                recovered.starvedFrames[0] >= held.starvedFrames[0] && !recoveredMap["liveStems"].toBool() &&
                controller.retainedPcmBytes() == retainedBefore + held.sourceFrames[0] * 8 * 4 &&
                controller.completedSessions() == sessions,
                "Automatic finite owner preserves cursor direction, exact geometry, controls and cumulative health without session restart");
            for (const auto* key : {"name", "sourceId", "title", "artist", "album", "artworkDataUrl", "bpm", "waveform", "overviewStem"})
                if (recoveredMap[key] != metadata[key]) throw std::runtime_error("Continuation reset original source metadata or waveform");
            insist(controller.liveStemObservations(0) == handle && handle->setup().sourceToken == token &&
                recoveredMap["bpm"].toMap()["manual"].toBool() && recoveredMap["bpm"].toMap()["reference"].toDouble() == 142. &&
                controller.recording()["active"].toBool() && controller.recording()["path"].toString() == wav,
                "Original observation identity, manual BPM, source metadata and the same recorder survive takeover");
            evidence.actions["recovered"] = snapshot(recovered);
            controller.stopRecording();
            await(evidence, [&] { return !controller.recording()["active"].toBool(); }, "Continuation recording did not finalize");
            const auto recording = controller.recording();
            insist(recording["status"].toString() == "Complete" && recording["acceptedFrames"] == recording["writtenFrames"] &&
                recording["overflowFrames"].toULongLong() == 0,
                "Automatic takeover retains every accepted recorded frame without writer overflow");
            current(); report["wav"] = inspectCompleteRecording(wav, recording["writtenFrames"].toULongLong(), true); current();
            // Test commands against the new finite owner only after the exact
            // recording oracle is complete, so controls do not contaminate it.
            controller.setPlaying(0, false);
            await(evidence, [&] { return !controller.audioState().playing[0]; }, "Continued finite owner did not accept Pause");
            controller.seekSeconds(0, 1.);
            await(evidence, [&] { return controller.audioState().sourceFrame[0] == 48000; }, "Continued finite owner did not accept seek");
            insist(controller.cuePress(0), "Continued finite owner accepts storing a new cue");
            await(evidence, [&] { return controller.audioState().cueFrame[0] == 48000 && controller.audioState().cueHeld[0]; },
                "Continued finite owner did not store the exact new cue");
            controller.cueRelease(0); controller.setStemGain(0, 1, .5);
            await(evidence, [&] { const auto s = controller.audioState(); return !s.cueHeld[0] && s.stemGain[0][1] == .5F; },
                "Continued finite owner did not accept release and manual stem gain");
        } else if (kind == ContinuationCase::Miss) {
            insist(!QFileInfo::exists(descriptorMarker),
                "Automatic missing-bank continuation did not restart the Python descriptor during live playback");
            await(evidence, [&] { return controller.audioState().outputFrame >= held.outputFrame + holdFrames; },
                "Prepared peer did not continue while the matching bank was unavailable");
            const auto unavailable = controller.audioState(); continuousPeer(before, unavailable);
            insist(status() == "Unavailable" && !continuation()["detail"].toString().isEmpty() &&
                continuation()["attempts"].toULongLong() == 1 && !continuation()["pending"].toBool() &&
                unavailable.sourceGeneration[0] == held.sourceGeneration[0] &&
                unavailable.sourceFrame[0] == held.sourceFrame[0] && unavailable.buffering[0] &&
                controller.decks()[0].toMap()["liveStems"].toBool() && controller.liveStemObservations(0) == handle,
                "Genuine bank miss stays unavailable once per unchanged episode without original-mix substitution");
        } else if (kind == ContinuationCase::Cancel) {
            controller.setPlaying(0, false);
            controller.seekSeconds(0, (held.sourceFrame[0] + 2048) / 48000.);
            controller.seekSeconds(0, held.sourceFrame[0] / 48000.);
            gate.unlock(); locked = false;
            await(evidence, [&] { const auto s = controller.audioState(); return status() == "Cancelled" &&
                !continuation()["pending"].toBool() && !s.playing[0] && s.sourceFrame[0] == held.sourceFrame[0] &&
                s.transportRevision[0] > held.transportRevision[0]; },
                "Pause and seek-away/back did not cancel and retire the stale automatic candidate");
            const auto cancelled = controller.audioState(); continuousPeer(before, cancelled);
            insist(cancelled.sourceGeneration[0] == held.sourceGeneration[0] &&
                controller.liveStemObservations(0) == handle && controller.decks()[0].toMap()["liveStems"].toBool() &&
                continuation()["attempts"].toULongLong() == 1 && controller.completedSessions() == sessions,
                "An equal numeric seek destination cannot revive a stale captured transport intent or install a new owner");
        } else if (kind == ContinuationCase::Foreground) {
            const auto failedFrame = held.sourceFrame[0];
            // Structural validation must precede cancellation/reservation. This
            // URL is never opened and needs no network access.
            controller.loadStemDeck(0, QUrl(QStringLiteral("https://invalid.invalid/track.wav")));
            insist(!controller.error().isEmpty() && !controller.busy() && status() == "Checking" &&
                continuation()["pending"].toBool() && continuation()["attempts"].toULongLong() == 1 &&
                controller.audioState().sourceGeneration[0] == held.sourceGeneration[0],
                "An invalid foreground URL does not cancel or reserve the in-flight continuation");
            controller.loadStemDeck(0, QUrl::fromLocalFile(b));
            const auto firstPending = controller.decks()[0].toMap();
            insist(controller.busy() && controller.error().isEmpty() &&
                controller.preparation()["status"].toString() == "Preparing" &&
                controller.preparation()["phase"].toString() == "continuation-cleanup" &&
                controller.preparation()["deck"].toInt() == 0 && firstPending["loading"].toBool() &&
                firstPending["pendingPath"].toString() == b &&
                firstPending["pendingName"].toString() == QFileInfo(b).fileName() &&
                status() == "Cancelled" && continuation()["pending"].toBool(),
                "A single valid foreground load is accepted immediately while background cleanup owns the loader");
            insist(controller.cancelPreparation(), "The reserved foreground load accepts cancellation while waiting");
            // Keep the cache lock held: cancellation must consume the checking
            // worker's empty result and settle even without an owner to retire.
            await(evidence, [&] { return !controller.busy() &&
                controller.preparation()["status"].toString() == "Cancelled" &&
                status() == "Cancelled" && !continuation()["pending"].toBool(); },
                "Cancelled queued foreground load did not settle through the empty-result cleanup path");
            const auto cancelled = controller.audioState();
            continuousPeer(before, cancelled);
            insist(controller.error().isEmpty() && cancelled.sourceGeneration[0] == held.sourceGeneration[0] &&
                cancelled.sourceFrame[0] == failedFrame && controller.liveStemObservations(0) == handle &&
                controller.decks()[0].toMap()["sourceId"] == metadata["sourceId"] &&
                controller.decks()[0].toMap()["liveStems"].toBool() &&
                controller.completedStarvedFrames() == priorStarved && controller.completedReadFailures() == priorFailures,
                "Cancelling the queued load preserves the original failed owner without installing or retiring it");
            evidence.actions["cancelled_foreground"] = snapshot(cancelled);
            controller.setPlaying(0, false);
            await(evidence, [&] { return !controller.audioState().playing[0]; },
                "Deliberate Pause did not end the first cancelled continuation episode");
            controller.setPlaying(0, true);
            await(evidence, [&] { return status() == "Checking" && continuation()["pending"].toBool() &&
                continuation()["attempts"].toULongLong() == 2 && controller.audioState().playing[0]; },
                "A fresh accepted transport intent did not rearm a second checking episode");
            const auto secondHeld = controller.audioState();
            insist(secondHeld.sourceGeneration[0] == held.sourceGeneration[0] &&
                secondHeld.sourceFrame[0] == failedFrame && secondHeld.transportRevision[0] > held.transportRevision[0],
                "Rearming changes transport intent without rewinding or manufacturing a new live owner");
            const auto recordingAtHold = controller.recording()["acceptedFrames"].toULongLong();
            await(evidence, [&] { const auto s = controller.audioState(); return
                s.outputFrame >= secondHeld.outputFrame + holdFrames + 512 &&
                controller.recording()["acceptedFrames"].toULongLong() >= recordingAtHold + holdFrames + 512; },
                "Foreground regression did not retain a complete peer-only recorded interval");
            controller.loadStemDeck(0, QUrl::fromLocalFile(b)); // Exactly one call; no retry after cleanup.
            insist(controller.busy() && controller.error().isEmpty() &&
                controller.preparation()["status"].toString() == "Preparing" &&
                controller.preparation()["phase"].toString() == "continuation-cleanup" &&
                controller.decks()[0].toMap()["pendingPath"].toString() == b &&
                status() == "Cancelled" && continuation()["pending"].toBool(),
                "The second foreground request is reserved once with normal pending metadata and no retry error");
            gate.unlock(); locked = false;
            await(evidence, [&] { const auto s = controller.audioState();
                if (!controller.busy() && !controller.error().isEmpty()) throw std::runtime_error(controller.error().toStdString());
                return !controller.busy() && s.sourceGeneration[0] != held.sourceGeneration[0] &&
                    !controller.liveStemObservations(0) && services.gone(0);
            }, "Accepted foreground request was lost instead of loading its prepared B bank", std::chrono::seconds(15));
            const auto replaced = controller.audioState(); const auto replacedMap = controller.decks()[0].toMap();
            continuousPeer(before, replaced); continuousPeer(secondHeld, replaced);
            insist(controller.outputReady() && controller.error().isEmpty() &&
                controller.preparation()["status"].toString() == "Complete" && controller.preparation()["cacheHit"].toBool() &&
                replaced.stemBank[0] && !replaced.playing[0] && replaced.sourceFrame[0] == 0 &&
                !replaced.sourceReadError[0] && !replaced.sourceReadErrorObserved[0] &&
                !replaced.buffering[0] && replaced.starvedFrames[0] == 0 &&
                replacedMap["sourceId"].toString() == digest(b) &&
                replacedMap["name"].toString() == QFileInfo(b).fileName() &&
                !replacedMap["liveStems"].toBool() && replacedMap["overviewStem"].toString() == "drums" &&
                !replacedMap["bpm"].toMap()["manual"].toBool() && !replaced.cueFrame[0] &&
                controller.completedSessions() == sessions && bound.groupGone(),
                "Accepted foreground load completes once as an ordinary paused B-bank replacement with correct new metadata");
            // Ordinary replacement intentionally retires A's historical totals;
            // unlike continuation, they now belong to completed accounting.
            insist(controller.completedStarvedFrames() >= priorStarved + secondHeld.starvedFrames[0] &&
                controller.completedStarvedFrames() <= priorStarved + secondHeld.starvedFrames[0] +
                    replaced.outputFrame - secondHeld.outputFrame &&
                controller.completedReadFailures() == priorFailures + 1,
                "Normal foreground replacement retains the failed generation's bounded historical starvation and one causal failure");
            retiredStarved = controller.completedStarvedFrames() - priorStarved;
            retiredFailures = 1;
            evidence.actions["retired_starved_frames"] = qint64(retiredStarved);
            evidence.actions["retired_read_failures"] = qint64(retiredFailures);
            evidence.actions["foreground_replacement"] = snapshot(replaced);
            insist(controller.recording()["active"].toBool() && controller.recording()["path"].toString() == wav,
                "Foreground cancellation and replacement preserve the same master recorder");
            await(evidence, [&] { return controller.audioState().outputFrame >= replaced.outputFrame + holdFrames; },
                "Prepared peer did not continue after the ordinary foreground replacement");
            continuousPeer(replaced, controller.audioState());
            controller.stopRecording();
            await(evidence, [&] { return !controller.recording()["active"].toBool(); }, "Foreground recording did not finalize");
            const auto recording = controller.recording();
            insist(recording["status"].toString() == "Complete" && recording["acceptedFrames"] == recording["writtenFrames"] &&
                recording["overflowFrames"].toULongLong() == 0,
                "Foreground priority preserves every accepted recorded frame without writer overflow");
            current(); report["wav"] = inspectCompleteRecording(wav, recording["writtenFrames"].toULongLong(), false); current();
        } else {
            controller.loadStemDeck(0, QUrl::fromLocalFile(b));
            insist(controller.busy() && controller.error().isEmpty() &&
                controller.preparation()["phase"].toString() == "continuation-cleanup" &&
                status() == "Cancelled" && continuation()["pending"].toBool(),
                "Stop case first reserves a foreground request behind the checking worker");
            // Release before teardown: the worker may finish, but no GUI result
            // delivery occurs between unlock and shutdown. Cleanup itself must
            // remain able to acquire this same cache lock.
            gate.unlock(); locked = false;
            controller.shutdown(); stopped = true;
            insist(controller.audioState().sourceGeneration[0] == held.sourceGeneration[0] &&
                !continuation()["pending"].toBool(),
                "Shutdown consumes or retires an in-flight result without publishing its candidate");
        }
        services.capture();
        const auto traceRows = services.rows();
        const auto allTraceCount = [&](const char* key, const char* value) {
            return std::ranges::count_if(traceRows, [&](const auto& row) { return row[key].toString() == value; });
        };
        // Count every retained trace row, including an unexpected service that
        // already exited before capture could bind a pidfd for it.
        insist(digest(exporterCalls) == callsBeforeFailure && services.size() == 1 &&
            allTraceCount("fixture", "started") == 1 && allTraceCount("command", "load") == 1 &&
            allTraceCount("fixture", "runtime") == 1 && allTraceCount("fixture", "complete") == 0,
            "Automatic continuation adds no export, second live load, runtime or hidden full-track completion");
        if (!stopped) controller.shutdown();
        evidence.inspectResources = true; evidence.lastOutput = UINT64_MAX; evidence.drain();
        const auto final = controller.audioState(); const auto counters = handle->counters();
        insist(services.gone(0) && bound.groupGone() && counters.producerFinished && !counters.droppedRecords &&
            !counters.invalidClockReads && counters.attemptedRecords == evidence.ordinal &&
            !controller.busy() && !controller.outputReady(),
            "Every continuation outcome retires the exact service/group/job and drains all local observations before safety cleanup");
        insist(controller.completedSessions() == sessions + 1 &&
            controller.completedStarvedFrames() == priorStarved + retiredStarved + final.starvedFrames[0] + final.starvedFrames[1] &&
            controller.completedReadFailures() == priorFailures + retiredFailures +
                std::uint64_t(final.sourceReadErrorObserved[0] || final.sourceReadError[0]) +
                std::uint64_t(final.sourceReadErrorObserved[1] || final.sourceReadError[1]),
            "Continuation accounting counts the stopped session and cumulative failure/starvation exactly once");
        report["source_sha256"] = digest(a); report["peer_sha256"] = digest(b);
        report["children"] = services.identities(); report["final_audio"] = snapshot(final);
        report["continuation_final"] = QJsonObject::fromVariantMap(continuation()); report["passed"] = true;
        evidence.write(true); evidence.controller = nullptr;
        insist(QDir(cacheHome).removeRecursively() && !QFileInfo::exists(cacheHome),
            "Only the fully retired continuation scenario cache is removed");
        results.append(report);
    } catch (const std::exception& error) {
        if (locked) gate.unlock(); // Never obstruct product cleanup with the fixture's gate.
        controller.shutdown();
        evidence.inspectResources = true; evidence.lastOutput = UINT64_MAX;
        try { evidence.write(false, QString::fromUtf8(error.what())); } catch (...) {}
        evidence.controller = nullptr;
        report["passed"] = false; report["error"] = error.what(); report["children"] = services.identities();
        results.append(report); throw;
    }
}

int run(int argc, char** argv, bool functionalOnly, std::optional<ContinuationCase> continuationCase = {})
{
    const bool continuationOnly = continuationCase.has_value();
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
        const auto privateRoot = QString::fromStdString((runtime / (continuationOnly ? "continuation-owner" : "live-stall-owner")).string());
        directory(privateRoot);
        pw_init(nullptr, nullptr);
        if (continuationOnly) {
            if (*continuationCase == ContinuationCase::ResidentHold || *continuationCase == ContinuationCase::ResidentTakeover ||
                *continuationCase == ContinuationCase::ResidentCancel)
                residentScenario(QString::fromStdString(output.string()), privateRoot, results,
                    *continuationCase == ContinuationCase::ResidentTakeover ? "takeover" :
                    *continuationCase == ContinuationCase::ResidentCancel ? "cancel" : "");
            else continuationScenario(QString::fromStdString(output.string()), privateRoot, *continuationCase, results);
        } else {
            scenario(QString::fromStdString(output.string()), privateRoot, false, results);
            scenario(QString::fromStdString(output.string()), privateRoot, true, results);
        }
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
        {"scope", continuationCase == ContinuationCase::ResidentHold ?
            "Generated virtual controller/callback resident hold and exact complete master PCM; no queue-full injection, physical/model/GPU, musical, sustained or latency qualification." : continuationOnly ?
            "Generated virtual same-deck prepared-bank continuation only. Filesystem observations are quiescent-only, not peak measurements; enforced file/cache admission bounds remain active. No physical/model/GPU, sustained, musical, hard-cancel or4095ms latency qualification." :
            "Generated virtual controller/recorder support check only. Explicit replacement remains paused. No physical/model/GPU, sustained, musical, hard-cancel or4095ms latency qualification."}};
    const auto bytes = QJsonDocument(final).toJson(QJsonDocument::Indented);
    if (bytes.size() > 8 * qint64(MiB)) return 1;
    std::cout << bytes.constData();
    return passed ? 0 : 1;
}

} // namespace liveFailure

int uiControlsFixture(int argc, char** argv, const QString& binary, const QString& expectedSha, bool functionalOnly, bool normalRedraw)
{
    const auto began = Clock::now(), deadline = began + std::chrono::seconds(75);
    QCoreApplication app(argc, argv);
    QJsonObject report{{"passed", false}, {"qualified", false}, {"functional_only", true}, {"owner_seconds", 75},
        {"scope", "Private Dummy actual Qt accessible command/ACK path; fake producer telemetry drops seven by design. No host/model/GPU/service-latency, pointer/touch or physical cue qualification."}};
    QString failure, childOutput;
    bool captureComplete = true, childStarted = false;
    std::array<qint64, 2> captureBytes{};
    std::array<std::unique_ptr<QFile>, 2> logs;
    std::unique_ptr<QTemporaryDir> fixtures;
    std::unique_ptr<ServiceTrace> services;
    std::unique_ptr<Sink> sink;
    QProcess child;
    const auto fail = [&](const QString& message) { if (failure.isEmpty()) failure = message; };
    const auto drain = [&] {
        for (std::size_t index = 0; index < 2; ++index) {
            child.setReadChannel(index ? QProcess::StandardError : QProcess::StandardOutput);
            while (child.bytesAvailable()) {
                const auto data = child.read(65536);
                if (data.isEmpty()) break;
                if (captureBytes[index] + data.size() > 256 * 1024) {
                    captureComplete = false; fail("Owned soak child capture exceeded256KiB per stream");
                } else if (!logs[index] || logs[index]->write(data) != data.size()) {
                    captureComplete = false; fail("Cannot retain owned soak child output");
                }
                captureBytes[index] += data.size();
            }
        }
    };
    const auto observe = [&] {
        drain();
        if (services) services->capture();
    };
    const auto pumpUntil = [&](Clock::time_point until) {
        while (child.state() != QProcess::NotRunning && Clock::now() < until) {
            (void)child.waitForFinished(10);
            try { observe(); } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
        }
    };
    // Every wait is clamped to the original owner deadline; reserve two seconds
    // for the existing pidfd fixture fallback after this direct child is reaped.
    const auto cleanup = [&] {
        if (child.state() != QProcess::NotRunning) {
            QFile stop(childOutput + "/stop");
            if (!stop.open(QIODevice::WriteOnly) || stop.write("stop\n") != 5) fail("Cannot request graceful owned soak cancellation");
            stop.close();
            pumpUntil(std::min(Clock::now() + std::chrono::seconds(1), deadline - std::chrono::seconds(5)));
            if (child.state() != QProcess::NotRunning) {
                child.terminate();
                pumpUntil(std::min(Clock::now() + std::chrono::seconds(1), deadline - std::chrono::seconds(4)));
            }
            if (child.state() != QProcess::NotRunning) {
                child.kill();
                pumpUntil(std::min(Clock::now() + std::chrono::seconds(2), deadline - std::chrono::seconds(2)));
            }
        }
        try { observe(); } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
        for (auto& log : logs) if (log && (!log->flush() || log->error() != QFileDevice::NoError)) {
            captureComplete = false; fail("Owned soak child log finalization failed");
        }
        if (child.state() != QProcess::NotRunning) fail("Owned soak child did not reap within original owner deadline");
    };
    try {
        if (!functionalOnly) throw std::runtime_error("UI controls fixture requires --functional-only");
        validatePrivate(std::filesystem::path(argv[1]), std::filesystem::path(argv[2]));
        const rlimit noCore{0, 0}, fileLimit{liveFailure::fileBytesLimit, liveFailure::fileBytesLimit};
        if (::setrlimit(RLIMIT_CORE, &noCore) != 0 || ::setrlimit(RLIMIT_FSIZE, &fileLimit) != 0 || ::prctl(PR_SET_DUMPABLE, 0) != 0)
            throw std::runtime_error("Owned UI fixture core/file limits failed");
        const QFileInfo executable(binary);
        if (!executable.isAbsolute() || !executable.isFile() || !executable.isExecutable() || executable.isSymLink() ||
            expectedSha.size() != 64 || liveFailure::digest(binary) != expectedSha)
            throw std::runtime_error("Explicit regular executable soak binary/hash differs");
        report["binary"] = binary; report["binary_sha256"] = expectedSha;
        liveFailure::ownerDeadline = deadline; liveFailure::stopOwner = 0;
        std::signal(SIGINT, liveFailure::requestStop); std::signal(SIGTERM, liveFailure::requestStop);
        app.setOrganizationName("CONFLUX"); app.setApplicationName("ui-controls-private-check");
        fixtures = std::make_unique<QTemporaryDir>(QString::fromLocal8Bit(argv[1]) + "/ui-controls-XXXXXX");
        if (!fixtures->isValid()) throw std::runtime_error("Cannot create owned UI controls fixtures");
        const auto source = fixtures->filePath("generated-a.wav"), peer = fixtures->filePath("generated-b.wav");
        writeFixture(source, 2048, -1024, 3 * 44100, 44100);
        writeFixture(peer, 1024, -512, 30 * 48000);
        const auto fixture = conflux::test::makeStemServiceFixture(fixtures->path());
        services = std::make_unique<ServiceTrace>(fixture);
        pw_init(nullptr, nullptr);
        sink = std::make_unique<Sink>("conflux-short-live-test-sink"); // Routing endpoint only; never arm the unrelated fixed-PCM oracle.
        childOutput = QString::fromLocal8Bit(argv[2]) + "/ui-controls";
        liveFailure::directory(childOutput); liveFailure::directory(childOutput + "/cache");
        for (std::size_t index = 0; index < 2; ++index) {
            logs[index] = std::make_unique<QFile>(childOutput + (index ? "/child.stderr.log" : "/child.stdout.log"));
            if (!logs[index]->open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw std::runtime_error("Cannot create bounded child logs");
        }
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen"); environment.insert("QT_QUICK_BACKEND", "software");
        environment.insert("QT_QUICK_CONTROLS_STYLE", "Basic"); environment.insert("XDG_CACHE_HOME", childOutput + "/cache");
        environment.remove("CONFLUX_ALLOW_HOST_SOAK");
        child.setProcessEnvironment(environment); child.setProgram(binary);
        const QStringList arguments{normalRedraw ? "--live-stems-controls-normal-redraw" : "--live-stems-controls", source, peer, childOutput, "10", childOutput + "/stop",
            fixture.engine.pythonExecutable, fixture.engine.exporterPath, fixture.engine.workerPath, fixture.service,
            fixture.engine.runtimePath, fixture.engine.modelPath};
        child.setArguments(arguments); child.setProcessChannelMode(QProcess::SeparateChannels);
        report["arguments"] = QJsonArray::fromStringList(arguments);
        if (liveFailure::stopOwner || Clock::now() >= deadline - std::chrono::seconds(7))
            throw std::runtime_error("Private owner stopped before child dispatch");
        child.start(QIODevice::ReadOnly);
        if (!child.waitForStarted(1000)) throw std::runtime_error("Owned soak child failed to start");
        childStarted = true; report["child_pid"] = qint64(child.processId());
        while (child.state() != QProcess::NotRunning) {
            observe();
            if (!failure.isEmpty() || liveFailure::stopOwner || Clock::now() >= deadline - std::chrono::seconds(7))
                throw std::runtime_error("Private owner cancelled or original child budget expired");
            (void)child.waitForFinished(10);
        }
    } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
    cleanup();
    report["child_started"] = childStarted; report["child_reaped"] = child.state() == QProcess::NotRunning;
    report["capture_complete"] = captureComplete; report["capture_bytes"] = QJsonArray{captureBytes[0], captureBytes[1]};
    report["child_exit"] = childStarted && child.state() == QProcess::NotRunning ? QJsonValue(child.exitCode()) : QJsonValue();
    try {
        if (childStarted && (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0)) fail("Owned soak child failed");
        bool allGone = true;
        if (services) {
            services->capture(); report["services"] = services->identities();
            for (std::size_t index = 0; index < services->size(); ++index) allGone = allGone && services->gone(index);
        }
        report["service_cleanup_proven"] = allGone;
        if (!allGone) fail("Owned fake producer/job remains after child shutdown");
        if (allGone && child.state() == QProcess::NotRunning && !childOutput.isEmpty()) {
            const auto cache = childOutput + "/cache";
            report["cache_removed"] = !QFileInfo::exists(cache) || QDir(cache).removeRecursively();
            if (!report["cache_removed"].toBool()) fail("Owned private child cache did not remove after quiescence");
        }
        if (failure.isEmpty()) {
            QFile file(childOutput + "/native-result.json");
            if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) throw std::runtime_error("Bounded controls result missing");
            const auto document = QJsonDocument::fromJson(file.readAll()); const auto native = document.object();
            if (!document.isObject() || !native["passed"].toBool() || native["mode"].toString() != (normalRedraw ? "live-stems-controls-normal-redraw" : "live-stems-controls") ||
                native["forced_redraw_interval_ms"].toInt() != (normalRedraw ? 50 : 16) ||
                !native["functional_only"].toBool() || native["host_audio"].toBool() || native["qualified"].toBool() ||
                !native["manual_controls_complete"].toBool() || native["manual_controls"].toArray().size() != 16 ||
                !native["shutdown_completed"].toBool() || native["cancelled"].toBool())
                throw std::runtime_error("Functional actual-UI sequence/health/cleanup did not pass");
            if (!services || services->size() != 1 || liveFailure::digest(binary) != expectedSha)
                throw std::runtime_error("Producer count or launched binary pin changed");
            report["native_result"] = childOutput + "/native-result.json";
        }
    } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
    if (liveFailure::stopOwner || Clock::now() >= deadline) fail("Private owner interrupted or original75s deadline expired");
    report["passed"] = failure.isEmpty() && childStarted && captureComplete;
    report["error"] = failure; report["elapsed_seconds"] = std::chrono::duration<double>(Clock::now() - began).count();
    std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
    return report["passed"].toBool() ? 0 : 1;
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
        bool functionalOnly = false, liveFailureOnly = false;
        QString uiControlsBinary, uiControlsSha;
        bool uiControlsRequested = false, normalRedraw = false;
        std::optional<liveFailure::ContinuationCase> continuationCase;
        const auto usage = "Usage: preparation-controller-check PRIVATE_RUNTIME EMPTY_RECORDING_DIR [--functional-only] [--live-failure-only | --continuation-case {takeover,miss,cancel,stop,foreground,resident-hold,resident-takeover,resident-cancel} | --ui-controls-soak ABSOLUTE_BINARY SHA256]";
        if (argc < 3) throw std::runtime_error(usage);
        for (int argument = 3; argument < argc; ++argument) {
            const std::string_view option(argv[argument]);
            if (option == "--functional-only") functionalOnly = true;
            else if (option == "--ui-controls-soak" || option == "--ui-controls-soak-normal-redraw") {
                if (uiControlsRequested || argument + 2 >= argc) throw std::runtime_error(usage);
                uiControlsRequested = true;
                normalRedraw = option == "--ui-controls-soak-normal-redraw";
                uiControlsBinary = QString::fromLocal8Bit(argv[++argument]);
                uiControlsSha = QString::fromLocal8Bit(argv[++argument]);
            }
            else if (option == "--live-failure-only") liveFailureOnly = true;
            else if (option == "--continuation-case") {
                if (continuationCase || ++argument == argc) throw std::runtime_error(usage);
                const std::string_view value(argv[argument]);
                if (value == "takeover") continuationCase = liveFailure::ContinuationCase::Takeover;
                else if (value == "miss") continuationCase = liveFailure::ContinuationCase::Miss;
                else if (value == "cancel") continuationCase = liveFailure::ContinuationCase::Cancel;
                else if (value == "stop") continuationCase = liveFailure::ContinuationCase::Stop;
                else if (value == "foreground") continuationCase = liveFailure::ContinuationCase::Foreground;
                else if (value == "resident-hold") continuationCase = liveFailure::ContinuationCase::ResidentHold;
                else if (value == "resident-takeover") continuationCase = liveFailure::ContinuationCase::ResidentTakeover;
                else if (value == "resident-cancel") continuationCase = liveFailure::ContinuationCase::ResidentCancel;
                else throw std::runtime_error(usage);
            } else throw std::runtime_error(usage);
        }
        if (liveFailureOnly && continuationCase) throw std::runtime_error(usage);
        if (uiControlsRequested) {
            if (liveFailureOnly || continuationCase) throw std::runtime_error(usage);
            return uiControlsFixture(argc, argv, uiControlsBinary, uiControlsSha, functionalOnly, normalRedraw);
        }
        if (liveFailureOnly || continuationCase) return liveFailure::run(argc, argv, functionalOnly, continuationCase);
        // Gcov cold-cache throughput is outside functional load-latency qualification.
        // Only initial A readiness and cold B import setup receive this allowance.
        const auto setupTimeout = std::chrono::seconds(functionalOnly ? 30 : 8);
        const auto setupTimeoutMilliseconds = int(std::chrono::duration_cast<std::chrono::milliseconds>(setupTimeout).count());
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
        // Functional mode checks every rendered master frame in the owned WAV.
        // The downstream virtual graph's scheduling remains separate telemetry.
        const auto sinkMatches = [&] { return functionalOnly || (!sink.wrong() && !sink.zeros()); };
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
        try {
            wait([&] { return !controller.busy() && controller.outputReady(); },
                 "Initial prepared A output did not become ready", true, setupTimeout);
        } catch (const std::exception& failure) {
            const auto state = controller.audioState();
            observations["initial_readiness_failure"] = QJsonObject{
                {"wait_error", failure.what()}, {"wait_milliseconds", setupTimeoutMilliseconds}, {"busy", controller.busy()},
                {"output_ready", controller.outputReady()}, {"error", controller.error()},
                {"preparation", QJsonObject::fromVariantMap(controller.preparation())},
                {"decks", QJsonArray::fromVariantList(controller.decks())},
                {"completed_sessions", qint64(controller.completedSessions())},
                {"submitted_commands", qint64(controller.submittedCommands())},
                {"audio", QJsonObject{
                    {"ready", state.ready}, {"graph_rate", int(state.graphRate)},
                    {"output_frame", qint64(state.outputFrame)}, {"callbacks", qint64(state.callbacks)},
                    {"source_frames", QJsonArray{qint64(state.sourceFrame[0]), qint64(state.sourceFrame[1])}},
                    {"source_total_frames", QJsonArray{qint64(state.sourceFrames[0]), qint64(state.sourceFrames[1])}},
                    {"source_generations", QJsonArray{qint64(state.sourceGeneration[0]), qint64(state.sourceGeneration[1])}},
                    {"playing", QJsonArray{state.playing[0], state.playing[1]}},
                    {"buffering", QJsonArray{state.buffering[0], state.buffering[1]}},
                    {"source_read_error", QJsonArray{state.sourceReadError[0], state.sourceReadError[1]}},
                    {"source_read_error_observed", QJsonArray{state.sourceReadErrorObserved[0], state.sourceReadErrorObserved[1]}},
                    {"invalid_buffers", qint64(state.invalidBuffers)}, {"rejected", qint64(state.rejected)}}},
                {"scope", "Last published public controller state before failure unwind; no worker stack or precise internal-stage timing inference."}};
            throw;
        }
        require(controller.error().isEmpty() && controller.decks()[0].toMap()["loaded"].toBool(),
                "Generated A loads on the private output");
        controller.setPlaying(0, true);
        wait([&] { return controller.audioState().playing[0] && controller.audioState().sourceFrame[0] >= 4096; },
             "A transport did not advance");
        sink.validate(true);
        wait([&] { return sink.received(0) >= 1024 && sink.received(1) >= 1024; }, "Steady private samples did not arrive");
        require(sinkMatches(), "Steady A sink samples match outside functional-only mode");
        const auto preparationRecordingPath = QString::fromStdString((output / "generated-preparation-controller.wav").string());
        controller.startRecording(QUrl::fromLocalFile(preparationRecordingPath));
        wait([&] { return controller.recording()["status"].toString() == "Recording" &&
                          controller.recording()["acceptedFrames"].toULongLong() >= 1024; },
             "Steady A preparation recording did not attach before library work");
        require(controller.recording()["error"].toString().isEmpty() &&
                controller.recording()["overflowFrames"].toULongLong() == 0,
                "Owned master recording covers the steady A preparation phase");
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
                !gatedImport.sourceReadError[0] && !gatedImport.starvedFrames[0] && sinkMatches(),
                "GUI polling during pending import preserves the same healthy advancing A session");
        importGate.unlock();
        auto* const importPool = QThreadPool::globalInstance();
        const bool importDone = importPool->waitForDone(setupTimeoutMilliseconds);
        if (!importDone) {
            const auto state = controller.audioState();
            QJsonArray cacheFiles;
            const auto cacheRoot = std::filesystem::path(QFile::encodeName(
                QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("prepared/v1")).constData());
            std::error_code metadataError;
            std::filesystem::recursive_directory_iterator entry(cacheRoot,
                std::filesystem::directory_options::skip_permission_denied, metadataError), end;
            std::size_t visited = 0, metadataErrors = 0;
            for (; !metadataError && entry != end && visited < 64; entry.increment(metadataError)) {
                ++visited;
                struct stat status{};
                if (::lstat(entry->path().c_str(), &status)) {
                    ++metadataErrors;
                    entry.disable_recursion_pending();
                    continue;
                }
                if (S_ISLNK(status.st_mode)) {
                    entry.disable_recursion_pending();
                    continue;
                }
                if (!S_ISREG(status.st_mode)) continue;
                cacheFiles.append(QJsonObject{
                    {"relative_path", QString::fromStdString(entry->path().lexically_relative(cacheRoot).generic_string())},
                    {"bytes", qint64(status.st_size)}});
            }
            observations["queued_import_failure"] = QJsonObject{
                {"worker_pool_done", importDone}, {"wait_milliseconds", setupTimeoutMilliseconds},
                {"busy", controller.busy()}, {"output_ready", controller.outputReady()},
                {"error", controller.error()}, {"preparation", QJsonObject::fromVariantMap(controller.preparation())},
                {"pool_active_threads", importPool->activeThreadCount()}, {"pool_max_threads", importPool->maxThreadCount()},
                {"audio", QJsonObject{
                    {"output_frame", qint64(state.outputFrame)}, {"callbacks", qint64(state.callbacks)},
                    {"source_frames", QJsonArray{qint64(state.sourceFrame[0]), qint64(state.sourceFrame[1])}},
                    {"source_total_frames", QJsonArray{qint64(state.sourceFrames[0]), qint64(state.sourceFrames[1])}},
                    {"playing", QJsonArray{state.playing[0], state.playing[1]}},
                    {"source_read_error", QJsonArray{state.sourceReadError[0], state.sourceReadError[1]}},
                    {"source_read_error_observed", QJsonArray{state.sourceReadErrorObserved[0], state.sourceReadErrorObserved[1]}},
                    {"starved_frames", QJsonArray{qint64(state.starvedFrames[0]), qint64(state.starvedFrames[1])}}}},
                {"sink", QJsonObject{{"received_left", qint64(sink.received(0))},
                    {"received_right", qint64(sink.received(1))}, {"wrong", qint64(sink.wrong())}, {"zeros", qint64(sink.zeros())}}},
                {"cache_metadata", QJsonObject{{"regular_files", cacheFiles}, {"entries_visited", qint64(visited)},
                    {"entry_bound", 64}, {"truncated", entry != end && !metadataError},
                    {"status_errors", qint64(metadataErrors)}, {"iteration_error", QString::fromStdString(metadataError.message())}}},
                {"scope", "GUI completion remains withheld. Preparation/audio are last GUI-published snapshots; pool, sink and bounded cache metadata are separately sampled, not a coherent worker-stage or timing proof. No PCM contents read."}};
        }
        require(importDone, "B import preparation finishes while GUI completion remains queued");
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
                sinkMatches(),
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
                !imported.sourceReadError[0] && !imported.starvedFrames[0] && sinkMatches(),
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
                    cueImported.outputFrame - cueImportBefore.outputFrame && sinkMatches(),
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
        try {
            require(!cancelled.invalidBuffers && !cancelled.rejected &&
                    !cancelled.sourceReadErrorObserved[0] && !cancelled.sourceReadError[0] && !cancelled.starvedFrames[0] &&
                    !cancelled.sourceReadErrorObserved[1] && !cancelled.sourceReadError[1] && !cancelled.starvedFrames[1] &&
                    sinkMatches(), "Cancellation preserves exact A samples without invalid buffers or source failures");
        } catch (const std::exception& failure) {
            const auto sinkWrong = sink.wrong(), sinkZeros = sink.zeros();
            const std::array<std::uint64_t, 2> sinkReceived{sink.received(0), sink.received(1)};
            // Disarm before intentional shutdown, including serialization-failure unwind.
            sink.validate(false);
            try {
                const auto audio = [](const auto& state) {
                    auto value = liveFailure::snapshot(state);
                    value["max_execution_ns"] = qint64(state.maxExecutionNs);
                    value["deadline_misses"] = qint64(state.deadlineMisses);
                    value["graph_rate"] = int(state.graphRate);
                    value["stream_frames_min"] = qint64(state.streamFramesMin);
                    value["stream_frames_max"] = qint64(state.streamFramesMax);
                    return value;
                };
                QJsonObject detail{
                    {"assertion_error", failure.what()}, {"controller_error", controller.error()},
                    {"busy", controller.busy()}, {"output_ready", controller.outputReady()},
                    {"completed_sessions_before", qint64(sessions)},
                    {"completed_sessions_on_catch", qint64(controller.completedSessions())},
                    {"before_preparation", audio(before)}, {"after_cancellation", audio(cancelled)},
                    {"sink_received_on_catch", QJsonArray{qint64(sinkReceived[0]), qint64(sinkReceived[1])}},
                    {"sink_wrong_on_catch", qint64(sinkWrong)}, {"sink_zeros_on_catch", qint64(sinkZeros)},
                    {"clauses", QJsonObject{
                        {"invalid_buffers_zero", !cancelled.invalidBuffers}, {"rejected_zero", !cancelled.rejected},
                        {"A_source_read_error_observed_zero", !cancelled.sourceReadErrorObserved[0]},
                        {"A_source_read_error_zero", !cancelled.sourceReadError[0]},
                        {"A_starved_frames_zero", !cancelled.starvedFrames[0]},
                        {"B_source_read_error_observed_zero", !cancelled.sourceReadErrorObserved[1]},
                        {"B_source_read_error_zero", !cancelled.sourceReadError[1]},
                        {"B_starved_frames_zero", !cancelled.starvedFrames[1]},
                        {"sink_wrong_zero_on_catch", !sinkWrong}, {"sink_zeros_zero_on_catch", !sinkZeros}}},
                    {"scope", "Failure-only coherent stored before/after controller snapshots. Catch-time sink atomics are separate asynchronous reads, not the assertion's exact read instant. Received counts are per channel; wrong/zero sample counts sum both channels. No root-cause inference."}};
                observations["cancelled_preparation_failure"] = detail;
                controller.shutdown();
                QJsonArray stoppedTiming;
                for (const auto& timing : controller.completedTiming()) {
                    const auto& counters = timing.counters;
                    stoppedTiming.append(QJsonObject{
                        {"callbacks", qint64(counters.callbacks)}, {"max_execution_ns", qint64(counters.maxExecutionNs)},
                        {"deadline_misses", qint64(counters.deadlineMisses)},
                        {"driver_discontinuities", qint64(counters.graphDiscontinuities)},
                        {"driver_xrun_recovery_flags", qint64(counters.xrunRecoveryFlags)},
                        {"graph_quantum_min", qint64(counters.graphQuantumMin)},
                        {"graph_quantum_max", qint64(counters.graphQuantumMax)},
                        {"driver_position_observed", counters.graphPositionObserved},
                        {"capabilities", QJsonObject{{"cycle", timing.capabilities.cycle},
                            {"discontinuity_flag", qint64(timing.capabilities.discontinuityFlag)},
                            {"xrun_recovery_flag", qint64(timing.capabilities.xrunRecoveryFlag)}}}});
                }
                detail["completed_sessions_after_diagnostic_shutdown"] = qint64(controller.completedSessions());
                detail["stopped_timing"] = stoppedTiming;
                detail["stopped_timing_scope"] = "Quiesced totals include callbacks after the assertion. Execution timing does not measure scheduler lateness; unavailable driver flag masks do not establish zero events.";
                observations["cancelled_preparation_failure"] = detail;
            } catch (const std::exception& diagnostic) {
                try { observations["cancelled_preparation_diagnostic_error"] = QString::fromUtf8(diagnostic.what()); }
                catch (...) {} // Diagnostic failures must not replace the original assertion.
            } catch (...) {
                try { observations["cancelled_preparation_diagnostic_error"] = "Unknown diagnostic failure"; }
                catch (...) {}
            }
            throw;
        }
        observations["cancelled_at_output_frame"] = qint64(cancelled.outputFrame);
        controller.loadLibraryTrack(1, libraryId);
        wait([&] { return !controller.busy() && controller.decks()[1].toMap()["loaded"].toBool() && controller.outputReady(); },
             "B did not load for imported cue jumps");
        const auto cueJumpBefore = controller.audioState();
        const auto cueJumpSessionsBefore = controller.completedSessions();
        const auto cueJumpCommandsBefore = controller.submittedCommands();
        const std::array<std::uint64_t, 2> cueJumpSinkReceivedBefore{sink.received(0), sink.received(1)};
        const auto cueJumpSinkWrongBefore = sink.wrong(), cueJumpSinkZerosBefore = sink.zeros();
        require(controller.decks()[1].toMap()["sourceId"].toString() == libraryId &&
                !cueJumpBefore.playing[1] && !controller.jumpToImportedCue(0, libraryId, 0),
                "Imported cue jumps require the exact loaded source identity");
        require(controller.jumpToImportedCue(1, libraryId, 0), "First imported cue submits an exact-frame seek");
        wait([&] { return controller.audioState().sourceFrame[1] == 640; }, "First imported cue did not land at frame640");
        require(controller.jumpToImportedCue(1, libraryId, 1), "Second imported cue submits an exact-frame seek");
        wait([&] { return controller.audioState().sourceFrame[1] == 47360; }, "Second imported cue did not land at frame47360");
        const auto cueJumped = controller.audioState();
        try {
            require(!cueJumped.playing[1] && cueJumped.cueFrame[1] == cueJumpBefore.cueFrame[1] &&
                    cueJumped.playing[0] && cueJumped.sourceGeneration[0] == cueJumpBefore.sourceGeneration[0] &&
                    cueJumped.sourceFrame[0] - cueJumpBefore.sourceFrame[0] == cueJumped.outputFrame - cueJumpBefore.outputFrame &&
                    !cueJumped.invalidBuffers && !cueJumped.rejected && sinkMatches(),
                    "Exact cue jumps preserve paused B, its temporary CUE and the advancing peer clock/samples");
        } catch (const std::exception& failure) {
            const auto sinkWrong = sink.wrong(), sinkZeros = sink.zeros();
            const std::array<std::uint64_t, 2> sinkReceived{sink.received(0), sink.received(1)};
            const auto audio = [](const auto& state) {
                return QJsonObject{
                    {"output_frame", qint64(state.outputFrame)}, {"applied", qint64(state.applied)},
                    {"rejected", qint64(state.rejected)}, {"last_command_id", qint64(state.lastCommandId)},
                    {"callbacks", qint64(state.callbacks)}, {"invalid_buffers", qint64(state.invalidBuffers)},
                    {"max_execution_ns", qint64(state.maxExecutionNs)}, {"deadline_misses", qint64(state.deadlineMisses)},
                    {"graph_rate", int(state.graphRate)}, {"stream_frames_min", qint64(state.streamFramesMin)},
                    {"stream_frames_max", qint64(state.streamFramesMax)},
                    {"snapshot_drops", qint64(state.snapshotDrops)}, {"ready", state.ready},
                    {"source_frames", QJsonArray{qint64(state.sourceFrame[0]), qint64(state.sourceFrame[1])}},
                    {"source_total_frames", QJsonArray{qint64(state.sourceFrames[0]), qint64(state.sourceFrames[1])}},
                    {"source_generations", QJsonArray{qint64(state.sourceGeneration[0]), qint64(state.sourceGeneration[1])}},
                    {"playing", QJsonArray{state.playing[0], state.playing[1]}},
                    {"cue_frames", QJsonArray{qint64(state.cueFrame[0]), qint64(state.cueFrame[1])}},
                    {"cue_held", QJsonArray{state.cueHeld[0], state.cueHeld[1]}},
                    {"cue_preview", QJsonArray{state.cuePreview[0], state.cuePreview[1]}},
                    {"buffering", QJsonArray{state.buffering[0], state.buffering[1]}},
                    {"starved_frames", QJsonArray{qint64(state.starvedFrames[0]), qint64(state.starvedFrames[1])}},
                    {"source_read_error", QJsonArray{state.sourceReadError[0], state.sourceReadError[1]}},
                    {"source_read_error_observed", QJsonArray{state.sourceReadErrorObserved[0], state.sourceReadErrorObserved[1]}},
                    {"deck_gain", QJsonArray{state.deckGain[0], state.deckGain[1]}},
                    {"master_gain", state.masterGain}, {"crossfader", state.crossfader}};
            };
            observations["cue_jump_failure"] = QJsonObject{
                {"assertion_error", failure.what()}, {"controller_error", controller.error()},
                {"busy", controller.busy()}, {"output_ready", controller.outputReady()},
                {"completed_sessions_before", qint64(cueJumpSessionsBefore)},
                {"completed_sessions_after", qint64(controller.completedSessions())},
                {"submitted_commands_before", qint64(cueJumpCommandsBefore)},
                {"submitted_commands_after", qint64(controller.submittedCommands())},
                {"before_cue_jumps", audio(cueJumpBefore)}, {"after_cue_jumps", audio(cueJumped)},
                {"sink_received_before", QJsonArray{qint64(cueJumpSinkReceivedBefore[0]), qint64(cueJumpSinkReceivedBefore[1])}},
                {"sink_received_on_catch", QJsonArray{qint64(sinkReceived[0]), qint64(sinkReceived[1])}},
                {"sink_wrong_before", qint64(cueJumpSinkWrongBefore)}, {"sink_wrong_on_catch", qint64(sinkWrong)},
                {"sink_zeros_before", qint64(cueJumpSinkZerosBefore)}, {"sink_zeros_on_catch", qint64(sinkZeros)},
                {"clauses", QJsonObject{
                    {"B_paused", !cueJumped.playing[1]},
                    {"B_temporary_cue_unchanged", cueJumped.cueFrame[1] == cueJumpBefore.cueFrame[1]},
                    {"A_playing", cueJumped.playing[0]},
                    {"A_generation_unchanged", cueJumped.sourceGeneration[0] == cueJumpBefore.sourceGeneration[0]},
                    {"A_source_delta_equals_output_delta", cueJumped.sourceFrame[0] - cueJumpBefore.sourceFrame[0] ==
                        cueJumped.outputFrame - cueJumpBefore.outputFrame},
                    {"invalid_buffers_zero", !cueJumped.invalidBuffers}, {"rejected_zero", !cueJumped.rejected},
                    {"sink_wrong_zero_on_catch", !sinkWrong}, {"sink_zeros_zero_on_catch", !sinkZeros}}},
                {"scope", "Failure-only coherent before/after controller audio snapshots. Sink counters are separate asynchronous observations; catch-time sink clauses do not claim the assertion's exact read instant or a root cause."}};
            try {
                // Preserve the failing sample evidence before intentional cleanup silence.
                sink.validate(false);
                controller.shutdown();
                QJsonArray stoppedTiming;
                for (const auto& timing : controller.completedTiming()) {
                    const auto& counters = timing.counters;
                    stoppedTiming.append(QJsonObject{
                        {"callbacks", qint64(counters.callbacks)},
                        {"max_execution_ns", qint64(counters.maxExecutionNs)},
                        {"deadline_misses", qint64(counters.deadlineMisses)},
                        {"driver_discontinuities", qint64(counters.graphDiscontinuities)},
                        {"driver_xrun_recovery_flags", qint64(counters.xrunRecoveryFlags)},
                        {"graph_quantum_min", qint64(counters.graphQuantumMin)},
                        {"graph_quantum_max", qint64(counters.graphQuantumMax)},
                        {"driver_position_observed", counters.graphPositionObserved},
                        {"capabilities", QJsonObject{
                            {"cycle", timing.capabilities.cycle},
                            {"discontinuity_flag", qint64(timing.capabilities.discontinuityFlag)},
                            {"xrun_recovery_flag", qint64(timing.capabilities.xrunRecoveryFlag)}}}});
                }
                auto detail = observations["cue_jump_failure"].toObject();
                detail["completed_sessions_after_diagnostic_shutdown"] = qint64(controller.completedSessions());
                detail["stopped_timing"] = stoppedTiming;
                detail["stopped_timing_scope"] = "Quiesced totals for all completed sessions after failure-only shutdown, including callbacks after the assertion. Execution timing does not measure scheduler lateness; unavailable driver flag masks do not establish zero events.";
                observations["cue_jump_failure"] = detail;
            } catch (const std::exception& diagnostic) {
                try { observations["cue_jump_timing_diagnostic_error"] = QString::fromUtf8(diagnostic.what()); }
                catch (...) {} // Preserve the original assertion if diagnostic serialization also fails.
            } catch (...) {
                try { observations["cue_jump_timing_diagnostic_error"] = "Unknown failure while collecting stopped timing"; }
                catch (...) {}
            }
            throw;
        }
        observations["cue_jump"] = QJsonObject{{"landed_frame", qint64(cueJumped.sourceFrame[1])},
            {"b_paused", !cueJumped.playing[1]}, {"temporary_cue_unchanged", true}, {"peer_clock_unchanged", true}};
        // The imported-cue oracle above is complete. Persist the paused B's
        // native CUE while A and the existing exact sink check keep advancing.
        const auto importedMarkers = controller.libraryTracks()[0].toMap()["cues"].toList();
        require(controller.cuePress(1), "B CUE press was not accepted for saving");
        const auto cuePressCommands = controller.submittedCommands();
        require(!controller.saveDeckCue(1) && controller.submittedCommands() == cuePressCommands &&
                controller.error().contains("acknowledge"),
                "Unacknowledged CUE cannot be saved or issue another command");
        wait([&] { const auto state = controller.audioState();
            return state.cueHeld[1] && state.cueFrame[1] == 47360 && state.lastCommandId >= cuePressCommands;
        }, "B native CUE did not acknowledge frame47360");
        controller.cueRelease(1);
        wait([&] { const auto state = controller.audioState();
            return !state.cueHeld[1] && state.lastCommandId >= controller.submittedCommands();
        }, "B CUE release did not acknowledge before saving");
        const auto beforeCueSave = controller.audioState();
        const auto cueSaveCommands = controller.submittedCommands();
        require(controller.saveDeckCue(1) && controller.submittedCommands() == cueSaveCommands,
                "Saving native CUE must not submit an audio command");
        wait([&] { return !controller.busy() && controller.audioState().outputFrame > beforeCueSave.outputFrame; },
             "B saved CUE did not finish its atomic catalog write while output advanced");
        const auto afterCueSave = controller.audioState();
        const auto savedCue = controller.decks()[1].toMap()["savedCue"].toMap();
        require(controller.libraryError().isEmpty() && savedCue["frame"].toString() == "47360" &&
                savedCue["saved"].toBool() && controller.submittedCommands() == cueSaveCommands &&
                afterCueSave.playing[0] && !afterCueSave.playing[1] && afterCueSave.sourceFrame[1] == 47360 &&
                afterCueSave.cueFrame[1] == 47360 && afterCueSave.outputFrame > beforeCueSave.outputFrame &&
                afterCueSave.sourceFrame[0] - beforeCueSave.sourceFrame[0] == afterCueSave.outputFrame - beforeCueSave.outputFrame &&
                afterCueSave.deckGain == beforeCueSave.deckGain && afterCueSave.eqGainDb == beforeCueSave.eqGainDb &&
                afterCueSave.stemGain == beforeCueSave.stemGain && afterCueSave.cueSelected == beforeCueSave.cueSelected &&
                afterCueSave.masterGain == beforeCueSave.masterGain && afterCueSave.cueGain == beforeCueSave.cueGain &&
                afterCueSave.crossfader == beforeCueSave.crossfader &&
                afterCueSave.tempo[1].percent == beforeCueSave.tempo[1].percent &&
                afterCueSave.tempo[1].keyLock == beforeCueSave.tempo[1].keyLock &&
                controller.libraryTracks()[0].toMap()["cues"].toList() == importedMarkers && sinkMatches(),
                "Saving CUE preserves imported markers, both decks' controls and the advancing peer/samples");
        {
            conflux::DeckController reopened;
            wait([&] { return reopened.libraryReady(); }, "Saved CUE catalog did not reopen");
            require(reopened.libraryTracks().size() == 1 &&
                    reopened.libraryTracks()[0].toMap()["id"].toString() == libraryId &&
                    reopened.libraryTracks()[0].toMap()["savedCueFrame"].toString() == "47360" &&
                    reopened.libraryTracks()[0].toMap()["cues"].toList() == importedMarkers &&
                    reopened.submittedCommands() == 0,
                    "Saved CUE persists across controller recreation without an automatic seek or marker replacement");
        }
        savedCatalog = catalogSnapshot(); // The explicit Save is the last authorized metadata change.
        const auto beforeQueuedCue = controller.decks()[1].toMap()["savedCue"].toMap();
        require(beforeQueuedCue["saveAvailable"].toBool() && beforeQueuedCue["recallAvailable"].toBool(),
                "Acknowledged saved CUE is available before the next transport command");
        controller.seekSeconds(1, 640 / 48000.0);
        const auto queuedCue = controller.decks()[1].toMap()["savedCue"].toMap();
        require(!queuedCue["saveAvailable"].toBool() && !queuedCue["recallAvailable"].toBool() &&
                queuedCue["saveReason"].toString().contains("acknowledge") &&
                queuedCue["recallReason"].toString().contains("acknowledge") &&
                beforeQueuedCue["saveAvailable"].toBool() && beforeQueuedCue["recallAvailable"].toBool(),
                "Queued transport immediately disables fresh saved-CUE actions without changing the retained snapshot");
        require(!controller.saveDeckCue(1) && controller.error().contains("acknowledge"),
                "Queued seek must invalidate the current CUE snapshot until acknowledgment");
        wait([&] { const auto state = controller.audioState();
            return state.sourceFrame[1] == 640 && state.lastCommandId >= controller.submittedCommands();
        }, "B did not acknowledge the pre-recall seek");
        require(controller.recallDeckCue(1), "Saved B CUE recall was rejected");
        wait([&] { const auto state = controller.audioState();
            return state.sourceFrame[1] == 47360 && state.lastCommandId >= controller.submittedCommands();
        }, "Saved B CUE recall did not land at exact frame47360");
        const auto cueRecalled = controller.audioState();
        require(!cueRecalled.playing[1] && cueRecalled.cueFrame[1] == 47360 &&
                cueRecalled.playing[0] && !cueRecalled.invalidBuffers && !cueRecalled.rejected && sinkMatches(),
                "Saved CUE recall preserves paused B, native CUE and the exact advancing peer/samples");
        controller.seekSeconds(1, cueRecalled.sourceFrames[1] / 48000.0);
        wait([&] { const auto state = controller.audioState();
            return state.sourceFrame[1] == state.sourceFrames[1] && state.lastCommandId >= controller.submittedCommands();
        }, "B did not acknowledge its source endpoint");
        const auto endpointCommands = controller.submittedCommands();
        require(!controller.saveDeckCue(1) && controller.error().contains("before its end") &&
                controller.submittedCommands() == endpointCommands,
                "Saving at EOF rejects without issuing a transport command");
        require(controller.recallDeckCue(1), "Saved CUE must remain recallable from EOF");
        wait([&] { const auto state = controller.audioState();
            return state.sourceFrame[1] == 47360 && state.lastCommandId >= controller.submittedCommands();
        }, "Saved CUE did not recall from EOF at exact frame47360");
        require(sinkMatches(), "Saved CUE endpoint checks changed peer samples");
        observations["saved_deck_cue"] = QJsonObject{{"frame", "47360"}, {"saved_without_audio_command", true},
            {"persisted_after_reopen", true}, {"recalled_exact_frame", true}, {"imported_cues_preserved", true},
            {"peer_clock_preserved", true}, {"virtual_sink_sample_gate", !functionalOnly},
            {"virtual_sink_samples_preserved", functionalOnly ? QJsonValue(QJsonValue::Null) : QJsonValue(true)}};
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
                {"wrong_samples", qint64(sink.wrong())}, {"zero_samples", qint64(sink.zeros())},
                {"virtual_sink_sample_gate", !functionalOnly},
                {"recording_at_complete", QJsonObject::fromVariantMap(controller.recording())}};
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
        const auto recording = controller.recording();
        completion["recording_after_shutdown"] = QJsonObject::fromVariantMap(recording);
        observations["completion"] = completion;
        const auto recordedFrames = recording["writtenFrames"].toULongLong();
        require(completion["recording_at_complete"].toObject()["active"].toBool() &&
                recording["status"].toString() == "Complete" && !recording["active"].toBool() &&
                recording["error"].toString().isEmpty() && recordedFrames >= 1024 &&
                recording["acceptedFrames"].toULongLong() == recordedFrames &&
                recording["overflowFrames"].toULongLong() == 0,
                "Direct shutdown finalizes all accepted steady A recording frames without overflow");
        auto preparationWav = inspectStemRecording(preparationRecordingPath, recordedFrames, recordedFrames);
        preparationWav["required_maximum_error"] = 0.0;
        preparationWav["scope"] = "Every finalized stereo frame checked exactly; no excluded frames. Downstream virtual-sink delivery is separate from rendered PCM correctness.";
        completion["recording_pcm"] = preparationWav;
        observations["completion"] = completion;
        require(preparationWav["checked_prefix_or_tail_frames"].toInteger() == qint64(recordedFrames) &&
                preparationWav["maximum_prefix_or_tail_error"].toDouble() == 0.0,
                "Every recorded preparation frame exactly matches independent asymmetric A PCM");
        require(completion["cache_hit"].toBool() && atComplete.sourceGeneration[1] > 0 &&
                !atComplete.playing[1] && atComplete.sourceFrame[1] == 0 &&
                atComplete.playing[0] && atComplete.sourceFrame[0] > cancelled.sourceFrame[0] &&
                atComplete.sourceFrame[0] - cancelled.sourceFrame[0] == atComplete.outputFrame - cancelled.outputFrame &&
                !atComplete.invalidBuffers && !atComplete.rejected && !atComplete.starvedFrames[0] &&
                !atComplete.sourceReadError[0] && !atComplete.sourceReadErrorObserved[0] &&
                !atComplete.starvedFrames[1] && !atComplete.sourceReadError[1] && !atComplete.sourceReadErrorObserved[1] &&
                (functionalOnly || (completion["wrong_samples"].toInteger() == 0 && completion["zero_samples"].toInteger() == 0)),
                "Retry reuses persisted B preparation while A remains exact and B installs paused on the same clock");
        require(controller.completedSessions() == sessions + 1, "Only explicit shutdown completes the shared playback session");
        require(catalogSnapshot() == savedCatalog,
                "Loading and shutting down preserve the persistent imported library record");
        observations["completion"] = completion;
        require(save(phasePath, completion), "Preparation PCM and downstream sink evidence is retained together");
        observations["fixture_frames_per_track"] = qint64(fixtureFrames);
        runStemController(output, fixtures.path(), a, b, libraryId, functionalOnly, require, observations);
        runScratchController(output, a, b, require, observations);
        runEqController(output, a, b, require, observations);
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
        {"scope", "Private virtual PipeWire graph; generated PCM and fake separator, private cache/library data. Functional-only mode allows 30s for initial A readiness and cold B import setup (8s otherwise). Functional import/preparation/stem bank/gain/cancellation/recording/reentrancy checks; initial preparation checks all rendered master WAV frames exactly, with downstream sink sample gating only outside functional-only mode. No model, hardware or latency qualification."}};
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    return passed ? 0 : 1;
}

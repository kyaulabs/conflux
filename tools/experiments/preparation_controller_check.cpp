// SPDX-License-Identifier: AGPL-3.0-or-later
// Functional controller integration on the private_routing_check.py daemon.
// Generated audio and cache stay inside its private runtime; no host output.
#include "ui/DeckController.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtEndian>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
void wait(Predicate&& predicate, const char* message, bool processGui = true)
{
    const auto deadline = Clock::now() + std::chrono::seconds(8);
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

void writeFixture(const QString& path, qint16 left, qint16 right)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot create private PCM fixture");
    QDataStream header(&file);
    header.setByteOrder(QDataStream::LittleEndian);
    header.writeRawData("RIFF", 4);
    header << quint32(36 + fixtureFrames * 4);
    header.writeRawData("WAVEfmt ", 8);
    header << quint32(16) << quint16(1) << quint16(2) << quint32(48000)
           << quint32(192000) << quint16(4) << quint16(16);
    header.writeRawData("data", 4);
    header << quint32(fixtureFrames * 4);
    std::array<char, 4096 * 4> block{};
    for (std::size_t frame = 0; frame < block.size() / 4; ++frame) {
        qToLittleEndian<qint16>(left, block.data() + frame * 4);
        qToLittleEndian<qint16>(right, block.data() + frame * 4 + 2);
    }
    for (std::uint32_t first = 0; first < fixtureFrames;) {
        const auto frames = std::min<std::uint32_t>(4096, fixtureFrames - first);
        if (file.write(block.data(), frames * 4) != qint64(frames * 4))
            throw std::runtime_error("Cannot write private PCM fixture");
        first += frames;
    }
    if (header.status() != QDataStream::Ok || !file.flush())
        throw std::runtime_error("Cannot finish private PCM fixture");
}

class Sink {
public:
    explicit Sink(std::string name) : name_(std::move(name))
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
                wrong += samples[frame] != expected[channel];
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
    std::array<void*, 2> ports_{};
    std::array<std::atomic<std::uint64_t>, 2> received_{};
    std::atomic<std::uint64_t> wrong_{0}, zeros_{0};
    std::atomic<bool> armed_{false};
    pw_thread_loop* loop_ = nullptr;
    pw_filter* filter_ = nullptr;
    bool started_ = false;
};

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
        const auto before = controller.audioState();
        const auto sessions = controller.completedSessions();
        const auto received = sink.received(0);
        controller.loadDeck(1, QUrl::fromLocalFile(b));
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
        controller.loadDeck(1, QUrl::fromLocalFile(b));
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
        observations["completion"] = completion;
        observations["fixture_frames_per_track"] = qint64(fixtureFrames);
        passed = true;
    } catch (const std::exception& failure) {
        error = QString::fromUtf8(failure.what());
    }
    const QJsonObject result{{"passed", passed}, {"error", error}, {"checks", checks},
        {"execution_deadline_gate", false}, {"observations", observations},
        {"scope", "Private virtual PipeWire graph; generated PCM and private cache only. Functional preparation/cancellation/reentrancy checks, no hardware or latency qualification."}};
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    return passed ? 0 : 1;
}

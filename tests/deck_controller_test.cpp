// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui/DeckController.hpp"
#include "ui/LocalTiming.hpp"
#include "library/LiveStemObservations.hpp"
#include "StemExportFixture.hpp"
#include "StemServiceFixture.hpp"
#include "TrackPresentationFixture.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QtConcurrent>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtTest>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <future>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits>
#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {

bool writeWave(const QString& path, quint16 channels, quint32 rate, quint32 frames = 4)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const quint32 payloadBytes = frames * channels * sizeof(qint16);
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + payloadBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << channels << rate;
    stream << quint32(rate * channels * sizeof(qint16));
    stream << quint16(channels * sizeof(qint16)) << quint16(16);
    stream.writeRawData("data", 4);
    stream << payloadBytes;
    for (quint32 sample = 0; sample < frames * channels; ++sample)
        stream << qint16(sample * 17 - 32);
    return stream.status() == QDataStream::Ok && file.flush();
}

bool writeCue(const QString& path, const QString& source, const QByteArray& indices =
    "  TRACK 01 AUDIO\n    TITLE \"<b>Drop</b> # %\"\n    INDEX 01 00:00:00\n"
    "  TRACK 02 AUDIO\n    INDEX 01 00:00:01\n")
{
    QFile file(path);
    const auto data = "FILE \"" + QFileInfo(source).fileName().toUtf8() + "\" WAVE\n" + indices;
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.flush();
}

// Generated stdlib-only parent/worker contract. No key model or NumPy here.
struct HarmonyFixture {
    QTemporaryDir directory;
    std::unique_ptr<conflux::audio::PreparedAudio> pcm;
    conflux::library::HarmonyEngine engine;
    QFile writable;
    QString marker, release;
    explicit HarmonyFixture(const QByteArray& mode = "good", std::uint64_t frames = 384013)
    {
        if (!directory.isValid()) throw std::runtime_error("Harmony fixture directory failed");
        const auto raw = directory.filePath("input.f32");
        std::vector<float> samples(frames * 2, .125F);
        conflux::test::writeFixtureBytes(raw, QByteArray(reinterpret_cast<const char*>(samples.data()), qsizetype(samples.size() * sizeof(float))));
        writable.setFileName(raw);
        if (!writable.open(QIODevice::ReadWrite)) throw std::runtime_error("Harmony writable fixture FD failed");
        const int fd = ::open(QFile::encodeName(raw).constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("Harmony fixture FD failed");
        try { pcm = std::make_unique<conflux::audio::PreparedAudio>(conflux::audio::reopenPreparedAudio(fd, frames, std::vector<float>(512, .125F))); }
        catch (...) { close(fd); throw; }
        close(fd);
        engine = {QStandardPaths::findExecutable("python3"), directory.filePath("worker.py"), 3};
        marker = directory.filePath("started"); release = directory.filePath("release");
        setMode(mode);
    }
    void setMode(const QByteArray& mode)
    {
        const auto quote = [](const QString& value) { return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).mid(1).chopped(1); };
        QByteArray script = "MODE='" + mode + "'\nMARKER=" + quote(marker) + "\nRELEASE=" + quote(release) + "\n";
        script += R"PY(import argparse, hashlib, json, os, pathlib, signal, sys, time
p=argparse.ArgumentParser();p.add_argument('--frames',type=int,required=True);a=p.parse_args()
b=sys.stdin.buffer.read();assert len(b)==a.frames*8
if MODE in ('sleep','await-invalid'):
    if MODE=='sleep':signal.signal(signal.SIGTERM,signal.SIG_IGN)
    temporary=pathlib.Path(MARKER+'.tmp');temporary.write_text(str(os.getpid()));temporary.replace(MARKER)
    if MODE=='sleep':
        time.sleep(30)
    while not pathlib.Path(RELEASE).exists():time.sleep(.005)
if MODE=='flood':sys.stdout.write('x'*200000);sys.stdout.flush();raise SystemExit(0)
if MODE in ('invalid','await-invalid'):print('invalid');raise SystemExit(0)
count=(a.frames+191999)//192000
head=dict(schema=1,recipe='local-chroma-krumhansl-v1',frames=a.frames,sample_rate=48000,channels=2,
          pcm_sha256=hashlib.sha256(b).hexdigest(),window_frames=192000,window_count=count)
if MODE=='hash':head['pcm_sha256']='0'*64
if MODE=='bool-schema':head['schema']=True
if MODE=='frames':head['frames']+=1
raw=json.dumps(head)
if MODE=='duplicate':raw='{"schema":0,'+raw[1:]
print(raw)
for i in range(count):
    first=i*192000;end=min(first+192000,a.frames)
    row=dict(first_frame=first,end_frame=end,status='Unknown',reason='short',chroma=[0.]*12,candidates=[],confidence='Unknown')
    if end-first>=32768:
        tonic=(0 if a.frames==192000 else 2) if MODE=='controller' else (0 if i==0 else 2)
        row.update(status='Estimated',reason='profile-match',chroma=[.25,0,.2,0,.15,0,0,.2,0,.2,0,0],
            candidates=[dict(tonic=tonic,mode='Major',correlation=.85),dict(tonic=(tonic+9)%12,mode='Minor',correlation=.5)])
    if end-first>=32768:
        if MODE in ('sparse-estimated','sparse-ambiguous','sparse-competing'):
            row['chroma']=[1.]+[0.]*11
        if MODE in ('flat-estimated','flat-unknown','flat-ambiguous'):
            row['chroma']=[1/12]*12
        if MODE in ('sparse-ambiguous','wrong-context','flat-ambiguous'):
            row.update(status='Ambiguous',reason='insufficient-key-context')
        if MODE in ('sparse-competing','matched-competing','competing-good'):
            row.update(status='Ambiguous',reason='competing-keys')
        if MODE in ('flat-unknown','wrong-non-tonal'):
            row.update(status='Unknown',reason='non-tonal',candidates=[])
        if MODE=='competing-good':row['candidates'][0]['correlation']=.55
        if MODE=='weak-estimated':row['candidates'][0]['correlation']=.55
    if MODE=='overlap':row['first_frame']=1
    if MODE=='confidence':row['confidence']='High'
    if MODE=='nan' and row['candidates']:row['candidates'][0]['correlation']=float('nan')
    print(json.dumps(row))
if MODE!='missing-end':print(json.dumps(dict(schema=1,complete=True,window_count=count)))
if MODE=='extra':print('{}')
)PY";
        conflux::test::writeFixtureBytes(engine.workerPath, script);
    }
};

struct RestoreEnvironment {
    std::array<const char*, 5> names{"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "XDG_DATA_HOME", "XDG_CACHE_HOME"};
    std::array<QByteArray, 5> values;
    std::array<bool, 5> present;
    RestoreEnvironment() {
        for (std::size_t i = 0; i < names.size(); ++i) {
            values[i] = qgetenv(names[i]);
            present[i] = qEnvironmentVariableIsSet(names[i]);
        }
    }
    ~RestoreEnvironment() {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (present[i]) qputenv(names[i], values[i]); else qunsetenv(names[i]);
    }
};


template<class Function>
void liveControllerChecked(Function&& function)
{
    try { function(); }
    catch (const std::exception& error) { QFAIL(error.what()); }
}

void isolateLiveController(const QString& data, const QString& runtime)
{
    qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    qputenv("PIPEWIRE_RUNTIME_DIR", runtime.toUtf8());
    qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
    qputenv("XDG_CACHE_HOME", data.toUtf8());
    qputenv("XDG_DATA_HOME", data.toUtf8());
}

QByteArray liveFixtureBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024)
        throw std::runtime_error("Cannot read bounded controller fixture evidence");
    return file.readAll();
}

std::vector<QJsonObject> liveFixtureRows(const conflux::test::StemServiceFixture& fixture)
{
    if (!QFileInfo::exists(fixture.marker(".trace"))) return {};
    const auto bytes = liveFixtureBytes(fixture.marker(".trace"));
    const auto last = bytes.lastIndexOf('\n');
    std::vector<QJsonObject> rows;
    if (last < 0) return rows; // Retry only an unfinished final fixture row.
    for (const auto& line : bytes.first(last + 1).split('\n')) {
        if (line.isEmpty()) continue;
        const auto doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) throw std::runtime_error("Malformed complete controller fixture trace row");
        rows.push_back(doc.object());
    }
    return rows;
}

std::size_t liveFixtureCount(const conflux::test::StemServiceFixture& fixture,
                             const char* key, const char* value)
{
    const auto rows = liveFixtureRows(fixture);
    return std::size_t(std::ranges::count_if(rows,
        [&](const auto& row) { return row.value(key).toString() == value; }));
}

std::uint64_t expectedLiveFrames(const QString& source)
{
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read controller source oracle");
    auto input = conflux::audio::prepareStemInput(file.handle());
    auto output = conflux::audio::prepareStemOutput(input.descriptor(), input.frames());
    return output.frames();
}

// Test-owned cleanup for the known stdlib service only. Start identity is
// checked after pidfd acquisition; a stale marker never authorizes a signal.
struct ControllerServiceSafety {
    const conflux::test::StemServiceFixture& fixture;
    struct Child { pid_t pid; int descriptor; qulonglong startTicks; };
    std::vector<Child> children;
    void capture()
    {
        for (const auto& row : liveFixtureRows(fixture)) {
            if (row.value("fixture") != QJsonValue("started")) continue;
            const auto pid = row.value("fixture_pid").toVariant().toLongLong();
            const auto expected = row.value("fixture_start_ticks").toVariant().toULongLong();
            if (pid <= 1 || !expected || std::ranges::any_of(children,
                [&](const auto& child) { return child.pid == pid && child.startTicks == expected; })) continue;
            const int descriptor = int(syscall(SYS_pidfd_open, pid_t(pid), 0));
            if (descriptor < 0) {
                if (errno == ESRCH) continue;
                throw std::runtime_error("Cannot retain controller service identity");
            }
            try {
                const auto stat = liveFixtureBytes(QString("/proc/%1/stat").arg(pid));
                const auto nameEnd = stat.lastIndexOf(") ");
                const auto fields = nameEnd < 0 ? QList<QByteArray>{} : stat.mid(nameEnd + 2).simplified().split(' ');
                if (fields.size() <= 19 || fields[19].toULongLong() != expected) {
                    close(descriptor);
                    continue;
                }
                children.push_back({pid_t(pid), descriptor, expected});
            } catch (...) {
                pollfd status{descriptor, POLLIN, 0};
                const bool exited = poll(&status, 1, 0) == 1 && (status.revents & POLLIN);
                close(descriptor);
                if (!exited) throw;
            }
        }
    }
    bool gone()
    {
        capture();
        return std::ranges::all_of(children, [](const auto& child) {
            pollfd status{child.descriptor, POLLIN, 0};
            return poll(&status, 1, 0) == 1 && (status.revents & POLLIN);
        });
    }
    bool jobsGone() const
    {
        for (const auto& row : liveFixtureRows(fixture)) {
            if (row.value("fixture") != QJsonValue("started")) continue;
            const auto service = row.value("fixture_job").toString();
            const auto parent = QFileInfo(service).absolutePath();
            if (QFileInfo(service).fileName() != "service" || !QFileInfo(parent).fileName().startsWith(".live-"))
                throw std::runtime_error("Fixture did not identify its private live owner job");
            if (QFileInfo::exists(parent)) return false;
        }
        return true;
    }
    ~ControllerServiceSafety()
    {
        try { capture(); } catch (...) {}
        for (const auto& child : children) {
            pollfd status{child.descriptor, POLLIN, 0};
            if (poll(&status, 1, 0) != 1) {
                (void)syscall(SYS_pidfd_send_signal, child.descriptor, SIGKILL, nullptr, 0);
                (void)poll(&status, 1, 2000);
            }
            int result = 0;
            (void)waitpid(child.pid, &result, WNOHANG);
            close(child.descriptor);
        }
    }
};

// Only test scheduling changes. Release and restore before the controller's
// destructor; waitForFinished is called only after isFinished becomes true.
class ControllerRetirementBarrier {
public:
    ControllerRetirementBarrier()
        : pool_(*QThreadPool::globalInstance()), maximum_(pool_.maxThreadCount()),
          expiry_(pool_.expiryTimeout()), state_(std::make_shared<State>())
    {
        pool_.setMaxThreadCount(1);
        try {
            task_ = QtConcurrent::run(&pool_, [state = state_] {
                state->entered.store(true, std::memory_order_release);
                state->gate.wait();
            });
        } catch (...) { pool_.setMaxThreadCount(maximum_); throw; }
    }
    ~ControllerRetirementBarrier() { release(); }
    bool entered() const { return state_->entered.load(std::memory_order_acquire); }
    void release()
    {
        if (released_) return;
        released_ = true;
        state_->release.set_value();
        while (!task_.isFinished()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        task_.waitForFinished();
        pool_.setMaxThreadCount(maximum_);
        pool_.setExpiryTimeout(expiry_);
    }
private:
    struct State {
        std::atomic<bool> entered{false};
        std::promise<void> release;
        std::shared_future<void> gate = release.get_future().share();
    };
    QThreadPool& pool_;
    int maximum_, expiry_;
    std::shared_ptr<State> state_;
    QFuture<void> task_;
    bool released_ = false;
};

void verifyNoPlayback(const conflux::DeckController& controller)
{
    QVERIFY(!controller.outputReady());
    const auto decks = controller.decks();
    QCOMPARE(decks.size(), 2);
    for (const auto& value : decks) {
        const auto deck = value.toMap();
        QVERIFY(!deck.value("loaded").toBool());
        QVERIFY(deck.value("name").toString().isEmpty());
        QVERIFY(!deck.value("playing").toBool());
        QVERIFY(deck.contains("replacing") && !deck.value("replacing").toBool());
        QCOMPARE(deck.value("sourceGeneration").toULongLong(), qulonglong(0));
        QCOMPARE(deck.value("positionSeconds").toDouble(), 0.0);
        QCOMPARE(deck.value("durationSeconds").toDouble(), 0.0);
        QVERIFY(deck.contains("cueSeconds") && deck.contains("cueHeld") && deck.contains("cuePreview"));
        QCOMPARE(deck.value("cueSeconds").toDouble(), 0.0);
        QVERIFY(!deck.value("cueHeld").toBool());
        QVERIFY(!deck.value("cuePreview").toBool());
    }
}

class DeckControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void recoveryRangePayloadRemainsChargedThroughQueuedCancellationAndRetirement()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime;
            QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore;
            isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("approved-live.wav"), peer = directory.filePath("kept-peer.wav");
            QVERIFY(writeWave(source, 2, 48000, 8192) && writeWave(peer, 2, 48000, 480));
            const auto engineRoot = directory.filePath("engine");
            QVERIFY(QDir().mkpath(engineRoot));
            const auto fixture = conflux::test::makeStemServiceFixture(engineRoot);
            ControllerServiceSafety safety{fixture};
            conflux::DeckController::PreparationOptions options;
            options.stemEngine = fixture.engine; options.stemServicePath = fixture.service;
            conflux::DeckController controller(options);
            QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 10000);
            controller.importTrack(QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QCOMPARE(controller.libraryTracks().size(), qsizetype(1));
            const auto id = controller.libraryTracks().front().toMap()["id"].toString();
            controller.loadDeck(0, QUrl::fromLocalFile(peer));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            const auto before = controller.decks();
            const auto retained = controller.retainedPcmBytes();
            QCOMPARE(retained, std::uint64_t(480 * 8));
            controller.loadLibraryStemTrackWithRecoveryRange(1, id, 2048. / 48000, 4096. / 48000);
            QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
            QVERIFY(controller.busy());
            QCOMPARE(controller.retainedPcmBytes(), retained + 65536);
            safety.capture(); QVERIFY(!safety.children.empty() && !safety.gone());
            ControllerRetirementBarrier barrier;
            QVERIFY(controller.cancelPreparation());
            QTRY_VERIFY_WITH_TIMEOUT(barrier.entered(), 10000);
            QCoreApplication::sendPostedEvents();
            QVERIFY(controller.busy());
            QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelling"));
            QCOMPARE(controller.retainedPcmBytes(), retained + 65536);
            QCOMPARE(controller.decks()[0], before[0]);
            barrier.release();
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelled"));
            QCOMPARE(controller.decks(), before);
            QCOMPARE(controller.retainedPcmBytes(), retained);
            QTRY_VERIFY_WITH_TIMEOUT(safety.gone() && safety.jobsGone(), 10000);
            controller.loadLibraryStemTrackWithRecoveryRange(1, id, 2048. / 48000, 4096. / 48000);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            const auto deck = controller.decks()[1].toMap();
            const auto hold = deck["residentHold"].toMap();
            QVERIFY2(deck["loaded"].toBool(), qPrintable(controller.error()));
            QVERIFY(hold["configured"].toBool() && hold["ready"].toBool() && !hold["armed"].toBool());
            QCOMPARE(hold["residentBytes"].toULongLong(), qulonglong(65536));
            QCOMPARE(controller.retainedPcmBytes(), retained + 65536);
            // This fixture deliberately has no output server. It must not fake
            // a fresh callback ticket, positive Arm or playback qualification.
            QVERIFY(!controller.outputReady());
            const auto submitted = controller.submittedCommands();
            QVERIFY(!controller.authorizeResidentHold(1, id, hold["outputSession"].toString(),
                                                     hold["rangeId"].toString(), hold["sourceGeneration"].toString(),
                                                     hold["transportRevision"].toString()));
            QVERIFY(!controller.cancelResidentHold(1, id, hold["outputSession"].toString(),
                                                  hold["rangeId"].toString(), "18446744073709551616", "0"));
            QVERIFY(!controller.exitResidentHold(1, QString(64, QLatin1Char('0')), "0", "0", "0", "0"));
            QCOMPARE(controller.submittedCommands(), submitted);
            safety.capture();
            controller.loadDeck(1, QUrl::fromLocalFile(peer));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(safety.gone() && safety.jobsGone(), 10000);
            QCOMPARE(controller.retainedPcmBytes(), retained * 2);
            QVERIFY(!controller.liveStemObservations(1));
            QCOMPARE(liveFixtureCount(fixture, "command", "load"), std::size_t(2));
            controller.shutdown();
        });
    }

    void malformedRecoveryIntentCannotCancelExistingPreparationOrTrustChangedSource()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime;
            QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore;
            isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("catalog-source.wav"), loading = directory.filePath("loading.wav");
            QVERIFY(writeWave(source, 2, 48000, 8192) && writeWave(loading, 2, 48000, 480));
            const auto engineRoot = directory.filePath("engine");
            QVERIFY(QDir().mkpath(engineRoot));
            const auto fixture = conflux::test::makeStemServiceFixture(engineRoot);
            ControllerServiceSafety safety{fixture};
            conflux::DeckController::PreparationOptions options;
            options.stemEngine = fixture.engine; options.stemServicePath = fixture.service;
            conflux::DeckController controller(options);
            QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 10000);
            controller.importTrack(QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            const auto id = controller.libraryTracks().front().toMap()["id"].toString();
            const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
            QLockFile lock(cache + "/.lock"); QVERIFY(lock.tryLock(0));
            controller.loadDeck(0, QUrl::fromLocalFile(loading));
            QVERIFY(controller.busy());
            const auto pending = controller.preparation();
            for (const auto bounds : {std::pair{std::numeric_limits<double>::quiet_NaN(), .1},
                                      std::pair{0., std::numeric_limits<double>::infinity()},
                                      std::pair{-.1, .1}, std::pair{.1, .1}, std::pair{0., .001},
                                      std::pair{0., 1.}}) {
                controller.loadLibraryStemTrackWithRecoveryRange(1, id, bounds.first, bounds.second);
                QVERIFY(controller.busy()); QCOMPARE(controller.preparation(), pending);
            }
            controller.loadLibraryStemTrackWithRecoveryRange(1, QString(64, QLatin1Char('0')), 0., .1);
            QVERIFY(controller.busy()); QCOMPARE(controller.preparation(), pending);
            lock.unlock();
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY2(controller.decks()[0].toMap()["loaded"].toBool(), qPrintable(controller.error()));
            QVERIFY(!controller.decks()[1].toMap()["loaded"].toBool());
            QCOMPARE(liveFixtureCount(fixture, "command", "load"), std::size_t(0));
            const auto previous = controller.decks();
            // Same path/shape, different source digest. A catalog selection is
            // not authority to reuse the old range on replacement content.
            QFile changed(source); QVERIFY(changed.open(QIODevice::ReadWrite));
            QVERIFY(changed.seek(44)); const auto old = changed.read(1); QCOMPARE(old.size(), qsizetype(1));
            QVERIFY(changed.seek(44)); QCOMPARE(changed.write(QByteArray(1, char(old[0] ^ 1))), qint64(1)); changed.close();
            controller.loadLibraryStemTrackWithRecoveryRange(1, id, 2048. / 48000, 4096. / 48000);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(controller.error().contains("changed", Qt::CaseInsensitive));
            QCOMPARE(controller.decks(), previous);
            QCOMPARE(liveFixtureCount(fixture, "command", "load"), std::size_t(0));
        });
    }

    void scratchPublicRejectionPreservesEmptyDecks()
    {
        conflux::DeckController controller;
        const auto original = controller.decks();
        const auto submitted = controller.submittedCommands();
        for (const int deck : {-1, 0, 1, 2}) {
            QVERIFY(controller.beginScratch(deck).isEmpty());
            for (const auto& token : {QString{}, QStringLiteral("0000000000000001"),
                                      QStringLiteral("ffffffffffffffff"), QStringLiteral("not-a-token")}) {
                QVERIFY(!controller.moveScratch(deck, token, std::numeric_limits<qint64>::min()));
                QVERIFY(!controller.moveScratch(deck, token, std::numeric_limits<qint64>::max()));
                QVERIFY(!controller.endScratch(deck, token));
                QCOMPARE(controller.decks(), original);
                QCOMPARE(controller.submittedCommands(), submitted);
            }
        }
        controller.shutdown();
        const auto stopped = controller.decks();
        for (const int deck : {0, 1}) {
            QVERIFY(controller.beginScratch(deck).isEmpty());
            QVERIFY(!controller.moveScratch(deck, QStringLiteral("0000000000000001"), 120));
            QVERIFY(!controller.endScratch(deck, QStringLiteral("0000000000000001")));
        }
        QCOMPARE(controller.decks(), stopped);
        QCOMPARE(controller.submittedCommands(), submitted);
    }

    void scratchUnavailableOutputAndWrongTokenPreserveLoadedSources()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto source = directory.filePath("scratch-unready.wav");
        QVERIFY(writeWave(source, 2, 48000, 4800));
        conflux::DeckController controller;
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(controller.decks()[0].toMap()["loaded"].toBool());
        QVERIFY(!controller.outputReady()); // Deliberately unavailable owned-test remote.
        const auto accepted = controller.decks();
        const auto submitted = controller.submittedCommands();
        const auto state = controller.audioState();
        for (const int deck : {0, 1}) {
            QVERIFY(controller.beginScratch(deck).isEmpty());
            QVERIFY(!controller.moveScratch(deck, QStringLiteral("0000000000000001"), 48000));
            QVERIFY(!controller.endScratch(deck, QStringLiteral("0000000000000001")));
            QCOMPARE(controller.decks(), accepted);
            QCOMPARE(controller.submittedCommands(), submitted);
            QVERIFY(controller.audioState().sourceFrame == state.sourceFrame);
            QVERIFY(controller.audioState().sourceGeneration == state.sourceGeneration);
        }
        controller.loadDeck(0, QUrl::fromLocalFile(directory.filePath("missing-scratch-replacement.wav")));
        QVERIFY(controller.beginScratch(0).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.decks(), accepted);
        QVERIFY(!controller.endScratch(0, QStringLiteral("0000000000000001")));
        QCOMPARE(controller.decks(), accepted);
        QCOMPARE(controller.submittedCommands(), submitted);
        // Positive token issuance, ACK and queue-full retry are intentionally
        // NOT claimed here: root's owned virtual-PipeWire integration covers them.
    }

    void observationsFollowCurrentLiveClipWithoutOwningItsLifetime()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime;
            QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore;
            isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("observed live.wav"), finite = directory.filePath("finite.wav");
            QVERIFY(writeWave(source, 2, 44100, 211) && writeWave(finite, 2, 48000, 480));
            const auto engineRoot = directory.filePath("engine");
            QVERIFY(QDir().mkpath(engineRoot));
            const auto fixture = conflux::test::makeStemServiceFixture(engineRoot);
            ControllerServiceSafety safety{fixture};
            conflux::DeckController::PreparationOptions options;
            options.stemEngine = fixture.engine; options.stemServicePath = fixture.service;
            conflux::DeckController controller(options);
            QVERIFY(!controller.liveStemObservations(-1) && !controller.liveStemObservations(0) &&
                    !controller.liveStemObservations(1) && !controller.liveStemObservations(2));
            controller.loadDeck(0, QUrl::fromLocalFile(finite));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(controller.decks()[0].toMap()["loaded"].toBool() && !controller.liveStemObservations(0));
            controller.loadStemDeck(0, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            auto handle = controller.liveStemObservations(0);
            QVERIFY2(handle, qPrintable(controller.error()));
            QVERIFY(handle == controller.liveStemObservations(0) && !controller.liveStemObservations(1));
            const auto liveDecks = controller.decks();
            const auto submitted = controller.submittedCommands();
            for (const auto command : {&conflux::DeckController::loopIn, &conflux::DeckController::loopOut,
                                       &conflux::DeckController::loopToggle}) {
                (controller.*command)(0);
                QVERIFY(controller.error().contains("prepared", Qt::CaseInsensitive));
                QCOMPARE(controller.decks(), liveDecks);
                QCOMPARE(controller.submittedCommands(), submitted);
                QVERIFY(controller.liveStemObservations(0) == handle);
            }
            controller.setTempoPercent(0, 3);
            QVERIFY(controller.error().contains("prepared", Qt::CaseInsensitive));
            for (const auto command : {&conflux::DeckController::toggleMasterTempo,
                                       &conflux::DeckController::resetTempo}) {
                (controller.*command)(0);
                QVERIFY(controller.error().contains("prepared", Qt::CaseInsensitive));
            }
            QCOMPARE(controller.decks(), liveDecks);
            QCOMPARE(controller.submittedCommands(), submitted);
            QVERIFY(controller.liveStemObservations(0) == handle);
            QCOMPARE(handle->setup().outputFrames48000, controller.audioState().sourceFrames[0]);
            safety.capture();
            QVERIFY(!safety.children.empty() && !safety.gone());
            controller.loadDeck(0, QUrl::fromLocalFile(finite));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(controller.decks()[0].toMap()["loaded"].toBool() && !controller.audioState().stemBank[0]);
            QVERIFY(!controller.liveStemObservations(0));
            QTRY_VERIFY_WITH_TIMEOUT(safety.gone() && safety.jobsGone(), 10000);
            QVERIFY(handle->counters().producerFinished);
            QVERIFY(handle->failureDetail().empty());
            controller.shutdown();
            QVERIFY(!controller.liveStemObservations(0));
        });
    }

    void liveServiceModePrefersCompleteBankWithoutUsingService()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime;
            QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore;
            isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("cached source.flac");
            QVERIFY(presentationFixture::writeFlac(source, "Cached stem title", "Generated artist", "Generated album",
                presentationFixture::artwork(Qt::blue), 44100, 211));
            const auto engineRoot = directory.filePath("engine");
            QVERIFY(QDir().mkpath(engineRoot));
            const auto fixture = conflux::test::makeStemServiceFixture(engineRoot);
            const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
            auto bank = conflux::library::PreparedTrackStore(cache).prepareStemBank(source, fixture.engine);
            const auto calls = liveFixtureBytes(fixture.engine.exporterPath.left(fixture.engine.exporterPath.size() - 3) + ".calls");
            conflux::DeckController::PreparationOptions options;
            options.stemEngine = fixture.engine;
            // An unused service is not part of the five-asset cache identity.
            options.stemServicePath = directory.filePath("unused missing service.py");
            conflux::DeckController controller(options);
            QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 10000);
            controller.loadStemDeck(0, QUrl::fromLocalFile(source));
            QVERIFY(controller.decks()[0].toMap()["loading"].toBool());
            QCOMPARE(controller.decks()[0].toMap()["loadPhase"].toString(), QString("stems"));
            QVERIFY(controller.preparation()["stems"].toBool());
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            auto deck = controller.decks()[0].toMap();
            QVERIFY2(deck["loaded"].toBool(), qPrintable(controller.error()));
            QVERIFY(deck["stemBank"].toBool() && controller.preparation()["cacheHit"].toBool());
            QCOMPARE(deck["name"].toString(), QFileInfo(source).fileName());
            QCOMPARE(deck["overviewStem"].toString(), QString("drums"));
            QCOMPARE(deck["title"].toString(), QString("Cached stem title"));
            QCOMPARE(deck["artist"].toString(), QString("Generated artist"));
            QVERIFY(deck["artworkDataUrl"].toString().startsWith("data:image/png;base64,"));
            QCOMPARE(controller.audioState().sourceFrames[0], bank.stems[0].frames());
            QCOMPARE(controller.retainedPcmBytes(), bank.stems[0].frames() * 8 * 4);
            QCOMPARE(deck["waveform"].toList().size(), qsizetype(bank.stems[0].peaks().size()));
            for (qsizetype i = 0; i < deck["waveform"].toList().size(); ++i)
                QCOMPARE(deck["waveform"].toList()[i].toFloat(), bank.stems[0].peaks()[std::size_t(i)]);

            controller.importTrack(QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QCOMPARE(controller.libraryTracks().size(), qsizetype(1));
            const auto sourceId = controller.libraryTracks()[0].toMap()["id"].toString();
            QCOMPARE(sourceId, bank.sourceSha256);
            controller.loadLibraryStemTrack(1, sourceId);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(controller.decks()[1].toMap()["loaded"].toBool());
            QVERIFY(controller.preparation()["cacheHit"].toBool());
            QCOMPARE(controller.decks()[1].toMap()["overviewStem"].toString(), QString("drums"));
            QCOMPARE(controller.audioState().sourceFrames[1], bank.stems[0].frames());
            QCOMPARE(liveFixtureBytes(fixture.engine.exporterPath.left(fixture.engine.exporterPath.size() - 3) + ".calls"), calls);
            QVERIFY(!QFileInfo::exists(fixture.marker(".started")));
            QVERIFY(liveFixtureRows(fixture).empty());
        });
    }

    void postReadyPrimeFailureReportsCauseAndBoundedServiceStderr()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime;
            QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore;
            isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("source.wav"), engineRoot = directory.filePath("engine");
            QVERIFY(writeWave(source, 2, 44100, 211) && QDir().mkpath(engineRoot));
            auto fixture = conflux::test::makeStemServiceFixture(engineRoot, "error-after-ready");
            fixture.engine.maxDiagnosticBytes = 8192;
            ControllerServiceSafety safety{fixture};
            conflux::DeckController::PreparationOptions options;
            options.stemEngine = fixture.engine; options.stemServicePath = fixture.service;
            conflux::DeckController controller(options);
            controller.loadStemDeck(0, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(fixture.marker(".failure-wait")), 10000);
            QTRY_COMPARE_WITH_TIMEOUT(controller.preparation()["phase"].toString(), QString("prime"), 10000);
            QVERIFY(controller.busy());
            safety.capture();
            conflux::test::writeFixtureBytes(fixture.marker(".failure-release"), "release\n");
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            const auto error = controller.error();
            QVERIFY2(error.startsWith("Cannot prime the prepared track: Live stem producer: "), qPrintable(error));
            QVERIFY(error.contains("Live stem service: fixture post-ready primary error"));
            QVERIFY(error.contains("[truncated; last bytes follow]") && error.endsWith("fixture stderr causal tail\n"));
            QVERIFY(!error.contains("fixture stderr prefix") && error.toUtf8().size() < 4352);
            QVERIFY(!controller.decks()[0].toMap()["loaded"].toBool() && !controller.liveStemObservations(0));
            QTRY_VERIFY_WITH_TIMEOUT(safety.gone() && safety.jobsGone(), 10000);
            controller.shutdown();
        });
    }

    void genuineMissInstallsOneLiveOwnerAndParksOnOutputFailure()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime;
            QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore;
            isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("live source.flac");
            QVERIFY(presentationFixture::writeFlac(source, "Live stem title", "Generated artist", "Generated album",
                presentationFixture::artwork(Qt::blue), 44100, 211));
            const auto frames = expectedLiveFrames(source);
            const auto engineRoot = directory.filePath("engine");
            QVERIFY(QDir().mkpath(engineRoot));
            const auto fixture = conflux::test::makeStemServiceFixture(engineRoot);
            ControllerServiceSafety safety{fixture}; // Outlives the controller.
            conflux::DeckController::PreparationOptions options;
            options.stemEngine = fixture.engine;
            options.stemServicePath = fixture.service;
            conflux::DeckController controller(options);
            controller.loadStemDeck(0, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            const auto deck = controller.decks()[0].toMap();
            QVERIFY2(deck["loaded"].toBool(), qPrintable(controller.error()));
            QVERIFY(deck["stemBank"].toBool() && !controller.preparation()["cacheHit"].toBool());
            QCOMPARE(deck["name"].toString(), QFileInfo(source).fileName());
            QCOMPARE(deck["overviewStem"].toString(), QString("source"));
            QCOMPARE(deck["title"].toString(), QString("Live stem title"));
            QCOMPARE(deck["album"].toString(), QString("Generated album"));
            QVERIFY(deck["artworkDataUrl"].toString().startsWith("data:image/png;base64,"));
            QCOMPARE(controller.audioState().sourceFrames[0], frames);
            QCOMPARE(deck["durationSeconds"].toDouble(), frames / 48000.0);
            QCOMPARE(deck["stemGains"].toList(), QVariantList({1.F, 1.F, 1.F, 1.F}));
            QCOMPARE(controller.retainedPcmBytes(), std::uint64_t(0));
            const auto peaks = deck["waveform"].toList();
            QCOMPARE(peaks.size(), qsizetype(211));
            // Independent PCM16 source oracle; 44100 input bypasses conversion.
            float maximum = 0.F;
            std::array<float, 211> expected{};
            for (std::size_t frame = 0; frame < expected.size(); ++frame) {
                const auto left = float(qint16(int(frame) * 2 * 17 - 32)) / 32768.F;
                const auto right = float(qint16((int(frame) * 2 + 1) * 17 - 32)) / 32768.F;
                expected[frame] = std::max(std::abs(left), std::abs(right));
                maximum = std::max(maximum, expected[frame]);
            }
            for (qsizetype i = 0; i < peaks.size(); ++i)
                QCOMPARE(peaks[i].toFloat(), expected[std::size_t(i)] / maximum);
            QVERIFY(!controller.outputReady());
            QVERIFY(!controller.error().isEmpty()); // Unavailable private server.
            safety.capture();
            QVERIFY(!safety.children.empty() && !safety.gone());
            QCOMPARE(liveFixtureCount(fixture, "command", "load"), std::size_t(1));
            QCOMPARE(liveFixtureCount(fixture, "command", "seek"), std::size_t(0));
            QCOMPARE(liveFixtureCount(fixture, "fixture", "runtime"), std::size_t(1));
            QVERIFY(!QFileInfo::exists(fixture.engine.exporterPath.left(fixture.engine.exporterPath.size() - 3) + ".calls"));
            controller.shutdown();
            QTRY_VERIFY_WITH_TIMEOUT(safety.gone(), 10000);
            const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
            QVERIFY(QDir(cache).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        });
    }

    void corruptBankAndChangedLibrarySourceNeverSelectLiveFallback()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime;
            QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore;
            isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("library source.wav"), kept = directory.filePath("kept.wav");
            QVERIFY(writeWave(source, 2, 44100, 211) && writeWave(kept, 2, 48000, 480));
            const auto engineRoot = directory.filePath("engine");
            QVERIFY(QDir().mkpath(engineRoot));
            const auto fixture = conflux::test::makeStemServiceFixture(engineRoot);
            ControllerServiceSafety safety{fixture};
            const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
            auto seeded = conflux::library::PreparedTrackStore(cache).prepareStemBank(source, fixture.engine);
            const auto manifestPath = QDir(cache).filePath(seeded.artifactKey + "/manifest.json");
            {
                QFile manifest(manifestPath);
                QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
                QCOMPARE(manifest.write("{}"), qint64(2));
            }
            const auto calls = liveFixtureBytes(fixture.engine.exporterPath.left(fixture.engine.exporterPath.size() - 3) + ".calls");
            conflux::DeckController::PreparationOptions options;
            options.stemEngine = fixture.engine;
            options.stemServicePath = fixture.service;
            conflux::DeckController controller(options);
            QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 10000);
            controller.loadDeck(0, QUrl::fromLocalFile(kept));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(controller.decks()[0].toMap()["loaded"].toBool());
            const auto before = controller.decks();
            const auto retained = controller.retainedPcmBytes();
            controller.loadStemDeck(0, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(!controller.error().isEmpty());
            QCOMPARE(controller.decks(), before);
            QCOMPARE(controller.retainedPcmBytes(), retained);
            QCOMPARE(liveFixtureBytes(manifestPath), QByteArray("{}"));
            QVERIFY(!QFileInfo::exists(fixture.marker(".started")));

            controller.importTrack(QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QCOMPARE(controller.libraryTracks().size(), qsizetype(1));
            const auto sourceId = controller.libraryTracks()[0].toMap()["id"].toString();
            QCOMPARE(sourceId, seeded.sourceSha256);
            QVERIFY(writeWave(source, 2, 44100, 213));
            controller.loadLibraryStemTrack(1, sourceId);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(controller.error().contains("Source changed"));
            QCOMPARE(controller.decks(), before);
            QCOMPARE(controller.retainedPcmBytes(), retained);
            QCOMPARE(liveFixtureCount(fixture, "command", "load"), std::size_t(0));
            QCOMPARE(liveFixtureCount(fixture, "command", "seek"), std::size_t(0));
            QCOMPARE(liveFixtureBytes(manifestPath), QByteArray("{}"));
            QCOMPARE(liveFixtureBytes(fixture.engine.exporterPath.left(fixture.engine.exporterPath.size() - 3) + ".calls"), calls);
            QTRY_VERIFY_WITH_TIMEOUT(safety.gone(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(safety.jobsGone(), 10000);
        });
    }

    void queuedLiveCancellationRetiresOnWorkerBeforeEndingBusy()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime;
            QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore;
            isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("cancel live.wav"), kept = directory.filePath("kept.wav");
            QVERIFY(writeWave(source, 2, 44100, 211) && writeWave(kept, 2, 48000, 480));
            const auto engineRoot = directory.filePath("engine");
            QVERIFY(QDir().mkpath(engineRoot));
            const auto fixture = conflux::test::makeStemServiceFixture(engineRoot);
            ControllerServiceSafety safety{fixture};
            conflux::DeckController::PreparationOptions options;
            options.stemEngine = fixture.engine;
            options.stemServicePath = fixture.service;
            conflux::DeckController controller(options);
            QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 10000);
            controller.loadDeck(0, QUrl::fromLocalFile(kept));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(controller.decks()[0].toMap()["loaded"].toBool());
            const auto before = controller.decks();
            const auto retained = controller.retainedPcmBytes();
            controller.loadStemDeck(1, QUrl::fromLocalFile(source));
            QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
            QVERIFY(controller.busy());
            QCOMPARE(liveFixtureCount(fixture, "command", "load"), std::size_t(1));
            safety.capture();
            QVERIFY(!safety.children.empty() && !safety.gone());
            QString job;
            for (const auto& row : liveFixtureRows(fixture))
                if (row.value("fixture") == QJsonValue("started")) job = row.value("fixture_job").toString();
            QVERIFY(!job.isEmpty() && QFileInfo::exists(job));
            // Declared after controller: every failure releases/restores the
            // global pool before controller.shutdown can wait on its worker.
            ControllerRetirementBarrier barrier;
            QVERIFY(controller.cancelPreparation());
            QTRY_VERIFY_WITH_TIMEOUT(barrier.entered(), 10000);
            QCoreApplication::sendPostedEvents();
            const auto guiTurn = std::make_shared<bool>(false);
            QTimer::singleShot(0, &controller, [guiTurn] { *guiTurn = true; });
            QTRY_VERIFY_WITH_TIMEOUT(*guiTurn, 10000);
            QVERIFY(controller.busy());
            QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelling"));
            QCOMPARE(controller.decks()[0], before[0]);
            const auto cancelling = controller.decks()[1].toMap();
            const auto accepted = before[1].toMap();
            QVERIFY(cancelling["loading"].toBool());
            QCOMPARE(cancelling["loadPhase"].toString(), QString("cancel"));
            QCOMPARE(cancelling["pendingName"].toString(), QString("cancel live.wav"));
            QVERIFY(cancelling["pendingWaveform"].toList().isEmpty());
            const QStringList pendingFields{"loading", "pendingName", "pendingPath", "pendingPresentation",
                "pendingWaveform", "loadPhase", "loadPhaseText", "loadCompletedStages", "loadTotalStages", "loadProgress"};
            for (auto it = accepted.cbegin(); it != accepted.cend(); ++it)
                if (!pendingFields.contains(it.key())) QCOMPARE(cancelling[it.key()], it.value());
            QCOMPARE(controller.retainedPcmBytes(), retained);
            QVERIFY(QFileInfo::exists(job) && !safety.gone());
            barrier.release();
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelled"));
            QCOMPARE(controller.decks(), before);
            QCOMPARE(controller.retainedPcmBytes(), retained);
            QTRY_VERIFY_WITH_TIMEOUT(safety.gone(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(safety.jobsGone(), 10000);
            QVERIFY(!QFileInfo::exists(job));
        });
    }

    void optionalStemToolsNeverBlockManualLoading()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        qputenv("XDG_CACHE_HOME", directory.path().toUtf8());
        const auto source = directory.filePath("manual-without-tools.wav");
        QVERIFY(writeWave(source, 2, 48000, 480));
        conflux::DeckController controller;
        QVERIFY(!controller.stemsAvailable());
        QCOMPARE(controller.retainedPcmBytes(), std::uint64_t(0));
        controller.loadStemDeck(0, QUrl::fromLocalFile(source));
        QVERIFY(!controller.busy());
        QVERIFY(controller.error().contains("configured"));
        verifyNoPlayback(controller);
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto previous = controller.decks();
        QVERIFY(previous[0].toMap()["loaded"].toBool());
        QVERIFY(!previous[0].toMap()["stemBank"].toBool());
        QVERIFY(previous[0].toMap()["overviewStem"].toString().isEmpty());
        QCOMPARE(controller.retainedPcmBytes(), controller.audioState().sourceFrames[0] * 8);
        controller.loadStemDeck(0, QUrl::fromLocalFile(source));
        QCOMPARE(controller.decks(), previous);
        for (const auto gain : {-.1, 0., .5, 1., 1.1, std::numeric_limits<double>::quiet_NaN()})
            controller.setStemGain(0, 0, gain);
        QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
        QCOMPARE(controller.decks(), previous);
    }

    void initialStemBankAndManualReloadKeepWholePcmOwners()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        qputenv("XDG_CACHE_HOME", directory.path().toUtf8());
        const auto source = directory.filePath("stem-and-normal.wav");
        QVERIFY(writeWave(source, 2, 48000, 4800));
        const auto engineRoot = directory.filePath("engine");
        QVERIFY(QDir().mkpath(engineRoot));
        conflux::DeckController::PreparationOptions options;
        options.stemEngine = conflux::test::makeStemEngineFixture(engineRoot);
        conflux::DeckController controller(options);
        QVERIFY(controller.stemsAvailable());
        controller.loadStemDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        auto deck = controller.decks()[0].toMap();
        QVERIFY2(deck["loaded"].toBool(), qPrintable(controller.error()));
        QVERIFY(deck["stemBank"].toBool());
        QCOMPARE(deck["overviewStem"].toString(), QString("drums"));
        QVERIFY(!deck["waveform"].toList().isEmpty());
        QCOMPARE(deck["stemGains"].toList(), QVariantList({1.F, 1.F, 1.F, 1.F}));
        const auto bankFrames = controller.audioState().sourceFrames[0];
        QVERIFY(bankFrames > 0);
        QCOMPARE(controller.retainedPcmBytes(), bankFrames * 8 * 4);
        QVERIFY(!controller.outputReady());
        bool retainedAtCutoff = false;
        connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            if (controller.preparation()["status"].toString() == "Installing" &&
                !controller.preparation()["stems"].toBool())
                retainedAtCutoff |= controller.retainedPcmBytes() == bankFrames * 8 * 4 + 4800 * 8;
        }, Qt::DirectConnection);
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(retainedAtCutoff);
        deck = controller.decks()[0].toMap();
        QVERIFY(deck["loaded"].toBool());
        QVERIFY(!deck["stemBank"].toBool());
        QVERIFY(deck["overviewStem"].toString().isEmpty());
        QCOMPARE(controller.retainedPcmBytes(), controller.audioState().sourceFrames[0] * 8);
        controller.loadStemDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(controller.preparation()["cacheHit"].toBool());
        QCOMPARE(controller.retainedPcmBytes(), controller.audioState().sourceFrames[0] * 8 * 4);
        QVERIFY(controller.decks()[0].toMap()["stemBank"].toBool());
    }

    void optionalBpmCannotConsumeTheOnlyStemCacheEntry()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime;
            QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore;
            isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("single-entry.wav");
            QVERIFY(writeWave(source, 2, 48000, 4800));
            const auto engineRoot = directory.filePath("engine");
            QVERIFY(QDir().mkpath(engineRoot));
            conflux::DeckController::PreparationOptions options;
            options.limits.maxEntries = 1;
            options.stemEngine = conflux::test::makeStemEngineFixture(engineRoot);
            const auto marker = directory.filePath("bpm-started");
            const auto worker = directory.filePath("bpm.py");
            // Valid local assets: cache admission, rather than engine setup,
            // must prevent this optional worker from starting.
            conflux::test::writeFixtureBytes(worker, "from pathlib import Path\nPath(" +
                QJsonDocument(QJsonArray{marker}).toJson(QJsonDocument::Compact).mid(1).chopped(1) +
                ").touch()\nprint('unexpected BPM invocation')\n");
            options.bpmEngine = conflux::library::BpmEngine{options.stemEngine->pythonExecutable,
                engineRoot, options.stemEngine->modelPath, worker};
            conflux::DeckController controller(options);
            controller.loadStemDeck(0, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            const auto deck = controller.decks()[0].toMap();
            QVERIFY2(deck["loaded"].toBool(), qPrintable(controller.error()));
            QVERIFY(deck["stemBank"].toBool());
            QCOMPARE(deck["bpm"].toMap()["status"].toString(), QString("Failed"));
            QCOMPARE(deck["bpm"].toMap()["estimated"].toDouble(), 0.);
            QCOMPARE(deck["bpm"].toMap()["reference"].toDouble(), 0.);
            QVERIFY(!QFileInfo::exists(marker));
            QCOMPARE(deck["stemGains"].toList(), QVariantList({1.F, 1.F, 1.F, 1.F}));
            QCOMPARE(deck["overviewStem"].toString(), QString("drums"));

            const auto cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
            const auto entries = QDir(cacheRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            QCOMPARE(entries.size(), 1);
            // Reuse the strict bank reader to verify every role's geometry,
            // digest and manifest; it cannot export or repair a missing bank.
            const auto bank = conflux::library::PreparedTrackStore(cacheRoot, options.limits)
                .findPreparedStemBank(source, *options.stemEngine);
            QVERIFY(bank.has_value());
            QCOMPARE(entries.front(), bank->artifactKey);
            const auto frames = bank->stems.front().frames();
            QVERIFY(frames >= 4799 && frames <= 4801);
            for (const auto& stem : bank->stems) QCOMPARE(stem.frames(), frames);
            QCOMPARE(controller.audioState().sourceFrames[0], frames);
            QCOMPARE(controller.retainedPcmBytes(), frames * 8 * 4);
            QCOMPARE(deck["durationSeconds"].toDouble(), frames / 48000.);
            QVariantList waveform;
            for (const auto peak : bank->stems.front().peaks()) waveform.append(peak);
            QVERIFY(!waveform.isEmpty());
            QCOMPARE(deck["waveform"].toList(), waveform);
            const QDir entry(QDir(cacheRoot).filePath(entries.front()));
            QCOMPARE(entry.entryList(QDir::Files), QStringList({"bass.f32", "drums.f32",
                "manifest.json", "other.f32", "vocals.f32"}));
            QVERIFY(!entry.exists("audio.f32"));
            QCOMPARE(QDir(cacheRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot), entries);
        });
    }

    void failedAndQueuedCancelledBanksPreserveExistingOwners()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        qputenv("XDG_CACHE_HOME", directory.path().toUtf8());
        const auto source = directory.filePath("kept-normal.wav");
        QVERIFY(writeWave(source, 2, 48000, 480));
        const auto engineRoot = directory.filePath("engine");
        QVERIFY(QDir().mkpath(engineRoot));
        conflux::DeckController::PreparationOptions options;
        options.stemEngine = conflux::test::makeStemEngineFixture(engineRoot, "partial");
        conflux::DeckController controller(options);
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto previous = controller.decks();
        const auto retained = controller.retainedPcmBytes();
        controller.loadStemDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.error().isEmpty());
        QCOMPARE(controller.decks(), previous);
        QCOMPARE(controller.retainedPcmBytes(), retained);
        conflux::test::makeStemEngineFixture(engineRoot, "good");
        controller.loadStemDeck(1, QUrl::fromLocalFile(source));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QVERIFY(controller.retainedPcmBytes() > retained);
        // Completion is still queued; cancellation must precede any acceptance.
        QVERIFY(controller.busy());
        QVERIFY(controller.cancelPreparation());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelled"));
        QCOMPARE(controller.decks(), previous);
        QCOMPARE(controller.retainedPcmBytes(), retained);
    }

    void retainedUnlinkedPcmIsReservedBeforeCorruptEntryRebuild()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        qputenv("XDG_CACHE_HOME", directory.path().toUtf8());
        const auto source = directory.filePath("retained-unlinked.wav");
        QVERIFY(writeWave(source, 2, 48000, 4096));
        conflux::DeckController::PreparationOptions options;
        options.limits.maxPreparedBytes = 4096 * 8;
        options.limits.maxCacheBytes = 90000; // Initial publication fits; retained+rebuild does not.
        conflux::DeckController controller(options);
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY2(controller.decks()[0].toMap()["loaded"].toBool(), qPrintable(controller.error()));
        const auto retained = controller.retainedPcmBytes();
        QCOMPARE(retained, std::uint64_t(4096 * 8));
        const auto previous = controller.decks();
        const auto cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
        const auto entries = QDir(cacheRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(entries.size(), 1);
        QFile manifest(QDir(cacheRoot).filePath(entries.front() + "/manifest.json"));
        QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(manifest.write("{}"), qint64(2));
        manifest.close();
        controller.loadDeck(1, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(controller.error().contains("budget", Qt::CaseInsensitive));
        QCOMPARE(controller.decks(), previous);
        QCOMPARE(controller.retainedPcmBytes(), retained);
        // The store removed the bad linked entry; A still retains its old inode.
        QVERIFY(QDir(cacheRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void catalogStartupDoesNotFinishAnActivePreparation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        const auto path = directory.filePath("held-preparation.wav");
        QVERIFY(writeWave(path, 2, 48000, 480));
        const auto root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prepared/v1";
        QVERIFY(QDir().mkpath(root));
        QLockFile cacheLock(root + "/.lock");
        QVERIFY(cacheLock.tryLock(0));
        conflux::DeckController controller;
        bool startupObserved = false, coherent = false;
        connect(&controller, &conflux::DeckController::libraryChanged, &controller, [&] {
            if (!startupObserved && controller.libraryReady()) {
                startupObserved = true;
                coherent = controller.busy() &&
                    controller.preparation()["status"].toString() == "Preparing";
            }
        }, Qt::DirectConnection);
        controller.loadDeck(0, QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(startupObserved, 1500);
        QVERIFY(coherent);
        QVERIFY(controller.busy());
        QVERIFY(controller.cancelPreparation());
        cacheLock.unlock();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        verifyNoPlayback(controller);
    }

    void libraryImportPersistsAndRevalidatesBeforeLoading()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        const auto path = directory.filePath(QString::fromUtf8("Été #1 % mix.wav"));
        QVERIFY(writeWave(path, 2, 48000, 480));
        QString sourceId;
        QVariantMap importedLevels;
        {
            conflux::DeckController controller;
            QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 5000);
            QVERIFY(controller.libraryTracks().isEmpty());
            controller.importTrack(QUrl::fromLocalFile(path));
            QVERIFY(controller.busy());
            QCOMPARE(controller.preparation()["deck"].toInt(), -1);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QVERIFY2(controller.libraryError().isEmpty(), qPrintable(controller.libraryError()));
            QCOMPARE(controller.preparation()["status"].toString(), QString("Complete"));
            QCOMPARE(controller.libraryTracks().size(), qsizetype(1));
            const auto track = controller.libraryTracks().front().toMap();
            sourceId = track["id"].toString();
            QCOMPARE(sourceId.size(), qsizetype(64));
            QCOMPARE(track["path"].toString(), path);
            QCOMPARE(track["name"].toString(), QFileInfo(path).fileName());
            QCOMPARE(track["durationSeconds"].toDouble(), .01);
            QVERIFY(track["available"].toBool());
            importedLevels = track["levels"].toMap();
            QCOMPARE(importedLevels.size(), 2);
            // Exact 48 kHz PCM16 fixture: 960 interleaved samples, 17*n-32.
            QVERIFY(std::abs(importedLevels["rms"].toDouble() - 0.28647775033325895) < 1e-12);
            QCOMPARE(importedLevels["samplePeak"].toDouble(), 0.496551513671875);
            verifyNoPlayback(controller);
            QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
        }
        conflux::DeckController reopened;
        QTRY_VERIFY_WITH_TIMEOUT(reopened.libraryReady(), 5000);
        QCOMPARE(reopened.libraryTracks().size(), qsizetype(1));
        QCOMPARE(reopened.libraryTracks().front().toMap()["id"].toString(), sourceId);
        QCOMPARE(reopened.libraryTracks().front().toMap()["levels"].toMap(), importedLevels);
        reopened.loadLibraryTrack(1, sourceId);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.busy(), 5000);
        QVERIFY(reopened.decks()[1].toMap()["loaded"].toBool());
        QCOMPARE(reopened.decks()[1].toMap()["durationSeconds"].toDouble(), .01);
        const auto previous = reopened.decks();
        const auto commands = reopened.submittedCommands();
        QVERIFY(writeWave(path, 2, 48000, 960));
        reopened.loadLibraryTrack(0, sourceId);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.busy(), 5000);
        QVERIFY(reopened.error().contains("changed", Qt::CaseInsensitive));
        QCOMPARE(reopened.decks(), previous);
        QCOMPARE(reopened.submittedCommands(), commands);
        QCOMPARE(reopened.libraryTracks().front().toMap()["id"].toString(), sourceId);
        reopened.importTrack(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.busy(), 5000);
        QCOMPARE(reopened.libraryTracks().size(), qsizetype(1));
        QVERIFY(reopened.libraryTracks().front().toMap()["id"].toString() != sourceId);
        QCOMPARE(reopened.libraryTracks().front().toMap()["durationSeconds"].toDouble(), .02);
        const auto updatedLevels = reopened.libraryTracks().front().toMap()["levels"].toMap();
        QCOMPARE(updatedLevels.size(), 2);
        QVERIFY(std::abs(updatedLevels["rms"].toDouble() - 0.5740249333995456) < 1e-12);
        QCOMPARE(updatedLevels["samplePeak"].toDouble(), 0.994598388671875);
        const auto updatedId = reopened.libraryTracks().front().toMap()["id"].toString();
        QVERIFY(QFile::remove(path));
        reopened.loadLibraryTrack(0, updatedId);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.busy(), 5000);
        QVERIFY(!reopened.error().isEmpty());
        QCOMPARE(reopened.decks(), previous);
        QCOMPARE(reopened.submittedCommands(), commands);
    }

    void legacyLevelsStayUnknownOnLoadAndRefreshOnReimport()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto path = directory.filePath("legacy-levels.wav");
        QVERIFY(writeWave(path, 2, 48000, 480));
        QString sourceId;
        {
            conflux::DeckController controller;
            QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 5000);
            controller.importTrack(QUrl::fromLocalFile(path));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QCOMPARE(controller.libraryTracks().size(), qsizetype(1));
            sourceId = controller.libraryTracks().front().toMap()["id"].toString();
        }
        QFile catalog(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/library.json");
        QVERIFY(catalog.open(QIODevice::ReadOnly));
        auto saved = QJsonDocument::fromJson(catalog.readAll()).object();
        catalog.close();
        auto legacy = saved["tracks"].toArray().first().toObject();
        legacy.remove("levels");
        saved["version"] = 2;
        saved["tracks"] = QJsonArray{legacy};
        const auto bytes = QJsonDocument(saved).toJson(QJsonDocument::Compact);
        QVERIFY(catalog.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(catalog.write(bytes), bytes.size());
        catalog.close();

        conflux::DeckController reopened;
        QTRY_VERIFY_WITH_TIMEOUT(reopened.libraryReady(), 5000);
        QCOMPARE(reopened.libraryTracks().size(), qsizetype(1));
        const auto unknown = reopened.libraryTracks().front().toMap();
        QVERIFY(unknown.contains("levels"));
        QVERIFY(unknown["levels"].toMap().isEmpty());
        reopened.loadLibraryTrack(0, sourceId);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.busy(), 5000);
        QVERIFY(reopened.decks()[0].toMap()["loaded"].toBool());
        QCOMPARE(reopened.libraryTracks().front().toMap(), unknown); // Loading never adds analysis.
        const auto decks = reopened.decks();
        const auto commands = reopened.submittedCommands();
        reopened.importTrack(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.busy(), 5000);
        QVERIFY(reopened.preparation()["cacheHit"].toBool());
        const auto refreshed = reopened.libraryTracks().front().toMap();
        QCOMPARE(refreshed["id"].toString(), sourceId);
        QCOMPARE(refreshed["levels"].toMap().size(), 2);
        QCOMPARE(refreshed["levels"].toMap()["samplePeak"].toDouble(), 0.496551513671875);
        QCOMPARE(reopened.decks(), decks);
        QCOMPARE(reopened.submittedCommands(), commands);
    }

    void cueImportPersistsWithoutChangingLoadedDecks()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto source = directory.filePath(QString::fromUtf8("Été #1 %.wav"));
        const auto other = directory.filePath("other.wav");
        const auto sheet = directory.filePath(QString::fromUtf8("Été #1 %.cue"));
        QVERIFY(writeWave(source, 2, 48000, 1920));
        QVERIFY(writeWave(other, 2, 48000, 3840));
        QVERIFY(writeCue(sheet, source));
        QString sourceId;
        {
            conflux::DeckController controller;
            QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 5000);
            controller.importTrack(QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            sourceId = controller.libraryTracks().front().toMap()["id"].toString();
            controller.loadDeck(0, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            controller.loadDeck(1, QUrl::fromLocalFile(other));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QCOMPARE(controller.decks()[0].toMap()["sourceId"].toString(), sourceId);
            QVERIFY(controller.decks()[1].toMap()["sourceId"].toString() != sourceId);
            const auto before = controller.decks();
            const auto commands = controller.submittedCommands();
            controller.importCueSheet(sourceId, QUrl::fromLocalFile(sheet));
            QVERIFY(controller.busy());
            QCOMPARE(controller.preparation()["deck"].toInt(), -1);
            QVERIFY(controller.preparation()["cancellable"].toBool());
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
            const auto cues = controller.libraryTracks().front().toMap()["cues"].toList();
            QCOMPARE(cues.size(), 2);
            QCOMPARE(cues[0].toMap()["frame"].toULongLong(), qulonglong(0));
            QCOMPARE(cues[0].toMap()["label"].toString(), QString("<b>Drop</b> # %"));
            QCOMPARE(cues[1].toMap()["frame"].toULongLong(), qulonglong(640));
            QCOMPARE(cues[1].toMap()["seconds"].toDouble(), 640 / 48000.0);
            QCOMPARE(controller.decks(), before);
            QCOMPARE(controller.submittedCommands(), commands);
            QVERIFY(!controller.jumpToImportedCue(1, sourceId, 0));
            QVERIFY(!controller.jumpToImportedCue(-1, sourceId, 0));
            QVERIFY(!controller.jumpToImportedCue(0, sourceId, -1));
            QVERIFY(!controller.jumpToImportedCue(0, sourceId, 2));
            // Matching identity does not bypass the ordinary output-ready gate.
            QVERIFY(!controller.jumpToImportedCue(0, sourceId, 1));
            QVERIFY(controller.error().contains("not ready"));
            QCOMPARE(controller.decks(), before);
            QCOMPARE(controller.submittedCommands(), commands);
        }
        conflux::DeckController reopened;
        QTRY_VERIFY_WITH_TIMEOUT(reopened.libraryReady(), 5000);
        QCOMPARE(reopened.libraryTracks().front().toMap()["id"].toString(), sourceId);
        QCOMPARE(reopened.libraryTracks().front().toMap()["cues"].toList().size(), 2);
        verifyNoPlayback(reopened);
    }

    void cueImportFailuresPreserveAcceptedCuesAndPlayback()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto source = directory.filePath("kept.wav"), sheet = directory.filePath("kept.cue");
        QVERIFY(writeWave(source, 2, 48000, 1920));
        QVERIFY(writeCue(sheet, source));
        conflux::DeckController controller;
        QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 5000);
        controller.importTrack(QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto id = controller.libraryTracks().front().toMap()["id"].toString();
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        controller.importCueSheet(id, QUrl::fromLocalFile(sheet));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto saved = controller.libraryTracks(), decks = controller.decks();
        QCOMPARE(saved.front().toMap()["cues"].toList().size(), 2);
        const auto commands = controller.submittedCommands();
        controller.importCueSheet("unknown", QUrl::fromLocalFile(sheet));
        QVERIFY(!controller.busy());
        QVERIFY(!controller.error().isEmpty());
        controller.importCueSheet(id, QUrl("https://example.invalid/remote.cue"));
        QVERIFY(!controller.busy());
        QVERIFY(!controller.error().isEmpty());
        QVERIFY(writeCue(sheet, source, " TRACK 01 AUDIO\n INDEX 01 00:00:75\n"));
        controller.importCueSheet(id, QUrl::fromLocalFile(sheet));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.preparation()["status"].toString(), QString("Failed"));
        QVERIFY(!controller.error().isEmpty());
        QCOMPARE(controller.libraryTracks(), saved);
        QVERIFY(writeCue(sheet, source));
        QVERIFY(writeWave(source, 2, 48000, 3840));
        controller.importCueSheet(id, QUrl::fromLocalFile(sheet));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(!controller.error().isEmpty());
        QCOMPARE(controller.libraryTracks(), saved);
        QCOMPARE(controller.decks(), decks);
        QCOMPARE(controller.submittedCommands(), commands);
    }

    void queuedCueCancellationPreservesCatalogAndEndsAtSaving()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        qputenv("XDG_CACHE_HOME", directory.path().toUtf8());
        const auto source = directory.filePath("cancel-cues.wav"), sheet = directory.filePath("cancel.cue");
        QVERIFY(writeWave(source, 2, 48000, 1920));
        QVERIFY(writeCue(sheet, source));
        conflux::DeckController controller;
        QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 5000);
        controller.importTrack(QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto before = controller.libraryTracks();
        const auto id = before.front().toMap()["id"].toString();
        controller.importCueSheet(id, QUrl::fromLocalFile(sheet));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QVERIFY(controller.cancelPreparation());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelled"));
        QCOMPARE(controller.libraryTracks(), before);
        bool saving = false, lateCancellation = true;
        connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            if (!saving && controller.preparation()["status"].toString() == "Saving") {
                saving = true;
                lateCancellation = controller.cancelPreparation();
            }
        }, Qt::DirectConnection);
        controller.importCueSheet(id, QUrl::fromLocalFile(sheet));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(saving);
        QVERIFY(!lateCancellation);
        QCOMPARE(controller.libraryTracks().front().toMap()["cues"].toList().size(), 2);
        verifyNoPlayback(controller);
    }

    void cueSavingShutdownJoinsAcceptedCatalogWrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        qputenv("XDG_CACHE_HOME", directory.path().toUtf8());
        const auto source = directory.filePath("shutdown-cues.wav"), sheet = directory.filePath("shutdown.cue");
        QVERIFY(writeWave(source, 2, 48000, 1920));
        QVERIFY(writeCue(sheet, source));
        conflux::DeckController controller;
        QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 5000);
        controller.importTrack(QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto id = controller.libraryTracks().front().toMap()["id"].toString();
        bool stopped = false;
        connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            if (!stopped && controller.preparation()["status"].toString() == "Saving") {
                stopped = true;
                controller.shutdown();
            }
        }, Qt::DirectConnection);
        controller.importCueSheet(id, QUrl::fromLocalFile(sheet));
        QTRY_VERIFY_WITH_TIMEOUT(stopped, 5000);
        QVERIFY(!controller.busy());
        conflux::DeckController reopened;
        QTRY_VERIFY_WITH_TIMEOUT(reopened.libraryReady(), 5000);
        QCOMPARE(reopened.libraryTracks().front().toMap()["cues"].toList().size(), 2);
        verifyNoPlayback(reopened);
    }

    void queuedImportCancellationPreservesCatalogAndEndsAtSaving()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        const auto path = directory.filePath("queued-library-import.wav");
        QVERIFY(writeWave(path, 2, 48000, 480));
        conflux::DeckController controller;
        QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 5000);
        controller.importTrack(QUrl::fromLocalFile(path));
        QVERIFY(controller.busy());
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QVERIFY(controller.cancelPreparation());
        QVERIFY(controller.busy());
        controller.importTrack(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelled"));
        QVERIFY(controller.libraryTracks().isEmpty());
        verifyNoPlayback(controller);
        {
            conflux::DeckController reopened;
            QTRY_VERIFY_WITH_TIMEOUT(reopened.libraryReady(), 5000);
            QVERIFY(reopened.libraryTracks().isEmpty());
        }
        bool savingObserved = false, lateAccepted = true;
        connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            if (!savingObserved && controller.preparation()["status"].toString() == "Saving") {
                savingObserved = true;
                lateAccepted = controller.cancelPreparation();
            }
        }, Qt::DirectConnection);
        controller.importTrack(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(savingObserved);
        QVERIFY(!lateAccepted);
        QCOMPARE(controller.libraryTracks().size(), qsizetype(1));
        QVERIFY(controller.preparation()["cacheHit"].toBool());
        QCOMPARE(controller.libraryTracks().front().toMap()["levels"].toMap().size(), 2);
        const auto accepted = controller.libraryTracks();
        QVERIFY(writeWave(path, 2, 48000, 960));
        controller.importTrack(QUrl::fromLocalFile(path));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QVERIFY(controller.cancelPreparation());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelled"));
        QCOMPARE(controller.libraryTracks(), accepted); // A completed scan is still cancellable before handoff.
        conflux::DeckController reopened;
        QTRY_VERIFY_WITH_TIMEOUT(reopened.libraryReady(), 5000);
        QCOMPARE(reopened.libraryTracks(), accepted);
        verifyNoPlayback(controller);
    }

    void savingSignalShutdownOwnsTheCatalogWrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        const auto path = directory.filePath("save-before-shutdown.wav");
        QVERIFY(writeWave(path, 2, 48000, 480));
        conflux::DeckController controller;
        QTRY_VERIFY_WITH_TIMEOUT(controller.libraryReady(), 5000);
        bool stopped = false;
        connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            if (!stopped && controller.preparation()["status"].toString() == "Saving") {
                stopped = true;
                controller.shutdown();
            }
        }, Qt::DirectConnection);
        controller.importTrack(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(stopped, 5000);
        QVERIFY(!controller.busy());
        verifyNoPlayback(controller);
        conflux::DeckController reopened;
        QTRY_VERIFY_WITH_TIMEOUT(reopened.libraryReady(), 5000);
        QCOMPARE(reopened.libraryTracks().size(), qsizetype(1));
        QCOMPARE(reopened.libraryTracks().front().toMap()["path"].toString(), path);
    }

    void damagedCatalogFailsClosedWithoutBlockingRawLoad()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(data));
        QFile catalog(data + "/library.json");
        QVERIFY(catalog.open(QIODevice::WriteOnly));
        const QByteArray damaged("{\"version\":1,\"tracks\":[broken");
        QCOMPARE(catalog.write(damaged), qint64(damaged.size()));
        catalog.close();
        const auto path = directory.filePath("still-playable.wav");
        QVERIFY(writeWave(path, 2, 48000, 480));
        conflux::DeckController controller;
        QTRY_VERIFY_WITH_TIMEOUT(!controller.libraryError().isEmpty(), 5000);
        QVERIFY(!controller.libraryReady());
        controller.importTrack(QUrl::fromLocalFile(path));
        QVERIFY(!controller.busy());
        QVERIFY(catalog.open(QIODevice::ReadOnly));
        QCOMPARE(catalog.readAll(), damaged);
        catalog.close();
        controller.loadDeck(0, QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(controller.decks()[0].toMap()["loaded"].toBool());
        QVERIFY(!controller.libraryError().isEmpty());
    }

    void initialState()
    {
        conflux::DeckController controller;
        QVERIFY(!controller.busy());
        QVERIFY(controller.error().isEmpty());
        QCOMPARE(controller.masterGain(), 1.0);
        QCOMPARE(controller.crossfader(), .5);
        QVERIFY(controller.property("crossfader").isValid());
        QCOMPARE(controller.property("crossfader").toDouble(), .5);
        verifyNoPlayback(controller);
        controller.shutdown();
        controller.shutdown();
        verifyNoPlayback(controller);
    }

    void crossfaderRequiresReadyOutputAndPreservesLoadedDecks()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto a = directory.filePath("crossfader-a.wav"), b = directory.filePath("crossfader-b.wav");
        QVERIFY(writeWave(a, 2, 48000, 480));
        QVERIFY(writeWave(b, 2, 48000, 960));
        conflux::DeckController controller;
        controller.loadDeck(0, QUrl::fromLocalFile(a));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        controller.loadDeck(1, QUrl::fromLocalFile(b));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(controller.decks()[0].toMap()["loaded"].toBool());
        QVERIFY(controller.decks()[1].toMap()["loaded"].toBool());
        QVERIFY(!controller.outputReady());
        const auto decks = controller.decks();
        const auto recording = controller.recording();
        const auto master = controller.masterGain(), cue = controller.cueGain();
        const auto submitted = controller.submittedCommands();
        const auto sessions = controller.completedSessions();

        // A valid position still requires current output readiness; it cannot
        // become a speculative UI value while no callback can acknowledge it.
        for (const auto position : {0., .25, .5, .75, 1.}) {
            QSignalSpy changed(&controller, &conflux::DeckController::stateChanged);
            controller.setCrossfader(position);
            QVERIFY(!controller.error().isEmpty());
            QVERIFY(!changed.isEmpty());
            QCOMPARE(controller.crossfader(), .5);
            QCOMPARE(controller.property("crossfader").toDouble(), .5);
            QCOMPARE(controller.decks(), decks);
            QCOMPARE(controller.masterGain(), master);
            QCOMPARE(controller.cueGain(), cue);
            QCOMPARE(controller.recording(), recording);
            QCOMPARE(controller.submittedCommands(), submitted);
        }
        const auto outputNotReady = controller.error();
        for (const auto position : {-.1, 1.1, std::nextafter(0., -1.), std::nextafter(1., 2.),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            controller.setCrossfader(position);
            QVERIFY(!controller.error().isEmpty());
            // Invalid values must fail their own validation rather than just
            // happen to be rejected by the unavailable-output gate.
            QVERIFY(controller.error() != outputNotReady);
            QCOMPARE(controller.crossfader(), .5);
            QCOMPARE(controller.decks(), decks);
            QCOMPARE(controller.masterGain(), master);
            QCOMPARE(controller.cueGain(), cue);
            QCOMPARE(controller.recording(), recording);
            QCOMPARE(controller.submittedCommands(), submitted);
        }
        QCOMPARE(controller.completedSessions(), sessions);
        controller.shutdown();
        controller.setCrossfader(.75);
        QCOMPARE(controller.crossfader(), .5);
        QCOMPARE(controller.submittedCommands(), submitted);
        QVERIFY(!controller.outputReady());
    }

    void channelEqRequiresReadyOutputAndRejectsInvalidControls()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto a = directory.filePath("eq-a.wav"), b = directory.filePath("eq-b.wav");
        QVERIFY(writeWave(a, 2, 48000, 480));
        QVERIFY(writeWave(b, 2, 48000, 960));
        conflux::DeckController controller;
        controller.loadDeck(0, QUrl::fromLocalFile(a));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        controller.loadDeck(1, QUrl::fromLocalFile(b));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(!controller.outputReady());
        const auto decks = controller.decks();
        for (const auto& deck : decks)
            QCOMPARE(deck.toMap()["eqGainDb"].toList(), QVariantList({0.F, 0.F, 0.F}));
        const auto state = controller.audioState();
        const auto submitted = controller.submittedCommands();
        const auto recording = controller.recording();
        for (int deck = 0; deck < 2; ++deck)
            for (int band = 0; band < 3; ++band)
                for (const auto db : {-26., 0., 6.}) {
                    controller.setEqGainDb(deck, band, db);
                    QVERIFY(!controller.error().isEmpty());
                    QCOMPARE(controller.decks(), decks);
                    QCOMPARE(controller.audioState().eqGainDb, state.eqGainDb);
                    QCOMPARE(controller.submittedCommands(), submitted);
                }
        const auto outputNotReady = controller.error();
        for (const auto db : {std::nextafter(-26., -27.), std::nextafter(6., 7.),
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            controller.setEqGainDb(0, 1, db);
            QVERIFY(!controller.error().isEmpty() && controller.error() != outputNotReady);
            QCOMPARE(controller.decks(), decks);
            QCOMPARE(controller.submittedCommands(), submitted);
        }
        for (const auto indices : {std::pair{-1, 0}, std::pair{2, 0}, std::pair{0, -1}, std::pair{1, 3}}) {
            controller.setEqGainDb(indices.first, indices.second, 1.);
            QVERIFY(!controller.error().isEmpty() && controller.error() != outputNotReady);
            QCOMPARE(controller.decks(), decks);
            QCOMPARE(controller.submittedCommands(), submitted);
        }
        QCOMPARE(controller.recording(), recording);
        QCOMPARE(controller.masterGain(), double(state.masterGain));
        QCOMPARE(controller.cueGain(), double(state.cueGain));
        QCOMPARE(controller.crossfader(), double(state.crossfader));
        controller.shutdown();
        controller.setEqGainDb(0, 0, -9.);
        QCOMPARE(controller.audioState().eqGainDb, state.eqGainDb);
        QCOMPARE(controller.submittedCommands(), submitted);
    }

    void manualLoopsRejectInvalidUnloadedAndUnreadyDecksWithoutSpeculation()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        conflux::DeckController controller;
        const auto commands = {&conflux::DeckController::loopIn, &conflux::DeckController::loopOut,
                               &conflux::DeckController::loopToggle};
        const auto empty = controller.decks();
        for (const int deck : {-1, 0, 1, 2}) {
            for (const auto command : commands) {
                (controller.*command)(deck);
                QVERIFY(controller.error().contains("prepared", Qt::CaseInsensitive));
                QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
                QCOMPARE(controller.decks(), empty);
            }
        }
        const auto a = directory.filePath("loop-a.wav"), b = directory.filePath("loop-b.wav");
        QVERIFY(writeWave(a, 2, 48000, 4800) && writeWave(b, 2, 48000, 9600));
        for (int deck = 0; deck < 2; ++deck) {
            controller.loadDeck(deck, QUrl::fromLocalFile(deck == 0 ? a : b));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QVERIFY(controller.decks()[deck].toMap()["loaded"].toBool());
        }
        QVERIFY(!controller.outputReady());
        const auto decks = controller.decks();
        for (const auto& value : decks) {
            const auto loop = value.toMap()["loop"].toMap();
            for (const auto* key : {"supported", "inSet", "ready", "outSet", "active", "cached", "inSeconds", "minimumOutSeconds",
                                    "outSeconds", "limitSeconds", "residentBytes", "errorCode"})
                QVERIFY2(loop.contains(key), key);
            QVERIFY(!loop["inSet"].toBool() && !loop["outSet"].toBool() && !loop["active"].toBool());
            QCOMPARE(loop["minimumOutSeconds"].toDouble(), 256.0 / 48000.0);
        }
        const auto recording = controller.recording();
        const auto submitted = controller.submittedCommands();
        const auto sessions = controller.completedSessions();
        const auto master = controller.masterGain(), cue = controller.cueGain(), crossfader = controller.crossfader();
        for (int deck = 0; deck < 2; ++deck) {
            for (const auto command : commands) {
                QSignalSpy changed(&controller, &conflux::DeckController::stateChanged);
                (controller.*command)(deck);
                QVERIFY(controller.error().contains("not ready", Qt::CaseInsensitive));
                QVERIFY(!changed.isEmpty());
                QCOMPARE(controller.decks(), decks); // Includes both peers, transport, CUE and loop flags.
                QCOMPARE(controller.submittedCommands(), submitted);
                QCOMPARE(controller.recording(), recording);
                QCOMPARE(controller.masterGain(), master);
                QCOMPARE(controller.cueGain(), cue);
                QCOMPARE(controller.crossfader(), crossfader);
            }
        }
        QCOMPARE(controller.completedSessions(), sessions);
        controller.shutdown();
        for (const auto command : commands) (controller.*command)(0);
        QCOMPARE(controller.submittedCommands(), submitted);
        QVERIFY(!controller.outputReady());
    }

    void tempoRejectsInvalidUnloadedAndUnreadyDecksWithoutSpeculation()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        conflux::DeckController controller;
        const auto selectionCommands = {&conflux::DeckController::toggleMasterTempo,
                                        &conflux::DeckController::resetTempo};
        const auto empty = controller.decks();
        for (const int deck : {-1, 0, 1, 2}) {
            controller.setTempoPercent(deck, 1);
            QVERIFY(controller.error().contains("prepared", Qt::CaseInsensitive));
            for (const auto command : selectionCommands) {
                (controller.*command)(deck);
                QVERIFY(controller.error().contains("prepared", Qt::CaseInsensitive));
            }
            QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
            QCOMPARE(controller.decks(), empty);
        }
        const auto a = directory.filePath("tempo-a.wav"), b = directory.filePath("tempo-b.wav");
        QVERIFY(writeWave(a, 2, 48000, 4800) && writeWave(b, 2, 48000, 9600));
        for (int deck = 0; deck < 2; ++deck) {
            controller.loadDeck(deck, QUrl::fromLocalFile(deck == 0 ? a : b));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QVERIFY(controller.decks()[deck].toMap()["loaded"].toBool());
        }
        QVERIFY(!controller.outputReady());
        const auto decks = controller.decks();
        const auto state = controller.audioState();
        for (int deck = 0; deck < 2; ++deck) {
            const auto tempo = decks[deck].toMap()["tempo"].toMap();
            QCOMPARE(tempo.size(), qsizetype(9));
            for (const auto* key : {"supported", "percent", "appliedPercent", "keyLock", "active",
                                    "pending", "finished", "errorCode", "generation"})
                QVERIFY2(tempo.contains(key), key);
            for (const auto* key : {"supported", "keyLock", "active", "pending", "finished"})
                QCOMPARE(tempo[key].metaType().id(), int(QMetaType::Bool));
            QCOMPARE(tempo["percent"].metaType().id(), int(QMetaType::Double));
            QCOMPARE(tempo["appliedPercent"].metaType().id(), int(QMetaType::Double));
            QCOMPARE(tempo["generation"].metaType().id(), int(QMetaType::ULongLong));
            const auto& acknowledged = state.tempo[deck];
            QCOMPARE(tempo["supported"].toBool(), acknowledged.supported);
            QCOMPARE(tempo["percent"].toDouble(), acknowledged.percent);
            QCOMPARE(tempo["appliedPercent"].toDouble(), acknowledged.appliedPercent);
            QCOMPARE(tempo["keyLock"].toBool(), acknowledged.keyLock);
            QCOMPARE(tempo["active"].toBool(), acknowledged.active);
            QCOMPARE(tempo["pending"].toBool(), acknowledged.pending);
            QCOMPARE(tempo["finished"].toBool(), acknowledged.finished);
            QCOMPARE(tempo["errorCode"].toInt(), acknowledged.errorCode);
            QCOMPARE(tempo["generation"].toULongLong(), qulonglong(acknowledged.generation));
        }
        const auto submitted = controller.submittedCommands(), sessions = controller.completedSessions();
        const auto recording = controller.recording();
        const auto master = controller.masterGain(), cue = controller.cueGain(), crossfader = controller.crossfader();
        const auto unchanged = [&] {
            QCOMPARE(controller.decks(), decks); // Includes peer, cursor, CUE, loop and acknowledged tempo fields.
            QCOMPARE(controller.submittedCommands(), submitted);
            QCOMPARE(controller.recording(), recording);
            QCOMPARE(controller.masterGain(), master);
            QCOMPARE(controller.cueGain(), cue);
            QCOMPARE(controller.crossfader(), crossfader);
            QCOMPARE(controller.completedSessions(), sessions);
        };
        for (int deck = 0; deck < 2; ++deck) {
            for (const double percent : {-6., 0., 6.}) {
                QSignalSpy changed(&controller, &conflux::DeckController::stateChanged);
                controller.setTempoPercent(deck, percent);
                QVERIFY(controller.error().contains("not ready", Qt::CaseInsensitive));
                QVERIFY(!changed.isEmpty());
                unchanged();
            }
            for (const auto command : selectionCommands) {
                (controller.*command)(deck);
                QVERIFY(controller.error().contains("not ready", Qt::CaseInsensitive));
                unchanged();
            }
            const auto outputNotReady = controller.error();
            for (const double percent : {std::nextafter(-6., -7.), std::nextafter(6., 7.),
                                         std::numeric_limits<double>::infinity(),
                                         -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
                controller.setTempoPercent(deck, percent);
                QVERIFY(controller.error().contains("between -6 and +6"));
                QVERIFY(controller.error() != outputNotReady); // Range validation must precede the submit readiness guard.
                unchanged();
            }
        }
        for (const int deck : {-1, 2}) {
            controller.setTempoPercent(deck, 1);
            QVERIFY(controller.error().contains("prepared", Qt::CaseInsensitive));
            for (const auto command : selectionCommands) (controller.*command)(deck);
            unchanged();
        }
        controller.shutdown();
        controller.setTempoPercent(0, 1);
        for (const auto command : selectionCommands) (controller.*command)(0);
        QCOMPARE(controller.submittedCommands(), submitted);
        QVERIFY(!controller.outputReady());
    }

    void localTimingProjectionUsesSourceIntervalsAndRecoveryCursor()
    {
        using conflux::ui::localTimingForDisplay;
        conflux::library::BpmEstimate estimate;
        estimate.analysisFrames = 144000; estimate.pcmSha256 = QByteArray(64, 'a');
        estimate.status = "Unknown"; // A null whole-track summary can still have local timing.
        estimate.beats = {{4800, true}, {28800, false}, {52800, true}, {70800, false},
                          {86800, false}, {101200, true}, {113200, false}};
        struct Row { double position; qulonglong first, end; double bpm; };
        for (const auto row : {Row{52800.75,52800,70800,160}, Row{52799.75,28800,52800,120},
                              Row{70800,70800,86800,180}, Row{4800,4800,28800,120},
                              Row{113199.75,101200,113200,240}}) {
            const auto local = localTimingForDisplay(estimate, row.position);
            QCOMPARE(local["status"].toString(), QString("Estimated"));
            QCOMPARE(local["confidence"].toString(), QString("Unknown"));
            QCOMPARE(local["firstFrame"].toULongLong(), row.first);
            QCOMPARE(local["endFrame"].toULongLong(), row.end);
            QCOMPARE(local["sourceFrame"].toULongLong(), qulonglong(std::floor(row.position)));
            QCOMPARE(local["bpm"].toDouble(), row.bpm);
        }
        for (const auto position : {-1., 0., 4799.99, 113200., 144000.,
                                    std::numeric_limits<double>::quiet_NaN(),
                                    std::numeric_limits<double>::infinity()})
            QCOMPARE(localTimingForDisplay(estimate, position)["status"].toString(), QString("Unknown"));
        const conflux::audio::AudioResidentHoldRange range{7,52800,101200,
            (101200-52800)*conflux::audio::AudioResidentHoldRange::bytesPerFrame,
            conflux::audio::AudioResidentHoldRange::Provenance::DjRange};
        conflux::audio::AudioResidentHoldState hold{range.id,range.inFrame,range.outFrame,128000,70800,
            range.admittedBytes,range.provenance,EIO,true,true,true,true,false};
        const auto resident = localTimingForDisplay(estimate, 128000, hold, range);
        QCOMPARE(resident["status"].toString(), QString("Estimated"));
        QCOMPARE(resident["sourceFrame"].toULongLong(), qulonglong(70800));
        QCOMPARE(resident["bpm"].toDouble(), 180.);
        QVERIFY(resident["resident"].toBool()); // Producer health does not erase independent timing.
        hold.linearFrame = estimate.analysisFrames;
        QCOMPARE(localTimingForDisplay(estimate, double(estimate.analysisFrames), hold, range)["bpm"].toDouble(), 180.);
        hold.linearFrame = 128000;
        QCOMPARE(localTimingForDisplay(estimate, 128000.5, hold, range)["status"].toString(), QString("Unknown"));
        hold.audibleFrame = range.outFrame;
        QCOMPARE(localTimingForDisplay(estimate, 128000, hold, range)["status"].toString(), QString("Unknown"));
        hold.audibleFrame = range.inFrame;
        QCOMPARE(localTimingForDisplay(estimate, 128000, hold, range)["bpm"].toDouble(), 160.);
        ++hold.rangeId;
        QCOMPARE(localTimingForDisplay(estimate, 128000, hold, range)["status"].toString(), QString("Unknown"));
        --hold.rangeId; ++hold.linearFrame;
        QCOMPARE(localTimingForDisplay(estimate, 128000, hold, range)["status"].toString(), QString("Unknown"));
        --hold.linearFrame; hold.active = false;
        QCOMPARE(localTimingForDisplay(estimate, 52800.5, hold, range)["bpm"].toDouble(), 160.);
        auto failed = estimate; failed.status = "Failed";
        QCOMPARE(localTimingForDisplay(failed, 52800)["status"].toString(), QString("Unknown"));
        auto malformed = estimate; malformed.beats.back().frame = malformed.beats.front().frame;
        QCOMPARE(localTimingForDisplay(malformed, 52800)["status"].toString(), QString("Unknown"));
        QCOMPARE(estimate.status, QString("Unknown")); QVERIFY(!estimate.bpm);
    }

    void localTimingIsSourceBoundAndIndependentOfReference()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto source = directory.filePath("local.wav"), other = directory.filePath("other.wav");
        QVERIFY(writeWave(source, 2, 48000, 192000) && writeWave(other, 2, 48000, 96000));
        const auto script = directory.filePath("bpm.py"), model = directory.filePath("model");
        QFile asset(model); QVERIFY(asset.open(QIODevice::WriteOnly)); asset.write("fixture"); asset.close();
        QFile worker(script); QVERIFY(worker.open(QIODevice::WriteOnly));
        worker.write("import argparse, hashlib, json, sys\np=argparse.ArgumentParser()\np.add_argument('--source-dir')\np.add_argument('--model')\np.add_argument('--frames',type=int)\na=p.parse_args()\nb=sys.stdin.buffer.read()\n");
        worker.write("events=(0,24000,48000,72000,88000,104000,128000) if a.frames==192000 else (4800,28800,46800)\n");
        worker.write("print(json.dumps(dict(schema=2,recipe='beat-this-final0-linear-mel-v1',frames=a.frames,pcm_sha256=hashlib.sha256(b).hexdigest(),bpm=120.0,beat_count=len(events))))\nfor f in events: print(json.dumps(dict(frame=f,downbeat=(f==events[0]))))\n");
        worker.close();
        conflux::DeckController::PreparationOptions options;
        options.bpmEngine = conflux::library::BpmEngine{"/usr/bin/python3", directory.path(), model, script};
        conflux::DeckController controller(options);
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(controller.decks()[0].toMap()["loaded"].toBool());
        const auto local = controller.decks()[0].toMap()["localTiming"].toMap();
        QCOMPARE(local["status"].toString(), QString("Estimated"));
        QCOMPARE(local["confidence"].toString(), QString("Unknown"));
        QCOMPARE(local["bpm"].toDouble(), 120.);
        QCOMPARE(local["firstFrame"].toULongLong(), qulonglong(0));
        QCOMPARE(local["endFrame"].toULongLong(), qulonglong(24000));
        QCOMPARE(local["sourceFrame"].toULongLong(), qulonglong(0));
        const auto submitted = controller.submittedCommands();
        controller.setBpmOverride(0, 128);
        QCOMPARE(controller.decks()[0].toMap()["bpm"].toMap()["reference"].toDouble(), 128.);
        QCOMPARE(controller.decks()[0].toMap()["localTiming"].toMap(), local);
        QCOMPARE(controller.submittedCommands(), submitted);
        controller.loadDeck(0, QUrl::fromLocalFile(directory.filePath("missing.wav")));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.decks()[0].toMap()["localTiming"].toMap(), local);
        controller.loadDeck(0, QUrl::fromLocalFile(other));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto changed = controller.decks()[0].toMap();
        QCOMPARE(changed["bpm"].toMap()["estimated"].toDouble(), 120.);
        QCOMPARE(changed["localTiming"].toMap()["status"].toString(), QString("Unknown"));
        QVERIFY(changed["localTiming"].toMap()["positionKnown"].toBool());
        // A playable track with failed analysis cannot inherit the old map.
        QVERIFY(worker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        worker.write("print('invalid output')\n"); worker.close();
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto failed = controller.decks()[0].toMap();
        QVERIFY(failed["loaded"].toBool());
        QCOMPARE(failed["bpm"].toMap()["status"].toString(), QString("Failed"));
        QCOMPARE(failed["localTiming"].toMap()["status"].toString(), QString("Unknown"));
        QVERIFY(!failed["localTiming"].toMap()["positionKnown"].toBool());
        QCOMPARE(controller.submittedCommands(), submitted);
    }

    void harmonyNativeIdentityIntervalsAndUnknownTail()
    {
        HarmonyFixture fixture;
        QVERIFY(lseek(fixture.pcm->descriptor(), 7, SEEK_SET) == 7);
        const auto estimate = conflux::library::analyzeHarmony(*fixture.pcm, fixture.engine);
        QCOMPARE(estimate.status, QString("Estimated")); QCOMPARE(estimate.windows.size(), std::size_t(3));
        QCOMPARE(estimate.analysisFrames, std::uint64_t(384013)); QCOMPARE(estimate.pcmSha256.size(), 64);
        QCOMPARE(lseek(fixture.pcm->descriptor(), 0, SEEK_CUR), off_t(7));
        const auto* first = conflux::library::localHarmonyAt(estimate, 191999);
        const auto* second = conflux::library::localHarmonyAt(estimate, 192000);
        QVERIFY(first && second); QCOMPARE(first->candidates.front().tonic, 0); QCOMPARE(second->candidates.front().tonic, 2);
        const auto* tail = conflux::library::localHarmonyAt(estimate, 384000);
        QVERIFY(tail); QCOMPARE(tail->status, QString("Unknown")); QCOMPARE(tail->endFrame, std::uint64_t(384013));
        QVERIFY(!conflux::library::localHarmonyAt(estimate, 384013));
        auto corrupt = estimate; corrupt.windows[1].firstFrame++;
        QVERIFY(!conflux::library::localHarmonyAt(corrupt, 0));
        corrupt = estimate; corrupt.pcmSha256 = "bad";
        QVERIFY(!conflux::library::localHarmonyAt(corrupt, 0));
        for (const auto* mode : {"hash", "bool-schema", "frames", "duplicate", "overlap", "confidence", "nan", "missing-end", "extra", "flood"}) {
            fixture.setMode(mode);
            const auto failed = conflux::library::analyzeHarmony(*fixture.pcm, fixture.engine);
            QCOMPARE(failed.status, QString("Failed")); QVERIFY(failed.windows.empty());
            QVERIFY(!conflux::library::localHarmonyAt(failed, 0));
        }
        // Status/reason must follow observable frozen chroma admission,
        // including when callers pass an in-memory map directly to the query.
        for (const auto* mode : {"sparse-estimated", "flat-estimated", "sparse-competing", "wrong-context",
                                 "flat-ambiguous", "matched-competing", "wrong-non-tonal", "weak-estimated"}) {
            fixture.setMode(mode);
            const auto failed = conflux::library::analyzeHarmony(*fixture.pcm, fixture.engine);
            QCOMPARE(failed.status, QString("Failed")); QVERIFY(failed.windows.empty());
        }
        for (const auto* mode : {"sparse-ambiguous", "flat-unknown", "competing-good"}) {
            fixture.setMode(mode);
            const auto accepted = conflux::library::analyzeHarmony(*fixture.pcm, fixture.engine);
            QCOMPARE(accepted.status, QString("Estimated"));
            const auto* selected = conflux::library::localHarmonyAt(accepted, 0); QVERIFY(selected);
            QCOMPARE(selected->status, QString(QByteArray(mode) == "flat-unknown" ? "Unknown" : "Ambiguous"));
        }
        corrupt = estimate; corrupt.windows[1].chroma = {1.,0,0,0,0,0,0,0,0,0,0,0};
        QVERIFY(!conflux::library::localHarmonyAt(corrupt, 0));
        corrupt = estimate; corrupt.windows[1].chroma.fill(1. / 12);
        QVERIFY(!conflux::library::localHarmonyAt(corrupt, 0));
        corrupt = estimate; corrupt.windows[1].status = "Ambiguous"; corrupt.windows[1].reason = "insufficient-key-context";
        QVERIFY(!conflux::library::localHarmonyAt(corrupt, 0));
        corrupt.windows[1].reason = "competing-keys";
        QVERIFY(!conflux::library::localHarmonyAt(corrupt, 0));
        corrupt = estimate; corrupt.windows[1].status = "Unknown"; corrupt.windows[1].reason = "non-tonal";
        corrupt.windows[1].candidates.clear();
        QVERIFY(!conflux::library::localHarmonyAt(corrupt, 0));
        corrupt = estimate; corrupt.windows[1].candidates[0].correlation = .55;
        QVERIFY(!conflux::library::localHarmonyAt(corrupt, 0));
        fixture.engine.maxSeconds = 0;
        QCOMPARE(conflux::library::analyzeHarmony(*fixture.pcm, fixture.engine).status, QString("Failed"));
        fixture.engine.maxSeconds = 3; fixture.engine.workerPath = fixture.directory.filePath("missing.py");
        QCOMPARE(conflux::library::analyzeHarmony(*fixture.pcm, fixture.engine).status, QString("Failed"));
    }

    void harmonySourceCorruptionAndCancellationRemainFatal()
    {
        {
            HarmonyFixture fixture("await-invalid"); std::stop_source cancellation;
            auto future = std::async(std::launch::async, [&] {
                return conflux::library::analyzeHarmony(*fixture.pcm, fixture.engine, cancellation.get_token());
            });
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(fixture.marker), 2000);
            const float replacement = .75F;
            const bool changed = pwrite(fixture.writable.handle(), &replacement, sizeof(replacement), 0) == sizeof(replacement);
            conflux::test::writeFixtureBytes(fixture.release, "release");
            if (!changed) cancellation.request_stop();
            QVERIFY(changed);
            QVERIFY_EXCEPTION_THROWN(future.get(), conflux::library::HarmonySourceError);
        }
        {
            HarmonyFixture fixture("sleep"); std::stop_source cancellation;
            auto future = std::async(std::launch::async, [&] {
                try { (void)conflux::library::analyzeHarmony(*fixture.pcm, fixture.engine, cancellation.get_token()); }
                catch (const std::runtime_error& error) { return std::string(error.what()).find("cancel") != std::string::npos; }
                return false;
            });
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(fixture.marker), 2000);
            const auto pid = liveFixtureBytes(fixture.marker).toLongLong(); QVERIFY(pid > 1);
            const int descriptor = int(syscall(SYS_pidfd_open, pid_t(pid), 0));
            cancellation.request_stop(); QVERIFY(future.get());
            if (descriptor >= 0) {
                pollfd value{descriptor, POLLIN, 0}; QCOMPARE(::poll(&value, 1, 0), 1); close(descriptor);
            } else { errno = 0; QVERIFY(::kill(pid_t(pid), 0) == -1 && errno == ESRCH); }
        }
        HarmonyFixture nonfinite;
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        QCOMPARE(pwrite(nonfinite.writable.handle(), &nan, sizeof(nan), 0), ssize_t(sizeof(nan)));
        QVERIFY_EXCEPTION_THROWN(conflux::library::analyzeHarmony(*nonfinite.pcm, nonfinite.engine), conflux::library::HarmonySourceError);
    }

    void optionalHarmonyIsAcceptedWithSourceAndNeverChangesManualBpm()
    {
        QTemporaryDir directory, runtime; QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore; isolateLiveController(directory.path(), runtime.path());
        const auto source = directory.filePath("source.wav"), other = directory.filePath("other.wav");
        QVERIFY(writeWave(source, 2, 48000, 192000) && writeWave(other, 2, 48000, 96000));
        HarmonyFixture fixture("controller");
        conflux::DeckController::PreparationOptions options; options.harmonyEngine = fixture.engine;
        conflux::DeckController controller(options); controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        auto harmony = controller.decks()[0].toMap()["harmony"].toMap();
        QCOMPARE(harmony["status"].toString(), QString("Estimated")); QCOMPARE(harmony["confidence"].toString(), QString("Unknown"));
        QCOMPARE(harmony["candidates"].toList()[0].toMap()["tonic"].toInt(), 0);
        controller.setBpmOverride(0, 128);
        QCOMPARE(controller.decks()[0].toMap()["harmony"].toMap(), harmony);
        QCOMPARE(controller.decks()[0].toMap()["bpm"].toMap()["reference"].toDouble(), 128.);
        controller.loadDeck(0, QUrl::fromLocalFile(directory.filePath("missing.wav")));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.decks()[0].toMap()["harmony"].toMap(), harmony);
        controller.loadDeck(0, QUrl::fromLocalFile(other)); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        harmony = controller.decks()[0].toMap()["harmony"].toMap();
        QCOMPARE(harmony["status"].toString(), QString("Estimated"));
        QCOMPARE(harmony["candidates"].toList().size(), qsizetype(2));
        QCOMPARE(harmony["candidates"].toList()[0].toMap()["tonic"].toInt(), 2);
        const auto accepted = controller.decks()[0].toMap()["sourceId"];
        fixture.setMode("sleep"); controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(fixture.marker), 3000); QVERIFY(controller.cancelPreparation());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelled"));
        QCOMPARE(controller.decks()[0].toMap()["sourceId"], accepted);
        QCOMPARE(controller.decks()[0].toMap()["harmony"].toMap(), harmony);
        fixture.setMode("invalid"); controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(controller.decks()[0].toMap()["loaded"].toBool());
        QCOMPARE(controller.decks()[0].toMap()["harmony"].toMap()["analysisStatus"].toString(), QString("Failed"));
        QCOMPARE(controller.decks()[0].toMap()["harmony"].toMap()["status"].toString(), QString("Unknown"));
        QVERIFY(controller.decks()[0].toMap()["harmony"].toMap()["candidates"].toList().isEmpty());
    }

    void optionalHarmonyCannotConsumeTheOnlyStemCacheEntry()
    {
        liveControllerChecked([&] {
            QTemporaryDir directory, runtime; QVERIFY(directory.isValid() && runtime.isValid());
            RestoreEnvironment restore; isolateLiveController(directory.path(), runtime.path());
            const auto source = directory.filePath("single-entry.wav"), engineRoot = directory.filePath("engine");
            QVERIFY(writeWave(source, 2, 48000, 4800)); QVERIFY(QDir().mkpath(engineRoot));
            conflux::DeckController::PreparationOptions options; options.limits.maxEntries = 1;
            options.stemEngine = conflux::test::makeStemEngineFixture(engineRoot);
            const auto marker = directory.filePath("harmony-started"), worker = directory.filePath("harmony.py");
            conflux::test::writeFixtureBytes(worker, "from pathlib import Path\nPath(" +
                QJsonDocument(QJsonArray{marker}).toJson(QJsonDocument::Compact).mid(1).chopped(1) +
                ").touch()\nprint('unexpected harmony invocation')\n");
            options.harmonyEngine = conflux::library::HarmonyEngine{options.stemEngine->pythonExecutable, worker};
            conflux::DeckController controller(options); controller.loadStemDeck(0, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            const auto deck = controller.decks()[0].toMap();
            QVERIFY2(deck["loaded"].toBool(), qPrintable(controller.error())); QVERIFY(deck["stemBank"].toBool());
            QCOMPARE(deck["harmony"].toMap()["analysisStatus"].toString(), QString("Failed"));
            QCOMPARE(deck["harmony"].toMap()["status"].toString(), QString("Unknown"));
            QVERIFY(!QFileInfo::exists(marker));
            QCOMPARE(deck["stemGains"].toList(), QVariantList({1.F, 1.F, 1.F, 1.F}));
        });
    }

    void bpmReferenceIsSourceBoundAndDoesNotChangePlayback()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto source = directory.filePath("a.wav"), other = directory.filePath("b.wav");
        QVERIFY(writeWave(source, 2, 48000, 4800) && writeWave(other, 2, 48000, 9600));
        conflux::DeckController controller;
        controller.setBpmOverride(0, 120);
        QVERIFY(controller.error().contains("loaded"));
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto before = controller.decks();
        const auto audio = controller.audioState();
        const auto submitted = controller.submittedCommands();
        auto bpm = controller.decks()[0].toMap()["bpm"].toMap();
        QCOMPARE(bpm["reference"].toDouble(), 0.);
        QCOMPARE(bpm["status"].toString(), QString("Unavailable"));
        controller.setTempoBpm(0, 120);
        QVERIFY(controller.error().contains("reference"));
        QCOMPARE(controller.submittedCommands(), submitted);
        for (double invalid : {19.9, 400.1, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
            controller.setBpmOverride(0, invalid);
            QCOMPARE(controller.decks(), before);
        }
        controller.setBpmOverride(0, 120);
        bpm = controller.decks()[0].toMap()["bpm"].toMap();
        QCOMPARE(bpm["reference"].toDouble(), 120.);
        QVERIFY(bpm["manual"].toBool());
        QCOMPARE(bpm["estimated"].toDouble(), 0.);
        QCOMPARE(controller.submittedCommands(), submitted);
        QCOMPARE(controller.audioState().tempo[0].percent, audio.tempo[0].percent);
        QCOMPARE(controller.decks()[1], before[1]);
        controller.setTempoBpm(0, 126);
        QVERIFY(controller.error().contains("not ready", Qt::CaseInsensitive));
        QCOMPARE(controller.submittedCommands(), submitted);
        controller.setTempoBpm(0, 128);
        QVERIFY(controller.error().contains("range"));
        controller.clearBpmOverride(0);
        QCOMPARE(controller.decks()[0].toMap()["bpm"].toMap()["reference"].toDouble(), 0.);
        controller.setBpmOverride(0, 128);
        controller.loadDeck(0, QUrl::fromLocalFile(directory.filePath("missing.wav")));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.decks()[0].toMap()["bpm"].toMap()["reference"].toDouble(), 128.);
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.decks()[0].toMap()["bpm"].toMap()["reference"].toDouble(), 128.);
        controller.loadDeck(0, QUrl::fromLocalFile(other));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.decks()[0].toMap()["bpm"].toMap()["reference"].toDouble(), 0.);
        QVERIFY(!controller.decks()[0].toMap()["bpm"].toMap()["manual"].toBool());
    }

    void pendingPresentationPrecedesBpmAndPreservesActiveTrack()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto original = directory.filePath("kept.flac"), next = directory.filePath("pending.flac");
        QVERIFY(presentationFixture::writeFlac(original, "Current title", "Current artist", "Current album",
            presentationFixture::artwork(Qt::blue), 48000, 4800));
        QVERIFY(presentationFixture::writeFlac(next, "Pending title", "Pending artist", "Pending album",
            presentationFixture::artwork(Qt::red), 48000, 9600));
        const auto model = directory.filePath("model"), script = directory.filePath("bpm.py");
        const auto marker = directory.filePath("bpm-started"), release = directory.filePath("bpm-release");
        QFile modelFile(model);
        QVERIFY(modelFile.open(QIODevice::WriteOnly)); modelFile.write("fixture"); modelFile.close();
        const QByteArray protocol =
            "print(json.dumps(dict(schema=2,recipe='beat-this-final0-linear-mel-v1',frames=a.frames,pcm_sha256=hashlib.sha256(b).hexdigest(),bpm=120.0,beat_count=4)))\n"
            "for frame in (0,960,1920,2880): print(json.dumps(dict(frame=frame,downbeat=(frame==0))))\n";
        const QByteArray preamble = "import argparse,hashlib,json,sys,time\nfrom pathlib import Path\n"
            "p=argparse.ArgumentParser()\np.add_argument('--source-dir')\np.add_argument('--model')\np.add_argument('--frames',type=int)\n"
            "a=p.parse_args()\nb=sys.stdin.buffer.read()\n";
        QFile worker(script);
        QVERIFY(worker.open(QIODevice::WriteOnly)); worker.write(preamble + protocol); worker.close();
        conflux::DeckController::PreparationOptions options;
        options.bpmEngine = conflux::library::BpmEngine{"/usr/bin/python3", directory.path(), model, script};
        conflux::DeckController controller(options);
        controller.loadDeck(0, QUrl::fromLocalFile(original));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto accepted = controller.decks();
        QVERIFY(accepted[0].toMap()["loaded"].toBool());
        const auto submitted = controller.submittedCommands();
        const auto quoted = [](const QString& path) {
            return QJsonDocument(QJsonArray{path}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
        };
        QVERIFY(worker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        worker.write(preamble + "Path(" + quoted(marker) + ").touch()\nwhile not Path(" + quoted(release) +
                     ").exists(): time.sleep(.005)\n" + protocol); worker.close();
        bool guiOnly = true, primeObserved = false, installObserved = false, progressBounded = true;
        connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            guiOnly = guiOnly && QThread::currentThread() == controller.thread();
            const auto pending = controller.decks()[0].toMap();
            if (!pending["loading"].toBool()) return;
            const auto phase = pending["loadPhase"].toString();
            primeObserved = primeObserved || phase == "prime";
            installObserved = installObserved || phase == "install";
            progressBounded = progressBounded && pending["loadProgress"].toDouble() >= 0. &&
                pending["loadProgress"].toDouble() <= .75;
        }, Qt::DirectConnection);
        controller.loadDeck(0, QUrl::fromLocalFile(next));
        auto pending = controller.decks()[0].toMap();
        QVERIFY(pending["loading"].toBool());
        QCOMPARE(pending["pendingName"].toString(), QString("pending.flac"));
        QCOMPARE(pending["pendingPath"].toString(), next);
        QCOMPARE(pending["loadPhase"].toString(), QString("decode-cache"));
        QCOMPARE(pending["loadProgress"].toDouble(), 0.);
        QVERIFY(pending["pendingPresentation"].toMap()["title"].toString().isEmpty());
        QVERIFY(pending["pendingWaveform"].toList().isEmpty());
        QCOMPARE(pending["title"], accepted[0].toMap()["title"]);
        QCOMPARE(pending["sourceId"], accepted[0].toMap()["sourceId"]);
        QCOMPARE(controller.decks()[1], accepted[1]);
        QVERIFY(!controller.preparation()["stems"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.decks()[0].toMap()["loadPhase"].toString(), QString("bpm"), 5000);
        pending = controller.decks()[0].toMap();
        QCOMPARE(pending["pendingPresentation"].toMap()["title"].toString(), QString("Pending title"));
        QCOMPARE(pending["pendingPresentation"].toMap()["artist"].toString(), QString("Pending artist"));
        QVERIFY(!pending["pendingPresentation"].toMap()["artworkDataUrl"].toString().isEmpty());
        QVERIFY(!pending["pendingWaveform"].toList().isEmpty());
        QCOMPARE(pending["waveform"], accepted[0].toMap()["waveform"]);
        QCOMPARE(pending["name"], accepted[0].toMap()["name"]);
        QCOMPARE(pending["loadCompletedStages"].toInt(), 1);
        QCOMPARE(pending["loadTotalStages"].toInt(), 4);
        QCOMPARE(pending["loadProgress"].toDouble(), .25);
        QCOMPARE(controller.preparation()["phase"].toString(), QString("bpm"));
        QVERIFY(controller.preparation()["indeterminate"].toBool());
        QCOMPARE(controller.submittedCommands(), submitted);
        QFile allow(release); QVERIFY(allow.open(QIODevice::WriteOnly)); allow.close();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(guiOnly && primeObserved && installObserved && progressBounded);
        const auto installed = controller.decks()[0].toMap();
        QCOMPARE(installed["title"].toString(), QString("Pending title"));
        QVERIFY(!installed["loading"].toBool());
        QVERIFY(installed["pendingName"].toString().isEmpty() && installed["pendingWaveform"].toList().isEmpty());
        QCOMPARE(controller.decks()[1], accepted[1]);
    }

    void queuedPreparationProgressCannotReviveCancelledLoad()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto cancelled = directory.filePath("cancelled.flac"), next = directory.filePath("next.flac");
        QVERIFY(presentationFixture::writeFlac(cancelled, "Obsolete title", "artist", "album",
            presentationFixture::artwork(Qt::blue), 48000, 4800));
        QVERIFY(presentationFixture::writeFlac(next, "Next deck title", "artist", "album",
            presentationFixture::artwork(Qt::red), 48000, 9600));
        conflux::DeckController controller;
        const auto original = controller.decks();
        controller.loadDeck(0, QUrl::fromLocalFile(cancelled));
        // Finish the real decode/prime without delivering its queued GUI updates.
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QVERIFY(controller.decks()[0].toMap()["pendingWaveform"].toList().isEmpty());
        QVERIFY(controller.cancelPreparation());
        bool obsoletePreview = false;
        connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            const auto deck = controller.decks()[0].toMap();
            obsoletePreview = obsoletePreview || !deck["pendingWaveform"].toList().isEmpty() ||
                !deck["pendingPresentation"].toMap()["title"].toString().isEmpty();
        }, Qt::DirectConnection);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(!obsoletePreview);
        QCOMPARE(controller.decks(), original);
        QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelled"));
        controller.loadDeck(1, QUrl::fromLocalFile(next));
        QCOMPARE(controller.decks()[0], original[0]);
        const auto pending = controller.decks()[1].toMap();
        QVERIFY(pending["loading"].toBool());
        QCOMPARE(pending["pendingName"].toString(), QString("next.flac"));
        QVERIFY(pending["pendingWaveform"].toList().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.decks()[1].toMap()["title"].toString(), QString("Next deck title"));
        QCOMPARE(controller.decks()[0], original[0]);
        QVERIFY(!obsoletePreview);
    }

    void bpmCacheReusesOriginalMixAcrossDecksAndReloads()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto source = directory.filePath("cached-source.wav");
        QVERIFY(writeWave(source, 2, 48000, 4800));
        const auto engineRoot = directory.filePath("engine");
        QVERIFY(QDir().mkpath(engineRoot));
        conflux::DeckController::PreparationOptions options;
        options.stemEngine = conflux::test::makeStemEngineFixture(engineRoot);
        for (const auto* relative : {"beat_this/utils.py", "beat_this/model/roformer.py",
                "beat_this/model/beat_tracker.py", "beat_this/model/postprocessor.py",
                "beat_this/preprocessing.py", "beat_this/inference.py", "LICENSE"}) {
            const auto path = QDir(engineRoot).filePath(QString::fromLatin1(relative));
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            conflux::test::writeFixtureBytes(path, "Synthetic cache identity only; never imported.\n");
        }
        const auto script = directory.filePath("bpm.py"), marker = directory.filePath("bpm.calls");
        conflux::test::writeFixtureBytes(script,
            "import argparse,hashlib,json,sys\nfrom pathlib import Path\n"
            "p=argparse.ArgumentParser()\np.add_argument('--source-dir')\np.add_argument('--model')\np.add_argument('--frames',type=int)\n"
            "a=p.parse_args()\nb=sys.stdin.buffer.read()\n"
            "with Path(__file__).with_suffix('.calls').open('ab') as f: f.write(b'x')\n"
            "print(json.dumps(dict(schema=2,recipe='beat-this-final0-linear-mel-v1',frames=a.frames,pcm_sha256=hashlib.sha256(b).hexdigest(),bpm=120.0,beat_count=4)))\n"
            "for frame in (0,960,1920,2880): print(json.dumps(dict(frame=frame,downbeat=(frame==0))))\n");
        options.bpmEngine = conflux::library::BpmEngine{options.stemEngine->pythonExecutable,
            engineRoot, options.stemEngine->modelPath, script};
        const auto calls = [&] { QFile file(marker); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; };
        {
            conflux::DeckController controller(options);
            controller.loadDeck(0, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QVERIFY(controller.decks()[0].toMap()["loaded"].toBool());
            QCOMPARE(controller.decks()[0].toMap()["bpm"].toMap()["estimated"].toDouble(), 120.);
            QCOMPARE(calls(), QByteArray("x"));
            controller.setBpmOverride(0, 128.);
            controller.loadDeck(0, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QCOMPARE(calls(), QByteArray("x"));
            const auto a = controller.decks()[0].toMap()["bpm"].toMap();
            QCOMPARE(a["reference"].toDouble(), 128.);
            QCOMPARE(a["estimated"].toDouble(), 120.);
            QVERIFY(a["manual"].toBool());
            controller.loadStemDeck(1, QUrl::fromLocalFile(source));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            const auto b = controller.decks()[1].toMap();
            QVERIFY2(b["stemBank"].toBool(), qPrintable(controller.error()));
            QCOMPARE(calls(), QByteArray("x"));
            QCOMPARE(b["bpm"].toMap()["reference"].toDouble(), 120.);
            QVERIFY(!b["bpm"].toMap()["manual"].toBool());
            controller.shutdown();
        }
        conflux::DeckController reopened(options);
        reopened.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.busy(), 5000);
        QVERIFY(reopened.decks()[0].toMap()["loaded"].toBool());
        QCOMPARE(calls(), QByteArray("x"));
        QCOMPARE(reopened.decks()[0].toMap()["bpm"].toMap()["reference"].toDouble(), 120.);
        QVERIFY(!reopened.decks()[0].toMap()["bpm"].toMap()["manual"].toBool());
        QVERIFY(writeWave(source, 2, 48000, 9600));
        reopened.loadDeck(1, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.busy(), 5000);
        QVERIFY(reopened.decks()[1].toMap()["loaded"].toBool());
        QCOMPARE(calls(), QByteArray("xx"));
    }

    void automaticBpmFailurePreservesPlayableAudioAndCancellation()
    {
        QTemporaryDir directory, runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        isolateLiveController(directory.path(), runtime.path());
        const auto source = directory.filePath("source.wav"), script = directory.filePath("bpm.py");
        const auto model = directory.filePath("model"), marker = directory.filePath("started");
        QVERIFY(writeWave(source, 2, 48000, 4800));
        QFile asset(model);
        QVERIFY(asset.open(QIODevice::WriteOnly)); asset.write("fixture"); asset.close();
        QFile worker(script);
        QVERIFY(worker.open(QIODevice::WriteOnly));
        worker.write("import argparse, hashlib, json, sys\np=argparse.ArgumentParser()\np.add_argument('--source-dir')\np.add_argument('--model')\np.add_argument('--frames',type=int)\na=p.parse_args()\nb=sys.stdin.buffer.read()\nprint(json.dumps(dict(schema=2,recipe='beat-this-final0-linear-mel-v1',frames=a.frames,pcm_sha256=hashlib.sha256(b).hexdigest(),bpm=120.0,beat_count=4)))\n");
        worker.write("for frame in (0, 960, 1920, 2880): print(json.dumps(dict(frame=frame,downbeat=(frame==0))))\n");
        worker.close();
        conflux::DeckController::PreparationOptions options;
        options.bpmEngine = conflux::library::BpmEngine{"/usr/bin/python3", directory.path(), model, script};
        conflux::DeckController controller(options);
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        auto deck = controller.decks()[0].toMap();
        QVERIFY(deck["loaded"].toBool());
        QCOMPARE(deck["bpm"].toMap()["estimated"].toDouble(), 120.);
        QCOMPARE(deck["bpm"].toMap()["status"].toString(), QString("Estimated"));
        controller.setBpmOverride(0, 128);
        controller.clearBpmOverride(0);
        QCOMPARE(controller.decks()[0].toMap()["bpm"].toMap()["reference"].toDouble(), 120.);
        QVERIFY(worker.open(QIODevice::WriteOnly | QIODevice::Truncate)); worker.write("print('invalid output')\n"); worker.close();
        controller.loadDeck(1, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(controller.decks()[1].toMap()["loaded"].toBool());
        QCOMPARE(controller.decks()[1].toMap()["bpm"].toMap()["status"].toString(), QString("Failed"));
        const auto accepted = controller.decks();
        QVERIFY(worker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        worker.write("import time\nfrom pathlib import Path\nPath(" +
                     QJsonDocument(QJsonArray{marker}).toJson(QJsonDocument::Compact).mid(1).chopped(1) +
                     ").touch()\ntime.sleep(30)\n"); worker.close();
        controller.loadDeck(0, QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
        QVERIFY(controller.cancelPreparation());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelled"));
        QCOMPARE(controller.decks(), accepted);
    }

    void invalidDeckAndUrl()
    {
        for (const int deck : {-1, 2}) {
            conflux::DeckController controller;
            QSignalSpy changed(&controller, &conflux::DeckController::stateChanged);
            controller.loadDeck(deck, QUrl::fromLocalFile("/missing-controller-test.wav"));
            QVERIFY(!controller.busy());
            QVERIFY(!controller.error().isEmpty());
            QVERIFY(!changed.isEmpty());
            verifyNoPlayback(controller);
        }
        for (const auto& url : {QUrl(), QUrl("https://example.invalid/music.wav")}) {
            conflux::DeckController controller;
            controller.loadDeck(0, url);
            QVERIFY(!controller.busy());
            QVERIFY(!controller.error().isEmpty());
            verifyNoPlayback(controller);
        }
    }

    void cancellationBeforeQueuedCompletion()
    {
        for (const bool waitForWorker : {false, true}) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("cancel-before-install.wav");
            QVERIFY(writeWave(path, 2, 48000, 480));
            conflux::DeckController controller;
            const auto original = controller.decks();
            QVERIFY(!controller.cancelPreparation());
            controller.loadDeck(0, QUrl::fromLocalFile(path));
            QVERIFY(controller.busy());
            if (waitForWorker)
                QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
            // No GUI events have run: even a completed worker cannot install
            // its source before the caller can cancel the queued completion.
            QVERIFY(controller.cancelPreparation());
            QVERIFY(controller.busy());
            QVERIFY(!controller.cancelPreparation());
            QCOMPARE(controller.preparation()["status"].toString(), QString("Cancelling"));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QCOMPARE(controller.decks(), original);
            QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
            QVERIFY(!controller.outputReady());
            QVERIFY(!controller.cancelPreparation());
        }
    }

    void directPreparationSignalCanCancelNewJob()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("direct-cancellation.wav");
        QVERIFY(writeWave(path, 2, 48000, 480));
        conflux::DeckController controller;
        bool accepted = false;
        connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            if (controller.preparation()["cancellable"].toBool())
                accepted = controller.cancelPreparation();
        }, Qt::DirectConnection);
        controller.loadDeck(1, QUrl::fromLocalFile(path));
        QVERIFY(accepted);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        verifyNoPlayback(controller);
    }

    void cancellationRetainsTrackAndEndsAtInstallation()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        const auto first = directory.filePath("kept-track.flac");
        const auto second = directory.filePath("cancelled-replacement.flac");
        QVERIFY(presentationFixture::writeFlac(first, "Keep this title", "artist", "album",
            presentationFixture::artwork(Qt::blue), 48000, 480));
        QVERIFY(presentationFixture::writeFlac(second, "Replacement title", "artist", "album",
            presentationFixture::artwork(Qt::red), 48000, 960));
        conflux::DeckController controller;
        bool installationObserved = false, lateAccepted = true;
        connect(&controller, &conflux::DeckController::stateChanged, &controller, [&] {
            if (!installationObserved && controller.preparation()["status"].toString() == "Installing") {
                installationObserved = true;
                lateAccepted = controller.cancelPreparation();
            }
        }, Qt::DirectConnection);
        controller.loadDeck(0, QUrl::fromLocalFile(first));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(installationObserved);
        QVERIFY(!lateAccepted);
        const auto original = controller.decks();
        QVERIFY(original[0].toMap()["loaded"].toBool());
        QCOMPARE(original[0].toMap()["title"].toString(), QString("Keep this title"));
        const auto retainedArt = original[0].toMap()["artworkDataUrl"].toString();
        QVERIFY(!retainedArt.isEmpty());
        const auto submitted = controller.submittedCommands();
        controller.loadDeck(0, QUrl::fromLocalFile(second));
        QVERIFY(controller.cancelPreparation());
        controller.loadDeck(1, QUrl::fromLocalFile(first));
        QVERIFY(controller.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.decks(), original);
        QCOMPARE(controller.submittedCommands(), submitted);
        controller.loadDeck(1, QUrl::fromLocalFile(second));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.decks()[1].toMap()["name"].toString(), QString("cancelled-replacement.flac"));
        QCOMPARE(controller.decks()[1].toMap()["title"].toString(), QString("Replacement title"));
        QVERIFY(controller.decks()[1].toMap()["artworkDataUrl"].toString() != retainedArt);
    }

    void actualDecodeFailures()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto corrupt = directory.filePath("corrupt.wav");
        QFile file(corrupt);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("RIFF", 4), qint64(4));
        file.close();
        const auto multichannel = directory.filePath("multichannel.wav");
        const auto otherRate = directory.filePath("7999.wav");
        QVERIFY(writeWave(multichannel, 3, 48000));
        QVERIFY(writeWave(otherRate, 2, 7999));
        const QStringList paths{directory.filePath("missing.wav"), corrupt, multichannel, otherRate};
        for (const auto& path : paths) {
            conflux::DeckController controller;
            QSignalSpy changed(&controller, &conflux::DeckController::stateChanged);
            controller.loadDeck(0, QUrl::fromLocalFile(path));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QVERIFY2(!controller.error().isEmpty(), qPrintable(path));
            QVERIFY(!changed.isEmpty());
            verifyNoPlayback(controller);
            QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
            QCOMPARE(controller.completedSessions(), std::uint64_t(0));
        }
    }

    void rejectsConcurrentLoad()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto first = directory.filePath("first-unsupported.wav");
        QVERIFY(writeWave(first, 3, 48000));
        conflux::DeckController controller;
        controller.loadDeck(0, QUrl::fromLocalFile(first));
        // The worker may already have decoded, but its queued completion has
        // not run on this GUI/control thread. No second job may be accepted.
        QVERIFY(controller.busy());
        controller.loadDeck(1, QUrl::fromLocalFile(directory.filePath("second-missing.wav")));
        QVERIFY(controller.busy());
        QVERIFY(!controller.error().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(!controller.error().isEmpty());
        verifyNoPlayback(controller);
        QCOMPARE(controller.completedSessions(), std::uint64_t(0));
    }

    void rejectsTransportWithoutOutput()
    {
        conflux::DeckController controller;
        controller.setPlaying(0, true);
        QVERIFY(!controller.jogFrames(0, 1));
        QVERIFY(!controller.jogFrames(-1, -1));
        QVERIFY(!controller.jogFrames(2, std::numeric_limits<qint64>::max()));
        QVERIFY(!controller.error().isEmpty());
        controller.seekSeconds(0, 1.0);
        controller.setDeckGain(0, .5);
        controller.setPlaying(-1, false);
        controller.seekSeconds(2, 0);
        controller.setDeckGain(2, .5);
        for (const auto gain : {-0.1, 1.1, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()}) {
            const auto previous = controller.masterGain();
            controller.setMasterGain(gain);
            QCOMPARE(controller.masterGain(), previous);
            QVERIFY(!controller.error().isEmpty());
        }
        verifyNoPlayback(controller);
        QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
    }

    void failedPreparationRetainsLoadedTrack()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        const auto original = directory.filePath("original.flac");
        QVERIFY(presentationFixture::writeFlac(original, "Original tagged title", "artist", "album",
            presentationFixture::artwork(Qt::blue), 48000, 8));
        conflux::DeckController controller;
        controller.loadDeck(0, QUrl::fromLocalFile(original));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto retained = controller.decks()[0].toMap();
        QVERIFY(retained["loaded"].toBool());
        QCOMPARE(retained["title"].toString(), QString("Original tagged title"));
        QVERIFY(!retained["artworkDataUrl"].toString().isEmpty());
        QVERIFY(!retained["replacing"].toBool());
        controller.loadDeck(0, QUrl::fromLocalFile(directory.filePath("missing-next.wav")));
        QVERIFY(controller.busy());
        const auto pending = controller.decks()[0].toMap();
        QVERIFY(pending["loading"].toBool());
        QCOMPARE(pending["pendingName"].toString(), QString("missing-next.wav"));
        for (const auto key : {"name", "title", "sourceId", "artworkDataUrl", "waveform"})
            QCOMPARE(pending[key], retained[key]);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY(!controller.error().isEmpty());
        QCOMPARE(controller.decks()[0].toMap(), retained);
        QVERIFY(!controller.jogFrames(0, std::numeric_limits<qint64>::min()));
        QCOMPARE(controller.decks()[0].toMap(), retained);
        QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
        controller.shutdown();
    }

    void rejectsCueWithoutLoadedOutput()
    {
        conflux::DeckController controller;
        for (const int deck : {-1, 0, 1, 2}) {
            QVERIFY(!controller.cuePress(deck));
            QVERIFY(!controller.error().isEmpty());
            // Releases are cleanup, including a late or duplicate release
            // after output is gone. They must not start a session or command.
            controller.cueRelease(deck);
            controller.cueRelease(deck);
            verifyNoPlayback(controller);
            QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
            QCOMPARE(controller.completedSessions(), std::uint64_t(0));
        }
        controller.shutdown();
        QVERIFY(!controller.cuePress(0));
        controller.cueRelease(0);
        verifyNoPlayback(controller);
        QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
    }

    void routingRecordingAndMetersWithoutOutput()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto recordingPath = directory.filePath("must-not-be-created.wav");
        conflux::DeckController controller;
        QCOMPARE(controller.cueGain(), 1.0);
        const auto initialRouting = controller.audioRouting();
        QVERIFY(initialRouting["ports"].toList().isEmpty());
        QVERIFY(!initialRouting["masterReady"].toBool());
        QVERIFY(!initialRouting["cueReady"].toBool());
        const auto initialMeters = controller.meters();
        QCOMPARE(initialMeters["blockFrames"].toULongLong(), qulonglong(0));
        for (const auto& meterName : {"master", "cue"}) {
            const auto meter = initialMeters[meterName].toMap();
            for (const auto& channel : {"peakLeft", "peakRight", "rmsLeft", "rmsRight"})
                QCOMPARE(meter[channel].toDouble(), 0.0);
            QCOMPARE(meter["overrangeLeft"].toULongLong(), qulonglong(0));
            QCOMPARE(meter["overrangeRight"].toULongLong(), qulonglong(0));
        }
        for (const auto& deckValue : controller.decks()) {
            const auto deck = deckValue.toMap();
            QVERIFY(!deck["cueSelected"].toBool());
            QCOMPARE(deck["meter"].toMap(), initialMeters["master"].toMap());
        }
        const auto initialRecording = controller.recording();
        QCOMPARE(initialRecording["status"].toString(), QString("Idle"));
        QVERIFY(!initialRecording["active"].toBool());
        QCOMPARE(initialRecording["acceptedFrames"].toULongLong(), qulonglong(0));
        QCOMPARE(initialRecording["writtenFrames"].toULongLong(), qulonglong(0));
        controller.startRecording(QUrl::fromLocalFile(recordingPath));
        const auto rejectedRecording = controller.recording();
        QVERIFY(!rejectedRecording["error"].toString().isEmpty());
        QVERIFY(!rejectedRecording["active"].toBool());
        QCOMPARE(rejectedRecording["status"].toString(), QString("Idle"));
        QVERIFY(!QFileInfo::exists(recordingPath));
        controller.stopRecording();
        controller.stopRecording();

        for (const auto gain : {-.1, .5, 1.1, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()}) {
            controller.setCueGain(gain);
            QCOMPARE(controller.cueGain(), 1.0);
            QVERIFY(!controller.error().isEmpty());
        }
        for (const int deck : {-1, 0, 1, 2}) {
            controller.setCueSelected(deck, true);
            controller.setCueSelected(deck, false);
            QVERIFY(!controller.error().isEmpty());
        }
        controller.configureAudioRouting("missing", "missing", {}, {});
        QVERIFY(!controller.error().isEmpty());
        controller.refreshAudioRouting();
        const auto unavailableRouting = controller.audioRouting();
        // A development-only PipeWire installation can lack client.conf and
        // fail discovery immediately. Retain that diagnostic; rejected routing
        // must preserve selection/readiness without assuming a host setup.
        for (const auto* key : {"ports", "masterReady", "cueReady", "masterLeft", "masterRight",
                               "cueLeft", "cueRight", "defaultSink", "cueError"})
            QCOMPARE(unavailableRouting[key], initialRouting[key]);
        QCOMPARE(controller.meters(), initialMeters);
        verifyNoPlayback(controller);
        QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
        QCOMPARE(controller.completedSessions(), std::uint64_t(0));
        controller.shutdown();
        controller.shutdown();
        controller.stopRecording();
        QVERIFY(!controller.recording()["active"].toBool());
        QVERIFY(!QFileInfo::exists(recordingPath));
    }

    void closeWithPendingDecode()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("converting-mono.wav");
        QVERIFY(writeWave(path, 1, 44100, 441));
        auto controller = std::make_unique<conflux::DeckController>();
        QPointer<conflux::DeckController> observer(controller.get());
        controller->loadDeck(0, QUrl::fromLocalFile(path));
        QVERIFY(controller->busy());
        // Destruction must join the finite decode job and remove its pending
        // completion. This does not claim that codec work was cancelled.
        controller.reset();
        QVERIFY(observer.isNull());
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }

    void overviewUsesDecodedStereoPeaks()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        // No test may reach the laptop's output server. Keep the environment
        // until the controller/session has been destroyed.
        RestoreEnvironment restore;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        const auto path = directory.filePath("peaks.wav");
        QVERIFY(writeWave(path, 2, 48000));
        conflux::DeckController controller;
        controller.loadDeck(0, QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto deck = controller.decks()[0].toMap();
        QVERIFY(deck["loaded"].toBool());
        const auto peaks = deck["waveform"].toList();
        QCOMPARE(peaks.size(), 4);
        // Independent fixture channel pairs are [-32,-15], [2,19], [36,53],
        // [70,87]; use max(abs(L),abs(R)) per source frame, then normalize.
        const std::array<double, 4> expected{32.0 / 87, 19.0 / 87, 53.0 / 87, 1.0};
        for (std::size_t i = 0; i < expected.size(); ++i)
            QVERIFY(std::abs(peaks[qsizetype(i)].toDouble() - expected[i]) < 1e-6);
        // The private, unavailable server lets a valid deck be loaded without
        // reaching host audio. A cue press still needs ready output.
        QVERIFY(!controller.outputReady());
        QVERIFY(!controller.cuePress(0));
        controller.cueRelease(0);
        controller.cueRelease(0);
        const auto unavailable = controller.decks()[0].toMap();
        QCOMPARE(unavailable["cueSeconds"].toDouble(), 0.0);
        QVERIFY(!unavailable["cueHeld"].toBool());
        QVERIFY(!unavailable["cuePreview"].toBool());
        QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
        controller.shutdown();
        QVERIFY(!controller.outputReady());
    }

    void acceptsConvertedSourcesWithoutHostAudio()
    {
        QTemporaryDir directory;
        QTemporaryDir runtime;
        QVERIFY(directory.isValid() && runtime.isValid());
        RestoreEnvironment restore;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "conflux-controller-test-unavailable");
        for (const auto& geometry : {std::pair<quint16, quint32>{1, 44100}, {2, 96000}}) {
            const auto [channels, rate] = geometry;
            const auto path = directory.filePath(QString("converted-%1-%2.wav").arg(channels).arg(rate));
            QVERIFY(writeWave(path, channels, rate, rate / 100));
            conflux::DeckController controller;
            controller.loadDeck(0, QUrl::fromLocalFile(path));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            const auto deck = controller.decks()[0].toMap();
            QVERIFY2(deck["loaded"].toBool(), qPrintable(controller.error()));
            const auto frames = controller.audioState().sourceFrames[0];
            QVERIFY(frames >= 479 && frames <= 481);
            QCOMPARE(deck["durationSeconds"].toDouble(), frames / 48000.0);
            QCOMPARE(deck["positionSeconds"].toDouble(), 0.0);
            QCOMPARE(deck["cueSeconds"].toDouble(), 0.0);
            QVERIFY(!deck["playing"].toBool());
            const auto peaks = deck["waveform"].toList();
            QCOMPARE(peaks.size(), qsizetype(frames));
            double maximum = 0;
            for (const auto& value : peaks) {
                const auto peak = value.toDouble();
                QVERIFY(std::isfinite(peak) && peak >= 0 && peak <= 1);
                maximum = std::max(maximum, peak);
            }
            QCOMPARE(maximum, 1.0);
            QVERIFY(!controller.outputReady());
            QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
            controller.shutdown();
            QVERIFY(!controller.outputReady());
        }
    }
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir cache;
    QTemporaryDir data;
    if (!cache.isValid() || !data.isValid()) return 2;
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    DeckControllerTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "deck_controller_test.moc"

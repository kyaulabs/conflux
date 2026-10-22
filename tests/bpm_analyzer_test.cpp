// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/BpmAnalyzer.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <future>
#include <limits>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {
using namespace conflux;
using namespace std::chrono_literals;
void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() || !file.flush())
        throw std::runtime_error("Cannot write BPM fixture");
}
QByteArray read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read BPM fixture marker");
    return file.readAll();
}
struct CloseFd {
    int value = -1;
    ~CloseFd() { if (value >= 0) close(value); }
};
struct Fixture {
    QTemporaryDir directory;
    CloseFd writable;
    std::unique_ptr<audio::PreparedAudio> pcm;
    library::BpmEngine engine;
    QString marker;
    static constexpr std::uint64_t frames = 4099;
    explicit Fixture(const QByteArray& mode = "good", std::uint64_t frameCount = frames)
    {
        if (!directory.isValid()) throw std::runtime_error("Cannot create private BPM fixture directory");
        std::vector<float> samples(frameCount * 2);
        for (std::size_t index = 0; index < samples.size(); ++index)
            samples[index] = float(int(index % 113) - 56) / 32.F;
        samples[0] = -0.F; samples[1] = 0.F;
        const auto raw = directory.filePath("input.f32");
        write(raw, QByteArray(reinterpret_cast<const char*>(samples.data()), qsizetype(samples.size() * sizeof(float))));
        writable.value = open(QFile::encodeName(raw).constData(), O_WRONLY | O_CLOEXEC);
        if (writable.value < 0) throw std::runtime_error("Cannot open separate corruption-fixture FD");
        const auto fd = open(QFile::encodeName(raw).constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("Cannot open prepared BPM fixture");
        try {
            auto prepared = audio::reopenPreparedAudio(fd, frameCount, std::vector<float>(512, 1.F));
            pcm = std::make_unique<audio::PreparedAudio>(std::move(prepared));
        } catch (...) { close(fd); throw; }
        close(fd);
        engine.pythonExecutable = QFileInfo(QStandardPaths::findExecutable("python3")).canonicalFilePath();
        engine.sourceDirectory = directory.path();
        engine.workerPath = directory.filePath("worker.py");
        engine.modelPath = directory.filePath("model.bin");
        engine.maxSeconds = 3;
        marker = directory.filePath("worker.started");
        write(engine.modelPath, "unused fake model; never loaded\n");
        QByteArray script = "MODE = '" + mode + "'\n";
        script += R"PY(import argparse, hashlib, json, os, pathlib, resource, signal, sys, time
p = argparse.ArgumentParser()
p.add_argument('--source-dir', required=True)
p.add_argument('--model', required=True)
p.add_argument('--frames', type=int, required=True)
a = p.parse_args()
data = sys.stdin.buffer.read()
assert len(data) == a.frames * 8
assert pathlib.Path(a.source_dir).is_dir() and pathlib.Path(a.model).is_file()
assert sys.flags.isolated == 1 and sys.flags.dont_write_bytecode == 1
if MODE == 'limits':
    assert resource.getrlimit(resource.RLIMIT_CORE) == (0, 0)
    assert resource.getrlimit(resource.RLIMIT_AS) == (8589934592, 8589934592)
P = pathlib.Path(__file__)
if MODE == 'require-venv':
    assert pathlib.Path(sys.prefix) == P.parent / 'venv' and sys.prefix != sys.base_prefix
child = 0
if MODE in ('descendant', 'partial-descendant'):
    child = os.fork()
    if child == 0:
        signal.signal(signal.SIGTERM, signal.SIG_DFL)
        time.sleep(60)
        os._exit(0)
    def stop(*_):
        os.waitpid(child, 0)
        sys.exit(0)
    signal.signal(signal.SIGTERM, stop)
P.with_suffix('.started').write_text(json.dumps([os.getpid(), child]))
if MODE in ('block', 'descendant'):
    time.sleep(60)
if MODE in ('await-change', 'await-change-bad-exit', 'await-change-bad-protocol'):
    while not P.with_suffix('.release').exists():
        time.sleep(.005)
if MODE == 'stdout-flood':
    sys.stdout.buffer.write(b'x' * (4 * 1024**2 + 1)); sys.stdout.buffer.flush(); time.sleep(60)
if MODE == 'stderr-flood':
    sys.stderr.buffer.write(b'x' * 70000); sys.stderr.buffer.flush(); time.sleep(60)
value = {'schema': 2, 'recipe': 'beat-this-final0-linear-mel-v1', 'frames': a.frames,
         'pcm_sha256': hashlib.sha256(data).hexdigest(), 'bpm': 120., 'beat_count': 4}
if MODE == 'unknown': value.update(bpm=None, beat_count=0)
if MODE == 'one-beat': value.update(bpm=None, beat_count=1)
if MODE == 'unknown-many': value.update(bpm=None)
if MODE == 'exact-output-limit': value.update(bpm=None, beat_count=300)
if MODE == 'schema-one': value['schema'] = 1
if MODE == 'wrong-hash': value['pcm_sha256'] = '0' * 64
if MODE == 'wrong-frames': value['frames'] += 1
if MODE == 'wrong-recipe': value['recipe'] += '-other'
if MODE == 'bool-schema': value['schema'] = True
if MODE == 'fractional-count': value['beat_count'] = 2.5
if MODE == 'oversized-count': value['beat_count'] = 34954
if MODE == 'source-count-limit': value['beat_count'] = a.frames * 50 // 48000 + 2
if MODE == 'negative-count': value['beat_count'] = -1
if MODE == 'bool-count': value['beat_count'] = True
if MODE == 'insufficient-beats': value['beat_count'] = 1
if MODE == 'nonfinite': value['bpm'] = float('nan')
if MODE == 'low-bpm': value['bpm'] = 19.999
if MODE == 'high-bpm': value['bpm'] = 400.001
if MODE == 'overprecision': value['bpm'] = 120.125
if MODE == 'extra': value['surprise'] = 'ignored?'
if MODE == 'missing': del value['bpm']
if MODE == 'wrong-type': value['bpm'] = '120'
# Synthetic protocol fixtures only: no model or musical accuracy is asserted.
events = [{'frame': frame, 'downbeat': index in (0, 3)}
          for index, frame in enumerate((0, 960, 1920, a.frames - 1))]
if MODE == 'unknown': events = []
if MODE == 'one-beat': events = [{'frame': 100, 'downbeat': True}]
if MODE == 'alternate-timing':
    events = [{'frame': frame, 'downbeat': index == 1}
              for index, frame in enumerate((1, 1001, 2001, 3001))]
if MODE == 'exact-output-limit':
    events = [{'frame': index * 960, 'downbeat': index == 299} for index in range(300)]
if MODE == 'event-bool-frame': events[0]['frame'] = False
if MODE == 'event-negative': events[0]['frame'] = -1
if MODE == 'event-fractional': events[0]['frame'] = .5
if MODE == 'event-eof': events[-1]['frame'] = a.frames
if MODE in ('event-duplicate', 'await-change-bad-protocol'): events[1]['frame'] = events[0]['frame']
if MODE == 'event-reversed': events[1], events[2] = events[2], events[1]
if MODE == 'event-numeric-downbeat': events[0]['downbeat'] = 1
if MODE == 'event-null-downbeat': events[0]['downbeat'] = None
if MODE == 'event-missing': del events[0]['downbeat']
if MODE == 'event-extra': events[0]['extra'] = 0
if MODE == 'missing-event': events.pop()
if MODE == 'extra-event': events.append({'frame': a.frames - 1, 'downbeat': False})
header = json.dumps(value, separators=(',', ':'))
rows = [json.dumps(event, separators=(',', ':')) for event in events]
if MODE == 'event-duplicate-key': rows[0] = '{"frame":0,"frame":1,"downbeat":true}'
if MODE == 'event-exponent': rows[0] = '{"frame":0e0,"downbeat":true}'
if MODE == 'event-float-spelling': rows[0] = '{"frame":0.0,"downbeat":true}'
if MODE == 'event-over-line': rows[0] += ' ' * 16384
if MODE == 'duplicate': header = '{"schema":0,' + header[1:]
if MODE == 'malformed': header = '{not-json'
if MODE == 'pretty': header = json.dumps(value, indent=2)
if MODE == 'header-over-limit': header += ' ' * (4097 - len(header) - 1)
if MODE == 'blank-row': rows.insert(1, '')
lines = [header, *rows]
if MODE == 'exact-output-limit':
    remaining = 4 * 1024**2 - len(('\n'.join(lines) + '\n').encode())
    for index in range(1, len(lines)):
        padding = min(remaining, 16383 - len(lines[index].encode()))
        lines[index] += ' ' * padding
        remaining -= padding
    assert remaining == 0
out = '\n'.join(lines) + '\n'
if MODE == 'two-results': out += out
if MODE == 'header-only': out = header + '\n'
if MODE == 'missing-header-lf': out = header + '\n'.join(rows) + '\n'
if MODE == 'event-before-header': out = rows[0] + '\n' + out
if MODE == 'unterminated': out = out[:-1]
if MODE == 'trailing-bytes': out += 'x'
if MODE == 'partial-descendant':
    sys.stdout.write(header + '\n' + rows[0] + '\n'); sys.stdout.flush()
    P.with_suffix('.partial').write_text('header and one event flushed\n')
    time.sleep(60)
if MODE == 'fragmented':
    encoded = out.encode()
    for index in range(0, len(encoded), 3):
        os.write(sys.stdout.fileno(), encoded[index:index + 3])
else:
    sys.stdout.write(out)
    sys.stdout.flush()
if MODE in ('bad-exit', 'await-change-bad-exit'): sys.exit(7)
)PY";
        write(engine.workerPath, script);
    }
    bool started() const
    {
        const auto end = std::chrono::steady_clock::now() + 2s;
        while (!QFileInfo::exists(marker) && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(5ms);
        return QFileInfo::exists(marker);
    }
};
void absent(const QString& marker)
{
    const auto pids = QJsonDocument::fromJson(read(marker)).array();
    for (const auto value : pids) {
        const auto pid = pid_t(value.toInteger());
        if (pid > 1) {
            QVERIFY(kill(pid, 0) == -1 && errno == ESRCH);
            // The direct worker is also its explicitly owned session/group.
            if (value == pids.first()) QVERIFY(kill(-pid, 0) == -1 && errno == ESRCH);
        }
    }
}
} // namespace

class BpmAnalyzerTest : public QObject {
    Q_OBJECT
private slots:
    void estimatesAndUnknownPreservePreparedOffset()
    {
        for (const auto& mode : {QByteArray("good"), QByteArray("fragmented"), QByteArray("limits"), QByteArray("unknown"), QByteArray("one-beat"), QByteArray("unknown-many")}) {
            Fixture fixture(mode);
            QCOMPARE(fcntl(fixture.pcm->descriptor(), F_GETFL) & O_ACCMODE, O_RDONLY);
            QCOMPARE(lseek(fixture.pcm->descriptor(), 13, SEEK_SET), off_t(13));
            const auto result = library::analyzeBpm(*fixture.pcm, fixture.engine);
            QVERIFY(result.error.isEmpty());
            const bool estimated = mode == "good" || mode == "fragmented" || mode == "limits";
            QCOMPARE(result.status, estimated ? QString("Estimated") : QString("Unknown"));
            QCOMPARE(result.bpm.has_value(), estimated);
            if (estimated) QCOMPARE(*result.bpm, 120.);
            QCOMPARE(result.analysisFrames, Fixture::frames);
            QCOMPARE(result.pcmSha256, QCryptographicHash::hash(read(fixture.directory.filePath("input.f32")),
                                                              QCryptographicHash::Sha256).toHex());
            const std::vector<std::uint32_t> expected = mode == "unknown" ? std::vector<std::uint32_t>{} :
                mode == "one-beat" ? std::vector<std::uint32_t>{100} :
                std::vector<std::uint32_t>{0, 960, 1920, std::uint32_t(Fixture::frames - 1)};
            QCOMPARE(result.beats.size(), expected.size());
            for (std::size_t index = 0; index < expected.size(); ++index) {
                QCOMPARE(result.beats[index].frame, expected[index]);
                QCOMPARE(result.beats[index].downbeat, mode == "one-beat" || index == 0 || index == 3);
            }
            QCOMPARE(lseek(fixture.pcm->descriptor(), 0, SEEK_CUR), off_t(13));
            absent(fixture.marker);
        }
    }
    void invalidWorkerResults_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto* mode : {"wrong-hash", "wrong-frames", "wrong-recipe", "bool-schema", "fractional-count",
             "oversized-count", "source-count-limit", "negative-count", "bool-count", "insufficient-beats", "nonfinite", "low-bpm",
             "high-bpm", "overprecision", "extra", "missing", "wrong-type", "duplicate", "malformed", "two-results", "bad-exit",
             "schema-one", "pretty", "header-over-limit", "header-only", "missing-header-lf", "event-before-header",
             "event-bool-frame", "event-negative", "event-fractional", "event-eof", "event-duplicate", "event-reversed",
             "event-numeric-downbeat", "event-null-downbeat", "event-missing", "event-extra", "event-duplicate-key",
             "event-exponent", "event-float-spelling", "event-over-line", "missing-event", "extra-event",
             "blank-row", "unterminated", "trailing-bytes"})
            QTest::newRow(mode) << QByteArray(mode);
    }
    void invalidWorkerResults()
    {
        QFETCH(QByteArray, mode);
        Fixture fixture(mode);
        const auto result = library::analyzeBpm(*fixture.pcm, fixture.engine);
        QCOMPARE(result.status, QString("Failed"));
        QVERIFY(!result.bpm && !result.error.isEmpty());
        QCOMPARE(result.analysisFrames, std::uint64_t(0));
        QVERIFY(result.pcmSha256.isEmpty() && result.beats.empty());
        absent(fixture.marker);
    }
    void boundedWorkerFailures_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto* mode : {"block", "stdout-flood", "stderr-flood"}) QTest::newRow(mode) << QByteArray(mode);
    }
    void boundedWorkerFailures()
    {
        QFETCH(QByteArray, mode);
        Fixture fixture(mode);
        fixture.engine.maxSeconds = 1;
        const auto begin = std::chrono::steady_clock::now();
        const auto result = library::analyzeBpm(*fixture.pcm, fixture.engine);
        QCOMPARE(result.status, QString("Failed"));
        QVERIFY(!result.bpm && !result.error.isEmpty());
        QVERIFY(std::chrono::steady_clock::now() - begin < 6s); // Failure/cleanup guard, not inference qualification.
        absent(fixture.marker);
    }
    void cancellationReapsOwnedWorkerAndDescendant()
    {
        Fixture fixture("descendant");
        std::stop_source cancellation;
        auto future = std::async(std::launch::async, [&] {
            try { (void)library::analyzeBpm(*fixture.pcm, fixture.engine, cancellation.get_token()); }
            catch (const std::runtime_error& error) { return std::string(error.what()).find("cancel") != std::string::npos; }
            return false;
        });
        const bool started = fixture.started();
        cancellation.request_stop();
        const bool settled = future.wait_for(6s) == std::future_status::ready;
        QVERIFY(started && settled);
        QVERIFY(future.get());
        absent(fixture.marker);
        Fixture neverStarted;
        QVERIFY_EXCEPTION_THROWN(library::analyzeBpm(*neverStarted.pcm, neverStarted.engine, cancellation.get_token()), std::runtime_error);
        QVERIFY(!QFileInfo::exists(neverStarted.marker));
    }
    void changedAndNonfinitePcmFailTheLoad()
    {
        for (const auto& mode : {QByteArray("await-change"), QByteArray("await-change-bad-exit"), QByteArray("await-change-bad-protocol")}) {
            for (const bool truncate : {false, true}) {
                Fixture fixture(mode);
                auto future = std::async(std::launch::async, [&] { return library::analyzeBpm(*fixture.pcm, fixture.engine); });
                const bool started = fixture.started();
                const float changed = .875F;
                const bool modified = truncate ? ftruncate(fixture.writable.value, 16) == 0 :
                    pwrite(fixture.writable.value, &changed, sizeof(changed), 0) == sizeof(changed);
                write(fixture.directory.filePath("worker.release"), "release\n");
                QVERIFY(started && modified);
                QVERIFY_EXCEPTION_THROWN(future.get(), library::BpmSourceError);
                absent(fixture.marker);
            }
        }
        Fixture nonfinite;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        QCOMPARE(pwrite(nonfinite.writable.value, &nan, sizeof(nan), 0), ssize_t(sizeof(nan)));
        QVERIFY_EXCEPTION_THROWN(library::analyzeBpm(*nonfinite.pcm, nonfinite.engine), library::BpmSourceError);
        QVERIFY(!QFileInfo::exists(nonfinite.marker));
        Fixture truncated;
        QCOMPARE(ftruncate(truncated.writable.value, 16), 0);
        QVERIFY_EXCEPTION_THROWN(library::analyzeBpm(*truncated.pcm, truncated.engine), library::BpmSourceError);
        QVERIFY(!QFileInfo::exists(truncated.marker));
    }
    void timingMapsKeepTheirOwnPreparedIdentity()
    {
        Fixture first, second("alternate-timing");
        const float changed = .875F;
        QCOMPARE(pwrite(second.writable.value, &changed, sizeof(changed), 0), ssize_t(sizeof(changed)));
        const auto a = library::analyzeBpm(*first.pcm, first.engine);
        const auto b = library::analyzeBpm(*second.pcm, second.engine);
        QCOMPARE(a.status, QString("Estimated"));
        QCOMPARE(b.status, QString("Estimated"));
        QCOMPARE(a.analysisFrames, b.analysisFrames);
        QVERIFY(a.pcmSha256 != b.pcmSha256);
        QCOMPARE(b.pcmSha256, QCryptographicHash::hash(read(second.directory.filePath("input.f32")),
                                                     QCryptographicHash::Sha256).toHex());
        QCOMPARE(a.beats.front().frame, std::uint32_t(0));
        QCOMPARE(b.beats.front().frame, std::uint32_t(1));
        QVERIFY(a.beats.front().downbeat && !b.beats.front().downbeat && b.beats[1].downbeat);
        absent(first.marker);
        absent(second.marker);
    }
    void wholeOutputLimitIsInclusive()
    {
        Fixture fixture("exact-output-limit", 300000);
        fixture.engine.maxSeconds = 10; // Bounded protocol parsing, not model inference.
        const auto result = library::analyzeBpm(*fixture.pcm, fixture.engine);
        QCOMPARE(result.status, QString("Unknown"));
        QVERIFY(!result.bpm && result.error.isEmpty());
        QCOMPARE(result.analysisFrames, std::uint64_t(300000));
        QCOMPARE(result.beats.size(), std::size_t(300));
        QCOMPARE(result.beats.back().frame, std::uint32_t(299 * 960));
        QVERIFY(result.beats.back().downbeat);
        absent(fixture.marker);
    }
    void cancellationDuringPartialMapReapsOwnedDescendant()
    {
        Fixture fixture("partial-descendant");
        std::stop_source cancellation;
        auto future = std::async(std::launch::async, [&] {
            try { (void)library::analyzeBpm(*fixture.pcm, fixture.engine, cancellation.get_token()); }
            catch (const std::runtime_error& error) { return std::string(error.what()).find("cancel") != std::string::npos; }
            return false;
        });
        const auto limit = std::chrono::steady_clock::now() + 2s;
        const auto partial = fixture.directory.filePath("worker.partial");
        while (!QFileInfo::exists(partial) && std::chrono::steady_clock::now() < limit)
            std::this_thread::sleep_for(5ms);
        const bool emitted = QFileInfo::exists(partial);
        cancellation.request_stop();
        const bool settled = future.wait_for(6s) == std::future_status::ready;
        QVERIFY(emitted && settled);
        QVERIFY(future.get());
        absent(fixture.marker);
    }
    void configurationFailureDoesNotLaunch()
    {
        Fixture fixture;
        for (const int budget : {0, 181}) {
            auto engine = fixture.engine; engine.maxSeconds = budget;
            const auto result = library::analyzeBpm(*fixture.pcm, engine);
            QCOMPARE(result.status, QString("Failed"));
            QVERIFY(!result.bpm && !QFileInfo::exists(fixture.marker));
        }
        auto engine = fixture.engine; engine.workerPath = "relative.py";
        QCOMPARE(library::analyzeBpm(*fixture.pcm, engine).status, QString("Failed"));
        QVERIFY(!QFileInfo::exists(fixture.marker));
    }
    void interpreterInvocationPreservesVenvIdentity()
    {
        Fixture fixture("require-venv");
        const auto venv = fixture.directory.filePath("venv");
        QVERIFY(QDir().mkpath(venv + "/bin"));
        const auto alias = venv + "/bin/python3";
        QCOMPARE(symlink(QFile::encodeName(fixture.engine.pythonExecutable).constData(), QFile::encodeName(alias).constData()), 0);
        write(venv + "/pyvenv.cfg", "home = " + QFile::encodeName(QFileInfo(fixture.engine.pythonExecutable).absolutePath()) +
              "\ninclude-system-site-packages = false\n");
        fixture.engine.pythonExecutable = alias;
        const auto result = library::analyzeBpm(*fixture.pcm, fixture.engine);
        QCOMPARE(result.status, QString("Estimated"));
        QVERIFY(result.bpm && *result.bpm == 120. && result.error.isEmpty());
        absent(fixture.marker);
    }
    void tempoTargetsStayInsideTheExactRail()
    {
        struct Row { double reference, target, percent; };
        for (const auto row : {Row{120, 126, 5}, Row{128, 130, 1.5625}, Row{120, 112.8, -6},
                               Row{120, 127.2, 6}, Row{20, 20, 0}, Row{400, 400, 0}}) {
            const auto result = library::tempoPercentForBpm(row.reference, row.target);
            QVERIFY(result);
            QVERIFY(std::abs(*result - row.percent) < 1e-12);
        }
        const double infinity = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
        for (const auto reference : {0., 19.999, 400.001, -120., infinity, nan})
            QVERIFY(!library::tempoPercentForBpm(reference, 120));
        for (const auto target : {0., -1., infinity, nan, std::nextafter(120 * .94, -infinity),
                                  std::nextafter(120 * 1.06, infinity)})
            QVERIFY(!library::tempoPercentForBpm(120, target));
    }
};

QTEST_GUILESS_MAIN(BpmAnalyzerTest)
#include "bpm_analyzer_test.moc"

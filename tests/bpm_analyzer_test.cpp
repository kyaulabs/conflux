// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/BpmAnalyzer.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
#include <sys/file.h>
#include <unistd.h>

namespace {
using namespace conflux;
using namespace std::chrono_literals;
constexpr std::array cacheSourceNames{
    "beat_this/utils.py", "beat_this/model/roformer.py", "beat_this/model/beat_tracker.py",
    "beat_this/model/postprocessor.py", "beat_this/preprocessing.py", "beat_this/inference.py", "LICENSE"};
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
with P.with_suffix('.invocations').open('ab') as invocations:
    invocations.write(b'run\n')
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
if MODE == 'manual-status': value['status'] = 'Manual'
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
if MODE == 'local-variable':
    events = [{'frame': frame, 'downbeat': index == 0}
              for index, frame in enumerate((12000, 36000, 54000, 70000, 84400))]
    value.update(bpm=None, beat_count=len(events))
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
    void prepareCacheIdentity() const
    {
        // These are protocol-fixture identities, not a fake BeatThis model.
        // The generated worker never imports them or claims detector accuracy.
        for (const auto* relative : cacheSourceNames) {
            const auto path = directory.filePath(QString::fromLatin1(relative));
            if (!QDir().mkpath(QFileInfo(path).absolutePath()))
                throw std::runtime_error("Cannot create fake BPM source identity");
            write(path, QByteArray("# Generated identity fixture: ") + relative + '\n');
        }
    }
    QString cacheRoot() const
    {
        const auto path = directory.filePath("analysis-cache");
        if (!QDir().mkdir(path) ||
            !QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            throw std::runtime_error("Cannot create private BPM analysis cache fixture");
        return path;
    }
    int invocations() const { return read(directory.filePath("worker.invocations")).count('\n'); }
    void useVenv()
    {
        const auto prefix = directory.filePath("venv");
        if (!QDir().mkpath(prefix + "/bin")) throw std::runtime_error("Cannot create fake BPM venv");
        const auto invocation = prefix + "/bin/python3";
        if (symlink(QFile::encodeName(engine.pythonExecutable).constData(), QFile::encodeName(invocation).constData()))
            throw std::runtime_error("Cannot create BPM interpreter invocation alias");
        write(prefix + "/pyvenv.cfg", "home = " + QFile::encodeName(QFileInfo(engine.pythonExecutable).absolutePath()) +
              "\ninclude-system-site-packages = false\n");
        engine.pythonExecutable = invocation;
    }
};
QString cacheEntry(const QString& root)
{
    const auto entries = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden);
    if (entries.size() != 1) throw std::runtime_error("Expected one completed BPM cache entry");
    return QDir(root).filePath(entries.front());
}
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
    void cachedValidatedMapsSkipTheSecondWorker_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto* mode : {"good", "unknown-many", "one-beat", "unknown"})
            QTest::newRow(mode) << QByteArray(mode);
    }
    void cachedValidatedMapsSkipTheSecondWorker()
    {
        QFETCH(QByteArray, mode);
        Fixture fixture(mode);
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        QCOMPARE(lseek(fixture.pcm->descriptor(), 13, SEEK_SET), off_t(13));
        const auto first = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        const bool estimated = mode == "good";
        QCOMPARE(first.status, estimated ? QString("Estimated") : QString("Unknown"));
        QCOMPARE(first.bpm.has_value(), estimated);
        if (estimated) QCOMPARE(*first.bpm, 120.);
        QVERIFY(first.error.isEmpty() && !first.cacheHit);
        QCOMPARE(first.analysisFrames, Fixture::frames);
        QCOMPARE(first.pcmSha256, QCryptographicHash::hash(read(fixture.directory.filePath("input.f32")),
                                                         QCryptographicHash::Sha256).toHex());
        const std::vector<std::uint32_t> expected = mode == "unknown" ? std::vector<std::uint32_t>{} :
            mode == "one-beat" ? std::vector<std::uint32_t>{100} :
            std::vector<std::uint32_t>{0, 960, 1920, std::uint32_t(Fixture::frames - 1)};
        QCOMPARE(first.beats.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(first.beats[index].frame, expected[index]);
            QCOMPARE(first.beats[index].downbeat, mode == "one-beat" || index == 0 || index == 3);
        }
        const auto invocations = fixture.directory.filePath("worker.invocations");
        QCOMPARE(read(invocations), QByteArray("run\n"));
        QCOMPARE(lseek(fixture.pcm->descriptor(), 0, SEEK_CUR), off_t(13));
        absent(fixture.marker);

        const auto second = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        // Count real process entries before inspecting cacheHit: the delegating
        // stub must fail on an actual second child, not just a missing flag.
        QCOMPARE(read(invocations), QByteArray("run\n"));
        QVERIFY(second.cacheHit);
        QCOMPARE(second.status, first.status);
        QCOMPARE(second.error, first.error);
        QCOMPARE(second.bpm.has_value(), first.bpm.has_value());
        if (first.bpm) QCOMPARE(*second.bpm, *first.bpm);
        QCOMPARE(second.analysisFrames, first.analysisFrames);
        QCOMPARE(second.pcmSha256, first.pcmSha256);
        QCOMPARE(second.beats.size(), first.beats.size());
        for (std::size_t index = 0; index < first.beats.size(); ++index) {
            QCOMPARE(second.beats[index].frame, first.beats[index].frame);
            QCOMPARE(second.beats[index].downbeat, first.beats[index].downbeat);
        }
        QCOMPARE(lseek(fixture.pcm->descriptor(), 0, SEEK_CUR), off_t(13));
        absent(fixture.marker);
    }
    void cacheIdentityTracksActualProducerAndPcmBytes_data()
    {
        QTest::addColumn<QString>("changed");
        for (const auto* part : {"pcm", "worker", "model", "python-invocation", "venv-config"})
            QTest::newRow(part) << QString::fromLatin1(part);
        for (const auto* relative : cacheSourceNames)
            QTest::newRow(relative) << QStringLiteral("source:") + QString::fromLatin1(relative);
    }
    void cacheIdentityTracksActualProducerAndPcmBytes()
    {
        QFETCH(QString, changed);
        Fixture fixture(changed == "venv-config" ? QByteArray("require-venv") : QByteArray("good"));
        fixture.prepareCacheIdentity();
        if (changed == "venv-config") fixture.useVenv();
        const auto cache = fixture.cacheRoot();
        QCOMPARE(lseek(fixture.pcm->descriptor(), 13, SEEK_SET), off_t(13));
        const auto first = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QCOMPARE(first.status, QString("Estimated"));
        QVERIFY(!first.cacheHit && first.error.isEmpty());
        QCOMPARE(fixture.invocations(), 1);
        if (changed == "pcm") {
            const float replacement = .875F;
            QCOMPARE(pwrite(fixture.writable.value, &replacement, sizeof(replacement), 0), ssize_t(sizeof(replacement)));
        } else if (changed == "python-invocation") {
            const auto alias = fixture.directory.filePath("python-invocation");
            QCOMPARE(symlink(QFile::encodeName(fixture.engine.pythonExecutable).constData(), QFile::encodeName(alias).constData()), 0);
            fixture.engine.pythonExecutable = alias;
        } else {
            const auto path = changed == "worker" ? fixture.engine.workerPath :
                changed == "model" ? fixture.engine.modelPath : changed == "venv-config" ?
                fixture.directory.filePath("venv/pyvenv.cfg") : fixture.directory.filePath(changed.sliced(7));
            write(path, read(path) + "\n# identity changed\n");
        }
        const auto second = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QCOMPARE(second.status, QString("Estimated"));
        QVERIFY(!second.cacheHit && second.error.isEmpty());
        QCOMPARE(fixture.invocations(), 2);
        QCOMPARE(second.analysisFrames, Fixture::frames);
        QCOMPARE(second.pcmSha256, QCryptographicHash::hash(read(fixture.directory.filePath("input.f32")),
                                                          QCryptographicHash::Sha256).toHex());
        QCOMPARE(second.pcmSha256 == first.pcmSha256, changed != "pcm");
        QCOMPARE(second.beats.size(), std::size_t(4));
        const auto hit = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QVERIFY(hit.cacheHit && hit.error.isEmpty());
        QCOMPARE(hit.pcmSha256, second.pcmSha256);
        QCOMPARE(hit.beats.size(), second.beats.size());
        QCOMPARE(fixture.invocations(), 2);
        QCOMPARE(lseek(fixture.pcm->descriptor(), 0, SEEK_CUR), off_t(13));
        absent(fixture.marker);
    }
    void corruptOrUnsafeEntriesAreNeverTrusted_data()
    {
        QTest::addColumn<QByteArray>("damage");
        for (const auto* damage : {"timing-json", "timing-protocol", "timing-oversize", "manifest-json", "manifest-oversize",
                                  "timing-symlink", "manifest-symlink", "entry-symlink", "root-symlink"})
            QTest::newRow(damage) << QByteArray(damage);
    }
    void corruptOrUnsafeEntriesAreNeverTrusted()
    {
        QFETCH(QByteArray, damage);
        Fixture fixture;
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        const auto first = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QCOMPARE(first.status, QString("Estimated"));
        QVERIFY(!first.cacheHit);
        const auto entry = cacheEntry(cache);
        const auto payload = entry + "/timing.jsonl", manifest = entry + "/manifest.json";
        const auto pcmBefore = read(fixture.directory.filePath("input.f32"));
        QString protectedPath, unsafeAlias, unsafeTarget;
        QByteArray protectedBytes;
        if (damage.endsWith("-symlink")) {
            const auto source = damage == "timing-symlink" ? payload : damage == "manifest-symlink" ? manifest :
                damage == "entry-symlink" ? entry : cache;
            const auto outside = fixture.directory.filePath("outside-" + QString::fromLatin1(damage));
            if (damage == "entry-symlink" || damage == "root-symlink") {
                QVERIFY(QDir().rename(source, outside));
                protectedPath = outside + (damage == "root-symlink" ? "/" + QFileInfo(entry).fileName() : QString{}) + "/timing.jsonl";
            } else {
                protectedBytes = read(source);
                write(outside, protectedBytes);
                QVERIFY(QFile::setPermissions(outside, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
                QVERIFY(QFile::remove(source));
                protectedPath = outside;
            }
            QCOMPARE(symlink(QFile::encodeName(outside).constData(), QFile::encodeName(source).constData()), 0);
            unsafeAlias = source; unsafeTarget = outside;
            protectedBytes = read(protectedPath);
        } else {
            protectedPath = damage.startsWith("timing-") ? payload : manifest;
            protectedBytes = damage == "timing-protocol" ? read(payload) : damage.endsWith("oversize") ?
                QByteArray(damage.startsWith("timing-") ? 4 * 1024 * 1024 + 1 : 16385, 'x') : QByteArray("{not valid JSON}\n");
            if (damage == "timing-protocol") {
                const auto newline = protectedBytes.indexOf('\n');
                auto header = QJsonDocument::fromJson(protectedBytes.first(newline)).object();
                header["status"] = "Manual"; // Not a permitted worker/cache result, even with a matching payload hash.
                protectedBytes = QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n' + protectedBytes.sliced(newline + 1);
                auto metadata = QJsonDocument::fromJson(read(manifest)).object();
                metadata["timing_bytes"] = double(protectedBytes.size());
                metadata["timing_sha256"] = QString::fromLatin1(QCryptographicHash::hash(protectedBytes, QCryptographicHash::Sha256).toHex());
                write(manifest, QJsonDocument(metadata).toJson(QJsonDocument::Compact) + '\n');
            }
            write(protectedPath, protectedBytes);
        }
        const auto second = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QCOMPARE(second.status, QString("Estimated"));
        QVERIFY(!second.cacheHit && second.error.isEmpty());
        QCOMPARE(fixture.invocations(), 2);
        QCOMPARE(second.pcmSha256, first.pcmSha256);
        QCOMPARE(read(protectedPath), protectedBytes); // No eviction, repair or writes through aliases.
        if (!unsafeAlias.isEmpty()) {
            QVERIFY(QFileInfo(unsafeAlias).isSymLink());
            QCOMPARE(QFileInfo(unsafeAlias).symLinkTarget(), unsafeTarget);
        }
        QCOMPARE(read(fixture.directory.filePath("input.f32")), pcmBefore);
        absent(fixture.marker);
    }
    void failedAndManualProtocolResultsAreNotCached_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto* mode : {"bad-exit", "event-duplicate", "header-only", "manual-status"})
            QTest::newRow(mode) << QByteArray(mode);
    }
    void failedAndManualProtocolResultsAreNotCached()
    {
        QFETCH(QByteArray, mode);
        Fixture fixture(mode);
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        for (int invocation = 1; invocation <= 2; ++invocation) {
            const auto result = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
            QCOMPARE(result.status, QString("Failed"));
            QVERIFY(!result.cacheHit && !result.bpm && !result.error.isEmpty());
            QVERIFY(result.beats.empty() && result.pcmSha256.isEmpty() && !result.analysisFrames);
            QCOMPARE(fixture.invocations(), invocation);
            QVERIFY(QDir(cache).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
            absent(fixture.marker);
        }
    }
    void cachedHitStillHonorsCancellationAndSourceCorruption()
    {
        Fixture fixture;
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        QCOMPARE(library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache).status, QString("Estimated"));
        const auto entry = cacheEntry(cache);
        const auto manifestBefore = read(entry + "/manifest.json"), timingBefore = read(entry + "/timing.jsonl");
        std::stop_source cancelled;
        cancelled.request_stop();
        const auto throwsCancellation = [&](const auto& operation) {
            try { operation(); }
            catch (const std::runtime_error& error) { return std::string(error.what()).find("cancel") != std::string::npos; }
            return false;
        };
        QVERIFY(throwsCancellation([&] { (void)library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache, cancelled.get_token()); }));
        const float nan = std::numeric_limits<float>::quiet_NaN();
        QCOMPARE(pwrite(fixture.writable.value, &nan, sizeof(nan), 0), ssize_t(sizeof(nan)));
        QVERIFY_EXCEPTION_THROWN(library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache), library::BpmSourceError);
        QVERIFY(throwsCancellation([&] { (void)library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache, cancelled.get_token()); }));
        QCOMPARE(fixture.invocations(), 1);
        QCOMPARE(read(entry + "/manifest.json"), manifestBefore);
        QCOMPARE(read(entry + "/timing.jsonl"), timingBefore);
        absent(fixture.marker);
    }
    void cancelledMissReapsDescendantAndPublishesNothing()
    {
        Fixture fixture("partial-descendant");
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        std::stop_source cancellation;
        auto future = std::async(std::launch::async, [&] {
            try { (void)library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache, cancellation.get_token()); }
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
        QCOMPARE(fixture.invocations(), 1);
        QVERIFY(QDir(cache).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
        absent(fixture.marker);
    }
    void changedPcmDuringMissWinsOverWorkerFailureAndCannotPublish()
    {
        Fixture fixture("await-change-bad-exit");
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        auto future = std::async(std::launch::async, [&] { return library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache); });
        const bool started = fixture.started();
        const float replacement = .875F;
        const bool modified = pwrite(fixture.writable.value, &replacement, sizeof(replacement), 0) == sizeof(replacement);
        write(fixture.directory.filePath("worker.release"), "release\n");
        QVERIFY(started && modified);
        QVERIFY_EXCEPTION_THROWN(future.get(), library::BpmSourceError);
        QCOMPARE(fixture.invocations(), 1);
        QVERIFY(QDir(cache).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
        absent(fixture.marker);
    }
    void producerMutationDuringMissCannotPublish_data()
    {
        QTest::addColumn<QByteArray>("changed");
        for (const auto* part : {"worker", "model", "source"}) QTest::newRow(part) << QByteArray(part);
    }
    void producerMutationDuringMissCannotPublish()
    {
        QFETCH(QByteArray, changed);
        Fixture fixture("await-change");
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        auto future = std::async(std::launch::async, [&] { return library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache); });
        const bool started = fixture.started();
        const auto path = changed == "worker" ? fixture.engine.workerPath : changed == "model" ?
            fixture.engine.modelPath : fixture.directory.filePath("beat_this/model/beat_tracker.py");
        write(path, read(path) + "\n# Changed after the child started\n");
        write(fixture.directory.filePath("worker.release"), "release\n");
        QVERIFY(started);
        const auto result = future.get();
        QCOMPARE(result.status, QString("Failed"));
        QVERIFY(!result.cacheHit && !result.bpm && !result.error.isEmpty() && result.beats.empty());
        QCOMPARE(fixture.invocations(), 1);
        QVERIFY(QDir(cache).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
        absent(fixture.marker);
    }
    void busyCacheFallsBackWithoutReplacingTheRetainedResult()
    {
        Fixture fixture;
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        QCOMPARE(library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache).status, QString("Estimated"));
        const auto entry = cacheEntry(cache);
        const auto manifest = read(entry + "/manifest.json"), timing = read(entry + "/timing.jsonl");
        CloseFd lock;
        lock.value = open(QFile::encodeName(cache + "/.lock").constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        QVERIFY(lock.value >= 0);
        QCOMPARE(flock(lock.value, LOCK_EX | LOCK_NB), 0);
        const auto fallback = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QCOMPARE(fallback.status, QString("Estimated"));
        QVERIFY(!fallback.cacheHit && fallback.error.isEmpty());
        QCOMPARE(fixture.invocations(), 2);
        QCOMPARE(read(entry + "/manifest.json"), manifest);
        QCOMPARE(read(entry + "/timing.jsonl"), timing);
        QCOMPARE(flock(lock.value, LOCK_UN), 0);
        const auto hit = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QVERIFY(hit.cacheHit && hit.error.isEmpty());
        QCOMPARE(fixture.invocations(), 2);
        absent(fixture.marker);
    }
    void cachePublicationIsBoundedAndIsolatedFromPreparedLibraryFiles()
    {
        Fixture fixture("exact-output-limit", 300000);
        fixture.engine.maxSeconds = 10;
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        const auto prepared = fixture.directory.filePath("prepared/v1"), catalog = fixture.directory.filePath("catalog/library.json");
        QVERIFY(QDir().mkpath(prepared) && QDir().mkpath(QFileInfo(catalog).absolutePath()));
        const auto sentinel = prepared + "/retained.f32";
        write(sentinel, "Existing prepared-cache sentinel\n");
        write(catalog, "{\"existing_library\":true}\n");
        const auto pcmBefore = read(fixture.directory.filePath("input.f32"));
        const auto preparedBefore = read(sentinel), catalogBefore = read(catalog);
        const auto first = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QCOMPARE(first.status, QString("Unknown"));
        QVERIFY(!first.cacheHit && first.error.isEmpty());
        QCOMPARE(first.beats.size(), std::size_t(300));
        const auto entry = cacheEntry(cache);
        QCOMPARE(QFileInfo(entry + "/timing.jsonl").size(), qint64(4 * 1024 * 1024));
        QVERIFY(QFileInfo(entry + "/manifest.json").size() > 0 && QFileInfo(entry + "/manifest.json").size() <= 16384);
        QCOMPARE(QDir(entry).entryList(QDir::Files | QDir::NoDotAndDotDot), QStringList({"manifest.json", "timing.jsonl"}));
        const auto second = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QVERIFY(second.cacheHit && second.error.isEmpty());
        QCOMPARE(second.beats.size(), first.beats.size());
        QCOMPARE(fixture.invocations(), 1);
        QCOMPARE(read(sentinel), preparedBefore);
        QCOMPARE(read(catalog), catalogBefore);
        QCOMPARE(read(fixture.directory.filePath("input.f32")), pcmBefore);
        absent(fixture.marker);

        Fixture full;
        full.prepareCacheIdentity();
        const auto fullCache = full.cacheRoot();
        for (int index = 0; index < 64; ++index) {
            const auto reserved = QDir(fullCache).filePath(QString("%1").arg(index, 64, 16, QChar(u'0')));
            QVERIFY(QDir().mkdir(reserved));
            QVERIFY(QFile::setPermissions(reserved, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
            for (const auto* name : {"manifest.json", "timing.jsonl"}) {
                const auto path = reserved + '/' + QString::fromLatin1(name);
                write(path, "Do not evict existing entries\n");
                QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            }
        }
        const auto entriesBefore = QDir(fullCache).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
        const auto fallback = library::analyzeBpmCached(*full.pcm, full.engine, fullCache);
        QCOMPARE(fallback.status, QString("Estimated"));
        QVERIFY(!fallback.cacheHit && fallback.error.isEmpty());
        QCOMPARE(full.invocations(), 1);
        QCOMPARE(QDir(fullCache).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden), entriesBefore);
        for (const auto& name : entriesBefore)
            for (const auto* leaf : {"manifest.json", "timing.jsonl"})
                QCOMPARE(read(QDir(fullCache).filePath(name + '/' + QString::fromLatin1(leaf))), QByteArray("Do not evict existing entries\n"));
        absent(full.marker);
    }
    void cacheByteBudgetDoesNotDeleteEntriesToMakePublicationFit()
    {
        Fixture fixture("exact-output-limit", 300000);
        fixture.engine.maxSeconds = 10;
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        // 63 individually bounded occupied entries leave one directory slot,
        // but insufficient aggregate space for the next 4 MiB payload. Sparse
        // files test accounting, without writing/reading 253 MiB of contents.
        for (int index = 0; index < 63; ++index) {
            const auto directory = QDir(cache).filePath(QString("%1").arg(index, 64, 16, QChar(u'0')));
            QVERIFY(QDir().mkdir(directory));
            QVERIFY(QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
            for (const auto* leaf : {"timing.jsonl", "manifest.json"}) {
                QFile file(directory + '/' + QString::fromLatin1(leaf));
                QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
                QVERIFY(file.resize(QByteArray(leaf) == "timing.jsonl" ? 4 * 1024 * 1024 : 16384));
                QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            }
        }
        const auto before = QDir(cache).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
        const auto result = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QCOMPARE(result.status, QString("Unknown"));
        QVERIFY(!result.cacheHit && result.error.isEmpty());
        QCOMPARE(result.beats.size(), std::size_t(300));
        QCOMPARE(fixture.invocations(), 1);
        QCOMPARE(QDir(cache).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden), before);
        for (const auto& name : before) {
            QCOMPARE(QFileInfo(QDir(cache).filePath(name + "/timing.jsonl")).size(), qint64(4 * 1024 * 1024));
            QCOMPARE(QFileInfo(QDir(cache).filePath(name + "/manifest.json")).size(), qint64(16384));
        }
        absent(fixture.marker);
    }
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
    void localTempoTracksTransitionsAndSourceSeeks()
    {
        library::BpmEstimate estimate;
        estimate.analysisFrames = 144000;
        estimate.bpm = 120.;
        estimate.status = "Estimated";
        estimate.beats = {{4800, true}, {28800, false}, {52800, true}, {70800, false},
                          {86800, false}, {101200, true}, {113200, false}};
        struct Row { std::uint64_t frame; std::uint32_t first, end; double bpm; };
        // Deliberately seek backwards and across exact shared boundaries.
        for (const auto row : {Row{113199, 101200, 113200, 240}, Row{4800, 4800, 28800, 120},
                               Row{52800, 52800, 70800, 160}, Row{70800, 70800, 86800, 180},
                               Row{86800, 86800, 101200, 200}, Row{28800, 28800, 52800, 120},
                               Row{52799, 28800, 52800, 120}, Row{101200, 101200, 113200, 240}}) {
            const auto interval = library::localTempoAt(estimate, row.frame);
            QVERIFY(interval);
            QCOMPARE(interval->firstFrame, row.first);
            QCOMPARE(interval->endFrame, row.end);
            QCOMPARE(interval->bpm, row.bpm);
        }
        for (const auto frame : std::array<std::uint64_t, 6>{0, 4799, 113200, 143999, 144000,
                                                           std::numeric_limits<std::uint64_t>::max()})
            QVERIFY(!library::localTempoAt(estimate, frame));
        QCOMPARE(*estimate.bpm, 120.); // Local lookup must not rewrite the summary.
    }
    void localTempoUnknownSummaryKeepsSupportedIntervals()
    {
        library::BpmEstimate estimate;
        estimate.analysisFrames = 400000;
        estimate.status = "Unknown";
        estimate.beats = {{12000, true}, {156000, false}, {163200, false},
                          {170399, false}, {314400, false}, {338400, true}};
        const auto slow = library::localTempoAt(estimate, 12000);
        const auto fast = library::localTempoAt(estimate, 156000);
        const auto afterUnsupported = library::localTempoAt(estimate, 314400);
        QVERIFY(slow && fast && afterUnsupported);
        QCOMPARE(slow->bpm, 20.);
        QCOMPARE(fast->bpm, 400.);
        QCOMPARE(afterUnsupported->bpm, 120.);
        QVERIFY(!library::localTempoAt(estimate, 163200)); // 7199 frames: above 400 BPM.
        QVERIFY(!library::localTempoAt(estimate, 170399)); // 144001 frames: below 20 BPM.
        QVERIFY(!estimate.bpm);
        QCOMPARE(estimate.status, QString("Unknown"));
    }
    void localTempoRejectsMalformedWholeMaps_data()
    {
        QTest::addColumn<QByteArray>("fault");
        for (const auto* fault : {"zero-frames", "too-many-frames", "zero-beats", "one-beat", "first-at-eof",
                                  "duplicate-first", "duplicate-late", "descending-late", "at-eof", "beyond-eof",
                                  "event-cap", "source-event-cap"})
            QTest::newRow(fault) << QByteArray(fault);
    }
    void localTempoRejectsMalformedWholeMaps()
    {
        QFETCH(QByteArray, fault);
        library::BpmEstimate estimate;
        estimate.analysisFrames = 144000;
        estimate.beats = {{0, true}, {24000, false}, {48000, false}, {72000, true}};
        if (fault == "zero-frames") estimate.analysisFrames = 0;
        if (fault == "too-many-frames") estimate.analysisFrames = audio::PreparedAudio::defaultMaxPreparedBytes / 8U + 1;
        if (fault == "zero-beats") estimate.beats.clear();
        if (fault == "one-beat") estimate.beats.resize(1);
        if (fault == "first-at-eof") estimate.beats.front().frame = 144000;
        if (fault == "duplicate-first") estimate.beats[1].frame = 0;
        if (fault == "duplicate-late") estimate.beats.back().frame = 48000;
        if (fault == "descending-late") estimate.beats.back().frame = 47000;
        if (fault == "at-eof") estimate.beats.back().frame = 144000;
        if (fault == "beyond-eof") estimate.beats.back().frame = std::numeric_limits<std::uint32_t>::max();
        if (fault == "event-cap" || fault == "source-event-cap") {
            estimate.analysisFrames = fault == "event-cap" ? audio::PreparedAudio::defaultMaxPreparedBytes / 8U : 48000;
            estimate.beats.resize(fault == "event-cap" ? 34954 : 52);
            estimate.beats[0] = {0, false}; estimate.beats[1] = {24000, false};
            for (std::size_t index = 2; index < estimate.beats.size(); ++index)
                estimate.beats[index] = {std::uint32_t(24000 + index - 1), false};
        }
        // The queried first interval is valid in all late-corruption/cap cases.
        // Checking only neighbors would silently accept a malformed map.
        QVERIFY(!library::localTempoAt(estimate, 12000));
    }
    void cachedLocalTempoMatchesTheOriginalTimingMap()
    {
        Fixture fixture("local-variable", 144000);
        fixture.prepareCacheIdentity();
        const auto cache = fixture.cacheRoot();
        const auto first = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QVERIFY(first.error.isEmpty() && !first.cacheHit && !first.bpm);
        QCOMPARE(first.status, QString("Unknown"));
        QCOMPARE(fixture.invocations(), 1);
        const auto second = library::analyzeBpmCached(*fixture.pcm, fixture.engine, cache);
        QVERIFY(second.cacheHit && second.error.isEmpty() && !second.bpm);
        QCOMPARE(fixture.invocations(), 1);
        struct Row { std::uint64_t frame; std::uint32_t first, end; double bpm; };
        for (const auto row : {Row{84399, 70000, 84400, 200}, Row{12000, 12000, 36000, 120},
                               Row{36000, 36000, 54000, 160}, Row{54000, 54000, 70000, 180},
                               Row{35999, 12000, 36000, 120}, Row{70000, 70000, 84400, 200}}) {
            const auto original = library::localTempoAt(first, row.frame);
            const auto cached = library::localTempoAt(second, row.frame);
            QVERIFY(original && cached);
            QCOMPARE(original->firstFrame, row.first); QCOMPARE(cached->firstFrame, row.first);
            QCOMPARE(original->endFrame, row.end); QCOMPARE(cached->endFrame, row.end);
            QCOMPARE(original->bpm, row.bpm); QCOMPARE(cached->bpm, row.bpm);
        }
        for (const auto frame : {0U, 11999U, 84400U, 143999U, 144000U}) {
            QVERIFY(!library::localTempoAt(first, frame));
            QVERIFY(!library::localTempoAt(second, frame));
        }
        QCOMPARE(fixture.invocations(), 1);
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

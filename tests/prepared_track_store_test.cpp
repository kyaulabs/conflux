// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/PreparedTrackStore.hpp"
#include "StemExportFixture.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <future>
#include <iostream>
#include <limits>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using conflux::audio::PreparedAudio;
using conflux::library::PreparedTrack;
using conflux::library::PreparedTrackStore;
using conflux::library::PreparedStemTrack;
using conflux::library::StemEngine;

template<class Function>
void checked(Function&& function)
{
    try {
        function();
    } catch (const std::exception& error) {
        QFAIL(error.what());
    }
}

template<class Exception, class Function>
bool rejects(Function&& function, const char* reason)
{
    try {
        function();
    } catch (const std::exception& error) {
        if (dynamic_cast<const Exception*>(&error))
            return std::string(error.what()).find(reason) != std::string::npos;
        std::cerr << "Unexpected rejection: " << error.what() << '\n';
    }
    return false;
}

template<class Function>
bool boundedChild(Function&& function)
{
    const auto child = fork();
    if (child < 0)
        throw std::runtime_error("Cannot fork bounded store check");
    if (!child) {
        try { _exit(function() ? 0 : 1); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; _exit(2); }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status = 0;
    pid_t result = 0;
    while ((result = waitpid(child, &status, WNOHANG)) == 0 || (result < 0 && errno == EINTR)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void append16(QByteArray& bytes, std::uint16_t value)
{
    bytes.append(char(value & 255));
    bytes.append(char(value >> 8));
}

void append32(QByteArray& bytes, std::uint32_t value)
{
    append16(bytes, std::uint16_t(value));
    append16(bytes, std::uint16_t(value >> 16));
}

void writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() || !file.flush())
        throw std::runtime_error("Cannot write independent store fixture");
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read store fixture");
    return file.readAll();
}

std::vector<float> samples(float marker = 1.5F)
{
    std::vector<float> result(1025 * 2);
    for (std::size_t frame = 0; frame < result.size() / 2; ++frame) {
        result[frame * 2] = float(int(frame % 17) - 8) / 32;
        result[frame * 2 + 1] = float(int(frame % 11) - 5) / 64;
    }
    result[0] = marker;
    result[1] = -0.0F;
    result[result.size() - 2] = -.75F;
    return result;
}

void wave(const QString& path, std::span<const float> pcm, std::uint32_t rate = 48000)
{
    QByteArray bytes;
    bytes.append("RIFF", 4); append32(bytes, std::uint32_t(36 + pcm.size_bytes()));
    bytes.append("WAVEfmt ", 8); append32(bytes, 16);
    append16(bytes, 3); append16(bytes, 2); append32(bytes, rate);
    append32(bytes, rate * 8); append16(bytes, 8); append16(bytes, 32);
    bytes.append("data", 4); append32(bytes, std::uint32_t(pcm.size_bytes()));
    for (const auto sample : pcm) append32(bytes, std::bit_cast<std::uint32_t>(sample));
    writeBytes(path, bytes);
}

bool identicalPcm(const PreparedAudio& audio, std::span<const float> expected)
{
    if (audio.frames() != expected.size() / 2)
        return false;
    std::vector<float> actual(expected.size());
    std::size_t received = 0;
    while (received < expected.size_bytes()) {
        const auto count = pread(audio.descriptor(), reinterpret_cast<char*>(actual.data()) + received,
                                 expected.size_bytes() - received, static_cast<off_t>(received));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        received += std::size_t(count);
    }
    for (std::size_t index = 0; index < actual.size(); ++index)
        if (std::bit_cast<std::uint32_t>(actual[index]) != std::bit_cast<std::uint32_t>(expected[index]))
            return false;
    return true;
}

std::vector<float> overview(std::span<const float> pcm)
{
    const auto frames = pcm.size() / 2;
    const auto count = std::min<std::size_t>(512, frames);
    std::vector<float> result(count);
    float maximum = 0;
    // Reduce independently delimited bucket intervals, rather than assigning
    // each sample with the production frame-to-bucket expression.
    for (std::size_t bin = 0; bin < count; ++bin) {
        const auto begin = (bin * frames + count - 1) / count;
        const auto end = ((bin + 1) * frames + count - 1) / count;
        for (auto frame = begin; frame < end; ++frame)
            result[bin] = std::max({result[bin], std::abs(pcm[frame * 2]), std::abs(pcm[frame * 2 + 1])});
        maximum = std::max(maximum, result[bin]);
    }
    if (maximum > 0)
        for (auto& value : result) value /= maximum;
    return result;
}

bool overviewMatches(const PreparedAudio& audio, std::span<const float> pcm)
{
    const auto expected = overview(pcm);
    return std::ranges::equal(audio.peaks(), expected);
}

QString entryPath(const QString& root, const PreparedTrack& track, const QString& file)
{
    return QDir(root).filePath(track.artifactKey + "/" + file);
}

QStringList entries(const QString& root)
{
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
}

// Real subprocess fixture: independent raw F32LE writer and manifest producer.
// It performs no inference, audio output or imports from the product worker.
StemEngine stemEngine(const QString& directory, const QByteArray& mode = "good")
{
    return conflux::test::makeStemEngineFixture(directory, mode);
}

std::vector<float> stemSamples()
{
    std::vector<float> result(11025 * 2);
    for (std::size_t frame = 0; frame < result.size() / 2; ++frame) {
        result[frame * 2] = 1.5F;
        result[frame * 2 + 1] = -.75F;
    }
    return result;
}

QString bankPath(const QString& root, const PreparedStemTrack& track, const QString& file)
{
    return QDir(root).filePath(track.artifactKey + "/" + file);
}

bool bankSamplesMatch(const PreparedStemTrack& track)
{
    const auto frames = track.stems[0].frames();
    if (frames < 11999 || frames > 12001) return false;
    for (std::size_t stem = 0; stem < track.stems.size(); ++stem) {
        const auto& audio = track.stems[stem];
        if (audio.frames() != frames || audio.peaks().size() != 512) return false;
        const auto flags = fcntl(audio.descriptor(), F_GETFL);
        if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || !(fcntl(audio.descriptor(), F_GETFD) & FD_CLOEXEC)) return false;
        std::vector<float> pcm(frames * 2);
        std::size_t received = 0;
        while (received < pcm.size() * sizeof(float)) {
            const auto count = pread(audio.descriptor(), reinterpret_cast<char*>(pcm.data()) + received,
                pcm.size() * sizeof(float) - received, static_cast<off_t>(received));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            received += std::size_t(count);
        }
        if (!overviewMatches(audio, pcm)) return false;
        for (std::size_t frame = 1536; frame + 1536 < frames; ++frame)
            if (std::abs(pcm[frame * 2] - 1.5F * float(stem + 1) / 4) > 2e-4F ||
                std::abs(pcm[frame * 2 + 1] + .75F * float(stem + 1) / 4) > 2e-4F) return false;
    }
    return true;
}

class SourceReadWatch {
public:
    explicit SourceReadWatch(const QString& path) : descriptor_(inotify_init1(IN_NONBLOCK | IN_CLOEXEC))
    {
        if (descriptor_ < 0) throw std::runtime_error("Cannot observe source reads");
        if (inotify_add_watch(descriptor_, QFile::encodeName(path).constData(), IN_ACCESS) < 0) {
            close(descriptor_);
            throw std::runtime_error("Cannot watch source fixture");
        }
    }
    ~SourceReadWatch() { close(descriptor_); }
    bool wait()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd source{descriptor_, POLLIN, 0};
            const auto ready = poll(&source, 1, 100);
            if (ready > 0) return source.revents & POLLIN;
            if (ready < 0 && errno != EINTR) return false;
        }
        return false;
    }
private:
    int descriptor_;
};
} // namespace

class PreparedTrackStoreTest : public QObject {
    Q_OBJECT
private slots:
    void stemLookupMissDoesNotPrepareOrCreateRoot()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path());
            const auto marker = engine.exporterPath.left(engine.exporterPath.size() - 3);
            QVERIFY(!PreparedTrackStore(root).findPreparedStemBank(source, engine).has_value());
            QVERIFY(!QFileInfo::exists(root));
            QVERIFY(!QFileInfo::exists(marker + ".calls"));
            QVERIFY(!QFileInfo::exists(marker + ".started"));

            // An existing empty root and an unrelated normal artifact are
            // genuine keyed misses; discovery must not create a bank or stage.
            QVERIFY(QDir().mkpath(root));
            QVERIFY(!PreparedTrackStore(root).findPreparedStemBank(source, engine).has_value());
            QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
            auto normal = PreparedTrackStore(root).prepare(source);
            const auto manifest = readBytes(entryPath(root, normal, "manifest.json"));
            const auto pcm = readBytes(entryPath(root, normal, "audio.f32"));
            const auto names = QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            QVERIFY(!PreparedTrackStore(root).findPreparedStemBank(source, engine).has_value());
            PreparedTrackStore::Limits belowUsage;
            belowUsage.maxCacheBytes = std::uint64_t(pcm.size());
            QVERIFY(rejects<std::length_error>([&] {
                PreparedTrackStore(root, belowUsage).findPreparedStemBank(source, engine); }, "budget"));
            QCOMPARE(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), names);
            QCOMPARE(readBytes(entryPath(root, normal, "manifest.json")), manifest);
            QCOMPARE(readBytes(entryPath(root, normal, "audio.f32")), pcm);
            QVERIFY(!QFileInfo::exists(marker + ".calls"));
            QVERIFY(!QFileInfo::exists(marker + ".started"));
        });
    }

    void stemLookupFreshInstanceReturnsExactBankAtExistingBudget()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path());
            auto seeded = PreparedTrackStore(root).prepareStemBank(source, engine);
            const auto marker = engine.exporterPath.left(engine.exporterPath.size() - 3);
            const auto calls = readBytes(marker + ".calls");
            QVERIFY(QFile::remove(marker + ".started"));
            const auto names = QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            PreparedTrackStore::Limits exact;
            exact.maxEntries = 1;
            exact.maxPreparedBytes = std::size_t(seeded.stems[0].frames() * 8);
            exact.maxManifestBytes = std::size_t(readBytes(bankPath(root, seeded, "manifest.json")).size());
            exact.maxSourceBytes = std::uint64_t(QFileInfo(source).size());
            exact.maxCacheBytes = 0;
            for (const auto& file : QDir(QDir(root).filePath(seeded.artifactKey)).entryInfoList(QDir::Files))
                exact.maxCacheBytes += std::uint64_t(file.size());

            // Same bytes at another path retain content identity, and a hit
            // needs no native/staging headroom beyond the existing files.
            const auto alias = directory.filePath("same content # %.wav");
            writeBytes(alias, readBytes(source));
            auto found = PreparedTrackStore(root, exact).findPreparedStemBank(alias, engine);
            QVERIFY(found.has_value());
            QVERIFY(found->cacheHit);
            QVERIFY(bankSamplesMatch(*found));
            QCOMPARE(found->sourceSha256, seeded.sourceSha256);
            QCOMPARE(found->sourceSha256,
                QString::fromLatin1(QCryptographicHash::hash(readBytes(source), QCryptographicHash::Sha256).toHex()));
            QCOMPARE(found->artifactKey, seeded.artifactKey);
            for (std::size_t role = 0; role < 4; ++role) {
                const auto raw = readBytes(bankPath(root, seeded,
                    QString::fromLatin1(std::array{"drums.f32", "bass.f32", "other.f32", "vocals.f32"}[role])));
                std::vector<float> expected(std::size_t(seeded.stems[role].frames() * 2));
                QCOMPARE(raw.size(), qsizetype(expected.size() * sizeof(float)));
                std::copy_n(reinterpret_cast<const char*>(raw.constData()), raw.size(),
                    reinterpret_cast<char*>(expected.data()));
                QVERIFY(identicalPcm(found->stems[role], expected));
                QVERIFY(std::ranges::equal(found->stems[role].peaks(), seeded.stems[role].peaks()));
            }
            QCOMPARE(readBytes(marker + ".calls"), calls);
            QVERIFY(!QFileInfo::exists(marker + ".started"));
            QCOMPARE(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), names);

            // A later, valid content change selects a different absent key;
            // it neither adopts nor repairs the prior bank.
            auto changed = stemSamples();
            changed[0] = .125F;
            wave(alias, changed, 44100);
            QVERIFY(!PreparedTrackStore(root).findPreparedStemBank(alias, engine).has_value());
            QCOMPARE(readBytes(marker + ".calls"), calls);
            QVERIFY(!QFileInfo::exists(marker + ".started"));
            QVERIFY(bankSamplesMatch(seeded));
            QCOMPARE(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), names);
        });
    }

    void stemLookupPresentFailurePreservesEntry_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto* fault : {"manifest", "digest", "incomplete", "symlink", "fifo",
                                 "prepared", "manifest-budget", "cache-budget", "entry-budget", "orphan-stage"})
            QTest::newRow(fault) << QString::fromLatin1(fault);
    }

    void stemLookupPresentFailurePreservesEntry()
    {
        QFETCH(QString, fault);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path());
            auto seeded = PreparedTrackStore(root).prepareStemBank(source, engine);
            const auto marker = engine.exporterPath.left(engine.exporterPath.size() - 3);
            const auto calls = readBytes(marker + ".calls");
            QVERIFY(QFile::remove(marker + ".started"));
            PreparedTrackStore::Limits limits;
            const auto outside = directory.filePath("outside.bin");
            const QByteArray sentinel("outside must remain unchanged");
            writeBytes(outside, sentinel);
            const auto manifestPath = bankPath(root, seeded, "manifest.json");
            const auto bassPath = bankPath(root, seeded, "bass.f32");
            const auto stage = QDir(root).filePath(".stage-0123456789abcdef");
            if (fault == "manifest") writeBytes(manifestPath, "{}");
            else if (fault == "digest") {
                auto bytes = readBytes(bassPath);
                bytes[0] = char(static_cast<unsigned char>(bytes[0]) ^ 1U);
                writeBytes(bassPath, bytes);
            } else if (fault == "incomplete") QVERIFY(QFile::remove(bankPath(root, seeded, "vocals.f32")));
            else if (fault == "symlink" || fault == "fifo") {
                QVERIFY(QFile::remove(bassPath));
                if (fault == "symlink")
                    QCOMPARE(symlink(QFile::encodeName(outside).constData(), QFile::encodeName(bassPath).constData()), 0);
                else
                    QCOMPARE(mkfifo(QFile::encodeName(bassPath).constData(), 0600), 0);
            } else if (fault == "prepared") limits.maxPreparedBytes = std::size_t(seeded.stems[0].frames() * 8 - 8);
            else if (fault == "manifest-budget") limits.maxManifestBytes = std::size_t(QFileInfo(manifestPath).size() - 1);
            else if (fault == "cache-budget") {
                limits.maxCacheBytes = 0;
                for (const auto& file : QDir(QDir(root).filePath(seeded.artifactKey)).entryInfoList(QDir::Files))
                    limits.maxCacheBytes += std::uint64_t(file.size());
                --limits.maxCacheBytes;
            } else if (fault == "entry-budget") {
                auto normal = PreparedTrackStore(root).prepare(source);
                QVERIFY(normal.artifactKey != seeded.artifactKey);
                limits.maxEntries = 1;
            } else if (fault == "orphan-stage") {
                // This recognized regular leaf is removable by ordinary
                // preparation's GC; lookup must leave the entire stage intact.
                QVERIFY(QDir().mkpath(stage));
                writeBytes(QDir(stage).filePath("audio.f32"), sentinel);
            }

            const std::array leaves{"drums.f32", "bass.f32", "other.f32", "vocals.f32", "manifest.json"};
            std::array<QByteArray, 5> before;
            std::array<mode_t, 5> kinds{};
            for (std::size_t i = 0; i < leaves.size(); ++i) {
                struct stat status{};
                const auto path = bankPath(root, seeded, QString::fromLatin1(leaves[i]));
                if (lstat(QFile::encodeName(path).constData(), &status) == 0) {
                    kinds[i] = status.st_mode & S_IFMT;
                    if (S_ISREG(status.st_mode)) before[i] = readBytes(path);
                } else QCOMPARE(errno, ENOENT);
            }
            const auto names = QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            auto rejected = [&] {
                auto lookup = [&] { PreparedTrackStore(root, limits).findPreparedStemBank(source, engine); };
                if (fault == "prepared" || fault.endsWith("-budget"))
                    return rejects<std::length_error>(lookup, fault == "entry-budget" ? "entry limit" : "budget");
                if (fault == "symlink" || fault == "fifo")
                    return rejects<std::runtime_error>(lookup, "unsafe cache entry");
                if (fault == "orphan-stage") return rejects<std::runtime_error>(lookup, "staging");
                return rejects<std::invalid_argument>(lookup,
                    fault == "manifest" ? "identity" : fault == "digest" ? "digest" : "incomplete");
            };
            if (fault == "fifo") QVERIFY(boundedChild(rejected));
            else QVERIFY(rejected());
            QCOMPARE(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), names);
            for (std::size_t i = 0; i < leaves.size(); ++i) {
                struct stat status{};
                const auto path = bankPath(root, seeded, QString::fromLatin1(leaves[i]));
                if (!kinds[i]) {
                    QCOMPARE(lstat(QFile::encodeName(path).constData(), &status), -1);
                    QCOMPARE(errno, ENOENT);
                } else {
                    QCOMPARE(lstat(QFile::encodeName(path).constData(), &status), 0);
                    QCOMPARE(status.st_mode & S_IFMT, kinds[i]);
                    if (S_ISREG(status.st_mode)) QCOMPARE(readBytes(path), before[i]);
                }
            }
            if (fault == "orphan-stage") QCOMPARE(readBytes(QDir(stage).filePath("audio.f32")), sentinel);
            QCOMPARE(readBytes(outside), sentinel);
            QCOMPARE(readBytes(marker + ".calls"), calls);
            QVERIFY(!QFileInfo::exists(marker + ".started"));
        });
    }

    void stemLookupInvalidInputsCancellationAndBusyNeverExport_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto* fault : {"missing-runtime", "fifo-runtime", "fifo-source", "fifo-root", "cancelled", "busy"})
            QTest::newRow(fault) << QString::fromLatin1(fault);
    }

    void stemLookupInvalidInputsCancellationAndBusyNeverExport()
    {
        QFETCH(QString, fault);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path());
            const auto marker = engine.exporterPath.left(engine.exporterPath.size() - 3);
            std::stop_source cancellation;
            std::unique_ptr<QLockFile> lock;
            if (fault == "missing-runtime" || fault == "fifo-runtime") {
                QVERIFY(QFile::remove(engine.runtimePath));
                if (fault == "fifo-runtime") QCOMPARE(mkfifo(QFile::encodeName(engine.runtimePath).constData(), 0600), 0);
            } else if (fault == "fifo-source") {
                QVERIFY(QFile::remove(source));
                QCOMPARE(mkfifo(QFile::encodeName(source).constData(), 0600), 0);
            } else if (fault == "fifo-root") {
                QCOMPARE(mkfifo(QFile::encodeName(root).constData(), 0600), 0);
            } else if (fault == "cancelled") cancellation.request_stop();
            else {
                QVERIFY(QDir().mkpath(root));
                lock = std::make_unique<QLockFile>(QDir(root).filePath(".lock"));
                QVERIFY(lock->tryLock(0));
            }
            auto rejected = [&] {
                auto lookup = [&] { PreparedTrackStore(root).findPreparedStemBank(source, engine, cancellation.get_token()); };
                if (fault == "missing-runtime") return rejects<std::runtime_error>(lookup, "engine file");
                if (fault == "fifo-runtime") return rejects<std::invalid_argument>(lookup, "regular files");
                if (fault == "fifo-source") return rejects<std::invalid_argument>(lookup, "regular local file");
                if (fault == "fifo-root") return rejects<std::runtime_error>(lookup, "cache root");
                return rejects<std::runtime_error>(lookup, fault == "cancelled" ? "cancel" : "busy");
            };
            if (fault.startsWith("fifo-")) QVERIFY(boundedChild(rejected));
            else QVERIFY(rejected());
            if (fault != "busy" && fault != "fifo-root") QVERIFY(!QFileInfo::exists(marker + ".prefix"));
            QVERIFY(!QFileInfo::exists(marker + ".calls"));
            QVERIFY(!QFileInfo::exists(marker + ".started"));
            if (fault == "busy") {
                lock->unlock();
                QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
            } else if (fault == "fifo-root") {
                struct stat status{};
                QCOMPARE(lstat(QFile::encodeName(root).constData(), &status), 0);
                QVERIFY(S_ISFIFO(status.st_mode));
            } else QVERIFY(!QFileInfo::exists(root));
        });
    }

    void completeStemBankAndFreshInstanceReuse()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path());
            auto first = PreparedTrackStore(root).prepareStemBank(source, engine);
            QVERIFY(!first.cacheHit);
            QVERIFY(bankSamplesMatch(first));
            QCOMPARE(first.sourceSha256, QString::fromLatin1(QCryptographicHash::hash(readBytes(source), QCryptographicHash::Sha256).toHex()));
            QCOMPARE(readBytes(engine.exporterPath.left(engine.exporterPath.size() - 3) + ".calls"), QByteArray("x"));
            QCOMPARE(QDir(QDir(root).filePath(first.artifactKey)).entryList(QDir::Files).size(), qsizetype(5));
            const auto manifest = QJsonDocument::fromJson(readBytes(bankPath(root, first, "manifest.json"))).object();
            QCOMPARE(manifest.value("schema"), QJsonValue(2));
            QCOMPARE(manifest.value("kind").toString(), QString("stem_bank"));
            QVERIFY(readBytes(bankPath(root, first, "manifest.json")).size() <= qsizetype(PreparedTrackStore::Limits{}.maxManifestBytes));
            auto second = PreparedTrackStore(root).prepareStemBank(source, engine);
            QVERIFY(second.cacheHit);
            QCOMPARE(second.artifactKey, first.artifactKey);
            QVERIFY(bankSamplesMatch(second));
            QCOMPARE(readBytes(engine.exporterPath.left(engine.exporterPath.size() - 3) + ".calls"), QByteArray("x"));
            PreparedTrackStore::Limits tightSource;
            tightSource.maxSourceBytes = std::uint64_t(QFileInfo(source).size());
            // The interpreter/model bounds are independent of original media.
            QVERIFY(PreparedTrackStore(root, tightSource).prepareStemBank(source, engine).cacheHit);
            auto alias = engine;
            alias.pythonExecutable = directory.filePath("python-alias");
            QCOMPARE(symlink(QFile::encodeName(engine.pythonExecutable).constData(), QFile::encodeName(alias.pythonExecutable).constData()), 0);
            QVERIFY(PreparedTrackStore(root).prepareStemBank(source, alias).cacheHit);
            auto normal = PreparedTrackStore(root).prepare(source);
            QVERIFY(!normal.cacheHit);
            QVERIFY(normal.artifactKey != first.artifactKey);
            QVERIFY(PreparedTrackStore(root).prepare(source).cacheHit);
            QCOMPARE(entries(root).size(), qsizetype(2));
        });
    }

    void corruptStemBankRegeneratesAsOneUnit_data()
    {
        QTest::addColumn<bool>("geometry");
        QTest::newRow("same-size-digest") << false;
        QTest::newRow("self-consistent-wrong-duration") << true;
    }

    void corruptStemBankRegeneratesAsOneUnit()
    {
        QFETCH(bool, geometry);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path());
            auto first = PreparedTrackStore(root).prepareStemBank(source, engine);
            if (geometry) {
                const auto path = bankPath(root, first, "manifest.json");
                auto manifest = QJsonDocument::fromJson(readBytes(path)).object();
                auto stems = manifest.value("stems").toArray();
                for (qsizetype index = 0; index < stems.size(); ++index) {
                    auto stem = stems[index].toObject();
                    const auto leaf = bankPath(root, first, stem.value("file").toString());
                    QVERIFY(QFile::resize(leaf, 512 * 8));
                    stem.insert("bytes", 512 * 8);
                    stem.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(readBytes(leaf), QCryptographicHash::Sha256).toHex()));
                    stems[index] = stem;
                }
                manifest.insert("stems", stems);
                manifest.insert("frames", 512);
                manifest.insert("pcm_bytes", 512 * 8 * 4);
                writeBytes(path, QJsonDocument(manifest).toJson(QJsonDocument::Compact));
            } else {
                QFile damaged(bankPath(root, first, "other.f32"));
                QVERIFY(damaged.open(QIODevice::ReadWrite));
                QByteArray value; append32(value, std::bit_cast<std::uint32_t>(.0625F));
                QCOMPARE(damaged.write(value), qint64(value.size()));
                damaged.close();
            }
            auto regenerated = PreparedTrackStore(root).prepareStemBank(source, engine);
            QVERIFY(!regenerated.cacheHit);
            QCOMPARE(regenerated.artifactKey, first.artifactKey);
            QVERIFY(bankSamplesMatch(regenerated));
            QVERIFY(PreparedTrackStore(root).prepareStemBank(source, engine).cacheHit);
            QCOMPARE(entries(root).size(), qsizetype(1));
            QCOMPARE(readBytes(engine.exporterPath.left(engine.exporterPath.size() - 3) + ".calls"), QByteArray("xx"));
        });
    }

    void invalidStemExportDoesNotPublish_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto* mode : {"truncate", "nonfinite", "partial", "wrong-order", "wrong-model", "wrong-describe", "changed-source"})
            QTest::newRow(mode) << QByteArray(mode);
    }

    void invalidStemExportDoesNotPublish()
    {
        QFETCH(QByteArray, mode);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path(), mode);
            QVERIFY(rejects<std::exception>([&] { PreparedTrackStore(root).prepareStemBank(source, engine); }, ""));
            QVERIFY(entries(root).isEmpty());
            QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        });
    }

    void stemBankSharesAggregateBudgetAndPreservesNormalEntry()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path());
            auto original = PreparedTrackStore(root).prepare(source);
            PreparedTrackStore::Limits limits;
            limits.maxCacheBytes = 1000000; // Normal PCM fits; native export reservation does not.
            QVERIFY(rejects<std::length_error>([&] { PreparedTrackStore(root, limits).prepareStemBank(source, engine); }, "budget"));
            QVERIFY(!QFileInfo::exists(engine.exporterPath.left(engine.exporterPath.size() - 3) + ".calls"));
            QVERIFY(PreparedTrackStore(root, limits).prepare(source).cacheHit);
            QCOMPARE(entries(root).size(), qsizetype(1));
            QVERIFY(QFileInfo::exists(entryPath(root, original, "audio.f32")));
        });
    }

    void cancelledStemExportReapsItsProcessAndKeepsPriorBank()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto good = stemEngine(directory.path());
            auto prior = PreparedTrackStore(root).prepareStemBank(source, good);
            const auto engine = stemEngine(directory.path(), "block");
            const auto marker = engine.exporterPath.left(engine.exporterPath.size() - 3) + ".started";
            QFile::remove(marker);
            std::stop_source cancellation;
            auto preparing = std::async(std::launch::async, [&] {
                return rejects<std::runtime_error>([&] { PreparedTrackStore(root).prepareStemBank(source, engine, cancellation.get_token()); }, "cancel");
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!QFileInfo::exists(marker) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const bool started = QFileInfo::exists(marker);
            cancellation.request_stop();
            const bool finished = preparing.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
            QVERIFY(started && finished);
            QVERIFY(preparing.get());
            const auto pid = readBytes(marker).toLongLong();
            QVERIFY(pid > 1 && kill(pid_t(pid), 0) == -1 && errno == ESRCH);
            QVERIFY(bankSamplesMatch(prior));
            QCOMPARE(entries(root).size(), qsizetype(1));
            QVERIFY(QFileInfo::exists(bankPath(root, prior, "vocals.f32")));
        });
    }

    void unsafeStemBankLeafIsPreservedAndNeverFollowed()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path());
            auto bank = PreparedTrackStore(root).prepareStemBank(source, engine);
            const auto leaf = bankPath(root, bank, "bass.f32"), outside = directory.filePath("outside.bin");
            writeBytes(outside, "do not read or overwrite");
            QVERIFY(QFile::remove(leaf));
            QCOMPARE(symlink(QFile::encodeName(outside).constData(), QFile::encodeName(leaf).constData()), 0);
            QVERIFY(rejects<std::runtime_error>([&] { PreparedTrackStore(root).prepareStemBank(source, engine); }, "unsafe cache entry"));
            QCOMPARE(readBytes(outside), QByteArray("do not read or overwrite"));
            struct stat status{};
            QCOMPARE(lstat(QFile::encodeName(leaf).constData(), &status), 0);
            QVERIFY(S_ISLNK(status.st_mode));
        });
    }

    void stemExporterDeadlineAndDiagnosticBounds_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("deadline") << QByteArray("block");
        QTest::newRow("diagnostic-overflow") << QByteArray("flood");
    }

    void stemExporterDeadlineAndDiagnosticBounds()
    {
        QFETCH(QByteArray, mode);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            auto engine = stemEngine(directory.path(), mode);
            engine.maxSeconds = 1;
            QVERIFY(rejects<std::exception>([&] { PreparedTrackStore(root).prepareStemBank(source, engine); },
                mode == "block" ? "timed out" : "diagnostic"));
            const auto pid = readBytes(engine.exporterPath.left(engine.exporterPath.size() - 3) + ".started").toLongLong();
            QVERIFY(pid > 1 && kill(pid_t(pid), 0) == -1 && errno == ESRCH);
            QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        });
    }

    void concurrentStemPublishAndEngineIdentityChange()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            const auto engine = stemEngine(directory.path());
            auto left = std::async(std::launch::async, [&] { return PreparedTrackStore(root).prepareStemBank(source, engine); });
            auto right = std::async(std::launch::async, [&] { return PreparedTrackStore(root).prepareStemBank(source, engine); });
            auto first = left.get();
            auto second = right.get();
            QVERIFY(first.cacheHit != second.cacheHit);
            QCOMPARE(first.artifactKey, second.artifactKey);
            QVERIFY(bankSamplesMatch(first) && bankSamplesMatch(second));
            QCOMPARE(readBytes(engine.exporterPath.left(engine.exporterPath.size() - 3) + ".calls"), QByteArray("x"));
            writeBytes(engine.runtimePath, "changed runtime bytes\n");
            auto changed = PreparedTrackStore(root).prepareStemBank(source, engine);
            QVERIFY(!changed.cacheHit);
            QVERIFY(changed.artifactKey != first.artifactKey);
            QVERIFY(bankSamplesMatch(changed) && bankSamplesMatch(first));
            QCOMPARE(entries(root).size(), qsizetype(2));
        });
    }

    void staleStemJobRequiresReleasedLockAndProvenNativeCleanup()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            const auto stage = QDir(root).filePath(".stage-0123456789abcdef");
            wave(source, samples());
            QVERIFY(QDir().mkpath(stage));
            const auto lockPath = QDir(stage).filePath(".raw.export.lock");
            writeBytes(QDir(stage).filePath("input.f32"), "retained caller input");
            writeBytes(lockPath, {});
            QFile lease(lockPath);
            QVERIFY(lease.open(QIODevice::ReadWrite));
            QCOMPARE(flock(lease.handle(), LOCK_EX | LOCK_NB), 0);
            QVERIFY(rejects<std::runtime_error>([&] { PreparedTrackStore(root).prepare(source); }, "busy"));
            QCOMPARE(readBytes(QDir(stage).filePath("input.f32")), QByteArray("retained caller input"));
            QCOMPARE(flock(lease.handle(), LOCK_UN), 0);
            lease.close();
            const auto native = QDir(stage).filePath("raw.stage/native");
            QVERIFY(QDir().mkpath(native));
            writeBytes(QDir(native).filePath("input.f32"), "unexplained native spool");
            QVERIFY(rejects<std::runtime_error>([&] { PreparedTrackStore(root).prepare(source); }, "unproven"));
            QCOMPARE(readBytes(QDir(stage).filePath("input.f32")), QByteArray("retained caller input"));
            QCOMPARE(readBytes(QDir(native).filePath("input.f32")), QByteArray("unexplained native spool"));
            // This fixture has no native process. Explicitly remove its spool;
            // the next normal preparation may now collect the bounded orphan.
            QVERIFY(QDir(native).removeRecursively());
            QVERIFY(!PreparedTrackStore(root).prepare(source).cacheHit);
            QVERIFY(!QFileInfo::exists(stage));
        });
    }

    void configuredPythonVirtualEnvironmentIsPreserved()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            wave(source, stemSamples(), 44100);
            auto engine = stemEngine(directory.path());
            const auto environment = directory.filePath("venv"), bin = QDir(environment).filePath("bin");
            QVERIFY(QDir().mkpath(bin));
            const auto invoked = QDir(bin).filePath("python3");
            QCOMPARE(symlink(QFile::encodeName(engine.pythonExecutable).constData(), QFile::encodeName(invoked).constData()), 0);
            writeBytes(QDir(environment).filePath("pyvenv.cfg"),
                "home = " + QFile::encodeName(QFileInfo(engine.pythonExecutable).absolutePath()) +
                "\ninclude-system-site-packages = false\n");
            engine.pythonExecutable = invoked;
            auto bank = PreparedTrackStore(root).prepareStemBank(source, engine);
            QVERIFY(bankSamplesMatch(bank));
            QCOMPARE(QString::fromUtf8(readBytes(engine.exporterPath.left(engine.exporterPath.size() - 3) + ".prefix")), environment);
        });
    }

    void coldAndFreshInstanceReuse()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto root = directory.filePath("cache");
            const auto pcm = samples();
            wave(source, pcm);
            PreparedTrackStore store(root);
            auto first = store.prepare(source);
            QVERIFY(!first.cacheHit);
            QCOMPARE(first.sourceSha256, QString::fromLatin1(QCryptographicHash::hash(readBytes(source), QCryptographicHash::Sha256).toHex()));
            QVERIFY(QRegularExpression("^[a-f0-9]{64}$").match(first.artifactKey).hasMatch());
            QVERIFY(identicalPcm(first.audio, pcm));
            QVERIFY(overviewMatches(first.audio, pcm));
            QCOMPARE(first.audio.peaks().size(), std::size_t(512));
            QVERIFY(QFileInfo::exists(entryPath(root, first, "audio.f32")));
            const auto manifest = readBytes(entryPath(root, first, "manifest.json"));
            QVERIFY(manifest.size() <= qsizetype(PreparedTrackStore::Limits{}.maxManifestBytes));
            QVERIFY(QJsonDocument::fromJson(manifest).isObject());
            PreparedTrackStore reopened(root);
            auto second = reopened.prepare(source);
            QVERIFY(second.cacheHit);
            QCOMPARE(second.sourceSha256, first.sourceSha256);
            QCOMPARE(second.artifactKey, first.artifactKey);
            QVERIFY(identicalPcm(second.audio, pcm));
            QVERIFY(overviewMatches(second.audio, pcm));
            QCOMPARE(entries(root).size(), qsizetype(1));
        });
    }

    void contentIdentityAndChangedSource()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("first.wav");
            const auto alias = directory.filePath("same-content.wav");
            const auto root = directory.filePath("cache");
            const auto original = samples();
            wave(source, original);
            QVERIFY(QFile::copy(source, alias));
            PreparedTrackStore store(root);
            auto first = store.prepare(source);
            auto second = store.prepare(alias);
            QVERIFY(second.cacheHit);
            QCOMPARE(second.sourceSha256, first.sourceSha256);
            QCOMPARE(second.artifactKey, first.artifactKey);
            const auto changed = samples(.625F);
            wave(source, changed);
            auto replacement = store.prepare(source);
            QVERIFY(!replacement.cacheHit);
            QVERIFY(replacement.sourceSha256 != first.sourceSha256);
            QVERIFY(replacement.artifactKey != first.artifactKey);
            QVERIFY(identicalPcm(replacement.audio, changed));
            QVERIFY(identicalPcm(first.audio, original));
            QCOMPARE(entries(root).size(), qsizetype(2));
            QVERIFY(QFile::remove(source));
            QVERIFY(rejects<std::runtime_error>([&] { store.prepare(source); }, "source"));
            QVERIFY(identicalPcm(first.audio, original));
        });
    }

    void corruptEntryIsRegenerated_data()
    {
        QTest::addColumn<QString>("corruption");
        QTest::newRow("malformed-manifest") << "manifest";
        QTest::newRow("finite-but-wrong-overview") << "overview";
        QTest::newRow("truncated-pcm") << "truncate";
        QTest::newRow("same-size-pcm-digest-mismatch") << "digest";
    }

    void corruptEntryIsRegenerated()
    {
        QFETCH(QString, corruption);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto root = directory.filePath("cache");
            const auto pcm = samples();
            wave(source, pcm);
            PreparedTrackStore store(root);
            auto first = store.prepare(source);
            if (corruption == "manifest") {
                writeBytes(entryPath(root, first, "manifest.json"), "{broken");
            } else if (corruption == "overview") {
                const auto path = entryPath(root, first, "manifest.json");
                auto object = QJsonDocument::fromJson(readBytes(path)).object();
                auto peaks = object.value("peaks").toArray();
                QVERIFY(!peaks.isEmpty());
                peaks[0] = .125;
                object.insert("peaks", peaks);
                writeBytes(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
            } else {
                QFile file(entryPath(root, first, "audio.f32"));
                QVERIFY(file.open(QIODevice::ReadWrite));
                if (corruption == "truncate") {
                    QVERIFY(file.resize(qint64(pcm.size() * sizeof(float) - 4)));
                } else {
                    QByteArray sample;
                    append32(sample, std::bit_cast<std::uint32_t>(-.5F));
                    QCOMPARE(file.write(sample), qint64(sample.size()));
                }
            }
            auto repaired = PreparedTrackStore(root).prepare(source);
            QVERIFY(!repaired.cacheHit);
            QCOMPARE(repaired.artifactKey, first.artifactKey);
            QVERIFY(identicalPcm(repaired.audio, pcm));
            QVERIFY(overviewMatches(repaired.audio, pcm));
            QVERIFY(PreparedTrackStore(root).prepare(source).cacheHit);
        });
    }

    void unsafeCacheFilesFailClosed_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<bool>("symbolicLink");
        QTest::newRow("pcm-symlink") << "audio.f32" << true;
        QTest::newRow("pcm-fifo") << "audio.f32" << false;
        QTest::newRow("manifest-symlink") << "manifest.json" << true;
        QTest::newRow("manifest-fifo") << "manifest.json" << false;
    }

    void unsafeCacheFilesFailClosed()
    {
        QFETCH(QString, fileName);
        QFETCH(bool, symbolicLink);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto root = directory.filePath("cache");
            wave(source, samples());
            PreparedTrackStore store(root);
            auto first = store.prepare(source);
            const auto target = entryPath(root, first, fileName);
            const auto outside = directory.filePath("untouched.bin");
            const QByteArray sentinel("outside file must remain unchanged");
            writeBytes(outside, sentinel);
            QVERIFY(QFile::remove(target));
            const auto encoded = QFile::encodeName(target);
            if (symbolicLink)
                QCOMPARE(symlink(QFile::encodeName(outside).constData(), encoded.constData()), 0);
            else
                QCOMPARE(mkfifo(encoded.constData(), 0600), 0);
            QVERIFY(boundedChild([&] {
                return rejects<std::runtime_error>([&] { PreparedTrackStore(root).prepare(source); }, "unsafe cache entry");
            }));
            QCOMPARE(readBytes(outside), sentinel);
            struct stat status{};
            QCOMPARE(lstat(encoded.constData(), &status), 0);
            QVERIFY(symbolicLink ? S_ISLNK(status.st_mode) : S_ISFIFO(status.st_mode));
        });
    }

    void cancelledDoesNotPublish()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto root = directory.filePath("cache");
            wave(source, samples());
            std::stop_source cancellation;
            cancellation.request_stop();
            PreparedTrackStore store(root);
            QVERIFY(rejects<std::runtime_error>([&] { store.prepare(source, cancellation.get_token()); }, "cancel"));
            QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
        });
    }

    void byteLimitsDoNotPublish_data()
    {
        QTest::addColumn<QString>("budget");
        QTest::newRow("source-bytes") << "source";
        QTest::newRow("normalized-pcm-bytes") << "prepared";
        QTest::newRow("total-cache-bytes") << "cache";
        QTest::newRow("manifest-bytes") << "manifest";
    }

    void byteLimitsDoNotPublish()
    {
        QFETCH(QString, budget);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto root = directory.filePath("cache");
            const auto pcm = samples();
            wave(source, pcm);
            PreparedTrackStore::Limits limits;
            if (budget == "source") limits.maxSourceBytes = std::uint64_t(QFileInfo(source).size() - 1);
            if (budget == "prepared") limits.maxPreparedBytes = pcm.size() * sizeof(float) - 1;
            if (budget == "cache") limits.maxCacheBytes = pcm.size() * sizeof(float) - 1;
            if (budget == "manifest") limits.maxManifestBytes = 32;
            PreparedTrackStore store(root, limits);
            const auto reason = budget.toStdString();
            QVERIFY(rejects<std::length_error>([&] { store.prepare(source); }, reason.c_str()));
            QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
        });
    }

    void entryLimitPreservesExistingTrack()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto other = directory.filePath("other.wav");
            const auto root = directory.filePath("cache");
            const auto pcm = samples();
            wave(source, pcm);
            wave(other, samples(.375F));
            PreparedTrackStore::Limits limits;
            limits.maxEntries = 1;
            PreparedTrackStore store(root, limits);
            auto first = store.prepare(source);
            QVERIFY(rejects<std::length_error>([&] { store.prepare(other); }, "entry limit"));
            auto retained = store.prepare(source);
            QVERIFY(retained.cacheHit);
            QCOMPARE(retained.artifactKey, first.artifactKey);
            QVERIFY(identicalPcm(retained.audio, pcm));
            QCOMPARE(entries(root).size(), qsizetype(1));
        });
    }

    void tighterLimitsPreserveValidEntry_data()
    {
        QTest::addColumn<QString>("budget");
        QTest::newRow("prepared-pcm-limit") << "prepared";
        QTest::newRow("manifest-limit") << "manifest";
    }

    void tighterLimitsPreserveValidEntry()
    {
        QFETCH(QString, budget);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto root = directory.filePath("cache");
            const auto pcm = samples();
            wave(source, pcm);
            auto original = PreparedTrackStore(root).prepare(source);
            const auto pcmPath = entryPath(root, original, "audio.f32");
            const auto manifestPath = entryPath(root, original, "manifest.json");
            const auto pcmBytes = readBytes(pcmPath);
            const auto manifestBytes = readBytes(manifestPath);
            PreparedTrackStore::Limits limits;
            if (budget == "prepared") limits.maxPreparedBytes = pcm.size() * sizeof(float) - 8;
            else limits.maxManifestBytes = std::size_t(manifestBytes.size() - 1);
            const auto reason = budget.toStdString();
            QVERIFY(rejects<std::length_error>([&] { PreparedTrackStore(root, limits).prepare(source); }, reason.c_str()));
            QCOMPARE(readBytes(pcmPath), pcmBytes);
            QCOMPARE(readBytes(manifestPath), manifestBytes);
            auto retained = PreparedTrackStore(root).prepare(source);
            QVERIFY(retained.cacheHit);
            QCOMPARE(retained.artifactKey, original.artifactKey);
            QVERIFY(identicalPcm(retained.audio, pcm));
        });
    }

    void cancellationAfterSourceReadIsBounded()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto root = directory.filePath("cache");
            wave(source, samples());
            QVERIFY(QDir().mkpath(root));
            QLockFile lock(QDir(root).filePath(".lock"));
            QVERIFY(lock.tryLock(0));
            QVERIFY(boundedChild([&] {
                SourceReadWatch read(source);
                std::stop_source cancellation;
                auto preparing = std::async(std::launch::async, [&] {
                    return rejects<std::runtime_error>([&] {
                        PreparedTrackStore(root).prepare(source, cancellation.get_token());
                    }, "cancel");
                });
                if (!read.wait() || preparing.wait_for(std::chrono::milliseconds(0)) != std::future_status::timeout)
                    return false;
                cancellation.request_stop();
                return preparing.wait_for(std::chrono::seconds(1)) == std::future_status::ready && preparing.get();
            }));
            QVERIFY(entries(root).isEmpty());
            lock.unlock();
            QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
        });
    }

    void sourceChangedAfterReadDoesNotPublish()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto root = directory.filePath("cache");
            wave(source, samples());
            QVERIFY(QDir().mkpath(root));
            QVERIFY(boundedChild([&] {
                QLockFile lock(QDir(root).filePath(".lock"));
                if (!lock.tryLock(0)) return false;
                SourceReadWatch read(source);
                auto preparing = std::async(std::launch::async, [&] {
                    return rejects<std::runtime_error>([&] { PreparedTrackStore(root).prepare(source); }, "source changed");
                });
                if (!read.wait() || preparing.wait_for(std::chrono::milliseconds(0)) != std::future_status::timeout)
                    return false;
                QFile changed(source);
                QByteArray marker;
                append32(marker, std::bit_cast<std::uint32_t>(-.875F));
                if (!changed.open(QIODevice::ReadWrite) || !changed.seek(44) || changed.write(marker) != marker.size() ||
                    !changed.flush()) return false;
                changed.close();
                lock.unlock();
                return preparing.wait_for(std::chrono::seconds(1)) == std::future_status::ready && preparing.get() && entries(root).isEmpty();
            }));
            QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
        });
    }

    void concurrentPublishHasOneCompleteEntry()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto root = directory.filePath("cache");
            const auto pcm = samples();
            wave(source, pcm);
            QVERIFY(boundedChild([&] {
                auto first = std::async(std::launch::async, [&] { return PreparedTrackStore(root).prepare(source); });
                auto second = std::async(std::launch::async, [&] { return PreparedTrackStore(root).prepare(source); });
                auto a = first.get();
                auto b = second.get();
                return a.cacheHit != b.cacheHit && a.sourceSha256 == b.sourceSha256 && a.artifactKey == b.artifactKey &&
                    identicalPcm(a.audio, pcm) && identicalPcm(b.audio, pcm) && overviewMatches(a.audio, pcm) &&
                    overviewMatches(b.audio, pcm) && entries(root).size() == 1 &&
                    QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).size() == 1;
            }));
        });
    }

    void descriptorPreparationUsesOpenedSource()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto source = directory.filePath("source.wav");
            const auto pcm = samples();
            wave(source, pcm);
            const auto encoded = QFile::encodeName(source);
            const int descriptor = open(encoded.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            QVERIFY(descriptor >= 0);
            const bool passed = boundedChild([&] {
                if (!QFile::rename(source, source + ".old") || mkfifo(encoded.constData(), 0600) != 0 ||
                    lseek(descriptor, 13, SEEK_SET) != 13)
                    return false;
                {
                    auto prepared = conflux::audio::prepareAudioFile(descriptor);
                    if (!identicalPcm(prepared, pcm) || !overviewMatches(prepared, pcm))
                        return false;
                }
                std::array<char, 4> header{};
                return fcntl(descriptor, F_GETFD) >= 0 && pread(descriptor, header.data(), header.size(), 0) == 4 &&
                    std::string_view(header.data(), header.size()) == "RIFF";
            });
            close(descriptor);
            QVERIFY(passed);
        });
    }

    void reopenedDescriptorAndMetadataAreValidated()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("audio.f32");
            const auto pcm = samples();
            QByteArray bytes;
            for (const auto sample : pcm) append32(bytes, std::bit_cast<std::uint32_t>(sample));
            writeBytes(path, bytes);
            const auto peaks = overview(pcm);
            const auto encoded = QFile::encodeName(path);
            QVERIFY(boundedChild([&] {
                QFile caller(path);
                if (!caller.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) return false;
                const auto descriptor = caller.handle();
                {
                    auto reopened = conflux::audio::reopenPreparedAudio(descriptor, pcm.size() / 2, peaks);
                    const auto flags = fcntl(reopened.descriptor(), F_GETFL);
                    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || !(fcntl(reopened.descriptor(), F_GETFD) & FD_CLOEXEC) ||
                        !QFile::rename(path, path + ".old") || mkfifo(encoded.constData(), 0600) != 0 ||
                        !identicalPcm(reopened, pcm) || !overviewMatches(reopened, pcm))
                        return false;
                }
                std::array<float, 2> first{};
                return fcntl(descriptor, F_GETFD) >= 0 &&
                    pread(descriptor, first.data(), sizeof(first), 0) == static_cast<ssize_t>(sizeof(first)) &&
                    std::bit_cast<std::uint32_t>(first[0]) == std::bit_cast<std::uint32_t>(pcm[0]) &&
                    std::bit_cast<std::uint32_t>(first[1]) == std::bit_cast<std::uint32_t>(pcm[1]);
            }));
            QVERIFY(QFile::remove(path));
            QVERIFY(QFile::rename(path + ".old", path));
            QFile caller(path);
            QVERIFY(caller.open(QIODevice::ReadOnly | QIODevice::Unbuffered));
            auto invalidPeaks = peaks;
            invalidPeaks.back() = std::numeric_limits<float>::quiet_NaN();
            QVERIFY(rejects<std::invalid_argument>([&] {
                conflux::audio::reopenPreparedAudio(caller.handle(), pcm.size() / 2, invalidPeaks);
            }, "overview"));
            QVERIFY(QFile::resize(path, bytes.size() - 4));
            QVERIFY(rejects<std::invalid_argument>([&] {
                conflux::audio::reopenPreparedAudio(caller.handle(), pcm.size() / 2, peaks);
            }, "size"));
        });
    }
};

QTEST_GUILESS_MAIN(PreparedTrackStoreTest)
#include "prepared_track_store_test.moc"

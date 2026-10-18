// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/PreparedTrackStore.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
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
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using conflux::audio::PreparedAudio;
using conflux::library::PreparedTrack;
using conflux::library::PreparedTrackStore;

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
    } catch (const Exception& error) {
        return std::string(error.what()).find(reason) != std::string::npos;
    } catch (const std::exception& error) {
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

void wave(const QString& path, std::span<const float> pcm)
{
    QByteArray bytes;
    bytes.append("RIFF", 4); append32(bytes, std::uint32_t(36 + pcm.size_bytes()));
    bytes.append("WAVEfmt ", 8); append32(bytes, 16);
    append16(bytes, 3); append16(bytes, 2); append32(bytes, 48000);
    append32(bytes, 48000 * 8); append16(bytes, 8); append16(bytes, 32);
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

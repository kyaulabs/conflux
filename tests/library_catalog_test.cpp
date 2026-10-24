// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/LibraryCatalog.hpp"
#include "audio/AudioFile.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <future>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using conflux::library::LibraryCatalog;
using conflux::library::LibraryTrack;

template<class Function>
void checked(Function&& function)
{
    try { function(); }
    catch (const std::exception& error) { QFAIL(error.what()); }
}

template<class Exception, class Function>
bool rejects(Function&& function, const char* reason)
{
    try { function(); }
    catch (const Exception& error) { return std::string(error.what()).find(reason) != std::string::npos; }
    catch (const std::exception& error) { std::cerr << "Unexpected rejection: " << error.what() << '\n'; }
    return false;
}

template<class Function>
bool boundedChild(Function&& function)
{
    const auto child = fork();
    if (child < 0) throw std::runtime_error("Cannot fork catalog check");
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

void writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() || !file.flush())
        throw std::runtime_error("Cannot write catalog fixture");
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read catalog fixture");
    return file.readAll();
}

LibraryTrack track(const QString& path, unsigned id = 1)
{
    // Synthetic identities exercise metadata persistence, never audio hashing.
    return {QString::number(id, 16).rightJustified(64, '0'), path, QFileInfo(path).fileName(),
            QString::number(id + 10000, 16).rightJustified(64, '0'), 1025, false};
}

QJsonObject object(const LibraryTrack& value)
{
    return {{"source_sha256", value.sourceSha256}, {"source_path", value.sourcePath}, {"name", value.name},
            {"artifact_key", value.artifactKey}, {"frames", double(value.frames)}};
}

QByteArray document(const QJsonArray& tracks)
{
    return QJsonDocument(QJsonObject{{"version", 1}, {"tracks", tracks}}).toJson(QJsonDocument::Compact);
}

bool equalMetadata(const LibraryTrack& a, const LibraryTrack& b)
{
    return a.sourceSha256 == b.sourceSha256 && a.sourcePath == b.sourcePath && a.name == b.name &&
        a.artifactKey == b.artifactKey && a.frames == b.frames && a.cues == b.cues && a.levels == b.levels && a.savedCueFrame == b.savedCueFrame;
}
} // namespace

class LibraryCatalogTest : public QObject {
    Q_OBJECT
private slots:
    void missingCatalogIsEmptyWithoutCreatingFiles()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("not-created/catalog.json");
            QVERIFY(LibraryCatalog(path).read().empty());
            QVERIFY(!QFileInfo::exists(directory.filePath("not-created")));
            QVERIFY(rejects<std::invalid_argument>([] { LibraryCatalog catalog("relative/catalog.json"); }, "absolute"));
        });
    }

    void restartPreservesMetadataAndRefreshesAvailability()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("metadata/catalog.json");
            const auto source = directory.filePath(QStringLiteral("Björk 東京 #100% mix.wav"));
            writeBytes(source, "metadata-only source fixture, not decoded audio");
            const auto input = track(source);
            auto imported = LibraryCatalog(path).upsert(input);
            QCOMPARE(imported.size(), std::size_t(1));
            QVERIFY(equalMetadata(imported.front(), input));
            QVERIFY(imported.front().available);
            const auto saved = readBytes(path);
            const auto json = QJsonDocument::fromJson(saved).object();
            QCOMPARE(json.size(), 2);
            QCOMPARE(json.value("version").toInt(), 4);
            const auto persisted = json.value("tracks").toArray();
            QCOMPARE(persisted.size(), qsizetype(1));
            auto expected = object(input);
            expected.insert("cues", QJsonArray{});
            expected.insert("levels", QJsonValue::Null);
            expected.insert("saved_cue_frame", QJsonValue::Null);
            QCOMPARE(persisted.at(0).toObject(), expected);
            auto restarted = LibraryCatalog(path).read();
            QCOMPARE(restarted.size(), std::size_t(1));
            QVERIFY(equalMetadata(restarted.front(), input));
            QVERIFY(restarted.front().available);
            QVERIFY(QFile::remove(source));
            auto missing = LibraryCatalog(path).read();
            QCOMPARE(missing.size(), std::size_t(1));
            QVERIFY(equalMetadata(missing.front(), input));
            QVERIFY(!missing.front().available);
            QCOMPARE(readBytes(path), saved);
            QVERIFY(QDir().mkdir(source));
            QVERIFY(!LibraryCatalog(path).read().front().available);
        });
    }

    void contentAliasesUpdateOneEntryAndKeepOrder()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("catalog.json");
            const auto first = track(directory.filePath("original.wav"));
            const auto other = track(directory.filePath("other.wav"), 2);
            LibraryCatalog catalog(path);
            catalog.upsert(first);
            catalog.upsert(other);
            auto alias = track(directory.filePath("renamed copy.wav"));
            alias.available = true; // Caller guesses must never become persisted availability.
            auto merged = catalog.upsert(alias);
            QCOMPARE(merged.size(), std::size_t(2));
            QVERIFY(equalMetadata(merged[0], alias));
            QVERIFY(equalMetadata(merged[1], other));
            QVERIFY(!merged[0].available);
            auto restarted = LibraryCatalog(path).read();
            QCOMPARE(restarted.size(), std::size_t(2));
            QVERIFY(equalMetadata(restarted[0], alias));
            QVERIFY(equalMetadata(restarted[1], other));
        });
    }

    void changedContentAtSamePathReplacesIdentity()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("catalog.json");
            const auto source = directory.filePath("changed.wav");
            writeBytes(source, "original source");
            LibraryCatalog catalog(path);
            catalog.upsert(track(source));
            writeBytes(source, "replacement source");
            auto replacement = track(source, 2);
            replacement.frames = 2049;
            auto merged = catalog.upsert(replacement);
            QCOMPARE(merged.size(), std::size_t(1));
            QVERIFY(equalMetadata(merged.front(), replacement));
            QVERIFY(merged.front().available);
            auto restarted = LibraryCatalog(path).read();
            QCOMPARE(restarted.size(), std::size_t(1));
            QVERIFY(equalMetadata(restarted.front(), replacement));
        });
    }

    void malformedCatalogIsPreserved_data()
    {
        QTest::addColumn<QString>("damage");
        for (const auto* name : {"truncated", "version", "duplicate-id", "duplicate-path", "relative-path",
                                 "invalid-name", "invalid-hash", "zero-frames", "excess-frames", "extra-field",
                                 "oversized", "too-many"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void unsafeCatalogFileFailsClosed_data()
    {
        QTest::addColumn<bool>("symbolicLink");
        QTest::newRow("symbolic-link") << true;
        QTest::newRow("writerless-fifo") << false;
    }

    void unsafeCatalogFileFailsClosed()
    {
        QFETCH(bool, symbolicLink);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("catalog.json");
            const auto outside = directory.filePath("outside.json");
            const auto original = document(QJsonArray{object(track(directory.filePath("outside.wav")))});
            writeBytes(outside, original);
            const auto encoded = QFile::encodeName(path);
            if (symbolicLink)
                QCOMPARE(symlink(QFile::encodeName(outside).constData(), encoded.constData()), 0);
            else
                QCOMPARE(mkfifo(encoded.constData(), 0600), 0);
            const bool readRejected = boundedChild([&] {
                return rejects<std::runtime_error>([&] { LibraryCatalog(path).read(); }, "unsafe catalog");
            });
            const bool updateRejected = boundedChild([&] {
                return rejects<std::runtime_error>([&] {
                    LibraryCatalog(path).upsert(track(directory.filePath("new.wav"), 2));
                }, "unsafe catalog");
            });
            QVERIFY(readRejected);
            QVERIFY(updateRejected);
            QCOMPARE(readBytes(outside), original);
            struct stat status{};
            QCOMPARE(lstat(encoded.constData(), &status), 0);
            QVERIFY(symbolicLink ? S_ISLNK(status.st_mode) : S_ISFIFO(status.st_mode));
        });
    }

    void malformedCatalogIsPreserved()
    {
        QFETCH(QString, damage);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("catalog.json");
            const auto first = track(directory.filePath("first.wav"));
            auto row = object(first);
            QJsonArray rows{row};
            if (damage == "duplicate-id") rows.append(object(track(directory.filePath("alias.wav"))));
            if (damage == "duplicate-path") rows.append(object(track(first.sourcePath, 2)));
            if (damage == "relative-path") row.insert("source_path", "relative.wav");
            if (damage == "invalid-name") row.insert("name", "different.wav");
            if (damage == "invalid-hash") row.insert("source_sha256", QString(64, 'A'));
            if (damage == "zero-frames") row.insert("frames", 0);
            if (damage == "excess-frames") row.insert("frames", double(conflux::audio::PreparedAudio::defaultMaxPreparedBytes / 8 + 1));
            if (damage == "extra-field") row.insert("available", true);
            if (damage != "duplicate-id" && damage != "duplicate-path") rows = {row};
            if (damage == "too-many") {
                rows = {};
                for (std::size_t index = 0; index <= LibraryCatalog::maxEntries; ++index)
                    rows.append(object(track(directory.filePath(QString("track-%1.wav").arg(index)), unsigned(index + 1))));
            }
            auto bytes = document(rows);
            if (damage == "truncated") bytes.chop(3);
            if (damage == "version") bytes = QJsonDocument(QJsonObject{{"version", 5}, {"tracks", rows}}).toJson();
            if (damage == "oversized") bytes = QByteArray(qsizetype(LibraryCatalog::maxBytes + 1), ' ');
            writeBytes(path, bytes);
            LibraryCatalog catalog(path);
            auto read = [&] { catalog.read(); };
            auto update = [&] { catalog.upsert(track(directory.filePath("new.wav"), 9000)); };
            if (damage == "oversized" || damage == "too-many") {
                QVERIFY(rejects<std::length_error>(read, "catalog"));
                QCOMPARE(readBytes(path), bytes);
                QVERIFY(rejects<std::length_error>(update, "catalog"));
            } else {
                QVERIFY(rejects<std::runtime_error>(read, "catalog"));
                QCOMPARE(readBytes(path), bytes);
                QVERIFY(rejects<std::runtime_error>(update, "catalog"));
            }
            QCOMPARE(readBytes(path), bytes);
        });
    }

    void invalidIncomingRecordPreservesCatalog_data()
    {
        QTest::addColumn<QString>("damage");
        QTest::newRow("relative-path") << "path";
        QTest::newRow("embedded-nul") << "nul";
        QTest::newRow("non-basename") << "name";
        QTest::newRow("bad-hash") << "hash";
        QTest::newRow("zero-frames") << "frames";
    }

    void invalidIncomingRecordPreservesCatalog()
    {
        QFETCH(QString, damage);
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("catalog.json");
            LibraryCatalog catalog(path);
            catalog.upsert(track(directory.filePath("original.wav")));
            const auto before = readBytes(path);
            auto incoming = track(directory.filePath("new.wav"), 2);
            if (damage == "path") incoming.sourcePath = "relative.wav";
            if (damage == "nul") incoming.sourcePath.append(QChar(0));
            if (damage == "name") incoming.name = "folder/track.wav";
            if (damage == "hash") incoming.artifactKey = QString(64, 'Z');
            if (damage == "frames") incoming.frames = 0;
            QVERIFY(rejects<std::invalid_argument>([&] { catalog.upsert(incoming); }, "track"));
            QCOMPARE(readBytes(path), before);
        });
    }

    void capacityRejectsNewEntryAndAllowsExistingUpdate()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("catalog.json");
            QJsonArray rows;
            for (std::size_t index = 0; index < LibraryCatalog::maxEntries; ++index)
                rows.append(object(track(directory.filePath(QString("track-%1.wav").arg(index)), unsigned(index + 1))));
            const auto before = document(rows);
            QVERIFY(std::size_t(before.size()) < LibraryCatalog::maxBytes);
            writeBytes(path, before);
            LibraryCatalog catalog(path);
            QCOMPARE(catalog.read().size(), LibraryCatalog::maxEntries);
            QVERIFY(rejects<std::length_error>([&] { catalog.upsert(track(directory.filePath("extra.wav"), 9000)); }, "catalog"));
            QCOMPARE(readBytes(path), before);
            auto existing = track(directory.filePath("relocated.wav"));
            existing.frames = 1;
            auto updated = catalog.upsert(existing);
            QCOMPARE(updated.size(), LibraryCatalog::maxEntries);
            QVERIFY(equalMetadata(updated.front(), existing));
            QCOMPARE(LibraryCatalog(path).read().size(), LibraryCatalog::maxEntries);
        });
    }

    void failedAtomicSavePreservesOriginalBytes()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("catalog.json");
            LibraryCatalog catalog(path);
            auto first = track(directory.filePath(QString(120, 'a') + ".wav"));
            catalog.upsert(first);
            const auto before = readBytes(path);
            QVERIFY(before.size() > 256);
            const auto incoming = track(directory.filePath(QString(120, 'b') + ".wav"), 2);
            QVERIFY(boundedChild([&] {
                std::signal(SIGXFSZ, SIG_IGN);
                struct rlimit limit{};
                if (getrlimit(RLIMIT_FSIZE, &limit) != 0 || limit.rlim_max < 256) return false;
                limit.rlim_cur = 256;
                if (setrlimit(RLIMIT_FSIZE, &limit) != 0) return false;
                return rejects<std::runtime_error>([&] { LibraryCatalog(path).upsert(incoming); }, "save catalog");
            }));
            QCOMPARE(readBytes(path), before);
            const auto retained = catalog.read();
            QCOMPARE(retained.size(), std::size_t(1));
            QVERIFY(equalMetadata(retained.front(), first));
            QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"catalog.json"});
        });
    }

    void legacyCatalogReadsWithoutRewritingAndCuesRoundTrip()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            const auto first = track(directory.filePath("first.wav"));
            const auto legacy = document(QJsonArray{object(first)});
            writeBytes(path, legacy);
            LibraryCatalog catalog(path);
            QVERIFY(catalog.read().front().cues.empty());
            QCOMPARE(readBytes(path), legacy);
            const std::vector<conflux::library::CuePoint> cues{{1, QStringLiteral("Intro 東京 <b>"), 0}, {3, "Drop", 640}};
            const auto updated = catalog.updateCues(first.sourceSha256, first.frames, cues);
            QVERIFY(updated.front().cues == cues);
            QVERIFY(LibraryCatalog(path).read().front().cues == cues);
            QCOMPARE(QJsonDocument::fromJson(readBytes(path)).object().value("version").toInt(), 4);
            const auto rows = QJsonDocument::fromJson(readBytes(path)).object().value("tracks").toArray();
            QCOMPARE(rows.at(0).toObject().value("cues").toArray()[1].toObject().value("frame").toDouble(), 640.0);
            QVERIFY(catalog.updateCues(first.sourceSha256, first.frames, {}).front().cues.empty());
        });
    }

    void levelsSurviveRestartCueUpdatesAndMatchingAliases_data()
    {
        QTest::addColumn<double>("rms");
        QTest::addColumn<double>("peak");
        QTest::newRow("silence") << 0.0 << 0.0;
        QTest::newRow("ordinary") << .25 << .75;
        QTest::newRow("headroom") << 1.5 << 2.0;
    }

    void levelsSurviveRestartCueUpdatesAndMatchingAliases()
    {
        QFETCH(double, rms);
        QFETCH(double, peak);
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            auto original = track(directory.filePath("original.wav"));
            original.levels = conflux::audio::SignalLevels{rms, peak};
            LibraryCatalog catalog(path);
            catalog.upsert(original);
            const auto reopened = LibraryCatalog(path).read();
            QVERIFY(equalMetadata(reopened.front(), original));
            const auto document = QJsonDocument::fromJson(readBytes(path)).object();
            QCOMPARE(document.value("version").toInt(), 4);
            const auto levelObject = document.value("tracks").toArray().at(0).toObject().value("levels").toObject();
            QCOMPARE(levelObject, (QJsonObject{{"recipe", "prepared-stereo-f32-48000-levels-v1"},
                                              {"rms", rms}, {"sample_peak", peak}}));
            const auto updated = catalog.updateCues(original.sourceSha256, original.frames, {{1, "Intro", 0}});
            QVERIFY(updated.front().levels == original.levels);
            auto alias = original;
            alias.sourcePath = directory.filePath("moved.wav");
            alias.name = "moved.wav";
            alias.levels.reset();
            const auto merged = catalog.upsert(alias);
            QVERIFY(merged.front().levels == original.levels);
            QCOMPARE(merged.front().cues.front().label, QString("Intro"));
            QVERIFY(LibraryCatalog(path).read().front().levels == original.levels);
        });
    }

    void levelsAreBoundToPreparedRecipeAndFrames()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            auto original = track(directory.filePath("source.wav"));
            original.levels = conflux::audio::SignalLevels{.25, .75};
            LibraryCatalog catalog(path);
            catalog.upsert(original);
            auto newRecipe = original;
            newRecipe.artifactKey = QString(64, 'a');
            newRecipe.levels.reset();
            QVERIFY(!catalog.upsert(newRecipe).front().levels);
            catalog.upsert(original);
            auto newFrames = original;
            ++newFrames.frames;
            newFrames.levels.reset();
            QVERIFY(!catalog.upsert(newFrames).front().levels);
            catalog.upsert(original);
            auto newSource = track(original.sourcePath, 2);
            QVERIFY(!catalog.upsert(newSource).front().levels);
            newSource.levels = conflux::audio::SignalLevels{.5, 1.0};
            QVERIFY(catalog.upsert(newSource).front().levels == newSource.levels);
            newSource.levels = conflux::audio::SignalLevels{.125, .25};
            QVERIFY(catalog.upsert(newSource).front().levels == newSource.levels);
        });
    }

    void legacyCueCatalogKeepsLevelsUnknownWithoutRewriting()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            const auto original = track(directory.filePath("source.wav"));
            auto row = object(original);
            row.insert("cues", QJsonArray{QJsonObject{{"track", 1}, {"label", "Intro"}, {"frame", 0}}});
            const auto legacy = QJsonDocument(QJsonObject{{"version", 2}, {"tracks", QJsonArray{row}}}).toJson();
            writeBytes(path, legacy);
            LibraryCatalog catalog(path);
            const auto read = catalog.read();
            QVERIFY(!read.front().levels);
            QCOMPARE(read.front().cues.front().label, QString("Intro"));
            QCOMPARE(readBytes(path), legacy);
            catalog.updateCues(original.sourceSha256, original.frames, {{1, "Updated", 0}});
            const auto migrated = LibraryCatalog(path).read();
            QVERIFY(!migrated.front().levels);
            QCOMPARE(migrated.front().cues.front().label, QString("Updated"));
            QCOMPARE(QJsonDocument::fromJson(readBytes(path)).object().value("version").toInt(), 4);
        });
    }

    void invalidLevelsPreserveCatalog()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            auto original = track(directory.filePath("source.wav"));
            original.levels = conflux::audio::SignalLevels{.25, .75};
            LibraryCatalog catalog(path);
            catalog.upsert(original);
            const auto before = readBytes(path);
            const auto inf = std::numeric_limits<double>::infinity();
            const auto nan = std::numeric_limits<double>::quiet_NaN();
            const auto tooLarge = double(std::numeric_limits<float>::max()) * 2;
            for (const auto invalid : {conflux::audio::SignalLevels{-.1, .5}, {.5, -.1}, {.75, .5},
                                       {0, .5}, {inf, inf}, {nan, .5}, {.1, nan}, {tooLarge, tooLarge},
                                       {1e-100, 1e-100}, {.05, .1}}) {
                auto incoming = original;
                incoming.levels = invalid;
                QVERIFY(rejects<std::invalid_argument>([&] { catalog.upsert(incoming); }, "levels"));
                QCOMPARE(readBytes(path), before);
            }
        });
    }

    void levelsRespectStereoGeometryAndFloatSamplePeaks()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            auto original = track(directory.filePath("source.wav"));
            original.frames = 1;
            original.levels = conflux::audio::SignalLevels{1 / std::sqrt(2.0), 1};
            LibraryCatalog catalog(path);
            QVERIFY(catalog.upsert(original).front().levels == original.levels);
            const auto before = readBytes(path);
            original.levels = conflux::audio::SignalLevels{.1, 1};
            QVERIFY(rejects<std::invalid_argument>([&] { catalog.upsert(original); }, "levels"));
            QCOMPARE(readBytes(path), before);
            const double smallestPeak = std::numeric_limits<float>::denorm_min();
            original.levels = conflux::audio::SignalLevels{smallestPeak / std::sqrt(2.0), smallestPeak};
            QVERIFY(catalog.upsert(original).front().levels == original.levels);
            QVERIFY(LibraryCatalog(path).read().front().levels == original.levels);
        });
    }

    void malformedPersistedLevelsFailClosed_data()
    {
        QTest::addColumn<QString>("damage");
        for (const auto* damage : {"missing", "array", "recipe", "extra", "negative", "peak-below-rms", "string", "null-rms", "zero-rms", "huge", "underflow-peak", "non-float-peak", "impossible-rms"})
            QTest::newRow(damage) << QString::fromLatin1(damage);
    }

    void malformedPersistedLevelsFailClosed()
    {
        QFETCH(QString, damage);
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            const auto original = track(directory.filePath("source.wav"));
            auto row = object(original);
            row.insert("cues", QJsonArray{});
            QJsonObject levels{{"recipe", "prepared-stereo-f32-48000-levels-v1"}, {"rms", .25}, {"sample_peak", .75}};
            if (damage == "recipe") levels.insert("recipe", "unknown-levels-v2");
            if (damage == "extra") levels.insert("source", "untrusted");
            if (damage == "negative") levels.insert("rms", -.25);
            if (damage == "peak-below-rms") levels.insert("sample_peak", .1);
            if (damage == "string") levels.insert("rms", ".25");
            if (damage == "null-rms") levels.insert("rms", QJsonValue::Null);
            if (damage == "zero-rms") levels.insert("rms", 0);
            if (damage == "huge") levels.insert("sample_peak", 1e100);
            if (damage == "underflow-peak") levels = {{"recipe", "prepared-stereo-f32-48000-levels-v1"}, {"rms", 1e-100}, {"sample_peak", 1e-100}};
            if (damage == "non-float-peak") levels.insert("sample_peak", .7);
            if (damage == "impossible-rms") { row.insert("frames", 1); levels.insert("rms", .1); }
            if (damage != "missing") row.insert("levels", damage == "array" ? QJsonValue(QJsonArray{}) : QJsonValue(levels));
            const auto bytes = QJsonDocument(QJsonObject{{"version", 3}, {"tracks", QJsonArray{row}}}).toJson();
            writeBytes(path, bytes);
            LibraryCatalog catalog(path);
            QVERIFY(rejects<std::runtime_error>([&] { catalog.read(); }, "catalog"));
            QVERIFY(rejects<std::runtime_error>([&] { catalog.upsert(original); }, "catalog"));
            QCOMPARE(readBytes(path), bytes);
        });
    }

    void reimportPreservesContentCuesAndNewContentDropsThem()
    {
        checked([&] {
            QTemporaryDir directory;
            LibraryCatalog catalog(directory.filePath("catalog.json"));
            const auto original = track(directory.filePath("original.wav"));
            catalog.upsert(original);
            const std::vector<conflux::library::CuePoint> cues{{1, "Intro", 0}, {2, "Drop", 640}};
            catalog.updateCues(original.sourceSha256, original.frames, cues);
            auto alias = track(directory.filePath("moved.wav"));
            auto merged = catalog.upsert(alias);
            QVERIFY(merged.front().cues == cues);
            QCOMPARE(merged.front().sourcePath, alias.sourcePath);
            auto changed = track(alias.sourcePath, 2);
            changed.cues = cues; // A caller carrying old metadata cannot transplant it to new content.
            changed.frames = 1;
            merged = catalog.upsert(changed);
            QVERIFY(merged.front().cues.empty());
            const auto before = readBytes(directory.filePath("catalog.json"));
            QVERIFY(rejects<std::runtime_error>([&] { catalog.updateCues(original.sourceSha256, original.frames, cues); }, "changed"));
            QVERIFY(rejects<std::runtime_error>([&] { catalog.updateCues(changed.sourceSha256, changed.frames + 1, {{1, "Intro", 0}}); }, "changed"));
            QCOMPARE(readBytes(directory.filePath("catalog.json")), before);
        });
    }

    void invalidCuesCannotReplaceStoredCues()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            const auto first = track(directory.filePath("first.wav"));
            LibraryCatalog catalog(path);
            catalog.upsert(first);
            catalog.updateCues(first.sourceSha256, first.frames, {{1, "Keep", 0}});
            const auto before = readBytes(path);
            const std::vector<std::vector<conflux::library::CuePoint>> invalid{
                {{0, "Bad", 0}}, {{100, "Bad", 0}}, {{1, "", 0}}, {{1, QString(257, 'x'), 0}},
                {{1, "line\nbreak", 0}}, {{1, "EOF", first.frames}}, {{1, "A", 640}, {2, "B", 640}},
                {{2, "A", 0}, {1, "B", 640}}};
            for (const auto& cues : invalid) {
                QVERIFY(rejects<std::invalid_argument>([&] { catalog.updateCues(first.sourceSha256, first.frames, cues); }, "cue"));
                QCOMPARE(readBytes(path), before);
            }
            std::vector<conflux::library::CuePoint> tooMany(100, {1, "Bad", 0});
            QVERIFY(rejects<std::length_error>([&] { catalog.updateCues(first.sourceSha256, first.frames, tooMany); }, "cue"));
            QCOMPARE(readBytes(path), before);
        });
    }

    void malformedPersistedCuesFailClosed()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            const auto first = track(directory.filePath("first.wav"));
            for (const QJsonValue& bad : {QJsonValue(true), QJsonValue(-1), QJsonValue(1.5), QJsonValue(double(first.frames))}) {
                auto row = object(first);
                row.insert("cues", QJsonArray{QJsonObject{{"track", 1}, {"label", "Bad"}, {"frame", bad}}});
                const auto bytes = QJsonDocument(QJsonObject{{"version", 2}, {"tracks", QJsonArray{row}}}).toJson();
                writeBytes(path, bytes);
                QVERIFY(rejects<std::runtime_error>([&] { LibraryCatalog(path).read(); }, "cue"));
                QVERIFY(rejects<std::runtime_error>([&] { LibraryCatalog(path).updateCues(first.sourceSha256, first.frames, {{1, "Good", 0}}); }, "cue"));
                QCOMPARE(readBytes(path), bytes);
            }
        });
    }

    void cueWriteFailurePreservesPreviousCatalog()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            const auto first = track(directory.filePath(QString(120, 'x') + ".wav"));
            LibraryCatalog(path).upsert(first);
            const auto before = readBytes(path);
            QVERIFY(boundedChild([&] {
                std::signal(SIGXFSZ, SIG_IGN);
                struct rlimit limit{};
                if (getrlimit(RLIMIT_FSIZE, &limit) != 0 || limit.rlim_max < 256) return false;
                limit.rlim_cur = 256;
                if (setrlimit(RLIMIT_FSIZE, &limit) != 0) return false;
                return rejects<std::runtime_error>([&] {
                    LibraryCatalog(path).updateCues(first.sourceSha256, first.frames, {{1, "Not committed", 0}});
                }, "save catalog");
            }));
            QCOMPARE(readBytes(path), before);
        });
    }

    void concurrentCueUpdateAndImportPreserveBoth()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            auto first = track(directory.filePath("first.wav"));
            const auto second = track(directory.filePath("second.wav"), 2);
            first.levels = conflux::audio::SignalLevels{.25, .5};
            LibraryCatalog(path).upsert(first);
            QVERIFY(boundedChild([&] {
                std::latch ready(2), start(1);
                auto a = std::async(std::launch::async, [&] {
                    ready.count_down(); start.wait();
                    return LibraryCatalog(path).updateCues(first.sourceSha256, first.frames, {{1, "Intro", 0}});
                });
                auto b = std::async(std::launch::async, [&] {
                    ready.count_down(); start.wait(); return LibraryCatalog(path).upsert(second);
                });
                ready.wait(); start.count_down(); a.get(); b.get();
                const auto tracks = LibraryCatalog(path).read();
                const auto one = std::ranges::find_if(tracks, [&](const auto& value) { return value.sourceSha256 == first.sourceSha256; });
                return tracks.size() == 2 && one != tracks.end() && one->cues.size() == 1 &&
                    one->cues.front().label == "Intro" && one->levels == first.levels;
            }));
        });
    }

    void savedCuePersistsWithoutChangingImportedCuesOrLevels_data()
    {
        QTest::addColumn<qulonglong>("frame");
        QTest::newRow("first-frame") << qulonglong(0);
        QTest::newRow("last-frame") << qulonglong(1024);
    }

    void savedCuePersistsWithoutChangingImportedCuesOrLevels()
    {
        QFETCH(qulonglong, frame);
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            auto original = track(directory.filePath("source.wav"));
            original.levels = conflux::audio::SignalLevels{.25, .5};
            LibraryCatalog catalog(path);
            catalog.upsert(original);
            const std::vector<conflux::library::CuePoint> imported{{3, "Intro", 0}, {11, "Drop", 640}};
            catalog.updateCues(original.sourceSha256, original.frames, imported);
            auto saved = catalog.updateSavedCue(original.sourceSha256, original.frames, frame).front();
            QVERIFY(saved.savedCueFrame && *saved.savedCueFrame == frame);
            QVERIFY(saved.cues == imported);
            QVERIFY(saved.levels == original.levels);
            const auto reopened = LibraryCatalog(path).read().front();
            QVERIFY(equalMetadata(saved, reopened));
            const auto document = QJsonDocument::fromJson(readBytes(path)).object();
            QCOMPARE(document.value("version").toInt(), 4);
            QCOMPARE(document.value("tracks").toArray()[0].toObject().value("saved_cue_frame").toDouble(), double(frame));
            const std::vector<conflux::library::CuePoint> nextImport{{2, "Start", 20}, {9, "End", 800}};
            saved = catalog.updateCues(original.sourceSha256, original.frames, nextImport).front();
            QVERIFY(saved.cues == nextImport);
            QVERIFY(saved.savedCueFrame == reopened.savedCueFrame);
            QVERIFY(saved.levels == original.levels);
            QVERIFY(equalMetadata(saved, LibraryCatalog(path).read().front()));
        });
    }

    void sameContentReimportRetainsSavedCueAndNewContentDropsIt()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            auto original = track(directory.filePath("source.wav"));
            original.levels = conflux::audio::SignalLevels{.25, .5};
            LibraryCatalog catalog(path);
            catalog.upsert(original);
            const std::vector<conflux::library::CuePoint> cues{{3, "Intro", 0}, {11, "Drop", 640}};
            catalog.updateCues(original.sourceSha256, original.frames, cues);
            catalog.updateSavedCue(original.sourceSha256, original.frames, 700);
            auto alias = track(directory.filePath("moved.wav"));
            alias.savedCueFrame = 0; // Reimport cannot replace a DJ correction.
            auto saved = catalog.upsert(alias).front();
            QVERIFY(saved.savedCueFrame && *saved.savedCueFrame == 700);
            QVERIFY(saved.cues == cues);
            QVERIFY(saved.levels == original.levels);
            QVERIFY(equalMetadata(saved, LibraryCatalog(path).read().front()));
            auto resized = alias;
            ++resized.frames; // Even an in-range point cannot change clocks silently.
            const auto before = readBytes(path);
            QVERIFY(rejects<std::runtime_error>([&] { catalog.upsert(resized); }, "changed"));
            QCOMPARE(readBytes(path), before);
            auto changed = track(alias.sourcePath, 2);
            changed.savedCueFrame = 700;
            changed.cues = cues;
            changed.frames = 1;
            saved = catalog.upsert(changed).front();
            QVERIFY(!saved.savedCueFrame);
            QVERIFY(saved.cues.empty());
            QVERIFY(!saved.levels);
            const auto replaced = readBytes(path);
            QVERIFY(rejects<std::runtime_error>([&] { catalog.updateSavedCue(original.sourceSha256, original.frames, 0); }, "changed"));
            QCOMPARE(readBytes(path), replaced);
        });
    }

    void invalidOrStaleSavedCuePreservesCatalog()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            const auto original = track(directory.filePath("source.wav"));
            LibraryCatalog catalog(path);
            catalog.upsert(original);
            catalog.updateSavedCue(original.sourceSha256, original.frames, 640);
            const auto before = readBytes(path);
            for (const auto frame : {original.frames, original.frames + 1, std::numeric_limits<std::uint64_t>::max()}) {
                QVERIFY(rejects<std::invalid_argument>([&] { catalog.updateSavedCue(original.sourceSha256, original.frames, frame); }, "saved cue"));
                QCOMPARE(readBytes(path), before);
            }
            QVERIFY(rejects<std::invalid_argument>([&] { catalog.updateSavedCue("bad", original.frames, 0); }, "saved cue"));
            QVERIFY(rejects<std::invalid_argument>([&] { catalog.updateSavedCue(original.sourceSha256, 0, 0); }, "saved cue"));
            QVERIFY(rejects<std::invalid_argument>([&] { catalog.updateSavedCue(original.sourceSha256, conflux::audio::PreparedAudio::defaultMaxPreparedBytes / 8 + 1, 0); }, "saved cue"));
            QVERIFY(rejects<std::runtime_error>([&] { catalog.updateSavedCue(original.sourceSha256, original.frames + 1, 0); }, "changed"));
            const auto missing = track(directory.filePath("missing.wav"), 2);
            QVERIFY(rejects<std::runtime_error>([&] { catalog.updateSavedCue(missing.sourceSha256, missing.frames, 0); }, "changed"));
            QCOMPARE(readBytes(path), before);
        });
    }

    void legacyCatalogSavedCueIsUnknownWithoutRewrite_data()
    {
        QTest::addColumn<int>("version");
        for (int version = 1; version <= 3; ++version)
            QTest::newRow(qPrintable(QString::number(version))) << version;
    }

    void legacyCatalogSavedCueIsUnknownWithoutRewrite()
    {
        QFETCH(int, version);
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            const auto original = track(directory.filePath("source.wav"));
            auto row = object(original);
            if (version >= 2) row.insert("cues", QJsonArray{QJsonObject{{"track", 7}, {"label", "Imported"}, {"frame", 640}}});
            if (version >= 3) row.insert("levels", QJsonObject{{"recipe", "prepared-stereo-f32-48000-levels-v1"}, {"rms", .25}, {"sample_peak", .5}});
            const auto bytes = QJsonDocument(QJsonObject{{"version", version}, {"tracks", QJsonArray{row}}}).toJson();
            writeBytes(path, bytes);
            LibraryCatalog catalog(path);
            const auto legacy = catalog.read().front();
            QVERIFY(!legacy.savedCueFrame);
            QCOMPARE(readBytes(path), bytes);
            const auto saved = catalog.updateSavedCue(original.sourceSha256, original.frames, 0).front();
            QVERIFY(saved.savedCueFrame && *saved.savedCueFrame == 0);
            QVERIFY(saved.cues == legacy.cues);
            QVERIFY(saved.levels == legacy.levels);
            QVERIFY(equalMetadata(saved, LibraryCatalog(path).read().front()));
            QCOMPARE(QJsonDocument::fromJson(readBytes(path)).object().value("version").toInt(), 4);
        });
    }

    void malformedPersistedSavedCueFailsClosed_data()
    {
        QTest::addColumn<QString>("damage");
        for (const auto* damage : {"missing", "bool", "negative", "fractional", "string", "array", "object", "EOF", "past-EOF", "huge"})
            QTest::newRow(damage) << QString::fromLatin1(damage);
    }

    void malformedPersistedSavedCueFailsClosed()
    {
        QFETCH(QString, damage);
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            const auto original = track(directory.filePath("source.wav"));
            auto row = object(original);
            row.insert("cues", QJsonArray{});
            row.insert("levels", QJsonValue::Null);
            QJsonValue value(0);
            if (damage == "bool") value = true;
            if (damage == "negative") value = -1;
            if (damage == "fractional") value = 1.5;
            if (damage == "string") value = "640";
            if (damage == "array") value = QJsonArray{};
            if (damage == "object") value = QJsonObject{};
            if (damage == "EOF") value = double(original.frames);
            if (damage == "past-EOF") value = double(original.frames + 1);
            if (damage == "huge") value = 1e100;
            if (damage != "missing") row.insert("saved_cue_frame", value);
            const auto bytes = QJsonDocument(QJsonObject{{"version", 4}, {"tracks", QJsonArray{row}}}).toJson();
            writeBytes(path, bytes);
            LibraryCatalog catalog(path);
            QVERIFY(rejects<std::runtime_error>([&] { catalog.read(); }, "catalog"));
            QVERIFY(rejects<std::runtime_error>([&] { catalog.upsert(original); }, "catalog"));
            QVERIFY(rejects<std::runtime_error>([&] { catalog.updateCues(original.sourceSha256, original.frames, {}); }, "catalog"));
            QVERIFY(rejects<std::runtime_error>([&] { catalog.updateSavedCue(original.sourceSha256, original.frames, 0); }, "catalog"));
            QCOMPARE(readBytes(path), bytes);
        });
    }

    void concurrentSavedCueAndCueImportPreserveBoth()
    {
        checked([&] {
            QTemporaryDir directory;
            const auto path = directory.filePath("catalog.json");
            auto original = track(directory.filePath("source.wav"));
            original.levels = conflux::audio::SignalLevels{.25, .5};
            LibraryCatalog(path).upsert(original);
            const std::vector<conflux::library::CuePoint> cues{{3, "Intro", 0}, {11, "Drop", 640}};
            QVERIFY(boundedChild([&] {
                std::latch ready(2), start(1);
                auto a = std::async(std::launch::async, [&] {
                    ready.count_down(); start.wait();
                    return LibraryCatalog(path).updateSavedCue(original.sourceSha256, original.frames, 700);
                });
                auto b = std::async(std::launch::async, [&] {
                    ready.count_down(); start.wait();
                    return LibraryCatalog(path).updateCues(original.sourceSha256, original.frames, cues);
                });
                ready.wait(); start.count_down(); a.get(); b.get();
                const auto saved = LibraryCatalog(path).read().front();
                return saved.savedCueFrame && *saved.savedCueFrame == 700 && saved.cues == cues && saved.levels == original.levels;
            }));
        });
    }

    void concurrentWritersMergeBothEntries()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("catalog.json");
            const auto first = track(directory.filePath("first.wav"));
            const auto second = track(directory.filePath("second.wav"), 2);
            QVERIFY(boundedChild([&] {
                std::latch ready(2), start(1);
                auto writer = [&](const LibraryTrack& incoming) {
                    ready.count_down();
                    start.wait();
                    return LibraryCatalog(path).upsert(incoming);
                };
                auto a = std::async(std::launch::async, [&] { return writer(first); });
                auto b = std::async(std::launch::async, [&] { return writer(second); });
                ready.wait();
                start.count_down();
                if (a.get().empty() || b.get().empty()) return false;
                const auto merged = LibraryCatalog(path).read();
                return merged.size() == 2 && std::ranges::count_if(merged, [&](const auto& value) { return equalMetadata(value, first); }) == 1 &&
                    std::ranges::count_if(merged, [&](const auto& value) { return equalMetadata(value, second); }) == 1;
            }));
            QCOMPARE(LibraryCatalog(path).read().size(), std::size_t(2));
        });
    }

    void heldLockFailsWithinBoundAndPreservesCatalog()
    {
        checked([&] {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("catalog.json");
            LibraryCatalog catalog(path);
            catalog.upsert(track(directory.filePath("first.wav")));
            const auto before = readBytes(path);
            QLockFile lock(path + ".lock");
            QVERIFY(lock.tryLock(0));
            QVERIFY(boundedChild([&] {
                return rejects<std::runtime_error>([&] {
                    LibraryCatalog(path).upsert(track(directory.filePath("second.wav"), 2));
                }, "busy");
            }));
            QCOMPARE(readBytes(path), before);
        });
    }
};

QTEST_GUILESS_MAIN(LibraryCatalogTest)
#include "library_catalog_test.moc"

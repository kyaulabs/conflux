// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui/DeckController.hpp"
#include "StemExportFixture.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

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
            verifyNoPlayback(controller);
            QCOMPARE(controller.submittedCommands(), std::uint64_t(0));
        }
        conflux::DeckController reopened;
        QTRY_VERIFY_WITH_TIMEOUT(reopened.libraryReady(), 5000);
        QCOMPARE(reopened.libraryTracks().size(), qsizetype(1));
        QCOMPARE(reopened.libraryTracks().front().toMap()["id"].toString(), sourceId);
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
        const auto updatedId = reopened.libraryTracks().front().toMap()["id"].toString();
        QVERIFY(QFile::remove(path));
        reopened.loadLibraryTrack(0, updatedId);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.busy(), 5000);
        QVERIFY(!reopened.error().isEmpty());
        QCOMPARE(reopened.decks(), previous);
        QCOMPARE(reopened.submittedCommands(), commands);
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
        verifyNoPlayback(controller);
        controller.shutdown();
        controller.shutdown();
        verifyNoPlayback(controller);
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
        const auto first = directory.filePath("kept-track.wav");
        const auto second = directory.filePath("cancelled-replacement.wav");
        QVERIFY(writeWave(first, 2, 48000, 480));
        QVERIFY(writeWave(second, 2, 48000, 960));
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
        QCOMPARE(controller.decks()[1].toMap()["name"].toString(), QString("cancelled-replacement.wav"));
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
        const auto original = directory.filePath("original.wav");
        QVERIFY(writeWave(original, 2, 48000, 8));
        conflux::DeckController controller;
        controller.loadDeck(0, QUrl::fromLocalFile(original));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto retained = controller.decks()[0].toMap();
        QVERIFY(retained["loaded"].toBool());
        QVERIFY(!retained["replacing"].toBool());
        controller.loadDeck(0, QUrl::fromLocalFile(directory.filePath("missing-next.wav")));
        QVERIFY(controller.busy());
        QCOMPARE(controller.decks()[0].toMap(), retained);
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

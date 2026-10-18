// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui/DeckController.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
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
    std::array<const char*, 3> names{"XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE"};
    std::array<QByteArray, 3> values;
    std::array<bool, 3> present;
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
    if (!cache.isValid()) return 2;
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    DeckControllerTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "deck_controller_test.moc"

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui/DeckController.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <limits>
#include <memory>

namespace {

bool writeUnsupportedWave(const QString& path, quint16 channels, quint32 rate)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    constexpr quint32 frames = 4;
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
        const auto mono = directory.filePath("mono.wav");
        const auto otherRate = directory.filePath("44100.wav");
        QVERIFY(writeUnsupportedWave(mono, 1, 48000));
        QVERIFY(writeUnsupportedWave(otherRate, 2, 44100));
        const QStringList paths{directory.filePath("missing.wav"), corrupt, mono, otherRate};
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
        QVERIFY(writeUnsupportedWave(first, 1, 48000));
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

    void closeWithPendingDecode()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("unsupported.wav");
        QVERIFY(writeUnsupportedWave(path, 2, 44100));
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
};

} // namespace

QTEST_GUILESS_MAIN(DeckControllerTest)
#include "deck_controller_test.moc"

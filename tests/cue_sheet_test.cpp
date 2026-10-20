// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/CueSheet.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <stop_token>
#include <stdexcept>
#include <sys/stat.h>

namespace {
using conflux::library::LibraryTrack;
using conflux::library::readCueSheet;

void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() || !file.flush())
        throw std::runtime_error("Cannot write CUE fixture");
}

struct Fixture {
    QTemporaryDir directory;
    QString source, cue;
    LibraryTrack selected;
    Fixture()
    {
        if (!directory.isValid()) throw std::runtime_error("Cannot create CUE fixture directory");
        source = directory.filePath(QStringLiteral("東京 mix #100%.flac"));
        cue = directory.filePath("set.cue");
        const QByteArray content("Selected source identity fixture; no audio decoder is needed.");
        write(source, content);
        selected = {QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()),
            source, QFileInfo(source).fileName(), QString(64, 'a'), 4000000, true};
    }
    QByteArray sheet(const QByteArray& body = "TRACK 01 AUDIO\nINDEX 01 00:00:00\n") const
    {
        return "FILE \"" + QFileInfo(source).fileName().toUtf8() + "\" WAVE\n" + body;
    }
};
} // namespace

class CueSheetTest : public QObject {
    Q_OBJECT
private slots:
    void exactSourceRelativeFramesTitlesAndMultiFileSelection()
    {
        Fixture f;
        const auto text = QByteArray("\xef\xbb\xbf") + "TITLE \"Album title\"\r\nREM uninterpreted \"comment\r\n"
            "CDTEXTFILE \"must-not-open\"\r\nFILE \"missing-other.bin\" BINARY\r\n"
            "TRACK 01 MODE1/2352\r\nINDEX 01 00:00:00\r\n" + f.sheet(
            "TRACK 02 AUDIO\r\nTITLE \"Intro 東京 <b>\"\r\nPREGAP 00:02:00\r\n"
            "INDEX 00 00:00:00\r\nINDEX 01 00:00:01\r\nINDEX 02 00:00:02\r\n"
            "TRACK 03 AUDIO\r\nINDEX 01 01:02:03\r\nPOSTGAP 00:01:00\r\n");
        const auto unused = f.directory.filePath("must-not-open");
        QVERIFY(mkfifo(QFile::encodeName(unused).constData(), 0600) == 0);
        write(f.cue, text);
        const auto result = readCueSheet(f.cue, f.selected);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(!result.cancelled);
        QCOMPARE(result.cues.size(), std::size_t(2));
        QCOMPARE(result.cues[0].trackNumber, 2);
        QCOMPARE(result.cues[0].label, QStringLiteral("Intro 東京 <b>"));
        QCOMPARE(result.cues[0].frame, std::uint64_t(640));
        QCOMPARE(result.cues[1].trackNumber, 3);
        QCOMPARE(result.cues[1].label, QString("Track 03"));
        QCOMPARE(result.cues[1].frame, std::uint64_t(2977920));
    }

    void relativeParentAndAbsolutePathsMatchWithoutBasenameGuessing()
    {
        Fixture f;
        QVERIFY(QDir(f.directory.path()).mkdir("lists"));
        f.cue = f.directory.filePath("lists/set.cue");
        write(f.cue, "FILE \"../" + QFileInfo(f.source).fileName().toUtf8() + "\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n");
        QVERIFY(readCueSheet(f.cue, f.selected).error.isEmpty());
        write(f.cue, "FILE \"" + f.source.toUtf8() + "\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n");
        QVERIFY(readCueSheet(f.cue, f.selected).error.isEmpty());
        write(f.cue, f.sheet()); // Same basename in the wrong directory is not the selected source.
        const auto mismatch = readCueSheet(f.cue, f.selected);
        QVERIFY(!mismatch.error.isEmpty());
        QVERIFY(mismatch.cues.empty());
    }

    void malformedTimeAndStructureRejectWholeImport_data()
    {
        QTest::addColumn<QByteArray>("body");
        for (const auto& time : {QByteArray("00:60:00"), QByteArray("00:00:75"), QByteArray("-1:00:00"),
                QByteArray("999999999999999999999:00:00"), QByteArray("00:00:1.5"), QByteArray("00:00"), QByteArray("02:00:00")})
            QTest::newRow(time.constData()) << QByteArray("TRACK 01 AUDIO\nINDEX 01 ") + time + '\n';
        QTest::newRow("missing-index-one") << QByteArray("TRACK 01 AUDIO\nINDEX 00 00:00:00\n");
        QTest::newRow("duplicate-index") << QByteArray("TRACK 01 AUDIO\nINDEX 01 00:00:00\nINDEX 01 00:00:01\n");
        QTest::newRow("late-error-discards-first-cue") << QByteArray("TRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:00:00\n");
        QTest::newRow("duplicate-track") << QByteArray("TRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 01 AUDIO\nINDEX 01 00:00:01\n");
        QTest::newRow("index-two-first") << QByteArray("TRACK 01 AUDIO\nINDEX 02 00:00:00\n");
        QTest::newRow("selected-data-track") << QByteArray("TRACK 01 MODE1/2352\nINDEX 01 00:00:00\n");
        QTest::newRow("unclosed-title") << QByteArray("TRACK 01 AUDIO\nTITLE \"unclosed\nINDEX 01 00:00:00\n");
        QTest::newRow("unknown-command") << QByteArray("TRACK 01 AUDIO\nEXEC bad\nINDEX 01 00:00:00\n");
        QTest::newRow("long-title") << QByteArray("TRACK 01 AUDIO\nTITLE \"") + QByteArray(257, 'x') + "\"\nINDEX 01 00:00:00\n";
    }

    void malformedTimeAndStructureRejectWholeImport()
    {
        QFETCH(QByteArray, body);
        Fixture f;
        write(f.cue, f.sheet(body));
        const auto result = readCueSheet(f.cue, f.selected);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!result.cancelled);
        QVERIFY(result.cues.empty());
    }

    void duplicateFileReferenceAndEofCueFail()
    {
        Fixture f;
        write(f.cue, f.sheet() + f.sheet("TRACK 02 AUDIO\nINDEX 01 00:00:01\n"));
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
        f.selected.frames = 640;
        write(f.cue, f.sheet("TRACK 01 AUDIO\nINDEX 01 00:00:01\n"));
        const auto result = readCueSheet(f.cue, f.selected);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.cues.empty());
    }

    void sourceIdentityAndRegularFileRequirementsAreRechecked()
    {
        Fixture f;
        write(f.cue, f.sheet());
        write(f.source, "changed after library import");
        const auto changed = readCueSheet(f.cue, f.selected);
        QVERIFY(!changed.error.isEmpty());
        QVERIFY(changed.cues.empty());
        QVERIFY(QFile::remove(f.source));
        QVERIFY(mkfifo(QFile::encodeName(f.source).constData(), 0600) == 0);
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
        QVERIFY(QFile::remove(f.source));
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
    }

    void unsafeCueTypesAndTextFailClosed()
    {
        Fixture f;
        const auto actual = f.directory.filePath("actual.cue");
        write(actual, f.sheet());
        QVERIFY(QFile::link(actual, f.cue));
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
        QVERIFY(QFile::remove(f.cue));
        QVERIFY(mkfifo(QFile::encodeName(f.cue).constData(), 0600) == 0);
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
        QVERIFY(QFile::remove(f.cue));
        for (const auto& bytes : {QByteArray{}, QByteArray("\xff"), f.sheet() + QByteArray(1, '\0')}) {
            write(f.cue, bytes);
            const auto result = readCueSheet(f.cue, f.selected);
            QVERIFY(!result.error.isEmpty());
            QVERIFY(result.cues.empty());
        }
        QVERIFY(!readCueSheet("relative.cue", f.selected).error.isEmpty());
    }

    void boundedSheetAndTrackCounts()
    {
        Fixture f;
        write(f.cue, f.sheet());
        QFile file(f.cue);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(1024 * 1024 + 1));
        file.close();
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
        write(f.cue, f.sheet() + "REM " + QByteArray(4097, 'x'));
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
        write(f.cue, f.sheet() + QByteArray(4097, ' '));
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
        write(f.cue, f.sheet() + QByteArray(8193, '\n'));
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
        QByteArray tracks;
        for (int number = 1; number <= 99; ++number)
            tracks += QString("TRACK %1 AUDIO\nINDEX 01 00:%2:%3\n").arg(number).arg((number - 1) / 75).arg((number - 1) % 75).toUtf8();
        write(f.cue, f.sheet(tracks));
        const auto accepted = readCueSheet(f.cue, f.selected);
        QVERIFY2(accepted.error.isEmpty(), qPrintable(accepted.error));
        QCOMPARE(accepted.cues.size(), std::size_t(99));
        write(f.cue, f.sheet(tracks + "TRACK 100 AUDIO\nINDEX 01 00:02:00\n"));
        QVERIFY(!readCueSheet(f.cue, f.selected).error.isEmpty());
    }

    void truncatedUtf8TailCannotBeDroppedAfterValidCuesOrInsideComment()
    {
        Fixture f;
        write(f.cue, f.sheet() + "REM " + QByteArray::fromHex("f09f92a1"));
        const auto valid = readCueSheet(f.cue, f.selected);
        QVERIFY2(valid.error.isEmpty(), qPrintable(valid.error));
        QCOMPARE(valid.cues.size(), std::size_t(1));
        for (const auto& tail : {QByteArray::fromHex("c2"), QByteArray::fromHex("e282"), QByteArray::fromHex("f09f92")}) {
            for (const auto& prefix : {f.sheet(), f.sheet() + "REM trailing comment "}) {
                write(f.cue, prefix + tail);
                const auto result = readCueSheet(f.cue, f.selected);
                QVERIFY2(result.error.contains("UTF-8"), qPrintable(result.error));
                QVERIFY(result.cues.empty());
                QVERIFY(!result.cancelled);
            }
        }
    }

    void cancelledImportDoesNotOpenFilesOrReturnPartialCues()
    {
        Fixture f;
        std::stop_source stop;
        stop.request_stop();
        const auto result = readCueSheet(f.cue, f.selected, stop.get_token()); // No CUE file exists.
        QVERIFY(result.cancelled);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.cues.empty());
    }
};

QTEST_GUILESS_MAIN(CueSheetTest)
#include "cue_sheet_test.moc"

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/TrackPresentation.hpp"
#include "library/PreparedTrackStore.hpp"
#include "TrackPresentationFixture.hpp"

#include <QTemporaryDir>
#include <QtTest>
#include <taglib/attachedpictureframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/vorbisfile.h>
#include <taglib/opusfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/wavfile.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// Deterministic descriptor-local cancellation/change at the final source
// identity check. Real writes affect this generated fixture only.
struct Injection {
    int reader = -1, writer = -1;
    unsigned calls = 0;
    std::stop_source* stop = nullptr;
} injection;
struct ScopedInjection {
    ScopedInjection(int reader, int writer = -1, std::stop_source* stop = nullptr)
    { injection = {reader, writer, 0, stop}; }
    ~ScopedInjection() { injection = {}; }
};
template<class Stat, class Function>
int injectedStat(int fd, Stat* value, Function real)
{
    if (fd == injection.reader && ++injection.calls == 2) {
        if (injection.writer >= 0) (void)ftruncate(injection.writer, 1);
        if (injection.stop) injection.stop->request_stop();
    }
    return real(fd, value);
}
using conflux::library::readTrackPresentation;
QByteArray integerBytes(unsigned value, bool syncsafe = false)
{
    QByteArray result;
    const unsigned shift = syncsafe ? 7 : 8, mask = syncsafe ? 127 : 255;
    for (const unsigned part : {3U, 2U, 1U, 0U}) result.append(char((value >> (part * shift)) & mask));
    return result;
}
QByteArray id3Frame(const char* id, const QByteArray& data, bool compressed = false)
{
    return QByteArray(id, 4) + integerBytes(unsigned(data.size())) + QByteArray(1, '\0') +
        QByteArray(1, compressed ? char(0x80) : '\0') + data;
}
bool writeMpeg(const QString& path, const QByteArray& tag)
{
    QByteArray frame(417, '\0'); // Generated MPEG1 layer3 128kb/s44.1kHz headers; no decoding here.
    frame.replace(0, 4, QByteArray::fromHex("fffb9000"));
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(tag + frame.repeated(3)) == tag.size() + frame.size() * 3;
}
QByteArray id3Tag(const QByteArray& frames)
{ return QByteArray("ID3\x03\0\0", 6) + integerBytes(unsigned(frames.size()), true) + frames; }
class PresentationTest : public QObject {
    Q_OBJECT
private slots:
    void tagsAndArtworkPreserveSource()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("fallback.flac");
        QVERIFY(presentationFixture::writeFlac(path, "<b>Title</b>", "Artist & artist", "Album"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto before = file.readAll();
        QVERIFY(file.seek(17));
        const auto result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title, QString("<b>Title</b>"));
        QCOMPARE(result.artist, QString("Artist & artist")); QCOMPARE(result.album, QString("Album"));
        QVERIFY(result.artworkDataUrl.startsWith("data:image/png;base64,"));
        const auto image = QImage::fromData(QByteArray::fromBase64(result.artworkDataUrl.mid(22).toLatin1()));
        QVERIFY(!image.isNull()); QCOMPARE(image.pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(image.width() <= 256 && image.height() <= 256);
        QCOMPARE(file.pos(), qint64(17)); QCOMPARE(lseek(file.handle(), 0, SEEK_CUR), off_t(17));
        QVERIFY(file.seek(0)); QCOMPARE(file.readAll(), before);
        conflux::library::PreparedTrackStore store(directory.filePath("cache"));
        const auto first = store.prepare(path); const auto second = store.prepare(path);
        QCOMPARE(first.presentation, result); QCOMPARE(second.presentation, result); QVERIFY(second.cacheHit);
        QCOMPARE(first.audio.frames(), std::uint64_t(16));
    }

    void oversizedTagReadKeepsAudioPlayable()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("oversized-tag.flac");
        QVERIFY(presentationFixture::writeFlac(path, "bounded tags", {}, {}, QByteArray(8 * 1024 * 1024 + 1, 'x')));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.seek(19));
        const auto result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title, QString("oversized-tag.flac"));
        QVERIFY(result.artworkDataUrl.isEmpty());
        QCOMPARE(lseek(file.handle(), 0, SEEK_CUR), off_t(19));
        conflux::library::PreparedTrackStore store(directory.filePath("cache"));
        QCOMPARE(store.prepare(path).audio.frames(), std::uint64_t(16));
    }

    void missingTagsAndUnsupportedInputUseFilename()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("untagged.flac");
        QVERIFY(presentationFixture::writeFlac(path, {}, {}, {}, {}));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        auto result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title, QString("untagged.flac"));
        QVERIFY(result.artist.isEmpty() && result.album.isEmpty() && result.artworkDataUrl.isEmpty());
        const auto unknown = directory.filePath("unknown.bin");
        QFile data(unknown); QVERIFY(data.open(QIODevice::ReadWrite)); QCOMPARE(data.write("not audio"), qint64(9));
        result = readTrackPresentation(data.handle(), unknown);
        QCOMPARE(result.title, QString("unknown.bin")); QVERIFY(result.artworkDataUrl.isEmpty());
    }

    void frontCoverPreferenceAndJpeg()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("covers.flac");
        QVERIFY(presentationFixture::writeFlac(path, "title", {}, {}, {}));
        {
            TagLib::FLAC::File tags(QFile::encodeName(path).constData(), false);
            for (const bool front : {false, true}) {
                const auto bytes = presentationFixture::artwork(front ? Qt::green : Qt::red, 600, front ? "JPEG" : "PNG");
                auto* picture = new TagLib::FLAC::Picture;
                picture->setType(front ? TagLib::FLAC::Picture::FrontCover : TagLib::FLAC::Picture::BackCover);
                picture->setData(TagLib::ByteVector(bytes.constData(), static_cast<unsigned int>(bytes.size())));
                tags.addPicture(picture);
            }
            QVERIFY(tags.save());
        }
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto result = readTrackPresentation(file.handle(), path);
        const auto image = QImage::fromData(QByteArray::fromBase64(result.artworkDataUrl.mid(22).toLatin1()));
        QCOMPARE(image.width(), 256); QVERIFY(image.height() <= 256);
        QVERIFY(image.pixelColor(0, 0).green() > 240 && image.pixelColor(0, 0).red() < 15);
    }

    void corruptOversizedAndExternalArtworkFallsBack_data()
    {
        QTest::addColumn<QByteArray>("cover");
        QTest::newRow("malformed-png") << QByteArray("\x89PNG\r\n\x1a\ninvalid");
        QTest::newRow("external-url") << QByteArray("https://example.invalid/cover.png");
        QTest::newRow("svg") << QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
        QTest::newRow("dimension-limit") << presentationFixture::artwork(Qt::blue, 4097);
        auto oversized = presentationFixture::artwork(Qt::blue);
        oversized.append(QByteArray(4 * 1024 * 1024 + 1 - oversized.size(), '\0'));
        QVERIFY(!QImage::fromData(oversized).isNull()); // It would decode without the encoded byte bound.
        QTest::newRow("encoded-limit") << oversized;
    }
    void corruptOversizedAndExternalArtworkFallsBack()
    {
        QFETCH(QByteArray, cover);
        QTemporaryDir directory;
        const auto path = directory.filePath("invalid-cover.flac");
        QVERIFY(presentationFixture::writeFlac(path, "Retained title", "Artist", "Album", cover));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title, QString("Retained title"));
        QVERIFY(result.artworkDataUrl.isEmpty());
        conflux::library::PreparedTrackStore store(directory.filePath("cache"));
        QCOMPARE(store.prepare(path).audio.frames(), std::uint64_t(16));
    }

    void boundedLiteralLabelsAndCancelledRead()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("labels.flac");
        QVERIFY(presentationFixture::writeFlac(path, QString(2000, 'T'), "line\nartist\tname", "<img src=x>", {}));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.seek(11));
        const auto result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title.size(), qsizetype(256)); QCOMPARE(result.artist, QString("line artist name"));
        QCOMPARE(result.album, QString("<img src=x>"));
        QCOMPARE(lseek(file.handle(), 0, SEEK_CUR), off_t(11));
        std::stop_source cancellation; cancellation.request_stop();
        QVERIFY_EXCEPTION_THROWN(readTrackPresentation(file.handle(), path, cancellation.get_token()), std::runtime_error);
        QCOMPARE(file.pos(), qint64(11));
        QVERIFY_EXCEPTION_THROWN(readTrackPresentation(-1, path), std::invalid_argument);
        const int directoryFd = open(QFile::encodeName(directory.path()).constData(), O_RDONLY | O_DIRECTORY);
        QVERIFY(directoryFd >= 0);
        QVERIFY_EXCEPTION_THROWN(readTrackPresentation(directoryFd, path), std::invalid_argument);
        close(directoryFd);
    }

    void id3ArtworkAndTags()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("id3.mp3");
        {
            TagLib::ID3v2::Tag tags;
            tags.setTitle("ID3 title"); tags.setArtist("ID3 artist"); tags.setAlbum("ID3 album");
            auto* picture = new TagLib::ID3v2::AttachedPictureFrame;
            picture->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover); picture->setMimeType("image/png");
            const auto bytes = presentationFixture::artwork(Qt::red);
            picture->setPicture(TagLib::ByteVector(bytes.constData(), static_cast<unsigned int>(bytes.size())));
            tags.addFrame(picture);
            const auto bytesTag = tags.render();
            QVERIFY(writeMpeg(path, QByteArray(bytesTag.data(), qsizetype(bytesTag.size()))));
        }
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title, QString("ID3 title")); QCOMPARE(result.artist, QString("ID3 artist"));
        const auto image = QImage::fromData(QByteArray::fromBase64(result.artworkDataUrl.mid(22).toLatin1()));
        QVERIFY(!image.isNull()); QCOMPARE(image.pixelColor(0, 0), QColor(Qt::red));
    }

    void compressedId3NeverInflatesAndNestedFramesAreSkipped()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("compressed.mp3");
        const auto text = QByteArray(1, '\0') + "Inflated title";
        const auto compressed = integerBytes(unsigned(text.size())) + qCompress(text).mid(4);
        QVERIFY(writeMpeg(path, id3Tag(id3Frame("TIT2", compressed, true))));
        {
            // A valid, tiny compressed fixture reaches ordinary upstream inflation;
            // the presentation reader must deliberately decline that same title.
            TagLib::MPEG::File ordinary(QFile::encodeName(path).constData(), false);
            QVERIFY(ordinary.isValid()); QCOMPARE(QString::fromStdString(ordinary.tag()->title().to8Bit(true)), QString("Inflated title"));
        }
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        auto result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title, QString("compressed.mp3")); QVERIFY(result.artworkDataUrl.isEmpty()); file.close();

        QByteArray nested = id3Frame("TIT2", compressed, true);
        for (unsigned depth = 0; depth < 1000; ++depth)
            nested = id3Frame("CHAP", QByteArray("chapter\0", 8) + QByteArray(16, '\0') + nested);
        const auto literal = id3Frame("TIT2", QByteArray(1, '\0') + "Safe outer title");
        QVERIFY(writeMpeg(path, id3Tag(nested + literal)));
        QVERIFY(file.open(QIODevice::ReadOnly));
        result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title, QString("Safe outer title")); QVERIFY(result.artworkDataUrl.isEmpty());
    }

    void boundedReadOperationsAndUnsupportedWaveFallback()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("many-blocks.flac");
        QVERIFY(presentationFixture::writeFlac(path));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll(); file.close(); QVERIFY(original.startsWith("fLaC"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        // Keep mandatory STREAMINFO first; add valid tiny PADDING blocks before
        // the existing comments/picture, testing operation count rather than bytes.
        const auto padded = original.first(42) + QByteArray::fromHex("0100000100").repeated(300) + original.mid(42);
        QCOMPARE(file.write(padded), padded.size()); file.close();
        {
            TagLib::FLAC::File ordinary(QFile::encodeName(path).constData(), false);
            QCOMPARE(QString::fromStdString(ordinary.tag()->title().to8Bit(true)), QString("Tagged title"));
        }
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title, QString("many-blocks.flac")); QVERIFY(result.artworkDataUrl.isEmpty());
        conflux::library::PreparedTrackStore store(directory.filePath("cache"));
        QCOMPARE(store.prepare(path).audio.frames(), std::uint64_t(16));

        const auto wavePath = directory.filePath("unsupported-tags.wav");
        SF_INFO info{}; info.samplerate = 48000; info.channels = 2; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        auto* audio = sf_open(QFile::encodeName(wavePath).constData(), SFM_WRITE, &info);
        QVERIFY(audio); const float pcm[16]{}; QCOMPARE(sf_writef_float(audio, pcm, 8), sf_count_t(8)); QCOMPARE(sf_close(audio), 0);
        QVERIFY(presentationFixture::tagWave(wavePath, "Optional WAV title"));
        QFile wave(wavePath); QVERIFY(wave.open(QIODevice::ReadOnly));
        const auto fallback = readTrackPresentation(wave.handle(), wavePath);
        QCOMPARE(fallback.title, QString("unsupported-tags.wav"));
        QVERIFY(fallback.artist.isEmpty() && fallback.album.isEmpty() && fallback.artworkDataUrl.isEmpty());
        QCOMPARE(store.prepare(wavePath).audio.frames(), std::uint64_t(8));
    }

    void xiphTagsAndEmbeddedCover_data()
    {
        QTest::addColumn<bool>("opus");
        QTest::newRow("vorbis") << false; QTest::newRow("opus") << true;
    }
    void xiphTagsAndEmbeddedCover()
    {
        QFETCH(bool, opus);
        QTemporaryDir directory;
        const auto path = directory.filePath(opus ? "tagged.opus" : "tagged.ogg");
        SF_INFO info{}; info.samplerate = 48000; info.channels = 2;
        info.format = SF_FORMAT_OGG | (opus ? SF_FORMAT_OPUS : SF_FORMAT_VORBIS);
        auto* audio = sf_open(QFile::encodeName(path).constData(), SFM_WRITE, &info);
        QVERIFY(audio); const float pcm[32]{}; QCOMPARE(sf_writef_float(audio, pcm, 16), sf_count_t(16)); QCOMPARE(sf_close(audio), 0);
        {
            std::unique_ptr<TagLib::File> tags;
            if (opus) tags = std::make_unique<TagLib::Ogg::Opus::File>(QFile::encodeName(path).constData(), false);
            else tags = std::make_unique<TagLib::Ogg::Vorbis::File>(QFile::encodeName(path).constData(), false);
            auto* xiph = dynamic_cast<TagLib::Ogg::XiphComment*>(tags->tag());
            QVERIFY(xiph); xiph->setTitle("Xiph title"); xiph->setArtist("Xiph artist"); xiph->setAlbum("Xiph album");
            auto* picture = new TagLib::FLAC::Picture;
            const auto bytes = presentationFixture::artwork(Qt::yellow);
            picture->setData(TagLib::ByteVector(bytes.constData(), static_cast<unsigned int>(bytes.size())));
            picture->setType(TagLib::FLAC::Picture::FrontCover); picture->setMimeType("image/png");
            xiph->addPicture(picture); QVERIFY(tags->save());
        }
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto result = readTrackPresentation(file.handle(), path);
        QCOMPARE(result.title, QString("Xiph title")); QCOMPARE(result.artist, QString("Xiph artist"));
        QCOMPARE(result.album, QString("Xiph album"));
        const auto image = QImage::fromData(QByteArray::fromBase64(result.artworkDataUrl.mid(22).toLatin1()));
        QVERIFY(!image.isNull()); QCOMPARE(image.pixelColor(0, 0), QColor(Qt::yellow));
    }

    void sourceChangesAndFinalCancellationReject()
    {
        for (const bool cancel : {false, true}) {
            QTemporaryDir directory;
            const auto path = directory.filePath("changed.flac");
            QVERIFY(presentationFixture::writeFlac(path));
            QFile reader(path), writer(path);
            QVERIFY(reader.open(QIODevice::ReadOnly)); QVERIFY(writer.open(QIODevice::ReadWrite));
            QVERIFY(reader.seek(13));
            std::stop_source stop;
            ScopedInjection fault(reader.handle(), cancel ? -1 : writer.handle(), cancel ? &stop : nullptr);
            QVERIFY_EXCEPTION_THROWN(readTrackPresentation(reader.handle(), path, stop.get_token()), std::runtime_error);
            QCOMPARE(injection.calls, 2U);
            QCOMPARE(reader.pos(), qint64(13));
            QCOMPARE(lseek(reader.handle(), 0, SEEK_CUR), off_t(13));
            if (cancel) QVERIFY(stop.stop_requested()); else QCOMPARE(writer.size(), qint64(1));
        }
    }
};
} // namespace
extern "C" int __real_fstat(int, struct stat*);
extern "C" int __real_fstat64(int, struct stat64*);
extern "C" int __wrap_fstat(int fd, struct stat* value)
{ return injectedStat(fd, value, __real_fstat); }
extern "C" int __wrap_fstat64(int fd, struct stat64* value)
{ return injectedStat(fd, value, __real_fstat64); }
QTEST_GUILESS_MAIN(PresentationTest)
#include "track_presentation_test.moc"

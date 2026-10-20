// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <sndfile.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/wavfile.h>
#include <vector>

namespace presentationFixture {
inline QByteArray artwork(const QColor& color, int width = 48, const char* format = "PNG")
{
    QImage image(width, 40, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, format)) return {};
    return bytes;
}

inline bool writeFlac(const QString& path, const QString& title = "Tagged title",
                      const QString& artist = "Tagged artist", const QString& album = "Tagged album",
                      const QByteArray& cover = artwork(Qt::blue), int rate = 48000, unsigned frames = 16)
{
    SF_INFO info{};
    info.samplerate = rate; info.channels = 2; info.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
    auto* audio = sf_open(QFile::encodeName(path).constData(), SFM_WRITE, &info);
    if (!audio) return false;
    std::vector<short> samples(std::size_t(frames) * 2);
    for (std::size_t index = 0; index < samples.size(); ++index) samples[index] = short(index * 17 - 32);
    const bool wrote = sf_writef_short(audio, samples.data(), frames) == sf_count_t(frames);
    const bool closed = sf_close(audio) == 0;
    if (!wrote || !closed) return false;
    TagLib::FLAC::File file(QFile::encodeName(path).constData(), false);
    if (!file.isValid() || !file.tag()) return false;
    const auto text = [](const QString& value) { return TagLib::String(value.toUtf8().constData(), TagLib::String::UTF8); };
    file.tag()->setTitle(text(title)); file.tag()->setArtist(text(artist)); file.tag()->setAlbum(text(album));
    if (!cover.isEmpty()) {
        auto* picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/png");
        picture->setData(TagLib::ByteVector(cover.constData(), static_cast<unsigned int>(cover.size())));
        file.addPicture(picture);
    }
    return file.save();
}

inline bool tagWave(const QString& path, const QString& title, const QColor& color = Qt::blue)
{
    TagLib::RIFF::WAV::File file(QFile::encodeName(path).constData(), false);
    if (!file.isValid()) return false;
    file.ID3v2Tag()->setTitle(TagLib::String(title.toUtf8().constData(), TagLib::String::UTF8));
    file.ID3v2Tag()->setArtist("Generated artist"); file.ID3v2Tag()->setAlbum("Generated album");
    auto* picture = new TagLib::ID3v2::AttachedPictureFrame;
    picture->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover); picture->setMimeType("image/png");
    const auto bytes = artwork(color);
    picture->setPicture(TagLib::ByteVector(bytes.constData(), static_cast<unsigned int>(bytes.size())));
    file.ID3v2Tag()->addFrame(picture);
    return file.save();
}
} // namespace presentationFixture

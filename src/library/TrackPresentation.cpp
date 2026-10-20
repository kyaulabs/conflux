// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/TrackPresentation.hpp"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <taglib/tfilestream.h>
#include <taglib/tag.h>
#include <taglib/flacfile.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/id3v2framefactory.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/xiphcomment.h>
#include <taglib/vorbisfile.h>
#include <taglib/opusfile.h>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace conflux::library {
namespace {
constexpr std::size_t maximumRead = 8U * 1024 * 1024, maximumTotalRead = 16U * 1024 * 1024;
constexpr unsigned maximumReadOperations = 256, maximumFrames = 256;
constexpr unsigned maximumEncodedArt = 4U * 1024 * 1024;
constexpr int maximumLabel = 256, maximumImageDimension = 4096, thumbnailDimension = 256;
void checkStop(std::stop_token stop)
{
    if (stop.stop_requested()) throw std::runtime_error("Track metadata preparation cancelled");
}
struct stat status(int descriptor)
{
    struct stat result{};
    if (fstat(descriptor, &result) != 0 || !S_ISREG(result.st_mode))
        throw std::invalid_argument("Track metadata requires a retained regular source");
    return result;
}
bool unchanged(const struct stat& a, const struct stat& b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
QString label(QString value)
{
    value = value.left(maximumLabel);
    if (!value.isEmpty() && value.back().isHighSurrogate()) value.chop(1);
    for (auto& character : value)
        if (character.category() == QChar::Other_Control) character = QChar(' ');
    return value.simplified();
}
QString label(const TagLib::String& value)
{
    // Bound the conversion too, rather than first copying an arbitrarily long tag.
    const auto bytes = value.substr(0, maximumLabel).to8Bit(true);
    return label(QString::fromUtf8(bytes.data(), qsizetype(bytes.size())));
}
class TagStream final : public TagLib::FileStream {
public:
    TagStream(int descriptor, QByteArray name, std::stop_token stop)
        : FileStream(descriptor, true), name_(std::move(name)), stop_(stop) {}
    TagLib::FileName name() const override { return name_.constData(); }
    TagLib::ByteVector readBlock(std::size_t length) override
    {
        if (exhausted_ || stop_.stop_requested() || ++operations_ > maximumReadOperations ||
            length > maximumRead || length > maximumTotalRead - read_) {
            exhausted_ = true;
            return {}; // Let old TagLib constructors release their partial objects normally.
        }
        read_ += length;
        auto bytes = FileStream::readBlock(length);
        if (stop_.stop_requested()) { exhausted_ = true; return {}; }
        return bytes;
    }
    bool exhausted() const { return exhausted_; }
private:
    QByteArray name_;
    std::stop_token stop_;
    std::size_t read_ = 0;
    unsigned operations_ = 0;
    bool exhausted_ = false;
};

// Old TagLib factories offer this per-file virtual hook before inflation and
// recursive CHAP/CTOC parsing. Never change its process-wide default factory.
class PresentationFrames final : public TagLib::ID3v2::FrameFactory {
public:
    explicit PresentationFrames(std::stop_token stop) : stop_(stop) {}
    bool rejected() const { return rejected_; }
protected:
    bool updateFrame(TagLib::ID3v2::Frame::Header* header) const override
    {
        const auto id = header->frameID();
        const bool wanted = id == "TIT2" || id == "TPE1" || id == "TALB" || id == "APIC" ||
                            id == "TT2" || id == "TP1" || id == "TAL" || id == "PIC";
        const bool abort = header->compression() || stop_.stop_requested() || ++frames_ > maximumFrames;
        if (abort || !wanted) {
            // UnknownFrame otherwise still inflates compressed bodies. Make a
            // read-only inert header; preserve size only when safely skipping an
            // ordinary irrelevant frame. On rejection consume the rest of the
            // at-most8MiB tag, including on versions that tolerate empty frames.
            const auto size = abort ? static_cast<unsigned>(maximumRead) : header->frameSize();
            header->setData(TagLib::ByteVector(10, '\0'), header->version());
            header->setFrameID("XXXX"); header->setFrameSize(size);
            rejected_ |= abort;
            return false;
        }
        return FrameFactory::updateFrame(header);
    }
private:
    std::stop_token stop_;
    mutable unsigned frames_ = 0;
    mutable bool rejected_ = false;
};

std::unique_ptr<TagLib::File> tags(TagStream& stream, const QString& suffix, PresentationFrames& factory)
{
    // WAV/AIFF ID3 parsing cannot use this bounded factory on TagLib1.13.
    // Use the same explicit supported set on every version, never FileRef's
    // generic fallback into an unbounded or presentation-irrelevant parser.
#if TAGLIB_MAJOR_VERSION >= 2
    if (suffix == "flac") return std::make_unique<TagLib::FLAC::File>(&stream, false, TagLib::AudioProperties::Average, &factory);
    if (suffix == "mp3" || suffix == "mp2") return std::make_unique<TagLib::MPEG::File>(&stream, false, TagLib::AudioProperties::Fast, &factory);
#else
    if (suffix == "flac") return std::make_unique<TagLib::FLAC::File>(&stream, &factory, false);
    if (suffix == "mp3" || suffix == "mp2") return std::make_unique<TagLib::MPEG::File>(&stream, &factory, false, TagLib::AudioProperties::Fast);
#endif
    if (suffix == "ogg" || suffix == "oga") return std::make_unique<TagLib::Ogg::Vorbis::File>(&stream, false);
    if (suffix == "opus") return std::make_unique<TagLib::Ogg::Opus::File>(&stream, false);
    return {};
}
QString thumbnail(const TagLib::ByteVector& data, std::stop_token stop)
{
    checkStop(stop);
    if (data.isEmpty() || data.size() > maximumEncodedArt) return {};
    const auto bytes = QByteArray::fromRawData(data.data(), qsizetype(data.size()));
    const QByteArray format = bytes.startsWith("\x89PNG\r\n\x1a\n") ? "png" :
        bytes.startsWith("\xff\xd8\xff") ? "jpeg" : "";
    if (format.isEmpty()) return {}; // No URLs, SVGs, animations or external resources.
    QBuffer input;
    input.setData(bytes);
    if (!input.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&input, format);
    reader.setAutoDetectImageFormat(false);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > maximumImageDimension || size.height() > maximumImageDimension)
        return {};
    reader.setScaledSize(size.scaled(thumbnailDimension, thumbnailDimension, Qt::KeepAspectRatio));
    auto image = reader.read();
    checkStop(stop); // Decoding is bounded by encoded bytes and original dimensions.
    if (image.isNull()) return {};
    image = image.scaled(thumbnailDimension, thumbnailDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer output(&png);
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || png.size() > 256 * 1024)
        return {};
    checkStop(stop);
    return "data:image/png;base64," + QString::fromLatin1(png.toBase64());
}
QString pictures(const TagLib::List<TagLib::FLAC::Picture*>& list, std::stop_token stop)
{
    // Prefer a decodable front cover; deterministic fallback to another embedded picture.
    for (const bool front : {true, false}) {
        unsigned checked = 0;
        for (const auto* picture : list) {
            checkStop(stop);
            if (++checked > 32) break;
            if (!picture || (picture->type() == TagLib::FLAC::Picture::FrontCover) != front) continue;
            if (auto result = thumbnail(picture->data(), stop); !result.isEmpty()) return result;
        }
    }
    return {};
}
QString pictures(TagLib::ID3v2::Tag* tag, std::stop_token stop)
{
    if (!tag) return {};
    const auto& list = tag->frameList("APIC");
    for (const bool front : {true, false}) {
        unsigned checked = 0;
        for (const auto* frame : list) {
            checkStop(stop);
            if (++checked > 32) break;
            const auto* picture = dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(frame);
            if (!picture || picture->mimeType() == "-->" ||
                (picture->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) != front) continue;
            if (auto result = thumbnail(picture->picture(), stop); !result.isEmpty()) return result;
        }
    }
    return {};
}
QString artwork(TagLib::File* file, std::stop_token stop)
{
    if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(file)) return pictures(flac->pictureList(), stop);
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file)) return pictures(mpeg->ID3v2Tag(), stop);
    if (auto* xiph = dynamic_cast<TagLib::Ogg::XiphComment*>(file->tag())) return pictures(xiph->pictureList(), stop);
    return {};
}
} // namespace

TrackPresentation readTrackPresentation(int descriptor, const QString& sourcePath, std::stop_token stop)
{
    checkStop(stop);
    const auto before = status(descriptor);
    const auto offset = lseek(descriptor, 0, SEEK_CUR);
    if (offset < 0) throw std::runtime_error("Cannot retain metadata source position");
    struct RestoreOffset {
        int descriptor; off_t offset;
        ~RestoreOffset() { (void)lseek(descriptor, offset, SEEK_SET); }
    } restore{descriptor, offset};
    TrackPresentation result{label(QFileInfo(sourcePath).fileName()), {}, {}, {}};
    const auto encodedName = QFile::encodeName(sourcePath);
    struct Duplicate {
        int descriptor;
        ~Duplicate() { if (descriptor >= 0) close(descriptor); }
    } duplicate{fcntl(descriptor, F_DUPFD_CLOEXEC, 0)};
    if (duplicate.descriptor < 0) throw std::runtime_error("Cannot retain metadata source descriptor");
    try {
        TagStream stream(duplicate.descriptor, encodedName, stop);
        if (!stream.isOpen()) throw std::runtime_error("Cannot open metadata source stream");
        duplicate.descriptor = -1; // Transfer only after successful construction/open.
        stream.seek(0);
        PresentationFrames factory(stop);
        auto file = tags(stream, QFileInfo(sourcePath).suffix().toLower(), factory);
        checkStop(stop);
        if (!stream.exhausted() && !factory.rejected() && file && file->isValid() && file->tag()) {
            if (auto title = label(file->tag()->title()); !title.isEmpty()) result.title = std::move(title);
            result.artist = label(file->tag()->artist());
            result.album = label(file->tag()->album());
            result.artworkDataUrl = artwork(file.get(), stop);
        }
    } catch (const std::bad_alloc&) { throw; }
      catch (const std::exception&) {
        checkStop(stop); // Optional malformed/oversized tags never prevent ordinary playback.
    }
    if (!unchanged(before, status(descriptor))) throw std::runtime_error("Audio source changed while reading metadata");
    checkStop(stop);
    return result;
}
} // namespace conflux::library

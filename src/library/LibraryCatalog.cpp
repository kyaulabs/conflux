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
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace conflux::library {
namespace {
constexpr std::uint64_t maximumFrames = audio::PreparedAudio::defaultMaxPreparedBytes / (2 * sizeof(float));

class Descriptor {
public:
    explicit Descriptor(int value) noexcept : value_(value) {}
    ~Descriptor() { if (value_ >= 0) close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return value_; }
private:
    int value_;
};

bool digest(const QString& text)
{
    return text.size() == 64 && std::ranges::all_of(text, [](QChar c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void validateTrack(const LibraryTrack& track)
{
    if (track.sourcePath.size() > qsizetype(LibraryCatalog::maxBytes) || track.name.size() > qsizetype(LibraryCatalog::maxBytes))
        throw std::length_error("Library catalog byte budget exceeded");
    if (!digest(track.sourceSha256) || !digest(track.artifactKey) || !QFileInfo(track.sourcePath).isAbsolute() ||
        track.sourcePath.contains(QChar(0)) || track.name.isEmpty() || track.name == "." || track.name == ".." ||
        track.name != QFileInfo(track.sourcePath).fileName() || !track.frames || track.frames > maximumFrames)
        throw std::invalid_argument("Invalid library track identity, path, name or frame count");
}

struct stat statusOf(int descriptor)
{
    struct stat result{};
    if (fstat(descriptor, &result) != 0)
        throw std::runtime_error("Cannot inspect library catalog");
    return result;
}

bool unchanged(const struct stat& before, const struct stat& after)
{
    // Atomic replacement can unlink this open inode and change its ctime;
    // the retained descriptor still contains the complete previous document.
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
}

void refreshAvailability(std::vector<LibraryTrack>& tracks)
{
    for (auto& track : tracks) track.available = QFileInfo(track.sourcePath).isFile();
}

QByteArray encode(const std::vector<LibraryTrack>& tracks)
{
    QJsonArray rows;
    for (const auto& track : tracks)
        rows.append(QJsonObject{{"source_sha256", track.sourceSha256}, {"source_path", track.sourcePath},
            {"name", track.name}, {"artifact_key", track.artifactKey}, {"frames", double(track.frames)}});
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"tracks", rows}}).toJson(QJsonDocument::Compact);
    if (std::size_t(bytes.size()) > LibraryCatalog::maxBytes)
        throw std::length_error("Library catalog byte budget exceeded");
    return bytes;
}

[[noreturn]] void saveError(const QSaveFile& file)
{
    throw std::runtime_error("Cannot save catalog: " + file.errorString().toStdString());
}
} // namespace

LibraryCatalog::LibraryCatalog(QString absoluteFilePath) : filePath_(std::move(absoluteFilePath))
{
    if (!QFileInfo(filePath_).isAbsolute() || filePath_.contains(QChar(0)) || QFileInfo(filePath_).fileName().isEmpty())
        throw std::invalid_argument("Library catalog requires an absolute file path");
}

std::vector<LibraryTrack> LibraryCatalog::read() const
{
    Descriptor file(open(QFile::encodeName(filePath_).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) {
        if (errno == ENOENT) return {};
        if (errno == ELOOP) throw std::runtime_error("unsafe catalog file: symbolic link");
        throw std::runtime_error(std::string("Cannot open library catalog: ") + std::strerror(errno));
    }
    const auto before = statusOf(file.get());
    if (!S_ISREG(before.st_mode) || before.st_size < 0)
        throw std::runtime_error("unsafe catalog file: not a regular file");
    if (std::uint64_t(before.st_size) > maxBytes)
        throw std::length_error("Library catalog byte budget exceeded");
    QByteArray bytes(qsizetype(before.st_size), Qt::Uninitialized);
    std::size_t position = 0;
    while (position < std::size_t(bytes.size())) {
        const auto wanted = std::min<std::size_t>(64U * 1024, std::size_t(bytes.size()) - position);
        const auto count = pread(file.get(), bytes.data() + position, wanted, static_cast<off_t>(position));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Library catalog changed or could not be read completely");
        position += std::size_t(count);
    }
    if (!unchanged(before, statusOf(file.get()))) throw std::runtime_error("Library catalog changed while reading");
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) throw std::runtime_error("Malformed library catalog document");
    const auto root = document.object();
    if (root.size() != 2 || root.value("version") != QJsonValue(1) || !root.value("tracks").isArray())
        throw std::runtime_error("Malformed or unsupported library catalog version");
    const auto rows = root.value("tracks").toArray();
    if (std::size_t(rows.size()) > maxEntries) throw std::length_error("Library catalog entry limit exceeded");
    std::vector<LibraryTrack> tracks;
    tracks.reserve(std::size_t(rows.size()));
    QSet<QString> identities, paths;
    for (const auto row : rows) {
        if (!row.isObject()) throw std::runtime_error("Malformed library catalog track");
        const auto object = row.toObject();
        if (object.size() != 5 || !object.value("source_sha256").isString() || !object.value("source_path").isString() ||
            !object.value("name").isString() || !object.value("artifact_key").isString() || !object.value("frames").isDouble())
            throw std::runtime_error("Malformed library catalog track fields");
        const auto frames = object.value("frames").toDouble();
        if (!std::isfinite(frames) || frames < 1 || frames > double(maximumFrames) || std::floor(frames) != frames)
            throw std::runtime_error("Malformed library catalog frame count");
        LibraryTrack track{object.value("source_sha256").toString(), object.value("source_path").toString(),
            object.value("name").toString(), object.value("artifact_key").toString(), std::uint64_t(frames), false};
        try { validateTrack(track); }
        catch (const std::invalid_argument& error) {
            throw std::runtime_error(std::string("Malformed library catalog track: ") + error.what());
        }
        if (identities.contains(track.sourceSha256) || paths.contains(track.sourcePath))
            throw std::runtime_error("Duplicate library catalog identity or path");
        identities.insert(track.sourceSha256);
        paths.insert(track.sourcePath);
        tracks.push_back(std::move(track));
    }
    refreshAvailability(tracks);
    return tracks;
}

std::vector<LibraryTrack> LibraryCatalog::upsert(const LibraryTrack& track)
{
    validateTrack(track);
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        throw std::runtime_error("Cannot save catalog directory");
    QLockFile lock(filePath_ + ".lock");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!lock.tryLock(0)) {
        if (lock.error() != QLockFile::LockFailedError) throw std::runtime_error("Cannot lock library catalog");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Library catalog is busy");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto existing = read();
    std::vector<LibraryTrack> merged;
    merged.reserve(std::min(maxEntries, existing.size() + 1));
    std::size_t position = maxEntries;
    for (const auto& value : existing) {
        if (value.sourceSha256 == track.sourceSha256 || value.sourcePath == track.sourcePath) {
            if (position == maxEntries) position = merged.size();
        } else {
            merged.push_back(value);
        }
    }
    if (merged.size() == maxEntries) throw std::length_error("Library catalog entry limit exceeded");
    if (position == maxEntries) position = merged.size();
    merged.insert(merged.begin() + static_cast<std::ptrdiff_t>(position), track);
    const auto bytes = encode(merged);
    QSaveFile file(filePath_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) saveError(file);
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) saveError(file);
    // Qt 6.4 can miss buffered flush errors inside commit(). Detect them
    // before the temporary file can replace the existing catalog.
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        file.cancelWriting();
        saveError(file);
    }
    if (!file.commit()) saveError(file);
    refreshAvailability(merged);
    return merged;
}

} // namespace conflux::library

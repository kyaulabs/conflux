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
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace conflux::library {
namespace {
constexpr std::uint64_t maximumFrames = audio::PreparedAudio::defaultMaxPreparedBytes / (2 * sizeof(float));
constexpr auto levelsRecipe = "prepared-stereo-f32-48000-levels-v1";

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

void validateCues(const std::vector<CuePoint>& cues, std::uint64_t frames)
{
    if (cues.size() > 99) throw std::length_error("Library cue limit exceeded");
    int previousTrack = 0;
    std::uint64_t previousFrame = 0;
    for (const auto& cue : cues) {
        if (cue.trackNumber <= previousTrack || cue.trackNumber > 99 || cue.label.isEmpty() ||
            cue.label.size() > 256 || cue.frame >= frames ||
            (previousTrack && cue.frame <= previousFrame) ||
            std::ranges::any_of(cue.label, [](QChar c) { return c.category() == QChar::Other_Control; }))
            throw std::invalid_argument("Invalid library cue label, track number or frame");
        previousTrack = cue.trackNumber;
        previousFrame = cue.frame;
    }
}

void validateLevels(const std::optional<audio::SignalLevels>& levels, std::uint64_t frames)
{
    if (!levels) return;
    const auto [rms, peak] = *levels;
    if (!std::isfinite(rms) || !std::isfinite(peak) || rms < 0 || peak < 0 || rms > peak ||
        peak > double(std::numeric_limits<float>::max()) || ((rms == 0) != (peak == 0)) ||
        double(static_cast<float>(peak)) != peak)
        throw std::invalid_argument("Invalid library signal levels");
    // At least one of the two-channel samples reached the advertised peak.
    // Allow only final double-rounding error at this arithmetic lower bound.
    const auto minimumRms = peak / std::sqrt(double(frames) * 2);
    if (rms < minimumRms * (1 - 8 * std::numeric_limits<double>::epsilon()))
        throw std::invalid_argument("Invalid library signal levels for the frame count");
}

void validateTrack(const LibraryTrack& track)
{
    if (track.sourcePath.size() > qsizetype(LibraryCatalog::maxBytes) || track.name.size() > qsizetype(LibraryCatalog::maxBytes))
        throw std::length_error("Library catalog byte budget exceeded");
    if (!digest(track.sourceSha256) || !digest(track.artifactKey) || !QFileInfo(track.sourcePath).isAbsolute() ||
        track.sourcePath.contains(QChar(0)) || track.name.isEmpty() || track.name == "." || track.name == ".." ||
        track.name != QFileInfo(track.sourcePath).fileName() || !track.frames || track.frames > maximumFrames)
        throw std::invalid_argument("Invalid library track identity, path, name or frame count");
    validateCues(track.cues, track.frames);
    validateLevels(track.levels, track.frames);
    if (track.savedCueFrame && *track.savedCueFrame >= track.frames)
        throw std::invalid_argument("Invalid saved library cue frame");
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
    for (const auto& track : tracks) {
        QJsonArray cues;
        for (const auto& cue : track.cues)
            cues.append(QJsonObject{{"track", cue.trackNumber}, {"label", cue.label}, {"frame", double(cue.frame)}});
        const auto levels = track.levels ? QJsonValue(QJsonObject{{"recipe", levelsRecipe},
            {"rms", track.levels->rms}, {"sample_peak", track.levels->samplePeak}}) : QJsonValue(QJsonValue::Null);
        const auto savedCue = track.savedCueFrame ? QJsonValue(double(*track.savedCueFrame)) : QJsonValue(QJsonValue::Null);
        rows.append(QJsonObject{{"source_sha256", track.sourceSha256}, {"source_path", track.sourcePath},
            {"name", track.name}, {"artifact_key", track.artifactKey}, {"frames", double(track.frames)},
            {"cues", cues}, {"levels", levels}, {"saved_cue_frame", savedCue}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"version", 4}, {"tracks", rows}}).toJson(QJsonDocument::Compact);
    if (std::size_t(bytes.size()) > LibraryCatalog::maxBytes)
        throw std::length_error("Library catalog byte budget exceeded");
    return bytes;
}

[[noreturn]] void saveError(const QSaveFile& file)
{
    throw std::runtime_error("Cannot save catalog: " + file.errorString().toStdString());
}

std::unique_ptr<QLockFile> lockCatalog(const QString& path)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        throw std::runtime_error("Cannot save catalog directory");
    auto lock = std::make_unique<QLockFile>(path + ".lock");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!lock->tryLock(0)) {
        if (lock->error() != QLockFile::LockFailedError) throw std::runtime_error("Cannot lock library catalog");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Library catalog is busy");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return lock;
}

void save(const QString& path, const std::vector<LibraryTrack>& tracks)
{
    const auto bytes = encode(tracks);
    QSaveFile file(path);
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
    const bool withSavedCue = root.value("version") == QJsonValue(4);
    const bool withLevels = withSavedCue || root.value("version") == QJsonValue(3);
    const bool withCues = withLevels || root.value("version") == QJsonValue(2);
    if (root.size() != 2 || (!withCues && root.value("version") != QJsonValue(1)) || !root.value("tracks").isArray())
        throw std::runtime_error("Malformed or unsupported library catalog version");
    const auto rows = root.value("tracks").toArray();
    if (std::size_t(rows.size()) > maxEntries) throw std::length_error("Library catalog entry limit exceeded");
    std::vector<LibraryTrack> tracks;
    tracks.reserve(std::size_t(rows.size()));
    QSet<QString> identities, paths;
    for (const auto row : rows) {
        if (!row.isObject()) throw std::runtime_error("Malformed library catalog track");
        const auto object = row.toObject();
        if (object.size() != (withSavedCue ? 8 : withLevels ? 7 : withCues ? 6 : 5) || !object.value("source_sha256").isString() || !object.value("source_path").isString() ||
            !object.value("name").isString() || !object.value("artifact_key").isString() || !object.value("frames").isDouble())
            throw std::runtime_error("Malformed library catalog track fields");
        const auto frames = object.value("frames").toDouble();
        if (!std::isfinite(frames) || frames < 1 || frames > double(maximumFrames) || std::floor(frames) != frames)
            throw std::runtime_error("Malformed library catalog frame count");
        LibraryTrack track{object.value("source_sha256").toString(), object.value("source_path").toString(),
            object.value("name").toString(), object.value("artifact_key").toString(), std::uint64_t(frames), false};
        if (withCues) {
            if (!object.value("cues").isArray()) throw std::runtime_error("Malformed library cues");
            const auto cues = object.value("cues").toArray();
            if (cues.size() > 99) throw std::length_error("Library cue limit exceeded");
            for (const auto cue : cues) {
                if (!cue.isObject()) throw std::runtime_error("Malformed library cue");
                const auto item = cue.toObject();
                const auto number = item.value("track").toDouble(-1), frame = item.value("frame").toDouble(-1);
                if (item.size() != 3 || !item.value("track").isDouble() || !item.value("label").isString() ||
                    !item.value("frame").isDouble() || !std::isfinite(number) || number < 1 || number > 99 ||
                    std::floor(number) != number || !std::isfinite(frame) || frame < 0 || frame >= frames || std::floor(frame) != frame)
                    throw std::runtime_error("Malformed library cue fields");
                track.cues.push_back({int(number), item.value("label").toString(), std::uint64_t(frame)});
            }
        }
        if (withLevels && !object.value("levels").isNull()) {
            if (!object.value("levels").isObject()) throw std::runtime_error("Malformed library catalog levels");
            const auto levels = object.value("levels").toObject();
            if (levels.size() != 3 || levels.value("recipe") != QJsonValue(levelsRecipe) ||
                !levels.value("rms").isDouble() || !levels.value("sample_peak").isDouble())
                throw std::runtime_error("Malformed library catalog level fields");
            track.levels = audio::SignalLevels{levels.value("rms").toDouble(), levels.value("sample_peak").toDouble()};
        }
        if (withSavedCue && !object.value("saved_cue_frame").isNull()) {
            const auto cue = object.value("saved_cue_frame");
            const auto frame = cue.toDouble(-1);
            if (!cue.isDouble() || !std::isfinite(frame) || frame < 0 || frame >= frames || std::floor(frame) != frame)
                throw std::runtime_error("Malformed library catalog saved cue frame");
            track.savedCueFrame = std::uint64_t(frame);
        }
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
    auto replacement = track;
    replacement.cues.clear(); // Only dedicated updates install source-bound cues.
    replacement.savedCueFrame.reset();
    validateTrack(replacement);
    const auto lock = lockCatalog(filePath_);
    const auto existing = read();
    std::vector<LibraryTrack> merged;
    merged.reserve(std::min(maxEntries, existing.size() + 1));
    std::size_t position = maxEntries;
    for (const auto& value : existing) {
        if (value.sourceSha256 == track.sourceSha256 || value.sourcePath == track.sourcePath) {
            if (position == maxEntries) position = merged.size();
            if (value.sourceSha256 == track.sourceSha256) {
                replacement.cues = value.cues;
                validateCues(replacement.cues, replacement.frames);
                if (value.savedCueFrame && value.frames != replacement.frames)
                    throw std::runtime_error("Saved cue source frame count changed; the saved source clock cannot be reused");
                replacement.savedCueFrame = value.savedCueFrame;
                // Legacy callers cannot erase a known measurement of this
                // exact prepared recipe/geometry; new preparations recompute.
                if (!replacement.levels && value.artifactKey == replacement.artifactKey && value.frames == replacement.frames)
                    replacement.levels = value.levels;
            }
        } else {
            merged.push_back(value);
        }
    }
    if (merged.size() == maxEntries) throw std::length_error("Library catalog entry limit exceeded");
    if (position == maxEntries) position = merged.size();
    merged.insert(merged.begin() + static_cast<std::ptrdiff_t>(position), std::move(replacement));
    save(filePath_, merged);
    refreshAvailability(merged);
    return merged;
}

std::vector<LibraryTrack> LibraryCatalog::updateCues(const QString& sourceId, std::uint64_t expectedFrames,
                                                    const std::vector<CuePoint>& cues)
{
    if (!digest(sourceId) || !expectedFrames || expectedFrames > maximumFrames)
        throw std::invalid_argument("Invalid library cue source identity or frame count");
    validateCues(cues, expectedFrames);
    const auto lock = lockCatalog(filePath_);
    auto tracks = read();
    const auto found = std::ranges::find_if(tracks, [&](const auto& track) { return track.sourceSha256 == sourceId; });
    if (found == tracks.end() || found->frames != expectedFrames)
        throw std::runtime_error("Cue source changed; select its current library entry again");
    found->cues = cues;
    save(filePath_, tracks);
    refreshAvailability(tracks);
    return tracks;
}

std::vector<LibraryTrack> LibraryCatalog::updateSavedCue(const QString& sourceId, std::uint64_t expectedFrames,
                                                        std::uint64_t frame)
{
    if (!digest(sourceId) || !expectedFrames || expectedFrames > maximumFrames || frame >= expectedFrames)
        throw std::invalid_argument("Invalid saved cue source identity, frame count or frame");
    const auto lock = lockCatalog(filePath_);
    auto tracks = read();
    const auto found = std::ranges::find_if(tracks, [&](const auto& track) { return track.sourceSha256 == sourceId; });
    if (found == tracks.end() || found->frames != expectedFrames)
        throw std::runtime_error("Saved cue source changed; select its current library entry again");
    found->savedCueFrame = frame;
    save(filePath_, tracks);
    refreshAvailability(tracks);
    return tracks;
}

} // namespace conflux::library

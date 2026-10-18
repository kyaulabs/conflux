// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/PreparedTrackStore.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRandomGenerator>
#include <samplerate.h>
#include <sndfile.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace conflux::library {
namespace {
constexpr std::uint64_t frameBytes = 2 * sizeof(float);
constexpr std::size_t staleStageLimit = 8;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

class Descriptor {
public:
    explicit Descriptor(int value) noexcept : value_(value) {}
    ~Descriptor() { if (value_ >= 0) close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return value_; }
    int release() noexcept { return std::exchange(value_, -1); }
private:
    int value_;
};

void checkStop(std::stop_token stop)
{
    if (stop.stop_requested()) throw std::runtime_error("Audio preparation cancelled");
}

[[noreturn]] void ioError(const char* operation)
{
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

struct stat statusOf(int descriptor)
{
    struct stat result{};
    if (fstat(descriptor, &result) != 0) ioError("Cannot inspect prepared-track file");
    return result;
}

bool unchanged(const struct stat& before, const struct stat& after)
{
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

bool hexDigest(const QByteArray& value)
{
    return value.size() == 64 && std::ranges::all_of(value, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void writeAll(int descriptor, const char* data, std::size_t count, std::stop_token stop)
{
    while (count) {
        checkStop(stop);
        const auto written = write(descriptor, data, count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) ioError("Cannot write prepared cache");
        data += written;
        count -= std::size_t(written);
    }
}

struct Scan {
    QByteArray sha256;
    std::vector<float> peaks;
};

// Read exactly the captured size, never chase a growing source to EOF. All
// callers own the descriptor throughout; pread does not consume decoder offset.
Scan scan(int descriptor, std::uint64_t bytes, std::stop_token stop,
          std::uint64_t frames = 0, int copyTo = -1)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    Scan result;
    if (frames) result.peaks.assign(std::min<std::uint64_t>(512, frames), 0.F);
    std::array<float, 8192> buffer{};
    float maximum = 0;
    for (std::uint64_t position = 0; position < bytes;) {
        const auto wanted = std::size_t(std::min<std::uint64_t>(sizeof(buffer), bytes - position));
        auto* data = reinterpret_cast<char*>(buffer.data());
        std::size_t received = 0;
        while (received < wanted) {
            checkStop(stop);
            const auto count = pread(descriptor, data + received, wanted - received,
                                     static_cast<off_t>(position + received));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) ioError("Cannot read prepared-track bytes");
            if (!count) throw std::runtime_error("Audio source or cache changed or was truncated while reading");
            received += std::size_t(count);
        }
        if (frames) {
            for (std::size_t index = 0; index < wanted / frameBytes; ++index) {
                const auto left = buffer[index * 2], right = buffer[index * 2 + 1];
                if (!std::isfinite(left) || !std::isfinite(right))
                    throw std::invalid_argument("Prepared cache contains non-finite PCM");
                const auto peak = std::max(std::abs(left), std::abs(right));
                const auto frame = position / frameBytes + index;
                auto& bin = result.peaks[frame * result.peaks.size() / frames];
                bin = std::max(bin, peak);
                maximum = std::max(maximum, peak);
            }
        }
        hash.addData(QByteArrayView(data, qsizetype(wanted)));
        if (copyTo >= 0) writeAll(copyTo, data, wanted, stop);
        position += wanted;
    }
    if (maximum > 0)
        for (auto& value : result.peaks) value /= maximum;
    result.sha256 = hash.result().toHex();
    checkStop(stop);
    return result;
}

QJsonObject pipelineIdentity()
{
    return {{"version", 1}, {"decoder", QString::fromLatin1(sf_version_string())},
        {"converter", QString::fromLatin1(src_get_version())}, {"resampler", "SRC_SINC_BEST_QUALITY"},
        {"sample_rate", 48000}, {"channels", 2},
        {"sample_format", std::endian::native == std::endian::little ? "f32le" : "f32be"},
        {"overview_bins", 512}, {"overview_normalization", "track_peak_v1"}};
}

std::vector<std::string> directoryNames(int descriptor, std::size_t limit, std::stop_token stop = {})
{
    const int opened = openat(descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (opened < 0) ioError("Cannot list prepared cache");
    auto closeDirectory = [](DIR* value) { closedir(value); };
    std::unique_ptr<DIR, decltype(closeDirectory)> directory(fdopendir(opened), closeDirectory);
    if (!directory) { close(opened); ioError("Cannot list prepared cache"); }
    std::vector<std::string> result;
    while (true) {
        checkStop(stop);
        errno = 0;
        const auto* entry = readdir(directory.get());
        if (!entry) {
            if (errno) ioError("Cannot list prepared cache");
            return result;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        if (result.size() == limit) throw std::length_error("Prepared cache entry limit exceeded");
        result.emplace_back(entry->d_name);
    }
}

struct stat childStatus(int directory, const std::string& name)
{
    struct stat result{};
    if (fstatat(directory, name.c_str(), &result, AT_SYMLINK_NOFOLLOW) != 0)
        ioError("Cannot inspect prepared cache entry");
    return result;
}

void checkEntryContents(int directory)
{
    for (const auto& name : directoryNames(directory, 2)) {
        if ((name != "audio.f32" && name != "manifest.json") || !S_ISREG(childStatus(directory, name).st_mode))
            throw std::runtime_error("unsafe cache entry: unexpected or non-regular content");
    }
}

void removeEntry(int root, const std::string& name, int directory)
{
    checkEntryContents(directory);
    for (const auto* file : {"audio.f32", "manifest.json"})
        if (unlinkat(directory, file, 0) != 0 && errno != ENOENT) ioError("Cannot clean prepared cache entry");
    if (unlinkat(root, name.c_str(), AT_REMOVEDIR) != 0) ioError("Cannot clean prepared cache directory");
}

bool stageName(const std::string& name)
{
    return name.size() == 23 && name.starts_with(".stage-") &&
        std::ranges::all_of(std::string_view(name).substr(7), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

std::uint64_t cacheUsage(int root, const PreparedTrackStore::Limits& limits, std::stop_token stop)
{
    std::uint64_t bytes = 0;
    std::size_t entries = 0, stages = 0;
    for (const auto& name : directoryNames(root, limits.maxEntries + staleStageLimit + 1, stop)) {
        if (name == ".lock") continue;
        const auto status = childStatus(root, name);
        if (!S_ISDIR(status.st_mode) || (!hexDigest(QByteArray::fromStdString(name)) && !stageName(name)))
            throw std::runtime_error("unsafe cache entry: unexpected directory");
        Descriptor directory(openat(root, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (directory.get() < 0) ioError("Cannot open prepared cache entry");
        if (stageName(name)) {
            if (++stages > staleStageLimit) throw std::length_error("Prepared cache staging entry limit exceeded");
            removeEntry(root, name, directory.get());
            continue;
        }
        if (++entries > limits.maxEntries) throw std::length_error("Prepared cache entry limit exceeded");
        checkEntryContents(directory.get());
        for (const auto& file : directoryNames(directory.get(), 2, stop)) {
            const auto size = childStatus(directory.get(), file).st_size;
            if (size < 0 || std::uint64_t(size) > limits.maxCacheBytes - bytes)
                throw std::length_error("Prepared cache byte budget exceeded");
            bytes += std::uint64_t(size);
        }
    }
    return bytes;
}

int openCacheFile(int directory, const char* name)
{
    const int descriptor = openat(directory, name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        if (errno == ENOENT) throw std::invalid_argument("Prepared cache entry is incomplete");
        if (errno == ELOOP) throw std::runtime_error("unsafe cache entry: symbolic link");
        ioError("Cannot open prepared cache file");
    }
    Descriptor owned(descriptor);
    if (!S_ISREG(statusOf(descriptor).st_mode)) throw std::runtime_error("unsafe cache entry: non-regular file");
    return owned.release();
}

std::uint64_t jsonInteger(const QJsonValue& value, std::uint64_t maximum)
{
    if (!value.isDouble()) throw std::invalid_argument("Invalid prepared cache geometry");
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > double(maximum) || std::floor(number) != number)
        throw std::invalid_argument("Invalid prepared cache geometry");
    return std::uint64_t(number);
}

audio::PreparedAudio readEntry(int directory, const QByteArray& sourceId, const QByteArray& key,
                              const QJsonObject& pipeline, const PreparedTrackStore::Limits& limits,
                              std::stop_token stop)
{
    Descriptor manifest(openCacheFile(directory, "manifest.json"));
    const auto before = statusOf(manifest.get());
    const PreparedTrackStore::Limits formatLimits;
    if (before.st_size <= 0 || std::uint64_t(before.st_size) > formatLimits.maxManifestBytes)
        throw std::invalid_argument("Prepared cache manifest size is invalid");
    if (std::uint64_t(before.st_size) > limits.maxManifestBytes)
        throw std::length_error("Prepared cache manifest byte budget exceeded");
    QByteArray bytes(qsizetype(before.st_size), Qt::Uninitialized);
    std::size_t position = 0;
    while (position < std::size_t(bytes.size())) {
        checkStop(stop);
        const auto count = pread(manifest.get(), bytes.data() + position, std::size_t(bytes.size()) - position,
                                 static_cast<off_t>(position));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) ioError("Cannot read prepared cache manifest");
        if (!count) throw std::invalid_argument("Prepared cache manifest is truncated");
        position += std::size_t(count);
    }
    if (!unchanged(before, statusOf(manifest.get()))) throw std::invalid_argument("Prepared cache manifest changed");
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) throw std::invalid_argument("Prepared cache manifest is malformed");
    const auto object = document.object();
    if (object.size() != 8 || object.value("schema") != QJsonValue(1) ||
        object.value("source_sha256").toString().toLatin1() != sourceId ||
        object.value("artifact_key").toString().toLatin1() != key || object.value("pipeline").toObject() != pipeline)
        throw std::invalid_argument("Prepared cache identity or version differs");
    const auto frames = jsonInteger(object.value("frames"), formatLimits.maxPreparedBytes / frameBytes);
    if (!frames || jsonInteger(object.value("pcm_bytes"), formatLimits.maxPreparedBytes) != frames * frameBytes)
        throw std::invalid_argument("Prepared cache PCM size is invalid");
    if (frames * frameBytes > limits.maxPreparedBytes)
        throw std::length_error("Cached audio exceeds the prepared PCM byte budget");
    const auto digest = object.value("pcm_sha256").toString().toLatin1();
    const auto array = object.value("peaks").toArray();
    if (!hexDigest(digest) || array.size() != qsizetype(std::min<std::uint64_t>(512, frames)))
        throw std::invalid_argument("Prepared cache overview or digest is invalid");
    std::vector<float> peaks;
    peaks.reserve(std::size_t(array.size()));
    for (const auto value : array) {
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0 || value.toDouble() > 1)
            throw std::invalid_argument("Prepared cache overview is invalid");
        peaks.push_back(float(value.toDouble()));
    }
    Descriptor pcm(openCacheFile(directory, "audio.f32"));
    auto prepared = audio::reopenPreparedAudio(pcm.get(), frames, peaks, limits.maxPreparedBytes);
    const auto pcmBefore = statusOf(prepared.descriptor());
    const auto observed = scan(prepared.descriptor(), frames * frameBytes, stop, frames);
    if (!unchanged(pcmBefore, statusOf(prepared.descriptor())) || observed.sha256 != digest ||
        !std::ranges::equal(observed.peaks, peaks))
        throw std::invalid_argument("Prepared cache PCM digest or overview differs");
    return prepared;
}

class Stage {
public:
    explicit Stage(int root) : root_(root), name_(".stage-" +
        QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0').toStdString()) {}
    ~Stage()
    {
        if (directory_ >= 0) {
            if (!published_) { try { removeEntry(root_, name_, directory_); } catch (...) {} }
            close(directory_);
        }
    }
    void create()
    {
        if (mkdirat(root_, name_.c_str(), 0700) != 0) ioError("Cannot create prepared cache staging directory");
        directory_ = openat(root_, name_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory_ < 0) {
            unlinkat(root_, name_.c_str(), AT_REMOVEDIR);
            ioError("Cannot open prepared cache staging directory");
        }
    }
    int directory() const noexcept { return directory_; }
    void publish(const QByteArray& key)
    {
        if (fsync(directory_) != 0 || renameat(root_, name_.c_str(), root_, key.constData()) != 0)
            ioError("Cannot publish complete prepared cache entry");
        published_ = true;
        if (fsync(root_) != 0) ioError("Cannot synchronize prepared cache directory");
    }
private:
    int root_, directory_ = -1;
    std::string name_;
    bool published_ = false;
};

void verifySource(int descriptor, const struct stat& before, const QByteArray& digest, std::stop_token stop)
{
    if (!unchanged(before, statusOf(descriptor))) throw std::runtime_error("Audio source changed during preparation");
    const auto observed = scan(descriptor, std::uint64_t(before.st_size), stop).sha256;
    if (observed != digest || !unchanged(before, statusOf(descriptor)))
        throw std::runtime_error("Audio source changed during preparation");
}
} // namespace

PreparedTrackStore::PreparedTrackStore(QString cacheRoot)
    : PreparedTrackStore(std::move(cacheRoot), Limits{}) {}

PreparedTrackStore::PreparedTrackStore(QString cacheRoot, Limits limits)
    : cacheRoot_(std::move(cacheRoot)), limits_(limits)
{
    const Limits defaults;
    if (cacheRoot_.isEmpty() || limits_.maxPreparedBytes < frameBytes || limits_.maxPreparedBytes > defaults.maxPreparedBytes ||
        !limits_.maxSourceBytes || limits_.maxSourceBytes > defaults.maxSourceBytes ||
        !limits_.maxCacheBytes || limits_.maxCacheBytes > defaults.maxCacheBytes ||
        !limits_.maxEntries || limits_.maxEntries > defaults.maxEntries ||
        !limits_.maxManifestBytes || limits_.maxManifestBytes > defaults.maxManifestBytes)
        throw std::invalid_argument("Invalid prepared cache limits or directory");
}

PreparedTrack PreparedTrackStore::prepare(const QString& sourcePath, std::stop_token stop)
{
    checkStop(stop);
    Descriptor source(open(QFile::encodeName(sourcePath).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (source.get() < 0) ioError("Cannot open audio source");
    const auto sourceBefore = statusOf(source.get());
    if (!S_ISREG(sourceBefore.st_mode)) throw std::invalid_argument("Audio input must be a regular local file");
    if (sourceBefore.st_size < 0 || std::uint64_t(sourceBefore.st_size) > limits_.maxSourceBytes)
        throw std::length_error("Audio source byte budget exceeded");
    const auto sourceId = scan(source.get(), std::uint64_t(sourceBefore.st_size), stop).sha256;
    if (!unchanged(sourceBefore, statusOf(source.get()))) throw std::runtime_error("Audio source changed during hashing");
    const auto pipeline = pipelineIdentity();
    const auto key = QCryptographicHash::hash(sourceId + '\n' + QJsonDocument(pipeline).toJson(QJsonDocument::Compact),
                                             QCryptographicHash::Sha256).toHex();
    if (!QDir().mkpath(cacheRoot_)) throw std::runtime_error("Cannot create prepared cache root");
    Descriptor root(open(QFile::encodeName(cacheRoot_).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (root.get() < 0) ioError("Cannot open prepared cache root");
    if (fchmod(root.get(), 0700) != 0) ioError("Cannot protect prepared cache root");
    QLockFile lock(QDir(cacheRoot_).filePath(".lock"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!lock.tryLock(0)) {
        checkStop(stop);
        if (lock.error() != QLockFile::LockFailedError) throw std::runtime_error("Cannot lock prepared cache");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Prepared cache is busy");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    checkStop(stop);
    auto usage = cacheUsage(root.get(), limits_, stop);
    struct stat entryStatus{};
    if (fstatat(root.get(), key.constData(), &entryStatus, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISDIR(entryStatus.st_mode)) throw std::runtime_error("unsafe cache entry: non-directory artifact");
        Descriptor entry(openat(root.get(), key.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (entry.get() < 0) ioError("Cannot open prepared cache entry");
        try {
            auto prepared = readEntry(entry.get(), sourceId, key, pipeline, limits_, stop);
            verifySource(source.get(), sourceBefore, sourceId, stop);
            return {std::move(prepared), QString::fromLatin1(sourceId), QString::fromLatin1(key), true};
        } catch (const std::invalid_argument&) {
            removeEntry(root.get(), key.toStdString(), entry.get());
            usage = cacheUsage(root.get(), limits_, stop);
        }
    } else if (errno != ENOENT) {
        ioError("Cannot inspect prepared cache artifact");
    }
    const auto names = directoryNames(root.get(), limits_.maxEntries + 1, stop);
    const auto count = std::ranges::count_if(names, [](const auto& name) { return hexDigest(QByteArray::fromStdString(name)); });
    if (std::size_t(count) >= limits_.maxEntries) throw std::length_error("Prepared cache entry limit exceeded");
    if (limits_.maxPreparedBytes > limits_.maxCacheBytes - usage)
        throw std::length_error("Prepared cache byte budget lacks preparation headroom");
    auto prepared = audio::prepareAudioFile(source.get(), limits_.maxPreparedBytes, stop,
                                            std::filesystem::path(QFile::encodeName(cacheRoot_).constData()));
    const auto pcmBytes = prepared.frames() * frameBytes;
    if (pcmBytes > (limits_.maxCacheBytes - usage) / 2 ||
        limits_.maxManifestBytes > limits_.maxCacheBytes - usage - pcmBytes * 2)
        throw std::length_error("Prepared cache byte budget lacks publication headroom");
    Stage stage(root.get());
    stage.create();
    Descriptor destination(openat(stage.directory(), "audio.f32", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (destination.get() < 0) ioError("Cannot create prepared cache PCM");
    const auto copied = scan(prepared.descriptor(), pcmBytes, stop, prepared.frames(), destination.get());
    if (fsync(destination.get()) != 0) ioError("Cannot synchronize prepared cache PCM");
    Descriptor readable(openCacheFile(stage.directory(), "audio.f32"));
    auto persisted = audio::reopenPreparedAudio(readable.get(), prepared.frames(), prepared.peaks(), limits_.maxPreparedBytes);
    const auto pcmScan = scan(persisted.descriptor(), pcmBytes, stop, persisted.frames());
    if (pcmScan.sha256 != copied.sha256 || !std::ranges::equal(pcmScan.peaks, prepared.peaks()))
        throw std::runtime_error("Prepared cache copy digest or overview differs");
    QJsonArray peaks;
    for (const auto value : prepared.peaks()) peaks.append(double(value));
    const QJsonObject object{{"schema", 1}, {"source_sha256", QString::fromLatin1(sourceId)},
        {"artifact_key", QString::fromLatin1(key)}, {"pipeline", pipeline},
        {"frames", double(persisted.frames())}, {"pcm_bytes", double(pcmBytes)},
        {"pcm_sha256", QString::fromLatin1(pcmScan.sha256)}, {"peaks", peaks}};
    const auto manifest = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (std::size_t(manifest.size()) > limits_.maxManifestBytes)
        throw std::length_error("Prepared cache manifest byte budget exceeded");
    Descriptor metadata(openat(stage.directory(), "manifest.json", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (metadata.get() < 0) ioError("Cannot create prepared cache manifest");
    writeAll(metadata.get(), manifest.constData(), std::size_t(manifest.size()), stop);
    if (fsync(metadata.get()) != 0) ioError("Cannot synchronize prepared cache manifest");
    verifySource(source.get(), sourceBefore, sourceId, stop);
    checkStop(stop);
    stage.publish(key);
    return {std::move(persisted), QString::fromLatin1(sourceId), QString::fromLatin1(key), false};
}

} // namespace conflux::library

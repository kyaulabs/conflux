// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/PreparedTrackStore.hpp"
#include "library/StemEngineSupport.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRandomGenerator>
#include <QProcess>
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
#include <sys/file.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace conflux::library {
namespace detail {
constexpr std::uint64_t frameBytes = 2 * sizeof(float);
constexpr std::size_t staleStageLimit = 8;
constexpr std::array<const char*, 4> stemNames{"drums.f32", "bass.f32", "other.f32", "vocals.f32"};
constexpr std::array<const char*, 4> stemRoles{"drums", "bass", "other", "vocals"};
constexpr std::uint64_t nativeSpoolBytes = 13759200, exportManifestBytes = 16384;
bool stemLeaf(std::string_view name)
{
    return std::ranges::any_of(stemNames, [name](const char* leaf) { return name == leaf; });
}
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);


QJsonObject pipelineIdentity()
{
    return {{"version", 1}, {"decoder", QString::fromLatin1(sf_version_string())},
        {"converter", QString::fromLatin1(src_get_version())}, {"resampler", "SRC_SINC_BEST_QUALITY"},
        {"sample_rate", 48000}, {"channels", 2},
        {"sample_format", std::endian::native == std::endian::little ? "f32le" : "f32be"},
        {"overview_bins", 512}, {"overview_normalization", "track_peak_v1"}};
}

std::vector<std::string> directoryNames(int descriptor, std::size_t limit, std::stop_token stop)
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
    bool normal = false, bank = false;
    for (const auto& name : directoryNames(directory, 5)) {
        const bool stem = stemLeaf(name);
        if ((name != "audio.f32" && name != "manifest.json" && !stem) || !S_ISREG(childStatus(directory, name).st_mode))
            throw std::runtime_error("unsafe cache entry: unexpected or non-regular content");
        normal |= name == "audio.f32";
        bank |= stem;
    }
    if (normal && bank) throw std::runtime_error("unsafe cache entry: mixed normal and stem content");
}

void removeEntry(int root, const std::string& name, int directory)
{
    checkEntryContents(directory);
    for (const auto* file : {"audio.f32", "drums.f32", "bass.f32", "other.f32", "vocals.f32", "manifest.json"})
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

void removeStage(int root, const std::string& name, int directory, bool nativeCleanupProven = false);

std::uint64_t cacheUsage(int root, const PreparedTrackStore::Limits& limits, std::stop_token stop)
{
    std::uint64_t bytes = 0;
    std::size_t entries = 0, stages = 0;
    for (const auto& name : directoryNames(root, limits.maxEntries + staleStageLimit + 1, stop)) {
        if (name == ".lock") continue;
        const auto status = childStatus(root, name);
        if (!S_ISDIR(status.st_mode) || (!hexDigest(QByteArray::fromStdString(name)) && !stageName(name) && !liveName(name)))
            throw std::runtime_error("unsafe cache entry: unexpected directory");
        Descriptor directory(openat(root, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (directory.get() < 0) ioError("Cannot open prepared cache entry");
        if (stageName(name)) {
            if (++stages > staleStageLimit) throw std::length_error("Prepared cache staging entry limit exceeded");
            removeStage(root, name, directory.get());
            continue;
        }
        if (++entries > limits.maxEntries) throw std::length_error("Prepared cache entry limit exceeded");
        if (liveName(name)) {
            const auto reserved = liveReservationBytes(directory.get(), limits, stop);
            if (reserved > limits.maxCacheBytes - bytes) throw std::length_error("Prepared cache byte budget exceeded");
            bytes += reserved;
            continue; // Never adopt/clean a live job without its lost group proof.
        }
        checkEntryContents(directory.get());
        for (const auto& file : directoryNames(directory.get(), 5, stop)) {
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

QJsonObject readManifest(int directory, const PreparedTrackStore::Limits& limits, std::stop_token stop)
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
    return document.object();
}

audio::PreparedAudio readEntry(int directory, const QByteArray& sourceId, const QByteArray& key,
                              const QJsonObject& pipeline, const PreparedTrackStore::Limits& limits,
                              std::stop_token stop)
{
    const auto object = readManifest(directory, limits, stop);
    const PreparedTrackStore::Limits formatLimits;
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
            if (!published_ && !preserved_) { try { removeStage(root_, name_, directory_, nativeCleanupProven_); } catch (...) {} }
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
    const std::string& name() const noexcept { return name_; }
    void allowNativeCleanup() noexcept { nativeCleanupProven_ = true; }
    void preserve() noexcept { preserved_ = true; }
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
    bool published_ = false, preserved_ = false, nativeCleanupProven_ = false;
};


// Only this fixed job layout is traversed. Unknown files, symlinks, held
// exporter locks and unexplained native spools are preserved, never followed.
void rawJobContents(int directory, bool nativeCleanupProven, bool remove)
{
    for (const auto& name : directoryNames(directory, 6)) {
        const auto status = childStatus(directory, name);
        if (name == "native") {
            if (!S_ISDIR(status.st_mode)) throw std::runtime_error("unsafe cache entry: native spool is not a directory");
            if (!nativeCleanupProven) throw std::runtime_error("Prepared cache is busy: native export cleanup is unproven");
            Descriptor native(openat(directory, "native", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (native.get() < 0) ioError("Cannot inspect native export spool");
            for (const auto& leaf : directoryNames(native.get(), 3)) {
                if ((leaf != "input.f32" && leaf != "output.f32" && leaf != "runtime.stderr") ||
                    !S_ISREG(childStatus(native.get(), leaf).st_mode))
                    throw std::runtime_error("unsafe cache entry: unexpected native export spool content");
                if (remove && unlinkat(native.get(), leaf.c_str(), 0) != 0) ioError("Cannot clean native export spool");
            }
            if (remove && unlinkat(directory, "native", AT_REMOVEDIR) != 0) ioError("Cannot clean native export directory");
        } else {
            if ((name != "manifest.json" && !stemLeaf(name)) || !S_ISREG(status.st_mode))
                throw std::runtime_error("unsafe cache entry: unexpected raw export content");
            if (remove && unlinkat(directory, name.c_str(), 0) != 0) ioError("Cannot clean raw export file");
        }
    }
}

void removeStage(int root, const std::string& name, int directory, bool nativeCleanupProven)
{
    Descriptor lease(openat(directory, ".raw.export.lock", O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (lease.get() < 0 && errno != ENOENT) ioError("Cannot inspect export ownership lock");
    if (lease.get() >= 0) {
        if (!S_ISREG(statusOf(lease.get()).st_mode)) throw std::runtime_error("unsafe cache entry: export lock is not regular");
        if (flock(lease.get(), LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("Prepared cache is busy: export ownership lock is held");
    }
    const auto names = directoryNames(directory, 9);
    // Validate the entire bounded tree before deleting any part of it.
    for (const auto& leaf : names) {
        const auto status = childStatus(directory, leaf);
        if (leaf == "raw" || leaf == "raw.stage") {
            if (!S_ISDIR(status.st_mode)) throw std::runtime_error("unsafe cache entry: raw export is not a directory");
            Descriptor raw(openat(directory, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (raw.get() < 0) ioError("Cannot inspect raw export directory");
            rawJobContents(raw.get(), nativeCleanupProven, false);
        } else if ((leaf != "audio.f32" && leaf != "manifest.json" && leaf != "input.f32" && leaf != ".raw.export.lock" &&
                    !stemLeaf(leaf)) || !S_ISREG(status.st_mode)) {
            throw std::runtime_error("unsafe cache entry: unexpected staging content");
        }
    }
    for (const auto& leaf : names) {
        if (leaf == "raw" || leaf == "raw.stage") {
            Descriptor raw(openat(directory, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (raw.get() < 0) ioError("Cannot open raw export directory for cleanup");
            rawJobContents(raw.get(), nativeCleanupProven, true);
            if (unlinkat(directory, leaf.c_str(), AT_REMOVEDIR) != 0) ioError("Cannot clean raw export directory");
        } else if (unlinkat(directory, leaf.c_str(), 0) != 0) ioError("Cannot clean prepared staging file");
    }
    if (unlinkat(root, name.c_str(), AT_REMOVEDIR) != 0) ioError("Cannot clean prepared staging directory");
}


class ExportProcess {
public:
    explicit ExportProcess(bool& groupsGone) : owner_(groupsGone), process_(owner_.process())
    {
        QObject::connect(&process_, &QProcess::readyReadStandardOutput, &process_, [this] {
            append(output_, process_.readAllStandardOutput());
        }, Qt::DirectConnection);
        QObject::connect(&process_, &QProcess::readyReadStandardError, &process_, [this] {
            append(error_, process_.readAllStandardError());
        }, Qt::DirectConnection);
    }
    ~ExportProcess()
    {
        (void)finish();
        QObject::disconnect(&process_, nullptr, &process_, nullptr);
    }
    QByteArray run(const QString& program, const QStringList& arguments, std::stop_token stop,
                   std::chrono::milliseconds timeout, std::size_t maximumDiagnosticBytes)
    {
        checkStop(stop);
        maximumDiagnosticBytes_ = maximumDiagnosticBytes;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        process_.start(program, arguments);
        if (!process_.waitForStarted(1000)) throw std::runtime_error("Cannot start the local stem exporter");
        if (owner_.group() <= 1) throw std::runtime_error("Cannot identify the owned stem exporter process group");
        while (process_.state() != QProcess::NotRunning) {
            checkStop(stop);
            if (overflow_) throw std::length_error("Stem exporter diagnostics exceed their byte budget");
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Stem exporter timed out");
            process_.waitForFinished(25);
        }
        append(output_, process_.readAllStandardOutput());
        append(error_, process_.readAllStandardError());
        if (overflow_) throw std::length_error("Stem exporter diagnostics exceed their byte budget");
        const bool succeeded = process_.exitStatus() == QProcess::NormalExit && process_.exitCode() == 0;
        if (!finish()) throw std::runtime_error("Cannot prove owned stem exporter process-group cleanup");
        if (!succeeded) throw std::runtime_error("Local stem export failed: " + error_.right(4096).toStdString());
        checkStop(stop);
        return output_;
    }
    bool finish() noexcept { return owner_.finish(); }

private:
    void append(QByteArray& retained, QByteArray bytes)
    {
        const auto available = maximumDiagnosticBytes_ - diagnostics_;
        const auto count = std::min<std::size_t>(available, std::size_t(bytes.size()));
        retained.append(bytes.constData(), qsizetype(count));
        diagnostics_ += count;
        overflow_ |= count != std::size_t(bytes.size());
    }
    OwnedStemProcess owner_;
    QProcess& process_;
    bool overflow_ = false;
    std::size_t diagnostics_ = 0, maximumDiagnosticBytes_ = 0;
    QByteArray output_, error_;
};

QJsonObject describeEngine(const StemEngine& engine, const EngineFiles& files, std::stop_token stop)
{
    verifyEngine(files, stop);
    bool groupsGone = true;
    ExportProcess process(groupsGone);
    const auto output = process.run(files[0].invocation, {"-B", files[1].path, "--describe"}, stop,
        std::chrono::seconds(std::min(engine.maxSeconds, 5)), engine.maxDiagnosticBytes);
    verifyEngine(files, stop);
    if (output.size() > 4096) throw std::length_error("Stem description exceeds its byte budget");
    const auto document = QJsonDocument::fromJson(output);
    const auto object = document.object();
    if (!document.isObject()) throw std::invalid_argument("Stem description must be an object");
    validateDescription(object, files);
    return object;
}

QJsonObject stemPipeline(const StemEngine& engine, const EngineFiles& files, const QJsonObject& describe)
{
    return {{"version", 1}, {"kind", "stem_bank"}, {"python", files[0].identity}, {"helper", files[1].identity},
        {"worker", files[2].identity}, {"runtime", files[3].identity}, {"model", files[4].identity},
        {"backend", engine.backend}, {"threads", engine.threads}, {"describe", describe},
        {"input_conversion", QJsonObject{{"decoder", QString::fromLatin1(sf_version_string())},
            {"converter", QString::fromLatin1(src_get_version())}, {"resampler", "SRC_SINC_BEST_QUALITY"},
            {"sample_rate", 44100}, {"channels", 2}, {"sample_format", "f32le"}}},
        {"output_conversion", pipelineIdentity()}};
}

void validateExport(const QJsonObject& object, const StemEngine& engine, const EngineFiles& files,
                    const QJsonObject& describe, const QByteArray& inputDigest, std::uint64_t frames,
                    const QJsonObject* requestedLimits = nullptr)
{
    if (object.size() != 9 || object.value("schema") != QJsonValue(1) || object.value("describe").toObject() != describe)
        throw std::invalid_argument("Stem export manifest version or description differs");
    const QJsonObject source{{"format", "f32le"}, {"sample_rate", 44100}, {"channels", 2},
        {"frames", double(frames)}, {"bytes", double(frames * frameBytes)}, {"sha256", QString::fromLatin1(inputDigest)}};
    const auto ready = object.value("runtime").toObject().value("ready").toObject();
    validateRuntimeReady(ready, engine);
    auto runtime = files[3].identity;
    runtime.insert("ready", ready);
    const auto separation = object.value("separation").toObject();
    if (!frames || !hexDigest(inputDigest) || object.value("source").toObject() != source ||
        object.value("model").toObject() != files[4].identity || object.value("runtime").toObject() != runtime ||
        separation.size() != 9 || separation.value("worker_sha256") != describe.value("worker_sha256") ||
        separation.value("numpy_version") != describe.value("numpy_version") ||
        separation.value("window_frames") != describe.value("window_frames") ||
        separation.value("hop_frames") != describe.value("hop_frames") ||
        separation.value("overlap_frames") != describe.value("overlap_frames") ||
        separation.value("normalization") != describe.value("normalization") ||
        separation.value("overlap_add") != describe.value("overlap_add") ||
        !separation.value("mean").isDouble() || !std::isfinite(separation.value("mean").toDouble()) ||
        !separation.value("std").isDouble() || !std::isfinite(separation.value("std").toDouble()) || separation.value("std").toDouble() <= 0)
        throw std::invalid_argument("Stem export source, model, runtime or separation identity differs");
    const auto limits = object.value("limits").toObject();
    if (limits.size() != 6 || (requestedLimits && limits != *requestedLimits) ||
        jsonInteger(limits.value("max_input_bytes"), audio::PreparedStemInput::defaultMaxPreparedBytes) < frames * frameBytes ||
        jsonInteger(limits.value("max_output_bytes"), 4ULL * audio::PreparedStemInput::defaultMaxPreparedBytes) < frames * frameBytes * 4 ||
        !jsonInteger(limits.value("max_seconds"), 7200) || !jsonInteger(limits.value("request_timeout"), 30) ||
        !jsonInteger(limits.value("max_diagnostic_bytes"), 65536))
        throw std::invalid_argument("Stem export limits differ or are invalid");
    const auto reservation = frames * frameBytes * 5 + nativeSpoolBytes +
        jsonInteger(limits.value("max_diagnostic_bytes"), 65536) + exportManifestBytes;
    if (jsonInteger(object.value("reservation_bytes"), 2ULL * 1024 * 1024 * 1024) != reservation ||
        jsonInteger(limits.value("max_job_bytes"), 2ULL * 1024 * 1024 * 1024) < reservation)
        throw std::invalid_argument("Stem export reservation differs");
    const auto stems = object.value("stems").toArray();
    if (stems.size() != 4) throw std::invalid_argument("Stem export must contain every fixed role");
    for (std::size_t index = 0; index < stemNames.size(); ++index) {
        const auto stem = stems[qsizetype(index)].toObject();
        if (stem.size() != 5 || stem.value("name") != QJsonValue(QString::fromLatin1(stemRoles[index])) ||
            stem.value("file") != QJsonValue(QString::fromLatin1(stemNames[index])) ||
            jsonInteger(stem.value("frames"), audio::PreparedStemInput::defaultMaxPreparedBytes / frameBytes) != frames ||
            jsonInteger(stem.value("bytes"), audio::PreparedStemInput::defaultMaxPreparedBytes) != frames * frameBytes ||
            !hexDigest(stem.value("sha256").toString().toLatin1()))
            throw std::invalid_argument("Stem export role, geometry or digest differs");
    }
}

PreparedStemTrack readStemEntry(int directory, const QByteArray& sourceId, const QByteArray& key,
                               const QJsonObject& pipeline, const StemEngine& engine, const EngineFiles& files,
                               const QJsonObject& describe, const PreparedTrackStore::Limits& limits,
                               std::stop_token stop)
{
    const auto object = readManifest(directory, limits, stop);
    if (object.size() != 9 || object.value("schema") != QJsonValue(2) || object.value("kind") != QJsonValue("stem_bank") ||
        object.value("source_sha256") != QJsonValue(QString::fromLatin1(sourceId)) ||
        object.value("artifact_key") != QJsonValue(QString::fromLatin1(key)) || object.value("pipeline").toObject() != pipeline)
        throw std::invalid_argument("Prepared stem cache identity or version differs");
    const PreparedTrackStore::Limits formatLimits;
    const auto frames = jsonInteger(object.value("frames"), formatLimits.maxPreparedBytes / frameBytes);
    if (!frames || jsonInteger(object.value("pcm_bytes"), 4ULL * formatLimits.maxPreparedBytes) != frames * frameBytes * 4)
        throw std::invalid_argument("Prepared stem cache PCM geometry is invalid");
    if (frames * frameBytes > limits.maxPreparedBytes) throw std::length_error("Cached stems exceed the prepared PCM byte budget");
    const auto exported = object.value("export").toObject();
    const auto input = exported.value("source").toObject();
    const auto inputFrames = jsonInteger(input.value("frames"), audio::PreparedStemInput::defaultMaxPreparedBytes / frameBytes);
    validateExport(exported, engine, files, describe, input.value("sha256").toString().toLatin1(),
        inputFrames);
    const auto remainder = (inputFrames % 44100) * 48000;
    const auto floorFrames = inputFrames / 44100 * 48000 + remainder / 44100;
    const auto ceilFrames = floorFrames + (remainder % 44100 != 0);
    if (frames < (floorFrames ? floorFrames - 1 : 0) || frames > ceilFrames + 1)
        throw std::invalid_argument("Prepared stem cache duration differs from its exported source");
    const auto stems = object.value("stems").toArray();
    if (stems.size() != 4) throw std::invalid_argument("Prepared stem cache is incomplete");
    std::vector<audio::PreparedAudio> prepared;
    prepared.reserve(4);
    for (std::size_t index = 0; index < stemNames.size(); ++index) {
        const auto stem = stems[qsizetype(index)].toObject();
        if (stem.size() != 4 || stem.value("name") != QJsonValue(QString::fromLatin1(stemRoles[index])) ||
            stem.value("file") != QJsonValue(QString::fromLatin1(stemNames[index])) ||
            jsonInteger(stem.value("bytes"), formatLimits.maxPreparedBytes) != frames * frameBytes ||
            !hexDigest(stem.value("sha256").toString().toLatin1()))
            throw std::invalid_argument("Prepared stem cache role or geometry differs");
        Descriptor pcm(openCacheFile(directory, stemNames[index]));
        const auto before = statusOf(pcm.get());
        if (before.st_size < 0 || std::uint64_t(before.st_size) != frames * frameBytes)
            throw std::invalid_argument("Prepared stem cache PCM size differs");
        const auto observed = scan(pcm.get(), frames * frameBytes, stop, frames);
        if (observed.sha256 != stem.value("sha256").toString().toLatin1() || !unchanged(before, statusOf(pcm.get())))
            throw std::invalid_argument("Prepared stem cache PCM digest differs");
        prepared.push_back(audio::reopenPreparedAudio(pcm.get(), frames, observed.peaks, limits.maxPreparedBytes));
        if (!unchanged(before, statusOf(pcm.get()))) throw std::invalid_argument("Prepared stem cache PCM changed");
    }
    return {{{std::move(prepared[0]), std::move(prepared[1]), std::move(prepared[2]), std::move(prepared[3])}},
        QString::fromLatin1(sourceId), QString::fromLatin1(key), true};
}
} // namespace

using namespace detail;

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
    const auto count = std::ranges::count_if(names, [](const auto& name) { return hexDigest(QByteArray::fromStdString(name)) || liveName(name); });
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

PreparedStemTrack PreparedTrackStore::prepareStemBank(const QString& sourcePath, const StemEngine& engine,
                                                       std::stop_token stop)
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
    const auto files = pinEngine(engine, stop);
    const auto describe = describeEngine(engine, files, stop);
    const auto pipeline = stemPipeline(engine, files, describe);
    const auto key = QCryptographicHash::hash(sourceId + '\n' + QJsonDocument(pipeline).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
    if (!QDir().mkpath(cacheRoot_)) throw std::runtime_error("Cannot create prepared cache root");
    Descriptor root(open(QFile::encodeName(cacheRoot_).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (root.get() < 0) ioError("Cannot open prepared cache root");
    if (fchmod(root.get(), 0700) != 0) ioError("Cannot protect prepared cache root");
    QLockFile lock(QDir(cacheRoot_).filePath(".lock"));
    const auto lockDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!lock.tryLock(0)) {
        checkStop(stop);
        if (lock.error() != QLockFile::LockFailedError) throw std::runtime_error("Cannot lock prepared cache");
        if (std::chrono::steady_clock::now() >= lockDeadline) throw std::runtime_error("Prepared cache is busy");
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
            auto prepared = readStemEntry(entry.get(), sourceId, key, pipeline, engine, files, describe, limits_, stop);
            verifySource(source.get(), sourceBefore, sourceId, stop);
            verifyEngine(files, stop);
            return prepared;
        } catch (const std::invalid_argument&) {
            removeEntry(root.get(), key.toStdString(), entry.get());
            usage = cacheUsage(root.get(), limits_, stop);
        }
    } else if (errno != ENOENT) {
        ioError("Cannot inspect prepared cache artifact");
    }
    const auto names = directoryNames(root.get(), limits_.maxEntries + 1, stop);
    if (std::size_t(std::ranges::count_if(names, [](const auto& name) { return hexDigest(QByteArray::fromStdString(name)) || liveName(name); })) >= limits_.maxEntries)
        throw std::length_error("Prepared cache entry limit exceeded");
    const auto available = limits_.maxCacheBytes - usage;
    if (available < frameBytes) throw std::length_error("Prepared cache byte budget lacks stem input headroom");
    Stage stage(root.get());
    stage.create();
    const auto jobPath = QDir(QFileInfo(cacheRoot_).absoluteFilePath()).filePath(QString::fromStdString(stage.name()));
    const auto inputPath = QDir(jobPath).filePath("input.f32");
    const auto rawPath = QDir(jobPath).filePath("raw");
    std::uint64_t inputFrames = 0, inputBytes = 0, convertedBytes = 0, reservation = 0;
    QByteArray inputDigest;
    bool groupsGone = true;
    try {
        {
            auto input = audio::prepareStemInput(source.get(), std::size_t(std::min<std::uint64_t>(limits_.maxPreparedBytes, available)),
                stop, std::filesystem::path(QFile::encodeName(jobPath).constData()));
            inputFrames = input.frames();
            inputBytes = inputFrames * frameBytes;
            // Reserve the actual rational output ceiling plus the converter's
            // permitted one-frame rounding. The same cap guards every emit.
            const auto convertedFrames = inputFrames / 44100 * 48000 +
                ((inputFrames % 44100) * 48000 + 44100 - 1) / 44100 + 1;
            convertedBytes = convertedFrames * frameBytes;
            if (convertedBytes > limits_.maxPreparedBytes)
                throw std::length_error("Stem conversion exceeds the prepared PCM byte budget");
            reservation = inputBytes * 5 + nativeSpoolBytes + engine.maxDiagnosticBytes + exportManifestBytes;
            const auto total = reservation + convertedBytes * 8 + limits_.maxManifestBytes;
            if (total > available) throw std::length_error("Prepared cache byte budget lacks complete stem-job headroom");
            Descriptor named(openat(stage.directory(), "input.f32", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (named.get() < 0) ioError("Cannot create named stem input");
            inputDigest = scan(input.descriptor(), inputBytes, stop, 0, named.get()).sha256;
            if (fsync(named.get()) != 0) ioError("Cannot synchronize named stem input");
        } // Anonymous copy closes before raw outputs/native spools are created.
        Descriptor input(openCacheFile(stage.directory(), "input.f32"));
        const auto inputBefore = statusOf(input.get());
        if (inputBefore.st_size < 0 || std::uint64_t(inputBefore.st_size) != inputBytes)
            throw std::runtime_error("Named stem input geometry changed");
        const QJsonObject requestedLimits{{"max_input_bytes", double(inputBytes)}, {"max_output_bytes", double(inputBytes * 4)},
            {"max_job_bytes", double(reservation)}, {"max_seconds", engine.maxSeconds},
            {"request_timeout", engine.requestTimeout}, {"max_diagnostic_bytes", double(engine.maxDiagnosticBytes)}};
        verifyEngine(files, stop);
        {
            ExportProcess process(groupsGone);
            (void)process.run(files[0].invocation, {"-B", files[1].path,
                "--input", inputPath, "--destination", rawPath, "--runtime", files[3].path, "--model", files[4].path,
                "--backend", engine.backend, "--threads", QString::number(engine.threads),
                "--max-input-bytes", QString::number(qulonglong(inputBytes)),
                "--max-output-bytes", QString::number(qulonglong(inputBytes * 4)),
                "--max-job-bytes", QString::number(qulonglong(reservation)),
                "--max-seconds", QString::number(engine.maxSeconds), "--request-timeout", QString::number(engine.requestTimeout),
                "--max-diagnostic-bytes", QString::number(qulonglong(engine.maxDiagnosticBytes))}, stop,
                std::chrono::seconds(engine.maxSeconds), engine.maxDiagnosticBytes);
        }
        verifyEngine(files, stop);
        stage.allowNativeCleanup(); // The direct child is reaped and its group is gone.
        Descriptor lease(openat(stage.directory(), ".raw.export.lock", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
        if (lease.get() < 0 && errno != ENOENT) ioError("Cannot inspect completed export ownership lock");
        if (lease.get() >= 0 && (!S_ISREG(statusOf(lease.get()).st_mode) || flock(lease.get(), LOCK_EX | LOCK_NB) != 0))
            throw std::runtime_error("Prepared cache is busy: exporter ownership is still held");
        Descriptor raw(openat(stage.directory(), "raw", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (raw.get() < 0) ioError("Cannot open complete raw stem bank");
        rawJobContents(raw.get(), false, false);
        if (directoryNames(raw.get(), 5, stop).size() != 5) throw std::invalid_argument("Raw stem bank is incomplete");
        const auto exported = readManifest(raw.get(), limits_, stop);
        validateExport(exported, engine, files, describe, inputDigest, inputFrames, &requestedLimits);
        verifySource(input.get(), inputBefore, inputDigest, stop);
        const auto exportedStems = exported.value("stems").toArray();
        std::vector<audio::PreparedAudio> prepared;
        prepared.reserve(4);
        QJsonArray stems;
        std::uint64_t frames = 0;
        for (std::size_t index = 0; index < stemNames.size(); ++index) {
            Descriptor rawStem(openCacheFile(raw.get(), stemNames[index]));
            const auto rawBefore = statusOf(rawStem.get());
            const auto rawDigest = exportedStems[qsizetype(index)].toObject().value("sha256").toString().toLatin1();
            if (rawBefore.st_size < 0 || std::uint64_t(rawBefore.st_size) != inputBytes ||
                scan(rawStem.get(), inputBytes, stop).sha256 != rawDigest || !unchanged(rawBefore, statusOf(rawStem.get())))
                throw std::invalid_argument("Raw stem bank geometry or digest differs");
            auto converted = audio::prepareStemOutput(rawStem.get(), inputFrames, std::size_t(convertedBytes), stop,
                std::filesystem::path(QFile::encodeName(jobPath).constData()));
            verifySource(rawStem.get(), rawBefore, rawDigest, stop);
            if (!index) frames = converted.frames();
            if (converted.frames() != frames) throw std::invalid_argument("Converted stem bank frame counts differ");
            Descriptor destination(openat(stage.directory(), stemNames[index], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (destination.get() < 0) ioError("Cannot create prepared stem PCM");
            const auto copied = scan(converted.descriptor(), frames * frameBytes, stop, frames, destination.get());
            if (fsync(destination.get()) != 0) ioError("Cannot synchronize prepared stem PCM");
            Descriptor readable(openCacheFile(stage.directory(), stemNames[index]));
            auto persisted = audio::reopenPreparedAudio(readable.get(), frames, converted.peaks(), limits_.maxPreparedBytes);
            const auto observed = scan(persisted.descriptor(), frames * frameBytes, stop, frames);
            if (observed.sha256 != copied.sha256 || !std::ranges::equal(observed.peaks, converted.peaks()))
                throw std::runtime_error("Prepared stem copy digest or overview differs");
            stems.append(QJsonObject{{"name", QString::fromLatin1(stemRoles[index])}, {"file", QString::fromLatin1(stemNames[index])},
                {"bytes", double(frames * frameBytes)}, {"sha256", QString::fromLatin1(observed.sha256)}});
            prepared.push_back(std::move(persisted));
        }
        const QJsonObject object{{"schema", 2}, {"kind", "stem_bank"}, {"source_sha256", QString::fromLatin1(sourceId)},
            {"artifact_key", QString::fromLatin1(key)}, {"pipeline", pipeline}, {"frames", double(frames)},
            {"pcm_bytes", double(frames * frameBytes * 4)}, {"stems", stems}, {"export", exported}};
        const auto manifest = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (std::size_t(manifest.size()) > limits_.maxManifestBytes) throw std::length_error("Prepared stem manifest byte budget exceeded");
        Descriptor metadata(openat(stage.directory(), "manifest.json", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (metadata.get() < 0) ioError("Cannot create prepared stem manifest");
        writeAll(metadata.get(), manifest.constData(), std::size_t(manifest.size()), stop);
        if (fsync(metadata.get()) != 0) ioError("Cannot synchronize prepared stem manifest");
        verifySource(source.get(), sourceBefore, sourceId, stop);
        verifyEngine(files, stop);
        checkStop(stop);
        rawJobContents(raw.get(), true, true);
        if (unlinkat(stage.directory(), "raw", AT_REMOVEDIR) != 0 || unlinkat(stage.directory(), "input.f32", 0) != 0)
            ioError("Cannot remove completed stem-job scratch");
        if (unlinkat(stage.directory(), ".raw.export.lock", 0) != 0 && errno != ENOENT) ioError("Cannot remove completed export lock");
        checkEntryContents(stage.directory());
        stage.publish(key);
        return {{{std::move(prepared[0]), std::move(prepared[1]), std::move(prepared[2]), std::move(prepared[3])}},
            QString::fromLatin1(sourceId), QString::fromLatin1(key), false};
    } catch (const std::exception& error) {
        if (!groupsGone) {
            stage.preserve();
            throw std::runtime_error(std::string(error.what()) + "; owned exporter cleanup unproven; private job retained");
        }
        stage.allowNativeCleanup();
        throw;
    } catch (...) {
        if (groupsGone) stage.allowNativeCleanup();
        else stage.preserve();
        throw;
    }
}

} // namespace conflux::library

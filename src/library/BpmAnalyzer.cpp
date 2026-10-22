// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/BpmAnalyzer.hpp"
#include "library/StemEngineSupport.hpp"
#include "library/StemServiceJsonl.hpp"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <exception>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace conflux::library {
namespace {
using Clock = std::chrono::steady_clock;
constexpr qsizetype maximumHeader = 4096, maximumOutput = 4 * 1024 * 1024, maximumDiagnostics = 65536;
constexpr std::uint64_t maximumEvents = 34953;
constexpr qsizetype maximumManifest = 16 * 1024;
constexpr std::uint64_t maximumCacheBytes = 256ULL * 1024 * 1024;
constexpr std::size_t maximumCacheEntries = 64;

QString asset(const QString& path, bool directory = false, bool executable = false)
{
    const QFileInfo info(path);
    if (!info.isAbsolute() || path.contains(QChar(0)) ||
        (directory ? !info.isDir() : !info.isFile()) || (executable && !info.isExecutable()))
        throw std::invalid_argument("BPM engine requires absolute existing local assets");
    const auto canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) throw std::invalid_argument("Cannot resolve local BPM engine asset");
    // A venv interpreter is commonly a symlink. Executing its canonical
    // system target would discard the adjacent pyvenv.cfg/site-packages.
    return executable ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

void deadline(Clock::time_point end, std::stop_token stop)
{
    detail::checkStop(stop);
    if (Clock::now() >= end) throw std::runtime_error("BPM analysis deadline exceeded");
}

// These are the source buffers executed by the isolated worker. The worker
// also checks dependency versions on a miss; this is deliberately a critical
// producer closure, not a claim to hash every installed Python package.
constexpr std::array sourceFiles{
    "beat_this/utils.py", "beat_this/model/roformer.py", "beat_this/model/beat_tracker.py",
    "beat_this/model/postprocessor.py", "beat_this/preprocessing.py", "beat_this/inference.py", "LICENSE"};

struct Producer {
    std::vector<detail::EngineFile> files;
    std::vector<QString> absent;
    QJsonObject identity;
};

Producer producer(const BpmEngine& engine, std::uint64_t frames, const QByteArray& digest,
                  Clock::time_point end, std::stop_token stop)
{
    Producer result;
    QJsonArray rows;
    const auto add = [&](const QString& role, const QString& path, bool small) {
        deadline(end, stop);
        auto file = detail::pinStemFile(path, small, stop);
        rows.append(QJsonObject{{"role", role}, {"invocation", file.invocation}, {"path", file.path},
                                {"bytes", file.identity.value("bytes")}, {"sha256", file.identity.value("sha256")}});
        result.files.push_back(std::move(file));
    };
    const auto python = asset(engine.pythonExecutable, false, true);
    const auto source = asset(engine.sourceDirectory, true);
    add(QStringLiteral("python"), python, false);
    (void)asset(engine.workerPath);
    (void)asset(engine.modelPath);
    add(QStringLiteral("worker"), engine.workerPath, true);
    add(QStringLiteral("model"), engine.modelPath, false);
    for (const auto* relative : sourceFiles)
        add(QString::fromLatin1(relative), QDir(engine.sourceDirectory).filePath(QString::fromLatin1(relative)), true);
    // CPython searches the executable directory and its parent for pyvenv.cfg.
    // Preserve invocation spelling: resolving a venv symlink loses its prefix.
    const QDir binaryDirectory(QFileInfo(python).absolutePath());
    QJsonArray missing;
    for (const auto& path : {binaryDirectory.filePath(QStringLiteral("pyvenv.cfg")),
                            QDir::cleanPath(binaryDirectory.filePath(QStringLiteral("../pyvenv.cfg")))}) {
        struct stat status{};
        if (lstat(QFile::encodeName(path).constData(), &status) == 0) {
            add(QStringLiteral("venv"), path, true);
        } else if (errno == ENOENT) {
            result.absent.push_back(path);
            missing.append(path);
        } else detail::ioError("Cannot inspect BPM interpreter configuration");
    }
    result.identity = {{"schema", 1}, {"recipe", QString::fromLatin1(bpmAnalysisRecipe)},
        {"wire_schema", 2}, {"frames", double(frames)}, {"pcm_sha256", QString::fromLatin1(digest)},
        {"format", QStringLiteral("stereo-f32le-48000")}, {"source_directory", source},
        {"files", rows}, {"absent_venv", missing}};
    if (QJsonDocument(result.identity).toJson(QJsonDocument::Compact).size() + 256 > maximumManifest)
        throw std::length_error("BPM producer identity exceeds the cache manifest budget");
    return result;
}

void verifyProducer(const Producer& producer, Clock::time_point end, std::stop_token stop)
{
    for (const auto& file : producer.files) {
        deadline(end, stop);
        detail::verifyStemFile(file, stop);
    }
    for (const auto& path : producer.absent) {
        struct stat status{};
        if (lstat(QFile::encodeName(path).constData(), &status) == 0 || errno != ENOENT)
            throw std::runtime_error("BPM interpreter configuration changed during analysis");
    }
    deadline(end, stop);
}

bool privateDirectory(const struct stat& status)
{ return S_ISDIR(status.st_mode) && status.st_uid == getuid() && (status.st_mode & 0777) == 0700; }

bool privateFile(const struct stat& status)
{ return S_ISREG(status.st_mode) && status.st_uid == getuid() && status.st_nlink == 1 && (status.st_mode & 0777) == 0600; }

// Walk every component with NOFOLLOW, including when creating the separate
// analysis namespace. Never chmod an existing directory or repair foreign data.
std::unique_ptr<detail::Descriptor> cacheRoot(const QString& path, Clock::time_point end, std::stop_token stop)
{
    if (!QFileInfo(path).isAbsolute() || path.contains(QChar(0)) || path.size() > 4096 ||
        QDir::cleanPath(path) != path || path == QStringLiteral("/"))
        throw std::invalid_argument("BPM cache requires a clean absolute private directory");
    auto current = std::make_unique<detail::Descriptor>(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (current->get() < 0) detail::ioError("Cannot open BPM cache ancestor");
    const auto parts = path.split('/', Qt::SkipEmptyParts);
    for (const auto& part : parts) {
        deadline(end, stop);
        const auto name = QFile::encodeName(part);
        int opened = openat(current->get(), name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (opened < 0 && errno == ENOENT) {
            if (mkdirat(current->get(), name.constData(), 0700) != 0 && errno != EEXIST)
                detail::ioError("Cannot create private BPM cache directory");
            opened = openat(current->get(), name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        if (opened < 0) detail::ioError("Cannot open private BPM cache directory");
        current = std::make_unique<detail::Descriptor>(opened);
    }
    if (!privateDirectory(detail::statusOf(current->get())))
        throw std::runtime_error("BPM cache directory is not owned and private");
    return current;
}

class CacheLock {
public:
    explicit CacheLock(int root)
        : file_(openat(root, ".lock", O_RDWR | O_CREAT | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600))
    {
        if (file_.get() < 0) throw std::runtime_error("BPM analysis cache is unavailable or busy");
        const auto status = detail::statusOf(file_.get());
        if (!privateFile(status) || status.st_size != 0 ||
            !detail::unchanged(status, detail::childStatus(root, ".lock")) ||
            flock(file_.get(), LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("BPM analysis cache is unavailable or busy");
    }
private:
    detail::Descriptor file_;
};

bool stageName(const std::string& name)
{
    return name.size() == 23 && name.starts_with(".stage-") &&
        std::all_of(name.begin() + 7, name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

struct CacheUsage { std::size_t entries = 0; std::uint64_t bytes = 0; };
CacheUsage cacheUsage(int root, Clock::time_point end, std::stop_token stop)
{
    CacheUsage usage;
    for (const auto& name : detail::directoryNames(root, maximumCacheEntries + 1, stop)) {
        deadline(end, stop);
        if (name == ".lock") continue;
        const auto status = detail::childStatus(root, name);
        if ((!detail::hexDigest(QByteArray::fromStdString(name)) && !stageName(name)) || !privateDirectory(status))
            throw std::runtime_error("Unsafe BPM cache entry");
        if (++usage.entries > maximumCacheEntries) throw std::length_error("BPM cache entry budget exceeded");
        detail::Descriptor entry(openat(root, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (entry.get() < 0) detail::ioError("Cannot open BPM cache entry");
        for (const auto& leaf : detail::directoryNames(entry.get(), 2, stop)) {
            const auto file = detail::childStatus(entry.get(), leaf);
            const auto maximum = leaf == "timing.jsonl" ? maximumOutput : leaf == "manifest.json" ? maximumManifest : 0;
            if (!maximum || !privateFile(file) || file.st_size < 0 || file.st_size > maximum)
                throw std::runtime_error("Unsafe or oversized BPM cache file");
            if (std::uint64_t(file.st_size) > maximumCacheBytes - usage.bytes)
                throw std::length_error("BPM cache byte budget exceeded");
            usage.bytes += std::uint64_t(file.st_size);
        }
    }
    return usage;
}

QByteArray cacheBytes(int directory, const char* name, qsizetype maximum,
                      Clock::time_point end, std::stop_token stop)
{
    detail::Descriptor file(openat(directory, name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) detail::ioError("Cannot open BPM cache file");
    const auto before = detail::statusOf(file.get());
    if (!privateFile(before) || before.st_size <= 0 || before.st_size > maximum)
        throw std::runtime_error("BPM cache file geometry differs");
    QByteArray raw(qsizetype(before.st_size), Qt::Uninitialized);
    for (qsizetype offset = 0; offset < raw.size();) {
        deadline(end, stop);
        const auto count = pread(file.get(), raw.data() + offset, std::size_t(raw.size() - offset), off_t(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("BPM cache file changed or was truncated");
        offset += qsizetype(count);
    }
    const auto named = detail::childStatus(directory, name);
    if (!detail::unchanged(before, detail::statusOf(file.get())) || !detail::unchanged(before, named))
        throw std::runtime_error("BPM cache file changed while reading");
    return raw;
}

QByteArray cacheKey(const QJsonObject& identity)
{ return QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex(); }

std::optional<QByteArray> cachedTiming(int root, const QJsonObject& identity,
                                     Clock::time_point end, std::stop_token stop)
{
    CacheLock lock(root);
    (void)cacheUsage(root, end, stop);
    const auto key = cacheKey(identity);
    detail::Descriptor entry(openat(root, key.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (entry.get() < 0) {
        if (errno == ENOENT) return {};
        detail::ioError("Cannot open BPM analysis artifact");
    }
    const auto metadata = cacheBytes(entry.get(), "manifest.json", maximumManifest, end, stop);
    detail::StemServiceJsonl parser;
    const auto parsed = parser.feed(metadata);
    parser.finish();
    if (!parsed.object || parsed.consumed != metadata.size()) throw std::runtime_error("Invalid BPM cache manifest");
    const auto manifest = *parsed.object;
    const auto timing = cacheBytes(entry.get(), "timing.jsonl", maximumOutput, end, stop);
    if (manifest.size() != 4 || manifest.value("schema") != QJsonValue(1) ||
        manifest.value("identity") != QJsonValue(identity) ||
        manifest.value("timing_bytes") != QJsonValue(double(timing.size())) ||
        manifest.value("timing_sha256").toString().toLatin1() != QCryptographicHash::hash(timing, QCryptographicHash::Sha256).toHex())
        throw std::runtime_error("BPM cache artifact identity differs");
    return timing;
}

class CacheStage {
public:
    explicit CacheStage(int root) : root_(root), name_(".stage-" +
        QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0'))
    {
        if (mkdirat(root_, name_.constData(), 0700) != 0) detail::ioError("Cannot create BPM cache stage");
        detail::Descriptor opened(openat(root_, name_.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (opened.get() < 0) detail::ioError("Cannot open BPM cache stage");
        identity_ = detail::statusOf(opened.get());
        if (!privateDirectory(identity_)) throw std::runtime_error("Unsafe BPM cache stage");
        directory_ = opened.release();
    }
    ~CacheStage()
    {
        if (directory_ < 0) return;
        if (!published_) {
            // Only our unpredictable stage, never stale entries or other files.
            (void)unlinkat(directory_, "timing.jsonl", 0);
            (void)unlinkat(directory_, "manifest.json", 0);
            struct stat current{};
            if (fstatat(root_, name_.constData(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
                current.st_dev == identity_.st_dev && current.st_ino == identity_.st_ino)
                (void)unlinkat(root_, name_.constData(), AT_REMOVEDIR);
        }
        close(directory_);
    }
    void write(const char* name, const QByteArray& bytes, Clock::time_point end, std::stop_token stop)
    {
        deadline(end, stop);
        detail::Descriptor file(openat(directory_, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (file.get() < 0) detail::ioError("Cannot create BPM cache artifact");
        detail::writeAll(file.get(), bytes.constData(), std::size_t(bytes.size()), stop);
        if (fsync(file.get()) != 0) detail::ioError("Cannot synchronize BPM cache artifact");
        deadline(end, stop);
    }
    void publish(const QByteArray& key, Clock::time_point end, std::stop_token stop)
    {
        deadline(end, stop);
        const auto named = detail::childStatus(root_, name_.toStdString());
        if (named.st_dev != identity_.st_dev || named.st_ino != identity_.st_ino || !privateDirectory(named))
            throw std::runtime_error("BPM cache staging path changed");
        // Do not replace even an empty directory that appeared after lookup.
        if (fsync(directory_) != 0 || syscall(SYS_renameat2, root_, name_.constData(), root_, key.constData(), RENAME_NOREPLACE) != 0)
            detail::ioError("Cannot publish BPM cache artifact");
        published_ = true;
        if (fsync(root_) != 0) detail::ioError("Cannot synchronize BPM cache directory");
    }
private:
    int root_, directory_ = -1;
    QByteArray name_;
    struct stat identity_{};
    bool published_ = false;
};

void cacheTiming(int root, const QJsonObject& identity, const QByteArray& timing,
                 Clock::time_point end, std::stop_token stop)
{
    CacheLock lock(root);
    const auto usage = cacheUsage(root, end, stop);
    const auto key = cacheKey(identity);
    struct stat existing{};
    if (fstatat(root, key.constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0) return;
    if (errno != ENOENT) detail::ioError("Cannot inspect BPM cache destination");
    const auto metadata = QJsonDocument(QJsonObject{{"schema", 1}, {"identity", identity},
        {"timing_bytes", double(timing.size())}, {"timing_sha256", QString::fromLatin1(
            QCryptographicHash::hash(timing, QCryptographicHash::Sha256).toHex())}}).toJson(QJsonDocument::Compact) + '\n';
    if (timing.size() > maximumOutput || metadata.size() > maximumManifest || usage.entries >= maximumCacheEntries ||
        std::uint64_t(timing.size() + metadata.size()) > maximumCacheBytes - usage.bytes)
        throw std::length_error("BPM cache lacks bounded publication headroom");
    CacheStage stage(root);
    stage.write("timing.jsonl", timing, end, stop);
    stage.write("manifest.json", metadata, end, stop);
    stage.publish(key, end, stop);
}

void collect(QProcess& process, QProcess::ProcessChannel channel, QByteArray& retained, qsizetype maximum)
{
    process.setReadChannel(channel);
    // Do not retain an unbounded readAll() copy when a worker floods either
    // channel. QProcess's internal transport storage is separate from this cap.
    const auto available = process.bytesAvailable();
    if (available > maximum - retained.size())
        throw std::length_error(channel == QProcess::StandardOutput ?
            "BPM timing output exceeds 4 MiB" : "BPM diagnostics exceed 64 KiB");
    retained += process.read(maximum - retained.size() + 1);
    if (retained.size() > maximum)
        throw std::length_error("BPM worker channel exceeded its byte budget");
}

BpmEstimate timingResult(const QByteArray& bytes, std::uint64_t frames, const QByteArray& digest,
                         Clock::time_point end, std::stop_token stop)
{
    // Keep actual JSONL boundaries. No implicit final LF, blank records or
    // newline-flattening repair is permitted by schema 2.
    const auto newline = bytes.indexOf('\n');
    if (newline < 0 || newline + 1 > maximumHeader)
        throw std::runtime_error("BPM timing header exceeds its line budget or lacks LF");
    detail::StemServiceJsonl parser;
    qsizetype offset = 0;
    const auto next = [&]() {
        deadline(end, stop);
        const auto parsed = parser.feed(QByteArrayView(bytes).sliced(offset));
        offset += parsed.consumed;
        if (!parsed.object) {
            parser.finish(); // Rejects an unterminated body even if its JSON is valid.
            throw std::runtime_error("BPM timing record is missing");
        }
        return *parsed.object;
    };
    const auto result = next();
    if (result.size() != 6 || result.value("schema") != QJsonValue(2) ||
        result.value("recipe") != QJsonValue(QString::fromLatin1(bpmAnalysisRecipe)) ||
        !result.value("frames").isDouble() || result.value("frames").toDouble() != double(frames) ||
        result.value("pcm_sha256").toString().toLatin1() != digest ||
        !result.value("beat_count").isDouble())
        throw std::runtime_error("BPM result identity or schema differs");
    const auto count = result.value("beat_count").toDouble();
    const auto maximumCount = std::min(maximumEvents, frames * 50U / 48000U + 1U);
    if (!std::isfinite(count) || count < 0 || count > double(maximumCount) || std::floor(count) != count)
        throw std::runtime_error("BPM result beat count is invalid");
    BpmEstimate estimate;
    const auto value = result.value("bpm");
    if (value.isNull()) {
        estimate.status = QStringLiteral("Unknown");
    } else {
        const double bpm = value.toDouble();
        if (!value.isDouble() || !std::isfinite(bpm) || bpm < 20 || bpm > 400 || count < 2 ||
            std::round(bpm * 10) / 10 != bpm)
            throw std::runtime_error("BPM result estimate is invalid");
        estimate.bpm = bpm;
        estimate.status = QStringLiteral("Estimated");
    }
    estimate.analysisFrames = frames;
    estimate.pcmSha256 = digest;
    estimate.beats.reserve(std::size_t(count));
    for (std::size_t index = 0; index < std::size_t(count); ++index) {
        const auto event = next();
        const auto frameValue = event.value("frame");
        if (event.size() != 2 || !frameValue.isDouble() || !event.value("downbeat").isBool())
            throw std::runtime_error("BPM timing record schema differs");
        const double frame = frameValue.toDouble();
        if (!std::isfinite(frame) || frame < 0 || frame >= double(frames) || std::floor(frame) != frame ||
            (!estimate.beats.empty() && frame <= double(estimate.beats.back().frame)))
            throw std::runtime_error("BPM timing frames must be distinct, increasing and inside the source");
        estimate.beats.push_back({std::uint32_t(frame), event.value("downbeat").toBool()});
    }
    if (offset != bytes.size())
        throw std::runtime_error("BPM timing stream has extra records or bytes");
    parser.finish();
    deadline(end, stop);
    return estimate;
}

BpmEstimate analyze(const audio::PreparedAudio& audio, const BpmEngine& engine, std::stop_token stop,
                    const QString& cachePath = {})
{
    detail::checkStop(stop);
    if (engine.maxSeconds < 1 || engine.maxSeconds > 180)
        throw std::invalid_argument("BPM deadline must be between 1 and 180 seconds");
    const auto end = Clock::now() + std::chrono::seconds(engine.maxSeconds);
    if constexpr (std::endian::native != std::endian::little)
        throw std::runtime_error("BPM worker input requires little-endian float PCM");
    const auto frames = audio.frames();
    struct stat before{};
    QByteArray digest;
    try {
        const auto flags = fcntl(audio.descriptor(), F_GETFL);
        before = detail::statusOf(audio.descriptor());
        if (!frames || frames > conflux::audio::PreparedAudio::defaultMaxPreparedBytes / 8U ||
            flags < 0 || (flags & O_ACCMODE) == O_WRONLY || !S_ISREG(before.st_mode) ||
            before.st_size < 0 || std::uint64_t(before.st_size) != frames * 8U)
            throw BpmSourceError("BPM analysis requires exact bounded regular prepared PCM");
        digest = detail::scan(audio.descriptor(), frames * 8U, stop, frames).sha256;
        if (!detail::unchanged(before, detail::statusOf(audio.descriptor())))
            throw BpmSourceError("Prepared PCM changed during BPM input verification");
    } catch (const std::exception& error) {
        if (stop.stop_requested()) throw;
        throw BpmSourceError(error.what());
    }
    const auto verifyInput = [&] {
        try { detail::verifySource(audio.descriptor(), before, digest, stop); }
        catch (const std::exception& error) {
            if (stop.stop_requested()) throw;
            throw BpmSourceError(error.what());
        }
    };
    std::unique_ptr<Producer> inputs;
    std::unique_ptr<detail::Descriptor> cache;
    std::optional<BpmEstimate> hit;
    if (!cachePath.isEmpty()) {
        try {
            cache = cacheRoot(cachePath, end, stop);
            inputs = std::make_unique<Producer>(producer(engine, frames, digest, end, stop));
            if (const auto raw = cachedTiming(cache->get(), inputs->identity, end, stop)) {
                hit = timingResult(*raw, frames, digest, end, stop);
                verifyProducer(*inputs, end, stop);
            }
        } catch (...) {
            // An unavailable, foreign, corrupt or full cache never prevents
            // fresh analysis. Keep the same deadline and check cancellation.
            // Keep a completely pinned producer even if only the cache read
            // failed, so a subsequent worker cannot conceal changed inputs.
            hit.reset(); cache.reset();
        }
        if (hit) {
            verifyInput();
            deadline(end, stop);
            hit->cacheHit = true;
            return *hit;
        }
    }
    QByteArray output, diagnostics;
    std::exception_ptr workerFailure;
    bool normal = false;
    try {
        deadline(end, stop);
        const auto python = asset(engine.pythonExecutable, false, true);
        const auto worker = asset(engine.workerPath), model = asset(engine.modelPath);
        const auto source = asset(engine.sourceDirectory, true);
        bool groupsGone = false;
        detail::OwnedStemProcess owned(groupsGone);
        auto& process = owned.process();
        process.setChildProcessModifier([] {
            const rlimit core{0, 0};
            constexpr rlim_t addressBytes = 8ULL * 1024 * 1024 * 1024;
            const rlimit addressSpace{addressBytes, addressBytes};
            if (setsid() < 0 || setrlimit(RLIMIT_CORE, &core) != 0 ||
                setrlimit(RLIMIT_AS, &addressSpace) != 0) _exit(127);
        });
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto* key : {"OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS", "NUMEXPR_NUM_THREADS"})
            environment.insert(QString::fromLatin1(key), QStringLiteral("2"));
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.setStandardInputFile(QStringLiteral("/proc/self/fd/") + QString::number(audio.descriptor()));
        process.start(python, {"-I", "-B", worker, "--source-dir", source, "--model", model,
                               "--frames", QString::number(qulonglong(frames))});
        if (!process.waitForStarted(1000) || owned.group() <= 1)
            throw std::runtime_error("Cannot start the owned local BPM worker");
        while (process.state() != QProcess::NotRunning) {
            deadline(end, stop);
            collect(process, QProcess::StandardOutput, output, maximumOutput);
            collect(process, QProcess::StandardError, diagnostics, maximumDiagnostics);
            process.waitForFinished(20);
        }
        collect(process, QProcess::StandardOutput, output, maximumOutput);
        collect(process, QProcess::StandardError, diagnostics, maximumDiagnostics);
        normal = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (!owned.finish()) throw std::runtime_error("Cannot prove BPM process-group cleanup");
        if (inputs) verifyProducer(*inputs, end, stop);
    } catch (...) {
        workerFailure = std::current_exception(); // Owned process cleanup has already run.
    }
    // Even an optional worker failure must not conceal changed/corrupt source
    // bytes. Cancellation takes precedence and is discarded by the load gate.
    verifyInput();
    deadline(end, stop);
    if (workerFailure) std::rethrow_exception(workerFailure);
    if (!normal) throw std::runtime_error("Local BPM analysis failed: " + diagnostics.right(2048).toStdString());
    auto result = timingResult(output, frames, digest, end, stop);
    if (cache && inputs) {
        try { cacheTiming(cache->get(), inputs->identity, output, end, stop); }
        catch (...) { /* Optional publication; never replace/delete existing data. */ }
        // Publication performs I/O too. Do not hide a source/producer mutation
        // that happened after the first final verification.
        verifyInput();
        verifyProducer(*inputs, end, stop);
    }
    return result;
}
} // namespace

BpmEstimate analyzeBpm(const audio::PreparedAudio& audio, const BpmEngine& engine, std::stop_token stop)
{
    try { return analyze(audio, engine, stop); }
    catch (const BpmSourceError&) {
        if (stop.stop_requested()) throw std::runtime_error("BPM analysis cancelled");
        throw;
    }
    catch (const std::exception& error) {
        if (stop.stop_requested()) throw std::runtime_error("BPM analysis cancelled");
        return {{}, QStringLiteral("Failed"), QString::fromUtf8(error.what()).left(2048)};
    } catch (...) {
        if (stop.stop_requested()) throw std::runtime_error("BPM analysis cancelled");
        return {{}, QStringLiteral("Failed"), QStringLiteral("BPM analysis failed")};
    }
}

std::optional<BpmTempoInterval> localTempoAt(const BpmEstimate& estimate, std::uint64_t sourceFrame) noexcept
{
    const auto frames = estimate.analysisFrames;
    if (!frames || frames > audio::PreparedAudio::defaultMaxPreparedBytes / 8U || sourceFrame >= frames ||
        estimate.beats.size() < 2 || estimate.beats.size() > std::min(maximumEvents, frames * 50U / 48000U + 1U))
        return {};
    auto previous = estimate.beats.front().frame;
    if (previous >= frames) return {};
    std::optional<BpmTempoInterval> selected;
    for (std::size_t index = 1; index < estimate.beats.size(); ++index) {
        const auto current = estimate.beats[index].frame;
        if (current >= frames || current <= previous) return {};
        if (sourceFrame >= previous && sourceFrame < current) {
            const double bpm = 60. * 48000. / (current - previous);
            if (std::isfinite(bpm) && bpm >= 20 && bpm <= 400)
                selected = BpmTempoInterval{previous, current, bpm};
        }
        previous = current;
    }
    return selected;
}

std::optional<double> tempoPercentForBpm(double reference, double target) noexcept
{
    if (!std::isfinite(reference) || reference < 20 || reference > 400 || !std::isfinite(target) ||
        target < reference * .94 || target > reference * 1.06)
        return {};
    return std::clamp(100 * (target / reference - 1), -6., 6.);
}

BpmEstimate analyzeBpmCached(const audio::PreparedAudio& audio, const BpmEngine& engine,
                             const QString& absoluteCacheRoot, std::stop_token stop)
{
    try { return analyze(audio, engine, stop, absoluteCacheRoot); }
    catch (const BpmSourceError&) {
        if (stop.stop_requested()) throw std::runtime_error("BPM analysis cancelled");
        throw;
    }
    catch (const std::exception& error) {
        if (stop.stop_requested()) throw std::runtime_error("BPM analysis cancelled");
        return {{}, QStringLiteral("Failed"), QString::fromUtf8(error.what()).left(2048)};
    } catch (...) {
        if (stop.stop_requested()) throw std::runtime_error("BPM analysis cancelled");
        return {{}, QStringLiteral("Failed"), QStringLiteral("BPM analysis failed")};
    }
}

} // namespace conflux::library

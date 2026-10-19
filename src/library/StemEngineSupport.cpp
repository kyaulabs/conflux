// SPDX-License-Identifier: AGPL-3.0-or-later
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

namespace conflux::library::detail {
constexpr std::uint64_t frameBytes = 8;
constexpr std::array stemRoles{"drums", "bass", "other", "vocals"};
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

// Read exactly the captured size, never chase a growing source to EOF. All
// callers own the descriptor throughout; pread does not consume decoder offset.
Scan scan(int descriptor, std::uint64_t bytes, std::stop_token stop,
          std::uint64_t frames, int copyTo, bool littleEndian)
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
                auto sample = [&](std::size_t position) {
                    if (!littleEndian || std::endian::native == std::endian::little) return buffer[position];
                    const auto* raw = reinterpret_cast<const unsigned char*>(buffer.data()) + position * 4;
                    const auto bits = std::uint32_t(raw[0]) | (std::uint32_t(raw[1]) << 8) |
                        (std::uint32_t(raw[2]) << 16) | (std::uint32_t(raw[3]) << 24);
                    return std::bit_cast<float>(bits);
                };
                const auto left = sample(index * 2), right = sample(index * 2 + 1);
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

void verifySource(int descriptor, const struct stat& before, const QByteArray& digest, std::stop_token stop)
{
    if (!unchanged(before, statusOf(descriptor))) throw std::runtime_error("Audio source changed during preparation");
    const auto observed = scan(descriptor, std::uint64_t(before.st_size), stop).sha256;
    if (observed != digest || !unchanged(before, statusOf(descriptor)))
        throw std::runtime_error("Audio source changed during preparation");
}

EngineFile pinStemFile(const QString& path, bool small, std::stop_token stop)
{
    EngineFile file;
    file.path = path;
    if (!QFileInfo(path).isAbsolute() || path.contains(QChar(u'\0')))
        throw std::invalid_argument("Stem engine files require absolute local paths");
    file.invocation = path;
    file.path = QFileInfo(path).canonicalFilePath();
    if (file.path.isEmpty()) throw std::runtime_error("Cannot resolve local stem engine file");
    file.descriptor = std::make_unique<Descriptor>(open(QFile::encodeName(file.path).constData(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (file.descriptor->get() < 0) ioError("Cannot open local stem engine file");
    file.before = statusOf(file.descriptor->get());
    const std::uint64_t maximum = small ? 1024ULL * 1024 : 512ULL * 1024 * 1024;
    if (!S_ISREG(file.before.st_mode) || file.before.st_size <= 0 || std::uint64_t(file.before.st_size) > maximum)
        throw std::invalid_argument("Local stem engine files must be bounded regular files");
    const auto digest = scan(file.descriptor->get(), std::uint64_t(file.before.st_size), stop).sha256;
    if (!unchanged(file.before, statusOf(file.descriptor->get()))) throw std::runtime_error("Stem engine changed during hashing");
    file.identity = {{"sha256", QString::fromLatin1(digest)}, {"bytes", double(file.before.st_size)}};
    return file;
}
EngineFiles pinEngine(const StemEngine& engine, std::stop_token stop)
{
    if ((engine.backend != "vulkan" && engine.backend != "cpu") || engine.threads < 1 || engine.threads > 64 ||
        engine.maxSeconds < 1 || engine.maxSeconds > 7200 || engine.requestTimeout < 1 || engine.requestTimeout > 30 ||
        !engine.maxDiagnosticBytes || engine.maxDiagnosticBytes > 65536)
        throw std::invalid_argument("Invalid local stem engine limits");
    EngineFiles files;
    const std::array paths{engine.pythonExecutable, engine.exporterPath, engine.workerPath, engine.runtimePath, engine.modelPath};
    for (std::size_t index = 0; index < files.size(); ++index)
        files[index] = pinStemFile(paths[index], index == 1 || index == 2, stop);
    return files;
}
void verifyStemFile(const EngineFile& file, std::stop_token stop)
{
    if (QFileInfo(file.invocation).canonicalFilePath() != file.path)
        throw std::runtime_error("Stem engine invocation alias changed during preparation");
    if (!unchanged(file.before, statusOf(file.descriptor->get())) ||
        scan(file.descriptor->get(), std::uint64_t(file.before.st_size), stop).sha256 !=
            file.identity.value("sha256").toString().toLatin1() ||
        !unchanged(file.before, statusOf(file.descriptor->get())))
        throw std::runtime_error("Stem engine changed during preparation");
    struct stat current{};
    if (lstat(QFile::encodeName(file.path).constData(), &current) != 0 || !unchanged(file.before, current))
        throw std::runtime_error("Stem engine path changed during preparation");
}
void verifyEngine(const EngineFiles& files, std::stop_token stop)
{ for (const auto& file : files) verifyStemFile(file, stop); }

OwnedStemProcess::OwnedStemProcess(bool& groupsGone) : groupsGone_(groupsGone)
{
    process_.setChildProcessModifier([] { if (setsid() < 0) _exit(127); });
    QObject::connect(&process_, &QProcess::started, &process_, [this] {
        group_ = process_.processId(); started_ = true; groupsGone_ = false;
    }, Qt::DirectConnection);
}
OwnedStemProcess::~OwnedStemProcess()
{ (void)finish(); QObject::disconnect(&process_, nullptr, &process_, nullptr); }
bool OwnedStemProcess::finish() noexcept
    {
        if (finished_) return groupsGone_;
        finished_ = true;
        if (group_ > 1) (void)kill(-pid_t(group_), SIGTERM);
        if (process_.state() != QProcess::NotRunning) process_.waitForFinished(250);
        // Descendants remain owned even if their direct leader already exited.
        if (group_ > 1) (void)kill(-pid_t(group_), SIGKILL);
        if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(2000); }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (group_ > 1 && (kill(-pid_t(group_), 0) == 0 || errno != ESRCH)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        groupsGone_ = process_.state() == QProcess::NotRunning && (!started_ || group_ > 1);
        return groupsGone_;
    }

void validateDescription(const QJsonObject& object, const EngineFiles& files)
{
    const auto version = object.value("numpy_version").toString();
    const QJsonObject input{{"format", "f32le"}, {"sample_rate", 44100}, {"channels", 2}};
    if (object.size() != 10 || object.value("schema") != QJsonValue(1) ||
        object.value("helper_sha256") != files[1].identity.value("sha256") ||
        object.value("worker_sha256") != files[2].identity.value("sha256") || version.isEmpty() || version.size() > 128 ||
        object.value("input").toObject() != input || object.value("window_frames") != QJsonValue(343980) ||
        object.value("hop_frames") != QJsonValue(257985) || object.value("overlap_frames") != QJsonValue(85995) ||
        object.value("normalization") != QJsonValue("track-mono-sample-variance-f32-v1") ||
        object.value("overlap_add") != QJsonValue("fixed-grid-triangle-f32-v1"))
        throw std::invalid_argument("Stem exporter description differs from its fixed contract or source identities");
}

void validateRuntimeReady(const QJsonObject& ready, const StemEngine& engine)
{
    QJsonArray roles;
    for (const auto* role : stemRoles) roles.append(QString::fromLatin1(role));
    const auto revision = ready.value("revision").toString().toLatin1();
    const bool revisionValid = revision.size() == 40 && std::ranges::all_of(revision, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
    if (ready.size() != 9 || ready.value("sample_rate") != QJsonValue(44100) || ready.value("channels") != QJsonValue(2) ||
        ready.value("segment_frames") != QJsonValue(343980) || ready.value("stems").toArray() != roles ||
        ready.value("backend") != QJsonValue(engine.backend) || ready.value("threads") != QJsonValue(engine.threads) ||
        !revisionValid || (ready.value("weight_storage") != QJsonValue("native") && ready.value("weight_storage") != QJsonValue("f32")) ||
        (ready.value("fence_wait_mode") != QJsonValue("blocking") && ready.value("fence_wait_mode") != QJsonValue("upstream-hybrid")))
        throw std::invalid_argument("Stem export runtime ready contract differs");
}

} // namespace conflux::library::detail

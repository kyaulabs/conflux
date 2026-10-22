// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/BpmAnalyzer.hpp"
#include "library/StemEngineSupport.hpp"
#include "library/StemServiceJsonl.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <exception>
#include <fcntl.h>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace conflux::library {
namespace {
using Clock = std::chrono::steady_clock;
constexpr qsizetype maximumOutput = 4096, maximumDiagnostics = 65536;

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

void collect(QProcess& process, QProcess::ProcessChannel channel, QByteArray& retained, qsizetype maximum)
{
    process.setReadChannel(channel);
    // Do not retain an unbounded readAll() copy when a worker floods either
    // channel. QProcess's internal transport storage is separate from this cap.
    const auto available = process.bytesAvailable();
    if (available > maximum - retained.size())
        throw std::length_error(channel == QProcess::StandardOutput ?
            "BPM output exceeds 4096 bytes" : "BPM diagnostics exceed 64 KiB");
    retained += process.read(maximum - retained.size() + 1);
    if (retained.size() > maximum)
        throw std::length_error("BPM worker channel exceeded its byte budget");
}

QJsonObject resultObject(QByteArray bytes)
{
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) throw std::runtime_error("Malformed BPM result JSON");
    // Reuse the existing strict duplicate-key/UTF-8/number parser. After a
    // successful JSON parse physical CR/LF can only be whitespace outside
    // strings; replacing that whitespace does not change any JSON value.
    bytes.replace('\r', ' ');
    bytes.replace('\n', ' ');
    bytes += '\n';
    detail::StemServiceJsonl parser;
    auto parsed = parser.feed(bytes);
    if (!parsed.object || parsed.consumed != bytes.size())
        throw std::runtime_error("BPM result must contain exactly one JSON object");
    parser.finish();
    return std::move(*parsed.object);
}

BpmEstimate analyze(const audio::PreparedAudio& audio, const BpmEngine& engine, std::stop_token stop)
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
    } catch (...) {
        workerFailure = std::current_exception(); // Owned process cleanup has already run.
    }
    // Even an optional worker failure must not conceal changed/corrupt source
    // bytes. Cancellation takes precedence and is discarded by the load gate.
    try {
        detail::verifySource(audio.descriptor(), before, digest, stop);
    } catch (const std::exception& error) {
        if (stop.stop_requested()) throw;
        throw BpmSourceError(error.what());
    }
    deadline(end, stop);
    if (workerFailure) std::rethrow_exception(workerFailure);
    if (!normal) throw std::runtime_error("Local BPM analysis failed: " + diagnostics.right(2048).toStdString());
    const auto result = resultObject(std::move(output));
    if (result.size() != 6 || result.value("schema") != QJsonValue(1) ||
        result.value("recipe") != QJsonValue(QString::fromLatin1(bpmAnalysisRecipe)) ||
        !result.value("frames").isDouble() || result.value("frames").toDouble() != double(frames) ||
        result.value("pcm_sha256").toString().toLatin1() != digest ||
        !result.value("beat_count").isDouble())
        throw std::runtime_error("BPM result identity or schema differs");
    const auto count = result.value("beat_count").toDouble();
    const auto maximumCount = std::min<std::uint64_t>(60000, frames * 50U / 48000U + 1U);
    if (!std::isfinite(count) || count < 0 || count > double(maximumCount) || std::floor(count) != count)
        throw std::runtime_error("BPM result beat count is invalid");
    const auto value = result.value("bpm");
    if (value.isNull()) return {{}, QStringLiteral("Unknown"), {}};
    const double bpm = value.toDouble();
    if (!value.isDouble() || !std::isfinite(bpm) || bpm < 20 || bpm > 400 || count < 2 ||
        std::round(bpm * 10) / 10 != bpm)
        throw std::runtime_error("BPM result estimate is invalid");
    return {bpm, QStringLiteral("Estimated"), {}};
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

std::optional<double> tempoPercentForBpm(double reference, double target) noexcept
{
    if (!std::isfinite(reference) || reference < 20 || reference > 400 || !std::isfinite(target) ||
        target < reference * .94 || target > reference * 1.06)
        return {};
    return std::clamp(100 * (target / reference - 1), -6., 6.);
}

} // namespace conflux::library

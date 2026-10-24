// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/HarmonyAnalyzer.hpp"
#include "library/StemEngineSupport.hpp"
#include "library/StemServiceJsonl.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <exception>
#include <fcntl.h>
#include <sys/resource.h>

namespace conflux::library {
namespace {
using Clock = std::chrono::steady_clock;
constexpr qsizetype outputLimit = 128 * 1024, diagnosticLimit = 65536;
constexpr std::uint64_t maximumFrames = audio::PreparedAudio::defaultMaxPreparedBytes / 8U;
void current(Clock::time_point end, std::stop_token stop)
{
    detail::checkStop(stop);
    if (Clock::now() >= end) throw std::runtime_error("Harmony deadline exceeded");
}
QString asset(const QString& path, bool executable)
{
    const QFileInfo file(path);
    if (!file.isAbsolute() || path.contains(QChar(0)) || !file.isFile() || (executable && !file.isExecutable()))
        throw std::invalid_argument("Harmony engine requires absolute local assets");
    // Preserve venv invocation spelling, as in BpmAnalyzer.
    return executable ? QDir::cleanPath(file.absoluteFilePath()) : file.canonicalFilePath();
}
void collect(QProcess& process, QProcess::ProcessChannel channel, QByteArray& bytes, qsizetype maximum)
{
    process.setReadChannel(channel);
    if (process.bytesAvailable() > maximum - bytes.size()) throw std::length_error("Harmony channel exceeds its byte budget");
    bytes += process.read(maximum - bytes.size() + 1);
    if (bytes.size() > maximum) throw std::length_error("Harmony channel exceeds its byte budget");
}
bool digest(const QByteArray& value) { return detail::hexDigest(value); }
bool validWindow(const HarmonyWindow& window) noexcept
{
    double sum = 0;
    for (const auto value : window.chroma) {
        if (!std::isfinite(value) || value < 0 || value > 1) return false;
        sum += value;
    }
    if ((sum != 0 && std::abs(sum - 1) > 1e-8) || window.candidates.size() > 2) return false;
    for (std::size_t i = 0; i < window.candidates.size(); ++i) {
        const auto& key = window.candidates[i];
        if (key.tonic < 0 || key.tonic > 11 || (key.mode != "Major" && key.mode != "Minor") ||
            !std::isfinite(key.correlation) || key.correlation < -1 || key.correlation > 1 ||
            (i && (key.correlation > window.candidates[i-1].correlation ||
                (key.tonic == window.candidates[0].tonic && key.mode == window.candidates[0].mode)))) return false;
    }
    const auto length = window.endFrame - window.firstFrame;
    if (length < 32768)
        return window.status == "Unknown" && window.reason == "short" && window.candidates.empty() && sum == 0;
    if (sum == 0)
        return window.status == "Unknown" && window.reason == "silence" && window.candidates.empty();
    // Check observable frozen recipe admission without treating raw profile
    // correlation as confidence or recomputing features from the source here.
    const double maximum = *std::max_element(window.chroma.begin(), window.chroma.end()), mean = sum / 12;
    double logSum = 0, centeredNormSquared = 0;
    std::size_t occupied = 0;
    for (const auto value : window.chroma) {
        logSum += std::log(std::max(value, 1e-300));
        centeredNormSquared += (value - mean) * (value - mean);
        if (value >= maximum * .1) ++occupied;
    }
    if (std::exp(logSum / 12) / mean >= .9 || centeredNormSquared <= 0)
        return window.status == "Unknown" && window.reason == "non-tonal" && window.candidates.empty();
    if (window.candidates.size() != 2) return false;
    if (occupied < 3)
        return window.status == "Ambiguous" && window.reason == "insufficient-key-context";
    const bool matched = window.candidates[0].correlation >= .6 &&
        window.candidates[0].correlation - window.candidates[1].correlation >= .1;
    return matched ? window.status == "Estimated" && window.reason == "profile-match" :
        window.status == "Ambiguous" && window.reason == "competing-keys";
}
HarmonyEstimate result(const QByteArray& bytes, std::uint64_t frames, const QByteArray& hash,
                       Clock::time_point end, std::stop_token stop)
{
    detail::StemServiceJsonl parser;
    qsizetype offset = 0;
    const auto next = [&] {
        current(end, stop);
        const auto row = parser.feed(QByteArrayView(bytes).sliced(offset)); offset += row.consumed;
        if (!row.object) { parser.finish(); throw std::runtime_error("Harmony record missing"); }
        return *row.object;
    };
    const auto header = next();
    const auto count = (frames + harmonyWindowFrames - 1) / harmonyWindowFrames;
    if (header.size() != 8 || header.value("schema") != QJsonValue(1) ||
        header.value("recipe") != QJsonValue(QString::fromLatin1(harmonyAnalysisRecipe)) ||
        detail::jsonInteger(header.value("frames"), maximumFrames) != frames ||
        header.value("pcm_sha256").toString().toLatin1() != hash ||
        header.value("sample_rate") != QJsonValue(48000) || header.value("channels") != QJsonValue(2) ||
        detail::jsonInteger(header.value("window_frames"), harmonyWindowFrames) != harmonyWindowFrames ||
        detail::jsonInteger(header.value("window_count"), 175) != count)
        throw std::runtime_error("Harmony source identity or recipe differs");
    HarmonyEstimate estimate; estimate.status = "Estimated"; estimate.analysisFrames = frames; estimate.pcmSha256 = hash;
    estimate.windows.reserve(std::size_t(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto row = next();
        HarmonyWindow window;
        window.firstFrame = detail::jsonInteger(row.value("first_frame"), frames);
        window.endFrame = detail::jsonInteger(row.value("end_frame"), frames);
        window.status = row.value("status").toString(); window.reason = row.value("reason").toString();
        const auto chroma = row.value("chroma").toArray(), candidates = row.value("candidates").toArray();
        if (row.size() != 7 || window.firstFrame != index * harmonyWindowFrames ||
            window.endFrame != std::min(frames, window.firstFrame + harmonyWindowFrames) ||
            row.value("confidence") != QJsonValue("Unknown") || !row.value("chroma").isArray() || chroma.size() != 12 ||
            !row.value("candidates").isArray() || candidates.size() > 2)
            throw std::runtime_error("Harmony interval/schema differs");
        double sum = 0;
        for (qsizetype i = 0; i < chroma.size(); ++i) {
            const double value = chroma[i].toDouble(-1.);
            if (!chroma[i].isDouble() || !std::isfinite(value) || value < 0 || value > 1)
                throw std::runtime_error("Harmony chroma differs");
            window.chroma[std::size_t(i)] = value; sum += value;
        }
        if (sum != 0 && std::abs(sum - 1) > 1e-8) throw std::runtime_error("Harmony chroma normalization differs");
        for (const auto candidate : candidates) {
            if (!candidate.isObject()) throw std::runtime_error("Harmony candidate missing");
            const auto object = candidate.toObject();
            const auto tonic = detail::jsonInteger(object.value("tonic"), 11);
            const auto mode = object.value("mode").toString(); const auto score = object.value("correlation");
            if (object.size() != 3 || (mode != "Major" && mode != "Minor") || !score.isDouble() ||
                !std::isfinite(score.toDouble()) || score.toDouble() < -1 || score.toDouble() > 1 ||
                (!window.candidates.empty() && (score.toDouble() > window.candidates.back().correlation ||
                    (int(tonic) == window.candidates.front().tonic && mode == window.candidates.front().mode))))
                throw std::runtime_error("Harmony candidate/schema order differs");
            window.candidates.push_back({int(tonic), mode, score.toDouble()});
        }
        if (!validWindow(window)) throw std::runtime_error("Harmony evidence/status differs");
        estimate.windows.push_back(std::move(window));
    }
    const auto terminal = next();
    if (terminal.size() != 3 || terminal.value("schema") != QJsonValue(1) ||
        terminal.value("complete") != QJsonValue(true) ||
        detail::jsonInteger(terminal.value("window_count"), 175) != count || offset != bytes.size())
        throw std::runtime_error("Harmony completion differs");
    parser.finish(); current(end, stop); return estimate;
}
HarmonyEstimate analyze(const audio::PreparedAudio& audio, const HarmonyEngine& engine, std::stop_token stop)
{
    if (engine.maxSeconds < 1 || engine.maxSeconds > 30) throw std::invalid_argument("Harmony deadline must be1..30seconds");
    const auto end = Clock::now() + std::chrono::seconds(engine.maxSeconds); current(end, stop);
    if constexpr (std::endian::native != std::endian::little) throw std::runtime_error("Harmony needs little-endian PCM");
    const auto frames = audio.frames(); struct stat before{}; QByteArray hash;
    try {
        const auto flags = fcntl(audio.descriptor(), F_GETFL); before = detail::statusOf(audio.descriptor());
        if (!frames || frames > maximumFrames || flags < 0 || (flags & O_ACCMODE) == O_WRONLY ||
            !S_ISREG(before.st_mode) || before.st_size < 0 || std::uint64_t(before.st_size) != frames * 8U)
            throw HarmonySourceError("Harmony needs exact bounded regular prepared PCM");
        hash = detail::scan(audio.descriptor(), frames * 8U, stop, frames).sha256;
        if (!detail::unchanged(before, detail::statusOf(audio.descriptor()))) throw HarmonySourceError("Harmony PCM changed");
    } catch (const std::exception& error) {
        if (stop.stop_requested()) throw;
        throw HarmonySourceError(error.what());
    }
    QByteArray bytes, diagnostics; std::exception_ptr failure;
    try {
        current(end, stop);
        auto python = detail::pinStemFile(asset(engine.pythonExecutable, true), false, stop);
        auto worker = detail::pinStemFile(asset(engine.workerPath, false), true, stop);
        bool gone = false; detail::OwnedStemProcess owned(gone); auto& process = owned.process();
        process.setChildProcessModifier([] {
            const rlimit core{0, 0}, space{8ULL * 1024 * 1024 * 1024, 8ULL * 1024 * 1024 * 1024};
            if (setsid() < 0 || setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_AS, &space)) _exit(127);
        });
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto* key : {"OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS", "NUMEXPR_NUM_THREADS"}) environment.insert(QString::fromLatin1(key), QStringLiteral("1"));
        process.setProcessEnvironment(environment); process.setProcessChannelMode(QProcess::SeparateChannels);
        process.setStandardInputFile(QStringLiteral("/proc/self/fd/") + QString::number(audio.descriptor()));
        current(end, stop);
        process.start(python.invocation, {"-I", "-B", worker.path, "--frames", QString::number(qulonglong(frames))});
        if (!process.waitForStarted(1000) || owned.group() <= 1) throw std::runtime_error("Cannot start owned harmony worker");
        while (process.state() != QProcess::NotRunning) {
            current(end, stop); collect(process, QProcess::StandardOutput, bytes, outputLimit);
            collect(process, QProcess::StandardError, diagnostics, diagnosticLimit); process.waitForFinished(20);
        }
        collect(process, QProcess::StandardOutput, bytes, outputLimit); collect(process, QProcess::StandardError, diagnostics, diagnosticLimit);
        const bool normal = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (!owned.finish()) throw std::runtime_error("Cannot prove harmony process-group cleanup");
        detail::verifyStemFile(python, stop); detail::verifyStemFile(worker, stop); current(end, stop);
        if (!normal) throw std::runtime_error("Harmony worker failed: " + diagnostics.right(1024).toStdString());
    } catch (...) { failure = std::current_exception(); }
    try { detail::verifySource(audio.descriptor(), before, hash, stop); }
    catch (const std::exception& error) { if (stop.stop_requested()) throw; throw HarmonySourceError(error.what()); }
    current(end, stop);
    if (failure) std::rethrow_exception(failure);
    return result(bytes, frames, hash, end, stop);
}
} // namespace

HarmonyEstimate analyzeHarmony(const audio::PreparedAudio& audio, const HarmonyEngine& engine, std::stop_token stop)
{
    try { return analyze(audio, engine, stop); }
    catch (const HarmonySourceError&) { if (stop.stop_requested()) throw std::runtime_error("Harmony cancelled"); throw; }
    catch (const std::exception& error) {
        if (stop.stop_requested()) throw std::runtime_error("Harmony cancelled");
        return {"Failed", QString::fromUtf8(error.what()).left(2048), 0, {}, {}};
    }
}
const HarmonyWindow* localHarmonyAt(const HarmonyEstimate& estimate, std::uint64_t frame) noexcept
{
    const auto frames = estimate.analysisFrames;
    if (estimate.status != "Estimated" || !frames || frames > maximumFrames || frame >= frames || !digest(estimate.pcmSha256) ||
        estimate.windows.size() != (frames + harmonyWindowFrames - 1) / harmonyWindowFrames) return nullptr;
    for (std::size_t index = 0; index < estimate.windows.size(); ++index) {
        const auto& value = estimate.windows[index];
        if (value.firstFrame != index * harmonyWindowFrames || value.endFrame != std::min(frames, value.firstFrame + harmonyWindowFrames) || !validWindow(value)) return nullptr;
    }
    return &estimate.windows[std::size_t(frame / harmonyWindowFrames)];
}
} // namespace conflux::library

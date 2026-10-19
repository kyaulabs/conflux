// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/PreparedTrackStore.hpp"
#include "audio/PreparedPlayback.hpp"
#include "audio/RenderCore.hpp"
#include "audio/StemRateConverter.hpp"
#include "StemServiceFixture.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <future>
#include <filesystem>
#include <iostream>
#include <optional>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace {
using conflux::audio::PreparedAudio;
using conflux::audio::PreparedPlayback;
using conflux::audio::RenderCore;
using conflux::library::PreparedLiveStemTrack;
using conflux::library::PreparedTrackStore;
using conflux::test::StemServiceFixture;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t hop = 257985;
std::uint64_t nextToken() { static std::atomic<std::uint64_t> token{100}; return token.fetch_add(1) + 1; }

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
template<class F> void checked(F&& fn)
{ try { fn(); } catch (const std::exception& error) { QFAIL(error.what()); } }
template<class E, class F> bool rejects(F&& fn, const char* text = "")
{
    try { fn(); }
    catch (const std::exception& error) {
        return dynamic_cast<const E*>(&error) && std::string(error.what()).find(text) != std::string::npos;
    }
    return false;
}
template<class F> bool await(F&& fn, std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    const auto deadline = Clock::now() + timeout;
    do {
        if (fn()) return true; // A successful render predicate is never invoked twice.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (Clock::now() < deadline);
    return false;
}
void put32(QByteArray& bytes, std::uint32_t value)
{ for (unsigned shift = 0; shift < 32; shift += 8) bytes.append(char(value >> shift)); }
void put16(QByteArray& bytes, std::uint16_t value)
{ bytes.append(char(value)); bytes.append(char(value >> 8)); }
std::array<float, 2> original(std::uint64_t frame)
{
    return {1.5F + float(int(frame % 7) - 3) / 32.F,
            -.75F + float(int(frame % 11) - 5) / 64.F};
}
void writeAll(QFile& file, const QByteArray& bytes)
{ require(file.write(bytes) == bytes.size(), "Cannot write bounded bridge fixture"); }
void writeSource(const QString& path, std::uint64_t frames)
{
    QFile file(path); require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Cannot create bridge source");
    QByteArray header("RIFF", 4); put32(header, std::uint32_t(36 + frames * 8));
    header.append("WAVEfmt ", 8); put32(header, 16); put16(header, 3); put16(header, 2);
    put32(header, 44100); put32(header, 44100 * 8); put16(header, 8); put16(header, 32);
    header.append("data", 4); put32(header, std::uint32_t(frames * 8)); writeAll(file, header);
    for (std::uint64_t first = 0; first < frames;) {
        const auto count = std::min<std::uint64_t>(4096, frames - first);
        QByteArray bytes; bytes.reserve(qsizetype(count * 8));
        for (std::uint64_t f = 0; f < count; ++f)
            for (float sample : original(first + f)) put32(bytes, std::bit_cast<std::uint32_t>(sample));
        writeAll(file, bytes); first += count;
    }
    require(file.flush(), "Cannot flush bridge source");
}
QByteArray readBytes(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Cannot read bridge evidence");
    require(file.size() <= 256 * 1024, "Bridge fixture evidence exceeded its bound");
    return file.readAll();
}
QByteArray digestFile(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Cannot hash bridge fixture");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) { const auto bytes = file.read(32768); require(!bytes.isEmpty(), "Fixture hash read failed"); hash.addData(bytes); }
    return hash.result().toHex();
}
QByteArray inputDigest(std::uint64_t frames)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (std::uint64_t first = 0; first < frames;) {
        const auto count = std::min<std::uint64_t>(4096, frames - first);
        QByteArray block; block.reserve(qsizetype(count * 8));
        for (std::uint64_t f = 0; f < count; ++f)
            for (float value : original(first + f)) put32(block, std::bit_cast<std::uint32_t>(value));
        hash.addData(block); first += count;
    }
    return hash.result().toHex();
}
std::vector<float> overview(std::uint64_t frames)
{
    const auto bins = std::min<std::uint64_t>(512, frames);
    std::vector<float> peaks(std::size_t(bins), 0.F); float maximum = 0;
    for (std::uint64_t b = 0; b < bins; ++b) {
        const auto begin = (b * frames + bins - 1) / bins;
        const auto end = ((b + 1) * frames + bins - 1) / bins;
        for (auto frame = begin; frame < end; ++frame)
            for (float value : original(frame)) peaks[std::size_t(b)] = std::max(peaks[std::size_t(b)], std::abs(value));
        maximum = std::max(maximum, peaks[std::size_t(b)]);
    }
    if (maximum > 0) for (auto& peak : peaks) peak /= maximum;
    return peaks;
}
std::vector<QJsonObject> trace(const StemServiceFixture& fixture)
{
    if (!QFileInfo::exists(fixture.marker(".trace"))) return {};
    std::vector<QJsonObject> result;
    const auto bytes = readBytes(fixture.marker(".trace"));
    const auto last = bytes.lastIndexOf('\n');
    if (last < 0) return {}; // Only the unfinished trailing fixture row is ignored.
    for (const auto& line : bytes.first(last + 1).split('\n')) {
        if (line.isEmpty()) continue;
        const auto document = QJsonDocument::fromJson(line);
        require(document.isObject(), "Fixture trace was truncated"); result.push_back(document.object());
    }
    return result;
}
std::size_t count(const StemServiceFixture& fixture, const char* key, const char* value)
{
    const auto rows = trace(fixture);
    return std::size_t(std::ranges::count_if(rows, [&](const auto& row) { return row.value(key).toString() == value; }));
}
PreparedTrackStore::Limits limits(bool large = false)
{
    PreparedTrackStore::Limits result;
    result.maxPreparedBytes = large ? 8 * 1024 * 1024 : 8192;
    result.maxCacheBytes = 128ULL * 1024 * 1024;
    return result;
}
void touch(const QString& path)
{ conflux::test::writeFixtureBytes(path, "release\n"); }
void assertGone(const QString& pidFile)
{
    const auto pid = readBytes(pidFile).trimmed().toLongLong();
    require(pid > 1, "Fixture did not record an owned PID");
    require(kill(pid_t(pid), 0) == -1 && errno == ESRCH, "Owned fixture process survived cleanup");
}
QStringList rootNames(const QString& root)
{ return QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot); }

// Complete-file converter is an independent reference for the live converter.
// Files and pread buffers remain bounded; no full-track float vector is needed.
std::vector<PreparedAudio> references(const QString& directory, std::uint64_t frames)
{
    std::vector<PreparedAudio> result; result.reserve(4);
    for (unsigned role = 0; role < 4; ++role) {
        QFile raw(QDir(directory).filePath(QString("reference-%1.f32").arg(role)));
        require(raw.open(QIODevice::ReadWrite | QIODevice::NewOnly), "Cannot create reference");
        for (std::uint64_t first = 0; first < frames;) {
            const auto n = std::min<std::uint64_t>(4096, frames - first);
            QByteArray bytes; bytes.reserve(qsizetype(n * 8));
            for (std::uint64_t f = 0; f < n; ++f)
                for (float value : original(first + f)) {
                    const auto scaled = float(double(value) * (role + 1) / 4.0);
                    put32(bytes, std::bit_cast<std::uint32_t>(scaled));
                }
            writeAll(raw, bytes); first += n;
        }
        require(raw.flush(), "Cannot flush reference");
        result.push_back(conflux::audio::prepareStemOutput(raw.handle(), frames,
            8 * 1024 * 1024, {}, std::filesystem::path(QFile::encodeName(directory).constData())));
    }
    return result;
}
void preadAll(int descriptor, std::uint64_t first, std::span<float> samples)
{
    std::size_t done = 0;
    while (done < samples.size_bytes()) {
        const auto n = pread(descriptor, reinterpret_cast<char*>(samples.data()) + done,
            samples.size_bytes() - done, off_t(first * 8 + done));
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "Cannot read exact reference PCM"); done += std::size_t(n);
    }
}
conflux::audio::AudioSource peer()
{
    return {1000000000, nullptr, nullptr,
        [](void*, std::uint64_t, std::span<float> output) noexcept {
            for (std::size_t i = 0; i < output.size(); i += 2) { output[i] = .0625F; output[i + 1] = -.03125F; }
            return conflux::audio::AudioReadResult{output.size() / 2, false};
        }, nullptr};
}
void selectRole(RenderCore& core, unsigned role)
{
    for (unsigned index = 0; index < 4; ++index)
        require(core.setStemGain(0, index, index == role ? 1.F : 0.F), "Cannot select real bank role");
}
std::size_t compareBlock(RenderCore& core, const PreparedAudio& reference, std::size_t frames)
{
    const auto before = core.sourceFrame(0), clock = core.frameClock();
    std::array<float, 1024 * 2> output{}, expected{};
    require(frames && frames <= 1024, "Invalid comparison block");
    require(core.render(std::span(output).first(frames * 2)), "Render rejected a valid block");
    const auto delivered = std::size_t(core.sourceFrame(0) - before);
    require(delivered <= frames, "Target cursor advanced past delivered prefix");
    if (delivered) preadAll(reference.descriptor(), before, std::span(expected).first(delivered * 2));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float left = float((frame < delivered ? double(expected[frame * 2]) : 0.0) + .0625);
        const float right = float((frame < delivered ? double(expected[frame * 2 + 1]) : 0.0) - .03125);
        require(std::bit_cast<std::uint32_t>(output[frame * 2]) == std::bit_cast<std::uint32_t>(left) &&
            std::bit_cast<std::uint32_t>(output[frame * 2 + 1]) == std::bit_cast<std::uint32_t>(right),
            "Actual live bank/peer samples differ from independent reference; no mix substitution allowed");
    }
    require(core.frameClock() == clock + frames && core.sourceFrame(1) == core.frameClock(),
            "Independent peer/output clocks did not advance exactly once");
    return delivered;
}
void stop(PreparedLiveStemTrack& track)
{ track.playback->stop(); track.playback.reset(); }

// Test-only cleanup independent of the product group path under test. Acquire
// a pidfd, then validate the fixture's recorded start identity before retaining
// it: a stale marker must never authorize a signal to a reused PID.
struct FixtureChildSafety {
    QString marker, startMarker; int descriptor = -1; pid_t process = -1; bool reaped = false;
    void capture() {
        if (descriptor >= 0 || !QFileInfo::exists(marker)) return;
        const auto pid = readBytes(marker).trimmed().toLongLong();
        const auto expected = readBytes(startMarker).trimmed().toULongLong();
        require(pid > 1, "Invalid fixture child PID");
        const int opened = int(syscall(SYS_pidfd_open, pid_t(pid), 0));
        require(opened >= 0, "Cannot retain fixture child identity");
        try {
            const auto stat = readBytes(QString("/proc/%1/stat").arg(pid));
            const auto closeName = stat.lastIndexOf(") ");
            require(closeName >= 0 && expected > 0, "Invalid fixture start identity");
            const auto fields = stat.mid(closeName + 2).simplified().split(' ');
            require(fields.size() > 19 && fields[19].toULongLong() == expected,
                    "Fixture child PID was reused before safety acquisition");
            descriptor = opened; process = pid_t(pid);
        } catch (...) { close(opened); throw; }
    }
    bool reapKnown() noexcept {
        if (reaped) return true;
        if (process <= 1) return false;
        int status = 0; pid_t result;
        errno = 0;
        do { result = waitpid(process, &status, WNOHANG); } while (result < 0 && errno == EINTR);
        if (result == process) reaped = true;
        return reaped;
    }
    ~FixtureChildSafety() noexcept {
        try { capture(); } catch (...) {}
        if (descriptor < 0) return;
        if (!reaped) {
            (void)syscall(SYS_pidfd_send_signal, descriptor, SIGKILL, nullptr, 0);
            pollfd exited{descriptor, POLLIN, 0};
            (void)poll(&exited, 1, 2000);
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            do {
                if (reapKnown() || errno == ECHILD) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } while (Clock::now() < deadline);
        }
        close(descriptor);
    }
};
struct FixtureSubreaper {
    int previous = 0;
    FixtureSubreaper() {
        require(prctl(PR_GET_CHILD_SUBREAPER, &previous) == 0, "Cannot read test subreaper state");
        require(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "Cannot establish test subreaper");
    }
    ~FixtureSubreaper() noexcept { (void)prctl(PR_SET_CHILD_SUBREAPER, previous); }
};
}

class LiveStemBridgeTest : public QObject {
    Q_OBJECT
private slots:
    void metadataDeferredLoadAndAllRoles()
    {
        checked([&] {
            QTemporaryDir directory; require(directory.isValid(), "No fixture directory");
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto sourceHash = digestFile(source);
            const auto fixture = conflux::test::makeStemServiceFixture(directory.path());
            auto reference = references(directory.path(), 211);
            std::stop_source factoryCancellation;
            auto track = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service,
                nextToken(), factoryCancellation.get_token());
            require(track.playback && track.playback->isLive(), "Factory did not return a whole live owner");
            require(track.sourceSha256.toLatin1() == sourceHash && track.inputSha256.toLatin1() == inputDigest(211) &&
                track.sourceSha256 != track.inputSha256 && track.modelSha256.toLatin1() == digestFile(fixture.engine.modelPath),
                "Original/input/model identities were conflated");
            require(track.inputFrames == 211 && track.outputFrames == reference.front().frames() &&
                track.playback->frames() == track.outputFrames && track.sourceOverview == overview(211),
                "Count/normalized source overview differs from actual source/reference");
            require(track.reservedDiskBytes > 30270240, "Live service/job storage was not reserved");
            require(count(fixture, "command", "load") == 0, "Factory dispatched an implicit default load");
            const auto capturedPath = directory.filePath("captured-source.wav");
            require(QFile::rename(source, capturedPath), "Cannot replace original path after completed preparation");
            writeSource(source, 212); const auto replacementHash = digestFile(source);
            factoryCancellation.request_stop(); // Detached loader token cannot stop installed lifetime.
            require(track.playback->configureStemRamps(0), "Cannot configure bank before rendering");
            require(track.playback->prime(0, std::chrono::seconds(10)), "First real prime failed");
            RenderCore core(track.playback->sourceRef(), peer());
            require(core.restorePrimedCursor(0, 0), "Cannot adopt exact primed cursor");
            for (unsigned role = 0; role < 4; ++role) {
                selectRole(core, role);
                if (role) require(core.seek(0, 0), "Cannot return to source start");
                require(await([&] { return compareBlock(core, reference[role], 1) == 1; }), "Role never became ready");
                while (core.sourceFrame(0) < track.outputFrames) {
                    const auto frames = std::size_t(std::min<std::uint64_t>(97, track.outputFrames - core.sourceFrame(0)));
                    const auto before = core.sourceFrame(0);
                    require(await([&] { return compareBlock(core, reference[role], frames) != 0; }), "Continuous role stalled");
                    require(core.sourceFrame(0) > before, "Reference comparison made no progress");
                }
                require(compareBlock(core, reference[role], 17) == 0 && core.sourceFrame(0) == track.outputFrames,
                        "EOF invented or omitted a source frame");
            }
            require(count(fixture, "command", "load") == 1 && count(fixture, "fixture", "runtime") == 1,
                    "Prime/seek restarted or duplicated the native load/runtime");
            stop(track); assertGone(fixture.marker(".started"));
            require(rootNames(root).isEmpty() && digestFile(capturedPath) == sourceHash && digestFile(source) == replacementHash,
                    "Completed raw snapshot was retargeted or cleanup modified either original path");
        });
    }

    void initialAnchorLeasesFullSeekAndExactTail()
    {
        checked([&] {
            QTemporaryDir directory; require(directory.isValid(), "No fixture directory");
            const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 2 * hop + 211);
            const auto fixture = conflux::test::makeStemServiceFixture(directory.path());
            auto reference = references(directory.path(), 2 * hop + 211);
            auto track = PreparedTrackStore(root, limits(true)).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            require(track.playback->configureStemRamps(0), "Cannot configure real bank");
            constexpr std::uint64_t target = 10000;
            require(track.playback->prime(target, std::chrono::seconds(30)), "Nonzero initial prime failed");
            const auto rows = trace(fixture);
            const auto first = std::ranges::find_if(rows, [](const auto& row) { return row.value("command").toString() == "load"; });
            require(first != rows.end() && first->value("frame").toDouble() == 4410 && count(fixture, "command", "load") == 1,
                    "Initial load used default0 instead of actual rational/preroll anchor");
            RenderCore core(track.playback->sourceRef(), peer());
            require(core.restorePrimedCursor(0, target), "Primed cursor was not adopted"); selectRole(core, 0);
            compareBlock(core, reference[0], 31);
            require(await([&] { return count(fixture, "fixture", "two_leases") != 0; }), "Fixture did not exercise bounded raw leases/Full");
            const auto oldSource = core.sourceFrame(0);
            require(core.retrySource(0) && core.sourceFrame(0) == oldSource, "Same-position retry moved cursor");
            require(core.seek(0, 280801), "Interior HOP-boundary seek rejected");
            require(await([&] { return compareBlock(core, reference[0], 1) == 1; }), "New epoch never became ready");
            require(core.sourceFrame(0) == 280802 && core.stemGain(0, 0) == 1 && core.stemGain(0, 1) == 0,
                    "Latest binding or authoritative gains were lost");
            const auto end = track.outputFrames;
            require(core.seek(0, end - 17), "Tail seek rejected");
            require(await([&] { return compareBlock(core, reference[0], 1) == 1; }), "Actual EOF tail never became ready");
            require(compareBlock(core, reference[0], 16) == 16 && compareBlock(core, reference[0], 19) == 0,
                    "Short exact tail was padded/truncated");
            require(core.sourceFrame(0) == end && !track.playback->errorCode(), "Tail coverage/owner health is wrong");
            require(count(fixture, "command", "ack") >= 2 && count(fixture, "fixture", "complete") >= 1 &&
                    count(fixture, "fixture", "runtime") == 1, "Old leases/final complete/runtime lifetime were not serviced");
            stop(track); assertGone(fixture.marker(".started")); require(rootNames(root).isEmpty(), "Full/seek cleanup leaked a job");
        });
    }

    void shortageNeverSubstitutesOriginalAndRecoveryPreservesPeer()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 2 * hop + 211);
            const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), "gate-after-first");
            auto reference = references(directory.path(), 2 * hop + 211);
            auto track = PreparedTrackStore(root, limits(true)).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            require(track.playback->configureStemRamps(0) && track.playback->prime(0, std::chrono::seconds(30)), "Cannot prime first raw hop");
            RenderCore core(track.playback->sourceRef(), peer()); selectRole(core, 0);
            bool partial = false;
            const auto maximumBlocks = (track.outputFrames + 1022) / 1023 + 1;
            for (std::uint64_t blocks = 0; blocks < maximumBlocks && !core.isBuffering(0); ++blocks) {
                // Published nonfinal packets have4096frames;1023 is coprime
                // and 4096*k%1023=4*k for at most72 slots. It must straddle
                // this gated finite prefix without guessing SRC lookahead.
                const auto delivered = compareBlock(core, reference[0], 1023);
                partial |= delivered > 0 && delivered < 1023;
                require(!track.playback->errorCode(), "Gated source failed before deliberate shortage");
            }
            require(core.isBuffering(0) && partial && core.sourceFrame(0) >= PreparedPlayback::liveReserveFrames && core.sourceFrame(0) < track.outputFrames,
                    "Valid partial raw/converter prefix was not preserved");
            const auto stalled = core.sourceFrame(0);
            require(compareBlock(core, reference[0], 31) == 0 && core.sourceFrame(0) == stalled,
                    "Closed recovery gate consumed target or substituted original mix");
            touch(fixture.marker(".release"));
            require(await([&] { return compareBlock(core, reference[0], 1) == 1; }), "Reserve did not reopen current source");
            require(core.sourceFrame(0) == stalled + 1 && count(fixture, "command", "seek") == 0,
                    "Underrun recovery restarted converter or moved the source");
            stop(track); assertGone(fixture.marker(".started")); require(rootNames(root).isEmpty(), "Recovery cleanup leaked a job");
        });
    }

    void setupFailureWakesAndFactoryCancellationIsBounded_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto* mode : {"start-failure", "exit-before-ready", "error-before-ready", "truncated-ready", "ready-model", "ready-source", "ready-helper", "ready-extra", "ready-missing", "ready-bool-frames", "ready-gate"})
            QTest::newRow(mode) << QByteArray(mode);
    }
    void setupFailureWakesAndFactoryCancellationIsBounded()
    {
        QFETCH(QByteArray, mode);
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); auto fixture = conflux::test::makeStemServiceFixture(directory.path(), mode);
            if (mode == "start-failure") {
                fixture.engine.pythonExecutable = directory.filePath("not-executable");
                conflux::test::writeFixtureBytes(fixture.engine.pythonExecutable, "not an executable\n");
                require(QFile::setPermissions(fixture.engine.pythonExecutable, QFileDevice::ReadOwner | QFileDevice::WriteOwner), "Cannot protect invalid executable");
            }
            std::stop_source cancellation;
            auto result = std::async(std::launch::async, [&] {
                return rejects<std::exception>([&] {
                    auto track = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken(), cancellation.get_token());
                });
            });
            if (mode != "start-failure")
                require(await([&] { return QFileInfo::exists(fixture.marker(".started")); }), "Fixture never started");
            if (mode == "ready-gate") cancellation.request_stop();
            const bool prompt = result.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
            if (!prompt) cancellation.request_stop(); // Never leave the test-owned child alive after a failed oracle.
            require(result.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "Factory ignored cancellation and failure");
            const auto rejected = result.get();
            require(prompt && rejected, "Known setup failure was hidden behind the60s ready deadline");
            if (mode != "start-failure") assertGone(fixture.marker(".started"));
            else require(!QFileInfo::exists(fixture.marker(".started")), "Failed executable unexpectedly ran fixture");
            require(rootNames(root).isEmpty(), "Setup failure leaked charged job");
        });
    }

    void badMetadataOrLeaseCannotPublishAnyPrefix_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto* mode : {"accepted-gap", "runtime-future", "source-original", "wrong-hash", "wrong-origin", "wrong-final", "wrong-order", "sequence-gap", "chunk-bool-frames", "short", "changed-lease", "nonfinite", "fifo", "symlink", "complete-early", "error-then-clean", "unexpected-closed", "unknown-event", "stderr-flood"})
            QTest::newRow(mode) << QByteArray(mode);
    }
    void badMetadataOrLeaseCannotPublishAnyPrefix()
    {
        QFETCH(QByteArray, mode);
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), mode);
            const auto sourceHash = digestFile(source), sentinel = readBytes(fixture.marker(".outside"));
            auto track = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            require(!track.playback->prime(0, std::chrono::seconds(10)) && track.playback->errorCode(), "Malformed service/lease was accepted");
            require(track.playback->errorCode() != ETIMEDOUT && track.playback->errorCode() != ECANCELED,
                    "Protocol/lease failure was hidden as a readiness timeout or cancellation");
            RenderCore core(track.playback->sourceRef(), peer()); selectRole(core, 0);
            std::array<float, 514> output{}; require(core.render(output), "Valid failure render rejected");
            require(core.sourceFrame(0) == 0 && core.sourceFrame(1) == 257 && core.readFailed(0), "Invalid prefix advanced source state");
            for (std::size_t f = 0; f < 257; ++f)
                require(output[f * 2] == .0625F && output[f * 2 + 1] == -.03125F,
                        "Invalid raw prefix or original mix leaked into target output");
            stop(track); assertGone(fixture.marker(".started"));
            require(digestFile(source) == sourceHash && readBytes(fixture.marker(".outside")) == sentinel,
                    "Failure cleanup modified original/outside file");
        });
    }

    void liveReservationIsSharedAndReleasedOnlyAfterCleanup()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path());
            auto cap = limits(); auto live = PreparedTrackStore(root, cap).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            const auto reusableCap = live.reservedDiskBytes + 8192;
            cap.maxCacheBytes = live.reservedDiskBytes + live.outputFrames * 8 - 1;
            require(rejects<std::length_error>([&] { (void)PreparedTrackStore(root, cap).prepare(source); }, "budget"), "Normal preparation ignored live charge");
            require(rejects<std::length_error>([&] { (void)PreparedTrackStore(root, cap).prepareStemBank(source, fixture.engine); }, "budget"), "Offline stems ignored live charge");
            require(rejects<std::length_error>([&] { (void)PreparedTrackStore(root, cap).prepareLiveStem(source, fixture.engine, fixture.service, nextToken()); }, "budget"), "Second live job spent the same reservation");
            auto entryCap = limits(); entryCap.maxEntries = 1;
            require(rejects<std::length_error>([&] { (void)PreparedTrackStore(root, entryCap).prepare(source); }, "entry"), "Live job did not count toward shared entry ceiling");
            require(rejects<std::length_error>([&] { (void)PreparedTrackStore(root, entryCap).prepareLiveStem(source, fixture.engine, fixture.service, nextToken()); }, "entry"), "Live entry ceiling was bypassed");
            stop(live); require(rootNames(root).isEmpty(), "Retired owner did not release cleaned reservation");
            cap.maxCacheBytes = reusableCap;
            auto replacement = PreparedTrackStore(root, cap).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            stop(replacement); require(rootNames(root).isEmpty(), "Released headroom could not be reused");
        });
    }

    void longCorruptLeaseIsRejectedWithoutObservedRenderedPrefix()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 2 * hop + 211);
            const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), "late-nonfinite");
            auto live = PreparedTrackStore(root, limits(true)).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            // Hash/geometry are correct. The last sample of role2 in the first
            // HOP is NaN, beyond enough early input to fill the initial reserve.
            // This observes rejection/no rendered prefix. Source review must
            // separately prove whole-lease validation precedes publication:
            // terminal health can mask an earlier transient ring publication.
            require(!live.playback->prime(0, std::chrono::seconds(30)) && live.playback->errorCode() &&
                live.playback->errorCode() != ETIMEDOUT, "Long corrupt lease was not rejected before prime succeeded");
            RenderCore core(live.playback->sourceRef(), peer()); selectRole(core, 0);
            std::array<float, 514> output{}; require(core.render(output), "Valid failed-bank render rejected");
            require(core.sourceFrame(0) == 0 && core.readFailed(0), "Late invalid lease leaked a rendered prefix");
            for (std::size_t f = 0; f < 257; ++f)
                require(output[f * 2] == .0625F && output[f * 2 + 1] == -.03125F,
                        "Late invalid lease/original mix contaminated independent peer");
            stop(live); assertGone(fixture.marker(".started"));
        });
    }

    void startupRuntimeMayNameKnownOlderOrSupersedingGeneration_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("old-startup-tag") << QByteArray("late-runtime-old");
        QTest::newRow("superseding-startup-tag") << QByteArray("late-runtime-current");
    }
    void startupRuntimeMayNameKnownOlderOrSupersedingGeneration()
    {
        QFETCH(QByteArray, mode);
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), mode);
            auto reference = references(directory.path(), 211);
            auto live = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            require(live.playback->configureStemRamps(0), "Cannot configure pending startup bank");
            std::stop_source firstCancellation;
            auto first = std::async(std::launch::async, [&] {
                return live.playback->prime(0, std::chrono::seconds(30), firstCancellation.get_token());
            });
            const bool requested = await([&] { return QFileInfo::exists(fixture.marker(".load-wait")); });
            firstCancellation.request_stop();
            require(requested && !first.get() && live.playback->errorCode() == ECANCELED,
                    "First startup could not be cancelled without terminating the owner");
            require(live.playback->prime(0, std::chrono::seconds(30)), "Known delayed startup tag was rejected");
            RenderCore core(live.playback->sourceRef(), peer()); selectRole(core, 0);
            require(compareBlock(core, reference[0], 17) == 17 && !live.playback->errorCode(),
                    "Delayed lifetime runtime tag did not lead to the current audio binding");
            require(count(fixture, "command", "load") == 1 && count(fixture, "fixture", "runtime") == 1,
                    "Cancellation silently restarted the runtime");
            stop(live); require(rootNames(root).isEmpty(), "Delayed startup cleanup leaked a job");
        });
    }

    void olderOrdinaryEventAfterNewAcceptedControlIsFatal_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("ordinary-stale") << QByteArray("stale-after-seek");
        QTest::newRow("accepted-reset") << QByteArray("accepted-reset");
    }
    void olderOrdinaryEventAfterNewAcceptedControlIsFatal()
    {
        QFETCH(QByteArray, mode);
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), mode);
            auto reference = references(directory.path(), 211);
            auto live = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            require(live.playback->configureStemRamps(0) && live.playback->prime(0, std::chrono::seconds(10)), "Cannot prime valid initial generation");
            RenderCore core(live.playback->sourceRef(), peer()); selectRole(core, 0);
            require(compareBlock(core, reference[0], 1) == 1 && core.seek(0, 0), "Cannot supersede initial source");
            require(await([&] { return live.playback->errorCode() != 0; }), "Old ordinary event after accepted seek was silently rebound");
            require(compareBlock(core, reference[0], 31) == 0 && core.readFailed(0), "Failed generation leaked old queued PCM");
            stop(live);
        });
    }

    void unacceptedAckRemainsOutstandingDuringSignalCleanup()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), "ack-held");
            auto live = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            std::stop_source priming;
            auto prime = std::async(std::launch::async, [&] {
                return live.playback->prime(0, std::chrono::seconds(30), priming.get_token());
            });
            const bool held = await([&] { return QFileInfo::exists(fixture.marker(".held")); });
            priming.request_stop(); const bool primed = prime.get();
            require(held, "Fixture did not retain an unaccepted ACK");
            const int prior = live.playback->errorCode();
            require((primed && prior == 0) || (!primed && prior == ECANCELED), "Unaccepted ACK was treated as corrupt audio");
            live.playback->stop();
            require(live.playback->errorCode() == prior, "Signal closed count synthesized an ACK or poisoned clean cancellation");
            const auto rows = trace(fixture);
            const auto closed = std::ranges::find_if(rows, [](const auto& row) { return row.value("fixture").toString() == "closed"; });
            require(closed != rows.end() && closed->value("outstanding_slots").toDouble() == 1,
                    "Fixture failed to preserve the actually unaccepted lease");
            live.playback.reset(); assertGone(fixture.marker(".started"));
            require(rootNames(root).isEmpty(), "Stopped ACK lease was not cleaned after whole-group proof");
        });
    }

    void acceptedAckAndClosedCountsMustMatchActualLeases_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("normal-accepted-ack") << QByteArray("good");
        QTest::newRow("wrong-accepted-ack") << QByteArray("bad-ack");
        QTest::newRow("wrong-closed-count") << QByteArray("bad-closed-count");
    }
    void acceptedAckAndClosedCountsMustMatchActualLeases()
    {
        QFETCH(QByteArray, mode);
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), mode);
            auto live = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            const bool primed = live.playback->prime(0, std::chrono::seconds(10));
            require(await([&] { return count(fixture, "command", "ack") != 0; }), "No consumed-input ACK was issued");
            if (mode == "bad-ack")
                require(await([&] { return live.playback->errorCode() != 0; }), "Wrong accepted ACK identity was ignored");
            else require(primed && !live.playback->errorCode(), "Valid prefix failed before closing");
            live.playback->stop();
            require((mode == "good") == (live.playback->errorCode() == 0), "Closing message erased an error or mismatched lease count");
            live.playback.reset(); assertGone(fixture.marker(".started"));
            require(rootNames(root).isEmpty(), "Proven group cleanup did not remove known safe job");
        });
    }

    void continuingJsonlIsBoundedByUnreadDataNotLifetimeBytes()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path());
            auto reference = references(directory.path(), 211);
            auto live = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            require(live.playback->configureStemRamps(0) && live.playback->prime(0, std::chrono::seconds(10)), "Cannot prime continuing stream");
            RenderCore core(live.playback->sourceRef(), peer()); selectRole(core, 0);
            for (unsigned cycle = 0; cycle < 64; ++cycle) {
                if (cycle) require(core.seek(0, 0), "Repeated seek rejected");
                require(await([&] { return compareBlock(core, reference[0], 1) == 1; }), "Consumed JSONL bytes were retained as a lifetime cap");
            }
            live.playback->stop(); require(!live.playback->errorCode(), "Finite continuing stream became a diagnostic overflow");
            const auto rows = trace(fixture);
            const auto closed = std::ranges::find_if(rows, [](const auto& row) { return row.value("fixture").toString() == "closed"; });
            require(closed != rows.end() && closed->value("wire_bytes_before_closed").toDouble() > 65536 &&
                count(fixture, "command", "load") == 1 && count(fixture, "fixture", "runtime") == 1,
                "Fixture did not actually exceed one-shot output capacity with one runtime");
            live.playback.reset(); require(rootNames(root).isEmpty(), "Continuing protocol cleanup leaked a job");
        });
    }

    void concurrentLiveAdmissionsCannotOverspend()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path());
            auto cap = limits(); auto measured = PreparedTrackStore(root, cap).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            cap.maxCacheBytes = measured.reservedDiskBytes * 2 - 1; stop(measured);
            auto start = [&](std::uint64_t token) -> std::optional<PreparedLiveStemTrack> {
                try { return PreparedTrackStore(root, cap).prepareLiveStem(source, fixture.engine, fixture.service, token); }
                catch (const std::length_error&) { return std::nullopt; }
                catch (const std::runtime_error& error) {
                    if (std::string_view(error.what()) == "Prepared cache is busy") return std::nullopt;
                    throw;
                }
            };
            auto a = std::async(std::launch::async, [&] { return start(nextToken()); });
            auto b = std::async(std::launch::async, [&] { return start(nextToken()); });
            auto left = a.get(), right = b.get();
            require(bool(left) != bool(right), "Cooperating factories overspent one remaining job slot");
            if (left) stop(*left); if (right) stop(*right);
            require(rootNames(root).isEmpty(), "Concurrent admission leaked a reservation");
        });
    }

    void liveAndExistingPreparationShareOneAdmission_data()
    {
        QTest::addColumn<bool>("stems");
        QTest::newRow("normal-versus-live") << false;
        QTest::newRow("offline-bank-versus-live") << true;
    }
    void liveAndExistingPreparationShareOneAdmission()
    {
        QFETCH(bool, stems);
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 1025); const auto fixture = conflux::test::makeStemServiceFixture(directory.path());
            auto cap = limits(); cap.maxPreparedBytes = 16384;
            auto measured = PreparedTrackStore(root, cap).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            // Both operations fit alone. Their actual live reservation + PCM
            // alone cannot fit together, independently of private JSON layout.
            cap.maxCacheBytes = measured.reservedDiskBytes + measured.outputFrames * 8 * (stems ? 4 : 1) - 1;
            stop(measured);
            auto live = std::async(std::launch::async, [&]() -> std::optional<PreparedLiveStemTrack> {
                try { return PreparedTrackStore(root, cap).prepareLiveStem(source, fixture.engine, fixture.service, nextToken()); }
                catch (const std::length_error&) { return std::nullopt; }
                catch (const std::runtime_error& error) {
                    if (std::string_view(error.what()) == "Prepared cache is busy") return std::nullopt;
                    throw;
                }
            });
            auto prior = std::async(std::launch::async, [&] {
                try {
                    if (stems) (void)PreparedTrackStore(root, cap).prepareStemBank(source, fixture.engine);
                    else (void)PreparedTrackStore(root, cap).prepare(source);
                    return true;
                } catch (const std::length_error&) { return false; }
                catch (const std::runtime_error& error) {
                    if (std::string_view(error.what()) == "Prepared cache is busy") return false;
                    throw;
                }
            });
            auto owner = live.get(); const bool cached = prior.get();
            require(bool(owner) != cached, "Normal/offline and live admissions both spent incompatible headroom");
            if (owner) stop(*owner);
            if (cached) {
                if (stems) require(PreparedTrackStore(root, cap).prepareStemBank(source, fixture.engine).cacheHit, "Race damaged complete bank entry");
                else require(PreparedTrackStore(root, cap).prepare(source).cacheHit, "Race damaged complete normal entry");
            } else require(rootNames(root).isEmpty(), "Losing admission leaked a private job");
        });
    }

    void unsafeCleanupPreservesItsChargeAndValidCacheBytes()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), "preserve-job");
            auto cap = limits();
            auto offline = PreparedTrackStore(root, cap).prepareStemBank(source, fixture.engine);
            const auto key = offline.artifactKey;
            const auto manifestPath = QDir(root).filePath(key + "/manifest.json");
            const auto manifest = readBytes(manifestPath);
            auto live = PreparedTrackStore(root, cap).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            const auto charge = live.reservedDiskBytes;
            const auto serviceJob = QString::fromUtf8(readBytes(fixture.marker(".job")));
            stop(live); assertGone(fixture.marker(".started"));
            require(QFileInfo::exists(serviceJob + "/unknown-owner"), "Unsafe live tree was silently deleted");
            auto tighter = cap; tighter.maxCacheBytes = charge - 1;
            require(rejects<std::exception>([&] { (void)PreparedTrackStore(root, tighter).prepare(source); }), "New store ignored preserved live reservation");
            require(readBytes(manifestPath) == manifest, "Tighter budget deleted valid normal entry");
            const auto reservationPath = QFileInfo(serviceJob).dir().filePath("reservation.json");
            conflux::test::writeFixtureBytes(reservationPath, "{\"schema\":1,\"bytes\":1}\n");
            require(rejects<std::exception>([&] { (void)PreparedTrackStore(root, cap).prepare(source); }),
                    "Implausibly small preserved live reservation was admitted");
            require(readBytes(manifestPath) == manifest, "Invalid live reservation changed valid cache entry");
        });
    }

    void separateServicePinPreservesFiveAssetOfflineIdentity()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path());
            auto offline = PreparedTrackStore(root, limits()).prepareStemBank(source, fixture.engine);
            auto live = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            stop(live);
            const auto alternative = directory.filePath("alternative-service.py");
            conflux::test::writeFixtureBytes(alternative, readBytes(fixture.service) + "\n# distinct service bytes\n");
            auto other = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, alternative, nextToken());
            stop(other);
            auto reused = PreparedTrackStore(root, limits()).prepareStemBank(source, fixture.engine);
            require(reused.cacheHit && reused.artifactKey == offline.artifactKey,
                    "Separate service identity changed the existing five-asset offline cache identity");
        });
    }

    void stopReapsRealInheritedChildAndPreCancelledDoesNotSpawn()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211); const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), "descendant");
            FixtureChildSafety nativeSafety{fixture.marker(".native"), fixture.marker(".native-start")};
            require(rejects<std::invalid_argument>([&] {
                (void)PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, 0);
            }), "Zero ring identity was admitted");
            auto tiny = limits(); tiny.maxPreparedBytes = 211 * 8;
            require(rejects<std::length_error>([&] {
                (void)PreparedTrackStore(root, tiny).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            }, "budget"), "Actual48k output count was not checked before spawn");
            std::stop_source cancelled; cancelled.request_stop();
            require(rejects<std::runtime_error>([&] { (void)PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken(), cancelled.get_token()); }, "cancel"), "Already-cancelled factory spawned a child");
            require(!QFileInfo::exists(fixture.marker(".started")), "Cancelled preparation started service");
            auto live = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            require(await([&] { return QFileInfo::exists(fixture.marker(".native")); }), "No real inherited child was created");
            nativeSafety.capture();
            const auto servicePid = readBytes(fixture.marker(".started")).trimmed().toLongLong();
            const auto nativePid = readBytes(fixture.marker(".native")).trimmed().toLongLong();
            require(servicePid > 1 && nativePid > 1 && getpgid(pid_t(nativePid)) == servicePid,
                    "Fake native escaped the caller-owned process session");
            stop(live); assertGone(fixture.marker(".started")); assertGone(fixture.marker(".native"));
            require(rootNames(root).isEmpty(), "Known-group cleanup did not release job");
        });
    }

    void exitedServiceLeaderStillFinishesAndReapsItsKnownGroup()
    {
        checked([&] {
            QTemporaryDir directory; const auto source = directory.filePath("source.wav"), root = directory.filePath("cache");
            writeSource(source, 211);
            FixtureSubreaper subreaper; // Outlives service and identity-bound fallback cleanup.
            const auto fixture = conflux::test::makeStemServiceFixture(directory.path(), "dead-leader");
            FixtureChildSafety nativeSafety{fixture.marker(".native"), fixture.marker(".native-start")};
            auto live = PreparedTrackStore(root, limits()).prepareLiveStem(source, fixture.engine, fixture.service, nextToken());
            require(await([&] { return QFileInfo::exists(fixture.marker(".native")); }), "No real child before leader exit");
            nativeSafety.capture();
            const auto servicePid = readBytes(fixture.marker(".started")).trimmed().toLongLong();
            require(servicePid > 1 && getpgid(nativeSafety.process) == servicePid,
                    "Leader-exit child did not start in the owned session");
            conflux::test::writeFixtureBytes(fixture.marker(".exit"), "exit direct leader now\n");
            // Reap ONLY this previously identity-bound child while the product
            // checks its known group. No waitpid(-1), PID scan or reap thread.
            require(await([&] { return nativeSafety.reapKnown(); }),
                    "Exited leader's surviving child was not terminated by owned-group cleanup");
            require(await([&] { return live.playback->errorCode() != 0; }), "Unexpected leader exit was not a terminal producer failure");
            stop(live);
            assertGone(fixture.marker(".started")); assertGone(fixture.marker(".native"));
            require(rootNames(root).isEmpty(), "Dead-leader group proof did not release the safe job/reservation");
            // Product assertions finish before fallback safety can signal.
        });
    }
};
QTEST_GUILESS_MAIN(LiveStemBridgeTest)
#include "live_stem_bridge_test.moc"

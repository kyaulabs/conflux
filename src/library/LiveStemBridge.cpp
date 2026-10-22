// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/PreparedTrackStore.hpp"
#include "library/LiveStemObservations.hpp"
#include "library/StemEngineSupport.hpp"
#include "library/StemServiceJsonl.hpp"
#include "audio/StemRateConverter.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRandomGenerator>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <thread>
#include <type_traits>

namespace conflux::library {
static_assert(std::is_trivially_copyable_v<LiveStemObservations::Record>);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free &&
    std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

LiveStemObservations::LiveStemObservations() noexcept = default;
bool LiveStemObservations::tryPop(Record& output) noexcept
{
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == written_.load(std::memory_order_acquire)) return false;
    output = records_[read % capacity];
    read_.store(read + 1, std::memory_order_release);
    return true;
}
LiveStemObservations::Counters LiveStemObservations::counters() const noexcept
{
    Counters result;
    result.producerFinished = producerFinished_.load(std::memory_order_acquire);
    result.attemptedRecords = attempted_.load(std::memory_order_relaxed);
    result.droppedRecords = dropped_.load(std::memory_order_relaxed);
    result.invalidClockReads = clockErrors_.load(std::memory_order_relaxed);
    return result;
}
const LiveStemObservations::Setup& LiveStemObservations::setup() const noexcept { return setup_; }
std::string LiveStemObservations::failureDetail() const
{
    const std::lock_guard lock(failureMutex_);
    return {failureDetail_.data(), failureDetailSize_};
}
bool LiveStemObservations::requestDiagnosticDrain(std::uint64_t epoch, std::uint64_t generation,
    std::chrono::steady_clock::time_point deadline) noexcept
{
    if (!epoch || !generation || deadline <= std::chrono::steady_clock::now() ||
        producerFinished_.load(std::memory_order_acquire) || drainClaimed_.test_and_set(std::memory_order_acq_rel)) return false;
    drainEpoch_ = epoch; drainGeneration_ = generation; drainDeadline_ = deadline;
    drainState_.store(DiagnosticDrainState::Requested, std::memory_order_seq_cst);
    if (producerFinished_.load(std::memory_order_seq_cst)) {
        auto pending = DiagnosticDrainState::Requested;
        (void)drainState_.compare_exchange_strong(pending, DiagnosticDrainState::Failed, std::memory_order_seq_cst);
        return false;
    }
    return true;
}
LiveStemObservations::DiagnosticDrainStatus LiveStemObservations::diagnosticDrainStatus() const noexcept
{
    const auto state = drainState_.load(std::memory_order_acquire);
    if (state == DiagnosticDrainState::NotRequested) return {};
    return {state, drainEpoch_, drainGeneration_, drainDeadline_};
}
} // namespace conflux::library

namespace conflux::library::detail {
// Concrete single-Bridge writer, kept private beside its owning job.
struct LiveStemObservationWriter {
    static void fail(LiveStemObservations& observations, std::string_view cause, QByteArrayView diagnostics)
    {
        const std::lock_guard lock(observations.failureMutex_);
        if (observations.failureDetailSize_ || cause.empty()) return;
        auto append = [&](std::string_view text) {
            const auto count = std::min(text.size(), observations.failureDetail_.size() - observations.failureDetailSize_);
            std::copy_n(text.data(), count, observations.failureDetail_.data() + observations.failureDetailSize_);
            observations.failureDetailSize_ += count;
        };
        append(cause.substr(0, 1024));
        if (diagnostics.empty()) return;
        append("\nService stderr: ");
        auto available = observations.failureDetail_.size() - observations.failureDetailSize_;
        if (std::size_t(diagnostics.size()) > available) {
            append("[truncated; last bytes follow]\n");
            available = observations.failureDetail_.size() - observations.failureDetailSize_;
        }
        const auto count = std::min(std::size_t(diagnostics.size()), available);
        append({diagnostics.data() + diagnostics.size() - count, count});
    }
    static std::uint64_t stamp(LiveStemObservations& observations) noexcept
    {
        const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (value > 0) return std::uint64_t(value);
        observations.clockErrors_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    static void push(LiveStemObservations& observations, LiveStemObservations::Record record) noexcept
    {
        record.ordinal = observations.attempted_.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto written = observations.written_.load(std::memory_order_relaxed);
        const auto read = observations.read_.load(std::memory_order_acquire);
        using Kind = LiveStemObservations::Kind;
        const bool validClock = record.observedNs &&
            (record.kind != Kind::ChunkValidated || record.validatedNs) &&
            (record.kind != Kind::PacketAccepted || (record.packetReadyNs && record.publishBeginNs &&
                record.publishEndNs && (!record.fullObserved || record.firstFullNs)));
        if (!validClock || std::uint32_t(written - read) == LiveStemObservations::capacity) {
            observations.dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        observations.records_[written % LiveStemObservations::capacity] = record;
        observations.written_.store(written + 1, std::memory_order_release);
    }
    static void drainState(LiveStemObservations& observations, LiveStemObservations::DiagnosticDrainState state) noexcept
    { observations.drainState_.store(state, std::memory_order_release); }
    static void finish(LiveStemObservations& observations) noexcept
    {
        observations.producerFinished_.store(true, std::memory_order_seq_cst);
        auto pending = LiveStemObservations::DiagnosticDrainState::Requested;
        (void)observations.drainState_.compare_exchange_strong(pending,
            LiveStemObservations::DiagnosticDrainState::Failed, std::memory_order_seq_cst);
    }
};
namespace {
using Clock = std::chrono::steady_clock;
using Converter = audio::StemRateConverter;
using Buffer = audio::LiveStemBuffer;
constexpr std::uint64_t frameBytes = 8, hop = 257985;
constexpr std::uint64_t slotBytes = 8255520, spoolBytes = 13759200, metadataBytes = 16384;
constexpr std::uint64_t wireMaximum = StemServiceJsonl::maxWireInteger;
const QJsonArray roles{"drums", "bass", "other", "vocals"};
[[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }
void require(bool condition, const char* message) { if (!condition) invalid(message); }
void keys(const QJsonObject& object, std::initializer_list<const char*> required,
          std::initializer_list<const char*> optional = {})
{
    for (const auto* key : required) require(object.contains(key), "Missing stem service field");
    for (auto it = object.begin(); it != object.end(); ++it) {
        const auto match = [&](const char* key) { return it.key() == QLatin1String(key); };
        require(std::any_of(required.begin(), required.end(), match) ||
                std::any_of(optional.begin(), optional.end(), match), "Unexpected stem service field");
    }
}
std::uint64_t integer(const QJsonValue& value, std::uint64_t maximum = wireMaximum)
{ return jsonInteger(value, maximum); }
QString text(const QJsonValue& value, qsizetype maximum = 128)
{
    require(value.isString(), "Stem service string has wrong type");
    auto result = value.toString();
    require(result.size() <= maximum && !result.contains(QChar(u'\0')), "Stem service string exceeds its bound");
    return result;
}
QJsonObject object(const QJsonValue& value)
{ require(value.isObject(), "Stem service object has wrong type"); return value.toObject(); }
bool boolean(const QJsonValue& value)
{ require(value.isBool(), "Stem service boolean has wrong type"); return value.toBool(); }
double finite(const QJsonValue& value, bool signedValue = false)
{
    require(value.isDouble() && std::isfinite(value.toDouble()) && (signedValue || value.toDouble() >= 0),
            "Stem service scalar is invalid");
    return value.toDouble();
}
QByteArray digest(const QJsonValue& value)
{
    const auto result = text(value, 64).toLatin1();
    require(hexDigest(result), "Stem service SHA256 is invalid");
    return result;
}
void readAt(int fd, char* data, std::size_t bytes, std::uint64_t offset)
{
    while (bytes) {
        const auto count = pread(fd, data, bytes, off_t(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) ioError("Cannot read live stem file");
        if (!count) throw std::runtime_error("Live stem file changed or was truncated");
        data += count; bytes -= std::size_t(count); offset += std::uint64_t(count);
    }
}
float wireFloat(const char* bytes) noexcept
{
    const auto* p = reinterpret_cast<const unsigned char*>(bytes);
    return std::bit_cast<float>(std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
        (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24));
}
void lockRoot(QLockFile& lock, std::stop_token stop = {})
{
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!lock.tryLock(0)) {
        checkStop(stop);
        if (lock.error() != QLockFile::LockFailedError) throw std::runtime_error("Cannot lock prepared cache");
        if (Clock::now() >= deadline) throw std::runtime_error("Prepared cache is busy");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    checkStop(stop);
}
QJsonObject record(int directory, std::stop_token stop)
{
    Descriptor fd(openCacheFile(directory, "reservation.json"));
    const auto before = statusOf(fd.get());
    require(before.st_size > 0 && std::uint64_t(before.st_size) <= metadataBytes, "Invalid live reservation record");
    checkStop(stop);
    QByteArray bytes(qsizetype(before.st_size), Qt::Uninitialized);
    readAt(fd.get(), bytes.data(), std::size_t(bytes.size()), 0);
    require(unchanged(before, statusOf(fd.get())), "Live reservation changed during accounting");
    const auto document = QJsonDocument::fromJson(bytes);
    require(document.isObject(), "Invalid live reservation JSON");
    const auto result = document.object();
    keys(result, {"schema", "bytes"});
    require(integer(result.value("schema")) == 1, "Unknown live reservation schema");
    return result;
}
// Two passes validate this fixed tree before deleting anything. No generic
// recursion, stale adoption, or inference from an unlocked owner file.
void serviceTree(int directory, bool remove)
{
    const auto names = directoryNames(directory, 3);
    for (const auto& name : names) {
        const auto status = childStatus(directory, name);
        if (name == "native") {
            require(S_ISDIR(status.st_mode), "Unsafe live native directory");
            Descriptor native(openat(directory, "native", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (native.get() < 0) ioError("Cannot inspect live native directory");
            for (const auto& leaf : directoryNames(native.get(), 3)) {
                require((leaf == "input.f32" || leaf == "output.f32" || leaf == "runtime.stderr") &&
                    S_ISREG(childStatus(native.get(), leaf).st_mode), "Unsafe live native leaf");
                if (remove && unlinkat(native.get(), leaf.c_str(), 0) != 0) ioError("Cannot remove live native leaf");
            }
            if (remove && unlinkat(directory, "native", AT_REMOVEDIR) != 0) ioError("Cannot remove live native directory");
        } else {
            require((name == "slot0.f32" || name == "slot1.f32") && S_ISREG(status.st_mode), "Unsafe live service leaf");
            if (remove && unlinkat(directory, name.c_str(), 0) != 0) ioError("Cannot remove live slot");
        }
    }
}
class LiveJob {
public:
    LiveJob(QString rootPath, PreparedTrackStore::Limits limits, StemEngine config,
            EngineFiles files, EngineFile service)
        : rootPath(std::move(rootPath)), limits(limits), engine(std::move(config)),
          files(std::move(files)), service(std::move(service)) {}
    ~LiveJob() { (void)cleanup(); }
    void publishRecord(std::uint64_t value, std::stop_token stop)
    {
        const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"bytes", double(value)}}).toJson(QJsonDocument::Compact);
        require(std::uint64_t(bytes.size()) <= metadataBytes, "Live reservation metadata overflow");
        Descriptor out(openat(directory->get(), "reservation.next", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (out.get() < 0) ioError("Cannot create live reservation update");
        writeAll(out.get(), bytes.constData(), std::size_t(bytes.size()), stop);
        if (fsync(out.get()) != 0 || renameat(directory->get(), "reservation.next", directory->get(), "reservation.json") != 0 ||
            fsync(directory->get()) != 0) ioError("Cannot synchronize live reservation");
        charge = value;
    }
    bool cleanup() noexcept
    {
        if (!admitted || cleaned) return true;
        { const std::lock_guard lock(mutex); if (factoryActive) return false; }
        if (!groupsGone) return false;
        input.reset(); // Process group and all producer leases are already quiesced.
        try {
            QLockFile lock(QDir(rootPath).filePath(".lock")); lockRoot(lock);
            Descriptor serviceLock(openat(directory->get(), ".service.service.lock", O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
            if (serviceLock.get() < 0 && errno != ENOENT) ioError("Cannot inspect live service lock");
            if (serviceLock.get() >= 0)
                require(S_ISREG(statusOf(serviceLock.get()).st_mode) && flock(serviceLock.get(), LOCK_EX | LOCK_NB) == 0,
                        "Live service ownership remains held");
            const auto names = directoryNames(directory->get(), 6);
            for (const auto& leaf : names) {
                const auto status = childStatus(directory->get(), leaf);
                if (leaf == "service") {
                    require(S_ISDIR(status.st_mode), "Unsafe live service directory");
                    Descriptor child(openat(directory->get(), "service", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
                    if (child.get() < 0) ioError("Cannot inspect live service job");
                    serviceTree(child.get(), false);
                } else require((leaf == "input.f32" || leaf == "reservation.json" || leaf == "reservation.next" ||
                    leaf == ".owner.lock" || leaf == ".service.service.lock") && S_ISREG(status.st_mode), "Unsafe live job leaf");
            }
            for (const auto& leaf : names) {
                if (leaf == "reservation.json" || leaf == ".owner.lock") continue;
                if (leaf == "service") {
                    Descriptor child(openat(directory->get(), "service", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
                    if (child.get() < 0) ioError("Cannot clean live service job");
                    serviceTree(child.get(), true);
                    if (unlinkat(directory->get(), "service", AT_REMOVEDIR) != 0) ioError("Cannot remove live service job");
                } else if (unlinkat(directory->get(), leaf.c_str(), 0) != 0) ioError("Cannot remove live job leaf");
            }
            ownerLock.reset();
            if (unlinkat(directory->get(), ".owner.lock", 0) != 0 && errno != ENOENT) ioError("Cannot remove live owner lock");
            if (unlinkat(directory->get(), "reservation.json", 0) != 0 && errno != ENOENT)
                ioError("Cannot release live reservation");
            if (unlinkat(root->get(), name.c_str(), AT_REMOVEDIR) != 0) {
                // Restore the conservative charge while the same root lock is
                // still held. A failed restoration leaves an invalid job,
                // which makes subsequent admission fail closed.
                publishRecord(charge, {});
                ioError("Cannot remove live reservation directory");
            }
            cleaned = true;
            (void)fsync(root->get()); // No named job/PCM survives successful rmdir.
            return true;
        } catch (...) { return false; } // Preserve remaining paths/charge on any unproven cleanup.
    }
    void notifySetup(const QString& error = {}, QByteArrayView diagnostics = {}) noexcept
    {
        if (!error.isEmpty()) {
            const auto bytes = error.toUtf8();
            LiveStemObservationWriter::fail(*observations, {bytes.constData(), std::size_t(bytes.size())}, diagnostics);
        }
        { const std::lock_guard lock(mutex);
          if (setupDone) return;
          setupDone = true;
          setupError = error.isEmpty() ? QString{} : QString::fromStdString(observations->failureDetail()); }
        condition.notify_all();
    }
    QString rootPath;
    PreparedTrackStore::Limits limits;
    StemEngine engine;
    EngineFiles files;
    EngineFile service;
    std::shared_ptr<LiveStemObservations> observations;
    std::unique_ptr<Descriptor> root, directory, ownerLock, input;
    std::string name;
    QByteArray inputDigest;
    std::uint64_t charge = 0, inputFrames = 0, serviceBytes = 0;
    bool admitted = false, cleaned = false, groupsGone = true;
    std::mutex mutex;
    std::condition_variable condition;
    bool setupDone = false, factoryActive = false;
    QString setupError;
};
class Bridge {
public:
    Bridge(std::shared_ptr<LiveJob> job, Converter::FramePlan plan, std::stop_token stop,
           audio::PreparedPlayback::LiveProducerPort& port)
        : job_(std::move(job)), plan_(plan), stop_(stop), port_(port), owned_(job_->groupsGone), process_(owned_.process()) {}
    void run()
    {
        try {
            require(port_.tryRequest(initial_), "Cannot capture initial live request");
            latest_ = initial_;
            const auto path = QDir(job_->rootPath).filePath(QString::fromStdString(job_->name));
            const auto input = QDir(path).filePath("input.f32"), directory = QDir(path).filePath("service");
            process_.start(job_->files[0].invocation, {"-B", job_->service.path,
                "--input", input, "--directory", directory, "--runtime", job_->files[3].path, "--model", job_->files[4].path,
                "--backend", job_->engine.backend, "--threads", QString::number(job_->engine.threads),
                "--max-input-bytes", QString::number(qulonglong(job_->inputFrames * 8)),
                "--max-job-bytes", QString::number(qulonglong(job_->serviceBytes)),
                "--max-seconds", QString::number(job_->engine.maxSeconds), "--request-timeout", QString::number(job_->engine.requestTimeout),
                "--max-diagnostic-bytes", QString::number(qulonglong(job_->engine.maxDiagnosticBytes))});
            if (!process_.waitForStarted(1000) || owned_.group() <= 1)
                throw std::runtime_error("Cannot start the local stem service");
            for (;;) {
                if (diagnosticReady_) {
                    if (stop_.stop_requested()) break;
                    Buffer::Request request;
                    require(!port_.tryRequest(request) || request.epoch == latest_.epoch,
                        "Live epoch changed after diagnostic idle close");
                    require(Clock::now() < diagnosticDeadline_, "Diagnostic idle-close stop acknowledgement timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue; // Returning before owner stop would correctly become EPIPE.
                }
                if (stop_.stop_requested() && diagnosticDrain_) {
                    LiveStemObservationWriter::drainState(*job_->observations, DrainState::Failed);
                    diagnosticDrain_ = false;
                    closeDeadline_ = std::min(closeDeadline_, Clock::now() + std::chrono::milliseconds(250));
                }
                if (stop_.stop_requested() && !closing_) {
                    closing_ = true; discard();
                    closeDeadline_ = Clock::now() + std::chrono::milliseconds(250);
                }
                if (!closing_ && !port_.serviceRecovery()) {
                    // Owner health/timeout is already terminal; never publish more PCM.
                    break;
                }
                Buffer::Request request;
                if ((!closing_ || diagnosticDrain_) && port_.tryRequest(request) && request.epoch != latest_.epoch) {
                    require(job_->observations->diagnosticDrainStatus().state == DrainState::NotRequested,
                        "Live epoch changed during diagnostic drain");
                    latest_ = request; discard(); observeRequest();
                }
                drain();
                if (!closing_) beginDiagnosticDrain();
                if (process_.state() == QProcess::NotRunning) {
                    drain();
                    if (!incoming_.isEmpty() || process_.bytesAvailable()) continue;
                    parser_.finish();
                    require(closed_, "Stem service exited before its close receipt");
                    require(process_.exitStatus() == QProcess::NormalExit &&
                        (process_.exitCode() == 0 || (!diagnosticDrain_ && closing_ && process_.exitCode() == 130)),
                        "Stem service exited unsuccessfully");
                    if (diagnosticDrain_) {
                        require(Clock::now() < diagnosticDeadline_, "Diagnostic idle-close deadline expired");
                        LiveStemObservationWriter::drainState(*job_->observations, DrainState::ReadyForStop);
                        diagnosticReady_ = true;
                        continue;
                    }
                    break;
                }
                if (closed_) {
                    require(incoming_.isEmpty(), "Stem service emitted data after closed");
                } else if (ready_) {
                    schedule();
                    if (!closing_ && !diagnosticDrain_) convert();
                }
                const auto now = Clock::now();
                if (!closing_ && pending_.id && now >= replyDeadline_)
                    throw std::runtime_error("Stem service command acknowledgement timed out");
                // Inference timeout is not a shutdown allowance. A short
                // protocol grace is followed by the existing owned-group
                // TERM/KILL/reap bound, even if an ACK is stuck.
                if (diagnosticDrain_ && now >= diagnosticDeadline_)
                    throw std::runtime_error("Diagnostic idle-close deadline expired");
                if (closing_ && now >= closeDeadline_) break;
                // QProcess belongs only to this thread. A bounded wait services
                // pipes; it never suspends render or the producer's recovery clock.
                process_.waitForReadyRead(1);
            }
        } catch (const std::exception& error) {
            failDiagnosticDrain();
            retainFailure(QString::fromUtf8(error.what()).left(1024));
            port_.fail(EIO);
        } catch (...) {
            failDiagnosticDrain();
            retainFailure(QStringLiteral("Cannot start the live stem service"));
            port_.fail(EIO);
        }
        observeDiscard();
        // Release every read lease before process/job deletion. Failed protocol
        // does not turn into success through a friendly closed/exit message.
        for (auto& lease : leases_) lease.fd.reset();
        serviceDirectory_.reset();
        // Parsing is over. Prevent Qt's teardown waits from accumulating a
        // flooding child's unread bytes outside the bounded retained buffers.
        process_.closeReadChannel(QProcess::StandardOutput);
        process_.closeReadChannel(QProcess::StandardError);
        if (!owned_.finish()) {
            job_->notifySetup(QStringLiteral("Cannot finish the owned live stem process group"), diagnostics_);
            port_.fail(EIO);
        }
        if (!job_->cleanup()) {
            job_->notifySetup(QStringLiteral("Cannot clean the owned live stem job"), diagnostics_);
            port_.fail(EIO);
        }
        if (!ready_) job_->notifySetup(QStringLiteral("Live stem service stopped before ready"));
    }
private:
    struct Lease {
        std::unique_ptr<Descriptor> fd;
        struct stat before{};
        std::uint64_t generation = 0, sequence = 0, first = 0, frames = 0, used = 0;
        bool occupied = false, validated = false, ackReady = false, ackSent = false;
    };
    struct Command {
        QString kind;
        std::uint64_t id = 0, frame = 0, epoch = 0, target = 0;
        int slot = -1;
    };
    using Observation = LiveStemObservations::Record;
    using Kind = LiveStemObservations::Kind;
    using DrainState = LiveStemObservations::DiagnosticDrainState;
    void retainFailure(const QString& cause)
    {
        // Read only bytes already buffered by QProcess, within the existing
        // diagnostic budget. Never wait or parse more protocol after failure.
        process_.setReadChannel(QProcess::StandardError);
        const auto available = job_->engine.maxDiagnosticBytes - std::uint64_t(diagnostics_.size());
        const auto count = std::min<std::uint64_t>(std::uint64_t(process_.bytesAvailable()), available);
        if (count) diagnostics_.append(process_.read(qint64(count)));
        job_->notifySetup(cause, diagnostics_);
    }
    void failDiagnosticDrain() noexcept
    {
        if (job_->observations->diagnosticDrainStatus().state != DrainState::NotRequested)
            LiveStemObservationWriter::drainState(*job_->observations, DrainState::Failed);
    }
    void beginDiagnosticDrain()
    {
        const auto requested = job_->observations->diagnosticDrainStatus();
        if (requested.state != DrainState::Requested || diagnosticDrain_) return;
        require(ready_ && loaded_ && active_ && rawComplete_ && finished_ &&
            rawNext_ == plan_.inputFrames() && convertedInput_ == plan_.inputFrames() && !pendingFrames_ &&
            latest_.epoch == acceptedEpoch_ && acceptedEpoch_ == requested.providerEpoch &&
            generation_ == requested.workerGeneration && (!pending_.id || pending_.kind == "ack"),
            "Diagnostic idle-close requires the exact complete live generation");
        diagnosticDeadline_ = std::min(requested.deadline, Clock::now() + std::chrono::seconds(3));
        require(Clock::now() < diagnosticDeadline_, "Diagnostic idle-close deadline expired");
        diagnosticDrain_ = true;
    }
    std::uint64_t stamp() noexcept { return LiveStemObservationWriter::stamp(*job_->observations); }
    Observation observation(Kind kind) noexcept
    {
        Observation result;
        result.kind = kind; result.observedNs = stamp();
        result.providerEpoch = acceptedEpoch_; result.workerGeneration = generation_;
        return result;
    }
    void observe(Observation record) noexcept
    { LiveStemObservationWriter::push(*job_->observations, record); }
    void observeRequest() noexcept
    {
        auto record = observation(Kind::RequestObserved);
        record.providerEpoch = latest_.epoch; record.workerGeneration = 0;
        record.outputFirst48000 = latest_.firstFrame;
        observe(record);
    }
    void observeDiscard() noexcept
    {
        if (!pendingFrames_) return;
        auto record = observation(Kind::PacketDiscarded);
        record.outputFirst48000 = pendingFirst_; record.outputFrames48000 = pendingFrames_;
        observe(record);
    }
    void discard()
    {
        observeDiscard();
        packetReadyNs_ = firstFullNs_ = 0; fullObserved_ = false;
        converter_.reset(); pendingFrames_ = inputFrames_ = inputOffset_ = 0; inputSlot_ = -1;
        finished_ = false;
        for (auto& lease : leases_) if (lease.occupied && !lease.ackSent && lease.validated) {
            require(!lease.fd || unchanged(lease.before, statusOf(lease.fd->get())), "Live lease changed before discard");
            lease.fd.reset(); lease.ackReady = true;
        }
    }
    std::uint64_t send(QString kind, QJsonObject fields = {}, int slot = -1)
    {
        require(!pending_.id && nextCommand_ < wireMaximum, "Stem command state exhausted");
        pending_ = {std::move(kind), ++nextCommand_, 0, 0, 0, slot};
        fields.insert("v", 1); fields.insert("command", pending_.kind); fields.insert("id", double(pending_.id));
        auto wire = QJsonDocument(fields).toJson(QJsonDocument::Compact); wire.append('\n');
        require(wire.size() <= 4096 && process_.bytesToWrite() == 0, "Stem command buffer exceeds fixed bound");
        const auto queuedNs = (pending_.kind == "load" || pending_.kind == "seek") ? stamp() : 0;
        require(process_.write(wire) == wire.size(), "Cannot write stem service control");
        replyDeadline_ = Clock::now() + std::chrono::seconds(job_->engine.requestTimeout);
        return queuedNs;
    }
    void schedule()
    {
        if (pending_.id) return;
        if (closing_) { send("close"); return; }
        if (diagnosticDrain_ && std::ranges::none_of(leases_, [](const auto& lease) { return lease.occupied; })) {
            closing_ = true; closeDeadline_ = diagnosticDeadline_;
            send("close"); return;
        }
        // Controls supersede a potentially unlimited stream of old raw leases.
        // Outstanding leases remain immutable and are ACKed with their old key.
        if (!diagnosticDrain_ && latest_.epoch != initial_.epoch && latest_.epoch != acceptedEpoch_) {
            require(generation_ < wireMaximum, "Stem service generation exhausted");
            converter_ = std::make_unique<Converter>(plan_, latest_.firstFrame);
            const auto anchor = converter_->inputStartFrame();
            const auto queuedNs = send(loaded_ ? "seek" : "load", {{"frame", double(anchor)}});
            pending_.frame = anchor; pending_.epoch = latest_.epoch; pending_.target = latest_.firstFrame;
            auto record = observation(Kind::CommandQueued);
            record.observedNs = queuedNs; record.commandId = pending_.id;
            record.providerEpoch = pending_.epoch; record.workerGeneration = 0;
            record.inputFirst44100 = anchor; record.outputFirst48000 = pending_.target;
            observe(record);
            return;
        }
        for (std::size_t index = 0; index < leases_.size(); ++index) {
            auto& lease = leases_[index];
            if (!lease.occupied || !lease.ackReady || lease.ackSent) continue;
            send("ack", {{"slot", double(index)}, {"sequence", double(lease.sequence)},
                {"generation", double(lease.generation)}, {"source_id", QString::fromLatin1(job_->inputDigest)},
                {"model_id", job_->files[4].identity.value("sha256")}}, int(index));
            lease.ackSent = true;
            return;
        }
    }
    void drain()
    {
        process_.setReadChannel(QProcess::StandardError);
        const auto errors = process_.bytesAvailable();
        if (errors) {
            const auto available = job_->engine.maxDiagnosticBytes - diagnostics_.size();
            diagnostics_.append(process_.read(qint64(std::min<std::uint64_t>(std::uint64_t(errors), available))));
            if (std::uint64_t(errors) > available) throw std::length_error("Stem service diagnostics exceed byte budget");
        }
        process_.setReadChannel(QProcess::StandardOutput);
        const auto available = process_.bytesAvailable();
        if (available > 65536 - incoming_.size()) throw std::length_error("Unread stem service output exceeds byte budget");
        if (available) incoming_.append(process_.read(std::min<qint64>(available, 16384)));
        for (unsigned events = 0; events < 8 && !incoming_.isEmpty(); ++events) {
            require(!closed_, "Stem service event followed closed");
            const auto fed = parser_.feed(QByteArrayView(incoming_));
            incoming_.remove(0, fed.consumed);
            if (fed.object) event(*fed.object);
            else break;
        }
    }
    void ready(const QJsonObject& value)
    {
        keys(value, {"v", "event", "generation", "describe", "source", "model", "runtime", "limits", "reservation_bytes"});
        require(!ready_ && integer(value.value("generation")) == 0, "Repeated or invalid stem ready");
        auto description = object(value.value("describe"));
        require(description.size() == 11 && digest(description.value("service_sha256")) ==
            job_->service.identity.value("sha256").toString().toLatin1(), "Stem service source identity differs");
        description.remove("service_sha256"); validateDescription(description, job_->files);
        const QJsonObject expectedSource{{"format", "f32le"}, {"sample_rate", 44100}, {"channels", 2},
            {"frames", double(job_->inputFrames)}, {"bytes", double(job_->inputFrames * 8)}, {"sha256", QString::fromLatin1(job_->inputDigest)}};
        const QJsonObject expectedLimits{{"max_input_bytes", double(job_->inputFrames * 8)}, {"max_job_bytes", double(job_->serviceBytes)},
            {"max_seconds", job_->engine.maxSeconds}, {"request_timeout", job_->engine.requestTimeout},
            {"max_diagnostic_bytes", double(job_->engine.maxDiagnosticBytes)}};
        require(object(value.value("source")) == expectedSource && object(value.value("model")) == job_->files[4].identity &&
            object(value.value("runtime")) == job_->files[3].identity && object(value.value("limits")) == expectedLimits &&
            integer(value.value("reservation_bytes")) == job_->serviceBytes, "Stem service launch geometry or identity differs");
        serviceDirectory_ = std::make_unique<Descriptor>(openat(job_->directory->get(), "service", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (serviceDirectory_->get() < 0) ioError("Cannot open live service directory");
        ready_ = true; observe(observation(Kind::ServiceReady)); job_->notifySetup();
    }
    void accepted(const QJsonObject& value)
    {
        require(ready_ && pending_.id && integer(value.value("id")) == pending_.id &&
            text(value.value("command")) == pending_.kind, "Unexpected stem acknowledgement");
        (void)finite(value.value("accepted_at"));
        const auto returnedGeneration = integer(value.value("generation"));
        if (pending_.kind == "ack") {
            keys(value, {"v", "event", "id", "command", "generation", "accepted_at", "slot", "sequence", "source_id", "model_id", "current_generation"});
            auto& lease = leases_[std::size_t(pending_.slot)];
            require(lease.occupied && lease.ackSent && integer(value.value("slot"), 1) == std::uint64_t(pending_.slot) &&
                integer(value.value("sequence")) == lease.sequence && returnedGeneration == lease.generation &&
                integer(value.value("current_generation")) == generation_ &&
                digest(value.value("source_id")) == job_->inputDigest && digest(value.value("model_id")) == modelDigest(),
                "Stem ACK differs from its exact lease");
            lease = {};
        } else if (pending_.kind == "load" || pending_.kind == "seek") {
            keys(value, {"v", "event", "id", "command", "generation", "accepted_at", "frame"});
            require(generation_ < wireMaximum && returnedGeneration == generation_ + 1 &&
                integer(value.value("frame")) == pending_.frame, "Stem accepted generation or anchor differs");
            generation_ = returnedGeneration; loaded_ = active_ = true; sourceSeen_ = rawComplete_ = false;
            rawNext_ = pending_.frame; acceptedEpoch_ = pending_.epoch; convertedInput_ = pending_.frame;
            auto record = observation(Kind::CommandAccepted);
            record.commandId = pending_.id; record.inputFirst44100 = pending_.frame;
            record.outputFirst48000 = pending_.target;
            observe(record);
            if (acceptedEpoch_ != latest_.epoch || closing_) discard();
        } else {
            keys(value, {"v", "event", "id", "command", "generation", "accepted_at"});
            require(pending_.kind == "close" && returnedGeneration == generation_, "Invalid stem close acknowledgement");
        }
        pending_ = {};
    }
    QByteArray modelDigest() const { return job_->files[4].identity.value("sha256").toString().toLatin1(); }
    void current(const QJsonObject& value)
    {
        require(ready_ && active_ && integer(value.value("generation")) == generation_, "Stem event has unknown generation");
        require(digest(value.value("source_id")) == job_->inputDigest, "Stem input identity differs");
    }
    bool validateLease(Lease& lease, const QByteArray& expected)
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        std::array<char, 32768> bytes{};
        const auto size = lease.frames * 32;
        for (std::uint64_t offset = 0; offset < size;) {
            if (stop_.stop_requested()) return false;
            const auto count = std::size_t(std::min<std::uint64_t>(bytes.size(), size - offset));
            readAt(lease.fd->get(), bytes.data(), count, offset);
            for (std::size_t sample = 0; sample < count; sample += 4)
                require(std::isfinite(wireFloat(bytes.data() + sample)), "Live lease contains non-finite PCM");
            hash.addData(QByteArrayView(bytes.data(), qsizetype(count))); offset += count;
        }
        require(hash.result().toHex() == expected && unchanged(lease.before, statusOf(lease.fd->get())),
                "Live lease digest or retained descriptor changed");
        return true;
    }
    void chunk(const QJsonObject& value)
    {
        keys(value, {"v", "event", "slot", "sequence", "generation", "source_id", "model_id", "file", "source_frames", "source_format", "sample_rate", "channels", "stems", "frame_start", "window_start_frame", "frames", "final", "bytes", "sha256", "request_id", "worker_ready_at", "published_at"},
            {"request_ms", "source_read_ms", "overlap_ms", "hop_service_ms", "native_timings"});
        current(value);
        require(sourceSeen_ && !rawComplete_ && digest(value.value("model_id")) == modelDigest() &&
            integer(value.value("source_frames")) == job_->inputFrames && text(value.value("source_format")) == "f32le" &&
            integer(value.value("sample_rate")) == 44100 && integer(value.value("channels")) == 2 &&
            value.value("stems").isArray() && value.value("stems").toArray() == roles, "Stem chunk contract differs");
        const auto slot = std::size_t(integer(value.value("slot"), 1));
        const auto frames = integer(value.value("frames"), hop);
        require(rawNext_ < job_->inputFrames && frames == std::min(hop - rawNext_ % hop, job_->inputFrames - rawNext_) &&
            integer(value.value("frame_start")) == rawNext_ && integer(value.value("window_start_frame")) == rawNext_ / hop * hop &&
            boolean(value.value("final")) == (rawNext_ + frames == job_->inputFrames) && integer(value.value("bytes")) == frames * 32 &&
            lastSequence_ < wireMaximum && integer(value.value("sequence")) == lastSequence_ + 1 &&
            integer(value.value("request_id")) > 0 && !leases_[slot].occupied, "Stem chunk origin/sequence/slot differs");
        const auto file = QString("slot%1.f32").arg(slot);
        require(text(value.value("file")) == file, "Unexpected live lease pathname");
        (void)finite(value.value("worker_ready_at")); (void)finite(value.value("published_at"));
        for (const auto* key : {"request_ms", "source_read_ms", "overlap_ms", "hop_service_ms"})
            if (value.contains(key)) (void)finite(value.value(key));
        const auto nativeTimings = value.contains("native_timings") ? object(value.value("native_timings")) : QJsonObject{};
        keys(nativeTimings, {}, {"native_elapsed_ms", "native_inference_wall_ms", "native_inference_cpu_ms",
             "native_frontend_ms", "native_graph_ms", "native_graph_upload_ms", "native_graph_compute_ms",
             "native_graph_readback_ms", "native_postprocess_ms", "native_graph_rebuild_ms"});
        for (const auto* key : {"native_elapsed_ms", "native_inference_wall_ms", "native_inference_cpu_ms", "native_frontend_ms", "native_graph_ms",
             "native_graph_upload_ms", "native_graph_compute_ms", "native_graph_readback_ms",
             "native_postprocess_ms", "native_graph_rebuild_ms"})
            if (nativeTimings.contains(key)) (void)finite(nativeTimings.value(key));
        const auto expected = digest(value.value("sha256"));
        const auto receiptNs = stamp();
        auto& lease = leases_[slot];
        lease.occupied = true; lease.generation = generation_; lease.sequence = ++lastSequence_;
        lease.first = rawNext_; lease.frames = frames; rawNext_ += frames;
        lease.fd = std::make_unique<Descriptor>(openCacheFile(serviceDirectory_->get(), file.toLatin1().constData()));
        lease.before = statusOf(lease.fd->get());
        require(lease.before.st_size >= 0 && std::uint64_t(lease.before.st_size) == frames * 32, "Live lease size differs");
        lease.validated = validateLease(lease, expected);
        if (lease.validated) {
            auto record = observation(Kind::ChunkValidated);
            record.observedNs = receiptNs; record.validatedNs = stamp();
            record.requestId = integer(value.value("request_id")); record.leaseSequence = lease.sequence;
            record.inputFirst44100 = lease.first; record.inputFrames44100 = lease.frames;
            record.final = boolean(value.value("final"));
            record.workerReadySeconds = value.value("worker_ready_at").toDouble();
            record.servicePublishedSeconds = value.value("published_at").toDouble();
            record.hopServicePresent = value.contains("hop_service_ms");
            if (record.hopServicePresent) record.hopServiceMs = value.value("hop_service_ms").toDouble();
            const auto phase = [](const QJsonObject& timings, const char* key) -> std::optional<double> {
                return timings.contains(key) ? std::optional{timings.value(key).toDouble()} : std::nullopt;
            };
            record.sourceReadMs = phase(value, "source_read_ms"); record.requestMs = phase(value, "request_ms");
            record.overlapMs = phase(value, "overlap_ms"); record.nativeElapsedMs = phase(nativeTimings, "native_elapsed_ms");
            record.nativeInferenceWallMs = phase(nativeTimings, "native_inference_wall_ms");
            record.nativeInferenceCpuMs = phase(nativeTimings, "native_inference_cpu_ms");
            record.nativeFrontendMs = phase(nativeTimings, "native_frontend_ms"); record.nativeGraphMs = phase(nativeTimings, "native_graph_ms");
            record.nativeGraphUploadMs = phase(nativeTimings, "native_graph_upload_ms");
            record.nativeGraphComputeMs = phase(nativeTimings, "native_graph_compute_ms");
            record.nativeGraphReadbackMs = phase(nativeTimings, "native_graph_readback_ms");
            record.nativePostprocessMs = phase(nativeTimings, "native_postprocess_ms");
            record.nativeGraphRebuildMs = phase(nativeTimings, "native_graph_rebuild_ms");
            observe(record);
        }
        if (lease.validated && (acceptedEpoch_ != latest_.epoch || closing_ || !converter_)) {
            lease.fd.reset(); lease.ackReady = true;
        }
    }
    void event(const QJsonObject& value)
    {
        require(integer(value.value("v")) == 1, "Unknown stem service wire version");
        const auto kind = text(value.value("event"));
        if (kind == "ready") { ready(value); return; }
        if (kind == "error") {
            keys(value, {"v", "event", "generation", "code", "message", "retained_job"});
            (void)integer(value.value("generation")); require(!text(value.value("code")).isEmpty(), "Missing worker error code");
            const auto message = text(value.value("message"), 2048);
            require(message.toUcs4().size() <= 1024, "Worker diagnostic exceeds scalar bound");
            (void)boolean(value.value("retained_job"));
            throw std::runtime_error("Live stem service: " + message.toStdString());
        }
        require(ready_, "Stem event preceded ready");
        if (kind == "accepted") { accepted(value); return; }
        if (kind == "runtime") {
            keys(value, {"v", "event", "worker_generation", "model_id", "ready"});
            const auto tag = integer(value.value("worker_generation"));
            require(!runtimeSeen_ && tag > 0 && tag <= generation_ && digest(value.value("model_id")) == modelDigest(),
                    "Unknown or repeated runtime startup tag");
            validateRuntimeReady(object(value.value("ready")), job_->engine); runtimeSeen_ = true;
            auto record = observation(Kind::RuntimeReady);
            record.workerGeneration = tag;
            if (tag != generation_) record.providerEpoch = 0; // Older known startup context, no guessed epoch.
            observe(record);
        } else if (kind == "source") {
            keys(value, {"v", "event", "generation", "source_id", "source_frames", "source_format", "mean", "std"});
            current(value); require(runtimeSeen_ && !sourceSeen_ && integer(value.value("source_frames")) == job_->inputFrames &&
                text(value.value("source_format")) == "f32le", "Invalid stem source context");
            (void)finite(value.value("mean"), true); require(finite(value.value("std")) > 0, "Invalid source normalization");
            sourceSeen_ = true; // Worker PCM is already restored; never apply these a second time.
        } else if (kind == "started") {
            keys(value, {"v", "event", "generation", "source_id", "model_id", "request_id", "window_start_frame", "preroll", "timestamp"});
            current(value); const auto window = integer(value.value("window_start_frame"));
            require(sourceSeen_ && digest(value.value("model_id")) == modelDigest() && integer(value.value("request_id")) > 0 &&
                window < job_->inputFrames && window % hop == 0, "Invalid started telemetry context");
            (void)boolean(value.value("preroll")); (void)finite(value.value("timestamp"));
        } else if (kind == "chunk") chunk(value);
        else if (kind == "complete") {
            keys(value, {"v", "event", "generation", "source_id", "frame_start", "telemetry_dropped", "completed_at"});
            current(value); require(sourceSeen_ && !rawComplete_ && rawNext_ == job_->inputFrames &&
                integer(value.value("frame_start")) == job_->inputFrames, "Stem complete preceded exact raw coverage");
            const auto dropped = integer(value.value("telemetry_dropped"));
            (void)finite(value.value("completed_at")); rawComplete_ = true;
            auto record = observation(Kind::RawComplete);
            record.inputFirst44100 = job_->inputFrames;
            record.serviceTelemetryDroppedPresent = true; record.serviceTelemetryDropped = dropped;
            observe(record);
        } else if (kind == "closed") {
            keys(value, {"v", "event", "generation", "reason", "worker_closed", "outstanding_slots", "telemetry_dropped", "closed_at"});
            const auto reason = text(value.value("reason"));
            require(closing_ && integer(value.value("generation")) == generation_ && boolean(value.value("worker_closed")) &&
                (reason == "close" || reason == "eof" || reason == "signal") &&
                integer(value.value("outstanding_slots"), 2) == std::uint64_t(std::ranges::count_if(leases_, [](const auto& l) { return l.occupied; })),
                "Unexpected stem close or outstanding lease count");
            require(!diagnosticDrain_ || (reason == "close" &&
                std::ranges::none_of(leases_, [](const auto& lease) { return lease.occupied; })),
                "Diagnostic close retained raw leases or was cancelled");
            const auto dropped = integer(value.value("telemetry_dropped"));
            (void)finite(value.value("closed_at")); closed_ = true;
            auto record = observation(Kind::ServiceClosed);
            record.inputFirst44100 = rawNext_;
            record.serviceTelemetryDroppedPresent = true; record.serviceTelemetryDropped = dropped;
            observe(record);
        } else invalid("Unknown stem service event");
    }

    bool refresh()
    {
        if (stop_.stop_requested() || !port_.serviceRecovery()) return false;
        Buffer::Request request;
        if (!port_.tryRequest(request)) return false;
        if (request.epoch != latest_.epoch) { latest_ = request; discard(); observeRequest(); return false; }
        return true;
    }
    bool publish()
    {
        if (!pendingFrames_) return true;
        const auto end = pendingFirst_ + pendingFrames_;
        // The counted final prefix is not an EOF proof. Even a full packet
        // waits for the library's actual zero-output EOF/count validation.
        if (pendingFrames_ < Buffer::blockFrames && !finished_) return false;
        if (end == plan_.outputFrames() && !finished_) return false;
        if (!packetReadyNs_) packetReadyNs_ = stamp();
        if (!refresh() || acceptedEpoch_ != latest_.epoch || !converter_) return false;
        Buffer::Packet packet{latest_.sourceId, latest_.epoch, pendingFirst_, pendingFrames_,
            finished_ && end == plan_.outputFrames(), {}};
        for (std::size_t role = 0; role < 4; ++role)
            packet.stems[role] = std::span<const float>(output_[role]).first(pendingFrames_ * 2);
        const auto beginNs = stamp();
        const auto result = port_.tryPublish(packet);
        const auto endNs = stamp();
        switch (result) {
        case Buffer::PublishResult::Accepted: {
            auto record = observation(Kind::PacketAccepted);
            record.observedNs = endNs; record.outputFirst48000 = pendingFirst_;
            record.outputFrames48000 = pendingFrames_; record.final = packet.final;
            record.packetReadyNs = packetReadyNs_; record.publishBeginNs = beginNs; record.publishEndNs = endNs;
            record.fullObserved = fullObserved_; record.firstFullNs = firstFullNs_;
            observe(record);
            pendingFrames_ = 0; packetReadyNs_ = firstFullNs_ = 0; fullObserved_ = false;
            return true;
        }
        case Buffer::PublishResult::Full:
            if (!fullObserved_) { fullObserved_ = true; firstFullNs_ = endNs; }
            return false; // Retain this exact packet, without per-retry observations.
        case Buffer::PublishResult::Stale: {
            Buffer::Request request;
            if (port_.tryRequest(request) && request.epoch != latest_.epoch) { latest_ = request; discard(); observeRequest(); }
            return false;
        }
        case Buffer::PublishResult::Invalid:
            if (stop_.stop_requested() || !port_.serviceRecovery()) return false;
            invalid("Converted live packet violates its current bank contract");
        }
        return false;
    }
    void releaseInput()
    {
        require(inputSlot_ >= 0, "Missing raw lease for consumed input");
        auto& lease = leases_[std::size_t(inputSlot_)];
        require(lease.fd && unchanged(lease.before, statusOf(lease.fd->get())), "Live lease changed during conversion");
        if (lease.used == lease.frames) {
            // Closing and ACKing raw input is independent of output/ring EOF.
            lease.fd.reset(); lease.ackReady = true;
        }
        inputSlot_ = -1; inputFrames_ = inputOffset_ = 0;
    }
    void fillInput()
    {
        if (inputFrames_ || inputSlot_ >= 0) return;
        int next = -1;
        for (std::size_t slot = 0; slot < leases_.size(); ++slot) {
            const auto& lease = leases_[slot];
            if (lease.occupied && lease.validated && lease.fd && !lease.ackReady && !lease.ackSent &&
                lease.generation == generation_ && lease.used < lease.frames &&
                (next < 0 || lease.first < leases_[std::size_t(next)].first)) next = int(slot);
        }
        if (next < 0) return;
        auto& lease = leases_[std::size_t(next)];
        require(lease.first + lease.used == convertedInput_, "Raw conversion input is not contiguous");
        const auto count = std::size_t(std::min<std::uint64_t>(4096, lease.frames - lease.used));
        std::array<char, 4096 * 8> raw{};
        for (std::size_t role = 0; role < 4; ++role) {
            readAt(lease.fd->get(), raw.data(), count * 8, (role * lease.frames + lease.used) * 8);
            for (std::size_t sample = 0; sample < count * 2; ++sample) input_[role][sample] = wireFloat(raw.data() + sample * 4);
        }
        require(unchanged(lease.before, statusOf(lease.fd->get())), "Live lease changed after validation");
        inputSlot_ = next; inputFrames_ = count; inputOffset_ = 0;
    }
    void convert()
    {
        if (!converter_ || acceptedEpoch_ != latest_.epoch || !active_ || !sourceSeen_ || !refresh()) return;
        // A fixed number of bounded steps per pipe/control turn. Scratch and
        // pending output survive the next turn, including Full and raw gaps.
        for (unsigned work = 0; work < 8; ++work) {
            if (!refresh() || !converter_) return;
            if (pendingFrames_ == 4096 && pendingFirst_ + pendingFrames_ < plan_.outputFrames()) {
                if (!publish()) return;
            }
            if (finished_) { (void)publish(); return; }
            fillInput();
            const bool eof = rawComplete_ && convertedInput_ == plan_.inputFrames() && !inputFrames_;
            std::array<std::span<const float>, 4> input;
            std::array<std::span<float>, 4> output;
            const bool probe = pendingFrames_ == 4096;
            if (probe && !eof) return; // Await raw complete after the final raw ACK.
            require(!probe || pendingFirst_ + pendingFrames_ == plan_.outputFrames(), "Unexpected full converter staging");
            for (std::size_t role = 0; role < 4; ++role) {
                input[role] = std::span<const float>(input_[role]).subspan(inputOffset_ * 2, (inputFrames_ - inputOffset_) * 2);
                // No input remains during the final empty drain. Reuse that
                // consumed scratch for the mandatory nonempty output spans.
                output[role] = probe ? std::span<float>(input_[role]).first(2) :
                    std::span<float>(output_[role]).subspan(pendingFrames_ * 2);
            }
            const auto step = converter_->process(input, output, eof);
            require(step.inputFramesUsed <= inputFrames_ - inputOffset_ && (!probe || step.outputFrames == 0),
                    "Converter exceeded its bounded bridge staging");
            if (step.inputFramesUsed) {
                require(inputSlot_ >= 0, "Converter consumed missing raw input");
                auto& lease = leases_[std::size_t(inputSlot_)];
                lease.used += step.inputFramesUsed; inputOffset_ += step.inputFramesUsed;
                convertedInput_ += step.inputFramesUsed;
                if (inputOffset_ == inputFrames_) releaseInput();
            }
            if (step.outputFrames) {
                require(step.outputFrames <= 4096 - pendingFrames_, "Converter output exceeded packet capacity");
                if (!pendingFrames_) pendingFirst_ = step.firstOutputFrame;
                else require(step.firstOutputFrame == pendingFirst_ + pendingFrames_, "Converter output prefix is not contiguous");
                pendingFrames_ += step.outputFrames;
            }
            if (step.finished && !finished_) {
                auto record = observation(Kind::ConverterComplete);
                record.inputFirst44100 = plan_.inputFrames(); record.outputFirst48000 = plan_.outputFrames();
                observe(record); // Actual converter EOF proof, before final publication/render EOF.
            }
            finished_ = step.finished;
            if (pendingFrames_ == 4096 || finished_) {
                if (pendingFirst_ + pendingFrames_ < plan_.outputFrames() || finished_) {
                    if (!publish()) return;
                }
            }
            if (!step.inputFramesUsed && !step.outputFrames && !step.finished) return;
        }
    }
    std::shared_ptr<LiveJob> job_;
    Converter::FramePlan plan_;
    std::stop_token stop_;
    audio::PreparedPlayback::LiveProducerPort& port_;
    OwnedStemProcess owned_;
    QProcess& process_;
    StemServiceJsonl parser_;
    QByteArray incoming_, diagnostics_;
    std::unique_ptr<Descriptor> serviceDirectory_;
    std::array<Lease, 2> leases_;
    Buffer::Request initial_{}, latest_{};
    Command pending_{};
    std::unique_ptr<Converter> converter_;
    std::array<std::array<float, 4096 * 2>, 4> input_{}, output_{};
    std::size_t pendingFrames_ = 0, inputFrames_ = 0, inputOffset_ = 0;
    int inputSlot_ = -1;
    std::uint64_t pendingFirst_ = 0, convertedInput_ = 0, nextCommand_ = 0,
        acceptedEpoch_ = 0, generation_ = 0, lastSequence_ = 0, rawNext_ = 0;
    bool ready_ = false, loaded_ = false, active_ = false, runtimeSeen_ = false, sourceSeen_ = false,
        rawComplete_ = false, finished_ = false, closing_ = false, closed_ = false;
    bool diagnosticDrain_ = false, diagnosticReady_ = false;
    std::uint64_t packetReadyNs_ = 0, firstFullNs_ = 0;
    bool fullObserved_ = false;
    Clock::time_point replyDeadline_{}, closeDeadline_{}, diagnosticDeadline_{};
};
} // namespace
bool liveName(std::string_view name) noexcept
{
    return name.size() == 22 && name.starts_with(".live-") && std::ranges::all_of(name.substr(6), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::uint64_t liveReservationBytes(int directory, const PreparedTrackStore::Limits& limits, std::stop_token stop)
{
    const auto value = integer(record(directory, stop).value("bytes"));
    if (value > limits.maxCacheBytes) throw std::length_error("Prepared cache live reservation exceeds byte budget");
    constexpr auto fixedMinimum = slotBytes * 2 + spoolBytes + metadataBytes + 1; // At least one diagnostic byte.
    std::uint64_t inputBytes = frameBytes;
    struct stat input{};
    if (fstatat(directory, "input.f32", &input, AT_SYMLINK_NOFOLLOW) == 0) {
        require(S_ISREG(input.st_mode) && input.st_size >= 0 &&
            std::uint64_t(input.st_size) <= audio::PreparedStemInput::defaultMaxPreparedBytes,
            "Unsafe live reservation input geometry");
        // A concurrent cooperating factory can still be writing its named
        // copy under the initial worst-case charge; do not require a finished
        // frame-aligned file here. This is accounting, never source adoption.
        inputBytes = std::max(inputBytes, std::uint64_t(input.st_size));
    } else if (errno != ENOENT) ioError("Cannot inspect live reservation input");
    require(value >= fixedMinimum + inputBytes, "Invalid live reservation byte geometry");
    return value;
}
} // namespace conflux::library::detail

namespace conflux::library {
PreparedLiveStemTrack PreparedTrackStore::prepareLiveStem(const QString& sourcePath,
    const StemEngine& engine, const QString& servicePath, std::uint64_t sourceToken, std::stop_token stop)
{
    using namespace detail;
    auto observations = std::shared_ptr<LiveStemObservations>(new LiveStemObservations);
    LiveStemObservations::Setup setup;
    setup.factoryEntryNs = LiveStemObservationWriter::stamp(*observations);
    checkStop(stop);
    if (!sourceToken) throw std::invalid_argument("Live stem source token must be nonzero");
    Descriptor original(open(QFile::encodeName(sourcePath).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (original.get() < 0) ioError("Cannot open audio source");
    const auto originalBefore = statusOf(original.get());
    if (!S_ISREG(originalBefore.st_mode)) throw std::invalid_argument("Audio input must be a regular local file");
    if (originalBefore.st_size < 0 || std::uint64_t(originalBefore.st_size) > limits_.maxSourceBytes)
        throw std::length_error("Audio source byte budget exceeded");
    const auto originalDigest = scan(original.get(), std::uint64_t(originalBefore.st_size), stop).sha256;
    if (!unchanged(originalBefore, statusOf(original.get()))) throw std::runtime_error("Audio source changed during hashing");
    auto presentation = readTrackPresentation(original.get(), sourcePath, stop);
    auto files = pinEngine(engine, stop);
    auto service = pinStemFile(servicePath, true, stop);
    const auto rootPath = QFileInfo(cacheRoot_).absoluteFilePath();
    if (!QDir().mkpath(rootPath)) throw std::runtime_error("Cannot create prepared cache root");
    auto job = std::make_shared<LiveJob>(rootPath, limits_, engine, std::move(files), std::move(service));
    job->observations = observations;
    job->root = std::make_unique<Descriptor>(open(QFile::encodeName(rootPath).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (job->root->get() < 0) ioError("Cannot open prepared cache root");
    if (fchmod(job->root->get(), 0700) != 0) ioError("Cannot protect prepared cache root");
    // All operands have existing <=2GiB/256MiB ceilings. Reserve both decoder
    // copies during capture and the larger possible future fixed service job.
    const auto serviceOverhead = slotBytes * 2 + spoolBytes + engine.maxDiagnosticBytes;
    const auto initialCharge = std::max(std::uint64_t(limits_.maxPreparedBytes) * 2,
        std::uint64_t(limits_.maxPreparedBytes) + serviceOverhead) + metadataBytes;
    {
        QLockFile lock(QDir(rootPath).filePath(".lock")); lockRoot(lock, stop);
        const auto usage = cacheUsage(job->root->get(), limits_, stop);
        const auto names = directoryNames(job->root->get(), limits_.maxEntries + 1, stop);
        if (std::size_t(std::ranges::count_if(names, [](const auto& name) {
            return liveName(name) || hexDigest(QByteArray::fromStdString(name)); })) >= limits_.maxEntries)
            throw std::length_error("Prepared cache entry limit exceeded");
        if (initialCharge > limits_.maxCacheBytes - usage)
            throw std::length_error("Prepared cache byte budget lacks live-job headroom");
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            const auto value = QRandomGenerator::system()->generate64();
            job->name = ".live-" + QString::number(qulonglong(value), 16).rightJustified(16, '0').toStdString();
            if (mkdirat(job->root->get(), job->name.c_str(), 0700) == 0) break;
            if (errno != EEXIST) ioError("Cannot create live stem job");
            job->name.clear();
        }
        if (job->name.empty()) throw std::runtime_error("Cannot allocate exclusive live stem job");
        job->directory = std::make_unique<Descriptor>(openat(job->root->get(), job->name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (job->directory->get() < 0) {
            const int saved = errno;
            (void)unlinkat(job->root->get(), job->name.c_str(), AT_REMOVEDIR);
            errno = saved; ioError("Cannot open live stem job");
        }
        job->admitted = true;
        job->ownerLock = std::make_unique<Descriptor>(openat(job->directory->get(), ".owner.lock",
            O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (job->ownerLock->get() < 0 || flock(job->ownerLock->get(), LOCK_EX | LOCK_NB) != 0)
            ioError("Cannot hold live stem job ownership");
        job->charge = initialCharge; // Conservative even if initial record publication is cancelled.
        job->publishRecord(initialCharge, stop);
        if (fsync(job->root->get()) != 0) ioError("Cannot synchronize live stem admission");
    }
    const auto jobPath = QDir(rootPath).filePath(QString::fromStdString(job->name));
    std::vector<float> overview;
    {
        auto prepared = audio::prepareStemInput(original.get(), limits_.maxPreparedBytes, stop,
            std::filesystem::path(QFile::encodeName(jobPath).constData()));
        job->inputFrames = prepared.frames();
        Descriptor named(openat(job->directory->get(), "input.f32", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (named.get() < 0) ioError("Cannot create named live stem input");
        auto observed = scan(prepared.descriptor(), job->inputFrames * 8, stop, job->inputFrames, named.get(), true);
        job->inputDigest = std::move(observed.sha256); overview = std::move(observed.peaks);
        if (fsync(named.get()) != 0) ioError("Cannot synchronize live stem input");
    } // Anonymous decoder duplicate closes before reducing the shared charge.
    job->input = std::make_unique<Descriptor>(openCacheFile(job->directory->get(), "input.f32"));
    const auto inputBefore = statusOf(job->input->get());
    if (inputBefore.st_size <= 0 || std::uint64_t(inputBefore.st_size) != job->inputFrames * 8)
        throw std::runtime_error("Named live input geometry changed");
    setup.inputCapturedNs = LiveStemObservationWriter::stamp(*observations);
    Converter::FramePlan plan = [&] {
        try { return Converter::countFrames(job->inputFrames, limits_.maxPreparedBytes / 8, stop); }
        catch (const std::length_error&) { throw std::length_error("Live stem output byte budget exceeded"); }
    }();
    setup.countCompleteNs = LiveStemObservationWriter::stamp(*observations);
    job->serviceBytes = job->inputFrames * 8 + serviceOverhead;
    {
        QLockFile lock(QDir(rootPath).filePath(".lock")); lockRoot(lock, stop);
        // Same record atomically shrinks; no uncharged interval or second
        // admission can spend this operation's decoder headroom prematurely.
        job->publishRecord(job->serviceBytes + metadataBytes, stop);
    }
    verifySource(original.get(), originalBefore, originalDigest, stop);
    verifySource(job->input->get(), inputBefore, job->inputDigest, stop);
    verifyEngine(job->files, stop); verifyStemFile(job->service, stop);
    struct FactoryUse {
        std::shared_ptr<LiveJob> job;
        explicit FactoryUse(std::shared_ptr<LiveJob> value) : job(std::move(value))
        { const std::lock_guard lock(job->mutex); job->factoryActive = true; }
        ~FactoryUse() { const std::lock_guard lock(job->mutex); job->factoryActive = false; }
    } factoryUse(job);
    auto playback = audio::PreparedPlayback::makeLive(sourceToken, plan.outputFrames(),
        [job, plan](std::stop_token ownedStop, audio::PreparedPlayback::LiveProducerPort& port) {
            try { Bridge(job, plan, ownedStop, port).run(); }
            catch (const std::exception& error) {
                job->notifySetup(QString::fromUtf8(error.what()).left(1024)); port.fail(EIO); (void)job->cleanup();
            } catch (...) {
                job->notifySetup(QStringLiteral("Cannot construct the live stem producer")); port.fail(EIO); (void)job->cleanup();
            }
            LiveStemObservationWriter::finish(*job->observations);
        });
    try {
        {
            // Registration is destroyed before the final commit checks. Its
            // callback never stops/joins the producer or touches QProcess.
            std::stop_callback cancelled(stop, [job] {
                const std::lock_guard lock(job->mutex); job->condition.notify_all();
            });
            const auto deadline = Clock::now() + std::min(audio::PreparedPlayback::liveWait,
                std::chrono::seconds(engine.maxSeconds));
            std::unique_lock lock(job->mutex);
            job->condition.wait_until(lock, deadline, [&] { return job->setupDone || stop.stop_requested(); });
            checkStop(stop);
            if (!job->setupDone) throw std::runtime_error("Live stem service ready timed out");
            if (!job->setupError.isEmpty()) throw std::runtime_error(job->setupError.toStdString());
        }
        verifySource(original.get(), originalBefore, originalDigest, stop);
        // Producer owns the retained input after ready; its FD remains stable
        // while installed. Final factory verification cannot reopen a source.
        verifySource(job->input->get(), inputBefore, job->inputDigest, stop);
        verifyEngine(job->files, stop); verifyStemFile(job->service, stop);
        checkStop(stop);
        if (playback->errorCode()) {
            const auto detail = observations->failureDetail();
            throw std::runtime_error(detail.empty() ? playback->error() : detail);
        }
    } catch (...) { playback->stop(); throw; }
    setup.sourceToken = sourceToken; setup.inputFrames44100 = job->inputFrames; setup.outputFrames48000 = plan.outputFrames();
    const auto copyDigest = [](LiveStemObservations::Sha256Hex& target, const QByteArray& source) {
        std::copy_n(source.constData(), target.size(), target.begin()); // Existing validated SHA256 hex.
    };
    copyDigest(setup.sourceSha256, originalDigest); copyDigest(setup.inputSha256, job->inputDigest);
    copyDigest(setup.modelSha256, job->files[4].identity.value("sha256").toString().toLatin1());
    copyDigest(setup.runtimeSha256, job->files[3].identity.value("sha256").toString().toLatin1());
    copyDigest(setup.serviceSha256, job->service.identity.value("sha256").toString().toLatin1());
    setup.factoryCommittedNs = LiveStemObservationWriter::stamp(*observations);
    observations->setup_ = setup; // Factory publication only; producer never reads this storage.
    return {std::move(playback), QString::fromLatin1(originalDigest), QString::fromLatin1(job->inputDigest),
        job->files[4].identity.value("sha256").toString(), job->inputFrames, plan.outputFrames(), job->charge,
        std::move(overview), std::move(observations), std::move(presentation)};
}
} // namespace conflux::library

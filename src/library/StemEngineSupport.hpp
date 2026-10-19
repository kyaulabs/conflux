// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "library/PreparedTrackStore.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QProcess>
#include <array>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
namespace conflux::library::detail {
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
void checkStop(std::stop_token);
[[noreturn]] void ioError(const char*);
struct stat statusOf(int);
bool unchanged(const struct stat&, const struct stat&);
bool hexDigest(const QByteArray&);
void writeAll(int, const char*, std::size_t, std::stop_token);
struct Scan { QByteArray sha256; std::vector<float> peaks; };
// Existing native PCM scan/copy. Wire input explicitly selects F32LE decoding.
Scan scan(int, std::uint64_t, std::stop_token, std::uint64_t frames = 0, int copyTo = -1,
          bool littleEndian = false);
void verifySource(int, const struct stat&, const QByteArray&, std::stop_token);
struct EngineFile {
    QString path, invocation;
    std::unique_ptr<Descriptor> descriptor;
    struct stat before{};
    QJsonObject identity;
};
using EngineFiles = std::array<EngineFile, 5>;
EngineFile pinStemFile(const QString&, bool small, std::stop_token);
void verifyStemFile(const EngineFile&, std::stop_token);
EngineFiles pinEngine(const StemEngine&, std::stop_token);
void verifyEngine(const EngineFiles&, std::stop_token);
void validateDescription(const QJsonObject&, const EngineFiles&);
void validateRuntimeReady(const QJsonObject&, const StemEngine&);
// Private shared lifecycle only. Each caller retains its own bounded protocol
// or one-shot output loop. The service/native inherit this one owned session.
class OwnedStemProcess {
public:
    explicit OwnedStemProcess(bool& groupsGone);
    ~OwnedStemProcess();
    QProcess& process() noexcept { return process_; }
    qint64 group() const noexcept { return group_; }
    bool finish() noexcept;
private:
    QProcess process_;
    bool& groupsGone_;
    qint64 group_ = 0;
    bool finished_ = false, started_ = false;
};
// Existing fixed cache traversal stays in PreparedTrackStore.cpp.
std::vector<std::string> directoryNames(int, std::size_t, std::stop_token stop = {});
struct stat childStatus(int, const std::string&);
int openCacheFile(int, const char*);
std::uint64_t jsonInteger(const QJsonValue&, std::uint64_t);
std::uint64_t cacheUsage(int, const PreparedTrackStore::Limits&, std::stop_token);
bool liveName(std::string_view) noexcept;
std::uint64_t liveReservationBytes(int, const PreparedTrackStore::Limits&, std::stop_token);
} // namespace conflux::library::detail

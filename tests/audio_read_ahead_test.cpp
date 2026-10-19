// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PcmReadAhead.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {
using conflux::audio::AudioSource;
using conflux::audio::PcmReadAhead;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr float sentinel = 12345.F;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// Channel-asymmetric, dyadic values remain bit-exact through the raw file.
float sample(std::uint64_t frame, std::size_t channel)
{
    return static_cast<float>(static_cast<int>((frame * (channel ? 13U : 7U) + channel * 3U) % 8191U) - 4095) / 1024.F;
}

class TemporaryPcm {
public:
    explicit TemporaryPcm(std::uint64_t frames)
    {
        std::array<char, 40> path{};
        const std::string pattern = "/tmp/conflux-read-ahead-XXXXXX";
        std::copy(pattern.begin(), pattern.end(), path.begin());
        fd_ = mkstemp(path.data());
        if (fd_ < 0)
            throw std::system_error(errno, std::generic_category(), "mkstemp");
        path_ = path.data();
        try {
            std::array<float, 2048> block{};
            for (std::uint64_t first = 0; first < frames;) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(block.size() / 2, frames - first));
                for (std::size_t frame = 0; frame < count; ++frame) {
                    block[frame * 2] = sample(first + frame, 0);
                    block[frame * 2 + 1] = sample(first + frame, 1);
                }
                const auto bytes = count * 2 * sizeof(float);
                std::size_t done = 0;
                while (done < bytes) {
                    const auto wrote = write(fd_, reinterpret_cast<const char*>(block.data()) + done, bytes - done);
                    if (wrote < 0 && errno == EINTR)
                        continue;
                    if (wrote <= 0)
                        throw std::runtime_error("Could not write temporary PCM fixture");
                    done += static_cast<std::size_t>(wrote);
                }
                first += count;
            }
        } catch (...) {
            close(fd_);
            unlink(path_.c_str());
            throw;
        }
    }

    ~TemporaryPcm()
    {
        if (fd_ >= 0)
            close(fd_);
        unlink(path_.c_str());
    }
    TemporaryPcm(const TemporaryPcm&) = delete;
    TemporaryPcm& operator=(const TemporaryPcm&) = delete;
    int fd() const noexcept { return fd_; }
    const std::string& path() const noexcept { return path_; }
    void closeOriginal()
    {
        require(close(fd_) == 0, "Could not close original fixture descriptor");
        fd_ = -1;
    }
private:
    int fd_ = -1;
    std::string path_;
};

// Waits are exclusively test/control-thread work. Every delivered prefix is
// compared independently; the reader must leave every unavailable sample alone.
void expectFrames(AudioSource source, std::uint64_t first, std::uint64_t count,
    std::span<const std::size_t> partitions)
{
    std::array<float, 16384> output{};
    const auto deadline = Clock::now() + 5s;
    std::uint64_t delivered = 0;
    std::size_t partition = 0;
    while (delivered < count) {
        require(Clock::now() < deadline, "Timed out awaiting read-ahead frames");
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(
            partitions[partition++ % partitions.size()], count - delivered));
        require(wanted <= output.size() / 2, "Invalid test partition");
        output.fill(sentinel);
        const auto result = source.read(source.context, first + delivered,
            std::span<float>(output).first(wanted * 2));
        require(!result.failed && result.frames <= wanted, "Read-ahead failed or exceeded output capacity");
        for (std::size_t frame = 0; frame < result.frames; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(output[frame * 2 + channel] == sample(first + delivered + frame, channel),
                    "Read-ahead returned stale, reordered or changed PCM");
        require(std::all_of(output.begin() + static_cast<std::ptrdiff_t>(result.frames * 2),
                    output.end(), [](float value) { return value == sentinel; }),
            "Read-ahead overwrote an unavailable output suffix");
        delivered += result.frames;
        if (!result.frames)
            std::this_thread::sleep_for(1ms);
    }
}

void sequentialWrapAndOwnership()
{
    constexpr auto frames = std::uint64_t(PcmReadAhead::blockFrames) * (PcmReadAhead::slotCount * 3 + 3) + 17;
    TemporaryPcm file(frames);
    const auto originalOffset = lseek(file.fd(), 0, SEEK_CUR);
    PcmReadAhead reader(file.fd(), frames);
    auto source = reader.sourceRef();
    require(source.frames == frames && !source.isMemory() && source.read && source.discontinuity,
        "Read-ahead source descriptor is incomplete");
    require(PcmReadAhead::cacheBytes == 1024U * 1024U, "Read-ahead cache unexpectedly grew");
    require(reader.prime(0, 2s), "Initial read-ahead prime failed");
    require(lseek(file.fd(), 0, SEEK_CUR) == originalOffset, "Worker altered the shared file offset");
    file.closeOriginal();
    constexpr std::array partitions{std::size_t(1), std::size_t(17), std::size_t(1023),
        std::size_t(4097), std::size_t(8192), std::size_t(256)};
    expectFrames(source, 0, frames, partitions);
    std::array<float, 8> tail{};
    tail.fill(sentinel);
    const auto eof = source.read(source.context, frames, tail);
    require(!eof.failed && eof.frames == 0 &&
            std::all_of(tail.begin(), tail.end(), [](float value) { return value == sentinel; }),
        "EOF wrote samples or reported a failure");
    require(reader.errorCode() == 0 && reader.error().empty(), "Successful reader retained an error");
}

void seekInvalidationAndPartial()
{
    constexpr auto frames = std::uint64_t(PcmReadAhead::blockFrames) * (PcmReadAhead::slotCount + 9) + 7;
    TemporaryPcm file(frames);
    PcmReadAhead reader(file.fd(), frames);
    auto source = reader.sourceRef();
    require(reader.prime(0, 2s), "Seek fixture prime failed");
    constexpr std::array partitions{std::size_t(3), std::size_t(511), std::size_t(1024)};
    expectFrames(source, 0, 3, partitions);
    source.discontinuity(source.context, 3); // An unchanged cursor keeps its cache.
    expectFrames(source, 3, 19, partitions);
    std::array<float, 3> invalid{sentinel, sentinel, sentinel};
    const auto rejected = source.read(source.context, 22, invalid);
    require(rejected.failed && rejected.frames == 0 && invalid == std::array<float, 3>{sentinel, sentinel, sentinel},
        "Incomplete output frame was not rejected without writes");
    expectFrames(source, 22, 17, partitions);

    for (std::uint64_t index = 0; index < 10000; ++index)
        source.discontinuity(source.context, (index * 7919U) % frames);
    const auto target = PcmReadAhead::blockFrames * 7U + 31U;
    require(reader.prime(target, 2s), "Latest seek did not supersede old read-ahead work");
    expectFrames(source, target, PcmReadAhead::blockFrames * 2U + 19U, partitions);
    // A mismatching caller position must also seek rather than return old data.
    expectFrames(source, 11, 97, partitions);
    require(reader.prime(frames - 7, 2s), "Partial final block could not be primed");
    std::array<float, 32> output{};
    output.fill(sentinel);
    const auto result = source.read(source.context, frames - 7, output);
    require(!result.failed && result.frames == 7, "Final block did not return its exact available prefix");
    for (std::size_t frame = 0; frame < 7; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(output[frame * 2 + channel] == sample(frames - 7 + frame, channel), "Final samples differ");
    require(std::all_of(output.begin() + 14, output.end(), [](float value) { return value == sentinel; }),
        "Partial EOF overwrote the unavailable suffix");
    require(reader.prime(frames, 0ms), "Seeking exactly to EOF should not wait for disk");
    require(!reader.prime(frames + 1, 0ms) && reader.errorCode() == EINVAL,
        "Out-of-range prime was not rejected");
    require(reader.prime(0, 2s), "Valid seek did not recover after invalid control input");
    expectFrames(source, 0, 23, partitions);
}

void truncatedReadAndRecovery()
{
    constexpr auto frames = std::uint64_t(PcmReadAhead::blockFrames) * (PcmReadAhead::slotCount + 8);
    TemporaryPcm file(frames);
    PcmReadAhead reader(file.fd(), frames);
    const auto source = reader.sourceRef();
    require(reader.prime(0, 2s), "Truncation fixture prime failed");
    require(ftruncate(file.fd(), static_cast<off_t>(PcmReadAhead::blockFrames * 2U * sizeof(float))) == 0,
        "Could not truncate fixture");
    const auto missing = PcmReadAhead::blockFrames * (PcmReadAhead::slotCount + 4U);
    require(!reader.prime(missing, 2s) && reader.errorCode() == EIO && !reader.error().empty(),
        "Premature file EOF was confused with declared source EOF");
    std::array<float, 12> output{};
    output.fill(sentinel);
    const auto failed = source.read(source.context, missing, output);
    require(failed.failed && failed.frames == 0 &&
            std::all_of(output.begin(), output.end(), [](float value) { return value == sentinel; }),
        "Truncated source returned PCM or hid its error");
    require(ftruncate(file.fd(), static_cast<off_t>(frames * 2U * sizeof(float))) == 0,
        "Could not restore fixture length after truncation");
    require(reader.prime(0, 2s), "New generation did not recover after restoring the source");
    constexpr std::array partitions{std::size_t(64)};
    expectFrames(source, 0, 128, partitions);
}

void invalidFilesAndCleanup()
{
    bool rejected = false;
    try { PcmReadAhead invalid(-1, 1); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "Invalid descriptor was accepted");
    TemporaryPcm file(PcmReadAhead::blockFrames * (PcmReadAhead::slotCount + 2));
    rejected = false;
    try { PcmReadAhead invalid(file.fd(), 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Empty declared source was accepted");
    const int writeOnly = open(file.path().c_str(), O_WRONLY | O_CLOEXEC);
    require(writeOnly >= 0, "Could not open write-only test descriptor");
    rejected = false;
    try { PcmReadAhead invalid(writeOnly, 1); }
    catch (const std::exception&) { rejected = true; }
    close(writeOnly);
    require(rejected, "Write-only descriptor was accepted");
    std::array<int, 2> pipeFds{};
    require(pipe(pipeFds.data()) == 0, "Could not open test pipe");
    rejected = false;
    try { PcmReadAhead invalid(pipeFds[0], 1); }
    catch (const std::exception&) { rejected = true; }
    close(pipeFds[0]);
    close(pipeFds[1]);
    require(rejected, "Nonregular PCM source was accepted");

    for (int iteration = 0; iteration < 8; ++iteration) {
        PcmReadAhead reader(file.fd(), PcmReadAhead::blockFrames * (PcmReadAhead::slotCount + 2));
        require(reader.prime(0, 2s), "Cleanup fixture prime failed");
        const auto source = reader.sourceRef();
        const auto began = Clock::now();
        reader.stop();
        reader.stop();
        require(Clock::now() - began < 2s, "Stopping an idle/full reader did not join promptly");
        std::array<float, 2> output{sentinel, sentinel};
        const auto result = source.read(source.context, 0, output);
        require(result.failed && result.frames == 0 && output == std::array<float, 2>{sentinel, sentinel},
            "Stopped source remained usable or wrote output");
        require(!reader.prime(0, 1ms) && reader.errorCode() == ECANCELED,
            "Stopped reader accepted prime");
    }
    require(fcntl(file.fd(), F_GETFD) >= 0, "Reader closed its caller's borrowed descriptor");

    TemporaryPcm nonfinite(8);
    const float bad = std::numeric_limits<float>::quiet_NaN();
    require(pwrite(nonfinite.fd(), &bad, sizeof(bad), 0) == static_cast<ssize_t>(sizeof(bad)),
        "Could not write nonfinite test sample");
    PcmReadAhead badReader(nonfinite.fd(), 8);
    require(!badReader.prime(0, 2s) && badReader.errorCode() == EILSEQ,
        "Nonfinite normalized PCM reached the source");
}
} // namespace

int main()
{
    try {
        sequentialWrapAndOwnership();
        seekInvalidationAndPartial();
        truncatedReadAndRecovery();
        invalidFilesAndCleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedLoopCache.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using conflux::audio::PreparedLoopCache;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t frameBytes = 2U * sizeof(float);

void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

struct CloseFile { void operator()(std::FILE* file) const { if (file) std::fclose(file); } };
using File = std::unique_ptr<std::FILE, CloseFile>;
class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) { require(fd >= 0, "Cannot create fixture descriptor"); }
    ~Descriptor() { close(fd_); }
    int get() const { return fd_; }
private:
    int fd_;
};
File rawFile(std::span<const float> samples)
{
    File file(std::tmpfile());
    require(bool(file) && std::fwrite(samples.data(), sizeof(float), samples.size(), file.get()) == samples.size() &&
                std::fflush(file.get()) == 0, "Cannot write private loop PCM");
    return file;
}
bool same(std::span<const float> first, std::span<const float> second)
{
    return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin(),
        [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); });
}
template<class Function> void rejects(Function&& function, const char* message)
{
    try { function(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
template<class Predicate> void wait(Predicate&& predicate, const char* message,
                                     std::chrono::seconds budget = 5s)
{
    const auto deadline = Clock::now() + budget;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
const PreparedLoopCache::View& ready(PreparedLoopCache& cache, std::chrono::seconds budget = 5s)
{
    wait([&] { return cache.view() || cache.errorCode(); }, "Loop cache did not settle", budget);
    require(!cache.errorCode() && cache.view(), "Valid loop cache request failed");
    return *cache.view();
}

struct Fixture {
    static constexpr std::uint64_t frames = 4131;
    std::array<std::vector<float>, 4> samples;
    std::array<File, 4> files;
    std::array<struct stat, 4> identities{};
    Fixture()
    {
        for (std::size_t leaf = 0; leaf < files.size(); ++leaf) {
            auto& pcm = samples[leaf];
            pcm.resize(frames * 2U);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                // Distinct frame/channel/role values expose offsets and reordering.
                pcm[frame * 2] = float(frame + 1 + leaf * 8192) / 1024.F;
                pcm[frame * 2 + 1] = -float(frame + 3 + leaf * 16384) / 2048.F;
            }
            pcm[17 * 2] = -0.F; pcm[17 * 2 + 1] = 0.F;
            pcm[18 * 2] = 2.75F; pcm[18 * 2 + 1] = -3.25F;
            files[leaf] = rawFile(pcm);
            require(fstat(fileno(files[leaf].get()), &identities[leaf]) == 0, "Cannot identify fixture PCM");
        }
    }
    std::array<int, 4> fds() const
    { return {fileno(files[0].get()), fileno(files[1].get()), fileno(files[2].get()), fileno(files[3].get())}; }
};

// Inspect only this fixture's regular inodes; the directory iterator's own FD
// and unrelated process descriptors cannot affect the ownership assertions.
std::set<int> matchingDescriptors(const Fixture& fixture)
{
    std::set<int> result;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const int fd = std::stoi(entry.path().filename().string());
        struct stat status{};
        if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) continue;
        if (std::ranges::any_of(fixture.identities, [&](const auto& wanted) {
                return status.st_dev == wanted.st_dev && status.st_ino == wanted.st_ino;
            })) result.insert(fd);
    }
    return result;
}
void checkView(const PreparedLoopCache::View& view, const Fixture& fixture,
               std::size_t leaves, std::uint64_t first)
{
    require(view.leafCount == leaves && view.firstFrame == first && view.frames == Fixture::frames - first,
            "Loop cache source coordinates or role count differ");
    for (std::size_t leaf = 0; leaf < view.leaves.size(); ++leaf) {
        if (leaf < leaves)
            require(same(view.leaves[leaf], std::span(fixture.samples[leaf]).subspan(first * 2U)),
                    "Loop cache changed raw bits, channels, role order, headroom or signed zero");
        else require(view.leaves[leaf].empty(), "Loop cache invented an unused role");
    }
}

void exactBytesAndDescriptorOwnership()
{
    for (const std::size_t leaves : {1U, 4U}) {
        Fixture fixture;
        auto fds = fixture.fds();
        const auto originals = matchingDescriptors(fixture);
        std::array<off_t, 4> offsets{};
        for (std::size_t leaf = 0; leaf < leaves; ++leaf) {
            offsets[leaf] = off_t((leaf + 3) * frameBytes);
            require(lseek(fds[leaf], offsets[leaf], SEEK_SET) == offsets[leaf], "Cannot set original PCM offset");
        }
        PreparedLoopCache cache{std::span(fds).first(leaves), Fixture::frames};
        require(!cache.view() && !cache.errorCode() && !cache.residentBytes(), "Idle cache read or published PCM");
        const auto owned = matchingDescriptors(fixture);
        require(owned.size() == originals.size() + leaves, "Cache did not retain exactly one duplicate per role");
        for (const int fd : owned) {
            if (originals.contains(fd)) continue;
            const int flags = fcntl(fd, F_GETFD);
            require(flags >= 0 && (flags & FD_CLOEXEC) != 0, "Owned loop-cache FD lacks CLOEXEC");
        }
        require(cache.request(17), "Nonzero IN request rejected");
        checkView(ready(cache), fixture, leaves, 17); // Crosses the worker's 4096-frame block boundary.
        require(cache.residentBytes() >= (Fixture::frames - 17) * frameBytes * leaves &&
                    cache.residentBytes() <= PreparedLoopCache::maxBytes, "Cache payload accounting exceeds its bound");
        for (std::size_t leaf = 0; leaf < leaves; ++leaf)
            require(lseek(fds[leaf], 0, SEEK_CUR) == offsets[leaf], "Cache pread changed a shared original offset");
        for (auto& file : fixture.files) file.reset();
        require(cache.request(Fixture::frames - 3), "Closed-original EOF-clipped IN rejected");
        checkView(ready(cache), fixture, leaves, Fixture::frames - 3);
        cache.stop(); cache.stop();
        require(!cache.view() && !cache.residentBytes() && !cache.request(0) && !cache.cancel() &&
                    matchingDescriptors(fixture).empty(), "Stopped cache retained storage/FDs or accepted work");
    }
}

void constructorValidationAndCleanup()
{
    Fixture fixture;
    auto fds = fixture.fds();
    const auto before = matchingDescriptors(fixture);
    for (const std::size_t count : {0U, 2U, 3U})
        rejects([&] { PreparedLoopCache bad{std::span(fds).first(count), Fixture::frames}; },
                "Unsupported loop-cache role count accepted");
    const std::array<int, 5> five{fds[0], fds[1], fds[2], fds[3], fds[0]};
    rejects([&] { PreparedLoopCache bad(five, Fixture::frames); }, "Five cache roles accepted");
    for (const auto frames : {std::uint64_t(0), Fixture::frames - 1, Fixture::frames + 1,
                              std::numeric_limits<std::uint64_t>::max()})
        rejects([&] { PreparedLoopCache bad(fds, frames); }, "Invalid/excess/inexact source geometry accepted");
    auto badFds = fds; badFds[3] = -1;
    rejects([&] { PreparedLoopCache bad(badFds, Fixture::frames); }, "Invalid fourth FD accepted");
    const auto path = "/proc/self/fd/" + std::to_string(fds[3]);
    Descriptor writeOnly(open(path.c_str(), O_WRONLY | O_CLOEXEC));
    badFds[3] = writeOnly.get();
    rejects([&] { PreparedLoopCache bad(badFds, Fixture::frames); }, "Write-only fourth FD accepted");
#ifdef O_PATH
    Descriptor pathOnly(open(path.c_str(), O_PATH | O_CLOEXEC));
    badFds[3] = pathOnly.get();
    rejects([&] { PreparedLoopCache bad(badFds, Fixture::frames); }, "O_PATH fourth FD accepted");
#endif
    std::array<int, 2> pipeFds{};
    require(pipe(pipeFds.data()) == 0, "Cannot create nonregular fixture");
    Descriptor readPipe(pipeFds[0]), writePipe(pipeFds[1]);
    const std::array<int, 1> nonregular{readPipe.get()};
    rejects([&] { PreparedLoopCache bad(nonregular, 1); }, "Nonregular source accepted");
    auto expected = before;
    expected.insert(writeOnly.get());
#ifdef O_PATH
    expected.insert(pathOnly.get());
#endif
    require(matchingDescriptors(fixture) == expected, "Rejected partial construction leaked duplicated FDs");
}

void invalidRequestsCancelAndLatestPublication()
{
    Fixture fixture;
    const auto fds = fixture.fds();
    PreparedLoopCache cache(fds, Fixture::frames);
    require(cache.request(17), "Initial cache request rejected");
    const auto* previous = &ready(cache);
    for (const auto invalid : {Fixture::frames, Fixture::frames + 1, std::numeric_limits<std::uint64_t>::max()}) {
        require(!cache.request(invalid) && cache.view() == previous && !cache.errorCode(),
                "Invalid IN replaced or invalidated a ready cache view");
        checkView(*cache.view(), fixture, 4, 17);
    }
    // Do not borrow the old pointer after an accepted request. Rapid supersession
    // exercises real worker publication; only the final requested IN may appear.
    require(cache.request(0) && cache.request(29) && cache.cancel(), "Cannot supersede/cancel cache preparation");
    require(!cache.view() && !cache.errorCode(), "Cancel exposed an obsolete view or stale error");
    require(cache.request(31) && cache.request(Fixture::frames - 2), "Latest cache requests rejected");
    const auto& latest = ready(cache);
    checkView(latest, fixture, 4, Fixture::frames - 2);
    require(cache.cancel() && !cache.view() && !cache.errorCode(), "Cancel did not immediately hide ready PCM");
    cache.stop();
    require(!cache.residentBytes(), "Cancelled cache payload survived joined stop");
}

void badRawSamplesAndChangedFilesFailClosed()
{
    for (const std::size_t leaves : {1U, 4U}) {
        for (std::size_t broken = 0; broken < leaves; ++broken) {
            Fixture fixture;
            const auto fds = fixture.fds();
            const auto bad = broken % 3 == 0 ? std::numeric_limits<float>::quiet_NaN() :
                             broken % 3 == 1 ? std::numeric_limits<float>::infinity() :
                                              -std::numeric_limits<float>::infinity();
            require(pwrite(fds[broken], &bad, sizeof(bad), off_t((Fixture::frames * 2U - 1) * sizeof(float))) ==
                        ssize_t(sizeof(bad)), "Cannot inject nonfinite selected PCM");
            // Capture the malformed file's identity at construction so this
            // specifically reaches finite-sample validation, not ESTALE.
            PreparedLoopCache cache{std::span(fds).first(leaves), Fixture::frames};
            require(cache.request(17), "Nonfinite request was not admitted for worker validation");
            wait([&] { return cache.errorCode() != 0; }, "Nonfinite cache input did not fail");
            require(cache.errorCode() == EILSEQ && !cache.view() && !cache.residentBytes(),
                    "A nonfinite role published a partial view or lost its finite-validation error");
            require(cache.cancel() && !cache.errorCode() && !cache.view(), "Cancelled failure remained current");
        }
        for (std::size_t broken = 0; broken < leaves; ++broken) {
            Fixture fixture;
            const auto fds = fixture.fds();
            PreparedLoopCache cache{std::span(fds).first(leaves), Fixture::frames};
            require(ftruncate(fds[broken], off_t(Fixture::frames * frameBytes - sizeof(float))) == 0,
                    "Cannot truncate selected PCM");
            require(cache.request(17), "Truncated request rejected before worker validation");
            wait([&] { return cache.errorCode() != 0; }, "Changed cache input did not fail");
            require(!cache.view() && !cache.residentBytes(), "Truncated role published incomplete PCM");
            cache.stop();
            require(!cache.residentBytes(), "Failed cache retained payload after stop");
        }
    }
}

void maximumWindowUsesOnlyItsBoundedRange()
{
    require(PreparedLoopCache::maxFrames == 64U * 48000U &&
                PreparedLoopCache::maxBytes == 4U * 64U * 48000U * frameBytes,
            "Declared cache frame/byte cap changed");
    File file(std::tmpfile());
    require(bool(file), "Cannot create sparse cap fixture");
    const auto sourceFrames = PreparedLoopCache::maxFrames + 9;
    require(ftruncate(fileno(file.get()), off_t(sourceFrames * frameBytes)) == 0, "Cannot size sparse cap fixture");
    const std::array<float, 2> marker{2.75F, -3.25F};
    const float outside = std::numeric_limits<float>::quiet_NaN();
    require(pwrite(fileno(file.get()), marker.data(), sizeof(marker),
                   off_t((PreparedLoopCache::maxFrames - 1) * frameBytes)) == ssize_t(sizeof(marker)) &&
                pwrite(fileno(file.get()), &outside, sizeof(outside), off_t(PreparedLoopCache::maxFrames * frameBytes)) ==
                    ssize_t(sizeof(outside)), "Cannot write cap boundary sentinels");
    const std::array<int, 1> fds{fileno(file.get())};
    PreparedLoopCache cache(fds, sourceFrames);
    require(cache.request(0), "A source longer than the cache cap was rejected");
    const auto& view = ready(cache, 20s);
    require(view.firstFrame == 0 && view.frames == 64U * 48000U && view.leafCount == 1 &&
                view.leaves[0].size() == 64U * 48000U * 2U &&
                view.leaves[0][0] == 0.F && view.leaves[0][1] == 0.F &&
                same(std::span(view.leaves[0]).last(2), marker) &&
                view.leaves[1].empty() && view.leaves[2].empty() && view.leaves[3].empty() &&
                cache.residentBytes() <= PreparedLoopCache::maxBytes,
            "64-second cache read past its bound, omitted the last frame or expanded unused roles");
    // The NaN immediately beyond the cap would fail an overread. Release the
    // one 24.576MB leaf promptly; no four-role full-length fixture is needed.
    require(cache.cancel() && !cache.view() && !cache.errorCode(), "Cap fixture did not cancel immediately");
    cache.stop();
    require(!cache.residentBytes(), "Cap fixture retained allocation after joined cancellation");
}
} // namespace

int main()
{
    try {
        exactBytesAndDescriptorOwnership();
        constructorValidationAndCleanup();
        invalidRequestsCancelAndLatestPublication();
        badRawSamplesAndChangedFilesFailClosed();
        maximumWindowUsesOnlyItsBoundedRange();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

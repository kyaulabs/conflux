// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedScratchSource.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Source = conflux::audio::PreparedScratchSource;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
std::size_t assertions = 0;
void require(bool value, const char *message) {
    ++assertions;
    if (!value)
        throw std::runtime_error(message);
}
template <class Function> void rejects(Function &&function, const char *message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, message);
}
template <class Predicate> void wait(Predicate &&predicate, const char *message) {
    const auto deadline = Clock::now() + 5s;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
struct CloseFile {
    void operator()(std::FILE *file) const {
        if (file)
            std::fclose(file);
    }
};
using File = std::unique_ptr<std::FILE, CloseFile>;
File file(std::span<const float> samples) {
    File result(std::tmpfile());
    require(result && std::fwrite(samples.data(), sizeof(float), samples.size(), result.get()) == samples.size() &&
                std::fflush(result.get()) == 0,
            "Cannot write generated scratch fixture");
    return result;
}
bool bits(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
struct Fixture {
    static constexpr std::uint64_t frames = 32768;
    std::array<std::vector<float>, 4> pcm;
    std::array<File, 4> files;
    Fixture() {
        for (std::size_t leaf = 0; leaf < 4; ++leaf) {
            pcm[leaf].resize(frames * 2);
            for (std::uint64_t i = 0; i < frames; ++i) {
                pcm[leaf][i * 2] = float(int((i * 7 + leaf * 13) % 4093) - 2046) / 1024.F;
                pcm[leaf][i * 2 + 1] = -float(int((i * 11 + leaf * 31) % 4057) - 2028) / 512.F;
            }
            for (const auto i : {10020U, 10021U, 10022U, 9970U}) {
                pcm[leaf][i * 2] = -0.F;
                pcm[leaf][i * 2 + 1] = i % 2 ? -3.25F : 2.75F;
            }
            files[leaf] = file(pcm[leaf]);
        }
    }
    std::array<int, 4> fds() const {
        return {fileno(files[0].get()), fileno(files[1].get()), fileno(files[2].get()), fileno(files[3].get())};
    }
};

void motionEnvelopeDeclicksStopsAndResumes() {
    constexpr std::size_t fadeFrames = 240;
    constexpr std::uint64_t frames = 65536;
    constexpr std::array<std::array<float, 2>, 4> levels{{
        {.5F, -.25F}, {1.F, -.5F}, {-.75F, 0.F}, {2.F, -1.F}}};
    std::array<File, 4> files;
    std::array<int, 4> fds{};
    for (std::size_t leaf = 0; leaf < files.size(); ++leaf) {
        std::vector<float> pcm(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            pcm[i * 2] = levels[leaf][0];
            pcm[i * 2 + 1] = levels[leaf][1];
        }
        files[leaf] = file(pcm);
        fds[leaf] = fileno(files[leaf].get());
    }
    Source source(fds, frames);
    require(source.prime(8000, 5s) && source.begin(8000), "Cannot prepare de-click fixture");
    const auto fresh = source.read(37);
    require(fresh.frames == 37 && fresh.nominalPosition == 8000 && !fresh.failed,
            "Fresh stationary Begin changed its cursor");
    for (const auto leaf : fresh.leaves)
        require(std::ranges::all_of(leaf, [](float x) { return x == 0.F; }),
                "Fresh stationary Begin emitted an old motion tail");

    std::array<std::array<float, 2>, 4> previous{};
    const auto rememberLast = [&](const Source::Read &read) {
        require(read.frames && !read.failed, "Expected complete resident de-click PCM");
        for (std::size_t leaf = 0; leaf < levels.size(); ++leaf)
            for (std::size_t channel = 0; channel < 2; ++channel)
                previous[leaf][channel] = read.leaves[leaf][(read.frames - 1) * 2 + channel];
    };
    const auto checkBoundary = [&](const Source::Read &read) {
        require(read.frames && !read.failed, "De-click transition lost resident PCM");
        for (std::size_t i = 0; i < read.frames; ++i) {
            const double sharedGain = read.leaves[0][i * 2] / levels[0][0];
            require(sharedGain >= -2e-6 && sharedGain <= 1 + 2e-6,
                    "Motion envelope overshot unity or changed polarity");
            for (std::size_t leaf = 0; leaf < levels.size(); ++leaf) {
                require(read.leaves[leaf].size() == read.frames * 2, "De-click leaf geometry changed");
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    const float value = read.leaves[leaf][i * 2 + channel];
                    if (levels[leaf][channel] == 0.F)
                        require(value == 0.F, "Motion envelope contaminated an exactly silent lane");
                    require(std::isfinite(value) &&
                                std::abs(value - previous[leaf][channel]) <=
                                    std::abs(levels[leaf][channel]) / fadeFrames + 2e-6,
                            "Scratch motion-to-hold boundary contains an unbounded PCM step");
                    require(std::abs(value - levels[leaf][channel] * sharedGain) < 2e-6,
                            "De-click envelope changed stereo/stem proportions or a silent lane");
                    previous[leaf][channel] = value;
                }
            }
        }
    };
    const auto requireUnity = [&](const Source::Read &read) {
        require(read.frames >= fadeFrames && !read.failed, "Missing sustained-motion PCM");
        for (std::size_t i = read.frames - 64; i < read.frames; ++i)
            for (std::size_t leaf = 0; leaf < levels.size(); ++leaf)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    require(std::abs(read.leaves[leaf][i * 2 + channel] - levels[leaf][channel]) < 2e-6,
                            "Sustained motion failed to recover unity track amplitude");
    };
    const auto requireZero = [&](const Source::Read &read) {
        require(read.frames && !read.failed, "Missing settled hold PCM");
        for (const auto leaf : read.leaves)
            require(std::ranges::all_of(leaf, [](float x) { return x == 0.F; }),
                    "Settled motion envelope did not reach exact silence");
    };

    std::int64_t cumulative = 0;
    for (int block = 0; block < 3; ++block) {
        cumulative += 1024;
        require(source.move(cumulative, 1024.0 / Source::sampleRate), "Cannot drive steady envelope fixture");
        const auto moving = source.read(1024);
        require(moving.frames == 1024, "Steady envelope fixture lost PCM");
        requireUnity(moving);
        rememberLast(moving);
    }
    const auto target = 8000 + cumulative;
    for (int i = 0; source.position() != target && i < 64; ++i)
        checkBoundary(source.read(1));
    require(source.position() == target, "Target stop did not retain its exact source cursor");
    checkBoundary(source.read(fadeFrames)); // Original implementation jumps straight to zero here.
    require(source.position() == target, "Stop envelope advanced the held source cursor");
    for (const auto &leaf : previous)
        require(leaf[0] == 0.F && leaf[1] == 0.F, "Target release exceeded 240 output frames");
    requireZero(source.read(37));

    cumulative += 1024;
    require(source.move(cumulative, 1024.0 / Source::sampleRate), "Cannot resume held platter motion");
    const auto resumed = source.read(512);
    checkBoundary(resumed);
    requireUnity(resumed);
    require(source.position() > target && source.position() <= target + 512,
            "Resume teleported or failed to consume actual track motion");

    cumulative += 8192;
    require(source.move(cumulative, .001), "Cannot drive watchdog de-click fixture");
    const auto fast = source.read(512);
    checkBoundary(fast);
    requireUnity(fast);
    std::this_thread::sleep_for(35ms); // Existing functional watchdog trigger, not an audible-time guarantee.
    checkBoundary(source.read(512));
    const auto stopped = source.position();
    requireZero(source.read(37));
    require(source.position() == stopped, "Watchdog release extrapolated after its bounded stop");

    cumulative += 8192;
    require(source.move(cumulative, .001), "Cannot restart before direction reversal");
    const auto forward = source.read(512);
    checkBoundary(forward);
    requireUnity(forward);
    const auto beforeReverse = source.position();
    cumulative -= 8192;
    require(source.move(cumulative, .001), "Cannot reverse de-click fixture");
    const auto reverse = source.read(768);
    checkBoundary(reverse);
    requireUnity(reverse);
    require(source.position() < beforeReverse, "Reversal envelope hid actual reverse track motion");
    const auto newBegin = std::uint64_t(source.position());
    source.end();
    require(source.begin(newBegin), "Cannot begin a fresh stationary gesture after motion");
    requireZero(source.read(37));
    require(source.position() == newBegin, "Fresh Begin retained a prior gesture's envelope or cursor");
}

void geometryOwnershipAndControls() {
    Fixture fixture;
    auto fds = fixture.fds();
    rejects([&] { Source source(std::span<const int>{}, Fixture::frames); }, "Empty FD geometry accepted");
    rejects([&] { Source source(std::span<const int>(fds).first(2), Fixture::frames); }, "Two leaves accepted");
    rejects([&] { Source source(std::span<const int>(fds).first(1), 0); }, "Empty source accepted");
    rejects([&] { Source source(std::span<const int>(fds).first(1), Fixture::frames + 1); },
            "Wrong file size accepted");
    const std::array<int, 1> invalid{-1};
    rejects([&] { Source source(invalid, Fixture::frames); }, "Invalid descriptor accepted");
    int pipes[2];
    require(pipe(pipes) == 0, "Cannot create fixture pipe");
    const std::array<int, 1> pipeFd{pipes[0]};
    rejects([&] { Source source(pipeFd, Fixture::frames); }, "Nonregular source accepted");
    close(pipes[0]);
    close(pipes[1]);
    require(lseek(fds[0], 123, SEEK_SET) == 123, "Cannot set original descriptor offset");
    Source source(std::span<const int>(fds).first(1), Fixture::frames);
    require(source.leafCount() == 1 && source.residentBytes() == 3ULL * Source::windowFrames * 8,
            "Scratch payload is not the fixed three-window normal allocation");
    require(source.prime(10000, 5s) && lseek(fds[0], 0, SEEK_CUR) == 123, "Prime changed shared FD offset");
    const auto primedEpoch = source.epoch();
    std::stop_source cancelled;
    cancelled.request_stop();
    require(!source.prime(0, std::chrono::milliseconds::max()) && !source.prime(0, -1ms) &&
                !source.prime(0, 5s, cancelled.get_token()) && source.epoch() == primedEpoch &&
                source.position() == 10000,
            "Overflowing/negative/already-cancelled prime mutated the previous request");
    // Inspect ONLY this process's descriptors: the extra matching FD must be
    // independently owned and CLOEXEC. No child, external process or device.
    struct stat original{};
    require(fstat(fds[0], &original) == 0, "Cannot stat generated PCM");
    DIR *directory = opendir("/proc/self/fd");
    require(directory, "Cannot enumerate own descriptors");
    std::size_t duplicates = 0;
    while (auto *entry = readdir(directory)) {
        char *end = nullptr;
        const auto number = std::strtol(entry->d_name, &end, 10);
        if (!end || *end || number == fds[0] || number < 0)
            continue;
        struct stat actual{};
        if (fstat(int(number), &actual) == 0 && actual.st_dev == original.st_dev && actual.st_ino == original.st_ino) {
            ++duplicates;
            require((fcntl(int(number), F_GETFD) & FD_CLOEXEC) != 0, "Scratch duplicate is not CLOEXEC");
        }
    }
    closedir(directory);
    require(duplicates == 1, "Expected one independently owned normal scratch descriptor");
    fixture.files[0].reset(); // Worker must retain its own FD.
    require(source.prime(17, 5s) && source.begin(17), "Closing original descriptor invalidated scratch owner");
    const auto epoch = source.epoch();
    const auto held = source.read(37);
    require(held.frames == 37 && held.nominalPosition == 17 && !held.failed &&
                std::ranges::all_of(held.leaves[0], [](float x) { return x == 0.F; }),
            "Held platter emitted DC/advanced cursor");
    require(!source.begin(18) && !source.follow(18) && !source.move(10, 0) && !source.move(10, -1) &&
                !source.move(10, 2) && !source.move(10, std::numeric_limits<double>::quiet_NaN()) &&
                !source.move(10, std::numeric_limits<double>::infinity()) && source.read(0).frames == 0 &&
                source.read(Source::maxReadFrames + 1).frames == 0 && source.position() == 17 &&
                source.epoch() == epoch,
            "Invalid controls/read mutated active position or epoch");
    source.end();
    require(!source.active() && source.read(4).frames == 0 && !source.relocate(Fixture::frames + 1) &&
                source.position() == 17 && source.follow(64) && source.position() == 64 && source.epoch() == epoch,
            "End/follow/invalid relocate changed control semantics");
    source.stop();
    source.stop();
    require(!source.active() && !source.pending() && !source.begin(0) && !source.follow(0) && !source.relocate(0) &&
                source.residentBytes() == 0 && source.read(1).frames == 0,
            "Quiesced stop did not join/release/disable owner");
}

void warmToTarget(Source &source, double target, double stepBound = 1) {
    require(std::isfinite(target) && std::isfinite(stepBound) && stepBound > 0 && stepBound <= Source::maximumSpeed,
            "Invalid fixture warm target or motion bound");
    for (std::size_t i = 0; source.position() != target && i < 2000; ++i) {
        const auto before = source.position();
        require(std::isfinite(before), "Warm movement started from a nonfinite cursor");
        const auto remaining = std::abs(target - before);
        // Bound by remaining distance and this gesture's maximum step so the
        // target cannot precede the last requested frame. No held surplus may
        // fade the envelope before the following exact unit-rate oracle.
        const auto batch = static_cast<std::size_t>(std::clamp(std::ceil(remaining / stepBound),
            1., double(Source::maxReadFrames)));
        const auto value = source.read(batch);
        const auto after = source.position();
        const auto moved = (target > before ? 1 : -1) * (after - before);
        require(value.frames == batch && !value.failed && !source.errorCode(), "Warm movement unexpectedly lacked healthy PCM");
        require(std::isfinite(after) && moved > 0 && moved <= remaining && moved <= stepBound * double(batch),
                "Warm movement stopped, reversed, overshot or exceeded its bounded source advance");
    }
    require(source.position() == target, "Bounded movement did not stop at exact target");
}
void exactUnitMotionAndStemOrder() {
    Fixture fixture;
    const auto fds = fixture.fds();
    for (const auto leafCount : {1U, 4U}) {
        Source source(std::span<const int>(fds).first(leafCount), Fixture::frames);
        require(source.prime(9505, 5s) && source.begin(9505), "Cannot begin prepared normal/bank scratch");
        // Finish a short real startup segment at an integer cursor before
        // adding new unit-rate displacement. Both targets precede the unchanged
        // exact PCM oracle; no expired motion is resent or target skipped.
        require(source.move(64, 64.0 / Source::sampleRate), "Cannot start forward warm motion");
        warmToTarget(source, 9569);
        require(source.move(512, 448.0 / Source::sampleRate), "Cannot continue forward warm motion");
        warmToTarget(source, 10017);
        require(source.move(992, .01), "Cannot request exact unit forward motion");
        const auto value = source.read(480);
        require(value.frames == 480 && value.nominalPosition == 10497 && !value.failed,
                "Unit forward count/cursor mismatch");
        for (std::size_t leaf = 0; leaf < leafCount; ++leaf)
            for (std::size_t i = 0; i < value.frames * 2; ++i)
                require(bits(value.leaves[leaf][i], fixture.pcm[leaf][10017 * 2 + i]),
                        "Unit forward PCM changed bits, signed zero, headroom, stereo or stem order");
        require(source.epoch() == source.generation(), "Generation alias mismatch");
        source.end();
        require(source.begin(10495) && source.move(-64, 64.0 / Source::sampleRate), "Cannot start reverse warm motion");
        warmToTarget(source, 10431);
        require(source.move(-512, 448.0 / Source::sampleRate), "Cannot continue reverse warm motion");
        warmToTarget(source, 9983);
        require(source.move(-992, .01), "Cannot request exact unit reverse motion");
        const auto reverse = source.read(480);
        require(reverse.frames == 480 && reverse.nominalPosition == 9503 && !reverse.failed,
                "Unit reverse count/cursor mismatch");
        for (std::size_t leaf = 0; leaf < leafCount; ++leaf)
            for (std::size_t frame = 0; frame < reverse.frames; ++frame)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    require(bits(reverse.leaves[leaf][frame * 2 + channel],
                                 fixture.pcm[leaf][(9982 - frame) * 2 + channel]),
                            "Reverse did not read actual preceding stereo/stem PCM");
        const auto release = source.read(240);
        require(release.frames == 240 && release.nominalPosition == 9503 && !release.failed,
                "De-click release changed the exact target cursor");
        const auto held = source.read(13);
        require(held.frames == 13 && held.nominalPosition == 9503, "Target hold changed its position/count");
        for (std::size_t leaf = 0; leaf < leafCount; ++leaf)
            require(held.leaves[leaf].size() == 26 &&
                        std::ranges::all_of(held.leaves[leaf], [](float x) { return x == 0.F; }),
                    "Target hold repeated a leaf's DC sample");
    }
}

void activeGestureReversalDiscardsOldBacklog() {
    Fixture fixture;
    const auto fds = fixture.fds();
    Source source(std::span<const int>(fds).first(1), Fixture::frames);
    require(source.prime(10000, 5s) && source.begin(10000) && source.move(480, .01),
            "Cannot prepare an active forward gesture");
    const auto first = source.read(1);
    require(first.frames == 1 && !first.failed && source.position() > 10000 && source.position() < 10001,
            "Reversal fixture did not establish a lagging actual source cursor");
    const auto turningPosition = source.position();
    require(source.move(240, .01), "Cannot reverse cumulative gesture while old target remains ahead");
    bool reversed = false;
    double energy = 0;
    for (std::size_t frame = 0; frame < 32; ++frame) {
        const auto before = source.position();
        const auto value = source.read(1);
        require(value.frames == 1 && !value.failed && std::abs(source.position() - before) <= Source::maximumSpeed,
                "Direction reversal starved, teleported or exceeded bounded source speed");
        reversed |= source.position() < turningPosition;
        for (float sample : value.leaves[0]) {
            require(std::isfinite(sample), "Direction reversal returned nonfinite track PCM");
            energy += double(sample) * sample;
        }
    }
    require(reversed && source.position() < turningPosition && energy > 0,
            "Finger reversal chased undelivered forward target instead of braking/reversing actual track PCM");
    // Subsequent cumulative input must retain the rebased gesture mapping;
    // it cannot resurrect the abandoned +480/+240 forward destination.
    const auto afterTurn = source.position();
    require(source.move(120, .01), "Cannot continue the reversed cumulative gesture");
    const auto continued = source.read(32);
    require(continued.frames == 32 && !continued.failed && source.position() < afterTurn,
            "Later cumulative input lost the active-gesture direction rebase");
}

void endpointOvershootAndSmallReverse() {
    Fixture fixture;
    const auto fds = fixture.fds();
    Source source(std::span<const int>(fds).first(1), Fixture::frames);
    require(source.prime(Fixture::frames - 64, 5s) && source.begin(Fixture::frames - 64) &&
                source.move(std::numeric_limits<std::int64_t>::max(), .01),
            "Cannot overshoot EOF with legal cumulative input");
    warmToTarget(source, Fixture::frames, Source::maximumSpeed);
    const auto epoch = source.epoch();
    require(source.move(std::numeric_limits<std::int64_t>::max() - 1, .01),
            "Cannot request a one-frame reverse after overshoot");
    bool reverse = false;
    for (std::size_t i = 0; source.position() != Fixture::frames - 1 && i < 2000; ++i) {
        const auto before = source.position();
        const auto value = source.read(1);
        require(value.frames == 1 && !value.failed && source.position() <= before &&
                    source.position() >= Fixture::frames - 1 &&
                    std::abs(source.position() - before) <= Source::maximumSpeed,
                "EOF reverse chased clamped overshoot or jumped outside its actual one-frame target");
        reverse |= source.position() < before;
    }
    require(reverse && source.position() == Fixture::frames - 1 && source.epoch() == epoch,
            "Small reverse remained pinned behind an undelivered extreme positive target");
    source.end();
    require(source.begin(64) && source.move(std::numeric_limits<std::int64_t>::min(), .01),
            "Cannot overshoot beginning");
    warmToTarget(source, 0, Source::maximumSpeed);
    require(source.move(std::numeric_limits<std::int64_t>::min() + 1, .01),
            "Cannot request one-frame forward after negative overshoot");
    for (std::size_t i = 0; source.position() != 1 && i < 2000; ++i) {
        const auto before = source.position();
        const auto value = source.read(1);
        require(value.frames == 1 && !value.failed && source.position() >= before && source.position() <= 1,
                "Beginning reverse resurrected extreme negative target or exceeded one frame");
    }
    require(source.position() == 1 && source.epoch() == epoch,
            "Small forward remained pinned after negative overshoot");
    source.end();
    require(source.begin(10000) && source.move(std::numeric_limits<std::int64_t>::min(), .01),
            "Cannot set wide-delta negative gesture");
    require(source.read(1).frames == 1, "Wide-delta fixture lacked prepared PCM");
    const auto turningPosition = source.position();
    require(source.move(std::numeric_limits<std::int64_t>::max(), .01), "Signed-extreme reversal was rejected");
    const auto positive = source.read(512);
    require(positive.frames == 512 && !positive.failed && source.position() > turningPosition &&
                source.position() <= Fixture::frames,
            "INT64_MIN to INT64_MAX delta overflowed or did not reverse actual source direction");
}

float rolloverSample(std::uint64_t frame, std::size_t leaf, std::size_t channel) {
    const auto modulus = channel ? 4057ULL : 4093ULL;
    const auto multiplier = channel ? 11ULL : 7ULL;
    const auto bias = channel ? 31ULL : 13ULL;
    return float(std::int64_t((frame * multiplier + leaf * bias) % modulus) - 2028) / (channel ? -512.F : 1024.F);
}
void sameEpochBidirectionalWindowRollover() {
    constexpr std::uint64_t frames = Source::windowFrames * 3;
    std::array<File, 4> files;
    std::array<int, 4> fds{};
    for (std::size_t leaf = 0; leaf < 4; ++leaf) {
        std::vector<float> pcm(frames * 2);
        for (std::uint64_t frame = 0; frame < frames; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel)
                pcm[frame * 2 + channel] = rolloverSample(frame, leaf, channel);
        files[leaf] = file(pcm);
        fds[leaf] = fileno(files[leaf].get());
    }
    Source source(fds, frames);
    require(source.residentBytes() == 3ULL * 4 * Source::windowFrames * 8 && source.prime(17, 5s) && source.begin(17) &&
                source.move(64, 64.0 / Source::sampleRate),
            "Cannot prepare bidirectional rollover fixture");
    // Preserve the original 512-frame endpoint and later cumulative sequence,
    // using two genuine unit-rate gesture segments through an integer cursor.
    warmToTarget(source, 81);
    require(source.move(512, 448.0 / Source::sampleRate), "Cannot continue forward rollover warm motion");
    warmToTarget(source, 529);
    const auto epoch = source.epoch();
    std::uint64_t checkedFrames = 0;
    auto traverse = [&](int direction, double goal, std::int64_t displacement) {
        const auto deadline = Clock::now() + 5s;
        for (std::size_t iteration = 0; direction > 0 ? source.position() < goal : source.position() > goal;
             ++iteration) {
            require(iteration < 5000 && Clock::now() < deadline,
                    "Same-epoch resident rollover made no bounded progress");
            displacement += direction * std::int64_t(Source::maxReadFrames);
            require(source.move(displacement, double(Source::maxReadFrames) / Source::sampleRate),
                    "Cannot feed unit-rate platter movement");
            const auto before = source.position();
            const auto value = source.read(Source::maxReadFrames);
            require(!value.failed && source.errorCode() == 0 && source.active() && source.epoch() == epoch &&
                        value.nominalPosition == before + direction * double(value.frames),
                    "Window publication changed epoch/direction or advanced through unavailable PCM");
            bool exact = true;
            for (std::size_t leaf = 0; leaf < 4; ++leaf) {
                require(value.leaves[leaf].size() == value.frames * 2,
                        "Rollover returned incomplete stereo leaf geometry");
                for (std::size_t frame = 0; frame < value.frames; ++frame) {
                    const auto original =
                        direction > 0 ? std::uint64_t(before) + frame : std::uint64_t(before) - frame - 1;
                    for (std::size_t channel = 0; channel < 2; ++channel)
                        exact &= bits(value.leaves[leaf][frame * 2 + channel], rolloverSample(original, leaf, channel));
                }
            }
            require(exact, "Successful triple-buffer reuse changed exact source/stereo/leaf PCM");
            checkedFrames += value.frames;
            if (value.frames < Source::maxReadFrames) {
                require(source.pending(), "Unavailable window suffix was not visibly pending");
                std::this_thread::sleep_for(1ms); // Let a real file worker publish, not a performance gate.
            }
        }
    };
    traverse(1, 400000, 512);
    source.end();
    const auto reverseStart = std::uint64_t(source.position());
    require(source.begin(reverseStart) && source.move(-64, 64.0 / Source::sampleRate),
            "Cannot reverse after repeated window publications");
    warmToTarget(source, double(reverseStart - 64));
    require(source.move(-512, 448.0 / Source::sampleRate), "Cannot continue reverse rollover warm motion");
    warmToTarget(source, double(reverseStart - 512));
    traverse(-1, 50000, -512);
    require(checkedFrames > 700000 && source.epoch() == epoch &&
                source.residentBytes() == 3ULL * 4 * Source::windowFrames * 8,
            "Rollover did not exercise repeated fixed-buffer bidirectional reuse");
}

void fractionalPitchAndAntialias() {
    constexpr std::uint64_t frames = 65536;
    std::vector<float> pcm(frames * 2);
    for (std::uint64_t i = 0; i < frames; ++i) {
        pcm[i * 2] = float(.5 * std::sin(2 * std::numbers::pi * 20000 * double(i) / Source::sampleRate));
        pcm[i * 2 + 1] = float(.25 * std::cos(2 * std::numbers::pi * 1000 * double(i) / Source::sampleRate));
    }
    auto input = file(pcm);
    const std::array<int, 1> fds{fileno(input.get())};
    for (const int direction : {-1, 1}) {
        Source source(fds, frames);
        require(source.prime(32768, 5s) && source.begin(32768) && source.move(direction * 8192, .02),
                "Cannot prepare fast actual-track motion");
        bool atSpeed = false;
        for (std::size_t i = 0; i < 300 && !atSpeed; ++i) {
            const auto before = source.position();
            const auto value = source.read(1);
            require(value.frames == 1 && !value.failed, "Fast scratch lost resident PCM");
            const auto delta = source.position() - before;
            require(std::abs(delta) <= Source::maximumSpeed && delta * direction >= 0,
                    "Velocity cap/direction violated");
            atSpeed = delta == direction * Source::maximumSpeed;
        }
        require(atSpeed, "Slew did not reach capped signed rate");
        const auto before = source.position();
        const auto value = source.read(512);
        require(value.frames == 512 && !value.failed && source.position() == before + direction * 4096,
                "Fast motion stopped/skipped its exact capped trajectory");
        double rejectedEnergy = 0, passEnergy = 0, lowToneError = 0;
        for (std::size_t i = 0; i < value.frames; ++i) {
            const double samplePosition = before + direction * double(i + (direction < 0 ? 1 : 0)) * 8;
            const double expected = .25 * std::cos(2 * std::numbers::pi * 1000 * samplePosition / Source::sampleRate);
            require(std::isfinite(value.leaves[0][i * 2]) && std::isfinite(value.leaves[0][i * 2 + 1]),
                    "Sinc reconstruction returned nonfinite PCM");
            rejectedEnergy += std::pow(value.leaves[0][i * 2], 2);
            passEnergy += std::pow(value.leaves[0][i * 2 + 1], 2);
            lowToneError = std::max(lowToneError, std::abs(value.leaves[0][i * 2 + 1] - expected));
        }
        // Generated spectral screens, not a music/listening quality verdict.
        require(std::sqrt(rejectedEnergy / value.frames) < .002 && std::sqrt(passEnergy / value.frames) > .1 &&
                    lowToneError < .002,
                "Speed-aware sinc failed generated anti-alias/passband pitch screen");
    }
}

void fractionalAlignmentBoundariesAndWatchdog() {
    constexpr std::uint64_t frames = 16384;
    std::array<File, 4> files;
    std::array<int, 4> fds{};
    for (std::size_t leaf = 0; leaf < 4; ++leaf) {
        std::vector<float> pcm(frames * 2);
        for (std::uint64_t i = 0; i < frames; ++i) {
            const float x = float(.2 * std::sin(2 * std::numbers::pi * 440 * double(i) / Source::sampleRate));
            pcm[i * 2] = float(leaf + 1) * x;
            pcm[i * 2 + 1] = leaf == 3 ? 0.F : -float(leaf + 2) * x;
        }
        files[leaf] = file(pcm);
        fds[leaf] = fileno(files[leaf].get());
    }
    Source source(fds, frames);
    require(source.prime(8000, 5s) && source.begin(8000) && source.move(480, .02), "Cannot prepare fractional motion");
    const auto before = source.position();
    const auto value = source.read(512);
    require(value.frames == 512 && source.position() > before && source.position() < before + 256,
            "Fractional motion did not preserve a fractional/slewed cursor");
    for (std::size_t i = 0; i < value.frames; ++i) {
        const double reference = value.leaves[0][i * 2];
        for (std::size_t leaf = 0; leaf < 4; ++leaf) {
            require(std::abs(value.leaves[leaf][i * 2] - (leaf + 1) * reference) < 2e-6,
                    "Fractional interpolation misaligned stem leaves");
            require(std::abs(value.leaves[leaf][i * 2 + 1] - (leaf == 3 ? 0 : -(double(leaf) + 2) * reference)) < 2e-6,
                    "Fractional interpolation contaminated stereo/silent channel");
        }
    }
    std::this_thread::sleep_for(35ms); // Functional stale-input control, not a latency measurement.
    const auto settled = source.read(512);
    require(settled.frames == 512 && !settled.failed, "Watchdog failed resident output");
    const auto stopped = source.position();
    const auto held = source.read(512);
    require(held.frames == 512 && source.position() == stopped &&
                std::ranges::all_of(held.leaves[0], [](float x) { return x == 0.F; }),
            "Stale input extrapolated indefinitely");
    source.end();
    require(source.begin(frames) && source.move(std::numeric_limits<std::int64_t>::min(), .01),
            "Cannot reverse safely from exact EOF/extreme displacement");
    const auto reverse = source.read(512);
    require(reverse.frames == 512 && source.position() < frames && source.position() >= 0 && !reverse.failed,
            "Reverse from EOF wrapped/read outside track");
    source.end();
    require(source.begin(0) && source.move(std::numeric_limits<std::int64_t>::min(), .01), "Cannot clamp at beginning");
    const auto boundary = source.read(512);
    require(boundary.frames == 512 && source.position() == 0 &&
                std::ranges::all_of(boundary.leaves[0], [](float x) { return x == 0.F; }),
            "Negative target crossed frame zero");
    source.end();
    require(source.begin(frames - 64) && source.move(std::numeric_limits<std::int64_t>::max(), .01),
            "Cannot clamp extreme positive displacement");
    const auto end = source.read(1024);
    require(end.frames == 1024 && source.position() == frames && !end.failed &&
                std::ranges::all_of(end.leaves[0], [](float x) { return std::isfinite(x); }),
            "Positive target crossed EOF or read beyond bounded zero extension");
}

void nonfiniteAndMutationFailures() {
    Fixture fixture;
    const auto fds = fixture.fds();
    for (std::size_t leaf = 0; leaf < 4; ++leaf) {
        const float bad = leaf % 2 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
        constexpr off_t offset = 17 * 8;
        require(pwrite(fds[leaf], &bad, sizeof bad, offset) == sizeof bad, "Cannot seed nonfinite leaf");
        {
            Source source(fds, Fixture::frames);
            require(!source.prime(17, 5s) && source.errorCode() == EILSEQ && !source.begin(17),
                    "Any-role nonfinite PCM was published as healthy scratch");
        }
        require(pwrite(fds[leaf], &fixture.pcm[leaf][34], sizeof(float), offset) == sizeof(float),
                "Cannot restore finite fixture");
    }
    Source source(std::span<const int>(fds).first(1), Fixture::frames);
    require(source.prime(17, 5s), "Cannot prepare mutation fixture");
    require(ftruncate(fds[0], 128) == 0, "Cannot truncate generated PCM");
    require(!source.prime(17, 5s) && source.errorCode() == ESTALE && !source.begin(17),
            "Geometry mutation reused stale cache or hid current failure");
}

// One REAL worker pread is held and then fails after a newer epoch is accepted.
// RAII always releases the gate before Source destruction; no blocked worker
// interrupt-latency claim. Wrapping is fixture-only, never part of the helper.
struct GateState {
    struct stat identity{};
    std::uint64_t offset = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool claimed = false, entered = false, released = false;
};
std::mutex gateMutex;
std::shared_ptr<GateState> activeGate;
class Gate {
  public:
    Gate(int fd, std::uint64_t offset) : state_(std::make_shared<GateState>()) {
        require(fstat(fd, &state_->identity) == 0, "Cannot identify gate file");
        state_->offset = offset;
        const std::lock_guard lock(gateMutex);
        require(!activeGate, "Nested fixture gate");
        activeGate = state_;
    }
    ~Gate() { release(); }
    void entered() {
        std::unique_lock lock(state_->mutex);
        require(state_->changed.wait_for(lock, 5s, [&] { return state_->entered; }), "Real worker never entered gate");
    }
    void release() {
        {
            const std::lock_guard lock(state_->mutex);
            state_->released = true;
        }
        state_->changed.notify_all();
        const std::lock_guard lock(gateMutex);
        if (activeGate == state_)
            activeGate.reset();
    }

  private:
    std::shared_ptr<GateState> state_;
};
bool failGatedRead(int fd, std::uint64_t offset) {
    std::shared_ptr<GateState> state;
    {
        const std::lock_guard lock(gateMutex);
        state = activeGate;
    }
    if (!state || offset != state->offset)
        return false;
    struct stat actual{};
    if (fstat(fd, &actual) || actual.st_dev != state->identity.st_dev || actual.st_ino != state->identity.st_ino)
        return false;
    std::unique_lock lock(state->mutex);
    if (state->claimed)
        return false;
    state->claimed = state->entered = true;
    state->changed.notify_all();
    state->changed.wait(lock, [&] { return state->released; });
    return true;
}
void pendingAndStaleCompletion() {
    constexpr std::uint64_t frames = Source::windowFrames * 3;
    File input(std::tmpfile());
    require(input && ftruncate(fileno(input.get()), frames * 8) == 0, "Cannot create sparse window fixture");
    const std::array<float, 2> near{.75F, -.5F}, far{2.F, -3.F};
    require(pwrite(fileno(input.get()), near.data(), 8, 17 * 8) == 8 &&
                pwrite(fileno(input.get()), far.data(), 8, (frames - 8192) * 8) == 8,
            "Cannot seed sparse source markers");
    const std::array<int, 1> fds{fileno(input.get())};
    Source source(fds, frames);
    require(source.prime(17, 5s), "Cannot prime sparse first window");
    const auto epoch = source.epoch();
    Gate gate(fds[0], (frames - Source::windowFrames) * 8);
    require(source.follow(frames - 8192) && source.epoch() == epoch, "Same-epoch follow did not queue far window");
    gate.entered();
    require(source.begin(frames - 8192) && source.pending() && source.move(-480, .01), "Cannot begin pending scratch");
    const auto value = source.read(128);
    require(value.frames == 0 && !value.failed && value.nominalPosition == frames - 8192,
            "Missing window emitted stale PCM or teleported cursor");
    require(source.relocate(17) && source.epoch() > epoch && !source.active() && source.read(1).frames == 0,
            "New epoch did not cancel pending motion");
    gate.release();
    wait(
        [&] {
            (void)source.follow(17);
            return !source.pending() || source.errorCode();
        },
        "Current epoch never replaced obsolete read");
    require(source.errorCode() == 0 && source.position() == 17 && source.begin(17) && source.move(480, .01),
            "Stale EIO completion poisoned current generation");
    const auto current = source.read(64);
    require(current.frames == 64 && !current.failed && current.nominalPosition > 17 && current.nominalPosition < 497,
            "Current epoch did not provide bounded actual PCM after stale completion");
    require(std::ranges::all_of(current.leaves[0], [](float x) { return std::isfinite(x) && std::abs(x) < 1.F; }),
            "Old far-window payload escaped into new epoch");
    source.end();
    source.stop();
    require(source.residentBytes() == 0, "Joined worker retained its three windows");
}
} // namespace

extern "C" ssize_t __real_pread(int, void *, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void *out, size_t size, off_t offset) {
    if (failGatedRead(fd, std::uint64_t(offset))) {
        errno = EIO;
        return -1;
    }
    return __real_pread(fd, out, size, offset);
}
extern "C" ssize_t __real_pread64(int, void *, size_t, off64_t);
extern "C" ssize_t __wrap_pread64(int fd, void *out, size_t size, off64_t offset) {
    if (failGatedRead(fd, std::uint64_t(offset))) {
        errno = EIO;
        return -1;
    }
    return __real_pread64(fd, out, size, offset);
}
extern "C" ssize_t __real___pread_chk(int, void *, size_t, off_t, size_t);
extern "C" ssize_t __wrap___pread_chk(int fd, void *out, size_t size, off_t offset, size_t bound) {
    if (failGatedRead(fd, std::uint64_t(offset))) {
        errno = EIO;
        return -1;
    }
    return __real___pread_chk(fd, out, size, offset, bound);
}
extern "C" ssize_t __real___pread64_chk(int, void *, size_t, off64_t, size_t);
extern "C" ssize_t __wrap___pread64_chk(int fd, void *out, size_t size, off64_t offset, size_t bound) {
    if (failGatedRead(fd, std::uint64_t(offset))) {
        errno = EIO;
        return -1;
    }
    return __real___pread64_chk(fd, out, size, offset, bound);
}

int main() {
    try {
        motionEnvelopeDeclicksStopsAndResumes();
        geometryOwnershipAndControls();
        exactUnitMotionAndStemOrder();
        activeGestureReversalDiscardsOldBacklog();
        endpointOvershootAndSmallReverse();
        sameEpochBidirectionalWindowRollover();
        fractionalPitchAndAntialias();
        fractionalAlignmentBoundariesAndWatchdog();
        nonfiniteAndMutationFailures();
        pendingAndStaleCompletion();
        std::cout << "prepared scratch: 10 generated CPU cases, " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "prepared scratch fixture failed: " << error.what() << '\n';
        return 1;
    }
}

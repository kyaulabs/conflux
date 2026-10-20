// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioFile.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using conflux::audio::measurePreparedLevels;
using conflux::audio::PreparedAudio;
using conflux::audio::SignalLevels;

void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) { require(value >= 0, "Fixture descriptor failed"); }
    ~Descriptor() { if (value_ >= 0) close(value_); }
    int get() const { return value_; }
    int release() { return std::exchange(value_, -1); }
private:
    int value_;
};
struct Fixture {
    int writer;
    PreparedAudio audio;
    Fixture(int fd, PreparedAudio prepared) : writer(fd), audio(std::move(prepared)) {}
    ~Fixture() { close(writer); }
};

Fixture fixture(std::span<const float> samples)
{
    require(!samples.empty() && samples.size() % 2 == 0, "Invalid stereo fixture");
    std::string name = "/tmp/conflux-levels-XXXXXX";
    Descriptor writer(mkostemp(name.data(), O_CLOEXEC));
    Descriptor reader(open(name.c_str(), O_RDONLY | O_CLOEXEC));
    require(unlink(name.c_str()) == 0, "Cannot unlink private level fixture");
    std::size_t position = 0;
    while (position < samples.size_bytes()) {
        const auto written = write(writer.get(), reinterpret_cast<const char*>(samples.data()) + position,
                                   samples.size_bytes() - position);
        require(written > 0, "Cannot write level fixture");
        position += std::size_t(written);
    }
    const auto frames = samples.size() / 2;
    std::vector<float> overview(std::min<std::size_t>(512, frames), 0.F);
    return Fixture(writer.release(), conflux::audio::reopenPreparedAudio(reader.get(), frames, overview));
}

void near(double actual, double expected, const char* message)
{ require(std::isfinite(actual) && std::abs(actual - expected) <= std::max(1e-12, std::abs(expected) * 1e-12), message); }

template<class Function> void rejects(Function&& function, const char* reason)
{
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(reason) != std::string::npos, "Unexpected level rejection reason");
        return;
    }
    throw std::runtime_error("Invalid or cancelled measurement was accepted");
}

// Linker interposition affects only this executable. It supplies deterministic
// partial reads, cancellation and file changes without production test hooks,
// races, sleeps, hardware, or a large fixture. Other descriptors pass through.
enum class Fault { None, InterruptedShortReads, ReadError, Truncate, Change, Ctime, CancelRead, CancelReturn };
struct Injection {
    int descriptor = -1, writer = -1;
    Fault fault = Fault::None;
    unsigned reads = 0, stats = 0;
    std::stop_source* stop = nullptr;
} injection;
struct ScopedInjection {
    ScopedInjection(Fixture& value, Fault fault, std::stop_source* stop = nullptr)
    { injection = {value.audio.descriptor(), value.writer, fault, 0, 0, stop}; }
    ~ScopedInjection() { injection = {}; }
};

void stereoArithmetic()
{
    const std::array<float, 4> asymmetric{.25F, 0.F, .75F, -.5F};
    auto pcm = fixture(asymmetric);
    auto levels = measurePreparedLevels(pcm.audio);
    near(levels.rms, std::sqrt(.875 / 4), "Asymmetric stereo RMS differs");
    near(levels.samplePeak, .75, "Asymmetric sample peak differs");
    const std::array<float, 4> antiPhase{.5F, -.5F, .5F, -.5F};
    auto anti = fixture(antiPhase);
    levels = measurePreparedLevels(anti.audio);
    near(levels.rms, .5, "Anti-phase channels were summed before RMS");
    near(levels.samplePeak, .5, "Anti-phase peak differs");
    const std::array<float, 2> silence{};
    auto zero = fixture(silence);
    require(measurePreparedLevels(zero.audio) == SignalLevels{}, "One-frame silence is not valid zero");
    const std::array<float, 2> headroom{2.F, -4.F};
    auto loud = fixture(headroom);
    levels = measurePreparedLevels(loud.audio);
    near(levels.rms, std::sqrt(10.), "Float headroom was clipped");
    near(levels.samplePeak, 4., "Float headroom peak was clipped");
    const std::array<float, 2> maximum{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    auto huge = fixture(maximum);
    levels = measurePreparedLevels(huge.audio);
    near(levels.rms, double(maximum[0]), "RMS overflowed finite float input");
    near(levels.samplePeak, double(maximum[0]), "Finite maximum peak differs");
    const std::array<float, 2> minimum{std::numeric_limits<float>::denorm_min(), 0.F};
    auto tiny = fixture(minimum);
    levels = measurePreparedLevels(tiny.audio);
    require(levels.rms > 0 && levels.rms <= levels.samplePeak && levels.samplePeak == double(minimum[0]),
            "Positive subnormal signal was reported as silence");
}

void fullSourceAndBlockBoundaries()
{
    constexpr std::size_t frames = 8195;
    std::vector<float> samples(frames * 2, .25F);
    auto dc = fixture(samples);
    near(measurePreparedLevels(dc.audio).rms, .25, "Whole-source DC RMS differs");
    std::fill(samples.begin(), samples.end(), 0.F);
    samples[4095 * 2] = 1.F; samples[4096 * 2 + 1] = -2.F; samples.back() = .5F;
    auto impulse = fixture(samples);
    const auto levels = measurePreparedLevels(impulse.audio);
    near(levels.rms, std::sqrt(5.25 / double(frames * 2)), "Boundary or final partial block was omitted");
    near(levels.samplePeak, 2., "Block-boundary impulse peak differs");
    constexpr std::array<float, 4> cycle{0.F, .5F, 0.F, -.5F};
    samples.resize(4800 * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = cycle[(i / 2) % cycle.size()];
    auto sine = fixture(samples);
    near(measurePreparedLevels(sine.audio).rms, std::sqrt(.125), "Exact sampled sine RMS differs");
}

void variableTempoEnergy()
{
    // Two sections change pulse spacing and width. The independent energy
    // oracle counts known pulse samples; it makes no beat or tempo claim.
    constexpr std::size_t frames = 32771, pulsesA = 20, pulsesB = 30;
    std::vector<float> samples(frames * 2, 0.F);
    for (std::size_t pulse = 0; pulse < pulsesA; ++pulse)
        for (std::size_t width = 0; width < 16; ++width) samples[(pulse * 512 + width) * 2] = .5F;
    for (std::size_t pulse = 0; pulse < pulsesB; ++pulse)
        for (std::size_t width = 0; width < 8; ++width) samples[(16384 + pulse * 320 + width) * 2 + 1] = -.25F;
    auto pcm = fixture(samples);
    const auto levels = measurePreparedLevels(pcm.audio);
    near(levels.rms, std::sqrt((pulsesA * 16 * .25 + pulsesB * 8 * .0625) / (frames * 2)),
         "Changing-tempo sections were not both measured");
    near(levels.samplePeak, .5, "Variable-tempo fixture sample peak differs");
}

void readOnlyAndPartialReads()
{
    std::vector<float> samples(9001 * 2, .125F);
    samples.front() = -0.F;
    samples.back() = -.75F;
    auto pcm = fixture(samples);
    constexpr off_t initialOffset = 19;
    require(lseek(pcm.audio.descriptor(), initialOffset, SEEK_SET) == initialOffset, "Cannot set fixture offset");
    SignalLevels levels;
    {
        ScopedInjection fault(pcm, Fault::InterruptedShortReads);
        levels = measurePreparedLevels(pcm.audio);
        require(injection.reads > 2, "Fixture did not exercise interrupted and short reads");
    }
    near(levels.rms, std::sqrt((double(samples.size() - 2) / 64. + .5625) / samples.size()), "Partial reads altered RMS");
    near(levels.samplePeak, .75, "Partial reads altered peak");
    require(lseek(pcm.audio.descriptor(), 0, SEEK_CUR) == initialOffset, "Measurement changed descriptor offset");
    std::vector<float> after(samples.size());
    require(pread(pcm.audio.descriptor(), after.data(), after.size() * sizeof(float), 0) == ssize_t(after.size() * sizeof(float)),
            "Cannot verify preserved PCM");
    require(std::memcmp(after.data(), samples.data(), samples.size() * sizeof(float)) == 0,
            "Measurement changed PCM bytes");
}

void failuresAndCancellation()
{
    const std::vector<float> samples(8193 * 2, .25F);
    for (const auto fault : {Fault::ReadError, Fault::Truncate, Fault::Change, Fault::Ctime}) {
        auto pcm = fixture(samples);
        ScopedInjection injected(pcm, fault);
        rejects([&] { (void)measurePreparedLevels(pcm.audio); }, fault == Fault::ReadError || fault == Fault::Truncate ? "read" : "changed");
    }
    for (const auto fault : {Fault::CancelRead, Fault::CancelReturn}) {
        auto pcm = fixture(samples);
        std::stop_source stop;
        ScopedInjection injected(pcm, fault, &stop);
        rejects([&] { (void)measurePreparedLevels(pcm.audio, stop.get_token()); }, "cancelled");
    }
    auto pcm = fixture(samples);
    std::stop_source stop; stop.request_stop();
    rejects([&] { (void)measurePreparedLevels(pcm.audio, stop.get_token()); }, "cancelled");
    require(ftruncate(pcm.writer, samples.size() * sizeof(float) - 1) == 0, "Cannot truncate fixture");
    rejects([&] { (void)measurePreparedLevels(pcm.audio); }, "geometry");
    for (const auto bad : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::quiet_NaN()}) {
        auto corrupt = fixture(samples);
        require(pwrite(corrupt.writer, &bad, sizeof(bad), off_t((samples.size() - 1) * sizeof(float))) == sizeof(bad),
                "Cannot inject nonfinite sample");
        rejects([&] { (void)measurePreparedLevels(corrupt.audio); }, "non-finite");
    }
    auto moved = fixture(samples);
    auto owner = std::move(moved.audio);
    rejects([&] { (void)measurePreparedLevels(moved.audio); }, "regular");
    auto wrong = fixture(samples);
    Descriptor directory(open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    require(dup2(directory.get(), wrong.audio.descriptor()) == wrong.audio.descriptor(), "Cannot substitute descriptor type");
    rejects([&] { (void)measurePreparedLevels(wrong.audio); }, "regular");
}
} // namespace

extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" int __real_fstat(int, struct stat*);
extern "C" ssize_t __wrap_pread(int fd, void* data, size_t count, off_t offset)
{
    if (fd != injection.descriptor) return __real_pread(fd, data, count, offset);
    ++injection.reads;
    if (injection.fault == Fault::ReadError) { errno = EIO; return -1; }
    if (injection.fault == Fault::InterruptedShortReads) {
        if (injection.reads == 1) { errno = EINTR; return -1; }
        count = std::min<std::size_t>(count, 113);
    }
    const auto result = __real_pread(fd, data, count, offset);
    if (result > 0 && injection.reads == 1) {
        if (injection.fault == Fault::Truncate) {
            if (ftruncate(injection.writer, 0)) std::abort();
        } else if (injection.fault == Fault::Change) {
            struct stat before{};
            if (__real_fstat(fd, &before)) std::abort();
            const float replacement = .75F;
            const timespec times[2]{{0, UTIME_OMIT}, {before.st_mtim.tv_sec + 1, before.st_mtim.tv_nsec}};
            if (pwrite(injection.writer, &replacement, sizeof(replacement), 0) != sizeof(replacement) ||
                futimens(injection.writer, times)) std::abort();
        } else if (injection.fault == Fault::CancelRead) injection.stop->request_stop();
    }
    return result;
}
extern "C" int __wrap_fstat(int fd, struct stat* result)
{
    const auto status = __real_fstat(fd, result);
    if (status == 0 && fd == injection.descriptor && ++injection.stats == 2) {
        if (injection.fault == Fault::Ctime) result->st_ctim.tv_sec += 1;
        if (injection.fault == Fault::CancelReturn) injection.stop->request_stop();
    }
    return status;
}

int main()
{
    try {
        stereoArithmetic();
        fullSourceAndBlockBoundaries();
        variableTempoEnergy();
        readOnlyAndPartialReads();
        failuresAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Prepared source level arithmetic, failures and cancellation passed\n";
    return 0;
}

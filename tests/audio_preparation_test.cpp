// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioFile.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using conflux::audio::PreparedAudio;
using conflux::audio::prepareAudioFile;

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "conflux-preparation-test-XXXXXX").string();
        if (const auto* created = mkdtemp(pattern.data()))
            path = created;
        else
            throw std::runtime_error("Cannot create preparation test directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

template<typename Function>
bool rejects(Function&& function, const char* message, const char* reason = "")
{
    try {
        function();
    } catch (const std::exception& error) {
        return require(std::string(error.what()).find(reason) != std::string::npos,
                       "Preparation failed for an unexpected reason");
    }
    return require(false, message);
}

void append16(std::vector<char>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<char>(value & 255));
    bytes.push_back(static_cast<char>(value >> 8));
}

void append32(std::vector<char>& bytes, std::uint32_t value)
{
    append16(bytes, static_cast<std::uint16_t>(value));
    append16(bytes, static_cast<std::uint16_t>(value >> 16));
}

// Independent IEEE-float WAV writer. Large fixtures reserve sparse data rather
// than allocating the source or expected full PCM in the test process.
void floatWave(const std::filesystem::path& path, std::uint16_t channels, std::uint32_t rate,
               std::uint64_t frames)
{
    const auto payload = frames * channels * sizeof(float);
    if (payload > UINT32_MAX - 36)
        throw std::runtime_error("Test WAV is too large");
    std::vector<char> header;
    auto tag = [&](const char* text) { header.insert(header.end(), text, text + 4); };
    tag("RIFF"); append32(header, static_cast<std::uint32_t>(36 + payload));
    tag("WAVE"); tag("fmt "); append32(header, 16);
    append16(header, 3); append16(header, channels); append32(header, rate);
    append32(header, rate * channels * sizeof(float));
    append16(header, channels * sizeof(float)); append16(header, 32);
    tag("data"); append32(header, static_cast<std::uint32_t>(payload));
    std::ofstream file(path, std::ios::binary);
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write test WAV header");
    std::filesystem::resize_file(path, header.size() + payload);
}

void setSample(const std::filesystem::path& path, std::uint64_t sample, float value)
{
    std::vector<char> bytes;
    append32(bytes, std::bit_cast<std::uint32_t>(value));
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(44 + sample * sizeof(float)));
    file.write(bytes.data(), bytes.size());
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write test WAV marker");
}

void readAt(int descriptor, std::uint64_t frame, std::span<float> samples)
{
    auto* bytes = reinterpret_cast<char*>(samples.data());
    std::size_t received = 0;
    while (received < samples.size_bytes()) {
        const auto count = pread(descriptor, bytes + received, samples.size_bytes() - received,
                                 static_cast<off_t>(frame * 2 * sizeof(float) + received));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("Prepared descriptor ended before its advertised frames");
        received += static_cast<std::size_t>(count);
    }
}

bool closed(int descriptor)
{
    errno = 0;
    return fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
}

std::size_t descriptorCount()
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
        ++count;
    return count;
}

bool descriptorShape(const PreparedAudio& audio)
{
    struct stat info{};
    return require(audio.descriptor() >= 0 && fstat(audio.descriptor(), &info) == 0 && S_ISREG(info.st_mode) &&
                       info.st_nlink == 0 && static_cast<std::uint64_t>(info.st_size) == audio.frames() * 8 &&
                       (fcntl(audio.descriptor(), F_GETFD) & FD_CLOEXEC),
                   "Prepared PCM is not an anonymous, close-on-exec regular file of the advertised size");
}

struct Marker { std::uint64_t frame; float left, right; };
constexpr std::uint64_t longFrames = 200ULL * 48000;
constexpr std::array<Marker, 7> markers{{
    {0, 1.5F, -1.25F}, {1, -0.0F, .0625F}, {4095, .03125F, -.09375F},
    {longFrames / 512, .75F, -.5F}, {longFrames / 2, .0005F, -.00025F},
    {longFrames * 3 / 4, -.25F, .125F}, {longFrames - 1, .625F, -.375F}
}};

bool longStereoBypass(const std::filesystem::path& path)
{
    floatWave(path, 2, 48000, longFrames);
    for (const auto& marker : markers) {
        setSample(path, marker.frame * 2, marker.left);
        setSample(path, marker.frame * 2 + 1, marker.right);
    }
    auto prepared = prepareAudioFile(path);
    bool passed = require(prepared.frames() == longFrames && longFrames * 8 > 64ULL * 1024 * 1024,
                          "Long preparation did not exceed the previous whole-track memory limit");
    passed &= descriptorShape(prepared);
    std::array<float, 4096 * 2> block{};
    std::size_t markerIndex = 0;
    bool identical = true;
    for (std::uint64_t first = 0; first < prepared.frames();) {
        const auto count = std::min<std::uint64_t>(4096, prepared.frames() - first);
        readAt(prepared.descriptor(), first, std::span(block).first(count * 2));
        for (std::uint64_t frame = 0; frame < count; ++frame) {
            float left = 0, right = 0;
            if (markerIndex < markers.size() && markers[markerIndex].frame == first + frame) {
                left = markers[markerIndex].left;
                right = markers[markerIndex].right;
                ++markerIndex;
            }
            identical &= std::bit_cast<std::uint32_t>(block[frame * 2]) == std::bit_cast<std::uint32_t>(left) &&
                         std::bit_cast<std::uint32_t>(block[frame * 2 + 1]) == std::bit_cast<std::uint32_t>(right);
        }
        first += count;
    }
    passed &= require(identical && markerIndex == markers.size(),
                      "Chunked 48 kHz readback changed stereo order, bypass sample bits, signed zero or headroom");
    std::array<float, 512> expectedPeaks{};
    // The 200-second fixture has exactly 18,750 source frames per bucket.
    expectedPeaks[0] = 1.F;
    expectedPeaks[1] = .5F;
    expectedPeaks[256] = .0005F / 1.5F;
    expectedPeaks[384] = .25F / 1.5F;
    expectedPeaks[511] = .625F / 1.5F;
    passed &= require(prepared.peaks().size() == expectedPeaks.size() &&
                          std::equal(prepared.peaks().begin(), prepared.peaks().end(), expectedPeaks.begin()),
                      "Long source overview has incorrect bucket placement or normalization");
    return passed;
}

bool monoConversion(const std::filesystem::path& directory)
{
    const auto path = directory / "mono-44100.wav";
    constexpr std::size_t inputFrames = 44100;
    constexpr double amplitude = .25, frequency = 375;
    floatWave(path, 1, 44100, inputFrames);
    // Only one bounded source block exists at a time, independently encoded.
    std::fstream source(path, std::ios::binary | std::ios::in | std::ios::out);
    source.seekp(44);
    for (std::size_t first = 0; first < inputFrames;) {
        const auto count = std::min<std::size_t>(4096, inputFrames - first);
        std::vector<char> bytes;
        bytes.reserve(count * sizeof(float));
        for (std::size_t index = 0; index < count; ++index) {
            const auto value = static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * frequency * (first + index) / 44100.0));
            append32(bytes, std::bit_cast<std::uint32_t>(value));
        }
        source.write(bytes.data(), bytes.size());
        first += count;
    }
    source.close();
    if (!source)
        throw std::runtime_error("Cannot write mono conversion fixture");
    auto prepared = prepareAudioFile(path, 48000 * 8);
    bool passed = require(prepared.frames() >= 47999 && prepared.frames() <= 48000,
                          "Mono conversion changed the expected one-second duration");
    passed &= descriptorShape(prepared);
    std::array<float, 4096 * 2> block{};
    bool finiteStereo = true;
    double maximumError = 0;
    for (std::uint64_t first = 0; first < prepared.frames();) {
        const auto count = std::min<std::uint64_t>(4096, prepared.frames() - first);
        readAt(prepared.descriptor(), first, std::span(block).first(count * 2));
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto frame = first + index;
            const auto left = block[index * 2], right = block[index * 2 + 1];
            finiteStereo &= std::isfinite(left) && std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
            if (frame >= 1536 && frame + 1536 < prepared.frames()) {
                const auto expected = amplitude * std::sin(2 * std::numbers::pi * frequency * frame / 48000.0);
                maximumError = std::max(maximumError, std::abs(left - expected));
            }
        }
        first += count;
    }
    passed &= require(finiteStereo && maximumError < 1e-4,
                      "Mono conversion duplicated incorrectly, shifted timing or changed the independent sine");
    // Independently reduce complete bucket intervals from the anonymous output,
    // rather than duplicating the production frame-to-bucket indexing loop.
    std::array<float, 512> bucketPeaks{};
    float maximum = 0;
    passed &= require(prepared.peaks().size() == bucketPeaks.size(), "Converted overview lacks its bounded 512 buckets");
    for (std::size_t bucket = 0; bucket < bucketPeaks.size(); ++bucket) {
        const auto first = (bucket * prepared.frames() + 511) / 512;
        const auto end = ((bucket + 1) * prepared.frames() + 511) / 512;
        readAt(prepared.descriptor(), first, std::span(block).first((end - first) * 2));
        for (std::size_t sample = 0; sample < (end - first) * 2; ++sample)
            bucketPeaks[bucket] = std::max(bucketPeaks[bucket], std::abs(block[sample]));
        maximum = std::max(maximum, bucketPeaks[bucket]);
    }
    bool peaksMatch = maximum > .24F && maximum < .26F;
    for (std::size_t bucket = 0; bucket < std::min(prepared.peaks().size(), bucketPeaks.size()); ++bucket)
        peaksMatch &= std::abs(prepared.peaks()[bucket] - bucketPeaks[bucket] / maximum) < 1e-7;
    passed &= require(peaksMatch, "Converted overview did not normalize the actual stereo source peaks");
    passed &= rejects([&] { prepareAudioFile(path, 48000 * 8 - 1); },
                      "Input byte size bypassed the normalized stereo output budget", "budget");

    const auto impulse = directory / "mono-impulse.wav";
    floatWave(impulse, 1, 44100, 11025);
    setSample(impulse, 4410, .5F);
    auto aligned = prepareAudioFile(impulse);
    std::uint64_t peakFrame = 0;
    float peak = 0;
    for (std::uint64_t first = 0; first < aligned.frames();) {
        const auto count = std::min<std::uint64_t>(4096, aligned.frames() - first);
        readAt(aligned.descriptor(), first, std::span(block).first(count * 2));
        for (std::uint64_t index = 0; index < count; ++index)
            if (std::abs(block[index * 2]) > peak) {
                peak = std::abs(block[index * 2]);
                peakFrame = first + index;
            }
        first += count;
    }
    passed &= require(peak > .1F && peakFrame >= 4799 && peakFrame <= 4801,
                      "Converted impulse is not aligned to its independent 0.1-second source time");
    return passed;
}

bool ownershipAndFailures(const std::filesystem::path& directory)
{
    const auto path = directory / "small.wav";
    floatWave(path, 2, 48000, 4);
    setSample(path, 0, .125F);
    int finalDescriptor = -1;
    bool passed = true;
    {
        auto first = prepareAudioFile(path, 32);
        passed &= require(first.frames() == 4 && first.peaks().size() == 4,
                          "Small exact-budget preparation failed or expanded its overview");
        const auto firstDescriptor = first.descriptor();
        PreparedAudio moved(std::move(first));
        passed &= require(first.descriptor() == -1 && moved.descriptor() == firstDescriptor,
                          "Prepared move construction did not transfer descriptor ownership");
        auto replacement = prepareAudioFile(path, 32);
        finalDescriptor = replacement.descriptor();
        moved = std::move(replacement);
        passed &= require(replacement.descriptor() == -1 && moved.descriptor() == finalDescriptor && closed(firstDescriptor),
                          "Prepared move assignment leaked or retained the replaced descriptor");
        passed &= descriptorShape(moved);
    }
    passed &= require(closed(finalDescriptor), "Prepared destruction leaked its anonymous descriptor");
    const auto before = descriptorCount();
    passed &= rejects([&] { prepareAudioFile(path, 31); }, "Short prepared budget was accepted", "budget");
    passed &= rejects([&] { prepareAudioFile(path, 7); }, "Sub-frame prepared budget was accepted", "budget");
    passed &= rejects([&] { prepareAudioFile(directory / "missing.wav"); }, "Missing preparation input was accepted");
    passed &= rejects([&] { prepareAudioFile(directory); }, "Directory preparation input was accepted");
    const auto corrupt = directory / "corrupt.wav";
    { std::ofstream file(corrupt); file << "RIFF"; }
    passed &= rejects([&] { prepareAudioFile(corrupt); }, "Malformed preparation input was accepted");
    const auto invalid = directory / "nonfinite.wav";
    floatWave(invalid, 1, 44100, 8193);
    setSample(invalid, 8192, std::numeric_limits<float>::quiet_NaN());
    passed &= rejects([&] { prepareAudioFile(invalid); }, "Non-finite late conversion input was accepted", "non-finite");
    std::stop_source cancellation;
    cancellation.request_stop();
    passed &= rejects([&] { prepareAudioFile(path, PreparedAudio::defaultMaxPreparedBytes, cancellation.get_token()); },
                      "Cancelled preparation returned PCM", "cancel");
    passed &= require(descriptorCount() == before, "Rejected or cancelled preparation leaked a descriptor");
    return passed;
}

bool actualDiskFailure(const std::filesystem::path& input)
{
    const auto child = fork();
    if (child < 0)
        throw std::runtime_error("Cannot fork preparation write-failure test");
    if (!child) {
        try {
            std::signal(SIGXFSZ, SIG_IGN);
            struct rlimit limit{};
            if (getrlimit(RLIMIT_FSIZE, &limit) || limit.rlim_max < 4096)
                _exit(2);
            limit.rlim_cur = 4096;
            if (setrlimit(RLIMIT_FSIZE, &limit))
                _exit(3);
            const auto before = descriptorCount();
            const bool rejected = rejects([&] { prepareAudioFile(input); },
                                          "Prepared PCM ignored a real disk write failure", "write prepared PCM");
            _exit(rejected && descriptorCount() == before ? 0 : 4);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            _exit(5);
        }
    }
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    return require(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "Isolated preparation RLIMIT_FSIZE disk-error test failed");
}
} // namespace

int main()
{
    try {
        TemporaryDirectory directory;
        const auto longPath = directory.path / "sparse-200-seconds.wav";
        const bool bypass = longStereoBypass(longPath);
        const bool conversion = monoConversion(directory.path);
        const bool ownership = ownershipAndFailures(directory.path);
        const bool diskFailure = actualDiskFailure(longPath);
        return bypass && conversion && ownership && diskFailure ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

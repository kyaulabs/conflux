// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioFile.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
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
#include <thread>
#include <type_traits>
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
using conflux::audio::PreparedStemInput;
using conflux::audio::prepareStemInput;
using conflux::audio::prepareStemOutput;
static_assert(!std::is_copy_constructible_v<PreparedStemInput> &&
              !std::is_copy_assignable_v<PreparedStemInput> &&
              std::is_nothrow_move_constructible_v<PreparedStemInput> &&
              std::is_nothrow_move_assignable_v<PreparedStemInput>);

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

class InputFD {
public:
    explicit InputFD(const std::filesystem::path& path, int flags = O_RDONLY | O_CLOEXEC)
        : value(open(path.c_str(), flags))
    { if (value < 0) throw std::runtime_error("Cannot open conversion fixture"); }
    ~InputFD() { close(value); }
    InputFD(const InputFD&) = delete;
    InputFD& operator=(const InputFD&) = delete;
    int value;
};

void rawFloatFile(const std::filesystem::path& path, std::span<const float> samples)
{
    std::ofstream file(path, std::ios::binary);
    std::vector<char> bytes;
    bytes.reserve(samples.size_bytes());
    for (const auto sample : samples) append32(bytes, std::bit_cast<std::uint32_t>(sample));
    file.write(bytes.data(), bytes.size());
    file.close();
    if (!file) throw std::runtime_error("Cannot write raw F32LE fixture");
}

// Decode wire bytes independently of host endianness or libsndfile.
void readLittleEndian(int fd, std::span<float> samples)
{
    std::vector<unsigned char> bytes(samples.size_bytes());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto got = pread(fd, bytes.data() + offset, bytes.size() - offset, off_t(offset));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) throw std::runtime_error("Raw F32LE fixture ended unexpectedly");
        offset += std::size_t(got);
    }
    for (std::size_t sample = 0; sample < samples.size(); ++sample) {
        std::uint32_t bits = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            bits |= std::uint32_t(bytes[sample * 4 + byte]) << (byte * 8);
        samples[sample] = std::bit_cast<float>(bits);
    }
}

bool stemInputExactAndOwnership(const std::filesystem::path& directory)
{
    const auto source = directory / "stem-input-44100.wav";
    const auto spool = directory / "stem-input-spool";
    constexpr std::array<float, 12> values{1.5F, -1.25F, -0.F, .0625F,
        .000003814697265625F, 2.25F, -.75F, .125F, .5F, -.25F, 1.75F, -2.F};
    floatWave(source, 2, 44100, values.size() / 2);
    for (std::size_t sample = 0; sample < values.size(); ++sample) setSample(source, sample, values[sample]);
    InputFD input(source);
    bool passed = true;
    int finalFD = -1;
    {
        auto first = prepareStemInput(input.value, values.size() * sizeof(float), {}, spool);
        struct stat status{};
        passed &= require(PreparedStemInput::sampleRate == 44100 && PreparedStemInput::channels == 2 &&
            first.frames() == values.size() / 2 && fstat(first.descriptor(), &status) == 0 &&
            S_ISREG(status.st_mode) && status.st_nlink == 0 && status.st_size == off_t(values.size() * sizeof(float)) &&
            (fcntl(first.descriptor(), F_GETFD) & FD_CLOEXEC) && std::filesystem::is_empty(spool),
            "Stem input is not exact anonymous stereo44100 F32LE");
        std::array<float, values.size()> observed{};
        readLittleEndian(first.descriptor(), observed);
        passed &= require(std::equal(values.begin(), values.end(), observed.begin(), [](float a, float b) {
            return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
        }), "44100Hz direct input lost headroom, channel order, signed zero or exactfloat bits");
        const auto oldFD = first.descriptor();
        PreparedStemInput moved(std::move(first));
        passed &= require(first.descriptor() == -1 && moved.descriptor() == oldFD,
                          "Stem input move construction retained duplicate ownership");
        auto second = prepareStemInput(input.value);
        finalFD = second.descriptor();
        moved = std::move(second);
        passed &= require(second.descriptor() == -1 && moved.descriptor() == finalFD && closed(oldFD),
                          "Stem input move assignment leaked its previous descriptor");
    }
    passed &= require(closed(finalFD) && fcntl(input.value, F_GETFD) >= 0,
                      "Stem input destruction leaked its FD or closed the borrowed original");
    const auto mono = directory / "stem-mono-44100.wav";
    floatWave(mono, 1, 44100, 3);
    setSample(mono, 0, 1.75F); setSample(mono, 1, -.125F); setSample(mono, 2, .000003814697265625F);
    InputFD monoFD(mono);
    auto duplicated = prepareStemInput(monoFD.value, 24);
    std::array<float, 6> stereo{};
    readLittleEndian(duplicated.descriptor(), stereo);
    passed &= require(duplicated.frames() == 3 && stereo == std::array<float, 6>{1.75F, 1.75F, -.125F, -.125F,
        .000003814697265625F, .000003814697265625F}, "Mono separator input did not map to identical stereo44100 samples");
    return passed;
}

bool stemInputRateAndBudget(const std::filesystem::path& directory)
{
    const auto source = directory / "stem-input-48000.wav";
    floatWave(source, 2, 48000, 48000);
    std::fstream file(source, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(44);
    for (std::size_t first = 0; first < 48000;) {
        const auto count = std::min<std::size_t>(4096, 48000 - first);
        std::vector<char> bytes;
        for (std::size_t index = 0; index < count; ++index) {
            const double phase = 2 * std::numbers::pi * 375 * (first + index) / 48000.0;
            append32(bytes, std::bit_cast<std::uint32_t>(float(1.5 * std::sin(phase))));
            append32(bytes, std::bit_cast<std::uint32_t>(float(-.75 * std::cos(phase))));
        }
        file.write(bytes.data(), bytes.size()); first += count;
    }
    file.close();
    if (!file) throw std::runtime_error("Cannot write48000Hz separator input fixture");
    InputFD input(source);
    // The existing sinc converter may emit one frame beyond the rational
    // duration because of its floating-point phase accumulator.
    auto converted = prepareStemInput(input.value, 44101 * 8);
    std::vector<float> samples(converted.frames() * 2);
    readLittleEndian(converted.descriptor(), samples);
    bool passed = require(converted.frames() >= 44099 && converted.frames() <= 44101,
                          "48000->44100 separator input changed its expected duration");
    double error = 0;
    float peak = 0;
    for (std::size_t frame = 1536; frame + 1536 < converted.frames(); ++frame) {
        const double phase = 2 * std::numbers::pi * 375 * frame / 44100.0;
        error = std::max({error, std::abs(samples[frame * 2] - 1.5 * std::sin(phase)),
                         std::abs(samples[frame * 2 + 1] + .75 * std::cos(phase))});
        peak = std::max(peak, std::abs(samples[frame * 2]));
    }
    passed &= require(error < 3e-4 && peak > 1.49F, "Separator input conversion shifted channels/time or clipped headroom");
    passed &= rejects([&] { prepareStemInput(input.value, 44100 * 8 - 1); },
                      "Separator-rate normalized byte budget was ignored", "budget");
    return passed;
}

bool fourRawOutputsAlign(const std::filesystem::path& directory)
{
    constexpr std::uint64_t frames = 11025, impulse = 4410;
    std::uint64_t commonFrames = 0, commonPeak = 0;
    bool passed = true;
    for (std::size_t stem = 0; stem < 4; ++stem) {
        std::vector<float> raw(frames * 2);
        const float amplitude = float(stem + 1) * .5F;
        raw[impulse * 2] = amplitude;
        raw[impulse * 2 + 1] = -amplitude * .5F;
        const auto path = directory / ("stem-output-" + std::to_string(stem) + ".f32");
        rawFloatFile(path, raw);
        InputFD input(path);
        auto output = prepareStemOutput(input.value, frames, 12001 * 8);
        passed &= descriptorShape(output);
        std::vector<float> pcm(output.frames() * 2);
        readAt(output.descriptor(), 0, pcm);
        float peak = 0;
        std::uint64_t peakFrame = 0;
        bool finiteStereo = true;
        for (std::uint64_t frame = 0; frame < output.frames(); ++frame) {
            finiteStereo &= std::isfinite(pcm[frame * 2]) && std::isfinite(pcm[frame * 2 + 1]) &&
                            std::abs(pcm[frame * 2 + 1] + pcm[frame * 2] * .5F) < 3e-6;
            if (std::abs(pcm[frame * 2]) > peak) { peak = std::abs(pcm[frame * 2]); peakFrame = frame; }
        }
        passed &= require(finiteStereo, "Raw stereo stem output became nonfinite or changed channel mapping");
        if (!stem) { commonFrames = output.frames(); commonPeak = peakFrame; }
        passed &= require(output.frames() == commonFrames && output.frames() >= 11999 && output.frames() <= 12001 &&
                          peakFrame == commonPeak && peakFrame >= 4799 && peakFrame <= 4801 && peak > amplitude * .7F,
                          "Four44100 outputs did not share one aligned48000 frame mapping");
        passed &= require(output.peaks().size() == 512 &&
            *std::max_element(output.peaks().begin(), output.peaks().end()) == 1.F &&
            std::ranges::all_of(output.peaks(), [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }),
            "Converted raw output did not carry the normal normalized overview");
        if (stem == 3) passed &= require(peak > 1.F, "Raw stem output lost float headroom");
        passed &= rejects([&] { prepareStemOutput(input.value, frames, 12000 * 8 - 1); },
                          "Raw output bypassed normalized48000 byte budget", "budget");
    }
    return passed;
}

bool rawStemFailures(const std::filesystem::path& directory)
{
    const auto rawPath = directory / "stem-failures.f32";
    std::vector<float> values(8193 * 2, .125F);
    rawFloatFile(rawPath, values);
    const auto sourcePath = directory / "stem-source-failures.wav";
    floatWave(sourcePath, 2, 44100, 8193);
    InputFD raw(rawPath), source(sourcePath);
    const auto before = descriptorCount();
    bool passed = rejects([&] { prepareStemOutput(raw.value, 8192); }, "Wrong raw declared frame count accepted", "geometry");
    passed &= rejects([&] { prepareStemOutput(raw.value, 0); }, "Zero raw declared frames accepted", "geometry");
    passed &= rejects([&] { prepareStemOutput(raw.value, std::numeric_limits<std::uint64_t>::max()); },
                      "Overflowing raw geometry accepted", "geometry");
    passed &= rejects([&] { prepareStemInput(source.value, 7); }, "Sub-frame separator input budget accepted", "budget");
    passed &= rejects([&] { prepareStemOutput(raw.value, 8193, 7); }, "Sub-frame separator output budget accepted", "budget");
    passed &= rejects([&] { prepareStemInput(source.value, std::numeric_limits<std::size_t>::max()); },
                      "Unaddressable separator input budget accepted", "addressable");
    passed &= rejects([&] { prepareStemOutput(raw.value, 8193, std::numeric_limits<std::size_t>::max()); },
                      "Unaddressable separator output budget accepted", "addressable");
    {
        InputFD writeOnly(rawPath, O_WRONLY | O_CLOEXEC);
        passed &= rejects([&] { prepareStemOutput(writeOnly.value, 8193); }, "Unreadable raw output descriptor accepted", "readable");
        InputFD originalWriteOnly(sourcePath, O_WRONLY | O_CLOEXEC);
        passed &= rejects([&] { prepareStemInput(originalWriteOnly.value); }, "Unreadable original source descriptor accepted", "readable");
        InputFD nonregular(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        passed &= rejects([&] { prepareStemOutput(nonregular.value, 8193); }, "Nonregular raw descriptor accepted", "regular");
        passed &= rejects([&] { prepareStemInput(nonregular.value); }, "Nonregular original descriptor accepted", "regular");
    }
    std::stop_source cancellation;
    cancellation.request_stop();
    passed &= rejects([&] { prepareStemInput(source.value, PreparedStemInput::defaultMaxPreparedBytes, cancellation.get_token()); },
                      "Cancelled separator input returned PCM", "cancel");
    passed &= rejects([&] { prepareStemOutput(raw.value, 8193, PreparedAudio::defaultMaxPreparedBytes, cancellation.get_token()); },
                      "Cancelled separator output returned PCM", "cancel");
    values.back() = std::numeric_limits<float>::infinity();
    rawFloatFile(rawPath, values);
    passed &= rejects([&] { prepareStemOutput(raw.value, 8193); }, "Late nonfinite raw stem output accepted", "non-finite");
    setSample(sourcePath, 8192 * 2 + 1, std::numeric_limits<float>::quiet_NaN());
    passed &= rejects([&] { prepareStemInput(source.value); }, "Late nonfinite original input accepted", "non-finite");
    std::filesystem::resize_file(rawPath, values.size() * sizeof(float) - 1);
    passed &= rejects([&] { prepareStemOutput(raw.value, 8193); }, "Incomplete raw float/stereo frame accepted", "geometry");
    passed &= require(descriptorCount() == before && fcntl(source.value, F_GETFD) >= 0 && fcntl(raw.value, F_GETFD) >= 0,
                      "Rejected/cancelled float bridge leaked or closed a borrowed descriptor");
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

bool specialFileRejection(const std::filesystem::path& directory)
{
    const auto fifo = directory / "audio-input.fifo";
    if (mkfifo(fifo.c_str(), 0600) != 0)
        throw std::runtime_error("Cannot create special-file input fixture");
    bool passed = true;
    for (const bool prepare : {false, true}) {
        const auto child = fork();
        if (child < 0)
            throw std::runtime_error("Cannot fork special-file rejection test");
        if (!child) {
            try {
                const auto before = descriptorCount();
                bool rejected = false;
                try {
                    if (prepare)
                        prepareAudioFile(fifo, PreparedAudio::defaultMaxPreparedBytes, {}, directory);
                    else
                        conflux::audio::decodeAudioFile(fifo);
                } catch (const std::invalid_argument& error) {
                    rejected = std::string(error.what()).find("regular local file") != std::string::npos;
                }
                _exit(rejected && descriptorCount() == before ? 0 : 1);
            } catch (...) {
                _exit(2);
            }
        }
        // With no FIFO writer, an accidental open-for-read blocks indefinitely.
        // Bound and reap each API independently so this regression cannot hang CI.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        int status = 0;
        pid_t result = 0;
        while ((result = waitpid(child, &status, WNOHANG)) == 0 || (result < 0 && errno == EINTR)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                kill(child, SIGKILL);
                do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
                passed &= require(false, prepare ? "Preparation blocked on a FIFO input" : "Decode blocked on a FIFO input");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        passed &= require(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                          prepare ? "Preparation did not reject the FIFO without leaking descriptors"
                                  : "Decode did not reject the FIFO without leaking descriptors");
    }
    return passed;
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
        const bool specialFiles = specialFileRejection(directory.path);
        const bool stemExact = stemInputExactAndOwnership(directory.path);
        const bool stemRate = stemInputRateAndBudget(directory.path);
        const bool stemOutputs = fourRawOutputsAlign(directory.path);
        const bool stemFailures = rawStemFailures(directory.path);
        return bypass && conversion && ownership && diskFailure && specialFiles &&
               stemExact && stemRate && stemOutputs && stemFailures ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

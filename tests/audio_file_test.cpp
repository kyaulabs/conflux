// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioFile.hpp"
#include "audio/RenderCore.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using conflux::audio::decodeAudioFile;
using conflux::audio::RenderCore;

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "conflux-audio-file-test-XXXXXX").string();
        if (const auto* created = mkdtemp(pattern.data()))
            path = created;
        else
            throw std::runtime_error("Cannot create test directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

void append16(std::vector<char>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<char>(value & 255));
    bytes.push_back(static_cast<char>((value >> 8) & 255));
}

void append32(std::vector<char>& bytes, std::uint32_t value)
{
    append16(bytes, static_cast<std::uint16_t>(value));
    append16(bytes, static_cast<std::uint16_t>(value >> 16));
}

// Write the fixture independently of libsndfile, including explicit little-endian
// channel samples. Decoder and expected output do not share a codec implementation.
void waveFile(const std::filesystem::path& path, std::uint16_t format, std::uint16_t bits,
              std::uint16_t channels, std::uint32_t rate, const std::vector<char>& payload)
{
    std::vector<char> header;
    auto tag = [&](const char* text) { header.insert(header.end(), text, text + 4); };
    tag("RIFF");
    append32(header, static_cast<std::uint32_t>(36 + payload.size()));
    tag("WAVE");
    tag("fmt ");
    append32(header, 16);
    append16(header, format);
    append16(header, channels);
    append32(header, rate);
    append32(header, rate * channels * bits / 8);
    append16(header, static_cast<std::uint16_t>(channels * bits / 8));
    append16(header, bits);
    tag("data");
    append32(header, static_cast<std::uint32_t>(payload.size()));
    std::ofstream file(path, std::ios::binary);
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!payload.empty())
        file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write test WAV");
}

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

template<typename Function>
bool rejects(Function&& function, const char* message)
{
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return require(false, message);
}

void floatWave(const std::filesystem::path& path, std::uint16_t channels,
               std::uint32_t rate, std::span<const float> samples)
{
    std::vector<char> payload;
    payload.reserve(samples.size() * sizeof(float));
    for (const auto sample : samples)
        append32(payload, std::bit_cast<std::uint32_t>(sample));
    waveFile(path, 3, 32, channels, rate, payload);
}

bool convertedLength(std::uint64_t actual, std::size_t inputFrames, std::uint32_t rate)
{
    const double ideal = inputFrames * 48000.0 / rate;
    return actual > 0 && static_cast<double>(actual) >= std::floor(ideal) - 1 &&
           static_cast<double>(actual) <= std::ceil(ideal) + 1;
}

bool convertedSines(const std::filesystem::path& directory)
{
    bool passed = true;
    for (const auto rate : {8000U, 44100U, 96000U, 192000U}) {
        const std::size_t frames = rate / 4;
        std::vector<float> input(frames * 2);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const double t = static_cast<double>(frame) / rate;
            input[frame * 2] = static_cast<float>(.32 * std::sin(2 * std::numbers::pi * 997 * t));
            input[frame * 2 + 1] = static_cast<float>(.17 * std::sin(2 * std::numbers::pi * 1733 * t + .43));
        }
        const auto path = directory / ("sine-" + std::to_string(rate) + ".wav");
        floatWave(path, 2, rate, input);
        // At 192 kHz this fits the normalized output, but cannot hold the
        // decoded source. At 8 kHz it is much larger than the source.
        constexpr std::size_t outputBudget = 12001 * 2 * sizeof(float);
        const auto decoded = decodeAudioFile(path, outputBudget);
        passed &= require(convertedLength(decoded.frames(), frames, rate) &&
                              decoded.samples().size_bytes() <= outputBudget &&
                              decoded.samples().size() == decoded.frames() * 2,
                          "converted stereo duration or normalized byte budget differs");
        passed &= require(std::ranges::all_of(decoded.samples(), [](float x) { return std::isfinite(x); }),
                          "converted sine contains a nonfinite sample");
        // Discard only filter-edge transients. The independent continuous-time
        // reference checks channel order, amplitude, frequency and phase, and
        // crosses the converter's 4096-source-frame callback boundaries.
        double squaredError = 0, maximumError = 0;
        std::size_t compared = 0;
        for (std::size_t frame = 1536; frame + 1536 < decoded.frames(); ++frame) {
            const double t = static_cast<double>(frame) / 48000;
            const std::array expected{.32 * std::sin(2 * std::numbers::pi * 997 * t),
                                     .17 * std::sin(2 * std::numbers::pi * 1733 * t + .43)};
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto error = std::abs(decoded.samples()[frame * 2 + channel] - expected[channel]);
                squaredError += error * error;
                maximumError = std::max(maximumError, error);
                ++compared;
            }
        }
        passed &= require(compared > 0 && std::sqrt(squaredError / compared) < 3e-4 && maximumError < 1e-3,
                          "resampled stereo sine lost timing, phase, channel identity or amplitude");
        passed &= rejects([&] { decodeAudioFile(path, 11998 * 2 * sizeof(float)); },
                          "converted output exceeded its normalized stereo byte budget");
        if (rate == 8000)
            passed &= rejects([&] { decodeAudioFile(path, input.size() * sizeof(float)); },
                              "upsampling budget was based only on source bytes");
    }
    // Exercise a fractional target duration separately from the exact quarter
    // seconds above; do not demand padding to an estimated frame count.
    std::vector<float> fractional(11032 * 2, .125F);
    const auto path = directory / "fractional-44100.wav";
    floatWave(path, 2, 44100, fractional);
    const auto decoded = decodeAudioFile(path);
    passed &= require(convertedLength(decoded.frames(), fractional.size() / 2, 44100),
                      "fractional resampling duration was not based on produced frames");
    return passed;
}

bool monoAndImpulseConversion(const std::filesystem::path& directory)
{
    bool passed = true;
    const std::array<float, 6> exactMono{-0.F, .125F, -.5F, 1.5F, -1.25F, .25F};
    const auto mono48 = directory / "mono-48000.wav";
    floatWave(mono48, 1, 48000, exactMono);
    const auto duplicated = decodeAudioFile(mono48, exactMono.size() * 2 * sizeof(float));
    passed &= require(duplicated.frames() == exactMono.size(), "mono 48 kHz duration changed");
    for (std::size_t frame = 0; frame < std::min<std::size_t>(exactMono.size(), duplicated.frames()); ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            passed &= require(std::bit_cast<std::uint32_t>(duplicated.samples()[frame * 2 + channel]) ==
                                  std::bit_cast<std::uint32_t>(exactMono[frame]),
                              "mono 48 kHz duplication changed sample bits or headroom");
    passed &= rejects([&] { decodeAudioFile(mono48, exactMono.size() * sizeof(float)); },
                      "mono output budget did not include both stereo channels");

    std::vector<float> mono(11025);
    for (std::size_t frame = 0; frame < mono.size(); ++frame)
        mono[frame] = static_cast<float>(1.75 * std::sin(2 * std::numbers::pi * 375 * frame / 44100.0));
    const auto mono44 = directory / "mono-44100.wav";
    floatWave(mono44, 1, 44100, mono);
    const auto converted = decodeAudioFile(mono44);
    passed &= require(convertedLength(converted.frames(), mono.size(), 44100), "converted mono duration differs");
    double peak = 0, maximumError = 0;
    for (std::size_t frame = 0; frame < converted.frames(); ++frame) {
        const auto left = converted.samples()[frame * 2], right = converted.samples()[frame * 2 + 1];
        passed &= require(std::isfinite(left) && left == right, "converted mono channels differ or contain nonfinite output");
        if (frame >= 1536 && frame + 1536 < converted.frames()) {
            peak = std::max(peak, std::abs(static_cast<double>(left)));
            maximumError = std::max(maximumError, std::abs(left - 1.75 * std::sin(2 * std::numbers::pi * 375 * frame / 48000.0)));
        }
    }
    passed &= require(peak > 1.7 && peak < 1.8 && maximumError < 1e-3,
                      "resampling clipped, normalized or shifted finite mono headroom");

    for (const auto rate : {8000U, 44100U, 96000U, 192000U}) {
        const std::size_t frames = rate / 4, impulse = rate / 10;
        const std::size_t activeChannel = rate <= 44100 ? 0 : 1;
        std::vector<float> input(frames * 2, 0.F);
        input[impulse * 2 + activeChannel] = 1.F;
        const auto path = directory / ("impulse-" + std::to_string(rate) + ".wav");
        floatWave(path, 2, rate, input);
        const auto decoded = decodeAudioFile(path);
        std::size_t peakFrame = 0;
        float maximum = 0.F;
        bool isolated = true, finite = true;
        for (std::size_t frame = 0; frame < decoded.frames(); ++frame) {
            const auto active = decoded.samples()[frame * 2 + activeChannel];
            isolated &= decoded.samples()[frame * 2 + 1 - activeChannel] == 0.F;
            finite &= std::isfinite(active);
            if (std::abs(active) > maximum) {
                maximum = std::abs(active);
                peakFrame = frame;
            }
        }
        passed &= require(isolated && finite && maximum > .2F && peakFrame >= 4799 && peakFrame <= 4801 &&
                              convertedLength(decoded.frames(), frames, rate),
                          "impulse conversion introduced channel bleed, timing delay or invalid output");
    }
    return passed;
}
} // namespace

int main()
{
    try {
        TemporaryDirectory directory;
        bool passed = true;
        constexpr std::size_t frames = 4099;
        std::vector<char> pcm16;
        std::vector<float> expected;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto left = static_cast<std::int16_t>(static_cast<int>((frame * 31) % 65536) - 32768);
            const auto right = static_cast<std::int16_t>(static_cast<int>((frame * 113 + 101) % 65536) - 32768);
            append16(pcm16, static_cast<std::uint16_t>(left));
            append16(pcm16, static_cast<std::uint16_t>(right));
            expected.push_back(static_cast<float>(left) / 32768.0F);
            expected.push_back(static_cast<float>(right) / 32768.0F);
        }
        const auto input = directory.path / "asymmetric.wav";
        waveFile(input, 1, 16, 2, 48000, pcm16);
        const auto decoded = decodeAudioFile(input, frames * 2 * sizeof(float));
        passed &= require(decoded.frames() == frames &&
            std::ranges::equal(decoded.samples(), expected), "PCM16 decode values or channel order differ");

        std::vector<float> whole((frames + 513) * 2, 42.0F), partitioned(whole.size(), 42.0F);
        RenderCore offline(decoded.samples()), live(decoded.samples());
        passed &= require(offline.render(whole), "whole PCM render failed");
        constexpr std::array<std::size_t, 5> partitions{1, 256, 512, 7, 1024};
        for (std::size_t offset = 0, part = 0; offset < partitioned.size(); ++part) {
            const auto samples = std::min(partitions[part % partitions.size()] * 2, partitioned.size() - offset);
            passed &= require(live.render(std::span(partitioned).subspan(offset, samples)), "partition PCM render failed");
            offset += samples;
        }
        passed &= require(whole == partitioned && std::equal(expected.begin(), expected.end(), whole.begin()),
                          "PCM live/offline partition parity failed");
        passed &= require(std::all_of(whole.begin() + expected.size(), whole.end(),
                                    [](float value) { return value == 0.0F; }), "EOF tail is not silence");
        passed &= require(live.frameClock() == frames + 513 && offline.frameClock() == frames + 513 &&
                          live.sourceFrame() == frames && live.sourceFrames() == frames,
                          "source position and output clock differ at EOF");

        std::array<float, 10> block{};
        const auto clock = live.frameClock();
        for (const std::uint64_t position : {3001, 17, 0}) {
            passed &= require(live.seek(position) && live.render(block), "PCM seek failed");
            passed &= require(std::equal(block.begin(), block.end(), expected.begin() + position * 2) &&
                              live.sourceFrame() == position + block.size() / 2,
                              "seek output or source position differs");
        }
        passed &= require(live.frameClock() == clock + 15, "seek reset the output clock");
        const auto beforeRejectedSeek = live.sourceFrame();
        passed &= require(!live.seek(frames + 1) && live.sourceFrame() == beforeRejectedSeek &&
                          !live.seek(UINT64_MAX) && live.frameClock() == clock + 15,
                          "invalid seek changed state");
        passed &= require(live.seek(frames), "seek to EOF failed");
        block.fill(42.0F);
        passed &= require(live.render(block) && std::ranges::all_of(block, [](float sample) { return sample == 0.0F; }) &&
                          live.sourceFrame() == frames && live.frameClock() == clock + 20,
                          "render after EOF seek is not clocked silence");
        const auto beforeInvalid = live.frameClock();
        std::array<float, 3> incomplete{42.0F, 42.0F, 42.0F};
        passed &= require(!live.render(incomplete) && live.frameClock() == beforeInvalid &&
                          live.sourceFrame() == frames && incomplete == std::array<float, 3>{42.0F, 42.0F, 42.0F},
                          "invalid output frame changed state or samples");
        passed &= require(live.render({}) && live.frameClock() == beforeInvalid && live.sourceFrame() == frames,
                          "empty render changed PCM state");
        RenderCore tone(true), silence;
        passed &= require(!tone.seek(0) && !silence.seek(0), "non-file renderer accepted source seek");

        std::vector<char> floatBytes;
        const std::array<float, 6> floatExpected{0.25F, -0.75F, 1.5F, -1.25F, -0.0F, 0.5F};
        for (const auto sample : floatExpected)
            append32(floatBytes, std::bit_cast<std::uint32_t>(sample));
        const auto floating = directory.path / "float.wav";
        waveFile(floating, 3, 32, 2, 48000, floatBytes);
        const auto decodedFloat = decodeAudioFile(floating);
        passed &= require(std::ranges::equal(decodedFloat.samples(), floatExpected), "float decode clipped or changed samples");
        for (std::size_t sample = 0; sample < std::min(decodedFloat.samples().size(), floatExpected.size()); ++sample)
            passed &= require(std::bit_cast<std::uint32_t>(decodedFloat.samples()[sample]) ==
                                  std::bit_cast<std::uint32_t>(floatExpected[sample]),
                              "stereo 48 kHz bypass changed float sample bits");

        passed &= convertedSines(directory.path);
        passed &= monoAndImpulseConversion(directory.path);

        passed &= rejects([&] { decodeAudioFile(input, frames * 2 * sizeof(float) - 1); }, "decoded budget was not enforced");
        passed &= rejects([&] { decodeAudioFile(input, 0); }, "empty decoded budget was accepted");
        passed &= rejects([&] { decodeAudioFile(directory.path / "missing.wav"); }, "missing file was accepted");
        passed &= rejects([&] { decodeAudioFile(directory.path); }, "directory audio input was accepted");
        const auto invalid = directory.path / "invalid.wav";
        { std::ofstream file(invalid); file << "RIFF"; }
        passed &= rejects([&] { decodeAudioFile(invalid); }, "truncated header was accepted");
        const auto empty = directory.path / "empty.wav";
        waveFile(empty, 1, 16, 2, 48000, {});
        passed &= rejects([&] { decodeAudioFile(empty); }, "empty audio was accepted");
        const auto multichannel = directory.path / "multichannel.wav";
        waveFile(multichannel, 1, 16, 3, 48000, std::vector<char>(24, 0));
        passed &= rejects([&] { decodeAudioFile(multichannel); }, "unsupported multichannel audio was accepted");
        for (const auto rate : {7999U, 192001U}) {
            const auto otherRate = directory.path / ("unsupported-" + std::to_string(rate) + ".wav");
            waveFile(otherRate, 1, 16, 2, rate, pcm16);
            passed &= rejects([&] { decodeAudioFile(otherRate); }, "out-of-range sample rate was accepted");
        }
        for (const auto bad : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            std::vector<char> invalidPcm;
            append32(invalidPcm, std::bit_cast<std::uint32_t>(bad));
            append32(invalidPcm, 0);
            waveFile(floating, 3, 32, 2, 48000, invalidPcm);
            passed &= rejects([&] { decodeAudioFile(floating); }, "non-finite decoded audio was accepted");
            const std::array<float, 2> badFrame{bad, 0.0F};
            passed &= rejects([&] { RenderCore rejected{badFrame}; }, "non-finite playback PCM was accepted");
            for (const auto rate : {44100U, 48000U, 96000U})
                for (const std::uint16_t channels : {1, 2}) {
                    std::vector<float> nonfinite(8193 * channels, 0.F);
                    nonfinite.back() = bad;
                    floatWave(floating, channels, rate, nonfinite);
                    passed &= rejects([&] { decodeAudioFile(floating); },
                                      "nonfinite input at a conversion block boundary was accepted");
                }
        }
        passed &= rejects([] { RenderCore rejected{std::span<const float>{}}; }, "empty playback PCM was accepted");
        passed &= rejects([&] { RenderCore rejected{std::span<const float>(incomplete)}; }, "incomplete playback PCM was accepted");
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

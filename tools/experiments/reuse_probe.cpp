// SPDX-License-Identifier: AGPL-3.0-or-later
// API integration smoke probe. This is not a callback or musical-quality benchmark.
#include <rubberband/RubberBandStretcher.h>
#include <samplerate.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <stdexcept>
#include <vector>

using RubberBand::RubberBandStretcher;
using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: reuse-probe NEW_TEMPORARY_WAV_PATH\n");
        return 1;
    }
    if (std::filesystem::exists(argv[1])) {
        std::fprintf(stderr, "Refusing to overwrite an existing file\n");
        return 1;
    }
    constexpr int channels = 2;
    constexpr int sourceRate = 44100;
    constexpr std::size_t frames = sourceRate * 10;
    std::vector<float> input(frames * channels), loaded(input.size());
    for (std::size_t i = 0; i < frames; ++i) {
        const auto value = static_cast<float>(0.1 * std::sin(2 * std::numbers::pi * 440 * i / sourceRate));
        input[2 * i] = input[2 * i + 1] = value;
    }
    SF_INFO info{};
    info.samplerate = sourceRate;
    info.channels = channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    auto* file = sf_open(argv[1], SFM_WRITE, &info);
    if (!file) throw std::runtime_error(sf_strerror(nullptr));
    const auto written = sf_writef_float(file, input.data(), frames);
    const auto writeClose = sf_close(file);
    if (written != frames || writeClose != 0) return 2;
    file = sf_open(argv[1], SFM_READ, &info);
    if (!file) throw std::runtime_error(sf_strerror(nullptr));
    const auto read = sf_readf_float(file, loaded.data(), frames);
    const auto readClose = sf_close(file);
    if (read != frames || readClose != 0 || loaded != input) return 3;
    std::printf("sndfile=%s frames=%lld roundtrip_exact=yes\n", sf_version_string(), static_cast<long long>(read));
    int formatCount = 0;
    sf_command(nullptr, SFC_GET_FORMAT_MAJOR_COUNT, &formatCount, sizeof formatCount);
    for (int index = 0; index < formatCount; ++index) {
        SF_FORMAT_INFO format{};
        format.format = index;
        sf_command(nullptr, SFC_GET_FORMAT_MAJOR, &format, sizeof format);
        if (format.format == SF_FORMAT_WAV || format.format == SF_FORMAT_AIFF ||
            format.format == SF_FORMAT_FLAC || format.format == SF_FORMAT_OGG ||
            format.format == SF_FORMAT_MPEG) {
            std::printf("format=%s\n", format.name);
        }
    }

    std::vector<float> resampled(480100 * channels);
    SRC_DATA conversion{};
    conversion.data_in = loaded.data();
    conversion.input_frames = frames;
    conversion.data_out = resampled.data();
    conversion.output_frames = 480100;
    conversion.src_ratio = 48000.0 / sourceRate;
    conversion.end_of_input = 1;
    auto start = Clock::now();
    const auto error = src_simple(&conversion, SRC_SINC_FASTEST, channels);
    std::printf("samplerate=%s error=%d input=%ld output=%ld wall_ms=%.3f\n",
                src_get_version(), error, conversion.input_frames_used, conversion.output_frames_gen, elapsedMs(start));
    if (error || conversion.input_frames_used != frames) return 4;

    for (const bool finer : {false, true}) {
        const auto options = RubberBandStretcher::OptionProcessRealTime |
                             RubberBandStretcher::OptionThreadingNever |
                             RubberBandStretcher::OptionChannelsTogether |
                             RubberBandStretcher::OptionFormantPreserved |
                             (finer ? RubberBandStretcher::OptionEngineFiner : RubberBandStretcher::OptionEngineFaster);
        start = Clock::now();
        RubberBandStretcher stretcher(48000, channels, options, 1.06, 1.0);
        stretcher.setMaxProcessSize(512);
        const auto pad = stretcher.getPreferredStartPad();
        const auto delay = stretcher.getStartDelay();
        std::size_t retrieved = 0;
        std::array<std::array<float, 512>, channels> in{}, out{};
        const float* inChannels[channels] = {in[0].data(), in[1].data()};
        float* outChannels[channels] = {out[0].data(), out[1].data()};
        auto drain = [&] {
            while (stretcher.available() > 0) {
                retrieved += stretcher.retrieve(outChannels, std::min<std::size_t>(512, stretcher.available()));
            }
        };
        for (std::size_t position = 0; position < pad;) {
            const auto count = std::min<std::size_t>(512, pad - position);
            stretcher.process(inChannels, count, false);
            drain();
            position += count;
        }
        for (std::size_t position = 0; position < static_cast<std::size_t>(conversion.output_frames_gen);) {
            const auto count = std::min<std::size_t>(512, conversion.output_frames_gen - position);
            for (std::size_t i = 0; i < count; ++i) {
                in[0][i] = resampled[2 * (position + i)];
                in[1][i] = resampled[2 * (position + i) + 1];
            }
            position += count;
            stretcher.process(inChannels, count, position == static_cast<std::size_t>(conversion.output_frames_gen));
            drain();
        }
        std::printf("rubberband_engine=%d sample_rate=48000 ratio=1.06 pad=%zu delay=%zu untrimmed_output_frames=%zu wall_ms=%.3f\n",
                    stretcher.getEngineVersion(), pad, delay, retrieved, elapsedMs(start));
        if (retrieved <= delay) return 5;
    }
}

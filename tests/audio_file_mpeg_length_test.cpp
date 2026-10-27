// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioFile.hpp"
#include <sndfile.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace {
enum class Header { Exact, Over, Under, Unknown, ExactOver, DecoderError };
Header header = Header::Exact;
}
extern "C" SNDFILE* __real_sf_open_fd(int, int, SF_INFO*, int);
extern "C" int __real_sf_error(SNDFILE*);
extern "C" SNDFILE* __wrap_sf_open_fd(int fd, int mode, SF_INFO* info, int closeFd)
{
    auto* file = __real_sf_open_fd(fd, mode, info, closeFd);
    if (file && mode == SFM_READ && header != Header::Exact) {
        if (header != Header::ExactOver)
            info->format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
        if (header == Header::Over || header == Header::ExactOver) info->frames += 7000;
        if (header == Header::Under) info->frames -= 3000;
        if (header == Header::Unknown) info->frames = std::numeric_limits<sf_count_t>::max();
    }
    return file;
}
extern "C" int __wrap_sf_error(SNDFILE* file)
{
    return header == Header::DecoderError ? SF_ERR_SYSTEM : __real_sf_error(file);
}

int main()
{
    using namespace conflux::audio;
    auto pattern = (std::filesystem::temp_directory_path() / "conflux-codec-length-XXXXXX").string();
    const auto* created = mkdtemp(pattern.data());
    if (!created) return 1;
    const std::filesystem::path directory(created);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
    auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    try {
        for (const int rate : {44100, 48000}) for (const int channels : {1, 2}) {
            constexpr sf_count_t frames = 8195;
            std::vector<float> input(frames * channels);
            for (sf_count_t frame = 0; frame < frames; ++frame)
                for (int channel = 0; channel < channels; ++channel)
                    input[frame * channels + channel] = float(0.3 * std::sin(frame * (channel + 1) * 0.017) + 0.05);
            const auto path = directory / "generated.wav";
            SF_INFO info{}; info.samplerate = rate; info.channels = channels; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
            auto* writer = sf_open(path.c_str(), SFM_WRITE, &info);
            require(writer, "Cannot create generated codec-contract input");
            const auto written = sf_writef_float(writer, input.data(), frames);
            const auto closed = sf_close(writer);
            require(written == frames && closed == 0, "Cannot finish generated input");
            header = Header::Exact;
            const auto baseline = decodeAudioFile(path);
            for (const auto estimate : {Header::Over, Header::Under, Header::Unknown}) {
                header = estimate;
                // Real decoding, conversion and output limits are unchanged;
                // only the public codec length hint differs from actual EOF.
                const auto decoded = decodeAudioFile(path);
                require(decoded.frames() == baseline.frames() &&
                    std::ranges::equal(decoded.samples(), baseline.samples()), "Estimated codec length lost or changed samples");
                const auto prepared = prepareAudioFile(path, baseline.samples().size_bytes(), {}, directory);
                require(prepared.frames() == baseline.frames(), "Prepared estimated codec extent differs");
                std::vector<float> pcm(baseline.samples().size());
                require(pread(prepared.descriptor(), pcm.data(), pcm.size() * sizeof(float), 0) ==
                    static_cast<ssize_t>(pcm.size() * sizeof(float)) &&
                    std::ranges::equal(pcm, baseline.samples()), "Prepared estimated codec tail or channels differ");
                bool refused = false;
                try { (void)prepareAudioFile(path, baseline.samples().size_bytes() - 8, {}, directory); }
                catch (const std::length_error&) { refused = true; }
                require(refused, "Estimated codec length bypassed actual PCM output budget");
            }
            for (const auto invalid : {Header::ExactOver, Header::DecoderError}) {
                header = invalid;
                bool refused = false;
                try { (void)decodeAudioFile(path); }
                catch (const std::runtime_error&) { refused = true; }
                require(refused, "Exact-length short read or decoder error was accepted");
            }
        }
        std::cout << "Estimated codec lengths preserve all samples, channels and budgets at both rates; exact-length faults remain rejected.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

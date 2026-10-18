// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioFile.hpp"
#include "audio/RenderCore.hpp"

#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace conflux::audio {

DecodedAudio::DecodedAudio(std::vector<float> samples) noexcept : samples_(std::move(samples)) {}

std::uint64_t DecodedAudio::frames() const noexcept
{
    return samples_.size() / RenderCore::channels;
}

DecodedAudio decodeAudioFile(const std::filesystem::path& path, std::size_t maxDecodedBytes)
{
    constexpr auto frameBytes = RenderCore::channels * sizeof(float);
    if (maxDecodedBytes < frameBytes)
        throw std::invalid_argument("Decoded audio budget must hold at least one stereo frame");
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(path, fileError))
        throw std::invalid_argument("Audio input must be a regular local file");

    SF_INFO info{};
    std::unique_ptr<SNDFILE, decltype(&sf_close)> file(sf_open(path.c_str(), SFM_READ, &info), sf_close);
    if (!file)
        throw std::runtime_error(std::string("Cannot open audio file: ") + sf_strerror(nullptr));
    if (info.samplerate != static_cast<int>(RenderCore::sampleRate) ||
        info.channels != static_cast<int>(RenderCore::channels))
        throw std::invalid_argument("Audio file must be stereo 48000 Hz; resampling is not implemented");
    if (info.frames <= 0)
        throw std::runtime_error("Audio file contains no frames");

    const auto frames = static_cast<std::uint64_t>(info.frames);
    std::vector<float> samples;
    if (frames > maxDecodedBytes / frameBytes || frames > samples.max_size() / RenderCore::channels)
        throw std::length_error("Audio file exceeds the decoded PCM byte budget");
    samples.resize(static_cast<std::size_t>(frames) * RenderCore::channels);
    const auto read = sf_readf_float(file.get(), samples.data(), info.frames);
    if (read != info.frames || sf_error(file.get()) != SF_ERR_NO_ERROR)
        throw std::runtime_error(std::string("Could not decode every audio frame: ") + sf_strerror(file.get()));
    if (sf_close(file.release()) != 0)
        throw std::runtime_error("Could not close decoded audio file");
    if (std::ranges::any_of(samples, [](float sample) { return !std::isfinite(sample); }))
        throw std::runtime_error("Decoded audio contains non-finite samples");
    return DecodedAudio(std::move(samples));
}

} // namespace conflux::audio

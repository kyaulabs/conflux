// SPDX-License-Identifier: AGPL-3.0-or-later
// Offline corpus validation only. No live session, device or audio output.
#include "audio/AudioFile.hpp"
#include "audio/PcmReadAhead.hpp"
#include "audio/RenderCore.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using conflux::audio::PcmReadAhead;
using conflux::audio::PreparedAudio;
using conflux::audio::RenderCore;
using Clock = std::chrono::steady_clock;
constexpr std::size_t blockFrames = 256;
constexpr std::size_t channels = RenderCore::channels;

double elapsedMs(Clock::time_point began)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - began).count();
}

void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

QJsonObject checkFile(const std::filesystem::path& spoolDirectory, const char* name)
{
    const auto began = Clock::now();
    QJsonObject result{
        {"source", QString::fromLocal8Bit(name)},
        {"passed", false},
        {"all_samples_exact", QJsonValue::Null},
        {"fixed_cache_bytes", qint64(PcmReadAhead::cacheBytes)}};
    try {
        SF_INFO info{};
        std::unique_ptr<SNDFILE, decltype(&sf_close)> reference(
            sf_open(name, SFM_READ, &info), sf_close);
        if (!reference)
            throw std::runtime_error(std::string("Cannot open independent decoder reference: ") + sf_strerror(nullptr));
        const bool exactReference = info.channels == int(channels) && info.samplerate == int(RenderCore::sampleRate);
        result.insert("source_rate", info.samplerate);
        result.insert("source_channels", info.channels);
        result.insert("source_frames", qint64(info.frames));
        result.insert("rate_conversion", info.samplerate != int(RenderCore::sampleRate));
        result.insert("mono_duplication", info.channels == 1);
        result.insert("exact_reference_available", exactReference);
        if (!exactReference)
            result.insert("reference_note", "Conversion path checked for finite complete rendering; no bit-exact source reference");

        const auto preparationBegan = Clock::now();
        auto prepared = conflux::audio::prepareAudioFile(
            std::filesystem::path(name), PreparedAudio::defaultMaxPreparedBytes, {}, spoolDirectory);
        result.insert("preparation_elapsed_ms", elapsedMs(preparationBegan));
        result.insert("prepared_frames", qint64(prepared.frames()));
        result.insert("temporary_pcm_bytes", qint64(prepared.frames() * channels * sizeof(float)));
        result.insert("overview_bins", qint64(prepared.peaks().size()));
        if (exactReference)
            require(info.frames > 0 && prepared.frames() == static_cast<std::uint64_t>(info.frames),
                "Stereo 48 kHz preparation changed the source frame count");

        const auto validationBegan = Clock::now();
        PcmReadAhead reader(prepared.descriptor(), prepared.frames());
        require(reader.prime(0, std::chrono::seconds(2)), "Cannot prime normalized PCM at frame zero");
        RenderCore renderer(reader.sourceRef());
        std::array<float, blockFrames * channels> actual{}, expected{};
        std::uint64_t comparedSamples = 0, mismatchedSamples = 0, renderedBlocks = 0;
        bool finite = true, eofSilence = true;
        while (renderer.sourceFrame() < prepared.frames()) {
            const auto first = renderer.sourceFrame();
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(blockFrames, prepared.frames() - first));
            // Control-thread preparation before each deterministic render block;
            // these waits do not measure real-time delivery or callback latency.
            if (!reader.prime(first, std::chrono::seconds(2)))
                throw std::runtime_error("Cannot prime normalized PCM: " + reader.error());
            if (exactReference) {
                const auto read = sf_readf_float(reference.get(), expected.data(), static_cast<sf_count_t>(count));
                require(read == static_cast<sf_count_t>(count) && sf_error(reference.get()) == SF_ERR_NO_ERROR,
                    "Independent reference decoder did not return every requested frame");
            }
            actual.fill(12345.F);
            require(renderer.render(actual), "Shared renderer rejected a complete stereo block");
            ++renderedBlocks;
            require(renderer.sourceFrame() == first + count && !renderer.readFailed() &&
                    !renderer.isBuffering() && renderer.starvedFrames() == 0,
                "Primed offline rendering lost source frames, starved or reported a read error");
            for (std::size_t sample = 0; sample < count * channels; ++sample) {
                finite = finite && std::isfinite(actual[sample]);
                if (exactReference) {
                    ++comparedSamples;
                    if (std::bit_cast<std::uint32_t>(actual[sample]) != std::bit_cast<std::uint32_t>(expected[sample])) {
                        if (!mismatchedSamples) {
                            result.insert("first_mismatch_frame", qint64(first + sample / channels));
                            result.insert("first_mismatch_channel", int(sample % channels));
                        }
                        ++mismatchedSamples;
                    }
                }
            }
            for (std::size_t sample = count * channels; sample < actual.size(); ++sample)
                eofSilence = eofSilence && actual[sample] == 0.F;
            require(renderer.frameClock() == renderedBlocks * blockFrames,
                "Output clock differs from the number of rendered blocks");
        }
        if (exactReference) {
            std::array<float, channels> extra{};
            require(sf_readf_float(reference.get(), extra.data(), 1) == 0 &&
                    sf_error(reference.get()) == SF_ERR_NO_ERROR,
                "Independent reference contains unrendered frames or a final read error");
        }
        const auto beforeEof = renderer.frameClock();
        actual.fill(12345.F);
        require(renderer.render(actual), "Renderer rejected the final EOF block");
        eofSilence = eofSilence && std::all_of(actual.begin(), actual.end(), [](float value) { return value == 0.F; });
        require(renderer.sourceFrame() == prepared.frames() && renderer.frameClock() == beforeEof + blockFrames &&
                renderer.isPlaying() && renderer.starvedFrames() == 0 && !renderer.isBuffering() &&
                !renderer.readFailed(), "EOF changed transport, cursor or starvation state");
        require(reader.errorCode() == 0, "PCM read-ahead worker retained an error");
        reader.stop();
        if (sf_close(reference.release()) != 0)
            throw std::runtime_error("Could not close independent reference decoder");
        result.insert("validation_elapsed_ms", elapsedMs(validationBegan));
        result.insert("compared_samples", qint64(comparedSamples));
        result.insert("mismatched_samples", qint64(mismatchedSamples));
        result.insert("all_samples_exact", exactReference ? QJsonValue(mismatchedSamples == 0) : QJsonValue(QJsonValue::Null));
        result.insert("all_rendered_samples_finite", finite);
        result.insert("tail_eof_silence", eofSilence);
        result.insert("starved_frames", qint64(renderer.starvedFrames()));
        result.insert("final_source_frame", qint64(renderer.sourceFrame()));
        result.insert("final_output_frame", qint64(renderer.frameClock()));
        result.insert("passed", finite && eofSilence && (!exactReference || mismatchedSamples == 0));
    } catch (const std::exception& error) {
        result.insert("error", QString::fromUtf8(error.what()));
    }
    result.insert("elapsed_ms", elapsedMs(began));
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 10) {
        std::cerr << "Usage: conflux-prepare-audio-check SPOOL_DIRECTORY FILE... (1 to 8 files)\n";
        return 2;
    }
    const auto began = Clock::now();
    const std::filesystem::path spoolDirectory(argv[1]);
    QJsonArray files;
    bool passed = true;
    for (int index = 2; index < argc; ++index) {
        auto result = checkFile(spoolDirectory, argv[index]);
        passed = passed && result.value("passed").toBool();
        files.append(result);
    }
    const QJsonObject report{
        {"kind", "offline-prepared-audio-corpus-check"},
        {"audio_output_opened", false},
        {"realtime_delivery_measured", false},
        {"spool_directory", QString::fromLocal8Bit(argv[1])},
        {"block_frames", int(blockFrames)},
        {"files", files},
        {"elapsed_ms", elapsedMs(began)},
        {"passed", passed}};
    std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).constData();
    return passed ? 0 : 1;
}

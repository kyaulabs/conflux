// SPDX-License-Identifier: AGPL-3.0-or-later
// Isolated M0 adapter for audio.cpp v0.9.2's public, non-streaming chunk API.
// JSONL requests: {"id": 1, "input": "window.f32", "output": "stems.f32"}.
// F32LE input is one normalized stereo window; output is source-major,
// interleaved stereo. The caller owns source normalization, overlap and cancellation.
#include "engine/framework/core/execution_context.h"
#include "engine/framework/io/json.h"
#include "engine/models/demucs/assets.h"
#include "engine/models/demucs/pipeline.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {
namespace json = engine::io::json;
using Clock = std::chrono::steady_clock;
constexpr auto revision = "0097d365a00b52a6991ad7618f0a04e8d2935e47";

double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void emit(json::Value::Object object) {
    std::cout << json::stringify(json::Value::make_object(std::move(object))) << '\n' << std::flush;
    if (!std::cout) {
        throw std::runtime_error("worker output pipe closed");
    }
}

std::vector<float> read_samples(const std::filesystem::path& path, std::size_t count) {
    if (!std::filesystem::is_regular_file(path)
        || std::filesystem::file_size(path) != count * sizeof(float)) {
        throw std::runtime_error("input must be one exact interleaved F32LE segment");
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<float> samples(count);
    input.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(count * sizeof(float)));
    if (!input || std::any_of(samples.begin(), samples.end(), [](float x) { return !std::isfinite(x); })) {
        throw std::runtime_error("input contains unreadable or non-finite samples");
    }
    return samples;
}

void write_samples(const std::filesystem::path& path, const std::vector<float>& samples) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(samples.data()),
        static_cast<std::streamsize>(samples.size() * sizeof(float)));
    output.close();
    if (!output) {
        throw std::runtime_error("could not write output segment");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if constexpr (std::endian::native != std::endian::little) {
            throw std::runtime_error("this experimental adapter requires a little-endian host");
        }
        std::filesystem::path model;
        engine::core::BackendConfig backend;
        backend.type = engine::core::BackendType::Vulkan;
        backend.threads = 4;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--help") {
                std::cout << "conflux-stem-runtime --model FILE [--backend vulkan|cpu] [--threads N]\n";
                return 0;
            }
            if (index + 1 >= argc) {
                throw std::runtime_error("missing argument value");
            }
            const std::string value = argv[++index];
            if (argument == "--model") {
                model = value;
            } else if (argument == "--backend") {
                if (value == "vulkan") {
                    backend.type = engine::core::BackendType::Vulkan;
                } else if (value == "cpu") {
                    backend.type = engine::core::BackendType::Cpu;
                } else {
                    throw std::runtime_error("backend must be vulkan or cpu");
                }
            } else if (argument == "--threads") {
                std::size_t consumed = 0;
                backend.threads = std::stoi(value, &consumed);
                if (consumed != value.size() || backend.threads < 1 || backend.threads > 64) {
                    throw std::runtime_error("threads must be between 1 and 64");
                }
            } else {
                throw std::runtime_error("unknown argument: " + argument);
            }
        }
        if (model.empty()) {
            throw std::runtime_error("--model is required");
        }

        const auto load_start = Clock::now();
        engine::core::ExecutionContext context(backend);
        const auto assets = engine::models::demucs::load_htdemucs_assets(model);
        if (assets->submodels.size() != 1) {
            throw std::runtime_error("only a single HTDemucs model is supported");
        }
        engine::models::demucs::HTDemucsPipeline pipeline(assets->submodels.front(), context,
            context.uses_host_graph_plan() ? engine::assets::TensorStorageType::F32
                                         : engine::assets::TensorStorageType::Native);
        const auto& config = pipeline.config();
        if (config.sample_rate != 44100 || config.audio_channels != 2
            || config.segment_samples != 343980 || config.sources.size() != 4) {
            throw std::runtime_error("adapter requires the measured four-stem 7.8-second HTDemucs model");
        }
        assets->submodels.front()->tensor_source->release_storage();
        json::Value::Array names;
        for (const auto& name : config.sources) {
            names.push_back(json::Value::make_string(name));
        }
        emit({{"event", json::Value::make_string("ready")},
            {"revision", json::Value::make_string(revision)},
            {"model", json::Value::make_string(std::filesystem::canonical(model).string())},
            {"backend", json::Value::make_string(backend.type == engine::core::BackendType::Vulkan ? "vulkan" : "cpu")},
            {"threads", json::Value::make_number(backend.threads)},
            {"weight_storage", json::Value::make_string(context.uses_host_graph_plan() ? "f32" : "native")},
            {"pid", json::Value::make_number(getpid())},
            {"sample_rate", json::Value::make_number(config.sample_rate)},
            {"channels", json::Value::make_number(config.audio_channels)},
            {"segment_frames", json::Value::make_number(config.segment_samples)},
            {"stems", json::Value::make_array(std::move(names))},
            {"load_ms", json::Value::make_number(milliseconds(load_start))}});

        std::vector<float> planar(static_cast<std::size_t>(config.segment_samples * config.audio_channels));
        std::vector<float> output(planar.size() * config.sources.size());
        std::string line;
        while (std::getline(std::cin, line)) {
            std::int64_t id = -1;
            try {
                if (line.size() > 16384) {
                    throw std::runtime_error("request line is too long");
                }
                const auto request = json::parse(line);
                if (json::optional_string(request, "command", "") == "quit") {
                    return 0;
                }
                const auto request_id = request.require("id").as_number();
                if (!std::isfinite(request_id) || request_id < 0.0 || request_id > 9007199254740991.0
                    || std::trunc(request_id) != request_id) {
                    throw std::runtime_error("id must be a nonnegative exact JSON integer");
                }
                id = static_cast<std::int64_t>(request_id);
                const auto request_start = Clock::now();
                const auto input_path = std::filesystem::path(json::require_string(request, "input"));
                const auto output_path = std::filesystem::path(json::require_string(request, "output"));
                if (input_path == output_path) {
                    throw std::runtime_error("input and output paths must differ");
                }
                const auto samples = read_samples(input_path, planar.size());
                for (std::int64_t frame = 0; frame < config.segment_samples; ++frame) {
                    for (int channel = 0; channel < config.audio_channels; ++channel) {
                        planar[static_cast<std::size_t>(channel * config.segment_samples + frame)] =
                            samples[static_cast<std::size_t>(frame * config.audio_channels + channel)];
                    }
                }
                // separate_chunk normalizes/mutates planar and reuses its result storage.
                // Each call receives a fresh window; copy before the next call.
                const auto& separated = pipeline.separate_chunk(planar);
                if (separated.size() != output.size()) {
                    throw std::runtime_error("separator output shape mismatch");
                }
                for (std::size_t source = 0; source < config.sources.size(); ++source) {
                    for (std::int64_t frame = 0; frame < config.segment_samples; ++frame) {
                        for (int channel = 0; channel < config.audio_channels; ++channel) {
                            output[source * planar.size() + static_cast<std::size_t>(frame * config.audio_channels + channel)] =
                                separated[(source * config.audio_channels + channel) * static_cast<std::size_t>(config.segment_samples)
                                    + static_cast<std::size_t>(frame)];
                        }
                    }
                }
                if (std::any_of(output.begin(), output.end(), [](float x) { return !std::isfinite(x); })) {
                    throw std::runtime_error("separator returned non-finite samples");
                }
                write_samples(output_path, output);
                const auto& timing = pipeline.last_timing();
                emit({{"event", json::Value::make_string("result")},
                    {"id", json::Value::make_number(id)},
                    {"frames", json::Value::make_number(config.segment_samples)},
                    {"stems", json::Value::make_number(config.sources.size())},
                    {"elapsed_ms", json::Value::make_number(milliseconds(request_start))},
                    {"frontend_ms", json::Value::make_number(timing.frontend_ms)},
                    {"graph_ms", json::Value::make_number(timing.graph_ms)},
                    {"postprocess_ms", json::Value::make_number(timing.postprocess_ms)},
                    {"graph_rebuild_ms", json::Value::make_number(timing.graph_rebuild_ms)}});
            } catch (const std::exception& error) {
                emit({{"event", json::Value::make_string("error")},
                    {"id", json::Value::make_number(id)},
                    {"message", json::Value::make_string(error.what())}});
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "conflux-stem-runtime: " << error.what() << '\n';
        return 1;
    }
}

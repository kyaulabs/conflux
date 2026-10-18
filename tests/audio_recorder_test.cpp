// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/StereoRecorder.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using conflux::audio::StereoRecorder;

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "conflux-recorder-test-XXXXXX").string();
        if (const auto* created = mkdtemp(pattern.data()))
            path = created;
        else
            throw std::runtime_error("Cannot create recorder test directory");
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
bool rejects(Function&& function, const char* message)
{
    try {
        function();
    } catch (const std::exception& error) {
        return require(error.what()[0] != '\0', "Recorder rejection had no error message");
    }
    return require(false, message);
}

std::uint16_t read16(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return std::uint16_t(bytes.at(offset)) | (std::uint16_t(bytes.at(offset + 1)) << 8);
}

std::uint32_t read32(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return std::uint32_t(read16(bytes, offset)) | (std::uint32_t(read16(bytes, offset + 2)) << 16);
}

// Independent RIFF/IEEE-float readback: no libsndfile decoder shares the
// writer's channel, normalization, frame-count or sample-conversion behavior.
std::vector<float> readWave(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read recorded WAV");
    const std::vector<unsigned char> bytes(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    auto tag = [&](std::size_t offset, const char* text) {
        return offset + 4 <= bytes.size() && std::equal(text, text + 4, bytes.begin() + offset);
    };
    if (bytes.size() < 12 || !tag(0, "RIFF") || !tag(8, "WAVE") || read32(bytes, 4) != bytes.size() - 8)
        throw std::runtime_error("Recording did not finalize a bounded RIFF WAV");
    bool format = false, data = false;
    std::vector<float> samples;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const auto size = read32(bytes, offset + 4);
        const auto start = offset + 8;
        if (size > bytes.size() - start)
            throw std::runtime_error("Recorded chunk exceeds the WAV file");
        if (tag(offset, "fmt ")) {
            if (size < 16 || read16(bytes, start) != 3 || read16(bytes, start + 2) != 2 ||
                read32(bytes, start + 4) != 48000 || read32(bytes, start + 8) != 48000 * 8 ||
                read16(bytes, start + 12) != 8 || read16(bytes, start + 14) != 32)
                throw std::runtime_error("Recording is not stereo 48 kHz IEEE float WAV");
            format = true;
        } else if (tag(offset, "data")) {
            if (data || size % 8)
                throw std::runtime_error("Recording has duplicate or incomplete stereo data");
            for (std::size_t sample = 0; sample < size; sample += sizeof(float))
                samples.push_back(std::bit_cast<float>(read32(bytes, start + sample)));
            data = true;
        }
        offset = start + size + (size & 1U);
    }
    if (!format || !data)
        throw std::runtime_error("Recording lacks its format or PCM data");
    return samples;
}

bool exactSamples(const std::vector<float>& actual, const std::vector<float>& expected)
{
    return actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin(),
        [](float left, float right) { return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right); });
}

bool waitWritten(const StereoRecorder& recorder, std::uint64_t frames)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (recorder.writtenFrames() < frames && !recorder.failed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return recorder.writtenFrames() >= frames;
}

bool orderWrapAndDrain(const std::filesystem::path& directory)
{
    const auto path = directory / "wrapped.wav";
    StereoRecorder recorder(path, 7);
    bool passed = require(recorder.running() && !recorder.failed() && recorder.error().empty() &&
                              recorder.acceptedFrames() == 0 && recorder.writtenFrames() == 0,
                          "New recorder did not start empty and accepting");
    passed &= require(recorder.push({}), "Empty PCM push was rejected while recording");
    std::vector<float> expected;
    constexpr std::array<std::size_t, 4> batches{5, 6, 3, 7};
    for (std::size_t batch = 0; batch < batches.size(); ++batch) {
        std::vector<float> block;
        for (std::size_t frame = 0; frame < batches[batch]; ++frame) {
            const auto index = expected.size() / 2 + frame;
            block.push_back(float(int(index * 7 % 127) - 63) / 1048576.0F);
            block.push_back(float(int(index * 31 % 97) - 48) / 1048576.0F);
        }
        if (batch == 0) {
            block[0] = -0.0F;
            // This headroom fixture is written to disk only, never played.
            block[2] = 1.5F;
            block[3] = -1.25F;
        }
        passed &= require(recorder.push(block), "A complete in-capacity recording span was rejected");
        expected.insert(expected.end(), block.begin(), block.end());
        if (batch + 1 < batches.size())
            passed &= require(waitWritten(recorder, expected.size() / 2), "Recording writer did not make bounded progress");
    }
    // The last whole-capacity span is not awaited: stop must retain and drain
    // its entire accepted tail, including two physical portions at ring wrap.
    recorder.stop();
    recorder.stop();
    passed &= require(!recorder.running() && !recorder.failed() && recorder.error().empty() &&
                          recorder.acceptedFrames() == expected.size() / 2 &&
                          recorder.writtenFrames() == recorder.acceptedFrames() && recorder.overflowCount() == 0 &&
                          recorder.overflowFrames() == 0 && !recorder.push(std::array<float, 2>{}),
                      "Stopped recorder lost an accepted frame or accepted more PCM");
    passed &= require(exactSamples(readWave(path), expected), "Recorded samples, wrap order, channel order or headroom differ");

    const auto destructorPath = directory / "destructor.wav";
    const std::vector<float> tail{.0001F, -.0002F, .0003F, -.0004F};
    {
        StereoRecorder destructorDrain(destructorPath, 2);
        passed &= require(destructorDrain.push(tail), "Destructor-drain fixture was rejected");
    }
    passed &= require(exactSamples(readWave(destructorPath), tail), "Destructor failed to drain accepted PCM and close WAV");
    return passed;
}

bool terminalInputAndOverflow(const std::filesystem::path& directory)
{
    bool passed = true;
    const std::array<float, 3> incomplete{.0001F, .0002F, .0003F};
    {
        StereoRecorder recorder(directory / "incomplete.wav", 4);
        passed &= require(!recorder.push(incomplete) && recorder.failed() && !recorder.running() &&
                              !recorder.error().empty() && !recorder.push(std::array<float, 2>{}),
                          "Incomplete stereo input did not terminally stop recording");
        recorder.stop();
        passed &= require(recorder.acceptedFrames() == 0 && recorder.writtenFrames() == 0,
                          "Incomplete stereo input changed recording frame counts");
    }
    for (const auto value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        const auto name = std::isnan(value) ? "nan.wav" : "infinite.wav";
        StereoRecorder recorder(directory / name, 4);
        passed &= require(!recorder.push(std::array<float, 2>{value, 0}) && recorder.failed() &&
                              !recorder.running() && !recorder.error().empty(),
                          "Non-finite PCM did not terminally stop recording");
        recorder.stop();
        passed &= require(recorder.acceptedFrames() == 0 && recorder.writtenFrames() == 0,
                          "Non-finite PCM changed recording frame counts");
    }
    const auto path = directory / "overflow.wav";
    const std::vector<float> prefix{.0001F, -.0002F, .0003F, -.0004F};
    StereoRecorder recorder(path, 4);
    passed &= require(recorder.push(prefix), "Overflow test prefix was rejected");
    // Five frames can never fit this four-frame ring, independent of writer
    // scheduling. The rejected span must not be split or create a silent hole.
    passed &= require(!recorder.push(std::array<float, 10>{}) && recorder.failed() && !recorder.running() &&
                          recorder.overflowCount() == 1 && recorder.overflowFrames() == 5 &&
                          !recorder.error().empty() && !recorder.push(std::array<float, 2>{}),
                      "Recording overflow was not terminal and accounted");
    recorder.stop();
    passed &= require(recorder.acceptedFrames() == 2 && recorder.writtenFrames() == 2 &&
                          exactSamples(readWave(path), prefix),
                      "Overflow discarded accepted prefix data or silently appended a hole");
    return passed;
}

bool targetErrors(const std::filesystem::path& directory)
{
    const auto existing = directory / "existing.wav";
    { std::ofstream file(existing); file << "preserve-existing-target"; }
    bool passed = rejects([&] { StereoRecorder recorder(existing); }, "Recorder overwrote an existing target");
    std::ifstream file(existing);
    std::string contents;
    std::getline(file, contents);
    passed &= require(contents == "preserve-existing-target", "Existing recording target was modified");
    const auto link = directory / "symlink.wav";
    std::filesystem::create_symlink(existing, link);
    passed &= rejects([&] { StereoRecorder recorder(link); }, "Recorder followed an existing symlink target");
    passed &= rejects([&] { StereoRecorder recorder(directory); }, "Recorder accepted a directory target");
    passed &= rejects([&] { StereoRecorder recorder(directory / "missing" / "file.wav"); }, "Missing recording parent was accepted");
    passed &= rejects([&] { StereoRecorder recorder(directory / "zero.wav", 0); }, "Zero recording capacity was accepted");
    passed &= rejects([&] { StereoRecorder recorder(directory / "oversized.wav", StereoRecorder::maximumFrames + 1); },
                      "Unbounded recording capacity was accepted");
    passed &= require(!std::filesystem::exists(directory / "zero.wav") && !std::filesystem::exists(directory / "oversized.wav"),
                      "Invalid capacity created a recording file");
    return passed;
}

bool actualWriteFailure(const std::filesystem::path& directory)
{
    // Limit only this isolated child, including its writer thread. No host
    // filesystem quota, audio server or process-wide parent setting is changed.
    const auto child = fork();
    if (child < 0)
        throw std::runtime_error("Cannot fork recording write-failure test");
    if (!child) {
        try {
            std::signal(SIGXFSZ, SIG_IGN);
            struct rlimit limit{};
            if (getrlimit(RLIMIT_FSIZE, &limit) || limit.rlim_max < 2048)
                _exit(2);
            limit.rlim_cur = 2048;
            if (setrlimit(RLIMIT_FSIZE, &limit))
                _exit(3);
            StereoRecorder recorder(directory / "write-failure.wav", 1024);
            const std::vector<float> block(2048, .0001F);
            const bool accepted = recorder.push(block);
            recorder.stop();
            const bool passed = accepted && recorder.failed() && !recorder.running() && !recorder.error().empty() &&
                recorder.acceptedFrames() == 1024 && recorder.writtenFrames() < recorder.acceptedFrames() &&
                recorder.overflowCount() == 0 && !recorder.push(std::array<float, 2>{});
            if (!passed)
                std::cerr << "Actual writer failure was not reported: " << recorder.error() << '\n';
            _exit(passed ? 0 : 4);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            _exit(5);
        }
    }
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    return require(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "Isolated RLIMIT_FSIZE disk-write-failure test failed");
}
} // namespace

int main()
{
    try {
        TemporaryDirectory directory;
        const bool order = orderWrapAndDrain(directory.path);
        const bool terminal = terminalInputAndOverflow(directory.path);
        const bool targets = targetErrors(directory.path);
        const bool writeFailure = actualWriteFailure(directory.path);
        return order && terminal && targets && writeFailure ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

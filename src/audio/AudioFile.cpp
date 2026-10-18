// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioFile.hpp"
#include "audio/RenderCore.hpp"

#include <sndfile.h>
#include <samplerate.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace conflux::audio {
namespace {
class Descriptor {
public:
    explicit Descriptor(int value) noexcept : value_(value) {}
    ~Descriptor() { if (value_ >= 0) close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return value_; }
    int release() noexcept { return std::exchange(value_, -1); }
private:
    int value_;
};

int openSource(const std::filesystem::path& path)
{
    Descriptor source(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (source.get() < 0)
        throw std::runtime_error(std::string("Cannot open audio source: ") + std::strerror(errno));
    struct stat status{};
    if (fstat(source.get(), &status) != 0 || !S_ISREG(status.st_mode))
        throw std::invalid_argument("Audio input must be a regular local file");
    return source.release();
}

// Owned by the decode job. The library callback is never an audio callback.
struct DecodeInput {
    SNDFILE* file;
    sf_count_t remaining;
    int channels;
    std::stop_token stop;
    bool readFault = false, nonfinite = false;
    std::array<float, 4096 * RenderCore::channels> buffer{};

    static long read(void* context, float** data) noexcept
    {
        auto& self = *static_cast<DecodeInput*>(context);
        *data = self.buffer.data();
        if (self.stop.stop_requested() || !self.remaining || self.readFault || self.nonfinite)
            return 0;
        const auto wanted = std::min<sf_count_t>(self.remaining, 4096);
        const auto count = sf_readf_float(self.file, self.buffer.data(), wanted);
        if (count != wanted || sf_error(self.file) != SF_ERR_NO_ERROR) {
            self.readFault = true;
            return 0;
        }
        self.remaining -= count;
        self.nonfinite = std::any_of(self.buffer.begin(), self.buffer.begin() + count * self.channels,
                                    [](float sample) { return !std::isfinite(sample); });
        return self.nonfinite ? 0 : static_cast<long>(count);
    }
};
void checkStop(std::stop_token stop)
{
    if (stop.stop_requested())
        throw std::runtime_error("Audio preparation cancelled");
}

// Both in-memory probe input and disk-backed deck input use this conversion.
// Every codec/converter/sink call runs on the preparing owner, never in render.
template<class Sink>
std::uint64_t convertFile(int sourceDescriptor, std::size_t maximumBytes,
                          std::stop_token stop, Sink&& sink)
{
    constexpr auto frameBytes = RenderCore::channels * sizeof(float);
    if (maximumBytes < frameBytes)
        throw std::invalid_argument("Audio budget must hold at least one stereo frame");
    checkStop(stop);
    struct stat status{};
    if (fstat(sourceDescriptor, &status) != 0 || !S_ISREG(status.st_mode))
        throw std::invalid_argument("Audio input must be a regular local file");
    Descriptor decoderDescriptor(fcntl(sourceDescriptor, F_DUPFD_CLOEXEC, 0));
    if (decoderDescriptor.get() < 0 || lseek(decoderDescriptor.get(), 0, SEEK_SET) != 0)
        throw std::runtime_error("Cannot rewind the audio source descriptor");
    SF_INFO info{};
    // The duplicate shares the caller's offset. Source hashing uses pread;
    // conversion has exclusive worker ownership and explicitly rewinds above.
    std::unique_ptr<SNDFILE, decltype(&sf_close)> file(
        sf_open_fd(decoderDescriptor.get(), SFM_READ, &info, SF_FALSE), sf_close);
    if (!file)
        throw std::runtime_error(std::string("Cannot open audio file: ") + sf_strerror(nullptr));
    if (info.samplerate < 8000 || info.samplerate > 192000 || (info.channels != 1 && info.channels != 2))
        throw std::invalid_argument("Audio file must be mono or stereo at 8000 to 192000 Hz");
    if (info.frames <= 0)
        throw std::runtime_error("Audio file contains no frames");
    const auto maximum = static_cast<std::uint64_t>(maximumBytes / frameBytes);
    const auto frames = static_cast<std::uint64_t>(info.frames);
    const auto rate = static_cast<std::uint64_t>(info.samplerate);
    const auto seconds = frames / rate;
    const auto fractional = ((frames % rate) * RenderCore::sampleRate + rate - 1) / rate;
    if (seconds > maximum / RenderCore::sampleRate || fractional > maximum - seconds * RenderCore::sampleRate)
        throw std::length_error("Audio file exceeds the prepared PCM byte budget");
    DecodeInput input{file.get(), info.frames, info.channels, stop};
    std::uint64_t produced = 0;
    auto emit = [&](float* data, std::size_t count) {
        checkStop(stop);
        if (count > maximum - produced)
            throw std::length_error("Audio file exceeds the prepared PCM byte budget");
        if (info.channels == 1)
            for (auto frame = count; frame > 0; --frame) {
                const auto value = data[frame - 1];
                data[(frame - 1) * 2] = data[(frame - 1) * 2 + 1] = value;
            }
        const std::span<const float> block(data, count * RenderCore::channels);
        if (std::ranges::any_of(block, [](float value) { return !std::isfinite(value); }))
            throw std::runtime_error("Decoded audio contains non-finite samples");
        sink(block);
        produced += count;
    };
    if (info.samplerate == static_cast<int>(RenderCore::sampleRate)) {
        float* block = nullptr;
        while (const auto count = DecodeInput::read(&input, &block))
            emit(block, static_cast<std::size_t>(count));
    } else {
        int error = 0;
        std::unique_ptr<SRC_STATE, decltype(&src_delete)> converter(
            src_callback_new(DecodeInput::read, SRC_SINC_BEST_QUALITY, info.channels, &error, &input), src_delete);
        if (!converter)
            throw std::runtime_error(std::string("Cannot create sample-rate converter: ") + src_strerror(error));
        std::array<float, 4096 * RenderCore::channels> output{};
        const double ratio = double(RenderCore::sampleRate) / info.samplerate;
        while (const auto count = src_callback_read(converter.get(), ratio, 4096, output.data()))
            emit(output.data(), static_cast<std::size_t>(count));
        if (const auto fault = src_error(converter.get()))
            throw std::runtime_error(std::string("Sample-rate conversion failed: ") + src_strerror(fault));
    }
    checkStop(stop);
    if (input.nonfinite)
        throw std::runtime_error("Decoded audio contains non-finite samples");
    if (input.readFault || input.remaining || sf_error(file.get()) != SF_ERR_NO_ERROR)
        throw std::runtime_error(std::string("Could not decode every audio frame: ") + sf_strerror(file.get()));
    if (sf_close(file.release()) != 0)
        throw std::runtime_error("Could not close decoded audio file");
    if (!produced)
        throw std::runtime_error("Sample-rate conversion produced no audio frames");
    return produced;
}
} // namespace

DecodedAudio::DecodedAudio(std::vector<float> samples) noexcept : samples_(std::move(samples)) {}

std::uint64_t DecodedAudio::frames() const noexcept
{
    return samples_.size() / RenderCore::channels;
}

DecodedAudio decodeAudioFile(const std::filesystem::path& path, std::size_t maxDecodedBytes)
{
    Descriptor source(openSource(path));
    std::vector<float> samples;
    const auto maximumSamples = std::min(maxDecodedBytes / sizeof(float), samples.max_size()) / 2 * 2;
    convertFile(source.get(), maxDecodedBytes, {}, [&](std::span<const float> block) {
        const auto needed = samples.size() + block.size();
        if (needed > maximumSamples)
            throw std::length_error("Audio file exceeds the decoded PCM byte budget");
        if (needed > samples.capacity()) {
            const auto growth = samples.capacity() > maximumSamples / 2 ? maximumSamples : samples.capacity() * 2;
            samples.reserve(std::max(needed, growth));
        }
        samples.insert(samples.end(), block.begin(), block.end());
    });
    return DecodedAudio(std::move(samples));
}

PreparedAudio::PreparedAudio(int descriptor) noexcept : descriptor_(descriptor) {}
PreparedAudio::~PreparedAudio() { if (descriptor_ >= 0) close(descriptor_); }
PreparedAudio::PreparedAudio(PreparedAudio&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)), frames_(other.frames_), peaks_(std::move(other.peaks_)) {}
PreparedAudio& PreparedAudio::operator=(PreparedAudio&& other) noexcept
{
    if (this != &other) {
        if (descriptor_ >= 0) close(descriptor_);
        descriptor_ = std::exchange(other.descriptor_, -1);
        frames_ = other.frames_;
        peaks_ = std::move(other.peaks_);
    }
    return *this;
}

PreparedAudio prepareAudioFile(const std::filesystem::path& path, std::size_t maximumBytes, std::stop_token stop,
                               const std::filesystem::path& spoolDirectory)
{
    checkStop(stop);
    Descriptor source(openSource(path));
    return prepareAudioFile(source.get(), maximumBytes, stop, spoolDirectory);
}

PreparedAudio prepareAudioFile(int sourceDescriptor, std::size_t maximumBytes, std::stop_token stop,
                               const std::filesystem::path& spoolDirectory)
{
    checkStop(stop);
    if (maximumBytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        throw std::invalid_argument("Prepared PCM budget exceeds the addressable file size");
    const auto directory = spoolDirectory.empty() ? std::filesystem::temp_directory_path() : spoolDirectory;
    if (!spoolDirectory.empty()) std::filesystem::create_directories(directory);
    auto pattern = (directory / "conflux-pcm-XXXXXX").string();
    const int fd = mkostemp(pattern.data(), O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error(std::string("Cannot create prepared PCM file: ") + std::strerror(errno));
    PreparedAudio result(fd);
    // Unlink immediately: the owned descriptor keeps the file alive, and a
    // crash/close cannot leave a named cache file containing the user's music.
    if (unlink(pattern.c_str()) < 0)
        throw std::runtime_error("Cannot unlink temporary prepared PCM file");
    result.frames_ = convertFile(sourceDescriptor, maximumBytes, stop, [&](std::span<const float> block) {
        auto* bytes = reinterpret_cast<const char*>(block.data());
        auto remaining = block.size_bytes();
        while (remaining) {
            checkStop(stop);
            const auto count = write(fd, bytes, remaining);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0)
                throw std::runtime_error(std::string("Cannot write prepared PCM: ") + std::strerror(errno));
            bytes += count;
            remaining -= static_cast<std::size_t>(count);
        }
    });
    const auto bins = std::min<std::uint64_t>(512, result.frames_);
    result.peaks_.assign(bins, 0.F);
    std::array<float, 4096 * RenderCore::channels> block{};
    float maximum = 0;
    for (std::uint64_t position = 0; position < result.frames_;) {
        checkStop(stop);
        const auto count = std::min<std::uint64_t>(4096, result.frames_ - position);
        const auto bytes = count * RenderCore::channels * sizeof(float);
        std::size_t received = 0;
        while (received < bytes) {
            checkStop(stop);
            const auto read = pread(fd, reinterpret_cast<char*>(block.data()) + received, bytes - received,
                                    static_cast<off_t>(position * RenderCore::channels * sizeof(float) + received));
            if (read < 0 && errno == EINTR) continue;
            if (read <= 0)
                throw std::runtime_error("Cannot read every prepared PCM frame for the overview");
            received += static_cast<std::size_t>(read);
        }
        for (std::uint64_t frame = 0; frame < count; ++frame) {
            const auto peak = std::max(std::abs(block[frame * 2]), std::abs(block[frame * 2 + 1]));
            auto& bin = result.peaks_[(position + frame) * bins / result.frames_];
            bin = std::max(bin, peak);
            maximum = std::max(maximum, peak);
        }
        position += count;
    }
    if (maximum > 0)
        for (auto& peak : result.peaks_) peak /= maximum;
    return result;
}

PreparedAudio reopenPreparedAudio(int pcmDescriptor, std::uint64_t frames,
                                 std::span<const float> peaks, std::size_t maximumBytes)
{
    constexpr auto frameBytes = RenderCore::channels * sizeof(float);
    if (!frames || frames > maximumBytes / frameBytes ||
        frames > std::uint64_t(std::numeric_limits<off_t>::max()) / frameBytes)
        throw std::invalid_argument("Prepared PCM size exceeds its frame budget");
    struct stat status{};
    const auto flags = fcntl(pcmDescriptor, F_GETFL);
    if (fstat(pcmDescriptor, &status) != 0 || !S_ISREG(status.st_mode) || flags < 0 ||
        (flags & O_ACCMODE) != O_RDONLY)
        throw std::invalid_argument("Prepared PCM must be a read-only regular file");
    if (status.st_size < 0 || std::uint64_t(status.st_size) != frames * frameBytes)
        throw std::invalid_argument("Prepared PCM file size differs from its advertised frames");
    if (peaks.size() != std::min<std::uint64_t>(512, frames) ||
        std::ranges::any_of(peaks, [](float value) { return !std::isfinite(value) || value < 0 || value > 1; }))
        throw std::invalid_argument("Prepared PCM overview has invalid geometry or non-finite values");
    Descriptor owned(fcntl(pcmDescriptor, F_DUPFD_CLOEXEC, 0));
    if (owned.get() < 0)
        throw std::runtime_error("Cannot duplicate prepared PCM descriptor");
    PreparedAudio result(owned.release());
    result.frames_ = frames;
    result.peaks_.assign(peaks.begin(), peaks.end());
    return result;
}

} // namespace conflux::audio

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioFile.hpp"
#include "audio/RenderCore.hpp"

#include <sndfile.h>
#include <samplerate.h>

#include <algorithm>
#include <array>
#include <bit>
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
                          std::stop_token stop, Sink&& sink,
                          int targetRate = RenderCore::sampleRate, const SF_INFO* rawMetadata = nullptr)
{
    constexpr auto frameBytes = RenderCore::channels * sizeof(float);
    if (maximumBytes < frameBytes)
        throw std::invalid_argument("Audio budget must hold at least one stereo frame");
    checkStop(stop);
    struct stat status{};
    if (fstat(sourceDescriptor, &status) != 0 || !S_ISREG(status.st_mode))
        throw std::invalid_argument("Audio input must be a regular local file");
    const auto access = fcntl(sourceDescriptor, F_GETFL);
    if (access < 0 || (access & O_ACCMODE) == O_WRONLY)
        throw std::invalid_argument("Audio input must be readable");
    Descriptor decoderDescriptor(fcntl(sourceDescriptor, F_DUPFD_CLOEXEC, 0));
    if (decoderDescriptor.get() < 0 || lseek(decoderDescriptor.get(), 0, SEEK_SET) != 0)
        throw std::runtime_error("Cannot rewind the audio source descriptor");
    SF_INFO info = rawMetadata ? *rawMetadata : SF_INFO{};
    // The duplicate shares the caller's offset. Source hashing uses pread;
    // conversion has exclusive worker ownership and explicitly rewinds above.
    std::unique_ptr<SNDFILE, decltype(&sf_close)> file(
        sf_open_fd(decoderDescriptor.get(), SFM_READ, &info, SF_FALSE), sf_close);
    if (!file)
        throw std::runtime_error(std::string("Cannot open audio file: ") + sf_strerror(nullptr));
    if (rawMetadata && (info.frames != rawMetadata->frames || info.samplerate != rawMetadata->samplerate ||
                        info.channels != rawMetadata->channels))
        throw std::invalid_argument("Raw stem PCM geometry differs from its declared frames");
    if (info.samplerate < 8000 || info.samplerate > 192000 || (info.channels != 1 && info.channels != 2))
        throw std::invalid_argument("Audio file must be mono or stereo at 8000 to 192000 Hz");
    if (info.frames <= 0)
        throw std::runtime_error("Audio file contains no frames");
    const auto maximum = static_cast<std::uint64_t>(maximumBytes / frameBytes);
    const auto frames = static_cast<std::uint64_t>(info.frames);
    const auto rate = static_cast<std::uint64_t>(info.samplerate);
    const auto seconds = frames / rate;
    const auto outputRate = static_cast<std::uint64_t>(targetRate);
    const auto fractional = ((frames % rate) * outputRate + rate - 1) / rate;
    if (seconds > maximum / outputRate || fractional > maximum - seconds * outputRate)
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
    if (info.samplerate == targetRate) {
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
        const double ratio = double(targetRate) / info.samplerate;
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

int anonymousPcm(std::size_t maximumBytes, std::stop_token stop,
                 const std::filesystem::path& spoolDirectory)
{
    checkStop(stop);
    if (maximumBytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        throw std::invalid_argument("Prepared PCM budget exceeds the addressable file size");
    const auto directory = spoolDirectory.empty() ? std::filesystem::temp_directory_path() : spoolDirectory;
    if (!spoolDirectory.empty()) std::filesystem::create_directories(directory);
    auto pattern = (directory / "conflux-pcm-XXXXXX").string();
    Descriptor result(mkostemp(pattern.data(), O_CLOEXEC));
    if (result.get() < 0)
        throw std::runtime_error(std::string("Cannot create prepared PCM file: ") + std::strerror(errno));
    // Keep user media only through owned FDs, including all failure paths.
    if (unlink(pattern.c_str()) < 0)
        throw std::runtime_error("Cannot unlink temporary prepared PCM file");
    return result.release();
}

void writePcm(int fd, std::span<const float> block, std::stop_token stop, bool littleEndian = false)
{
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    [[maybe_unused]] std::array<char, 4096 * RenderCore::channels * sizeof(float)> encoded;
    const auto* bytes = reinterpret_cast<const char*>(block.data());
    if constexpr (std::endian::native != std::endian::little) {
        if (littleEndian) {
            if (block.size_bytes() > encoded.size())
                throw std::length_error("Prepared PCM block exceeds bounded encoding scratch");
            for (std::size_t sample = 0; sample < block.size(); ++sample) {
                const auto bits = std::bit_cast<std::uint32_t>(block[sample]);
                for (std::size_t byte = 0; byte < sizeof(float); ++byte)
                    encoded[sample * sizeof(float) + byte] = static_cast<char>((bits >> (byte * 8)) & 255);
            }
            bytes = encoded.data();
        }
    } else {
        (void)littleEndian;
    }
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
}

std::vector<float> pcmOverview(int fd, std::uint64_t frames, std::stop_token stop)
{
    const auto bins = std::min<std::uint64_t>(512, frames);
    std::vector<float> peaks(bins, 0.F);
    std::array<float, 4096 * RenderCore::channels> block{};
    float maximum = 0;
    for (std::uint64_t position = 0; position < frames;) {
        checkStop(stop);
        const auto count = std::min<std::uint64_t>(4096, frames - position);
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
            auto& bin = peaks[(position + frame) * bins / frames];
            bin = std::max(bin, peak);
            maximum = std::max(maximum, peak);
        }
        position += count;
    }
    if (maximum > 0)
        for (auto& peak : peaks) peak /= maximum;
    return peaks;
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

SignalLevels measurePreparedLevels(const PreparedAudio& audio, std::stop_token stop)
{
    checkStop(stop);
    constexpr std::uint64_t frameBytes = RenderCore::channels * sizeof(float);
    const auto descriptor = audio.descriptor();
    struct stat before{};
    const auto flags = fcntl(descriptor, F_GETFL);
    if (fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || flags < 0 ||
        (flags & O_ACCMODE) == O_WRONLY)
        throw std::invalid_argument("Prepared levels require a readable regular PCM file");
    const auto frames = audio.frames();
    if (!frames || frames > std::uint64_t(std::numeric_limits<off_t>::max()) / frameBytes ||
        before.st_size < 0 || std::uint64_t(before.st_size) != frames * frameBytes)
        throw std::invalid_argument("Prepared level PCM geometry differs from its declared frames");

    std::array<float, 4096 * RenderCore::channels> block{};
    double sumSquares = 0, correction = 0, peak = 0;
    for (std::uint64_t position = 0; position < frames;) {
        checkStop(stop);
        const auto count = std::min<std::uint64_t>(4096, frames - position);
        const auto bytes = count * frameBytes;
        std::size_t received = 0;
        while (received < bytes) {
            checkStop(stop);
            const auto read = pread(descriptor, reinterpret_cast<char*>(block.data()) + received,
                                    bytes - received, static_cast<off_t>(position * frameBytes + received));
            if (read < 0 && errno == EINTR) continue;
            if (read <= 0)
                throw std::runtime_error("Cannot read every prepared PCM frame for levels");
            received += static_cast<std::size_t>(read);
        }
        for (std::size_t sample = 0; sample < count * RenderCore::channels; ++sample) {
            if (sample % 256 == 0) checkStop(stop);
            const double value = block[sample];
            if (!std::isfinite(value))
                throw std::runtime_error("Prepared PCM levels contain non-finite samples");
            peak = std::max(peak, std::abs(value));
            // Compensated double accumulation preserves small contributions
            // without squaring finite float headroom in float precision.
            const double term = value * value - correction;
            const double next = sumSquares + term;
            correction = (next - sumSquares) - term;
            sumSquares = next;
        }
        position += count;
    }
    struct stat after{};
    if (fstat(descriptor, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        throw std::runtime_error("Prepared PCM changed while measuring levels");
    // A final rounding bit must not produce RMS greater than the sample peak.
    const SignalLevels result{std::min(peak, std::sqrt(sumSquares / (double(frames) * RenderCore::channels))), peak};
    checkStop(stop);
    return result;
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
    PreparedAudio result(anonymousPcm(maximumBytes, stop, spoolDirectory));
    result.frames_ = convertFile(sourceDescriptor, maximumBytes, stop, [&](std::span<const float> block) {
        writePcm(result.descriptor_, block, stop);
    });
    result.peaks_ = pcmOverview(result.descriptor_, result.frames_, stop);
    return result;
}

PreparedStemInput::PreparedStemInput(int descriptor) noexcept : descriptor_(descriptor) {}
PreparedStemInput::~PreparedStemInput() { if (descriptor_ >= 0) close(descriptor_); }
PreparedStemInput::PreparedStemInput(PreparedStemInput&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)), frames_(std::exchange(other.frames_, 0)) {}
PreparedStemInput& PreparedStemInput::operator=(PreparedStemInput&& other) noexcept
{
    if (this != &other) {
        if (descriptor_ >= 0) close(descriptor_);
        descriptor_ = std::exchange(other.descriptor_, -1);
        frames_ = std::exchange(other.frames_, 0);
    }
    return *this;
}

PreparedStemInput prepareStemInput(int sourceDescriptor, std::size_t maximumBytes, std::stop_token stop,
                                   const std::filesystem::path& spoolDirectory)
{
    PreparedStemInput result(anonymousPcm(maximumBytes, stop, spoolDirectory));
    result.frames_ = convertFile(sourceDescriptor, maximumBytes, stop, [&](std::span<const float> block) {
        writePcm(result.descriptor_, block, stop, true);
    }, PreparedStemInput::sampleRate);
    return result;
}

PreparedAudio prepareStemOutput(int rawDescriptor, std::uint64_t frames, std::size_t maximumBytes,
                                std::stop_token stop, const std::filesystem::path& spoolDirectory)
{
    checkStop(stop);
    constexpr std::uint64_t frameBytes = 2 * sizeof(float);
    struct stat status{};
    const auto access = fcntl(rawDescriptor, F_GETFL);
    if (fstat(rawDescriptor, &status) != 0 || !S_ISREG(status.st_mode))
        throw std::invalid_argument("Raw stem PCM must be a regular local file");
    if (access < 0 || (access & O_ACCMODE) == O_WRONLY)
        throw std::invalid_argument("Raw stem PCM must be readable");
    if (!frames || frames > std::uint64_t(std::numeric_limits<off_t>::max()) / frameBytes ||
        frames > std::uint64_t(std::numeric_limits<sf_count_t>::max()) || status.st_size < 0 ||
        std::uint64_t(status.st_size) != frames * frameBytes)
        throw std::invalid_argument("Raw stem PCM geometry differs from its declared frames");
    SF_INFO info{};
    info.frames = static_cast<sf_count_t>(frames);
    info.channels = 2;
    info.samplerate = PreparedStemInput::sampleRate;
    info.format = SF_FORMAT_RAW | SF_FORMAT_FLOAT | SF_ENDIAN_LITTLE;
    PreparedAudio result(anonymousPcm(maximumBytes, stop, spoolDirectory));
    result.frames_ = convertFile(rawDescriptor, maximumBytes, stop, [&](std::span<const float> block) {
        writePcm(result.descriptor_, block, stop);
    }, RenderCore::sampleRate, &info);
    result.peaks_ = pcmOverview(result.descriptor_, result.frames_, stop);
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

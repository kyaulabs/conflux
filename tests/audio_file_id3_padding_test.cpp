// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioFile.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdlib>
#include <stdexcept>
#include <stop_token>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
using Bytes = std::vector<unsigned char>;
Bytes read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot read generated fixture");
    return Bytes(std::istreambuf_iterator<char>(input), {});
}
void write(const std::filesystem::path& path, const Bytes& bytes)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.close();
    require(bool(output), "Cannot write tagged fixture");
}
Bytes tagged(const Bytes& audio, unsigned char version, std::size_t padding, bool footer = false)
{
    Bytes body;
    if (version == 2)
        body = {'T','T','2',0,0,5,0,'T','o','n','e'};
    else
        body = {'T','I','T','2',0,0,0,5,0,0,0,'T','o','n','e'};
    Bytes result{'I','D','3',version,0,static_cast<unsigned char>(footer ? 0x10 : 0),
                 0,0,0,static_cast<unsigned char>(body.size())};
    const Bytes header = result;
    result.insert(result.end(), body.begin(), body.end());
    if (footer) {
        auto end = header;
        end[0] = '3'; end[1] = 'D'; end[2] = 'I';
        result.insert(result.end(), end.begin(), end.end());
    }
    result.insert(result.end(), padding, 0);
    result.insert(result.end(), audio.begin(), audio.end());
    return result;
}
void samePcm(const conflux::audio::PreparedAudio& actual,
             const conflux::audio::DecodedAudio& expected)
{
    std::vector<float> samples(expected.samples().size());
    require(actual.frames() == expected.frames() &&
        pread(actual.descriptor(), samples.data(), samples.size() * sizeof(float), 0) ==
            static_cast<ssize_t>(samples.size() * sizeof(float)) &&
        std::ranges::equal(samples, expected.samples()),
        "Tagged preparation lost samples, shifted timing or changed channels");
}
}

int main(int argc, char** argv)
{
    using namespace conflux::audio;
    std::filesystem::path directory;
    try {
        require(argc == 2, "Expected generated MPEG sine fixture");
        auto pattern = (std::filesystem::temp_directory_path() / "conflux-id3-test-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        require(created, "Cannot create tagged-MPEG test directory");
        directory = created;
        // Generated stereo sine fixture: 5760 frames at 48 kHz, L=997 Hz/.21,
        // R=1733 Hz/.13 with .4-radian phase; LAME 128k, no Xing/ID3 tags.
        // The test compares every decoded sample, without an encoder dependency.
        const auto audio = read(argv[1]);
        const auto baseline = decodeAudioFile(argv[1]);
        require(baseline.frames() > 1000 &&
            std::ranges::any_of(baseline.samples(), [](float x) { return x > .01F; }),
            "Generated MPEG baseline has no tone");
        const auto path = directory / "source.without-mp3-suffix";
        unsigned positives = 0, negatives = 0;
        for (const unsigned char version : {2, 3, 4}) {
            for (const std::size_t gap : {0, 1, 417, 835, 4096}) {
                const auto input = tagged(audio, version, gap);
                write(path, input);
                const auto decoded = decodeAudioFile(path);
                require(decoded.frames() == baseline.frames() &&
                    std::ranges::equal(decoded.samples(), baseline.samples()),
                    "ID3/padding changed decoded frame count or PCM");
                const auto fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
                require(fd >= 0, "Cannot pin tagged source descriptor");
                // A replaced pathname must not redirect the already-open job.
                const auto renamed = directory / "pinned-source";
                std::filesystem::rename(path, renamed);
                write(path, Bytes{'n','o','t',' ','a','u','d','i','o'});
                try {
                    samePcm(prepareAudioFile(fd, baseline.samples().size_bytes(), {}, directory), baseline);
                    require(fcntl(fd, F_GETFD) >= 0 && read(renamed) == input,
                            "Borrowed source was closed or rewritten");
                    close(fd);
                } catch (...) { close(fd); throw; }
                std::filesystem::remove(renamed);
                ++positives;
            }
        }
        for (const std::size_t gap : {1, 835}) {
            write(path, tagged(audio, 4, gap, true));
            samePcm(prepareAudioFile(path, baseline.samples().size_bytes(), {}, directory), baseline);
            ++positives;
        }
        std::vector<Bytes> invalid;
        invalid.push_back(tagged(audio, 3, 4097)); // Bound, rather than searching arbitrary leading data.
        invalid.push_back(tagged(audio, 3, 417)); invalid.back()[25 + 200] = 1;
        invalid.push_back(tagged(audio, 3, 417)); invalid.back()[6] = 0x80;
        invalid.push_back(tagged(audio, 3, 417)); invalid.back()[5] = 1;
        invalid.push_back(tagged(audio, 3, 417)); invalid.back()[3] = 5;
        invalid.push_back(tagged(audio, 3, 417)); invalid.back()[4] = 255;
        invalid.push_back(tagged(audio, 3, 417)); invalid.back()[6] = 0x7f;
        invalid.push_back(tagged(audio, 3, 417)); invalid.back()[25 + 417 + 1] = 0xe8;
        invalid.push_back(tagged(audio, 3, 417)); invalid.back()[25 + 417 + 1] &= 0xf9;
        invalid.push_back(tagged(audio, 3, 417)); invalid.back()[25 + 417 + 2] = 0xff;
        invalid.push_back(tagged(audio, 3, 417)); invalid.back().resize(25 + 417 + 3);
        invalid.push_back(tagged(audio, 4, 417, true)); invalid.back()[25] = 'X';
        invalid.push_back(tagged(audio, 4, 417, true)); invalid.back()[25 + 3] = 3;
        for (const auto& bytes : invalid) {
            write(path, bytes);
            bool refused = false;
            try { (void)prepareAudioFile(path, 1048576, {}, directory); }
            catch (const std::exception&) { refused = true; }
            require(refused, "Malformed tag, nonzero junk or over-bound padding was accepted");
            ++negatives;
        }
        write(path, tagged(audio, 3, 417));
        bool refusedBudget = false, refusedCancellation = false;
        try { (void)prepareAudioFile(path, baseline.samples().size_bytes() - 8, {}, directory); }
        catch (const std::length_error&) { refusedBudget = true; }
        std::stop_source stop; stop.request_stop();
        try { (void)prepareAudioFile(path, 1048576, stop.get_token(), directory); }
        catch (const std::exception&) { refusedCancellation = true; }
        require(refusedBudget && refusedCancellation, "Tagged fallback bypassed budget or cancellation");
        std::cout << positives << " complete PCM/FD-identity cases and " << negatives
                  << " malformed/bound cases passed; output budget and cancellation retained.\n";
        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!directory.empty()) std::filesystem::remove_all(directory);
        return 1;
    }
}

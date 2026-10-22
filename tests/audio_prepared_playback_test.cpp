// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/RenderCore.hpp"
#include "audio/SourceHandoff.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace conflux::audio;
using namespace std::chrono_literals;
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

struct CloseFile { void operator()(std::FILE* file) const { if (file) std::fclose(file); } };
using File = std::unique_ptr<std::FILE, CloseFile>;
File rawFile(std::span<const float> samples)
{
    File file(std::tmpfile());
    require(bool(file), "Cannot create temporary PCM");
    require(std::fwrite(samples.data(), sizeof(float), samples.size(), file.get()) == samples.size() &&
            std::fflush(file.get()) == 0, "Cannot write temporary PCM");
    return file;
}
std::size_t descriptors()
{
    return std::size_t(std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                                     std::filesystem::directory_iterator{}));
}
bool same(std::span<const float> first, std::span<const float> second)
{
    return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin(),
        [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); });
}
struct Fixture {
    static constexpr std::size_t frames = 3007;
    std::array<std::vector<float>, 4> samples;
    std::array<File, 4> files;
    std::vector<float> sum = std::vector<float>(frames * 2);
    Fixture()
    {
        for (std::size_t stem = 0; stem < 4; ++stem) {
            samples[stem].resize(frames * 2);
            for (std::size_t sample = 0; sample < frames * 2; ++sample) {
                samples[stem][sample] = float(int((sample * (stem + 3) + stem * 7) % 67) - 33) / 32.F;
                sum[sample] += samples[stem][sample]; // Exact binary fractions; no rounding ambiguity.
            }
            files[stem] = rawFile(samples[stem]);
        }
    }
    std::array<int, 4> fds() const
    { return {fileno(files[0].get()), fileno(files[1].get()), fileno(files[2].get()), fileno(files[3].get())}; }
};
template<class Function>
void rejects(Function&& function, const char* message)
{
    try { function(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

void sumBusesAndBorrowedDescriptors()
{
    Fixture fixture;
    PreparedPlayback playback(fixture.fds(), Fixture::frames);
    for (auto& file : fixture.files) file.reset();
    require(playback.frames() == Fixture::frames && playback.prime(0, 1s), "Cannot prime all independent duplicate FDs");
    require(!playback.errorCode() && playback.error().empty(), "Healthy bank reports an error");
    RenderCore actual(playback.sourceRef()), expected(fixture.sum);
    for (auto* core : {&actual, &expected})
        require(core->setDeckGain(0, .5F) && core->setMasterGain(.75F) &&
                core->setCueSelected(0, true) && core->setCueGain(.25F), "Cannot configure shared buses");
    const auto frames = Fixture::frames + 31;
    std::vector<float> master(frames * 2), cue(frames * 2), wantedMaster(frames * 2), wantedCue(frames * 2);
    require(expected.renderBuses(wantedMaster, wantedCue), "Independent reference render failed");
    const std::array<std::size_t, 5> partitions{1, 7, 256, 1024, 511};
    for (std::size_t first = 0, part = 0; first < frames; ++part) {
        const auto count = std::min(partitions[part % partitions.size()], frames - first);
        require(actual.renderBuses(std::span(master).subspan(first * 2, count * 2),
                                   std::span(cue).subspan(first * 2, count * 2)), "Prepared bus render failed");
        first += count;
    }
    require(same(master, wantedMaster) && same(cue, wantedCue), "Prepared stem sum changed master/cue or render partitions");
    require(actual.frameClock() == frames && actual.sourceFrame() == Fixture::frames &&
            !actual.readFailed() && !actual.starvedFrames() && !playback.errorCode(),
            "Prepared bank did not share one cursor/EOF or reported a false shortage");
    auto normalFile = rawFile(fixture.sum);
    PreparedPlayback normal(fileno(normalFile.get()), Fixture::frames);
    normalFile.reset();
    require(normal.prime(0, 1s), "Normal prepared owner lost its duplicated descriptor");
    RenderCore normalCore(normal.sourceRef());
    std::array<float, 128> normalOutput{};
    require(normalCore.render(normalOutput) && same(normalOutput, std::span(fixture.sum).first(normalOutput.size())) &&
            !normal.errorCode(), "Normal single-source owner changed exact legacy samples");
}

void coherentPrimeSeekAndStop()
{
    Fixture fixture;
    PreparedPlayback playback(fixture.fds(), Fixture::frames);
    require(playback.prime(1001, 1s), "Cannot prime common nonzero position");
    RenderCore actual(playback.sourceRef()), expected(fixture.sum);
    require(actual.seek(1001) && expected.seek(1001), "Initial common seek failed");
    std::array<float, 514> output{}, wanted{};
    require(actual.render(output) && expected.render(wanted) && same(output, wanted), "Initial bank offset differs");
    require(actual.seek(9) && expected.seek(9) && playback.prime(9, 1s), "Common backward prime/seek failed");
    require(actual.render(output) && expected.render(wanted) && same(output, wanted), "Backward seek mixed stale stem epochs");
    require(!playback.prime(0, -1ms) && playback.errorCode() == EINVAL,
            "Negative shared priming budget was accepted");
    const auto began = std::chrono::steady_clock::now();
    const bool immediate = playback.prime(2000, 0ms);
    require(std::chrono::steady_clock::now() - began < 200ms &&
            (immediate ? !playback.errorCode() : playback.errorCode() == ETIMEDOUT),
            "Zero shared budget blocked or reported an unrelated error");
    require(actual.seek(2000) && expected.seek(2000) && playback.prime(2000, 1s) &&
            actual.render(output) && expected.render(wanted) && same(output, wanted),
            "A bounded prime retry did not restore coherent samples");
    require(actual.seek(Fixture::frames) && playback.prime(Fixture::frames, 1s) && actual.render(output) &&
            std::ranges::all_of(output, [](float sample) { return sample == 0.F; }) &&
            actual.sourceFrame() == Fixture::frames, "Common EOF was mistaken for missing data");
    playback.stop(); playback.stop();
    require(!playback.prime(0, 1s) && playback.errorCode() == ECANCELED,
            "Stopped whole owner accepted new priming");
}

void failureInAnyReaderAndConstructionCleanup()
{
    Fixture fixture;
    const auto before = descriptors();
    rejects([&] { PreparedPlayback invalid(fixture.fds(), 0); }, "Empty bank geometry accepted");
    rejects([&] { PreparedPlayback invalid(fixture.fds(), std::numeric_limits<std::uint64_t>::max()); },
            "Unaddressable common geometry accepted");
    auto bad = fixture.fds(); bad[3] = -1;
    rejects([&] { PreparedPlayback invalid(bad, Fixture::frames); }, "Invalid fourth FD accepted");
    require(descriptors() == before, "Partial bank construction leaked earlier readers/FDs");
    const auto path = "/proc/self/fd/" + std::to_string(fileno(fixture.files[3].get()));
    const int writeOnly = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    require(writeOnly >= 0, "Cannot create private write-only descriptor");
    bad = fixture.fds(); bad[3] = writeOnly;
    rejects([&] { PreparedPlayback invalid(bad, Fixture::frames); }, "Unreadable fourth descriptor accepted");
    close(writeOnly);
    require(descriptors() == before, "Unreadable descriptor failure leaked a partial bank");
    for (const auto adjustment : {-4, 8}) {
        Fixture malformed;
        require(ftruncate(fileno(malformed.files[3].get()),
                          off_t(Fixture::frames * 2 * sizeof(float)) + adjustment) == 0,
                "Cannot create malformed exact-size fixture");
        rejects([&] { PreparedPlayback invalid(malformed.fds(), Fixture::frames); },
                "A partial stereo frame or extra frame was accepted as an aligned bank");
    }
    for (std::size_t broken = 0; broken < 4; ++broken) {
        Fixture failing;
        PreparedPlayback playback(failing.fds(), Fixture::frames);
        require(playback.prime(0, 1s), "Cannot prime healthy failure fixture");
        require(ftruncate(fileno(failing.files[broken].get()), 0) == 0, "Cannot truncate one private stem");
        auto source = playback.sourceRef();
        source.discontinuity(source.context, 17);
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!playback.errorCode() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        require(playback.errorCode() != 0 && playback.error().find("Stem " + std::to_string(broken)) != std::string::npos,
                "Aggregate health missed a nonrepresentative reader failure");
        std::array<float, 16> output{};
        const auto read = source.read(source.context, 17, output);
        require(read.failed && !read.frames, "A failed stem bank silently returned an incomplete mix");
    }
}

void preparedLiveStemRamps()
{
    Fixture fixture;
    PreparedPlayback bank(fixture.fds(), Fixture::frames);
    require(bank.prime(0, 1s), "Cannot prime prepared gain fixture");
    RenderCore core(bank.sourceRef());
    require(core.isStemBank() && bank.configureStemRamps(4), "Prepared bank cannot configure live gain smoothing");
    for (std::size_t stem = 0; stem < 4; ++stem)
        require(core.setStemGain(0, stem, 0.F), "Cannot queue a prepared bank gain target");
    std::array<float, 8> output{};
    require(core.render(output) && core.sourceFrame() == 4 && !core.isBuffering(), "Prepared gain block did not render");
    for (std::size_t frame = 0; frame < 4; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(output[frame * 2 + channel] == fixture.sum[frame * 2 + channel] * float(3 - frame) / 4.F,
                    "Prepared bank changed gain abruptly or advanced its ramp independently of source frames");
    require(!bank.configureStemRamps(0), "Prepared ramp duration changed after consumption began");
    auto normalFile = rawFile(fixture.sum);
    PreparedPlayback normal(fileno(normalFile.get()), Fixture::frames);
    require(normal.configureStemRamps(240) && !normal.configureStemRamps(48001),
            "Normal playback ramp no-op changed its configuration bounds");
    bank.stop();
    require(!bank.configureStemRamps(4), "Stopped bank accepted live gain configuration");
}

void wholeBankHandoffLifetime()
{
    Fixture first, second;
    const auto baseline = descriptors();
    // Four each: read-ahead, optional loop cache, tempo and scratch workers.
    constexpr std::size_t descriptorsPerBank = 16;
    auto active = std::make_unique<PreparedPlayback>(first.fds(), Fixture::frames);
    auto candidate = std::make_unique<PreparedPlayback>(second.fds(), Fixture::frames);
    require(active->prime(0, 1s) && candidate->prime(0, 1s), "Cannot prime handoff banks");
    require(descriptors() == baseline + 2 * descriptorsPerBank, "Bank owners did not retain independent reader/cache/tempo/scratch FDs");
    RenderCore core(active->sourceRef());
    std::array<float, 128> output{};
    require(core.render(output) && core.setPlaying(0, false) && core.render(output), "Cannot silence old bank");
    SourceHandoff mailbox;
    require(mailbox.request({0, candidate->sourceRef(), candidate.get(), 1}), "Cannot publish whole bank");
    SourceHandoff::Request request;
    require(mailbox.readPending(request) && request.playback == candidate.get() &&
            core.isDeckSilentForReplacement(0) && core.replaceSource(0, request.source), "Whole-bank swap failed");
    require(core.render(output) && mailbox.complete(true, {64, 0, false}, core.frameClock()),
            "Callback-end acknowledgement failed");
    require(descriptors() == baseline + 2 * descriptorsPerBank, "Old bank was retired before control acquired acknowledgement");
    SourceHandoff::Completion completion;
    require(mailbox.takeCompletion(completion) && completion.installed, "Control did not acquire whole-bank completion");
    auto retired = std::move(active);
    active = std::move(candidate);
    retired.reset();
    require(descriptors() == baseline + descriptorsPerBank, "Retirement did not release all old reader/cache/tempo/scratch FDs");
    require(core.setPlaying(0, true) && core.render(output) && same(output, std::span(second.sum).first(output.size())),
            "New bank stopped or lost samples after old whole-owner destruction");
    auto cancelled = std::make_unique<PreparedPlayback>(first.fds(), Fixture::frames);
    require(cancelled->prime(0, 1s) && mailbox.request({0, cancelled->sourceRef(), cancelled.get(), 2}),
            "Cannot stage cancellation fixture");
    mailbox.quiesce();
    require(mailbox.takeCompletion(completion) && !completion.installed, "Quiesce falsely installed a bank");
    cancelled.reset();
    require(descriptors() == baseline + descriptorsPerBank && !active->errorCode(), "Cancelled bank cleanup affected the retained whole owner");
}
} // namespace

int main()
{
    try {
        sumBusesAndBorrowedDescriptors();
        coherentPrimeSeekAndStop();
        failureInAnyReaderAndConstructionCleanup();
        wholeBankHandoffLifetime();
        preparedLiveStemRamps();
        std::cout << "Prepared whole-bank files, alignment, health, budgets and lifetime checks completed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

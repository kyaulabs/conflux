// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedTempoSource.hpp"
#include "audio/StemBankSource.hpp"
#include <algorithm>
#include <array>
#include <memory>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace conflux::audio;
namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void await(const std::function<bool()>& condition)
{
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        require(Clock::now() < end, "Stream worker did not reach the expected boundary");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Input {
    std::uint64_t frames = 48000;
    std::size_t leaves = 4;
    std::atomic<std::uint64_t> available{0}, calls{0}, services{0}, emptyReads{0};
    std::atomic<int> fault{0};
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> wrongThread{false};
    static float sample(std::uint64_t frame, std::size_t leaf, std::size_t channel)
    {
        return float(.06 * (leaf + 1) * std::sin((frame + 13 * leaf + 29 * channel) * .017));
    }
    PreparedTempoSource::StreamInput descriptor()
    {
        return {frames, leaves, this,
            [](void* context, std::uint64_t generation, std::uint64_t first, std::span<float> output) noexcept {
                auto& self = *static_cast<Input*>(context);
                self.calls.fetch_add(1);
                if (std::this_thread::get_id() == self.caller) self.wrongThread.store(true);
                if (!generation || first > self.frames) return AudioReadResult{0, true};
                const auto wanted = output.size() / (self.leaves * 2);
                if (self.fault.load() == 1) return AudioReadResult{wanted + 1, false};
                if (self.fault.load() == 2) return AudioReadResult{0, false, false, 0, true};
                if (self.fault.load() == 3) return AudioReadResult{0, true};
                if (self.fault.load() == 5) return AudioReadResult{1, false}; // Unwritten advertised sample.
                if (self.fault.load() == 6) return AudioReadResult{0, false, true, 0};
                const auto end = std::min(self.frames, self.available.load());
                const auto count = first < end ? std::min<std::uint64_t>(wanted, end - first) : 0;
                if (!count) self.emptyReads.fetch_add(1);
                for (std::size_t f = 0; f < count; ++f)
                    for (std::size_t leaf = 0; leaf < self.leaves; ++leaf)
                        for (std::size_t channel = 0; channel < 2; ++channel)
                            output[f * self.leaves * 2 + leaf * 2 + channel] = sample(first + f, leaf, channel);
                if (self.fault.load() == 4 && count) output[0] = NAN;
                return AudioReadResult{std::size_t(count), false};
            },
            [](const void*) noexcept { return 0; },
            [](void* context) noexcept {
                auto& self = *static_cast<Input*>(context);
                self.services.fetch_add(1);
                if (std::this_thread::get_id() == self.caller) self.wrongThread.store(true);
            }};
    }
};
PreparedTempoSource::View next(PreparedTempoSource& source)
{
    PreparedTempoSource::View view;
    await([&] { view = source.peek(); return bool(view); });
    return view;
}
void exact(const PreparedTempoSource::View& view, std::uint64_t first)
{
    require(view.frames && !view.errorCode && !view.terminal && view.nominalStart == double(first),
            "Unity stream packet lost its source position");
    for (std::size_t leaf = 0; leaf < view.leafCount; ++leaf)
        for (std::size_t f = 0; f < view.frames; ++f)
            for (std::size_t channel = 0; channel < 2; ++channel)
                require(view.leaves[leaf][f * 2 + channel] == Input::sample(first + f, leaf, channel),
                        "Unity streaming changed, reordered or substituted source PCM");
}
void rawStemInput()
{
    std::array<std::array<float, 16>, 4> pcm{};
    std::array<AudioSource, 4> descriptors{};
    for (std::size_t leaf = 0; leaf < 4; ++leaf) {
        std::fill(pcm[leaf].begin(), pcm[leaf].end(), float(leaf + 1));
        descriptors[leaf] = AudioSource::fromMemory(pcm[leaf]);
    }
    StemBankSource bank(descriptors);
    for (std::size_t leaf = 1; leaf < 4; ++leaf) require(bank.setGain(leaf, 0), "Initial stem mute rejected");
    require(bank.configureRamps(4) && bank.setGain(0, 0), "Stem ramp setup rejected");
    std::array<float, 16> packed{};
    const auto raw = bank.readUnmixed(0, packed);
    require(raw.frames == 2 && !raw.failed && bank.sourceFrame() == 2, "Raw stem input did not commit a common prefix");
    std::array<std::array<float, 4>, 4> leaves{};
    std::array<std::span<const float>, 4> views{};
    for (std::size_t leaf = 0; leaf < 4; ++leaf) {
        views[leaf] = leaves[leaf];
        for (std::size_t frame = 0; frame < 2; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto value = packed[frame * 8 + leaf * 2 + channel];
                require(value == float(leaf + 1), "Raw DSP input applied a gain or reordered stems/channels");
                leaves[leaf][frame * 2 + channel] = value;
            }
    }
    std::array<float, 4> mixed{};
    require(bank.readCached(views, 0, mixed).frames == 2 && mixed[0] == .75F && mixed[2] == .5F,
            "Input prefetch advanced the render-owned stem ramp");
    require(bank.seekInput(0) && bank.readUnmixed(0, std::span(packed).first(8)).frames == 1,
            "Raw generation seek failed");
    require(bank.readCached(views, 0, std::span(mixed).first(2)).frames == 1 && mixed[0] == .25F,
            "Raw seek reset or advanced the render-owned stem ramp");
    require(!bank.seekInput(9) && bank.sourceFrame() == 1, "Invalid raw seek changed input state");
    const auto cursor = bank.sourceFrame();
    require(bank.readUnmixed(3, packed).failed && bank.sourceFrame() == cursor,
            "Wrong raw cursor implicitly sought or consumed input");
    require(bank.readUnmixed(cursor, std::span(packed).first(7)).failed && bank.sourceFrame() == cursor,
            "Partial four-stem frame changed input state");
}
void gaps()
{
    Input input; input.frames = 17009;
    PreparedTempoSource source(input.descriptor());
    require(source.request(0, 0, false), "Stream request rejected");
    await([&] { return input.calls.load() >= 3; });
    require(!source.peek() && !source.status().finished && !source.errorCode() && source.nominalPosition() == 0,
            "Initial unavailable input changed transport or reported EOF/error");
    input.available.store(333);
    auto view = next(source); exact(view, 0);
    require(view.frames == 333 && source.consume(view.frames), "Partial live prefix was not delivered exactly");
    const auto calls = input.calls.load();
    await([&] { return input.calls.load() >= calls + 3; });
    require(!source.peek() && source.nominalPosition() == 333 && !source.status().finished && !source.errorCode(),
            "A temporary input gap flushed or advanced the stream");
    input.available.store(input.frames);
    std::uint64_t cursor = 333;
    for (;;) {
        view = next(source);
        if (view.terminal) { require(source.consume(0), "EOF marker rejected"); break; }
        exact(view, cursor); cursor += view.frames;
        require(source.consume(view.frames), "Contiguous stream packet rejected");
    }
    require(cursor == input.frames && source.status().finished && !input.wrongThread.load(),
            "Complete source extent or off-callback input ownership was lost");
    const auto service = input.services.load();
    await([&] { return input.services.load() >= service + 3; });
    require(source.nominalPosition() == double(input.frames) && source.status().finished,
            "Finished-stream activity service changed transport");
}
void outputRevisionAcrossGap()
{
    Input input;input.available.store(16384);
    PreparedTempoSource source(input.descriptor());
    require(source.request(0,6,false) && source.outputConfigurationCurrent(),
            "A fresh generation was confused with queued older control revisions");
    require(source.requestSpeed(-6) && !source.outputConfigurationCurrent(),
            "A pending speed revision was mistaken for current queued output");
    await([&] {
        const auto view=source.peek();
        if(view.frames)require(source.consume(view.frames),"Revision reference consume failed");
        return source.outputConfigurationCurrent();
    });
    const auto emptyReads=input.emptyReads.load();
    await([&] {
        const auto view=source.peek();
        if(view.frames)require(source.consume(view.frames),"Gap revision drain failed");
        return input.emptyReads.load()>=emptyReads+3 && !source.peek();
    });
    require(source.status().pending && source.outputConfigurationCurrent(),
            "A missing raw suffix revoked an already-applied output configuration");
    require(source.cancel() && !source.outputConfigurationCurrent(),"Cancelled output remained current");
    source.stop();require(!source.outputConfigurationCurrent(),"Stopped output remained current");
}
void seekAndCancel()
{
    Input input;
    PreparedTempoSource source(input.descriptor());
    require(source.request(0, 0, false), "Gap request rejected");
    await([&] { return input.calls.load() >= 2; });
    const auto old = source.generation();
    require(source.request(12345, 0, false) && source.generation() != old, "Seek did not change generation");
    input.available.store(input.frames);
    auto view = next(source); exact(view, 12345);
    require(source.request(4567, 0, false), "Seek with borrowed stale output rejected");
    view = next(source); exact(view, 4567);
    require(source.cancel() && !source.peek() && !source.status().active, "Cancellation retained stale output");
    input.available.store(0);
    require(source.request(0, 0, false), "Gap restart rejected");
    const auto before = input.calls.load(); await([&] { return input.calls.load() > before; });
    const auto begin = Clock::now(); source.stop();
    require(Clock::now() - begin < std::chrono::seconds(1), "Missing input prevented worker shutdown");
}
void malformed()
{
    for (int fault : {1, 2, 3, 4, 5, 6}) {
        Input input; input.available.store(input.frames); input.fault.store(fault);
        PreparedTempoSource source(input.descriptor());
        require(source.request(0, 0, false), "Malformed fixture request rejected prematurely");
        auto view = next(source);
        require(view.errorCode && !view.terminal && !view.frames && source.errorCode(),
                "Malformed or failed live input became valid audio or successful EOF");
        require(source.consume(0), "Failed marker was not consumable");
        input.fault.store(0);
        require(source.request(1024, 0, false), "New generation could not recover an old input failure");
        view = next(source); exact(view, 1024);
        require(!source.errorCode(), "Old-generation input failure contaminated the new stream");
    }
    Input input;
    for (std::size_t leaves : {0U, 2U, 3U, 5U}) {
        auto descriptor = input.descriptor(); descriptor.leafCount = leaves;
        bool rejected = false;
        try { PreparedTempoSource bad(descriptor); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Invalid live role count accepted");
    }
    auto descriptor = input.descriptor(); descriptor.read = nullptr;
    bool rejected = false;
    try { PreparedTempoSource bad(descriptor); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Absent streaming callback accepted");
    for (const auto frames : {std::uint64_t{0}, (std::uint64_t{1} << 53U) + 1}) {
        descriptor = input.descriptor(); descriptor.frames = frames; rejected = false;
        try { PreparedTempoSource bad(descriptor); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Invalid streaming frame geometry accepted");
    }
    descriptor = input.descriptor(); descriptor.errorCode = [](const void*) noexcept { return -1; };
    PreparedTempoSource unhealthy(descriptor);
    require(unhealthy.request(0, 0, false), "Health fixture request rejected prematurely");
    require(next(unhealthy).errorCode == EINVAL && !input.calls.load(), "Invalid upstream health was ignored");
}
std::vector<float> drain(PreparedTempoSource& source)
{
    std::vector<float> result;
    for (;;) {
        const auto view = next(source);
        require(!view.errorCode, "DSP stream failed");
        if (view.terminal) { require(source.consume(0), "DSP EOF rejected"); return result; }
        for (std::size_t f = 0; f < view.frames; ++f)
            for (std::size_t leaf = 0; leaf < view.leafCount; ++leaf)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    result.push_back(view.leaves[leaf][f * 2 + channel]);
        require(source.consume(view.frames), "DSP output prefix rejected");
    }
}
void boundedPrefetch()
{
    Input input; input.available.store(input.frames);
    PreparedTempoSource source(input.descriptor());
    require(source.request(0, 0, false), "Bounded stream request rejected");
    await([&] { return input.calls.load() == PreparedTempoSource::slotCount; });
    const auto service = input.services.load();
    await([&] { return input.services.load() >= service + 3; });
    require(input.calls.load() == PreparedTempoSource::slotCount && !input.wrongThread.load(),
            "Full output ring consumed input or failed worker-side pause servicing");
    require(!source.request(256, 0, false, 256, 1024), "Sequential live input accepted a cached loop");
    auto view = next(source); exact(view, 0);
    require(source.request(4000, 0, false), "Full-ring stream seek rejected");
    view = next(source); exact(view, 4000);
}
struct FileCloser { void operator()(std::FILE* file) const noexcept { if (file) std::fclose(file); } };
void immutableLoopReference()
{
    constexpr std::uint64_t frames = 4096;
    struct Resident {
        std::array<std::vector<float>,4> leaves;
        std::atomic<bool> ready{false};
        std::uint64_t previous = 0, wraps = 0; // input worker, inspected after join
    };
    for (const bool keyLock : {false,true}) for (const double percent : {-6.,6.}) {
        Resident input;
        std::array<std::unique_ptr<std::FILE,FileCloser>,4> files;
        std::array<int,4> fds{};
        for (std::size_t role=0;role<4;++role) {
            auto& leaf = input.leaves[role];leaf.resize(frames*2);
            for (std::size_t f=0;f<frames;++f) for(std::size_t channel=0;channel<2;++channel)
                leaf[f*2+channel] = Input::sample(f,role,channel);
            files[role].reset(std::tmpfile());require(bool(files[role]),"Resident reference file failed");
            require(std::fwrite(leaf.data(),sizeof(float),leaf.size(),files[role].get())==leaf.size() &&
                    !std::fflush(files[role].get()),"Resident reference write failed");
            fds[role]=fileno(files[role].get());
        }
        PreparedTempoSource finite(fds,frames);
        PreparedTempoSource memory(PreparedTempoSource::StreamInput{
            frames,4,&input,
            [](void* context,std::uint64_t,std::uint64_t frame,std::span<float> output) noexcept {
                auto& resident=*static_cast<Resident*>(context);
                if (!resident.ready.load()) return AudioReadResult{};
                if (frame<resident.previous) ++resident.wraps;
                resident.previous=frame;
                const auto count=std::min<std::size_t>(output.size()/8,frames-frame);
                for(std::size_t f=0;f<count;++f) for(std::size_t role=0;role<4;++role)
                    for(std::size_t channel=0;channel<2;++channel)
                        output[f*8+role*2+channel]=resident.leaves[role][(frame+f)*2+channel];
                return AudioReadResult{count,false};
            },[](const void*) noexcept{return 0;},nullptr,true});
        require(finite.request(0,percent,keyLock,0,frames) && memory.request(0,percent,keyLock,0,frames),
                "Immutable resident loop request rejected");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(!memory.peek() && !memory.errorCode() && !memory.status().finished,
                "Pending resident capture became silence, failure or EOF");
        input.ready.store(true);
        auto take=[](PreparedTempoSource& source) {
            std::vector<float> result;
            constexpr std::size_t wanted=20000;
            while(result.size()<wanted*8) {
                PreparedTempoSource::View view;
                await([&]{view=source.peek();return bool(view);});
                require(view.frames && !view.errorCode && !view.terminal,"Resident loop ended or failed");
                const auto count=std::min(view.frames,wanted-result.size()/8);
                for(std::size_t f=0;f<count;++f)for(std::size_t role=0;role<4;++role)
                    for(std::size_t channel=0;channel<2;++channel)result.push_back(view.leaves[role][f*2+channel]);
                require(source.consume(count),"Resident reference consume failed");
            }
            return result;
        };
        const auto expected=take(finite),actual=take(memory);
        require(expected==actual,"Resident memory-loop DSP differs from the same immutable file loop");
        finite.stop();memory.stop();require(input.wraps>0,"Resident reference did not wrap input");
    }
}
void dspReference()
{
    for (const auto leaves : {1U, 4U}) for (const auto prefix : {0U, 3072U})
        for (bool keyLock : {false, true}) for (double percent : {-6., 6.}) {
        Input input; input.frames = 12011; input.leaves = leaves;
        std::array<std::unique_ptr<std::FILE, FileCloser>, 4> files;
        std::array<int, 4> fds{};
        for (std::size_t leaf = 0; leaf < leaves; ++leaf) {
            files[leaf].reset(std::tmpfile()); require(bool(files[leaf]), "Anonymous reference file failed");
            std::vector<float> pcm(input.frames * 2);
            for (std::size_t f = 0; f < input.frames; ++f)
                for (std::size_t c = 0; c < 2; ++c) pcm[f * 2 + c] = Input::sample(f, leaf, c);
            require(std::fwrite(pcm.data(), sizeof(float), pcm.size(), files[leaf].get()) == pcm.size() &&
                    !std::fflush(files[leaf].get()), "Reference write failed");
            fds[leaf] = fileno(files[leaf].get());
        }
        PreparedTempoSource finite{std::span(fds).first(leaves), input.frames};
        PreparedTempoSource live{input.descriptor()};
        input.available.store(prefix);
        require(finite.request(0, percent, keyLock) && live.request(0, percent, keyLock), "DSP requests rejected");
        await([&] { return input.calls.load() >= prefix / 1024 + 3; });
        require(!live.errorCode() && !live.status().finished, "Gap incorrectly finalized the DSP");
        input.available.store(input.frames);
        const auto expected = drain(finite), actual = drain(live);
        require(expected == actual && !actual.empty(), "Live DSP differs from the same complete finite source");
        require(!input.wrongThread.load(), "Live input ran on the caller thread");
    }
}
}
int main()
{
    try { rawStemInput(); gaps(); outputRevisionAcrossGap(); seekAndCancel(); malformed(); boundedPrefetch(); dspReference(); immutableLoopReference();
        std::cout << "Streaming tempo gap, source-clock, generation, failure and real DSP checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

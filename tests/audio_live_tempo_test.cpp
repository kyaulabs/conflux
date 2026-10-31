// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/PreparedTempoSource.hpp"
#include "audio/RenderCore.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
using namespace conflux::audio;
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t extent = 600000;
void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
void await(const std::function<bool()>& condition)
{
    const auto end = Clock::now() + std::chrono::seconds(4);
    while (!condition()) {
        require(Clock::now() < end, "Live tempo fixture did not reach its boundary");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
float sample(std::size_t role, std::uint64_t frame, std::size_t channel)
{
    const float value = float((role + 1) * 16 + frame % 31) / 4096.F;
    return channel ? -value : value;
}
struct Control {
    std::atomic<std::uint64_t> allowance{extent}, target{0}, epoch{0};
    std::atomic<bool> exited{false};
    std::atomic<int> failure{0};
};
PreparedPlayback::LiveProducer producer(std::shared_ptr<Control> control)
{
    return [control](std::stop_token stop, PreparedPlayback::LiveProducerPort& port) {
        std::uint64_t epoch = 0, cursor = 0, target = 0;
        std::array<std::array<float, LiveStemBuffer::blockFrames * 2>, 4> leaves{};
        while (!stop.stop_requested()) {
            if (const int code = control->failure.load()) { port.fail(code); break; }
            if (!port.serviceRecovery()) break;
            LiveStemBuffer::Request request;
            if (!port.tryRequest(request)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            if (request.epoch != epoch) {
                epoch = request.epoch; cursor = target = request.firstFrame;
                control->target.store(target); control->epoch.store(epoch);
            }
            const auto allowance = control->allowance.load();
            if (cursor == extent || cursor - target >= allowance) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
            }
            const auto count = std::size_t(std::min<std::uint64_t>({extent - cursor, LiveStemBuffer::blockFrames, allowance - (cursor - target)}));
            for (std::size_t role = 0; role < 4; ++role)
                for (std::size_t f = 0; f < count; ++f)
                    for (std::size_t c = 0; c < 2; ++c) leaves[role][f * 2 + c] = sample(role, cursor + f, c);
            LiveStemBuffer::Packet packet{request.sourceId, epoch, cursor, count, cursor + count == extent, {}};
            for (std::size_t role = 0; role < 4; ++role) packet.stems[role] = std::span(leaves[role]).first(count * 2);
            const auto result = port.tryPublish(packet);
            if (result == LiveStemBuffer::PublishResult::Accepted) cursor += count;
            else if (result == LiveStemBuffer::PublishResult::Invalid) { port.fail(EPROTO); break; }
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        control->exited.store(true);
    };
}
AudioReadResult read(AudioSource source, double& cursor, std::span<float> output)
{
    AudioReadResult result;
    await([&] {
        result = source.read(source.context, std::uint64_t(std::floor(cursor)), output);
        require(!result.failed, "Healthy live tempo source reported failed audio");
        if (result.positionValid) cursor = result.nominalPosition;
        return result.frames != 0;
    });
    return result;
}
void liveControls()
{
    auto control = std::make_shared<Control>();
    auto owner = PreparedPlayback::makeLive(5001, extent, producer(control));
    require(owner->prime(0, std::chrono::seconds(4)), "Live tempo fixture did not prime");
    auto source = owner->sourceRef();
    require(source.tempoControl && source.tempoState && source.tempoState(source.context).supported,
            "Live tempo hooks are unavailable");
    require(owner->configureStemRamps(0), "Live gain setup rejected");
    const std::array<float, 4> gains{1.F, .5F, 0.F, .25F};
    for (std::size_t leaf = 0; leaf < 4; ++leaf) require(source.setStemGain(source.context, leaf, gains[leaf]), "Stem target rejected");
    source.setRenderActive(source.context, true);
    std::array<float, 512> output{};
    const auto initial = source.read(source.context, 0, output);
    require(initial.frames == 256, "Initial raw live prefix unavailable");
    double cursor = 256;
    require(source.tempoControl(source.context, AudioTempoCommand::Set, 6, false, 256), "Live tempo increase rejected");
    const auto first = read(source, cursor, output);
    require(first.positionValid && cursor > 256 && std::ranges::any_of(output, [](float x) { return std::abs(x) > .0001F; }),
            "Live tempo lost its nominal source clock or audible signal");
    auto state = source.tempoState(source.context);
    const auto generation = state.generation;
    require(source.tempoControl(source.context, AudioTempoCommand::Set, -6, false, std::uint64_t(cursor)) &&
            source.tempoState(source.context).generation == generation, "Live speed adjustment unnecessarily reprimes");
    const auto nativeEpoch = control->epoch.load();
    require(source.tempoControl(source.context, AudioTempoCommand::Set, -6, true, std::uint64_t(cursor)) &&
            source.tempoState(source.context).generation != generation, "Master Tempo did not invalidate old PCM");
    cursor = std::floor(cursor); read(source, cursor, output);
    require(control->epoch.load() == nativeEpoch, "A tempo-mode change repeated separation of already available raw stems");
    for (std::size_t leaf = 0; leaf < 4; ++leaf) require(source.stemGain(source.context, leaf) == gains[leaf], "Live DSP changed a stem target");
    const auto unityAt = std::uint64_t(cursor);
    require(source.tempoControl(source.context, AudioTempoCommand::Reset, 0, true, unityAt), "Live tempo reset rejected");
    cursor = double(unityAt); const auto unity = read(source, cursor, output);
    for (std::size_t f = 0; f < unity.frames; ++f) for (std::size_t c = 0; c < 2; ++c) {
        double expected = 0;
        for (std::size_t leaf = 0; leaf < 4; ++leaf) expected += double(sample(leaf, unityAt + f, c)) * gains[leaf];
        require(output[f * 2 + c] == float(expected), "Live unity reset used stale/transformed PCM or double-applied gains");
    }
    source.discontinuity(source.context, 12345); cursor = 12345;
    const auto sought = read(source, cursor, output);
    require(sought.positionValid && cursor == 12345 + sought.frames, "Live processed seek lost its source position");
    // Advance beyond the entire bounded history, including physical ring wrap.
    while (cursor < 12345 + PreparedPlayback::liveTempoHistoryFrames + 4096) read(source, cursor, output);
    const auto recent = std::uint64_t(cursor) - 512;
    const auto retainedEpoch = control->epoch.load();
    source.discontinuity(source.context, recent); cursor = double(recent);
    const auto retained = read(source, cursor, output);
    require(control->epoch.load() == retainedEpoch, "Recent seek ignored retained raw stems");
    for (std::size_t f = 0; f < retained.frames; ++f) for (std::size_t c = 0; c < 2; ++c) {
        double expected = 0;
        for (std::size_t leaf = 0; leaf < 4; ++leaf) expected += double(sample(leaf, recent + f, c)) * gains[leaf];
        require(output[f * 2 + c] == float(expected), "Wrapped raw history returned stale or reordered PCM");
    }
    source.discontinuity(source.context, 0); cursor = 0;
    read(source, cursor, output);
    require(control->epoch.load() != retainedEpoch, "Evicted raw frames reused an old provider epoch");
    require(source.tempoControl(source.context, AudioTempoCommand::Set, 6, true, std::uint64_t(cursor)), "Live mode restore setup rejected");
    require(owner->primeForSession(5000, std::chrono::seconds(4)), "Live tempo session reconstruction did not prime");
    source = owner->sourceRef(); state = source.tempoState(source.context);
    require(state.active && state.percent == 6 && state.keyLock, "Session reconstruction lost the live tempo selection");
    cursor = 5000; require(read(source, cursor, output).positionValid, "Restored live mode lost processed positions");
    control->allowance.store(0);
    source.discontinuity(source.context, 500000);
    await([&] { return control->target.load() == 500000; });
    const auto gap = source.read(source.context, 500000, output);
    require(!gap.frames && !gap.failed && gap.positionValid && gap.nominalPosition == 500000,
            "Missing live input inserted audio, moved transport or failed prematurely");
    const auto begin = Clock::now(); owner->stop();
    require(Clock::now() - begin < std::chrono::seconds(1) && control->exited.load(), "Stalled live tempo prevented complete owner shutdown");
}
void renderCommands()
{
    auto control = std::make_shared<Control>();
    auto owner = PreparedPlayback::makeLive(5003, extent, producer(control));
    require(owner->prime(0, std::chrono::seconds(4)), "Render-command live fixture did not prime");
    AudioSource peer{10000000, nullptr, nullptr,
        [](void*, std::uint64_t, std::span<float> output) noexcept {
            std::fill(output.begin(), output.end(), .01F);
            return AudioReadResult{output.size()/2, false};
        }};
    RenderCore core(owner->sourceRef(), peer);
    require(core.setPlaying(0, true) && core.setPlaying(1, true) && core.setCueSelected(0, true) &&
            core.setDeckGain(0, .5F) && core.setCrossfader(.25F), "Render fixture controls rejected");
    std::array<float, 512> master{}, cue{};
    auto block = [&] {
        const auto clock = core.frameClock(), peerFrame = core.sourceFrame(1);
        require(core.renderBuses(master, cue) && !core.readFailed(0) && !core.readFailed(1),
                "Renderer rejected live tempo position/state metadata");
        require(core.frameClock() == clock+256 && core.sourceFrame(1) == peerFrame+256,
                "Live processing/buffering interrupted its peer or output clock");
        require(std::ranges::all_of(master, [](float x) { return std::isfinite(x); }) &&
                std::ranges::all_of(cue, [](float x) { return std::isfinite(x); }), "Live tempo emitted nonfinite buses");
    };
    auto advance = [&] {
        const double first = core.sourcePosition(0);
        await([&] { block(); return core.sourcePosition(0) >= first+1024; });
    };
    for (double percent : {6., -6.}) {
        require(core.setTempoPercent(0, percent), "Renderer rejected live BPM change");
        advance();
    }
    require(core.toggleMasterTempo(0) && core.tempoState(0).keyLock, "Renderer rejected live Master Tempo");
    advance();
    require(core.resetTempo(0) && core.tempoState(0).percent == 0 && core.tempoState(0).keyLock,
            "Renderer lost live unity reset or key-lock selection");
    advance();
    require(core.deckGain(0) == .5F && core.crossfader() == .25F && core.isCueSelected(0),
            "Live tempo commands changed manual gain/crossfader/cue controls");
    require(core.setPlaying(0, false), "Live pause rejected");
    const auto paused = core.sourcePosition(0); block();
    require(core.sourcePosition(0) == paused && core.setPlaying(0, true), "Pause consumed raw/processed source time");
    control->allowance.store(0);
    require(core.seek(0, 500000), "Renderer rejected a live processed seek");
    await([&] { return control->target.load() == 500000; });
    const auto starved = core.starvedFrames(0); block();
    require(core.sourcePosition(0) == 500000 && core.isBuffering(0) && core.starvedFrames(0) == starved+256,
            "A live gap advanced source time, became EOF or lost buffering accounting");
    require(core.setPlaying(0, false), "Final live pause rejected");
    owner->stop();
    require(control->exited.load(), "Render-command live owner did not retire its producer");
}

void preservesConfiguredRecovery()
{
    auto control = std::make_shared<Control>();
    AudioResidentHoldRange range;
    range.id = 87; range.inFrame = 0; range.outFrame = 2048;
    range.admittedBytes = 2048 * AudioResidentHoldRange::bytesPerFrame;
    range.provenance = AudioResidentHoldRange::Provenance::DjRange;
    auto owner = PreparedPlayback::makeLive(5002, extent, producer(control), range);
    require(owner->prime(0, std::chrono::seconds(4)), "Recovery fixture did not prime");
    const auto source = owner->sourceRef();
    require(source.tempoControl && source.tempoState && source.tempoState(source.context).supported &&
            source.residentHoldState && source.residentHoldControl,
            "Configured recovery requires tempo alongside its existing hold controls");
    require(source.residentHoldState(source.context).supported &&
            source.residentHoldControl(source.context, AudioResidentHoldCommand::Arm, range.id, 0) &&
            source.residentHoldState(source.context).armed, "Configured recovery range lost its authorization");
    source.setRenderActive(source.context, true);
    std::array<float, 512> output{};
    const auto result = source.read(source.context, 0, output);
    require(result.frames == 256 && !result.failed, "Configured recovery source lost ordinary live playback");
    owner->stop();
    require(control->exited.load(), "Configured recovery owner did not close");
}
struct CloseReference { void operator()(std::FILE* file) const noexcept { if (file) std::fclose(file); } };
struct ResidentReference {
    std::array<std::unique_ptr<std::FILE,CloseReference>,4> files;
    std::unique_ptr<PreparedTempoSource> processor;
    const std::uint64_t frames;
    ResidentReference(std::uint64_t firstFrame,std::uint64_t length,double percent,bool keyLock) : frames(length) {
        std::array<int,4> fds{};
        for(std::size_t role=0;role<4;++role) {
            files[role].reset(std::tmpfile()); require(bool(files[role]),"Recovery reference file failed");
            for(std::uint64_t frame=0;frame<frames;++frame) {
                const std::array<float,2> stereo{sample(role,firstFrame+frame,0),sample(role,firstFrame+frame,1)};
                require(std::fwrite(stereo.data(),sizeof(float),2,files[role].get())==2,"Recovery reference write failed");
            }
            require(!std::fflush(files[role].get()),"Recovery reference flush failed"); fds[role]=fileno(files[role].get());
        }
        processor=std::make_unique<PreparedTempoSource>(fds,frames);
        reset(percent,keyLock);
    }
    void reset(double percent,bool keyLock) {
        require(processor->request(0,percent,keyLock,0,frames),"Recovery reference loop request rejected");
    }
    void compare(std::span<const float> actual) {
        std::size_t offset=0;
        while(offset<actual.size()/2) {
            PreparedTempoSource::View view;
            await([&]{view=processor->peek();return bool(view);});
            require(view.frames && !view.errorCode && !view.terminal,"Recovery reference failed or ended");
            const auto count=std::min(view.frames,actual.size()/2-offset);
            for(std::size_t frame=0;frame<count;++frame)for(std::size_t channel=0;channel<2;++channel) {
                const float expected=float(double(view.leaves[0][frame*2+channel])*.5);
                require(actual[(offset+frame)*2+channel]==expected,
                        "Processed recovery waveform differs from its independent selected-stem file-loop oracle");
            }
            require(processor->consume(count),"Recovery reference prefix consume failed");offset+=count;
        }
    }
};
void recoveryAtSelectedTempo()
{
    for (const bool keyLock : {false, true}) for (const double percent : {-6., 6.}) {
        auto control = std::make_shared<Control>();
        const std::uint64_t recoveryIn=keyLock ? 1234 : 0;
        AudioResidentHoldRange range{89, recoveryIn, recoveryIn+8192, 8192 * AudioResidentHoldRange::bytesPerFrame,
                                     AudioResidentHoldRange::Provenance::DjRange};
        auto owner = PreparedPlayback::makeLive(5004, extent, producer(control), range);
        require(owner->prime(0, std::chrono::seconds(4)), "Selected-tempo recovery did not prime");
        auto source = owner->sourceRef();
        require(source.tempoControl && source.tempoState && source.tempoState(source.context).supported,
                "Configured recovery did not expose a compatible tempo path");
        require(source.setStemGain(source.context, 0, .5F), "Selected recovery stem gain rejected");
        for (std::size_t role = 1; role < 4; ++role)
            require(source.setStemGain(source.context, role, 0.F), "Recovery stem-only selection rejected");
        require(source.tempoControl(source.context, AudioTempoCommand::Set, percent, keyLock, 0),
                "Recovery-configured source rejected its selected BPM/key lock");
        source.setRenderActive(source.context, true);
        double cursor = 0;
        std::array<float, 512> output{};
        while (cursor < 1024) (void)read(source, cursor, output);
        const auto parked = cursor;
        ResidentReference reference(range.inFrame,range.outFrame-range.inFrame,percent,keyLock);
        require(source.residentHoldControl(source.context, AudioResidentHoldCommand::Arm, range.id,
                                          std::uint64_t(cursor)), "Processed recovery arm rejected");
        await([&] { return source.residentHoldState(source.context).ready; });
        require(source.residentHoldControl(source.context, AudioResidentHoldCommand::Enter, range.id,
                                          std::uint64_t(cursor)), "Processed recovery enter rejected");
        const auto epoch = control->epoch.load();
        std::size_t heldFrames = 0;
        while (heldFrames < 20000) {
            const auto part = read(source, cursor, output);
            require(part.residentHold && part.positionValid && !part.finished && cursor == parked &&
                    source.residentHoldState(source.context).active,
                    "Processed resident recovery moved the linear cursor or became EOF");
            reference.compare(std::span(output).first(part.frames*2));
            const auto tempo = source.tempoState(source.context);
            require(tempo.active && tempo.percent == percent && tempo.keyLock == keyLock,
                    "Resident recovery reset selected BPM or Master Tempo");
            double energy = 0;
            for (const float value : std::span(output).first(part.frames * 2)) {
                require(std::isfinite(value) && std::abs(value) < .02F,
                        "Recovery emitted invalid audio or restored unwanted stem roles");
                energy += double(value) * value;
            }
            require(energy > 1e-12, "Recovery substituted silence for resident music");
            heldFrames += part.frames;
        }
        require(source.residentHoldControl(source.context, AudioResidentHoldCommand::Exit, range.id,
                                          std::uint64_t(cursor)), "Processed recovery exit rejected");
        await([&] {
            (void)read(source, cursor, output);
            return !source.residentHoldState(source.context).active;
        });
        require(cursor > parked && control->epoch.load() == epoch && source.stemGain(source.context, 0) == .5F,
                "Recovery return repeated separation, lost stem gain or failed to advance");
        require(source.tempoState(source.context).percent == percent &&
                source.tempoState(source.context).keyLock == keyLock, "Return lost selected tempo");
        // A terminal native producer fault must not invalidate accepted
        // resident PCM or restore an unwanted role. Exit remains pending until
        // an actual healthy return is possible; explicit Cancel retains fault.
        require(source.residentHoldControl(source.context, AudioResidentHoldCommand::Arm, range.id,
                                          std::uint64_t(cursor)), "Second processed recovery arm rejected");
        await([&] { return source.residentHoldState(source.context).ready; });
        reference.reset(percent,keyLock);
        const auto failedAt = cursor;
        control->failure.store(EIO);
        await([&] { return control->exited.load() && source.residentHoldState(source.context).errorCode == EIO; });
        const auto automatic = read(source,cursor,output);
        require(automatic.residentHold && !automatic.failed && cursor==failedAt &&
                source.residentHoldState(source.context).active,
                "Terminal live failure did not automatically enter the armed processed resident range");
        reference.compare(std::span(output).first(automatic.frames*2));
        require(source.residentHoldControl(source.context, AudioResidentHoldCommand::Exit, range.id,
                                          std::uint64_t(cursor)), "Faulted resident exit request rejected");
        for (std::size_t block=0;block<48;++block) {
            const auto part = read(source,cursor,output);
            require(part.residentHold && !part.failed && cursor == failedAt &&
                    source.residentHoldState(source.context).active && source.residentHoldState(source.context).ready,
                    "Producer failure discarded healthy processed resident audio");
            reference.compare(std::span(output).first(part.frames*2));
        }
        require(source.residentHoldControl(source.context, AudioResidentHoldCommand::Cancel, range.id,
                                          std::uint64_t(cursor)), "Explicit processed recovery cancellation rejected");
        const auto failed = source.read(source.context,std::uint64_t(cursor),output);
        require(failed.failed && !failed.frames && owner->errorCode() == EIO,
                "Cancel concealed the original fault or restarted invalid live output");
        owner->stop();
        require(control->exited.load(), "Processed recovery failed to retire its input producer");
    }
}
void pendingTempoArm()
{
    auto control=std::make_shared<Control>();
    AudioResidentHoldRange range{93,0,4096,4096*AudioResidentHoldRange::bytesPerFrame,
                                 AudioResidentHoldRange::Provenance::DjRange};
    auto owner=PreparedPlayback::makeLive(5006,extent,producer(control),range);
    require(owner->prime(0,std::chrono::seconds(4)),"Pending-arm fixture did not prime");
    RenderCore core(owner->sourceRef(),{});
    require(core.setTempoPercent(0,6) && core.toggleMasterTempo(0) && core.setPlaying(0,true),
            "Pending tempo setup rejected");
    const auto revision=core.transportRevision(0);
    require(core.residentHoldControl(0,AudioResidentHoldCommand::Arm,range.id,revision),
            "Pending live tempo rejected its recovery arm");
    require(core.residentHoldState(0).armed && core.transportRevision(0)==revision+1,
            "Pending recovery arm lost its acknowledgement/revision");
    require(core.residentHoldControl(0,AudioResidentHoldCommand::Cancel,range.id,core.transportRevision(0)) &&
            !core.residentHoldState(0).armed,"Pending arm cancellation failed");
    owner->stop();require(control->exited.load(),"Pending-arm producer did not retire");
}
void rebuildAndStopDuringRecovery()
{
    for (const bool keyLock : {false,true}) {
        auto control=std::make_shared<Control>();
        AudioResidentHoldRange range{95,0,4096,4096*AudioResidentHoldRange::bytesPerFrame,
                                     AudioResidentHoldRange::Provenance::DjRange};
        auto owner=PreparedPlayback::makeLive(5007,extent,producer(control),range);
        require(owner->prime(0,std::chrono::seconds(4)),"Held rebuild fixture did not prime");
        auto source=owner->sourceRef();
        const double percent=keyLock ? -6. : 6.;
        require(source.tempoControl(source.context,AudioTempoCommand::Set,percent,keyLock,0),"Held rebuild tempo setup rejected");
        source.setRenderActive(source.context,true);
        double cursor=0;std::array<float,512> output{};
        while(cursor<1024)(void)read(source,cursor,output);
        auto enter=[&] {
            require(source.residentHoldControl(source.context,AudioResidentHoldCommand::Arm,range.id,std::uint64_t(cursor)),
                    "Held rebuild arm rejected");
            await([&]{return source.residentHoldState(source.context).ready;});
            require(source.residentHoldControl(source.context,AudioResidentHoldCommand::Enter,range.id,std::uint64_t(cursor)),
                    "Held rebuild entry rejected");
            require(read(source,cursor,output).residentHold,"Held rebuild did not consume resident audio");
        };
        enter();
        const auto resume=std::uint64_t(cursor);
        require(owner->primeForSession(resume,std::chrono::seconds(4)),"Active processed recovery could not reconstruct");
        source=owner->sourceRef();cursor=double(resume);
        const auto state=source.tempoState(source.context);
        const auto hold=source.residentHoldState(source.context);
        require(state.active && state.percent==percent && state.keyLock==keyLock &&
                hold.rangeId==range.id && !hold.armed && !hold.active,
                "Reconstruction lost selected tempo/range identity or silently restored recovery consent");
        source.setRenderActive(source.context,true);
        (void)read(source,cursor,output);
        enter();
        const auto began=Clock::now();owner->stop();
        require(control->exited.load() && Clock::now()-began<std::chrono::seconds(1),
                "Shutdown failed to join active live/resident processors and producer");
    }
}
void renderTempoContinuation()
{
    std::array<std::unique_ptr<std::FILE,CloseReference>,4> files;
    std::array<int,4> fds{};
    for(std::size_t role=0;role<4;++role) {
        files[role].reset(std::tmpfile());require(bool(files[role]),"Cannot create continuation bank fixture");
        for(std::uint64_t first=0;first<extent;first+=1024) {
            std::array<float,2048> block{};const auto count=std::min<std::uint64_t>(1024,extent-first);
            for(std::size_t f=0;f<count;++f)for(std::size_t channel=0;channel<2;++channel)
                block[f*2+channel]=sample(role,first+f,channel);
            require(std::fwrite(block.data(),sizeof(float),count*2,files[role].get())==count*2,
                    "Cannot write continuation bank fixture");
        }
        require(!std::fflush(files[role].get()),"Cannot flush continuation bank fixture");fds[role]=fileno(files[role].get());
    }
    for(const bool resident : {false,true})for(const bool keyLock : {false,true})for(const double percent : {-6.,6.}) {
        AudioResidentHoldRange range{92,1234,1234+8193,8193*AudioResidentHoldRange::bytesPerFrame,
            AudioResidentHoldRange::Provenance::DjRange};
        const auto rangeId=resident ? range.id : 0;
        auto control=std::make_shared<Control>();auto owner=PreparedPlayback::makeLive(5007,extent,producer(control),
            resident ? std::optional(range) : std::nullopt);
        require(owner->prime(0,std::chrono::seconds(4)),"Cannot prime live continuation fixture");
        AudioSource peer{10000000,nullptr,nullptr,[](void*,std::uint64_t,std::span<float> pcm)noexcept{
            std::fill(pcm.begin(),pcm.end(),.01F);return AudioReadResult{pcm.size()/2,false};}};
        RenderCore core(owner->sourceRef(),peer);
        require(core.setPlaying(0,true) && core.setPlaying(1,true) && core.setCueSelected(0,true) &&
                core.setDeckGain(0,.5F) && core.setCrossfader(.25F) &&
                core.setTempoPercent(0,percent) && (!keyLock || core.toggleMasterTempo(0)),
                "Cannot select continuation transport/mix mode");
        require(core.setStemGain(0,0,.5F),"Cannot select continuation drums");
        for(std::size_t role=1;role<4;++role)require(core.setStemGain(0,role,0),"Cannot mute unwanted continuation stem");
        std::array<float,514> master{},cue{};
        auto render=[&](std::size_t frames=257){const auto clock=core.frameClock(),peerFrame=core.sourceFrame(1);
            require(core.renderBuses(std::span(master).first(frames*2),std::span(cue).first(frames*2)) &&
                    core.frameClock()==clock+frames && core.sourceFrame(1)==peerFrame+frames,
                    "Continuation disturbed peer/output clocks");};
        await([&]{render();return core.sourcePosition(0)>512 && core.tempoState(0).appliedPercent==percent;});
        if(resident) {
            require(core.residentHoldControl(0,AudioResidentHoldCommand::Arm,range.id,core.transportRevision(0)),
                    "Cannot arm processed continuation recovery");
            await([&]{return core.residentHoldState(0).ready;});
            require(core.residentHoldControl(0,AudioResidentHoldCommand::Enter,range.id,core.transportRevision(0)),
                    "Cannot enter processed continuation recovery");
        }
        control->failure.store(EIO);await([&]{return control->exited.load();});render();
        const auto position=core.sourcePosition(0);const auto revision=core.transportRevision(0);
        require(position!=std::floor(position),"Continuation fixture did not retain a fractional source position");
        PreparedPlayback candidate(fds,extent);
        const auto mode=core.tempoState(0);
        for(const bool wrongKeyLock : {false,true}) {
            auto mismatch=mode;
            if(wrongKeyLock)mismatch.keyLock=!mode.keyLock;
            else mismatch.percent=-mode.percent;
            require(candidate.primeContinuation(position,mismatch,std::chrono::seconds(2)),
                    "Cannot prime mismatched continuation fixture");
            require(core.continuationStatus(0,candidate.sourceRef(),position,revision,rangeId)==RenderCore::ContinuationStatus::Invalid &&
                    core.sourcePosition(0)==position && core.transportRevision(0)==revision &&
                    candidate.primedForContinuation(position),
                    "A prepared bank with another BPM or Master Tempo setting changed the live deck");
        }
        require(candidate.primeContinuation(position,mode,std::chrono::seconds(2)) &&
                candidate.primedForContinuation(position),"Cannot prime tempo-matched continuation bank");
        auto source=candidate.sourceRef();
        require(core.continuationStatus(0,source,position+.125,revision,rangeId)==RenderCore::ContinuationStatus::Invalid &&
                core.continuationStatus(0,source,position,revision+1,rangeId)==RenderCore::ContinuationStatus::Invalid,
                "Stale continuation ticket was accepted");
        require(core.continuationPrefixFrames(RenderCore::deckCount,257)==0 &&
                core.continuationPrefixFrames(0,0)==0,"Invalid recovery prefix request returned audio frames");
        if(resident) {
            require(core.continuationStatus(0,source,position,revision,rangeId)==RenderCore::ContinuationStatus::WaitingForHoldOut,
                    "Processed recovery continuation did not wait for its output boundary");
            await([&]{
                const auto before=core.residentHoldState(0);
                if(before.audibleFrame==before.outFrame)return true;
                const auto count=core.continuationPrefixFrames(0,257);
                require(count>0 && count<=257,"Processed recovery produced an invalid output prefix");
                render(count);
                const auto after=core.residentHoldState(0);
                require(after.active && after.audibleFrame>=before.audibleFrame &&
                        after.audibleFrame<=after.outFrame && core.sourcePosition(0)==position &&
                        candidate.primedForContinuation(position),
                        "Recovery passed OUT or consumed the candidate inside a purported safe output prefix");
                return after.audibleFrame==after.outFrame;
            });
            require(core.continuationPrefixFrames(0,257)==0,"Processed OUT sentinel allowed another old-source read");
        } else require(core.continuationPrefixFrames(0,257)==0,"Unconfigured recovery invented an output boundary");
        require(core.continuationStatus(0,source,position,revision,rangeId)==RenderCore::ContinuationStatus::Ready &&
                candidate.copyStemMixStateFrom(*owner) && core.continueSource(0,source,position,revision,rangeId),
                "Tempo-aware prepared handoff rejected a matching fractional cursor");
        require(core.sourcePosition(0)==position && core.transportRevision(0)==revision+1 &&
                core.tempoState(0).active && core.tempoState(0).percent==percent &&
                core.tempoState(0).keyLock==keyLock && core.deckGain(0)==.5F && core.crossfader()==.25F &&
                core.isCueSelected(0) && source.stemGain(source.context,0)==.5F,
                "Prepared handoff changed position, mode or manual mixer state");
        for(std::size_t role=1;role<4;++role)require(source.stemGain(source.context,role)==0,
                "Prepared handoff restored an unwanted stem role");
        for(std::size_t block=0;block<8;++block){render();require(!candidate.errorCode(),"Prepared handoff source failed");}
        require(core.sourcePosition(0)>position && !candidate.primedForContinuation(position),
                "Prepared handoff failed to consume its primed output");
        candidate.stop();owner->stop();require(control->exited.load(),"Retired continuation producer remained live");
    }
}

void renderProcessedRecovery()
{
    for (const bool keyLock : {false,true}) {
        auto control = std::make_shared<Control>();
        AudioResidentHoldRange range{91,0,4096,4096*AudioResidentHoldRange::bytesPerFrame,
                                     AudioResidentHoldRange::Provenance::DjRange};
        auto owner = PreparedPlayback::makeLive(5005,extent,producer(control),range);
        require(owner->prime(0,std::chrono::seconds(4)), "Rendered processed recovery did not prime");
        AudioSource peer{10000000,nullptr,nullptr,
            [](void*,std::uint64_t,std::span<float> output) noexcept {
                std::fill(output.begin(),output.end(),.01F); return AudioReadResult{output.size()/2,false};
            }};
        RenderCore core(owner->sourceRef(),peer);
        require(core.setPlaying(0,true) && core.setPlaying(1,true) && core.setCueSelected(0,true) &&
                core.setDeckGain(0,.5F) && core.setCrossfader(.25F), "Processed recovery mix setup rejected");
        const double percent = keyLock ? -6. : 6.;
        require(core.setTempoPercent(0,percent) && (!keyLock || core.toggleMasterTempo(0)),
                "Rendered recovery tempo selection rejected");
        std::array<float,512> master{},cue{};
        auto block = [&] {
            const auto clock = core.frameClock(),beforePeer = core.sourceFrame(1);
            require(core.renderBuses(master,cue) && !core.readFailed(0) && !core.readFailed(1),
                    "RenderCore rejected processed resident metadata or audio");
            require(core.frameClock() == clock+256 && core.sourceFrame(1) == beforePeer+256,
                    "Processed recovery interrupted peer/output clocks");
            require(std::ranges::all_of(master,[](float x){return std::isfinite(x);}) &&
                    std::ranges::all_of(cue,[](float x){return std::isfinite(x);}), "Processed recovery emitted nonfinite buses");
        };
        await([&] { block(); return core.sourcePosition(0)>1024 &&
            core.tempoState(0).appliedPercent == percent; });
        require(core.residentHoldControl(0,AudioResidentHoldCommand::Arm,range.id,core.transportRevision(0)),
                "Rendered processed arm rejected");
        await([&] { return core.residentHoldState(0).ready; });
        require(core.residentHoldControl(0,AudioResidentHoldCommand::Enter,range.id,core.transportRevision(0)),
                "Rendered processed entry rejected");
        const auto parked = core.sourcePosition(0);
        const auto revision = core.transportRevision(0);
        require(!core.setTempoPercent(0,-percent) && core.sourcePosition(0)==parked &&
                core.transportRevision(0)==revision && core.tempoState(0).percent==percent,
                "An unsupported held tempo edit partially changed playback");
        for (std::size_t i=0;i<80;++i) {
            block();
            require(core.sourcePosition(0)==parked && core.residentHoldState(0).active,
                    "Rendered resident music advanced or lost the parked fractional cursor");
        }
        require(core.residentHoldControl(0,AudioResidentHoldCommand::Exit,range.id,core.transportRevision(0)),
                "Rendered processed exit rejected");
        await([&] { block(); return !core.residentHoldState(0).active; });
        require(core.sourcePosition(0)>parked && core.tempoState(0).percent==percent &&
                core.tempoState(0).keyLock==keyLock && core.deckGain(0)==.5F &&
                core.crossfader()==.25F && core.isCueSelected(0), "Rendered return changed tempo or manual mix state");
        owner->stop(); require(control->exited.load(), "Rendered recovery producer did not retire");
    }
}
}
int main()
{
    try { pendingTempoArm(); liveControls(); renderCommands(); preservesConfiguredRecovery(); recoveryAtSelectedTempo(); rebuildAndStopDuringRecovery(); renderProcessedRecovery(); renderTempoContinuation(); std::cout << "Live tempo source-clock, gains, seeks, reconstruction and cancellation passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PreparedPlayback.hpp"
#include "audio/RenderCore.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
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
};
PreparedPlayback::LiveProducer producer(std::shared_ptr<Control> control)
{
    return [control](std::stop_token stop, PreparedPlayback::LiveProducerPort& port) {
        std::uint64_t epoch = 0, cursor = 0, target = 0;
        std::array<std::array<float, LiveStemBuffer::blockFrames * 2>, 4> leaves{};
        while (!stop.stop_requested()) {
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
    require(!source.tempoControl && !source.tempoState && source.residentHoldState && source.residentHoldControl,
            "Unqualified tempo/hold combination displaced the configured recovery policy");
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
}
int main()
{
    try { liveControls(); renderCommands(); preservesConfiguredRecovery(); std::cout << "Live tempo source-clock, gains, seeks, reconstruction and cancellation passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

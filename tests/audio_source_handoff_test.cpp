// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/SourceHandoff.hpp"
#include "audio/StemBankSource.hpp"
#include "audio/TransportQueue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace conflux::audio;
using namespace std::chrono_literals;
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

struct Source {
    std::atomic<bool> alive{false};
    std::atomic<unsigned> accesses{0}, expiredAccesses{0};
    float value = 0;
    AudioSource descriptor() noexcept { return {64, nullptr, this, read, nullptr}; }
    static AudioReadResult read(void* data, std::uint64_t, std::span<float> output) noexcept
    {
        auto& source = *static_cast<Source*>(data);
        if (!source.alive.load(std::memory_order_acquire)) ++source.expiredAccesses;
        ++source.accesses;
        std::fill(output.begin(), output.end(), source.value);
        return {output.size() / 2, false};
    }
};
struct Owner {
    Source& source;
    explicit Owner(Source& value) : source(value) { source.alive.store(true, std::memory_order_release); }
    ~Owner() { source.alive.store(false, std::memory_order_release); }
};

void orderingAndQuiesce()
{
    SourceHandoff handoff;
    std::array<float, 4> pcm{.25F, -.5F, .75F, -.25F};
    const SourceHandoff::Request request{1, AudioSource::fromMemory(pcm), nullptr, 7};
    auto untouched = request;
    untouched.generation = 91;
    require(!handoff.readPending(untouched) && untouched.generation == 91,
        "Empty mailbox changed render output");
    require(!handoff.request({2, request.source, nullptr, 7}) &&
            !handoff.request({1, request.source, nullptr, 0}), "Invalid handoff was accepted");
    TransportQueue commands;
    for (std::uint32_t id = 1; id <= 31; ++id)
        require(commands.tryPush({RenderCore::Command::Type::Seek, 1, id, 1, id}), "Old commands did not fit");
    require(commands.tryPush({RenderCore::Command::Type::CueRelease, 1, 0, 1, 32}), "Old CUE release did not fit");
    require(handoff.request(request) && handoff.busy(), "Handoff was not published");
    require(!handoff.request({0, request.source, nullptr, 8}), "Pending descriptor was overwritten");
    require(handoff.readPending(untouched) && untouched.generation == 7 && untouched.source.memory == pcm.data(),
        "Published descriptor changed");
    RenderCore::Command command;
    std::uint32_t last = 0;
    for (std::size_t count = 0; count < TransportQueue::capacity && commands.tryPop(command); ++count)
        last = command.id;
    require(last == 32 && command.type == RenderCore::Command::Type::CueRelease,
        "Pre-publication CUE release did not precede swap boundary");
    SourceHandoff::Completion result;
    result.generation = 99;
    require(!handoff.takeCompletion(result) && result.generation == 99,
        "Pending fade invented a completion");
    require(handoff.readPending(untouched), "Pending source vanished between fade blocks");
    require(handoff.complete(true, {23, 11, true}, 256), "Actual swap was not acknowledged");
    require(!handoff.complete(true, {}, 512), "Same swap was acknowledged twice");
    require(!handoff.request({0, request.source, nullptr, 8}), "Uncollected completion was overwritten");
    require(handoff.takeCompletion(result) && result.installed && result.deck == 1 && result.generation == 7 &&
            result.outputFrame == 256 && result.retired.sourceFrame == 23 && result.retired.starvedFrames == 11 &&
            result.retired.readFailed && result.kind == SourceHandoff::Kind::Replacement && !handoff.busy(),
        "Retired source identity/diagnostics changed");
    require(!handoff.takeCompletion(result), "Completion was delivered twice");
    require(handoff.request({0, request.source, nullptr, 8}), "Collected slot could not be reused");
    handoff.quiesce();
    require(handoff.takeCompletion(result) && !result.installed && result.generation == 8,
        "Quiesced unobserved request was not returned as cancelled");
    handoff.quiesce();
    require(!handoff.takeCompletion(result), "Repeated quiesce invented another completion");
    require(handoff.request({0, request.source, nullptr, 9}) && handoff.readPending(untouched),
        "Observed pending request could not be staged");
    handoff.quiesce();
    require(handoff.takeCompletion(result) && !result.installed && result.generation == 9,
        "Quiesced request in its fade stage was not cancelled safely");
}

void concurrentOwnerLifetime()
{
    constexpr std::size_t changes = 128;
    std::array<Source, changes + 1> sources;
    for (std::size_t index = 0; index < sources.size(); ++index) sources[index].value = float(index);
    auto current = std::make_unique<Owner>(sources[0]);
    SourceHandoff handoff;
    std::atomic<std::uint64_t> observed{0}, permitCompletion{0};
    std::atomic<bool> badSample{false};
    std::jthread render([&](std::stop_token stop) {
        auto active = sources[0].descriptor();
        std::array<float, 2> output{};
        while (!stop.stop_requested()) {
            SourceHandoff::Request pending;
            const bool present = handoff.readPending(pending);
            if (present) {
                observed.store(pending.generation, std::memory_order_release);
                if (permitCompletion.load(std::memory_order_acquire) >= pending.generation) {
                    active = pending.source;
                    (void)active.read(active.context, 0, output);
                    if (output[0] != float(pending.generation)) badSample.store(true);
                    (void)handoff.complete(true, {pending.generation - 1, 0, false}, pending.generation * 256);
                }
            }
            (void)active.read(active.context, 0, output);
            std::this_thread::yield();
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (std::uint64_t generation = 1; generation <= changes; ++generation) {
        auto candidate = std::make_unique<Owner>(sources[generation]);
        require(handoff.request({generation % 2, sources[generation].descriptor(), nullptr, generation}),
            "Fresh source publication failed");
        while (observed.load(std::memory_order_acquire) != generation && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        require(observed.load() == generation, "Render owner did not observe source in finite time");
        SourceHandoff::Completion completion;
        require(!handoff.takeCompletion(completion) && current->source.alive.load() && candidate->source.alive.load(),
            "Owner was retired before final callback use");
        permitCompletion.store(generation, std::memory_order_release);
        bool completed = false;
        while (!(completed = handoff.takeCompletion(completion)) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        require(completed && completion.installed && completion.generation == generation,
            "Render completion was lost/reordered");
        current = std::move(candidate); // old owner expires only after acquire
    }
    render.request_stop();
    render.join();
    require(!badSample.load(), "Published source delivered another generation's samples");
    for (const auto& source : sources)
        require(!source.expiredAccesses.load(), "Render owner accessed storage after retirement");
}
void continuationIdentityAndQuiesce()
{
    SourceHandoff handoff;
    std::array<float, 8> pcm{};
    const SourceHandoff::Request continuation{0, AudioSource::fromMemory(pcm), nullptr, 9,
        SourceHandoff::Kind::Continuation, 7, 42, 3.375};
    auto invalid = continuation;
    for (const double bad : std::array<double,3>{-1.,NAN,INFINITY}) {
        invalid.position=bad;require(!handoff.request(invalid),"Continuation accepted a nonfinite or negative cursor");
    }
    invalid=continuation;
    invalid.position = 4;
    require(!handoff.request(invalid), "Continuation accepted an unprimed EOF cursor");
    invalid = continuation;
    invalid.generation = 7;
    require(!handoff.request(invalid), "Continuation did not advance source generation");
    invalid = continuation;
    invalid.expectedTransportRevision = std::numeric_limits<std::uint64_t>::max();
    require(!handoff.request(invalid), "Continuation accepted a saturated transport revision");
    invalid = continuation;
    invalid.kind = static_cast<SourceHandoff::Kind>(99);
    require(!handoff.request(invalid), "Unknown source handoff kind was published");
    require(handoff.request(continuation), "Valid continuation could not be published");
    SourceHandoff::Request observed;
    require(handoff.readPending(observed) && observed.kind == SourceHandoff::Kind::Continuation &&
            observed.expectedSourceGeneration == 7 && observed.expectedTransportRevision == 42 &&
            observed.position == 3.375 && observed.generation == 9 && observed.residentHoldRangeId == 0,
        "Continuation lost its source/transport/cursor identity");
    require(!handoff.request(continuation), "Pending continuation was overwritten");
    require(handoff.complete(false, {}, 256), "Rejected continuation lost callback completion");
    SourceHandoff::Completion completion;
    require(!handoff.request(continuation) && handoff.takeCompletion(completion) &&
            !completion.installed && completion.kind == SourceHandoff::Kind::Continuation &&
            completion.generation == 9 && completion.outputFrame == 256,
        "Rejected continuation released its owner before completion acquire");
    auto next = continuation;
    next.generation = 10;
    require(handoff.request(next) && handoff.readPending(observed),
        "Collected continuation slot could not be reused");
    handoff.quiesce();
    require(handoff.takeCompletion(completion) && !completion.installed &&
            completion.kind == SourceHandoff::Kind::Continuation && completion.generation == 10,
        "Quiesce lost a continuation owner or falsely installed it");
}

// The actual OUT predicate and PCM split belong to the renderer/session tests.
// This exercises the existing mailbox's owner lifetime while that predicate waits.
void residentContinuationWaitingOwnersAndQuiesce()
{
    for (const bool install : {false, true}) {
        Source retainedSource, candidateSource;
        retainedSource.value = .125F;
        candidateSource.value = -.25F;
        auto retained = std::make_unique<Owner>(retainedSource);
        auto candidate = std::make_unique<Owner>(candidateSource);
        SourceHandoff handoff;
        constexpr std::uint64_t frame = 3, rangeId = 771;
        const SourceHandoff::Request request{0, candidateSource.descriptor(), nullptr, 9,
            SourceHandoff::Kind::Continuation, 7, 42, frame, rangeId};
        auto invalid = request;
        invalid.kind = SourceHandoff::Kind::Replacement;
        require(!handoff.request(invalid), "Ordinary replacement accepted resident-boundary metadata");
        invalid = request; invalid.position = request.source.frames;
        require(!handoff.request(invalid), "Resident continuation accepted finite EOF");
        invalid = request; invalid.generation = request.expectedSourceGeneration;
        require(!handoff.request(invalid), "Resident continuation reused source generation");
        invalid = request; invalid.expectedTransportRevision = std::numeric_limits<std::uint64_t>::max();
        require(!handoff.request(invalid), "Resident continuation accepted exhausted transport identity");
        require(handoff.request(request), "Cannot publish a resident-boundary continuation");
        SourceHandoff::Request observed;
        SourceHandoff::Completion completion;
        std::array<float, 8> pcm{};
        auto oldRef = retainedSource.descriptor();
        for (unsigned block = 0; block < 3; ++block) {
            require(handoff.readPending(observed) && observed.kind == SourceHandoff::Kind::Continuation &&
                observed.residentHoldRangeId == rangeId && observed.position == frame &&
                observed.expectedSourceGeneration == 7 && observed.expectedTransportRevision == 42 &&
                observed.source.context == &candidateSource && handoff.busy(),
                "Waiting continuation changed its source, range or transport ticket");
            require(oldRef.read(oldRef.context, frame, pcm).frames == pcm.size() / 2 &&
                std::ranges::all_of(pcm, [&](float value) { return value == retainedSource.value; }) &&
                !handoff.takeCompletion(completion) && retainedSource.alive && candidateSource.alive &&
                candidateSource.accesses == 0,
                "Waiting for OUT consumed or released an uncommitted owner");
            require(!handoff.request(request), "Waiting resident continuation was overwritten");
        }
        if (install) {
            // Simulate the first new-source use at the selected render boundary.
            require(observed.source.read(observed.source.context, frame, pcm).frames == pcm.size() / 2 &&
                std::ranges::all_of(pcm, [&](float value) { return value == candidateSource.value; }) &&
                handoff.complete(true, {frame, 11, true}, 256),
                "Resident continuation did not acknowledge its final callback use");
        } else {
            handoff.quiesce(); // Callback has stopped before a safe OUT was committed.
        }
        require(retainedSource.alive && candidateSource.alive && !handoff.request(request),
            "Uncollected resident completion released or overwrote an owner");
        require(handoff.takeCompletion(completion) && completion.installed == install &&
            completion.kind == SourceHandoff::Kind::Continuation && completion.generation == 9 &&
            completion.outputFrame == (install ? 256 : 0) && !handoff.busy() &&
            (!install || (completion.retired.sourceFrame == frame && completion.retired.starvedFrames == 11 &&
                          completion.retired.readFailed)),
            "Resident completion lost its kind, ownership result or prior diagnostic");
        if (install) {
            auto retired = std::move(retained);
            retained = std::move(candidate);
            retired.reset();
            require(!retainedSource.alive && candidateSource.alive,
                "Installed resident continuation retired the new owner");
        } else {
            candidate.reset();
            require(retainedSource.alive && !candidateSource.alive && candidateSource.accesses == 0,
                "Quiesced resident continuation stopped the retained owner");
        }
        const auto active = install ? candidateSource.descriptor() : retainedSource.descriptor();
        require(active.read(active.context, frame, pcm).frames == pcm.size() / 2 &&
            retainedSource.expiredAccesses == 0 && candidateSource.expiredAccesses == 0,
            "Resident completion left a released source borrowed by the render owner");
    }
}

void exactContinuationPreservesBothBusesAndRunningMix()
{
    std::array<std::vector<float>, 4> leaves;
    std::array<AudioSource, 4> sources;
    std::vector<float> peer(4096 * 2);
    for (std::size_t stem = 0; stem < 4; ++stem) {
        leaves[stem].resize(peer.size());
        for (std::size_t sample = 0; sample < peer.size(); ++sample)
            leaves[stem][sample] = float(int((sample * (stem + 3) + stem * 11) % 67) - 33) / 256.F;
        sources[stem] = AudioSource::fromMemory(leaves[stem]);
    }
    for (std::size_t i = 0; i < peer.size(); ++i) peer[i] = float(int(i % 13) - 6) / 128.F;
    StemBankSource old(sources), reference(sources);
    for (auto* bank : {&old, &reference}) {
        require(bank->setGain(2, 0) && bank->setGain(3, 0) && bank->configureRamps(240), "Initial stem restriction failed");
    }
    RenderCore changed(old.sourceRef(), AudioSource::fromMemory(peer));
    RenderCore unchanged(reference.sourceRef(), AudioSource::fromMemory(peer));
    for (auto* core : {&changed, &unchanged}) {
        require(core->hotCue(0, 7) && core->setCueFrame(0, 91) && core->setCueSelected(0, true) && core->configureRamps(240),
            "Continuation transport setup failed");
    }
    std::array<float, 600> master{}, cue{}, expectedMaster{}, expectedCue{};
    require(changed.renderBuses(master, cue) && unchanged.renderBuses(expectedMaster, expectedCue) &&
        master == expectedMaster && cue == expectedCue, "Initial continuation reference differs");
    for (auto* core : {&changed, &unchanged}) {
        require(core->setStemGain(0, 0, 0) && core->setStemGain(0, 1, .375F) &&
            core->setEqGainDb(0, 0, -12.F) && core->setEqGainDb(0, 2, 4.F) &&
            core->setDeckGain(0, .4F) && core->setMasterGain(.6F) && core->setCueGain(.7F) &&
            core->setCrossfader(.7F), "Continuation ramp setup failed");
    }
    require(changed.renderBuses(std::span(master).first(146), std::span(cue).first(146)) &&
        unchanged.renderBuses(std::span(expectedMaster).first(146), std::span(expectedCue).first(146)),
        "Partial-ramp reference render failed");
    const auto next = changed.sourceFrame();
    const auto revision = changed.transportRevision(0), clock = changed.frameClock();
    StemBankSource candidate(sources, next);
    require(changed.canContinueSource(0, candidate.sourceRef(), next, revision) &&
        candidate.copyMixStateFrom(old) && changed.continueSource(0, candidate.sourceRef(), next, revision),
        "Exact primed source continuation failed");
    require(changed.sourceFrame() == next && changed.sourcePosition() == double(next) &&
        changed.frameClock() == clock && changed.isPlaying() && changed.cueFrame() == 91 &&
        changed.hotCues(0) == unchanged.hotCues(0) && changed.hotCues(0).set[7] &&
        changed.isCueSelected() && changed.deckGain() == .4F && changed.eqGainDb(0, 0) == -12.F &&
        changed.stemGain(0, 2) == 0 && changed.stemGain(0, 3) == 0 &&
        changed.transportRevision(0) > revision, "Continuation reset channel or transport state");
    for (unsigned block = 0; block < 4; ++block) {
        require(changed.renderBuses(master, cue) && unchanged.renderBuses(expectedMaster, expectedCue) &&
            master == expectedMaster && cue == expectedCue,
            "Continuation changed EQ history, gain-ramp PCM, stereo, cue or peer playback");
    }
    require(changed.sourceFrame() == unchanged.sourceFrame() && changed.sourceFrame(1) == unchanged.sourceFrame(1) &&
        changed.frameClock() == unchanged.frameClock(), "Continuation changed source or output clocks");
}

void continuationRejectsStaleTransportAndGeometry()
{
    std::array<float, 2048> pcm{};
    std::array<AudioSource, 4> sources;
    sources.fill(AudioSource::fromMemory(pcm));
    StemBankSource old(sources), candidate(sources, 31);
    RenderCore core(old.sourceRef());
    require(core.seek(0, 31), "Nonzero continuation setup failed");
    const auto revision = core.transportRevision(0);
    require(core.canContinueSource(0, candidate.sourceRef(), 31, revision), "Valid continuation rejected");
    auto mismatch = candidate.sourceRef(); --mismatch.frames;
    require(!core.continueSource(0, mismatch, 31, revision) &&
        !core.continueSource(0, candidate.sourceRef(), 30, revision) &&
        !core.continueSource(2, candidate.sourceRef(), 31, revision) &&
        !core.continueSource(0, AudioSource::fromMemory(pcm), 31, revision) &&
        !core.continueSource(0, old.sourceRef(), 31, revision) && core.sourceFrame() == 31 &&
        core.transportRevision(0) == revision, "Invalid continuation mutated the deck");
    require(core.seek(0, 47) && core.seek(0, 31) &&
        !core.continueSource(0, candidate.sourceRef(), 31, revision), "Seek-away-and-back reused a stale ticket");
    const auto current = core.transportRevision(0);
    require(!core.seek(0, 1025) && core.transportRevision(0) == current &&
        core.setStemGain(0, 0, .5F) && core.setEqGainDb(0, 1, -6.F) && core.transportRevision(0) == current &&
        core.canContinueSource(0, candidate.sourceRef(), 31, current),
        "Rejected transport or current human mix controls invalidated valid continuation");
    require(core.setPlaying(0, false) && !core.continueSource(0, candidate.sourceRef(), 31, current),
        "Human pause did not invalidate an already-prepared ticket");
}

void continuationKeepsFailureEvidenceAndRejectsPreparedModes()
{
    std::array<float, 512> pcm;
    pcm.fill(.125F);
    struct Failed {
        static AudioReadResult read(void*, std::uint64_t, std::span<float>) noexcept { return {0, true}; }
    };
    AudioSource broken{256, nullptr, nullptr, Failed::read};
    std::array<AudioSource, 4> failedSources; failedSources.fill(broken);
    std::array<AudioSource, 4> sources; sources.fill(AudioSource::fromMemory(pcm));
    StemBankSource old(failedSources), candidate(sources, 31);
    RenderCore core(old.sourceRef(), AudioSource::fromMemory(pcm));
    require(core.seek(0, 31) && core.setCueFrame(0, 17) && core.setCueSelected(0, true), "Failed-source setup failed");
    std::array<float, 16> output{};
    require(core.render(output) && core.readFailed() && core.starvedFrames() == 8 && core.sourceFrame() == 31,
        "Missing live source did not retain exact cursor and failure");
    const auto revision = core.transportRevision(0);
    auto rejected = candidate.sourceRef();
    rejected.tempoControl = [](void*, AudioTempoCommand, double, bool, std::uint64_t) noexcept { return false; };
    rejected.tempoState = [](const void*) noexcept { AudioTempoState s; s.active = true; return s; };
    require(!core.continueSource(0, rejected, 31, revision), "Active prepared tempo was silently reset");
    rejected.tempoState = [](const void*) noexcept { AudioTempoState s; s.pending = true; return s; };
    require(!core.continueSource(0, rejected, 31, revision), "Pending prepared tempo was silently reset");
    rejected = candidate.sourceRef();
    rejected.scratchControl = [](void*, AudioScratchCommand, std::int64_t, double, std::uint64_t) noexcept { return false; };
    rejected.scratchState = [](const void*) noexcept { AudioScratchState s; s.active = true; return s; };
    require(!core.continueSource(0, rejected, 31, revision), "Active scratch was silently reset");
    rejected.scratchState = [](const void*) noexcept { AudioScratchState s; s.pending = true; return s; };
    require(core.canContinueSource(0, rejected, 31, revision),
        "Inactive optional scratch prefetch blocked exact linear continuation");
    rejected = candidate.sourceRef();
    rejected.loopControl = [](void*, AudioLoopCommand, std::uint64_t) noexcept { return false; };
    rejected.loopState = [](const void*) noexcept { AudioLoopState s; s.inSet = true; return s; };
    require(!core.continueSource(0, rejected, 31, revision), "Prepared loop bounds were silently reset");
    require(core.transportRevision(0) == revision && core.readFailed() && core.starvedFrames() == 8 &&
        core.sourceFrame() == 31 && core.sourceFrame(1) == 8 && core.frameClock() == 8,
        "Rejected modes changed an old source, peer or diagnostic");
    require(candidate.copyMixStateFrom(old) && core.continueSource(0, candidate.sourceRef(), 31, revision) &&
        core.render(output) && core.sourceFrame() == 39 && core.sourceFrame(1) == 16 && core.frameClock() == 16 &&
        core.readFailed() && core.starvedFrames() == 8 && !core.isBuffering() && core.cueFrame() == 17 &&
        core.isCueSelected() && core.isPlaying(), "Recovery lost failure evidence or changed transport");
}

} // namespace
int main()
{
    try { orderingAndQuiesce(); continuationIdentityAndQuiesce(); residentContinuationWaitingOwnersAndQuiesce();
        concurrentOwnerLifetime();
        exactContinuationPreservesBothBusesAndRunningMix(); continuationRejectsStaleTransportAndGeometry();
        continuationKeepsFailureEvidenceAndRejectsPreparedModes(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

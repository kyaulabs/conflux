// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/LiveStemBuffer.hpp"
#include "audio/RenderCore.hpp"
#include "audio/StemBankSource.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using conflux::audio::AudioSource;
using conflux::audio::LiveStemBuffer;
using conflux::audio::RenderCore;
using conflux::audio::StemBankSource;
using Result = LiveStemBuffer::PublishResult;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t sourceId = 0x192d8b4a7c601e35ULL;
constexpr float sentinel = 12345.F;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function&& function)
{
    try { function(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid live buffer geometry was accepted");
}

// Dyadic, channel- and role-asymmetric samples make stale frames, swapped roles,
// partial overwrites and accidental clipping independently observable.
float sample(std::uint64_t frame, std::size_t stem, std::size_t channel)
{
    const auto value = (frame * (channel ? 17U : 11U) + stem * 127U + channel * 53U) % 1021U;
    return float(static_cast<int>(value) - 510) / 128.F;
}

struct PacketPcm {
    std::array<std::vector<float>, 4> pcm;
    std::uint64_t first;
    std::size_t frames;
    PacketPcm(std::uint64_t firstFrame, std::size_t frameCount) : first(firstFrame), frames(frameCount)
    {
        for (std::size_t stem = 0; stem < pcm.size(); ++stem) {
            pcm[stem].resize(frames * 2);
            for (std::size_t frame = 0; frame < frames; ++frame)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    pcm[stem][frame * 2 + channel] = sample(first + frame, stem, channel);
        }
    }
    LiveStemBuffer::Packet packet(const LiveStemBuffer::Request& request, bool final = false) const
    {
        return {request.sourceId, request.epoch, first, frames, final,
                {pcm[0], pcm[1], pcm[2], pcm[3]}};
    }
};

LiveStemBuffer::Request requested(const LiveStemBuffer& buffer)
{
    LiveStemBuffer::Request request{};
    require(buffer.tryRequest(request), "A quiescent request could not be read coherently");
    return request;
}

void readExact(AudioSource source, std::size_t stem, std::uint64_t first,
               std::size_t wanted, std::size_t expected)
{
    std::vector<float> output(wanted * 2, sentinel);
    const auto result = source.read(source.context, first, output);
    require(!result.failed && result.frames == expected, "A live view supplied the wrong contiguous prefix");
    for (std::size_t frame = 0; frame < expected; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel)
            require(output[frame * 2 + channel] == sample(first + frame, stem, channel),
                    "A live view changed role, channel, frame, headroom or packet bytes");
    require(std::ranges::all_of(std::span(output).subspan(expected * 2),
                               [](float value) { return value == sentinel; }),
            "A live view wrote beyond its available prefix");
}

void packetValidationIsNonmutating()
{
    rejects([] { auto buffer = std::make_unique<LiveStemBuffer>(sourceId, 0); });
    rejects([] { auto buffer = std::make_unique<LiveStemBuffer>(0, 32); });
    rejects([] { auto buffer = std::make_unique<LiveStemBuffer>(sourceId, std::numeric_limits<std::uint64_t>::max()); });
    auto buffer = std::make_unique<LiveStemBuffer>(sourceId, 24);
    const auto request = requested(*buffer);
    require(request.sourceId == sourceId && request.epoch == 1 && request.firstFrame == 0,
            "Initial source/epoch/origin differs");
    const auto views = buffer->sourceRefs();
    for (const auto& view : views)
        require(view.frames == 24 && !view.memory && view.context && view.read && view.discontinuity &&
                    !view.setStemGain && !view.stemGain,
                "A live view does not expose the fixed borrowed PCM contract");
    PacketPcm data(0, 8);
    const auto valid = data.packet(request);
    auto invalid = [&](LiveStemBuffer::Packet packet) {
        require(buffer->tryPublish(packet) == Result::Invalid, "A malformed live packet was accepted");
        for (std::size_t stem = 0; stem < 4; ++stem) readExact(views[stem], stem, 0, 1, 0);
        const auto after = requested(*buffer);
        require(after.epoch == request.epoch && after.firstFrame == 0 && after.sourceId == sourceId,
                "A rejected packet changed the request generation");
    };
    auto bad = valid;
    bad.sourceId ^= 1;
    invalid(bad);
    bad = valid; bad.frames = 0; invalid(bad);
    bad = valid; bad.frames = LiveStemBuffer::blockFrames + 1; invalid(bad);
    bad = valid; bad.firstFrame = 1; invalid(bad);
    bad = valid; bad.firstFrame = std::numeric_limits<std::uint64_t>::max(); invalid(bad);
    bad = valid; bad.final = true; invalid(bad);
    for (std::size_t stem = 0; stem < 4; ++stem) {
        bad = valid; bad.stems[stem] = {}; invalid(bad);
        bad = valid; bad.stems[stem] = std::span(data.pcm[stem]).first(15); invalid(bad);
        const auto original = data.pcm[stem].back();
        for (const auto value : {std::numeric_limits<float>::quiet_NaN(),
                                 std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity()}) {
            data.pcm[stem].back() = value;
            invalid(data.packet(request));
        }
        data.pcm[stem].back() = original;
    }
    for (const auto epoch : {std::uint64_t(0), request.epoch + 1}) {
        bad = valid; bad.epoch = epoch;
        require(buffer->tryPublish(bad) == Result::Stale, "A foreign generation was not rejected");
    }
    require(buffer->tryPublish(valid) == Result::Accepted, "Rejected packets advanced the producer cursor");
    for (auto& pcm : data.pcm) std::ranges::fill(pcm, -999.F);
    for (std::size_t stem = 0; stem < 4; ++stem) readExact(views[stem], stem, 0, 9, 8);
    PacketPcm next(8, 8);
    bad = next.packet(request); bad.firstFrame = 7;
    require(buffer->tryPublish(bad) == Result::Invalid, "An overlapping packet was accepted");
    bad.firstFrame = 9;
    require(buffer->tryPublish(bad) == Result::Invalid, "A gap in the producer prefix was accepted");
    require(buffer->tryPublish(next.packet(request)) == Result::Accepted,
            "Overlap/gap rejection advanced the expected producer frame");
    PacketPcm tail(16, 8);
    require(buffer->tryPublish(tail.packet(request)) == Result::Invalid,
            "Exact EOF without its terminal marker was accepted");
    require(buffer->tryPublish(tail.packet(request, true)) == Result::Accepted,
            "The exact terminal bank packet was rejected");
    require(buffer->tryPublish(tail.packet(request, true)) == Result::Invalid,
            "An already-published EOF was accepted twice");
    for (std::size_t stem = 0; stem < 4; ++stem) {
        readExact(views[stem], stem, 8, 20, 16);
        readExact(views[stem], stem, 24, 1, 0);
    }
}

void readsValidateBeforeChangingStorageOrPosition()
{
    auto buffer = std::make_unique<LiveStemBuffer>(sourceId, 16);
    const auto request = requested(*buffer);
    const auto views = buffer->sourceRefs();
    PacketPcm data(0, 16);
    require(buffer->tryPublish(data.packet(request, true)) == Result::Accepted, "Read validation packet rejected");
    std::array<float, 3> odd{sentinel, sentinel, sentinel};
    require(views[0].read(views[0].context, 0, odd).failed &&
                odd == std::array<float, 3>{sentinel, sentinel, sentinel},
            "An incomplete stereo read wrote or consumed PCM");
    std::array<float, 2> one{sentinel, sentinel};
    require(views[0].read(views[0].context, 1, one).failed && one[0] == sentinel && one[1] == sentinel,
            "An out-of-order view read wrote or consumed PCM");
    std::vector<float> tooLarge((LiveStemBuffer::blockFrames + 1) * 2, sentinel);
    require(views[0].read(views[0].context, 0, tooLarge).failed &&
                std::ranges::all_of(tooLarge, [](float value) { return value == sentinel; }),
            "An oversized read exceeded its bound or consumed PCM");
    require(!views[0].read(views[0].context, 0, {}).failed, "An empty read failed");
    for (std::size_t stem = 0; stem < 4; ++stem) readExact(views[stem], stem, 0, 19, 16);
    const auto before = requested(*buffer);
    views[0].discontinuity(views[0].context, 17);
    const auto after = requested(*buffer);
    require(after.epoch == before.epoch && after.firstFrame == before.firstFrame,
            "An out-of-range discontinuity changed the bank request");
}

void capacityWaitsForEveryViewAndRetainsFullPackets()
{
    constexpr auto capacity = LiveStemBuffer::slotCount;
    static_assert(capacity == 72 && LiveStemBuffer::blockFrames == 4096);
    auto buffer = std::make_unique<LiveStemBuffer>(sourceId, capacity + 2);
    const auto request = requested(*buffer);
    const auto views = buffer->sourceRefs();
    for (std::size_t frame = 0; frame < capacity; ++frame) {
        PacketPcm data(frame, 1);
        require(buffer->tryPublish(data.packet(request)) == Result::Accepted, "The documented fixed ring filled early");
    }
    PacketPcm next(capacity, 1), tail(capacity + 1, 1);
    require(buffer->tryPublish(next.packet(request)) == Result::Full, "An occupied ring was overwritten");
    for (std::size_t stem = 0; stem < 3; ++stem) readExact(views[stem], stem, 0, capacity, capacity);
    require(buffer->tryPublish(next.packet(request)) == Result::Full,
            "A slot was reused before the fourth view copied its prefix");
    readExact(views[3], 3, 0, 1, 1);
    require(buffer->tryPublish(next.packet(request)) == Result::Accepted,
            "A fully released slot was not reusable or Full advanced publication");
    require(buffer->tryPublish(tail.packet(request, true)) == Result::Full,
            "One released slot incorrectly freed a second occupied bank packet");
    readExact(views[3], 3, 1, 1, 1);
    require(buffer->tryPublish(tail.packet(request, true)) == Result::Accepted,
            "Full terminal publication changed its cursor or EOF state");
    for (std::size_t stem = 0; stem < 3; ++stem) readExact(views[stem], stem, capacity, 4, 2);
    readExact(views[3], 3, 2, capacity + 2, capacity);
    for (std::size_t stem = 0; stem < 4; ++stem) readExact(views[stem], stem, capacity + 2, 1, 0);
}

void wholeHopFitsAndPreservesPacketEdges()
{
    // One converted model hop is 280,800 frames. It must fit before rendering;
    // every full-size packet and the asymmetric final tail remain exact.
    constexpr std::uint64_t frames = 280800;
    auto buffer = std::make_unique<LiveStemBuffer>(sourceId, frames);
    const auto request = requested(*buffer);
    PacketPcm oversized(0, LiveStemBuffer::blockFrames + 1);
    require(buffer->tryPublish(oversized.packet(request)) == Result::Invalid,
            "A correctly shaped oversized packet exceeded fixed slot storage");
    std::size_t packets = 0;
    for (std::uint64_t first = 0; first < frames;) {
        const auto count = std::size_t(std::min<std::uint64_t>(LiveStemBuffer::blockFrames, frames - first));
        PacketPcm data(first, count);
        require(buffer->tryPublish(data.packet(request, first + count == frames)) == Result::Accepted,
                "A normal converted hop did not fit in the documented fixed ring");
        first += count;
        ++packets;
    }
    require(packets == 69 && buffer->sourceId() == sourceId && buffer->frames() == frames &&
                LiveStemBuffer::pcmBytes == 9437184,
            "The live source identity, exact length or fixed PCM bound changed");
    StemBankSource bank(buffer->sourceRefs());
    RenderCore core(bank.sourceRef());
    constexpr std::array<float, 4> gains{1.F, 1.F, 1.F, 1.F};
    constexpr std::array<std::size_t, 4> partitions{257, 1024, 511, 13};
    std::array<float, 2048> output{};
    for (std::uint64_t first = 0, part = 0; first < frames + 7; ++part) {
        const auto count = std::size_t(std::min<std::uint64_t>(partitions[part % partitions.size()], frames + 7 - first));
        require(core.render(std::span(output).first(count * 2)), "Partitioned whole-hop rendering failed");
        for (std::size_t frame = 0; frame < count; ++frame)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                double sum = 0;
                if (first + frame < frames)
                    for (std::size_t stem = 0; stem < 4; ++stem)
                        sum += double(sample(first + frame, stem, channel)) * gains[stem];
                require(output[frame * 2 + channel] == float(sum),
                        "A full-size packet edge, ring span or final tail changed PCM");
            }
        first += count;
    }
    require(core.sourceFrame() == frames && core.frameClock() == frames + 7 &&
                !core.isBuffering() && !core.starvedFrames() && !core.readFailed(),
            "A fully published hop starved or produced a guessed EOF frame");
}

void fullRingSeekCoalescesAndRejectsInflightGenerations()
{
    auto buffer = std::make_unique<LiveStemBuffer>(sourceId, 512);
    const auto initial = requested(*buffer);
    const auto views = buffer->sourceRefs();
    for (std::size_t frame = 0; frame < LiveStemBuffer::slotCount; ++frame) {
        PacketPcm data(frame, 1);
        require(buffer->tryPublish(data.packet(initial)) == Result::Accepted, "Seek fixture filled early");
    }
    PacketPcm inflight(72, 1);
    require(buffer->tryPublish(inflight.packet(initial)) == Result::Full, "Seek fixture was not full");
    views[0].discontinuity(views[0].context, 100);
    const auto firstNotification = requested(*buffer);
    require(firstNotification.epoch == initial.epoch + 1 && firstNotification.firstFrame == 100 &&
                firstNotification.sourceId == sourceId,
            "A render seek did not publish one coherent new request");
    PacketPcm fresh(100, 7);
    require(buffer->tryPublish(fresh.packet(firstNotification)) == Result::Accepted,
            "Seeking a full paused ring did not release its stale storage");
    for (std::size_t stem = 1; stem < 4; ++stem) views[stem].discontinuity(views[stem].context, 100);
    const auto coalesced = requested(*buffer);
    require(coalesced.epoch == firstNotification.epoch && coalesced.firstFrame == 100,
            "One bank seek was split into four producer epochs");
    require(buffer->tryPublish(inflight.packet(initial)) == Result::Stale,
            "An in-flight old packet survived a full-ring seek");
    for (std::size_t stem = 0; stem < 4; ++stem) readExact(views[stem], stem, 100, 9, 7);

    // A later logical retry at the same frame must invalidate an old worker
    // result, while the four callbacks within that retry still coalesce.
    for (std::size_t stem = 0; stem < 4; ++stem) views[stem].discontinuity(views[stem].context, 100);
    const auto retry = requested(*buffer);
    require(retry.epoch == coalesced.epoch + 1 && retry.firstFrame == 100,
            "A complete same-position retry reused an old worker generation");
    require(buffer->tryPublish(fresh.packet(coalesced)) == Result::Stale,
            "A late completion survived a same-position retry");
    for (std::size_t stem = 0; stem < 4; ++stem) readExact(views[stem], stem, 100, 1, 0);
    require(buffer->tryPublish(fresh.packet(retry)) == Result::Accepted, "A new same-position generation could not recover");
    for (std::size_t stem = 0; stem < 4; ++stem) readExact(views[stem], stem, 100, 7, 7);

    StemBankSource bank(views);
    const auto source = bank.sourceRef();
    source.discontinuity(source.context, 512);
    const auto eof = requested(*buffer);
    require(eof.epoch == retry.epoch + 1 && eof.firstFrame == 512,
            "A seek to exact EOF did not retain its authoritative length");
    std::array<float, 2> output{sentinel, sentinel};
    const auto ended = source.read(source.context, 512, output);
    require(!ended.failed && !ended.frames && output[0] == sentinel && output[1] == sentinel,
            "Exact EOF required a fabricated zero-length data packet or wrote a stale suffix");
}

void producerWorkHeldAcrossSeekIsStale()
{
    auto buffer = std::make_unique<LiveStemBuffer>(sourceId, 8192);
    StemBankSource bank(buffer->sourceRefs());
    const auto source = bank.sourceRef();
    std::atomic<bool> captured{false}, release{false}, done{false}, valid{false};
    std::atomic<Result> outcome{Result::Invalid};
    std::jthread producer([&](std::stop_token stop) {
        LiveStemBuffer::Request request{};
        if (!buffer->tryRequest(request)) { done.store(true); return; }
        PacketPcm prepared(0, 4096);
        valid.store(request.sourceId == sourceId && request.epoch == 1 && request.firstFrame == 0);
        captured.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire) && !stop.stop_requested()) std::this_thread::yield();
        if (!stop.stop_requested()) outcome.store(buffer->tryPublish(prepared.packet(request)));
        done.store(true, std::memory_order_release);
    });
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!captured.load(std::memory_order_acquire) && !done.load(std::memory_order_acquire) && Clock::now() < deadline)
        std::this_thread::yield();
    require(captured.load() && valid.load(), "The producer did not retain its old request before the test seek");
    source.discontinuity(source.context, 6000);
    const auto request = requested(*buffer);
    require(request.epoch == 2 && request.firstFrame == 6000, "Bank seek did not coalesce while a producer job was in flight");
    release.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire) && Clock::now() < deadline) std::this_thread::yield();
    require(done.load() && outcome.load() == Result::Stale, "A producer job captured before the seek published afterward");
    producer.join();
    std::array<float, 32> output{};
    output.fill(sentinel);
    const auto absent = source.read(source.context, 6000, output);
    require(!absent.failed && !absent.frames && std::ranges::all_of(output, [](float value) { return value == sentinel; }),
            "A late old producer job supplied material at the new source cursor");
    PacketPcm recovery(6000, 16);
    require(buffer->tryPublish(recovery.packet(request)) == Result::Accepted, "Fresh publication after in-flight invalidation failed");
    const auto recovered = source.read(source.context, 6000, output);
    require(!recovered.failed && recovered.frames == 16, "The new epoch could not recover after stale work");
    for (std::size_t frame = 0; frame < 16; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            double expected = 0;
            for (std::size_t stem = 0; stem < 4; ++stem) expected += sample(6000 + frame, stem, channel);
            require(output[frame * 2 + channel] == float(expected), "Recovery mixed an old or wrong-role producer packet");
        }
}

void concurrentPublicationAndRepeatedSeeksStayCoherent()
{
    constexpr std::uint64_t total = 32768;
    auto buffer = std::make_unique<LiveStemBuffer>(sourceId, total);
    StemBankSource bank(buffer->sourceRefs());
    const auto source = bank.sourceRef();
    std::atomic<bool> failed{false};
    std::atomic<std::uint64_t> published{0};
    std::jthread producer([&](std::stop_token stop) {
        try {
            std::uint64_t epoch = 0, next = 0;
            while (!stop.stop_requested()) {
                LiveStemBuffer::Request request{};
                if (!buffer->tryRequest(request)) { std::this_thread::yield(); continue; }
                const auto expectedOrigin = request.epoch == 1 ? 0 : ((request.epoch - 2) * 7919 + 17) % 16000;
                if (request.sourceId != sourceId || !request.epoch || request.epoch > 33 ||
                    request.firstFrame != expectedOrigin) { failed.store(true); break; }
                if (request.epoch != epoch) { epoch = request.epoch; next = request.firstFrame; }
                if (next == total) { std::this_thread::yield(); continue; }
                const auto count = std::size_t(std::min<std::uint64_t>(29, total - next));
                PacketPcm data(next, count);
                const auto result = buffer->tryPublish(data.packet(request, next + count == total));
                if (result == Result::Accepted) {
                    next += count;
                    published.fetch_add(1, std::memory_order_relaxed);
                } else if (result == Result::Invalid) { failed.store(true); break; }
                else std::this_thread::yield();
            }
        } catch (...) { failed.store(true); }
    });
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    std::array<float, 126> output{};
    for (std::uint64_t round = 0; round < 32; ++round) {
        const auto target = (round * 7919 + 17) % 16000;
        source.discontinuity(source.context, target);
        const auto request = requested(*buffer);
        require(request.epoch == round + 2 && request.firstFrame == target,
                "Concurrent request observation was torn or one bank seek created multiple epochs");
        std::uint64_t cursor = target;
        std::size_t consumed = 0;
        while (consumed < 504) {
            require(!failed.load() && Clock::now() < deadline,
                    "Concurrent live publication failed or did not make bounded progress");
            output.fill(sentinel);
            const auto result = source.read(source.context, cursor, output);
            require(!result.failed && result.frames <= output.size() / 2,
                    "A concurrent read failed or reported an impossible prefix");
            for (std::size_t frame = 0; frame < result.frames; ++frame)
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    double expected = 0;
                    for (std::size_t stem = 0; stem < 4; ++stem)
                        expected += sample(cursor + frame, stem, channel);
                    require(output[frame * 2 + channel] == float(expected),
                            "Concurrent seek/copy mixed epochs, roles or overwritten slot data");
                }
            require(std::ranges::all_of(std::span(output).subspan(result.frames * 2),
                                       [](float value) { return value == sentinel; }),
                    "A concurrent read wrote an unpublished suffix");
            cursor += result.frames;
            consumed += result.frames;
            if (!result.frames) std::this_thread::yield();
        }
    }
    producer.request_stop();
    producer.join();
    require(!failed.load() && published.load() > 32,
            "Concurrent SPSC exercise did not publish real current-epoch PCM");
}

float mixed(std::uint64_t frame, std::size_t channel, const std::array<float, 4>& gains)
{
    double value = 0;
    for (std::size_t stem = 0; stem < 4; ++stem) value += double(sample(frame, stem, channel)) * gains[stem];
    return float(value);
}

void liveBankShortagesPreservePeerAndStemAlignment()
{
    auto buffer = std::make_unique<LiveStemBuffer>(sourceId, 16);
    const auto request = requested(*buffer);
    StemBankSource bank(buffer->sourceRefs());
    std::array<float, 128> peer{};
    peer.fill(.5F);
    RenderCore core(bank.sourceRef(), AudioSource::fromMemory(peer));
    std::array<float, 4> gains{.5F, .25F, 0.F, 1.F};
    for (std::size_t stem = 0; stem < 4; ++stem) require(core.setStemGain(0, stem, gains[stem]), "Stem gain setup failed");
    require(core.setDeckGain(0, .5F) && core.setMasterGain(.5F) && core.setCueSelected(0, true) && core.setCueGain(.25F),
            "Independent master/cue setup failed");
    PacketPcm first(0, 3);
    require(buffer->tryPublish(first.packet(request)) == Result::Accepted, "Initial live prefix rejected");
    std::array<float, 16> master{}, cue{};
    require(core.renderBuses(master, cue) && core.sourceFrame(0) == 3 && core.sourceFrame(1) == 8 &&
                core.frameClock() == 8 && core.starvedFrames(0) == 5 && core.isBuffering(0) && !core.readFailed(0),
            "A short live bank advanced missing PCM or stalled the peer/output clock");
    for (std::size_t frame = 0; frame < 8; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto value = frame < 3 ? mixed(frame, channel, gains) : 0.F;
            require(master[frame * 2 + channel] == float((double(value) * .5 + .5) * .5) &&
                        cue[frame * 2 + channel] == value * .25F,
                    "A live shortage leaked a stem/peer to the wrong bus or damaged its available prefix");
        }
    gains[2] = 1.F;
    require(core.setStemGain(0, 2, 1.F), "Cannot unmute the live other stem");
    PacketPcm second(3, 5);
    require(buffer->tryPublish(second.packet(request)) == Result::Accepted, "Recovered live prefix rejected");
    require(core.renderBuses(master, cue) && core.sourceFrame(0) == 8 && core.sourceFrame(1) == 16 &&
                core.frameClock() == 16 && core.starvedFrames(0) == 8,
            "A recovered live bank lost its stopped common cursor");
    for (std::size_t frame = 0; frame < 8; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto value = frame < 5 ? mixed(frame + 3, channel, gains) : 0.F;
            require(master[frame * 2 + channel] == float((double(value) * .5 + .5) * .5) &&
                        cue[frame * 2 + channel] == value * .25F,
                    "Unmuting a previously unavailable role changed its shared frame alignment");
        }
    PacketPcm final(8, 8);
    require(buffer->tryPublish(final.packet(request, true)) == Result::Accepted, "Final live prefix rejected");
    std::array<float, 20> tailMaster{}, tailCue{};
    require(core.renderBuses(tailMaster, tailCue) && core.sourceFrame(0) == 16 && core.sourceFrame(1) == 26 &&
                core.frameClock() == 26 && core.starvedFrames(0) == 8 && !core.isBuffering(0) && !core.readFailed(0),
            "Exact live EOF was treated as an unpublished shortage");
    for (std::size_t frame = 0; frame < 10; ++frame)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto value = frame < 8 ? mixed(frame + 8, channel, gains) : 0.F;
            require(tailMaster[frame * 2 + channel] == float((double(value) * .5 + .5) * .5) &&
                        tailCue[frame * 2 + channel] == value * .25F,
                    "Final live tail leaked a stale suffix or used a different stem cursor");
        }
    require(core.deckGain(0) == .5F && core.masterGain() == .5F && core.cueGain() == .25F &&
                core.isCueSelected(0) && core.stemGain(0, 2) == 1.F,
            "Live publication/shortage/EOF changed acknowledged transport gains");
}

} // namespace

int main()
{
    try {
        packetValidationIsNonmutating();
        readsValidateBeforeChangingStorageOrPosition();
        capacityWaitsForEveryViewAndRetainsFullPackets();
        wholeHopFitsAndPreservesPacketEdges();
        fullRingSeekCoalescesAndRejectsInflightGenerations();
        producerWorkHeldAcrossSeekIsStale();
        concurrentPublicationAndRepeatedSeeksStayCoherent();
        liveBankShortagesPreservePeerAndStemAlignment();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Live four-stem packet bounds, release, prefix and rendering behavior passed\n";
    return 0;
}

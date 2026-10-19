// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/RenderCore.hpp"
#include "audio/TransportQueue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using conflux::audio::RenderCore;
using conflux::audio::TransportQueue;
using Command = RenderCore::Command;
using Type = Command::Type;

bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

struct State {
    std::uint64_t clock;
    std::array<std::uint64_t, 2> cursor;
    std::array<std::uint64_t, 2> frames;
    std::array<bool, 2> playing;
    std::array<float, 2> gain;
    float master;
    bool operator==(const State&) const = default;
};

State state(const RenderCore& core)
{
    return {core.frameClock(), {core.sourceFrame(0), core.sourceFrame(1)},
            {core.sourceFrames(0), core.sourceFrames(1)},
            {core.isPlaying(0), core.isPlaying(1)},
            {core.deckGain(0), core.deckGain(1)}, core.masterGain()};
}

constexpr std::array<float, 10> sourceA{
    .125F, -.25F, .375F, .5F, -.625F, .75F, .875F, -1.F, 1.125F, -1.25F};
constexpr std::array<float, 6> sourceB{1.F, -.5F, -.25F, .125F, .5F, .75F};

std::vector<float> source(std::size_t frames, std::size_t seed)
{
    std::vector<float> pcm;
    pcm.reserve(frames * RenderCore::channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        // Dyadic values make exact sample comparisons independent of tolerance.
        pcm.push_back(static_cast<float>(static_cast<int>((frame * 7 + seed) % 33) - 16) / 16.F);
        pcm.push_back(static_cast<float>(static_cast<int>((frame * 13 + seed * 3) % 31) - 15) / 16.F);
    }
    return pcm;
}

bool mixingAndIndependentTransport()
{
    bool passed = true;
    RenderCore mixed(sourceA, sourceB);
    passed &= require(RenderCore::deckCount == 2 && mixed.sourceFrames() == 5 &&
                          mixed.sourceFrames(1) == 3 && mixed.isPlaying(0) && mixed.isPlaying(1),
                      "two decks did not start with independent loaded sources");
    std::array<float, 10> output{};
    constexpr std::array<float, 10> sum{
        1.125F, -.75F, .125F, .625F, -.125F, 1.5F, .875F, -1.F, 1.125F, -1.25F};
    passed &= require(mixed.render(output) && output == sum && mixed.sourceFrame() == 5 &&
                          mixed.sourceFrame(1) == 3 && mixed.frameClock() == 5,
                      "independent stereo sum or EOF differs");
    passed &= require(mixed.isPlaying(0) && mixed.isPlaying(1), "EOF changed requested play state");
    output.fill(42.F);
    passed &= require(mixed.render(output) && std::ranges::all_of(output, [](float x) { return x == 0.F; }) &&
                          mixed.sourceFrame() == 5 && mixed.sourceFrame(1) == 3 && mixed.frameClock() == 10,
                      "independent EOF did not produce clocked silence");

    RenderCore gained(sourceA, sourceB);
    constexpr std::array<float, 10> scaled{
        .15625F, -.125F, .0625F, .140625F, -.09375F, .28125F,
        .21875F, -.25F, .28125F, -.3125F};
    passed &= require(gained.setDeckGain(0, .5F) && gained.setDeckGain(1, .25F) &&
                          gained.setMasterGain(.5F) && gained.render(output) && output == scaled,
                      "deck/master gain or stereo order differs");

    RenderCore muted(sourceA, sourceB);
    std::array<float, 4> mutedBlock{};
    passed &= require(muted.setDeckGain(0, 0) && muted.setPlaying(1, false) &&
                          muted.render(mutedBlock) && muted.sourceFrame(0) == 2 &&
                          muted.sourceFrame(1) == 0 &&
                          std::ranges::all_of(mutedBlock, [](float sample) { return sample == 0; }),
                      "muted playback did not advance independently of a paused deck");
    passed &= require(muted.setDeckGain(0, 1) && muted.render(mutedBlock) &&
                          mutedBlock == std::array<float, 4>{-.625F, .75F, .875F, -1.F},
                      "unmuting restarted or skipped source samples");

    RenderCore transport(sourceA, sourceB);
    std::array<float, 4> twoFrames{};
    passed &= require(transport.setPlaying(1, false) && transport.render(twoFrames) &&
                          twoFrames == std::array<float, 4>{.125F, -.25F, .375F, .5F} &&
                          transport.sourceFrame(0) == 2 && transport.sourceFrame(1) == 0,
                      "paused deck advanced or contributed samples");
    passed &= require(transport.seek(1, 1) && !transport.isPlaying(1) && transport.render(twoFrames) &&
                          twoFrames == std::array<float, 4>{-.625F, .75F, .875F, -1.F} &&
                          transport.sourceFrame(0) == 4 && transport.sourceFrame(1) == 1,
                      "owner seek changed paused state or the other deck");
    passed &= require(transport.setPlaying(0, false) && transport.setPlaying(1, true) &&
                          transport.render(output) &&
                          output == std::array<float, 10>{-.25F, .125F, .5F, .75F, 0.F, 0.F, 0.F, 0.F, 0.F, 0.F} &&
                          transport.sourceFrame(0) == 4 && transport.sourceFrame(1) == 3 &&
                          transport.frameClock() == 9 && transport.isPlaying(1),
                      "one deck EOF stopped/advanced the paused peer or output clock");
    passed &= require(transport.setPlaying(0, true) && transport.render(twoFrames) &&
                          twoFrames == std::array<float, 4>{1.125F, -1.25F, 0.F, 0.F} &&
                          transport.sourceFrame() == 5 && transport.frameClock() == 11,
                      "resuming the remaining deck did not preserve its cursor");
    return passed;
}

bool numericHeadroomAndFiniteOverflow()
{
    bool passed = true;
    const auto maximum = std::numeric_limits<float>::max();
    const std::array<float, 2> extremes{maximum, -maximum};
    RenderCore muted(extremes, extremes);
    std::array<float, 2> output{42.F, 42.F};
    passed &= require(muted.setMasterGain(0.F) && muted.render(output) &&
                          output == std::array<float, 2>{0.F, 0.F} &&
                          std::isfinite(output[0]) && std::isfinite(output[1]),
                      "master mute of finite overflowing deck sum produced nonfinite output");
    RenderCore saturated(extremes, extremes);
    passed &= require(saturated.render(output) && output == extremes &&
                          std::isfinite(output[0]) && std::isfinite(output[1]),
                      "finite overflow was not bounded at representable float limits");
    RenderCore scaled(extremes, extremes);
    const std::array<float, 2> halfMaximum{maximum * .5F, -maximum * .5F};
    passed &= require(scaled.setMasterGain(.25F) && scaled.render(output) && output == halfMaximum,
                      "deck sum was clipped before master scaling");
    const std::array<float, 2> headroomA{1.25F, -1.5F}, headroomB{1.5F, -1.75F};
    RenderCore headroom(headroomA, headroomB);
    passed &= require(headroom.render(output) && output == std::array<float, 2>{2.75F, -3.25F},
                      "ordinary finite headroom was limited to nominal audio range");

    const std::array<float, 4> a{.1234567F, -.9876543F, .3141592F, -.2718281F};
    const std::array<float, 4> b{-.3333333F, .7654321F, -.1428571F, .1111111F};
    constexpr float gainA = .37F, gainB = .61F, master = .73F;
    std::array<float, 4> expected{}, actual{};
    for (std::size_t sample = 0; sample < expected.size(); ++sample) {
        // Public sample/gain values are floats; the contract evaluates the mix
        // and master multiplication before the final single float conversion.
        const double first = static_cast<double>(a[sample]) * static_cast<double>(gainA);
        const double second = static_cast<double>(b[sample]) * static_cast<double>(gainB);
        expected[sample] = static_cast<float>((first + second) * static_cast<double>(master));
    }
    RenderCore nonDyadic(a, b);
    passed &= require(nonDyadic.setDeckGain(0, gainA) && nonDyadic.setDeckGain(1, gainB) &&
                          nonDyadic.setMasterGain(master) && nonDyadic.render(actual) && actual == expected,
                      "non-dyadic gains rounded before the final float sample");

    const std::array<float, 4> signedZero{-0.F, 0.F, -.5F, .5F};
    RenderCore legacy(signedZero);
    std::array<float, 4> copied{};
    passed &= require(legacy.render(copied), "legacy unity render failed");
    for (std::size_t sample = 0; sample < copied.size(); ++sample)
        passed &= require(std::bit_cast<std::uint32_t>(copied[sample]) ==
                              std::bit_cast<std::uint32_t>(signedZero[sample]),
                          "legacy unity render changed signed zero or source bits");
    return passed;
}

bool commandsAndRejectedState()
{
    bool passed = true;
    RenderCore commanded(sourceA, sourceB);
    for (const Command command : {
             Command{Type::Pause, 0}, Command{Type::DeckGain, 0, 0, .25F},
             Command{Type::MasterGain, 123, 0, .5F}, Command{Type::Seek, 1, 2},
             Command{Type::Play, 0}, Command{Type::Pause, 1}})
        passed &= require(commanded.apply(command), "valid owner transport command rejected");
    std::array<float, 6> block{};
    passed &= require(commanded.render(block) &&
                          block == std::array<float, 6>{.015625F, -.03125F, .046875F, .0625F, -.078125F, .09375F} &&
                          commanded.sourceFrame() == 3 && commanded.sourceFrame(1) == 2 &&
                          commanded.frameClock() == 3 && !commanded.isPlaying(1),
                      "owner command effects differ from expected samples/state");
    passed &= require(commanded.apply({Type::Pause, 0}) && commanded.apply({Type::Seek, 0, 1}) &&
                          !commanded.isPlaying(0) && commanded.frameClock() == 3,
                      "seek command resumed playback or reset the output clock");
    block.fill(42.F);
    passed &= require(commanded.render(block) && std::ranges::all_of(block, [](float x) { return x == 0.F; }) &&
                          commanded.sourceFrame() == 1 && commanded.sourceFrame(1) == 2 && commanded.frameClock() == 6,
                      "paused owner command state did not produce clocked silence");

    const auto pcmA = source(1024, 3), pcmB = source(997, 11);
    RenderCore actual(pcmA, pcmB), untouched(pcmA, pcmB);
    const auto reject = [&](const auto& operation) {
        const auto before = state(actual);
        bool ok = require(!operation() && state(actual) == before, "rejected command changed public transport state");
        std::array<float, 6> changed{}, expected{};
        ok &= require(actual.render(changed) && untouched.render(expected) && changed == expected &&
                          state(actual) == state(untouched),
                      "rejected command changed subsequent stereo output");
        return ok;
    };
    for (const std::size_t deck : {RenderCore::deckCount, std::numeric_limits<std::size_t>::max()}) {
        passed &= reject([&] { return actual.seek(deck, 0); });
        passed &= reject([&] { return actual.setPlaying(deck, false); });
        passed &= reject([&] { return actual.setDeckGain(deck, .5F); });
        passed &= require(actual.sourceFrame(deck) == 0 && actual.sourceFrames(deck) == 0 &&
                              !actual.isPlaying(deck) && actual.deckGain(deck) == 0.F,
                          "invalid deck getter did not return an empty state");
    }
    passed &= reject([&] { return actual.seek(0, 1025); });
    passed &= reject([&] { return actual.seek(1, std::numeric_limits<std::uint64_t>::max()); });
    for (const float bad : {-.001F, 1.001F, std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        passed &= reject([&] { return actual.setDeckGain(0, bad); });
        passed &= reject([&] { return actual.setMasterGain(bad); });
        passed &= reject([&] { return actual.apply({Type::DeckGain, 1, 0, bad}); });
        passed &= reject([&] { return actual.apply({Type::MasterGain, 0, 0, bad}); });
    }
    for (const Command bad : {Command{Type::Play, 2}, Command{Type::Pause, 2},
                              Command{Type::Seek, 2, 0}, Command{Type::DeckGain, 2, 0, .5F},
                              Command{Type::Seek, 0, 1025}, Command{static_cast<Type>(255), 0}})
        passed &= reject([&] { return actual.apply(bad); });
    const auto before = state(actual);
    std::array<float, 3> malformed{42.F, 42.F, 42.F};
    passed &= require(!actual.render(malformed) && state(actual) == before &&
                          malformed == std::array<float, 3>{42.F, 42.F, 42.F},
                      "invalid output mutated transport state or destination");
    passed &= require(actual.render({}) && state(actual) == before, "empty output advanced transport");
    return passed;
}

bool legacyAndPartitionParity()
{
    bool passed = true;
    const auto pcmA = source(4099, 5), pcmB = source(1583, 17);
    RenderCore legacy(pcmA), onlyA(pcmA, std::span<const float>{});
    std::array<float, 512> left{}, right{};
    passed &= require(legacy.render(left) && onlyA.render(right) && left == right && state(legacy) == state(onlyA),
                      "legacy single-deck construction changed samples or state");
    passed &= require(legacy.seek(17) && onlyA.seek(0, 17) && legacy.render(left) && onlyA.render(right) &&
                          left == right && legacy.sourceFrame() == legacy.sourceFrame(0) &&
                          legacy.sourceFrames() == legacy.sourceFrames(0),
                      "legacy seek/default getters differ from deck A");
    passed &= require(!onlyA.seek(1, 0) && !onlyA.setPlaying(1, true) && !onlyA.setDeckGain(1, .5F),
                      "unloaded deck accepted an owner action");
    const auto rejectsConstruction = [&](std::span<const float> a, std::span<const float> b) {
        try {
            RenderCore rejected(a, b);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return require(false, "invalid dual-deck PCM construction succeeded");
    };
    const std::array<float, 3> partialFrame{.5F, -.5F, .25F};
    const std::array<float, 2> nonfinite{0.F, std::numeric_limits<float>::quiet_NaN()};
    passed &= rejectsConstruction({}, {});
    passed &= rejectsConstruction(sourceA, partialFrame);
    passed &= rejectsConstruction(nonfinite, sourceB);
    passed &= rejectsConstruction(sourceA, nonfinite);
    RenderCore onlyB(std::span<const float>{}, sourceB);
    std::array<float, 6> b{};
    passed &= require(onlyB.sourceFrames() == 0 && onlyB.sourceFrames(1) == 3 && !onlyB.seek(0) &&
                          !onlyB.setPlaying(0, true) && !onlyB.setDeckGain(0, .5F) && onlyB.render(b) && b == sourceB,
                      "single loaded deck B did not remain independent");

    RenderCore whole(pcmA, pcmB), partitioned(pcmA, pcmB);
    const std::array<std::size_t, 6> phaseFrames{300, 1200, 17, 700, 1024, 1003};
    const std::array<std::size_t, 5> partitions{1, 256, 512, 7, 1024};
    const auto applyBoth = [&](Command command) {
        return require(whole.apply(command) && partitioned.apply(command), "partition test command rejected");
    };
    for (std::size_t phase = 0; phase < phaseFrames.size(); ++phase) {
        // Both render owners apply the same actions at these fixed boundaries.
        // Callback partition changes within a phase never change command time.
        if (phase == 1) {
            passed &= applyBoth({Type::Pause, 1});
            passed &= applyBoth({Type::DeckGain, 0, 0, .5F});
        } else if (phase == 2) {
            passed &= applyBoth({Type::Seek, 0, 11});
            passed &= applyBoth({Type::MasterGain, 0, 0, .25F});
            passed &= applyBoth({Type::Play, 1});
        } else if (phase == 3) {
            passed &= applyBoth({Type::Pause, 0});
            passed &= applyBoth({Type::Seek, 1, 700});
            passed &= applyBoth({Type::DeckGain, 1, 0, .75F});
        } else if (phase == 4) {
            passed &= applyBoth({Type::Play, 0});
            passed &= applyBoth({Type::Seek, 0, 4098});
            passed &= applyBoth({Type::MasterGain, 0, 0, 1.F});
        } else if (phase == 5) {
            passed &= applyBoth({Type::Pause, 1});
            passed &= applyBoth({Type::DeckGain, 0, 0, 0.F});
        }
        std::vector<float> expected(phaseFrames[phase] * 2), actual(expected.size());
        passed &= require(whole.render(expected), "whole phase render failed");
        for (std::size_t offset = 0, part = 0; offset < actual.size(); ++part) {
            const auto count = std::min(partitions[part % partitions.size()] * 2, actual.size() - offset);
            passed &= require(partitioned.render(std::span(actual).subspan(offset, count)), "partition phase render failed");
            offset += count;
        }
        passed &= require(expected == actual && state(whole) == state(partitioned),
                          "transport output/state depends on callback partitioning");
    }
    passed &= require(whole.frameClock() == 4244 && whole.sourceFrame() == 4099,
                      "transport or EOF reset the continuous output clock");
    return passed;
}

Command message(std::uint32_t id)
{
    return {static_cast<Type>(id % 5), id % 2, static_cast<std::uint64_t>(id) * 997,
            static_cast<float>(id % 5) * .25F, id};
}

bool sameMessage(const Command& a, const Command& b)
{
    return a.type == b.type && a.deck == b.deck && a.frame == b.frame && a.gain == b.gain && a.id == b.id;
}

bool queueCapacityAndWrap()
{
    bool passed = require(TransportQueue::capacity == 32, "transport queue capacity differs from 32 usable commands");
    TransportQueue queue;
    Command output{Type::MasterGain, 123, std::numeric_limits<std::uint64_t>::max(), .75F, 987654};
    const auto sentinel = output;
    passed &= require(!queue.tryPop(output) && sameMessage(output, sentinel), "empty pop changed its output");
    for (std::uint32_t id = 1000; id < 1032; ++id)
        passed &= require(queue.tryPush(message(id)), "queue rejected one of 32 usable slots");
    passed &= require(!queue.tryPush(message(999999)), "full queue accepted or overwrote a command");
    for (std::uint32_t id = 1000; id < 1017; ++id)
        passed &= require(queue.tryPop(output) && sameMessage(output, message(id)), "initial FIFO payload differs");
    for (std::uint32_t id = 1032; id < 1049; ++id)
        passed &= require(queue.tryPush(message(id)), "wrap-around slot was not reusable");
    passed &= require(!queue.tryPush(message(999999)), "wrapped full queue accepted a command");
    for (std::uint32_t id = 1017; id < 1049; ++id)
        passed &= require(queue.tryPop(output) && sameMessage(output, message(id)), "wrapped FIFO payload lost order/data");
    const auto last = output;
    passed &= require(!queue.tryPop(output) && sameMessage(output, last), "drained empty queue changed its output");
    return passed;
}

bool queueConcurrentFIFO()
{
    TransportQueue queue;
    constexpr std::uint32_t count = 16384;
    std::atomic<bool> failed{false};
    std::uint32_t produced = 0, consumed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto timedOut = [&] { return std::chrono::steady_clock::now() >= deadline; };
    // jthread destruction joins even if creating the second thread throws.
    std::jthread producer([&] {
        while (produced < count && !failed.load()) {
            if (queue.tryPush(message(produced)))
                ++produced;
            else
                std::this_thread::yield();
            if (timedOut())
                failed.store(true);
        }
    });
    std::jthread consumer([&] {
        Command value;
        while (consumed < count && !failed.load()) {
            if (queue.tryPop(value)) {
                if (!sameMessage(value, message(consumed))) {
                    failed.store(true);
                    break;
                }
                ++consumed;
            } else {
                std::this_thread::yield();
            }
            if (timedOut())
                failed.store(true);
        }
    });
    producer.join();
    consumer.join();
    Command remaining;
    return require(!failed.load() && produced == count && consumed == count && !queue.tryPop(remaining),
                   "concurrent SPSC lost/reordered payloads or exceeded finite timeout");
}
} // namespace

int main()
{
    try {
        bool passed = mixingAndIndependentTransport();
        passed &= numericHeadroomAndFiniteOverflow();
        passed &= commandsAndRejectedState();
        passed &= legacyAndPartitionParity();
        passed &= queueCapacityAndWrap();
        passed &= queueConcurrentFIFO();
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

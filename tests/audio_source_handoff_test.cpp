// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/SourceHandoff.hpp"
#include "audio/TransportQueue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

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
            result.retired.readFailed && !handoff.busy(), "Retired source identity/diagnostics changed");
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
} // namespace
int main()
{
    try { orderingAndQuiesce(); concurrentOwnerLifetime(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// Generated finite PCM is compared with actual complete-file preparation.
#include "audio/StemRateConverter.hpp"
#include "audio/AudioFile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
using Converter = conflux::audio::StemRateConverter;
using Pcm = std::vector<float>;
using Bank = std::array<Pcm, 4>;
using Inputs = std::array<std::span<const float>, 4>;
using Outputs = std::array<std::span<float>, 4>;
constexpr float untouched = 12345.F;
constexpr std::size_t hop = 257985, block = 4096;
using Clock = std::chrono::steady_clock;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Pcm fixture(std::size_t frames, unsigned role)
{
    Pcm pcm(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        pcm[frame * 2] = float(int((frame * (11 + role * 2) + role * 73) % 1543) - 771) / 512.F;
        pcm[frame * 2 + 1] = float(.37 * std::sin((frame + 7 * role) * (.011 + role * .003)));
        if (frame == 0 || frame == 4095 || frame == 4096 || frame == hop - 1 || frame == hop || frame == frames - 1)
            pcm[frame * 2] += (role & 1) ? -.75F : .75F;
    }
    return pcm;
}
struct Descriptor { int fd = -1; ~Descriptor() { if (fd >= 0) ::close(fd); } };
Pcm preparedReference(std::span<const float> input)
{
    auto name = (std::filesystem::temp_directory_path() / "conflux-converter-test-XXXXXX").string();
    Descriptor raw{::mkstemp(name.data())};
    require(raw.fd >= 0, "Cannot create generated raw input");
    require(::unlink(name.c_str()) == 0, "Cannot unlink generated raw input");
    std::array<unsigned char, block * 8> bytes{};
    for (std::size_t first = 0; first < input.size();) {
        const auto count = std::min(bytes.size() / 4, input.size() - first);
        for (std::size_t index = 0; index < count; ++index) {
            const auto bits = std::bit_cast<std::uint32_t>(input[first + index]);
            for (unsigned byte = 0; byte < 4; ++byte) bytes[index * 4 + byte] = static_cast<unsigned char>(bits >> (8 * byte));
        }
        for (std::size_t written = 0; written < count * 4;) {
            const auto got = ::write(raw.fd, bytes.data() + written, count * 4 - written);
            require(got > 0, "Generated raw input write failed"); written += std::size_t(got);
        }
        first += count;
    }
    const auto frames = input.size() / 2;
    auto reference = conflux::audio::prepareStemOutput(raw.fd, frames, (frames * 2 + 8192) * 8);
    Pcm pcm(reference.frames() * 2);
    for (std::size_t read = 0; read < pcm.size() * sizeof(float);) {
        const auto got = ::pread(reference.descriptor(), reinterpret_cast<char*>(pcm.data()) + read,
                                 pcm.size() * sizeof(float) - read, off_t(read));
        require(got > 0, "Prepared reference ended early"); read += std::size_t(got);
    }
    return pcm;
}

template<class Exception, class Function>
void throws(Function&& function, const char* message)
{
    try { function(); } catch (const Exception&) { return; }
    throw std::runtime_error(message);
}
Bank bankFixture(std::size_t n)
{
    Bank pcm;
    for (unsigned role = 0; role < 4; ++role) pcm[role] = fixture(n, role);
    return pcm;
}
Bank references(const Bank& pcm)
{
    Bank result;
    for (unsigned role = 0; role < 4; ++role) result[role] = preparedReference(pcm[role]);
    return result;
}
bool sameBits(std::span<const float> a, std::span<const float> b)
{
    return a.size() == b.size() && std::ranges::equal(a, b, [](float left, float right) {
        return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
    });
}
struct DriveResult { Bank pcm; std::size_t inputUsed = 0, zeroGapFrames = 0; bool finished = false; };
DriveResult drive(Converter& converter, const Bank& source, std::size_t target48,
                  std::span<const std::size_t> inPlan, std::span<const std::size_t> outPlan,
                  bool gapAfterFirstHop = false, bool rejectEofReversal = false, bool aliasInputRoles = false)
{
    const auto n = source.front().size() / 2;
    std::size_t input = std::size_t(converter.inputStartFrame()), next = target48, calls = 0;
    std::array<std::array<float, Converter::blockFrames * 2>, 4> buffers{};
    DriveResult result;
    auto call = [&](std::size_t available, std::size_t capacity, bool eof) {
        require(++calls < n * 3 + 65536, "Converter test made no bounded progress");
        Inputs in{}; Outputs out{};
        for (unsigned role = 0; role < 4; ++role) {
            in[role] = available ? std::span<const float>(source[aliasInputRoles ? 0 : role]).subspan(input * 2, available * 2)
                                 : std::span<const float>{};
            buffers[role].fill(untouched); out[role] = std::span(buffers[role]).first(capacity * 2);
        }
        const auto step = converter.process(in, out, eof);
        require(step.inputFramesUsed <= available && step.outputFrames <= capacity && step.firstOutputFrame == next,
                "Converter supplied impossible or mislabelled progress");
        for (unsigned role = 0; role < 4; ++role) {
            require(std::ranges::all_of(out[role].first(step.outputFrames * 2), [](float v) { return std::isfinite(v); }),
                    "Converter emitted nonfinite PCM");
            require(std::ranges::all_of(std::span(buffers[role]).subspan(step.outputFrames * 2),
                                       [](float v) { return v == untouched; }), "Converter overwrote unavailable output");
            result.pcm[role].insert(result.pcm[role].end(), buffers[role].begin(), buffers[role].begin() + step.outputFrames * 2);
        }
        input += step.inputFramesUsed; next += step.outputFrames; result.inputUsed += step.inputFramesUsed;
        result.finished = step.finished; return step;
    };
    std::size_t part = 0;
    while (input < n) {
        auto end = input + std::min(inPlan[part % inPlan.size()], n - input);
        if (gapAfterFirstHop && input < hop) end = std::min(end, hop);
        do {
            const auto step = call(end - input, outPlan[part % outPlan.size()], false);
            require(!step.finished && (step.inputFramesUsed || step.outputFrames), "Positive input stopped or falsely finished");
        } while (input < end);
        ++part;
        if (gapAfterFirstHop && input == hop) {
            for (;;) { const auto step = call(0, 4096, false);
                require(!step.finished, "Temporary gap flushed EOF"); if (!step.outputFrames) break; }
            const auto before = next;
            for (unsigned retry = 0; retry < 3; ++retry) {
                const auto step = call(0, 4096, false);
                require(!step.finished && !step.inputFramesUsed && !step.outputFrames,
                        "Repeated temporary gap changed converter coverage");
            }
            require(next == before && next < 280800, "First hop invented missing filter lookahead");
            result.zeroGapFrames = next;
        }
    }
    if (rejectEofReversal) {
        const auto firstTail = call(0, 1, true);
        require(!firstTail.finished && firstTail.outputFrames == 1, "EOF reversal fixture did not leave a pending tail");
        Outputs out{};
        for (unsigned role = 0; role < 4; ++role) {
            buffers[role].fill(untouched); out[role] = std::span(buffers[role]).first(2);
        }
        throws<std::invalid_argument>([&] { (void)converter.process({}, out, false); },
                                      "Started EOF drain accepted a temporary-gap flag");
        for (const auto& buffer : buffers)
            require(std::ranges::all_of(buffer, [](float value) { return value == untouched; }),
                    "Rejected EOF reversal modified output");
    }
    while (!result.finished) call(0, outPlan[part++ % outPlan.size()], true);
    const auto ended = call(0, 4096, true);
    require(ended.finished && !ended.inputFramesUsed && !ended.outputFrames, "Repeated EOF is not idempotent");
    return result;
}

void exactCountBudgetAndCancellation()
{
    for (const auto n : {1U, 2U, 146U, 147U, 148U, 4095U, 4096U, 4097U}) {
        const auto reference = preparedReference(fixture(n, 0));
        const auto actual = reference.size() / 2;
        const auto plan = Converter::countFrames(n, actual);
        require(plan.inputFrames() == n && plan.outputFrames() == actual, "Plan guessed a rounded EOF length");
        throws<std::length_error>([&] { (void)Converter::countFrames(n, actual - 1); }, "Actual count exceeded its output budget");
    }
    throws<std::invalid_argument>([] { (void)Converter::countFrames(0, 1024); }, "Empty source plan accepted");
    throws<std::invalid_argument>([] { (void)Converter::countFrames(32, 0); }, "Empty output budget accepted");
    throws<std::invalid_argument>([] { (void)Converter::countFrames(std::numeric_limits<std::uint64_t>::max(), 1024); },
                                 "Unaddressable count input accepted");
    throws<std::invalid_argument>([] { (void)Converter::countFrames(32, std::numeric_limits<std::uint64_t>::max()); },
                                 "Unaddressable output budget accepted");
    std::stop_source cancelled; cancelled.request_stop();
    throws<std::runtime_error>([&] { (void)Converter::countFrames(hop, hop * 2, cancelled.get_token()); },
                              "Cancelled count pass returned a usable plan");
}

void countCancellationAfterStart()
{
    std::stop_source cancellation;
    std::atomic<bool> entered{false}, done{false}, cancelled{false};
    std::exception_ptr failure;
    std::jthread counter([&] {
        entered.store(true, std::memory_order_release);
        try {
            (void)Converter::countFrames(8 * 1024 * 1024, 16 * 1024 * 1024, cancellation.get_token());
        } catch (const std::runtime_error& error) {
            cancelled.store(std::string(error.what()).find("cancel") != std::string::npos, std::memory_order_relaxed);
        } catch (...) { failure = std::current_exception(); }
        done.store(true, std::memory_order_release);
    });
    const auto startDeadline = Clock::now() + std::chrono::seconds(5);
    while (!entered.load(std::memory_order_acquire) && Clock::now() < startDeadline) std::this_thread::yield();
    const bool started = entered.load(std::memory_order_acquire);
    // A long synthetic count remains bounded in memory. Let the real library
    // task begin; then stop it without waiting for its millions of input frames.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool completedBeforeStop = done.load(std::memory_order_acquire);
    cancellation.request_stop();
    const auto stopDeadline = Clock::now() + std::chrono::seconds(5);
    while (!done.load(std::memory_order_acquire) && Clock::now() < stopDeadline) std::this_thread::yield();
    const bool stopped = done.load(std::memory_order_acquire);
    counter.join();
    if (failure) std::rethrow_exception(failure);
    require(started && !completedBeforeStop && stopped && cancelled.load(),
            "An active synthetic frame-count task ignored cancellation or returned a valid plan");
}

void continuousPartitionsGapHeadroomAndTail()
{
    constexpr std::size_t n = hop + 211;
    const auto source = bankFixture(n), reference = references(source);
    const auto plan = Converter::countFrames(n, n * 2);
    constexpr std::array<std::size_t, 1> fixed{4096};
    constexpr std::array<std::size_t, 4> in{1, 146, 4095, 997}, out{1, 257, 4096, 511};
    for (const auto mixed : {false, true}) {
        Converter converter(plan);
        const auto actual = drive(converter, source, 0,
            mixed ? std::span<const std::size_t>(in) : std::span<const std::size_t>(fixed),
            mixed ? std::span<const std::size_t>(out) : std::span<const std::size_t>(fixed), true);
        require(actual.inputUsed == n && actual.finished && actual.zeroGapFrames < 280800,
                "Continuous conversion lost input, lookahead or real EOF");
        for (unsigned role = 0; role < 4; ++role)
            require(sameBits(actual.pcm[role], reference[role]) && actual.pcm[role].size() / 2 == plan.outputFrames(),
                    "Partitions, temporary gap, channel/role mapping, headroom or EOF tail differs from prepared reference");
    }
}

void planReusedAcrossSeekAnchors()
{
    constexpr std::size_t n = hop * 4 + 211;
    const auto source = bankFixture(n), reference = references(source);
    const auto plan = Converter::countFrames(n, n * 2);
    constexpr std::array<std::size_t, 3> in{147, 4096, 811}, out{4096, 257, 13};
    // The same counted value is reused. Interior anchors have a substantial
    // remaining tail; they do not merely exercise a reset close to source EOF.
    const std::array<std::uint64_t, 5> targets{280800, 561601, 842523, plan.outputFrames() - 1, plan.outputFrames()};
    for (const auto target : targets) {
        Converter converter(plan, target);
        const auto cell = target / 160;
        const auto anchor = (cell > 32 ? cell - 32 : 0) * 147;
        require(converter.inputStartFrame() == anchor, "Seek lost rational anchor or conservative preroll");
        const auto actual = drive(converter, source, std::size_t(target), in, out);
        require(actual.inputUsed == n - anchor && actual.finished, "Seek did not cover exactly the remaining source");
        for (unsigned role = 0; role < 4; ++role) {
            const auto expected = std::span(reference[role]).subspan(std::size_t(target) * 2);
            require(sameBits(actual.pcm[role], expected), "Seek first window or EOF tail differs from complete conversion");
        }
    }
    throws<std::invalid_argument>([&] { Converter converter(plan, plan.outputFrames() + 1); }, "Seek beyond counted EOF accepted");
}

void readonlyAliasesAndStickyEof()
{
    const auto source = bankFixture(8193);
    const auto reference = preparedReference(source[0]);
    const auto plan = Converter::countFrames(8193, 16386);
    constexpr std::array<std::size_t, 1> fixed{4096};
    Converter converter(plan);
    const auto actual = drive(converter, source, 0, fixed, fixed, false, true, true);
    require(actual.finished && actual.inputUsed == 8193, "Alias/EOF drain lost exact input coverage");
    for (const auto& role : actual.pcm)
        require(sameBits(role, reference), "Read-only aliases or rejected EOF reversal changed the real tail");
    for (const auto target : {159U, 160U, 161U}) {
        Converter seek(plan, target);
        const auto selected = drive(seek, source, target, fixed, fixed, false, false, true);
        for (const auto& role : selected.pcm)
            require(sameBits(role, std::span(reference).subspan(target * 2)), "Initial partial-grid seek used a wrong phase");
    }
}

void finiteInputOverflowIsTerminalWithoutPartialPublication()
{
    Bank source;
    for (auto& role : source) role.assign(8193 * 2, .125F);
    for (std::size_t frame = 2048; frame < 8193; ++frame)
        source[3][frame * 2] = std::numeric_limits<float>::max();
    bool referenceOverflow = false;
    try { (void)preparedReference(source[3]); }
    catch (const std::runtime_error& error) {
        referenceOverflow = std::string(error.what()).find("non-finite") != std::string::npos;
    }
    require(referenceOverflow, "Real-lib finite-step overflow positive control did not overflow; fixture needs review");
    const auto plan = Converter::countFrames(8193, 16386);
    Converter converter(plan);
    Inputs in{}; std::array<std::array<float, 8192>, 4> storage{}; Outputs out{};
    for (unsigned role = 0; role < 4; ++role) {
        in[role] = std::span(source[role]).first(8192);
        storage[role].fill(untouched); out[role] = storage[role];
    }
    throws<std::runtime_error>([&] { (void)converter.process(in, out, false); }, "Nonfinite converted output was published");
    for (const auto& role : storage)
        require(std::ranges::all_of(role, [](float value) { return value == untouched; }),
                "A later overflowing role allowed a healthy role prefix to escape");
    throws<std::runtime_error>([&] { (void)converter.process(in, out, false); }, "Failed generation was reused after output overflow");
}

void invalidCallPreservesStreamAndOutput()
{
    const auto source = bankFixture(8193), reference = references(source);
    const auto plan = Converter::countFrames(8193, 16386);
    Converter converter(plan);
    std::array<std::array<float, 8194>, 4> storage{};
    Inputs in{}; Outputs out{};
    for (unsigned role = 0; role < 4; ++role) {
        in[role] = std::span(source[role]).first(16); storage[role].fill(untouched);
        out[role] = std::span(storage[role]).first(16);
    }
    auto invalid = [&](Inputs input, Outputs output, bool eof = false) {
        throws<std::invalid_argument>([&] { (void)converter.process(input, output, eof); }, "Invalid stream call accepted");
        for (const auto& buffer : storage)
            require(std::ranges::all_of(buffer, [](float v) { return v == untouched; }), "Rejected call wrote output");
    };
    auto badIn = in; badIn[2] = badIn[2].first(15); invalid(badIn, out);
    badIn = in; for (unsigned role = 0; role < 4; ++role) badIn[role] = std::span(source[role]).first(8194);
    invalid(badIn, out);
    auto badOut = out; badOut[1] = badOut[1].first(14); invalid(in, badOut);
    badOut = out; for (unsigned role = 0; role < 4; ++role) badOut[role] = storage[role];
    invalid(in, badOut);
    badOut = out; for (auto& span : badOut) span = {}; invalid(in, badOut);
    badOut = out; badOut[1] = badOut[0]; invalid(in, badOut);
    auto aliased = source;
    const auto beforeAlias = aliased;
    badIn = in; badIn[3] = std::span(aliased[3]).first(16);
    badOut = out; badOut[0] = std::span(aliased[3]).first(16);
    invalid(badIn, badOut);
    require(aliased == beforeAlias, "Cross-role input/output overlap modified borrowed input");
    invalid(in, out, true); // No positive-input EOF or premature tail invention.
    Inputs empty{}; invalid(empty, out, true); // Exact input coverage is required.
    auto corrupt = source;
    for (const auto nonfinite : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        corrupt[3][15] = nonfinite; badIn = in; badIn[3] = std::span(corrupt[3]).first(16); invalid(badIn, out);
    }
    constexpr std::array<std::size_t, 1> fixed{4096};
    const auto actual = drive(converter, source, 0, fixed, fixed);
    for (unsigned role = 0; role < 4; ++role)
        require(sameBits(actual.pcm[role], reference[role]), "Rejected input changed filter history, cursor or tail");
    invalid(in, out); // Finished state cannot accept another source silently.
}
} // namespace

int main()
{
    try {
        exactCountBudgetAndCancellation();
        countCancellationAfterStart();
        continuousPartitionsGapHeadroomAndTail();
        planReusedAcrossSeekAnchors();
        readonlyAliasesAndStickyEof();
        finiteInputOverflowIsTerminalWithoutPartialPublication();
        invalidCallPreservesStreamAndOutput();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Pure four-stem conversion count/gap/seek/EOF/error behavior passed\n";
}

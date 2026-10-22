// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/ChannelEq.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using conflux::audio::ChannelEq;
constexpr std::size_t low = 0, mid = 1, high = 2;
constexpr double sampleRate = 48000.;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool sameBits(float first, float second)
{
    return std::bit_cast<std::uint32_t>(first) == std::bit_cast<std::uint32_t>(second);
}

void sameFrame(const std::array<float, 2>& first, const std::array<float, 2>& second,
               const char* message)
{
    require(sameBits(first[0], second[0]) && sameBits(first[1], second[1]), message);
}

float tone(std::size_t frame, double frequency)
{
    return float(.0625 * std::sin(2. * std::numbers::pi * frequency * double(frame) / sampleRate));
}

std::array<float, 2> signal(std::size_t frame)
{
    return {tone(frame, 40.) + tone(frame, 1000.) + tone(frame, 12000.),
            tone(frame, 83.) - tone(frame, 1700.) + tone(frame, 9700.)};
}

void neutralPreservesBitsAndHeadroom()
{
    static_assert(ChannelEq::bandCount == 3);
    static_assert(ChannelEq::minGainDb == -26.F && ChannelEq::maxGainDb == 6.F);
    ChannelEq eq;
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
        require(eq.gainDb(band) == 0.F, "Channel EQ does not start neutral");
    constexpr std::array<std::array<float, 2>, 9> input{{
        {0.F, -0.F}, {-0.F, 0.F}, {.125F, -.75F}, {2.5F, -3.25F},
        {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()},
        {std::numeric_limits<float>::min(), -std::numeric_limits<float>::min()},
        {std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min()},
        {-1.F, 1.F}, {-0.F, -0.F},
    }};
    for (const auto& frame : input)
        sameFrame(eq.process(frame[0], frame[1]), frame,
                  "Neutral EQ changed sample bits, signed zero, stereo or float headroom");
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
        require(eq.setGain(band, 0.F, 240), "An unchanged neutral target was rejected");
    for (const auto& frame : input)
        sameFrame(eq.process(frame[0], frame[1]), frame,
                  "Repeated neutral controls engaged a nonidentity filter");
}

double responseDb(std::size_t band, float gain, double frequency)
{
    ChannelEq eq;
    require(eq.setGain(band, gain, 0), "EQ response setup was rejected");
    double inputEnergy = 0., outputEnergy = 0.;
    // Discard one second of settling, then measure complete cycles over one
    // second. This measures the actual filter, without recreating coefficients.
    for (std::size_t frame = 0; frame < 96000; ++frame) {
        const float input = tone(frame, frequency);
        const auto output = eq.process(input, 0.F);
        require(std::isfinite(output[0]) && output[1] == 0.F,
                "Band response is nonfinite or leaked into the silent channel");
        if (frame >= 48000) {
            inputEnergy += double(input) * input;
            outputEnergy += double(output[0]) * output[0];
        }
    }
    require(outputEnergy > 0. && inputEnergy > 0., "EQ response lost the test signal");
    return 10. * std::log10(outputEnergy / inputEnergy);
}

void bandsCutAndBoostTheirOwnRegion()
{
    constexpr std::array<double, 3> ownFrequency{40., 1000., 12000.};
    constexpr std::array<double, 3> distantFrequency{12000., 40., 40.};
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band) {
        const double cut = responseDb(band, ChannelEq::minGainDb, ownFrequency[band]);
        const double boost = responseDb(band, ChannelEq::maxGainDb, ownFrequency[band]);
        require(cut > -27. && cut < -20., "EQ cut did not attenuate its own frequency region");
        require(boost > 5. && boost < 7., "EQ boost did not amplify its own frequency region");
        require(std::abs(responseDb(band, ChannelEq::minGainDb, distantFrequency[band])) < 1.,
                "EQ cut acted as broadband gain or used the wrong frequency region");
        require(std::abs(responseDb(band, ChannelEq::maxGainDb, distantFrequency[band])) < 1.,
                "EQ boost acted as broadband gain or used the wrong frequency region");
    }
}

void stereoHistoriesStayIndependent()
{
    ChannelEq stereo, leftOnly, rightOnly;
    for (auto* eq : {&stereo, &leftOnly, &rightOnly}) {
        require(eq->setGain(low, -18.F, 0) && eq->setGain(mid, 6.F, 0) &&
                    eq->setGain(high, -9.F, 0), "Stereo EQ setup was rejected");
    }
    for (std::size_t frame = 0; frame < 8192; ++frame) {
        auto input = signal(frame);
        if (frame == 0) input[0] += .5F;
        if (frame == 127) input[1] -= .25F;
        const auto both = stereo.process(input[0], input[1]);
        const auto left = leftOnly.process(input[0], 0.F);
        const auto right = rightOnly.process(0.F, input[1]);
        require(sameBits(both[0], left[0]) && sameBits(both[1], right[1]) &&
                    left[1] == 0.F && right[0] == 0.F,
                "EQ reused the other channel's filter history");
    }
}

void invalidControlsPreserveTargetsAndRunningState()
{
    ChannelEq actual, reference;
    constexpr std::array<float, 3> target{-18.F, 4.F, -7.F};
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
        require(actual.setGain(band, target[band], 480) &&
                    reference.setGain(band, target[band], 480), "Validation fixture setup failed");
    for (std::size_t frame = 0; frame < 93; ++frame) {
        const auto input = signal(frame);
        sameFrame(actual.process(input[0], input[1]), reference.process(input[0], input[1]),
                  "Validation fixtures diverged before invalid controls");
    }
    for (const auto band : std::array<std::size_t, 2>{ChannelEq::bandCount,
                                                    std::numeric_limits<std::size_t>::max()})
        require(!actual.setGain(band, 1.F, 240), "Out-of-range EQ band was accepted");
    constexpr float infinity = std::numeric_limits<float>::infinity();
    const std::array invalid{std::numeric_limits<float>::quiet_NaN(), infinity, -infinity,
                            std::nextafter(ChannelEq::minGainDb, -infinity),
                            std::nextafter(ChannelEq::maxGainDb, infinity)};
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band) {
        for (const float gain : invalid)
            require(!actual.setGain(band, gain, 0), "Nonfinite/out-of-range EQ gain was accepted");
        require(actual.gainDb(band) == target[band], "Rejected EQ command changed its target");
    }
    for (std::size_t frame = 93; frame < 1024; ++frame) {
        const auto input = signal(frame);
        sameFrame(actual.process(input[0], input[1]), reference.process(input[0], input[1]),
                  "Rejected EQ command changed filter history or interrupted a running ramp");
    }
}

void rampsDeferAndDuplicateTargetsDoNotExtendThem()
{
    ChannelEq gradual, immediate;
    require(gradual.setGain(high, 6.F, 240) && immediate.setGain(high, 6.F, 0),
            "EQ ramp setup failed");
    const auto first = gradual.process(.125F, -.0625F);
    const auto jumped = immediate.process(.125F, -.0625F);
    require(std::abs(first[0] - .125F) < std::abs(jumped[0] - .125F) * .1F,
            "Nonzero EQ ramp applied the full gain change on its first sample");

    struct Change { std::size_t frame, band; float gain; std::uint32_t ramp; };
    constexpr std::array changes{
        Change{0, high, 6.F, 240}, Change{61, mid, -9.F, 111},
        Change{117, low, -26.F, 360}, Change{181, high, -3.F, 240},
        Change{514, low, 0.F, 240}, Change{701, mid, 6.F, 97},
        Change{900, high, 0.F, 0},
    };
    constexpr std::size_t frames = 2048;
    std::array<std::array<float, 2>, frames> expected{};
    ChannelEq once;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (const auto& change : changes)
            if (change.frame == frame)
                require(once.setGain(change.band, change.gain, change.ramp), "EQ retarget was rejected");
        const auto input = signal(frame);
        expected[frame] = once.process(input[0], input[1]);
    }
    // Repeated GUI targets at different callback partitions must not restart a
    // partially applied ramp. Every actual target change stays sample-aligned.
    for (const std::size_t block : {1U, 7U, 64U, 255U}) {
        ChannelEq repeated;
        std::array<float, 3> targets{};
        std::array<std::uint32_t, 3> ramps{};
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (const auto& change : changes)
                if (change.frame == frame) {
                    targets[change.band] = change.gain;
                    ramps[change.band] = change.ramp;
                    require(repeated.setGain(change.band, change.gain, change.ramp), "Partitioned retarget failed");
                }
            if (frame % block == 0)
                for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
                    require(repeated.setGain(band, targets[band], ramps[band]), "Duplicate target was rejected");
            const auto input = signal(frame);
            const auto output = repeated.process(input[0], input[1]);
            require(std::isfinite(output[0]) && std::isfinite(output[1]), "EQ ramp produced nonfinite PCM");
            sameFrame(output, expected[frame], "Callback partition or duplicate target extended an EQ ramp");
        }
    }
}

void clearStateRemovesOnlyAudioHistory()
{
    ChannelEq used, silentHistory;
    constexpr std::array<float, 3> targets{-12.F, 3.F, -8.F};
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
        require(used.setGain(band, targets[band], 720) &&
                    silentHistory.setGain(band, targets[band], 720), "Clear-state fixture setup failed");
    for (std::size_t frame = 0; frame < 159; ++frame) {
        const auto input = signal(frame);
        used.process(input[0], input[1]);
        silentHistory.process(0.F, 0.F);
    }
    used.clearState();
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
        require(used.gainDb(band) == targets[band], "Source reset discarded an accepted EQ setting");
    // The second instance advanced the same ramps using zeros, so its audio
    // state is already clear without resetting its parameter progression.
    for (std::size_t frame = 159; frame < 2048; ++frame) {
        const auto input = signal(frame);
        sameFrame(used.process(input[0], input[1]), silentHistory.process(input[0], input[1]),
                  "Clear-state retained prior-source audio or reset an in-progress parameter ramp");
    }
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
        require(used.setGain(band, 0.F, 0), "Cannot restore neutral EQ");
    used.clearState();
    sameFrame(used.process(-0.F, 2.5F), {-0.F, 2.5F},
              "Cleared neutral EQ did not restore exact bypass");
}

void finiteExtremesDoNotPoisonTheOtherChannel()
{
    ChannelEq eq;
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
        require(eq.setGain(band, ChannelEq::maxGainDb, 0), "Extreme-input EQ setup failed");
    constexpr float largest = std::numeric_limits<float>::max();
    for (std::size_t frame = 0; frame < 4096; ++frame) {
        const auto output = eq.process(frame % 7 < 3 ? largest : -largest, 0.F);
        require(std::isfinite(output[0]) && output[1] == 0.F,
                "Finite extreme input overflowed EQ output or contaminated its silent peer");
    }
    eq.clearState();
    ChannelEq fresh;
    for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
        require(fresh.setGain(band, ChannelEq::maxGainDb, 0), "Extreme reset fixture setup failed");
    for (std::size_t frame = 0; frame < 1024; ++frame) {
        const auto input = signal(frame);
        sameFrame(eq.process(input[0], input[1]), fresh.process(input[0], input[1]),
                  "Cleared EQ retained nonfinite/extreme history from a prior source");
    }
}

void nonfiniteInputClearsOnlyItsOwnHistory()
{
    ChannelEq actual, leftReference, rightReference;
    constexpr std::array<float, 3> targets{-8.F, 6.F, -14.F};
    for (auto* eq : {&actual, &leftReference, &rightReference})
        for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
            require(eq->setGain(band, targets[band], 720), "Nonfinite recovery setup failed");
    for (std::size_t frame = 0; frame < 2048; ++frame) {
        const auto input = signal(frame);
        auto contaminated = input;
        auto cleaned = input;
        if (frame == 83 || frame == 151) {
            contaminated[0] = frame == 83 ? std::numeric_limits<float>::quiet_NaN()
                                         : std::numeric_limits<float>::infinity();
            cleaned[0] = 0.F;
            leftReference.clearState();
        }
        if (frame == 201 || frame == 412) {
            contaminated[1] = frame == 201 ? -std::numeric_limits<float>::infinity()
                                          : std::numeric_limits<float>::quiet_NaN();
            cleaned[1] = 0.F;
            rightReference.clearState();
        }
        const auto output = actual.process(contaminated[0], contaminated[1]);
        const auto left = leftReference.process(cleaned[0], 0.F);
        const auto right = rightReference.process(0.F, cleaned[1]);
        require(std::isfinite(output[0]) && std::isfinite(output[1]),
                "Nonfinite EQ input poisoned subsequent output");
        sameFrame(output, {left[0], right[1]},
                  "Nonfinite input did not clear just its channel or interrupted accepted gain ramps");
        for (std::size_t band = 0; band < ChannelEq::bandCount; ++band)
            require(actual.gainDb(band) == targets[band], "Nonfinite input discarded an accepted EQ setting");
    }
}
} // namespace

int main()
{
    try {
        neutralPreservesBitsAndHeadroom();
        bandsCutAndBoostTheirOwnRegion();
        stereoHistoriesStayIndependent();
        invalidControlsPreserveTargetsAndRunningState();
        rampsDeferAndDuplicateTargetsDoNotExtendThem();
        clearStateRemovesOnlyAudioHistory();
        finiteExtremesDoNotPoisonTheOtherChannel();
        nonfiniteInputClearsOnlyItsOwnHistory();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Channel EQ neutral, response, stereo, ramp, validation and source-reset checks passed\n";
    return 0;
}

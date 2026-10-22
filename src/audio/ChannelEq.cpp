// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/ChannelEq.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace conflux::audio {
namespace {
double historyValue(double value) noexcept
{
    // Flush only double subnormals. Ordinary float subnormals remain representable
    // in these double histories, and the neutral output preserves their bits.
    return std::abs(value) < std::numeric_limits<double>::min() ? std::copysign(0.0, value) : value;
}
} // namespace

ChannelEq::Coefficients ChannelEq::coefficients(std::size_t band, float db) noexcept
{
    if (db == 0.F) return {1, 0, 0, 0, 0};
    // Normalized RBJ cookbook filters, not a hardware transfer-function model:
    // https://www.w3.org/TR/audio-eq-cookbook/
    // Shelves: S = 1 at 250/4000 Hz. Peak: Q = 1/sqrt(2) at 1000 Hz.
    constexpr std::array<double, bandCount> frequency{250, 1000, 4000};
    const double omega = 2 * std::numbers::pi * frequency[band] / 48000;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) * std::numbers::sqrt2 / 2;
    const double a = std::pow(10.0, double(db) / 40);
    double b0, b1, b2, a0, a1, a2;
    if (band == 1) {
        b0 = 1 + alpha * a; b1 = -2 * cosine; b2 = 1 - alpha * a;
        a0 = 1 + alpha / a; a1 = -2 * cosine; a2 = 1 - alpha / a;
    } else {
        const double plus = a + 1, minus = a - 1, beta = 2 * std::sqrt(a) * alpha;
        if (band == 0) {
            b0 = a * (plus - minus * cosine + beta);
            b1 = 2 * a * (minus - plus * cosine);
            b2 = a * (plus - minus * cosine - beta);
            a0 = plus + minus * cosine + beta;
            a1 = -2 * (minus + plus * cosine);
            a2 = plus + minus * cosine - beta;
        } else {
            b0 = a * (plus + minus * cosine + beta);
            b1 = -2 * a * (minus + plus * cosine);
            b2 = a * (plus + minus * cosine - beta);
            a0 = plus - minus * cosine + beta;
            a1 = 2 * (minus - plus * cosine);
            a2 = plus - minus * cosine - beta;
        }
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

bool ChannelEq::setGain(std::size_t index, float db, std::uint32_t rampFrames) noexcept
{
    if (index >= bandCount || !std::isfinite(db) || db < minGainDb || db > maxGainDb) return false;
    auto& band = bands_[index];
    if (db == band.targetDb && (!band.remaining || rampFrames)) return true;
    const auto target = coefficients(index, db);
    band.targetDb = db;
    band.target = target;
    band.remaining = rampFrames;
    if (!rampFrames) {
        band.current = target;
        band.step.fill(0);
    } else {
        for (std::size_t coefficient = 0; coefficient < target.size(); ++coefficient)
            band.step[coefficient] = (target[coefficient] - band.current[coefficient]) / rampFrames;
    }
    return true;
}

float ChannelEq::gainDb(std::size_t band) const noexcept
{
    return band < bandCount ? bands_[band].targetDb : 0.F;
}

void ChannelEq::clearChannel(std::size_t channel) noexcept
{
    for (auto& band : bands_) band.history[channel] = {};
}

void ChannelEq::clearState() noexcept
{
    for (auto& band : bands_) band.history = {};
}

std::array<float, 2> ChannelEq::process(float left, float right) noexcept
{
    std::array<double, 2> samples{left, right};
    std::array<bool, 2> fault{};
    for (std::size_t channel = 0; channel < samples.size(); ++channel)
        if (!std::isfinite(samples[channel])) {
            clearChannel(channel);
            samples[channel] = 0;
            fault[channel] = true;
        }
    for (auto& band : bands_) {
        if (band.remaining) {
            if (--band.remaining == 0) band.current = band.target;
            else for (std::size_t coefficient = 0; coefficient < band.current.size(); ++coefficient)
                band.current[coefficient] += band.step[coefficient];
        }
        const auto& c = band.current;
        const bool neutral = !band.remaining && band.targetDb == 0.F;
        for (std::size_t channel = 0; channel < samples.size(); ++channel) {
            if (fault[channel]) continue;
            auto& h = band.history[channel];
            const double input = samples[channel];
            const double output = neutral ? input :
                c[0] * input + c[1] * h.x1 + c[2] * h.x2 - c[3] * h.y1 - c[4] * h.y2;
            if (!std::isfinite(output)) {
                // Discard only the affected channel's recurrence, including any
                // earlier stages this sample touched. Parameter ramps keep time.
                clearChannel(channel);
                samples[channel] = 0;
                fault[channel] = true;
                continue;
            }
            h.x2 = h.x1; h.x1 = historyValue(input);
            h.y2 = h.y1; h.y1 = historyValue(output);
            samples[channel] = output;
        }
    }
    constexpr double limit = std::numeric_limits<float>::max();
    return {float(std::clamp(samples[0], -limit, limit)), float(std::clamp(samples[1], -limit, limit))};
}

} // namespace conflux::audio

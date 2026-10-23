// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/ResidentStemHold.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace conflux::audio {
ResidentStemHold::ResidentStemHold(std::uint64_t sourceId, std::uint64_t totalFrames,
                                 AudioResidentHoldRange range)
    : sourceId_(sourceId), range_(range), capturedEnd_(range.inFrame)
{
    const bool provenance = range.provenance == AudioResidentHoldRange::Provenance::DjRange ||
        range.provenance == AudioResidentHoldRange::Provenance::ValidatedRange;
    if (!sourceId || !range.id || !provenance || range.inFrame >= range.outFrame ||
        range.outFrame > totalFrames || range.outFrame-range.inFrame < minimumFrames ||
        range.outFrame-range.inFrame > maximumFrames)
        throw std::invalid_argument("Resident hold needs an explicit bounded same-source range");
    if (range.admittedBytes < bytes()) throw std::length_error("Resident hold byte budget exceeded");
    for (auto& leaf : leaves_) leaf.resize((range.outFrame-range.inFrame)*2);
}
void ResidentStemHold::captureAccepted(const LiveStemBuffer::Packet& packet) noexcept
{
    if (ready_.load(std::memory_order_relaxed) || packet.sourceId != sourceId_ || !packet.epoch ||
        !packet.frames || packet.frames > LiveStemBuffer::blockFrames) return;
    // Caller has successfully published this exact packet to LiveStemBuffer;
    // still reject malformed spans here before accessing any borrowed memory.
    for (const auto leaf : packet.stems)
        if (leaf.size() != packet.frames*2 || std::ranges::any_of(leaf,
            [](float sample) { return !std::isfinite(sample); })) return;
    if (epoch_ != packet.epoch) { epoch_ = packet.epoch; capturedEnd_ = range_.inFrame; }
    if (packet.firstFrame > capturedEnd_ || packet.firstFrame > range_.outFrame ||
        packet.frames > UINT64_MAX-packet.firstFrame) return;
    const auto end = std::min(range_.outFrame, packet.firstFrame+packet.frames);
    if (end <= capturedEnd_) return;
    const auto sourceOffset = (capturedEnd_-packet.firstFrame)*2;
    const auto destinationOffset = (capturedEnd_-range_.inFrame)*2;
    const auto count = (end-capturedEnd_)*2;
    for (std::size_t stem=0;stem<4;++stem)
        std::copy_n(packet.stems[stem].data()+sourceOffset,count,leaves_[stem].data()+destinationOffset);
    capturedEnd_ = end;
    if (capturedEnd_ == range_.outFrame) ready_.store(true,std::memory_order_release);
}
std::array<std::span<const float>,4> ResidentStemHold::leaves() const noexcept
{
    if (!ready()) return {};
    return {leaves_[0],leaves_[1],leaves_[2],leaves_[3]};
}
} // namespace conflux::audio

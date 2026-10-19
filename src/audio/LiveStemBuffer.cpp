// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/LiveStemBuffer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace conflux::audio {

class LiveStemBuffer::Impl {
public:
    struct Slot {
        std::array<std::array<float, blockFrames * channels>, stemCount> samples{};
        std::uint64_t sourceId = 0, epoch = 0, first = 0;
        std::size_t count = 0;
        bool final = false;
    };
    struct View {
        Impl* owner = nullptr;
        std::size_t stem = 0;
        std::uint64_t frame = 0, sequence = 0;
    };

    Impl(std::uint64_t sourceId, std::uint64_t totalFrames)
        : sourceId_(sourceId), frames_(totalFrames)
    {
        if (!sourceId || !totalFrames || totalFrames > std::numeric_limits<std::size_t>::max() / (channels * sizeof(float)))
            throw std::invalid_argument("Live stems require a nonzero identity and exact addressable stereo frame count");
        for (std::size_t stem = 0; stem < views_.size(); ++stem)
            views_[stem] = {this, stem, 0, 0};
    }

    std::array<AudioSource, stemCount> sourceRefs() noexcept
    {
        std::array<AudioSource, stemCount> sources{};
        for (std::size_t stem = 0; stem < sources.size(); ++stem) {
            sources[stem] = {frames_, nullptr, &views_[stem],
                [](void* context, std::uint64_t frame, std::span<float> output) noexcept {
                    auto& view = *static_cast<View*>(context);
                    return view.owner->read(view, frame, output);
                },
                [](void* context, std::uint64_t frame) noexcept {
                    auto& view = *static_cast<View*>(context);
                    view.owner->discontinuity(view.stem, frame);
                }};
        }
        return sources;
    }

    bool tryRequest(Request& output) const noexcept
    {
        // Same fixed-store mailbox as PcmReadAhead. Atomic payload plus SC
        // odd/even version prevent a mixed target/epoch snapshot without races.
        const auto before = requestVersion_.load(std::memory_order_seq_cst);
        if (before & 1U) return false;
        const auto frame = requestFrame_.load(std::memory_order_seq_cst);
        if (before != requestVersion_.load(std::memory_order_seq_cst)) return false;
        output = {sourceId_, before / 2, frame};
        return true;
    }

    PublishResult tryPublish(const Packet& packet) noexcept
    {
        if (packet.sourceId != sourceId_) return PublishResult::Invalid;
        Request request;
        if (!tryRequest(request) || packet.epoch != request.epoch) return PublishResult::Stale;
        if (packet.frames == 0 || packet.frames > blockFrames || packet.firstFrame > frames_ ||
            packet.frames > frames_ - packet.firstFrame ||
            packet.final != (packet.firstFrame + packet.frames == frames_))
            return PublishResult::Invalid;
        for (const auto source : packet.stems)
            if (source.size() != packet.frames * channels) return PublishResult::Invalid;
        const auto nextFrame = producerEpoch_ == request.epoch ? producerNextFrame_ : request.firstFrame;
        if (packet.firstFrame != nextFrame || writtenSequence_ == std::numeric_limits<std::uint64_t>::max())
            return PublishResult::Invalid;
        const auto released = released_.load(std::memory_order_acquire);
        if (writtenSequence_ - released >= slotCount) return PublishResult::Full;
        for (const auto source : packet.stems)
            if (std::ranges::any_of(source, [](float sample) { return !std::isfinite(sample); }))
                return PublishResult::Invalid;
        auto& slot = slots_[writtenSequence_ % slotCount];
        for (std::size_t stem = 0; stem < stemCount; ++stem)
            std::copy_n(packet.stems[stem].data(), packet.frames * channels, slot.samples[stem].data());
        slot.sourceId = sourceId_;
        slot.epoch = request.epoch;
        slot.first = packet.firstFrame;
        slot.count = packet.frames;
        slot.final = packet.final;
        // A seek can race this last check and release-store. At most this one
        // single-producer packet can then be stale; the render owner drops it.
        if (requestVersion_.load(std::memory_order_seq_cst) != request.epoch * 2)
            return PublishResult::Stale;
        written_.store(writtenSequence_ + 1, std::memory_order_release);
        ++writtenSequence_;
        producerEpoch_ = request.epoch;
        producerNextFrame_ = packet.firstFrame + packet.frames;
        return PublishResult::Accepted;
    }

    void retireStale() noexcept
    {
        for (std::size_t examined = 0; examined < slotCount; ++examined) {
            if (releasedSequence_ == written_.load(std::memory_order_acquire)) break;
            const auto& slot = slots_[releasedSequence_ % slotCount];
            if (slot.epoch == renderEpoch_) break;
            ++releasedSequence_;
            for (auto& view : views_)
                if (view.sequence < releasedSequence_) view.sequence = releasedSequence_;
            // Never touch a retired cell after handing it back to the producer.
            released_.store(releasedSequence_, std::memory_order_release);
        }
    }

    void releaseCommon() noexcept
    {
        auto common = views_.front().sequence;
        for (const auto& view : views_) common = std::min(common, view.sequence);
        if (common > releasedSequence_) {
            releasedSequence_ = common;
            released_.store(common, std::memory_order_release);
        }
    }

    void discontinuity(std::size_t stem, std::uint64_t frame) noexcept
    {
        if (frame > frames_ || renderFailed_) return;
        const auto bit = std::uint32_t(1U << stem);
        if (notificationMask_ && frame == notificationFrame_ && !(notificationMask_ & bit)) {
            notificationMask_ |= bit;
            if (notificationMask_ == (1U << stemCount) - 1U) notificationMask_ = 0;
            return;
        }
        if (renderEpoch_ == std::numeric_limits<std::uint64_t>::max() / 2) {
            renderFailed_ = true; // Do not wrap the mailbox and reuse an epoch.
            return;
        }
        ++renderEpoch_;
        notificationMask_ = bit;
        notificationFrame_ = frame;
        for (auto& view : views_) {
            view.frame = frame;
            view.sequence = releasedSequence_;
        }
        requestVersion_.store(renderEpoch_ * 2 - 1, std::memory_order_seq_cst);
        requestFrame_.store(frame, std::memory_order_seq_cst);
        requestVersion_.store(renderEpoch_ * 2, std::memory_order_seq_cst);
        retireStale();
    }

    AudioReadResult read(View& view, std::uint64_t frame, std::span<float> output) noexcept
    {
        if (output.size() % channels || output.size() / channels > blockFrames || frame > frames_ || frame != view.frame)
            return {0, true};
        if (renderFailed_) return {0, true};
        notificationMask_ = 0;
        retireStale();
        if (output.empty() || frame == frames_) return {};
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(output.size() / channels, frames_ - frame));
        AudioReadResult result;
        for (std::size_t examined = 0; examined < slotCount && result.frames < wanted; ++examined) {
            const auto available = written_.load(std::memory_order_acquire);
            if (view.sequence == available) break;
            const auto& slot = slots_[view.sequence % slotCount];
            if (slot.epoch != renderEpoch_) {
                // A late old publication is a prefix of new-epoch packets.
                // This drops it globally, never a currently borrowed new cell.
                retireStale();
                continue;
            }
            if (slot.sourceId != sourceId_ || view.frame < slot.first ||
                view.frame >= slot.first + slot.count) {
                result.failed = true;
                break;
            }
            const auto offset = static_cast<std::size_t>(view.frame - slot.first);
            const auto count = std::min(wanted - result.frames, slot.count - offset);
            std::copy_n(slot.samples[view.stem].data() + offset * channels, count * channels,
                        output.data() + result.frames * channels);
            result.frames += count;
            view.frame += count;
            if (offset + count == slot.count) {
                ++view.sequence;
                releaseCommon();
            }
        }
        return result;
    }

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    const std::uint64_t sourceId_, frames_;
    std::array<Slot, slotCount> slots_{};
    std::array<View, stemCount> views_{};
    // Producer alone owns its sequence/cursor. It never resets render indices.
    std::uint64_t writtenSequence_ = 0, producerEpoch_ = 0, producerNextFrame_ = 0;
    std::atomic<std::uint64_t> written_{0}, released_{0};
    // Render owner alone owns view cursors, retirement and seek invalidation.
    std::uint64_t releasedSequence_ = 0, renderEpoch_ = 1, notificationFrame_ = 0;
    std::uint32_t notificationMask_ = 0;
    bool renderFailed_ = false;
    std::atomic<std::uint64_t> requestVersion_{2}, requestFrame_{0};
};

LiveStemBuffer::LiveStemBuffer(std::uint64_t sourceId, std::uint64_t totalFrames)
    : impl_(std::make_unique<Impl>(sourceId, totalFrames)) {}
LiveStemBuffer::~LiveStemBuffer() = default;
std::uint64_t LiveStemBuffer::sourceId() const noexcept { return impl_->sourceId_; }
std::uint64_t LiveStemBuffer::frames() const noexcept { return impl_->frames_; }
std::array<AudioSource, LiveStemBuffer::stemCount> LiveStemBuffer::sourceRefs() noexcept { return impl_->sourceRefs(); }
bool LiveStemBuffer::tryRequest(Request& output) const noexcept { return impl_->tryRequest(output); }
LiveStemBuffer::PublishResult LiveStemBuffer::tryPublish(const Packet& packet) noexcept { return impl_->tryPublish(packet); }

} // namespace conflux::audio

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/LiveSession.hpp"
#include "audio/TransportQueue.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace conflux::audio {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto stride = RenderCore::channels * sizeof(float);

// The render thread publishes bounded POD snapshots; the control thread drains
// them. A full queue drops a snapshot, never a transport command.
class StateQueue {
public:
    bool push(const LiveSession::State& state) noexcept
    {
        const auto position = written_.load(std::memory_order_relaxed);
        if (position - read_.load(std::memory_order_acquire) >= states_.size())
            return false;
        states_[position % states_.size()] = state;
        written_.store(position + 1U, std::memory_order_release);
        return true;
    }
    bool pop(LiveSession::State& state) noexcept
    {
        const auto position = read_.load(std::memory_order_relaxed);
        if (position == written_.load(std::memory_order_acquire))
            return false;
        state = states_[position % states_.size()];
        read_.store(position + 1U, std::memory_order_release);
        return true;
    }
private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::is_trivially_copyable_v<LiveSession::State>);
    std::array<LiveSession::State, 32> states_{};
    std::atomic<std::uint32_t> written_{0}, read_{0};
};
} // namespace

class LiveSession::Impl {
public:
    Impl(std::span<const float> a, std::span<const float> b, const InitialState& initial)
        : renderer_(a.empty() && b.empty() ? RenderCore(false) : RenderCore(a, b))
    {
        if (!renderer_.setMasterGain(initial.masterGain))
            throw std::invalid_argument("Invalid initial master gain");
        for (std::size_t deck = 0; deck < RenderCore::deckCount; ++deck) {
            if (!std::isfinite(initial.deckGain[deck]) || initial.deckGain[deck] < 0 || initial.deckGain[deck] > 1)
                throw std::invalid_argument("Invalid initial deck gain");
            if (!renderer_.sourceFrames(deck)) {
                if (initial.sourceFrame[deck] || initial.playing[deck])
                    throw std::invalid_argument("Unloaded deck cannot have a source position or playing request");
                continue;
            }
            if (!renderer_.seek(deck, initial.sourceFrame[deck]) ||
                !renderer_.setDeckGain(deck, initial.deckGain[deck]) ||
                !renderer_.setPlaying(deck, initial.playing[deck]))
                throw std::invalid_argument("Invalid initial deck position");
        }
        stopped_ = capture();
    }

    ~Impl() { stop(); }

    bool ready() const noexcept
    {
        return ready_.load(std::memory_order_acquire) && !bufferFault_.load(std::memory_order_relaxed);
    }

    void setError(const char* message) noexcept
    {
        if (!error_[0])
            std::snprintf(error_.data(), error_.size(), "%s", message ? message : "PipeWire stream failed");
        ready_.store(false, std::memory_order_release);
    }

    static void stateChanged(void* userdata, pw_stream_state, pw_stream_state state, const char* error) noexcept
    {
        auto& self = *static_cast<Impl*>(userdata);
        if (self.shuttingDown_)
            return;
        if (state == PW_STREAM_STATE_STREAMING) {
            self.streaming_ = true;
            if (self.formatValid_ && !self.error_[0])
                self.ready_.store(true, std::memory_order_release);
        } else if (state == PW_STREAM_STATE_ERROR || self.streaming_) {
            self.setError(error ? error : "PipeWire stream stopped or disconnected");
        }
    }

    static void paramChanged(void* userdata, std::uint32_t id, const spa_pod* param) noexcept
    {
        auto& self = *static_cast<Impl*>(userdata);
        if (id != SPA_PARAM_Format || !param || self.shuttingDown_)
            return;
        spa_audio_info_raw format{};
        self.formatValid_ = spa_format_audio_raw_parse(param, &format) >= 0 &&
            format.format == SPA_AUDIO_FORMAT_F32 && format.rate == RenderCore::sampleRate &&
            format.channels == RenderCore::channels;
        if (!self.formatValid_)
            self.setError("PipeWire did not negotiate stereo 48000 Hz interleaved float audio");
        else if (self.streaming_ && !self.error_[0])
            self.ready_.store(true, std::memory_order_release);
    }

    void consumeCommands() noexcept
    {
        for (std::size_t count = 0; count < TransportQueue::capacity; ++count) {
            RenderCore::Command command;
            if (!commands_.tryPop(command))
                break;
            renderer_.apply(command) ? ++applied_ : ++rejected_;
            lastCommandId_ = command.id;
        }
    }

    State capture() const noexcept
    {
        State state;
        state.outputFrame = renderer_.frameClock();
        for (std::size_t deck = 0; deck < RenderCore::deckCount; ++deck) {
            state.sourceFrame[deck] = renderer_.sourceFrame(deck);
            state.sourceFrames[deck] = renderer_.sourceFrames(deck);
            state.playing[deck] = renderer_.isPlaying(deck);
            state.deckGain[deck] = renderer_.deckGain(deck);
        }
        state.masterGain = renderer_.masterGain();
        state.applied = applied_;
        state.rejected = rejected_;
        state.lastCommandId = lastCommandId_;
        state.callbacks = callbacks_;
        state.invalidBuffers = invalidBuffers_;
        state.snapshotDrops = snapshotDrops_;
        state.maxExecutionNs = maxExecutionNs_;
        state.deadlineMisses = deadlineMisses_;
        state.streamFramesMin = streamFramesMax_ ? streamFramesMin_ : 0;
        state.streamFramesMax = streamFramesMax_;
        state.ready = ready();
        return state;
    }

    static void process(void* userdata) noexcept
    {
        auto& self = *static_cast<Impl*>(userdata);
        const auto began = Clock::now();
        auto* buffer = pw_stream_dequeue_buffer(self.stream_);
        ++self.callbacks_;
        bool valid = false;
        std::uint64_t renderedFrames = 0;
        if (buffer && buffer->buffer && buffer->buffer->n_datas == 1) {
            auto& plane = buffer->buffer->datas[0];
            const auto capacity = plane.maxsize / stride;
            const auto requested = buffer->requested ? buffer->requested : capacity;
            if (plane.data && plane.chunk && capacity && requested <= capacity &&
                reinterpret_cast<std::uintptr_t>(plane.data) % alignof(float) == 0) {
                if (self.ready()) {
                    self.consumeCommands();
                    valid = self.renderer_.render({static_cast<float*>(plane.data),
                        static_cast<std::size_t>(requested * RenderCore::channels)});
                    renderedFrames = requested;
                    self.streamFramesMin_ = std::min<std::uint64_t>(self.streamFramesMin_, requested);
                    self.streamFramesMax_ = std::max<std::uint64_t>(self.streamFramesMax_, requested);
                } else {
                    // Negotiation/startup or a stream failure cannot send PCM
                    // in an unknown format. Return an empty buffer until ready.
                    valid = true;
                }
                plane.chunk->offset = 0;
                plane.chunk->stride = stride;
                plane.chunk->size = static_cast<std::uint32_t>(renderedFrames * stride);
                buffer->size = renderedFrames;
                if (pw_stream_queue_buffer(self.stream_, buffer) < 0)
                    valid = false;
            }
        }
        if (!valid) {
            ++self.invalidBuffers_;
            self.bufferFault_.store(true, std::memory_order_relaxed);
        }
        if (!self.states_.push(self.capture()))
            ++self.snapshotDrops_;
        // Include snapshot publication in callback execution time. A published
        // snapshot sees the previous timing aggregate; stoppedState is exact.
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began).count());
        self.maxExecutionNs_ = std::max(self.maxExecutionNs_, elapsed);
        if (renderedFrames && elapsed > renderedFrames * 1000000000ULL / RenderCore::sampleRate)
            ++self.deadlineMisses_;
    }

    bool start(std::string& error)
    {
        error.clear();
        if (loop_) {
            error = currentError();
            if (error.empty())
                return true;
            stop();
            return false;
        }
        static std::once_flag initialized;
        std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
        error_.fill(0);
        ready_.store(false, std::memory_order_release);
        bufferFault_.store(false, std::memory_order_relaxed);
        shuttingDown_ = streaming_ = formatValid_ = false;
        loop_ = pw_thread_loop_new("conflux-audio", nullptr);
        if (!loop_) {
            setError("Cannot create PipeWire thread loop");
        } else {
            events_.version = PW_VERSION_STREAM_EVENTS;
            events_.state_changed = stateChanged;
            events_.param_changed = paramChanged;
            events_.process = process;
            stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "CONFLUX manual audio",
                pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                    PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_NAME, "conflux-manual-audio",
                    PW_KEY_NODE_LATENCY, "256/48000", nullptr), &events_, this);
            if (!stream_)
                setError("Cannot create PipeWire stream");
            else if (pw_thread_loop_start(loop_) < 0)
                setError("Cannot start PipeWire thread loop");
            else {
                loopStarted_ = true;
                std::array<std::uint8_t, 1024> storage{};
                spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
                spa_audio_info_raw format{};
                format.format = SPA_AUDIO_FORMAT_F32;
                format.rate = RenderCore::sampleRate;
                format.channels = RenderCore::channels;
                format.position[0] = SPA_AUDIO_CHANNEL_FL;
                format.position[1] = SPA_AUDIO_CHANNEL_FR;
                const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
                const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                    PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_DONT_RECONNECT);
                started_ = Clock::now();
                pw_thread_loop_lock(loop_);
                if (pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0)
                    setError("Cannot connect PipeWire stream");
                const bool failed = error_[0];
                pw_thread_loop_unlock(loop_);
                if (!failed)
                    return true;
            }
        }
        error = currentError();
        stop();
        return false;
    }

    std::string currentError()
    {
        std::array<char, 256> message{};
        if (loopStarted_)
            pw_thread_loop_lock(loop_);
        if (loopStarted_ && !ready_.load(std::memory_order_acquire) && !error_[0] &&
            Clock::now() - started_ >= std::chrono::seconds(5))
            setError("PipeWire did not start streaming within five seconds");
        message = error_;
        if (loopStarted_)
            pw_thread_loop_unlock(loop_);
        if (bufferFault_.load(std::memory_order_relaxed))
            return "PipeWire output buffer validation or queueing failed";
        return message.data();
    }

    void stop() noexcept
    {
        if (loopStarted_)
            pw_thread_loop_lock(loop_);
        shuttingDown_ = true;
        ready_.store(false, std::memory_order_release);
        if (stream_) {
            pw_stream_disconnect(stream_);
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        if (loopStarted_) {
            pw_thread_loop_unlock(loop_);
            pw_thread_loop_stop(loop_);
            loopStarted_ = false;
        }
        if (loop_) {
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
        consumeCommands();
        State discarded;
        while (states_.pop(discarded)) {}
        stopped_ = capture();
    }

    RenderCore renderer_;
    TransportQueue commands_;
    StateQueue states_;
    pw_thread_loop* loop_ = nullptr;
    pw_stream* stream_ = nullptr;
    pw_stream_events events_{};
    std::array<char, 256> error_{};
    bool loopStarted_ = false, shuttingDown_ = false, streaming_ = false, formatValid_ = false;
    static_assert(std::atomic<bool>::is_always_lock_free);
    std::atomic<bool> ready_{false}, bufferFault_{false};
    Clock::time_point started_{};
    std::uint64_t applied_ = 0, rejected_ = 0, lastCommandId_ = 0;
    std::uint64_t callbacks_ = 0, invalidBuffers_ = 0, snapshotDrops_ = 0;
    std::uint64_t maxExecutionNs_ = 0, deadlineMisses_ = 0;
    std::uint64_t streamFramesMin_ = std::numeric_limits<std::uint64_t>::max(), streamFramesMax_ = 0;
    State stopped_{};
};

LiveSession::LiveSession(std::span<const float> a, std::span<const float> b)
    : LiveSession(a, b, InitialState{}) {}

LiveSession::LiveSession(std::span<const float> a, std::span<const float> b, const InitialState& initial)
    : impl_(std::make_unique<Impl>(a, b, initial)) {}

LiveSession::~LiveSession() = default;

bool LiveSession::start(std::string& error) { return impl_->start(error); }
bool LiveSession::ready() const noexcept { return impl_->ready(); }
std::string LiveSession::error() { return impl_->currentError(); }
bool LiveSession::submit(const RenderCore::Command& command) noexcept
{ return impl_->ready() && impl_->commands_.tryPush(command); }
bool LiveSession::pollState(State& state) noexcept
{
    if (!impl_->states_.pop(state))
        return false;
    state.ready = impl_->ready();
    return true;
}
void LiveSession::stop() noexcept { impl_->stop(); }
LiveSession::State LiveSession::stoppedState() const noexcept { return impl_->stopped_; }

} // namespace conflux::audio

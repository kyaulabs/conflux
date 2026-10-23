// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/LiveSession.hpp"
#include "audio/TransportQueue.hpp"
#include "audio/AudioFile.hpp"
#include "audio/PreparedPlayback.hpp"
#include "audio/RecorderTap.hpp"
#include "audio/PipeWireMetadata.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/extensions/metadata.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <type_traits>

namespace conflux::audio {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t scratchFrames = 1024;
constexpr std::array<const char*, 4> outputNames{"master_FL", "master_FR", "cue_FL", "cue_FR"};
constexpr std::uint32_t invalidId = PW_ID_ANY;

std::uint64_t number(const char* value, std::uint64_t fallback = 0) noexcept
{
    if (!value) return fallback;
    std::uint64_t result = 0;
    const auto end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end ? result : fallback;
}
std::uint32_t objectId(const char* value) noexcept
{
    const auto result = number(value, invalidId);
    return result <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(result) : invalidId;
}
bool flag(const char* value) noexcept
{ return value && (std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0); }
const char* property(const spa_dict* properties, const char* key) noexcept
{ return properties ? spa_dict_lookup(properties, key) : nullptr; }
void updateString(std::string& output, const spa_dict* properties, const char* key)
{ if (const auto* value = property(properties, key)) output = value; }

CallbackTiming::ClockCapabilities clockCapabilities() noexcept
{
    CallbackTiming::ClockCapabilities result;
#ifdef SPA_IO_CLOCK_FLAG_DISCONT
    result.discontinuityFlag = SPA_IO_CLOCK_FLAG_DISCONT;
#endif
#ifdef SPA_IO_CLOCK_FLAG_XRUN_RECOVER
    result.cycle = true;
    result.xrunRecoveryFlag = SPA_IO_CLOCK_FLAG_XRUN_RECOVER;
#endif
    return result;
}

// Render owner writes fixed POD snapshots; GUI delay drops snapshots only.
class StateQueue {
public:
    bool push(const LiveSession::State& state) noexcept
    {
        const auto position = written_.load(std::memory_order_relaxed);
        if (position - read_.load(std::memory_order_acquire) >= states_.size()) return false;
        states_[position % states_.size()] = state;
        written_.store(position + 1U, std::memory_order_release);
        return true;
    }
    bool pop(LiveSession::State& state) noexcept
    {
        const auto position = read_.load(std::memory_order_relaxed);
        if (position == written_.load(std::memory_order_acquire)) return false;
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
    { initialize(initial); }

    static PlaybackSources makePlaybacks(PreparedSources sources)
    {
        PlaybackSources playbacks;
        for (std::size_t deck = 0; deck < playbacks.size(); ++deck) {
            if (!sources[deck]) continue;
            playbacks[deck] = std::make_unique<PreparedPlayback>(sources[deck]->descriptor(), sources[deck]->frames());
        }
        return playbacks;
    }
    static PlaybackSources primePlaybacks(PlaybackSources playbacks, const InitialState& initial,
                                           std::stop_token stop)
    {
        for (std::size_t deck = 0; deck < playbacks.size(); ++deck) {
            if (stop.stop_requested()) throw std::runtime_error("Playback installation cancelled");
            if (!playbacks[deck]) continue;
            const auto timeout = playbacks[deck]->isLive() ? PreparedPlayback::liveWait : std::chrono::seconds(2);
            if (!playbacks[deck]->primeForSession(initial.sourceFrame[deck], timeout, stop))
                throw std::runtime_error("Cannot prime prepared PCM at the initial source position: " + playbacks[deck]->error());
        }
        return playbacks;
    }
    static RenderCore makeRenderer(const PlaybackSources& playbacks)
    {
        if (!playbacks[0] && !playbacks[1]) return RenderCore(false);
        return RenderCore(playbacks[0] ? playbacks[0]->sourceRef() : AudioSource{},
                          playbacks[1] ? playbacks[1]->sourceRef() : AudioSource{});
    }
    Impl(PreparedSources sources, const InitialState& initial)
        : Impl(makePlaybacks(sources), initial, {}) {}
    Impl(PlaybackSources sources, const InitialState& initial, std::stop_token stop)
        : playbacks_(primePlaybacks(std::move(sources), initial, stop)), renderer_(makeRenderer(playbacks_))
    { initialize(initial); }

    void initialize(const InitialState& initial)
    {
        for (const auto& playback : playbacks_)
            if (playback && !playback->configureStemRamps(0))
                throw std::invalid_argument("Cannot restore gains on an already-rendered stem bank");
        if (!renderer_.setMasterGain(initial.masterGain) || !renderer_.setCueGain(initial.cueGain))
            throw std::invalid_argument("Invalid initial master or cue gain");
        if (!renderer_.setCrossfader(initial.crossfader))
            throw std::invalid_argument("Invalid initial crossfader position");
        for (std::size_t deck = 0; deck < RenderCore::deckCount; ++deck) {
            activePlaybacks_[deck] = playbacks_[deck].get();
            if (!std::isfinite(initial.deckGain[deck]) || initial.deckGain[deck] < 0 || initial.deckGain[deck] > 1)
                throw std::invalid_argument("Invalid initial deck gain");
            for (std::size_t band = 0; band < RenderCore::eqBandCount; ++band)
                if (!renderer_.setEqGainDb(deck, band, initial.eqGainDb[deck][band]))
                    throw std::invalid_argument("Invalid initial channel EQ");
            for (std::size_t stem = 0; stem < RenderCore::stemCount; ++stem) {
                const auto gain = initial.stemGain[deck][stem];
                if (!std::isfinite(gain) || gain < 0.F || gain > 1.F)
                    throw std::invalid_argument("Invalid initial stem gain");
                if (renderer_.isStemBank(deck)) {
                    if (!renderer_.setStemGain(deck, stem, gain))
                        throw std::invalid_argument("Cannot restore initial stem gain");
                } else if (gain != 1.F) {
                    throw std::invalid_argument("Normal or unloaded deck cannot have stem gains");
                }
            }
            if (!renderer_.sourceFrames(deck)) {
                if (initial.sourceFrame[deck] || initial.cueFrame[deck] || initial.playing[deck] || initial.cueSelected[deck] ||
                    initial.residentHoldRangeId[deck])
                    throw std::invalid_argument("Unloaded deck cannot have transport or cue selection");
                continue;
            }
            if (!renderer_.setCueFrame(deck, initial.cueFrame[deck]) ||
                !renderer_.restorePrimedCursor(deck, initial.sourceFrame[deck]) ||
                !renderer_.setDeckGain(deck, initial.deckGain[deck]) ||
                !renderer_.setPlaying(deck, initial.playing[deck]) ||
                !renderer_.setCueSelected(deck, initial.cueSelected[deck]))
                throw std::invalid_argument("Invalid initial deck state");
            if (initial.residentHoldRangeId[deck] && !renderer_.residentHoldControl(deck,
                AudioResidentHoldCommand::Arm,initial.residentHoldRangeId[deck],renderer_.transportRevision(deck)))
                throw std::invalid_argument("Invalid initial resident stem hold authorization");
        }
        if (!renderer_.configureRamps(240))
            throw std::invalid_argument("Cannot configure live transport ramps");
        // Restore targets while banks still use their exact zero-ramp default,
        // then enable live smoothing before any callback can borrow the owner.
        for (const auto& playback : playbacks_)
            if (playback && !playback->configureStemRamps(240))
                throw std::invalid_argument("Cannot configure live stem gain ramps");
        for (std::size_t index = 0; index < owned_.size(); ++index) {
            owned_[index].self = this;
            owned_[index].index = index;
        }
        stopped_ = capture();
    }
    ~Impl() { stop(); }

    struct NodeWatch {
        Impl* self = nullptr;
        std::uint32_t id = invalidId;
        std::uint64_t serial = 0;
        std::string name, mediaClass;
        pw_node_state state = PW_NODE_STATE_CREATING;
        pw_node* proxy = nullptr;
        spa_hook hook{};
    };
    struct PortWatch {
        Impl* self = nullptr;
        PortIdentity identity;
        bool control = false;
        pw_port* proxy = nullptr;
        spa_hook hook{};
    };
    struct LinkWatch {
        Impl* self = nullptr;
        std::uint32_t id = invalidId, output = invalidId, input = invalidId;
        pw_link* proxy = nullptr;
        spa_hook hook{};
    };
    struct OwnedLink {
        Impl* self = nullptr;
        std::size_t index = 0;
        pw_link* proxy = nullptr;
        spa_hook infoHook{}, proxyHook{};
        std::uint32_t globalId = invalidId, output = invalidId, input = invalidId;
        pw_link_state state = PW_LINK_STATE_INIT;
        bool seen = false;
    };

    template<class Function> void control(Function&& function) noexcept
    {
        try { function(); }
        catch (...) { setError("Cannot update PipeWire routing discovery"); }
    }
    bool ready() const noexcept
    {
        return ready_.load(std::memory_order_acquire) && graphValid_.load(std::memory_order_acquire) &&
            masterBuffersReady_.load(std::memory_order_acquire) &&
            masterGate_.load(std::memory_order_acquire) && !bufferFault_.load(std::memory_order_relaxed) &&
            !graphFault_.load(std::memory_order_relaxed);
    }
    void setError(const char* message) noexcept
    {
        if (!error_[0]) std::snprintf(error_.data(), error_.size(), "%s", message ? message : "PipeWire filter failed");
        ready_.store(false, std::memory_order_release);
        masterGate_.store(false, std::memory_order_release);
        cueGate_.store(false, std::memory_order_release);
    }
    void failCue(CueRouteStatus status, const char* message)
    {
        cueGate_.store(false, std::memory_order_release);
        cueFailed_ = true;
        cueStatus_.store(status, std::memory_order_release);
        cueError_ = message;
        // A sink removes both links before the client receives both remove_id
        // events. Wait for their ordering barrier before destroying the pair:
        // pw_proxy_destroy then frees removed proxies without a stale request.
        if (cueCleanupSequence_ < 0) {
            cueCleanupSequence_ = pw_core_sync(core_, PW_ID_CORE, 0);
            if (cueCleanupSequence_ < 0) setError("Cannot confirm cue link cleanup");
        }
    }
    void destroyOwnedCueLinks() noexcept
    {
        // Both proxies are ours and non-lingering. Any still-live partial pair
        // is rolled back on the control loop, with the cue gate already closed.
        for (std::size_t index = 2; index < owned_.size(); ++index) {
            auto& link = owned_[index];
            if (!link.proxy) continue;
            auto* proxy = link.proxy;
            link.proxy = nullptr;
            spa_hook_remove(&link.infoHook);
            spa_hook_remove(&link.proxyHook);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(proxy));
            link.seen = false;
        }
    }

    static void updateNode(NodeWatch& node, const spa_dict* properties)
    {
        if (const auto* value = property(properties, PW_KEY_OBJECT_SERIAL)) node.serial = number(value);
        updateString(node.name, properties, PW_KEY_NODE_NAME);
        updateString(node.mediaClass, properties, PW_KEY_MEDIA_CLASS);
    }
    static void updatePort(PortWatch& port, const spa_dict* properties)
    {
        auto& identity = port.identity;
        if (const auto* value = property(properties, PW_KEY_OBJECT_SERIAL)) identity.serial = number(value);
        if (const auto* value = property(properties, PW_KEY_NODE_ID)) identity.nodeId = objectId(value);
        if (const auto* value = property(properties, PW_KEY_PORT_DIRECTION)) identity.input = std::strcmp(value, "in") == 0;
        if (const auto* value = property(properties, PW_KEY_PORT_MONITOR)) identity.monitor = flag(value);
        if (const auto* value = property(properties, PW_KEY_PORT_CONTROL)) port.control = flag(value);
        if (const auto* value = property(properties, PW_KEY_FORMAT_DSP))
            identity.monoDsp = std::strcmp(value, "32 bit float mono audio") == 0;
        updateString(identity.portName, properties, PW_KEY_PORT_NAME);
        updateString(identity.channel, properties, PW_KEY_AUDIO_CHANNEL);
    }
    static void nodeInfo(void* data, const pw_node_info* info) noexcept
    {
        auto& node = *static_cast<NodeWatch*>(data);
        node.self->control([&] {
            updateNode(node, info->props);
            node.state = info->state;
            node.self->routingChanged();
        });
    }
    static void portInfo(void* data, const pw_port_info* info) noexcept
    {
        auto& port = *static_cast<PortWatch*>(data);
        port.self->control([&] {
            updatePort(port, info->props);
            port.identity.input = info->direction == PW_DIRECTION_INPUT;
            port.self->routingChanged();
        });
    }
    static void observedLinkInfo(void* data, const pw_link_info* info) noexcept
    {
        auto& link = *static_cast<LinkWatch*>(data);
        link.self->control([&] {
            link.output = info->output_port_id;
            link.input = info->input_port_id;
            link.self->refreshRouting();
        });
    }
    static void ownedLinkInfo(void* data, const pw_link_info* info) noexcept
    {
        auto& link = *static_cast<OwnedLink*>(data);
        link.self->control([&] {
            link.globalId = info->id;
            link.output = info->output_port_id;
            link.input = info->input_port_id;
            link.state = info->state;
            link.seen = true;
            link.self->refreshRouting();
        });
    }
    static void ownedBound(void* data, std::uint32_t id) noexcept
    {
        auto& link = *static_cast<OwnedLink*>(data);
        link.globalId = id;
    }
    static void ownedRemoved(void* data) noexcept
    {
        auto& link = *static_cast<OwnedLink*>(data);
        link.self->control([&] { link.state = PW_LINK_STATE_UNLINKED; link.self->refreshRouting(); });
    }
    static void ownedError(void* data, int, int, const char*) noexcept
    {
        auto& link = *static_cast<OwnedLink*>(data);
        link.self->control([&] { link.state = PW_LINK_STATE_ERROR; link.self->refreshRouting(); });
    }
    static int metadataProperty(void* data, std::uint32_t subject, const char* key, const char*, const char* value) noexcept
    {
        auto& self = *static_cast<Impl*>(data);
        self.control([&] {
            if (subject != PW_ID_CORE || (key && std::strcmp(key, "default.audio.sink") != 0)) return;
            std::array<char, 4096> name{};
            self.defaultSinkName_.clear();
            if (key && detail::parseDefaultSinkName(value, name))
                self.defaultSinkName_ = name.data();
            // A resolved route is pinned. Default changes apply at the next
            // explicit start, never by retargeting a running cue/master pair.
            self.routingChanged();
        });
        return 0;
    }
    static void global(void* data, std::uint32_t id, std::uint32_t, const char* type,
                       std::uint32_t version, const spa_dict* properties) noexcept
    {
        auto& self = *static_cast<Impl*>(data);
        self.control([&] {
            if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
                auto node = std::make_unique<NodeWatch>();
                node->self = &self; node->id = id;
                updateNode(*node, properties);
                node->proxy = static_cast<pw_node*>(pw_registry_bind(self.registry_, id, type,
                    std::min(version, std::uint32_t(PW_VERSION_NODE)), 0));
                if (!node->proxy) return;
                auto* record = node.get();
                self.nodes_.emplace(id, std::move(node));
                pw_node_add_listener(record->proxy, &record->hook, &nodeEvents(), record);
            } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
                auto port = std::make_unique<PortWatch>();
                port->self = &self; port->identity.globalId = id;
                updatePort(*port, properties);
                port->proxy = static_cast<pw_port*>(pw_registry_bind(self.registry_, id, type,
                    std::min(version, std::uint32_t(PW_VERSION_PORT)), 0));
                if (!port->proxy) return;
                auto* record = port.get();
                self.ports_.emplace(id, std::move(port));
                pw_port_add_listener(record->proxy, &record->hook, &portEvents(), record);
            } else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
                auto link = std::make_unique<LinkWatch>();
                link->self = &self; link->id = id;
                link->output = objectId(property(properties, PW_KEY_LINK_OUTPUT_PORT));
                link->input = objectId(property(properties, PW_KEY_LINK_INPUT_PORT));
                link->proxy = static_cast<pw_link*>(pw_registry_bind(self.registry_, id, type,
                    std::min(version, std::uint32_t(PW_VERSION_LINK)), 0));
                if (!link->proxy) return;
                auto* record = link.get();
                self.links_.emplace(id, std::move(link));
                pw_link_add_listener(record->proxy, &record->hook, &observedLinkEvents(), record);
            } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && !self.metadata_ &&
                       property(properties, PW_KEY_METADATA_NAME) &&
                       std::strcmp(property(properties, PW_KEY_METADATA_NAME), "default") == 0) {
                self.metadata_ = static_cast<pw_metadata*>(pw_registry_bind(self.registry_, id, type,
                    std::min(version, std::uint32_t(PW_VERSION_METADATA)), 0));
                if (self.metadata_) {
                    self.metadataId_ = id;
                    pw_metadata_add_listener(self.metadata_, &self.metadataHook_, &metadataEvents(), &self);
                }
            }
            self.routingChanged();
        });
    }
    template<class Record> static void destroyWatch(Record& record) noexcept
    {
        spa_hook_remove(&record.hook);
        if (record.proxy) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(record.proxy));
    }
    static void globalRemove(void* data, std::uint32_t id) noexcept
    {
        auto& self = *static_cast<Impl*>(data);
        self.control([&] {
            if (const auto found = self.nodes_.find(id); found != self.nodes_.end()) {
                destroyWatch(*found->second); self.nodes_.erase(found);
            }
            if (const auto found = self.ports_.find(id); found != self.ports_.end()) {
                destroyWatch(*found->second); self.ports_.erase(found);
            }
            if (const auto found = self.links_.find(id); found != self.links_.end()) {
                destroyWatch(*found->second); self.links_.erase(found);
            }
            for (auto& link : self.owned_)
                if (link.globalId == id) link.state = PW_LINK_STATE_UNLINKED;
            if (self.metadataId_ == id) {
                spa_hook_remove(&self.metadataHook_);
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(self.metadata_));
                self.metadata_ = nullptr; self.metadataId_ = invalidId; self.defaultSinkName_.clear();
            }
            self.routingChanged();
        });
    }
    static void coreDone(void* data, std::uint32_t id, int sequence) noexcept
    {
        auto& self = *static_cast<Impl*>(data);
        self.control([&] {
            if (id != PW_ID_CORE) return;
            if (sequence == self.discoverySequence_) {
                if (self.discoveryRound_++ == 0)
                    self.discoverySequence_ = pw_core_sync(self.core_, PW_ID_CORE, 0);
                else self.discoveryReady_ = true;
                self.routingChanged();
            }
            if (sequence == self.linksSequence_) {
                self.linksConfirmed_ = true;
                self.refreshRouting();
            }
            if (sequence == self.cueCleanupSequence_) {
                self.cueCleanupSequence_ = -1;
                self.destroyOwnedCueLinks();
            }
        });
    }
    static void coreError(void* data, std::uint32_t id, int, int, const char* message) noexcept
    {
        auto& self = *static_cast<Impl*>(data);
        self.control([&] {
            for (auto& link : self.owned_)
                if (link.proxy && pw_proxy_get_id(reinterpret_cast<pw_proxy*>(link.proxy)) == id) {
                    link.state = PW_LINK_STATE_ERROR; self.refreshRouting(); return;
                }
            if (id == PW_ID_CORE) self.setError(message);
        });
    }
    static void filterState(void* data, pw_filter_state, pw_filter_state state, const char* error) noexcept
    {
        auto& self = *static_cast<Impl*>(data);
        self.control([&] {
            if (self.shuttingDown_) return;
            self.filterState_ = state;
            if (state == PW_FILTER_STATE_ERROR) self.setError(error);
            else if (state == PW_FILTER_STATE_UNCONNECTED && self.activated_)
                self.setError("PipeWire output filter disconnected");
            self.routingChanged();
        });
    }

    // Event tables must outlive listener registration.
    static const pw_node_events& nodeEvents() { static const auto value = [] { pw_node_events v{}; v.version=PW_VERSION_NODE_EVENTS; v.info=nodeInfo; return v; }(); return value; }
    static const pw_port_events& portEvents() { static const auto value = [] { pw_port_events v{}; v.version=PW_VERSION_PORT_EVENTS; v.info=portInfo; return v; }(); return value; }
    static const pw_link_events& observedLinkEvents() { static const auto value = [] { pw_link_events v{}; v.version=PW_VERSION_LINK_EVENTS; v.info=observedLinkInfo; return v; }(); return value; }
    static const pw_link_events& ownedLinkEvents() { static const auto value = [] { pw_link_events v{}; v.version=PW_VERSION_LINK_EVENTS; v.info=ownedLinkInfo; return v; }(); return value; }
    static const pw_proxy_events& ownedProxyEvents() { static const auto value = [] { pw_proxy_events v{}; v.version=PW_VERSION_PROXY_EVENTS; v.bound=ownedBound; v.removed=ownedRemoved; v.error=ownedError; return v; }(); return value; }
    static const pw_metadata_events& metadataEvents() { static const auto value = [] { pw_metadata_events v{}; v.version=PW_VERSION_METADATA_EVENTS; v.property=metadataProperty; return v; }(); return value; }
    static const pw_registry_events& registryEvents() { static const auto value = [] { pw_registry_events v{}; v.version=PW_VERSION_REGISTRY_EVENTS; v.global=global; v.global_remove=globalRemove; return v; }(); return value; }
    static const pw_core_events& coreEvents() { static const auto value = [] { pw_core_events v{}; v.version=PW_VERSION_CORE_EVENTS; v.done=coreDone; v.error=coreError; return v; }(); return value; }
    static const pw_filter_events& filterEvents() { static const auto value = [] { pw_filter_events v{}; v.version=PW_VERSION_FILTER_EVENTS; v.state_changed=filterState; v.process=process; return v; }(); return value; }

    RoutingSnapshot collectRouting() const
    {
        RoutingSnapshot snapshot;
        snapshot.defaultSinkName = defaultSinkName_;
        for (const auto& [id, watch] : ports_) {
            (void)id;
            auto identity = watch->identity;
            const auto node = nodes_.find(identity.nodeId);
            if (node == nodes_.end()) continue;
            identity.nodeSerial = node->second->serial;
            identity.nodeName = node->second->name;
            identity.audioSink = node->second->mediaClass == "Audio/Sink" && node->second->state != PW_NODE_STATE_ERROR;
            identity.monitor |= watch->control;
            if (isPlaybackPort(identity)) snapshot.playbackPorts.push_back(std::move(identity));
        }
        return snapshot;
    }
    RoutingSnapshot routingSnapshotUnlocked() const
    {
        auto snapshot = collectRouting();
        snapshot.defaultMaster = resolveAudioRouting(snapshot, {}).master;
        snapshot.master = routes_.master; snapshot.cue = routes_.cue;
        snapshot.masterReady = ready(); snapshot.cueReady = cueGate_.load(std::memory_order_acquire) &&
            cueBuffersReady_.load(std::memory_order_acquire) && ready();
        snapshot.cueStatus = cueStatus_.load(std::memory_order_acquire);
        snapshot.masterError = error_[0] ? error_.data() : routes_.masterError;
        snapshot.cueError = cueError_;
        return snapshot;
    }
    RoutingSnapshot routingSnapshot() const
    {
        if (loopStarted_) pw_thread_loop_lock(loop_);
        try {
            auto result = routingSnapshotUnlocked();
            if (loopStarted_) pw_thread_loop_unlock(loop_);
            return result;
        } catch (...) {
            if (loopStarted_) pw_thread_loop_unlock(loop_);
            throw;
        }
    }
    bool findOutputPorts()
    {
        const auto node = pw_filter_get_node_id(filter_);
        if (node == invalidId) return false;
        outputs_.fill(invalidId);
        for (const auto& [id, port] : ports_)
            if (port->identity.nodeId == node && !port->identity.input)
                for (std::size_t index = 0; index < outputNames.size(); ++index)
                    if (port->identity.portName == outputNames[index]) {
                        if (outputs_[index] != invalidId) { setError("Output filter port names are ambiguous"); return false; }
                        outputs_[index] = id;
                    }
        return std::all_of(outputs_.begin(), outputs_.end(), [](auto id) { return id != invalidId; });
    }
    std::uint32_t destination(std::size_t index) const noexcept
    {
        const auto& pair = index < 2 ? routes_.master : routes_.cue;
        if (!pair) return invalidId;
        return index % 2 ? pair->right.globalId : pair->left.globalId;
    }
    bool createLink(std::size_t index)
    {
        auto& link = owned_[index];
        auto* properties = pw_properties_new(PW_KEY_OBJECT_LINGER, "false", nullptr);
        if (!properties) return false;
        pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u", outputs_[index]);
        pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u", destination(index));
        link.proxy = static_cast<pw_link*>(pw_core_create_object(core_, "link-factory", PW_TYPE_INTERFACE_Link,
            PW_VERSION_LINK, &properties->dict, 0));
        pw_properties_free(properties);
        if (!link.proxy) return false;
        pw_proxy_add_listener(reinterpret_cast<pw_proxy*>(link.proxy), &link.proxyHook, &ownedProxyEvents(), &link);
        pw_link_add_listener(link.proxy, &link.infoHook, &ownedLinkEvents(), &link);
        return true;
    }
    void routingChanged()
    {
        if (shuttingDown_ || !filter_) return;
        if (discoveryOnly_) return;
        if (!routesCreated_ && discoveryReady_ && findOutputPorts()) {
            routes_ = resolveAudioRouting(collectRouting(), config_);
            cueStatus_.store(routes_.cueStatus, std::memory_order_release);
            cueError_ = routes_.cueError;
            if (!routes_.master) return;
            routesCreated_ = true;
            if (!createLink(0) || !createLink(1)) { setError("Cannot create the owned master links"); return; }
            if (routes_.cue && (!createLink(2) || !createLink(3)))
                failCue(CueRouteStatus::LinkError, "Cannot create both owned cue links; cue remains muted");
            linksSequence_ = pw_core_sync(core_, PW_ID_CORE, 0);
            if (linksSequence_ < 0) setError("Cannot confirm the owned output links");
        }
        refreshRouting();
    }
    void refreshRouting()
    {
        if (shuttingDown_ || !routesCreated_ || !routes_.master || error_[0]) return;
        const auto check = resolveAudioRouting(collectRouting(), RoutingConfig{routes_.master, routes_.cue});
        if (!check.master) { setError("Master playback endpoint disappeared or changed; select it again"); return; }
        if (routes_.cue && !check.cue && !cueFailed_)
            failCue(CueRouteStatus::MissingEndpoint, "Cue playback endpoint disappeared or changed; select it again");
        for (std::size_t index = 0; index < owned_.size(); ++index) {
            const auto& link = owned_[index];
            if (!link.proxy) continue;
            if (link.state < PW_LINK_STATE_INIT ||
                (link.seen && (link.output != outputs_[index] || link.input != destination(index)))) {
                if (index < 2) { setError("Owned master link failed or changed; select the output again"); return; }
                if (!cueFailed_) failCue(CueRouteStatus::LinkError, "Owned cue link failed or changed; select it again");
            }
        }
        if (linksConfirmed_ && routes_.cue && !cueFailed_)
            for (const auto& [id, link] : links_)
                for (std::size_t index = 2; index < 4; ++index)
                    if (link->output == outputs_[index] &&
                        (id != owned_[index].globalId || link->input != destination(index)))
                        failCue(CueRouteStatus::UnexpectedLink, "Unexpected outgoing cue link; cue muted until explicitly reselected");
        if (routes_.cue && cueBufferFault_.load(std::memory_order_relaxed) && !cueFailed_)
            failCue(CueRouteStatus::LinkError, "Cue output buffer became unavailable; cue remains muted");
        auto healthy = [&](std::size_t index) {
            const auto& link = owned_[index];
            return link.proxy && link.seen && link.state >= PW_LINK_STATE_PAUSED &&
                link.output == outputs_[index] && link.input == destination(index);
        };
        auto bound = [&](std::size_t index) {
            const auto& link = owned_[index];
            return link.proxy && link.globalId != invalidId && link.seen && link.state >= PW_LINK_STATE_INIT &&
                link.output == outputs_[index] && link.input == destination(index);
        };
        // PipeWire prepares links only while both endpoint nodes are active.
        // Activate the zero-producing filter after exact link identities are
        // confirmed; audio gates still require PAUSED/ACTIVE links below.
        if (linksConfirmed_ && bound(0) && bound(1) && !activated_) {
            activated_ = true;
            if (pw_filter_set_active(filter_, true) < 0) { setError("Cannot activate the routed output filter"); return; }
        }
        const bool master = linksConfirmed_ && healthy(0) && healthy(1);
        masterGate_.store(master, std::memory_order_release);
        const bool cue = master && routes_.cue && !cueFailed_ && healthy(2) && healthy(3);
        cueGate_.store(cue, std::memory_order_release);
        if (cue) cueStatus_.store(cueBuffersReady_.load(std::memory_order_acquire) ?
            CueRouteStatus::Ready : CueRouteStatus::Pending, std::memory_order_release);
        ready_.store(master && activated_ && filterState_ == PW_FILTER_STATE_STREAMING, std::memory_order_release);
    }

    void consumeCommands() noexcept
    {
        for (std::size_t count = 0; count < TransportQueue::capacity; ++count) {
            RenderCore::Command command;
            if (!commands_.tryPop(command)) break;
            const bool scratch = command.type == RenderCore::Command::Type::ScratchBegin ||
                command.type == RenderCore::Command::Type::ScratchMove ||
                command.type == RenderCore::Command::Type::ScratchEnd;
            const bool hold = command.type == RenderCore::Command::Type::ResidentHoldArm ||
                command.type == RenderCore::Command::Type::ResidentHoldEnter ||
                command.type == RenderCore::Command::Type::ResidentHoldExit ||
                command.type == RenderCore::Command::Type::ResidentHoldCancel;
            const bool validDeck = command.deck < sourceGeneration_.size();
            const bool stale = (scratch || hold) && validDeck && command.sourceGeneration != sourceGeneration_[command.deck];
            // A paired release of an already retired source is a safe no-op;
            // it must never stop a new source's gesture. Stale new motion fails.
            const bool applied = (scratch || hold) && !validDeck ? false : stale ?
                command.type == RenderCore::Command::Type::ScratchEnd && command.scratchToken != 0 : renderer_.apply(command);
            applied ? ++applied_ : ++rejected_;
            lastCommandId_ = command.id;
        }
    }
    bool requestSourceReplacement(std::size_t deck, std::unique_ptr<PreparedPlayback>& candidate,
                                  std::uint64_t generation) noexcept
    {
        if (!ready() || !candidate || replacementPlayback_ || handoff_.busy() ||
            deck >= RenderCore::deckCount || generation <= lastRequestedGeneration_)
            return false;
        const auto source = candidate->sourceRef();
        if (!candidate->configureStemRamps(240)) return false;
        if (!handoff_.request({deck, source, candidate.get(), generation})) return false;
        replacementDeck_ = deck;
        replacementKind_ = SourceHandoff::Kind::Replacement;
        lastRequestedGeneration_ = generation;
        replacementPlayback_ = std::move(candidate);
        return true;
    }
    bool requestSourceContinuation(std::size_t deck, std::unique_ptr<PreparedPlayback>& candidate,
                                   std::uint64_t expectedSourceGeneration,
                                   std::uint64_t expectedTransportRevision, std::uint64_t frame,
                                   std::uint64_t generation) noexcept
    {
        if (!ready() || !candidate || candidate->isLive() || candidate->errorCode() ||
            replacementPlayback_ || handoff_.busy() || deck >= RenderCore::deckCount ||
            generation <= lastRequestedGeneration_ || generation <= expectedSourceGeneration ||
            frame >= candidate->frames() ||
            expectedTransportRevision == std::numeric_limits<std::uint64_t>::max())
            return false;
        const auto source = candidate->sourceRef();
        if (!source.setStemGain || !source.stemGain || !candidate->configureStemRamps(240) ||
            !candidate->primedForContinuation(frame)) return false;
        if (!handoff_.request({deck, source, candidate.get(), generation, SourceHandoff::Kind::Continuation,
                              expectedSourceGeneration, expectedTransportRevision, frame}))
            return false;
        replacementDeck_ = deck;
        replacementKind_ = SourceHandoff::Kind::Continuation;
        lastRequestedGeneration_ = generation;
        replacementPlayback_ = std::move(candidate);
        return true;
    }
    std::optional<SourceReplacementResult> takeSourceReplacementResult() noexcept
    {
        SourceHandoff::Completion completion;
        if (!handoff_.takeCompletion(completion)) return std::nullopt;
        std::unique_ptr<PreparedPlayback> retired;
        if (completion.installed) {
            retired = std::move(playbacks_[completion.deck]);
            playbacks_[completion.deck] = std::move(replacementPlayback_);
        } else retired = std::move(replacementPlayback_);
        return SourceReplacementResult{completion, std::move(retired)};
    }
    bool submit(const RenderCore::Command& command) noexcept
    {
        if (!ready()) return false;
        const bool global = command.type == RenderCore::Command::Type::MasterGain ||
                            command.type == RenderCore::Command::Type::Crossfader ||
                            command.type == RenderCore::Command::Type::CueGain;
        if (!global && handoff_.busy() && command.deck == replacementDeck_ &&
            replacementKind_ == SourceHandoff::Kind::Replacement) return false;
        return commands_.tryPush(command);
    }
    State capture() const noexcept
    {
        State state;
        state.outputFrame = renderer_.frameClock();
        for (std::size_t deck = 0; deck < RenderCore::deckCount; ++deck) {
            state.sourceFrame[deck] = renderer_.sourceFrame(deck);
            state.sourcePosition[deck] = renderer_.sourcePosition(deck);
            state.sourceFrames[deck] = renderer_.sourceFrames(deck);
            state.sourceGeneration[deck] = sourceGeneration_[deck];
            state.transportRevision[deck] = renderer_.transportRevision(deck);
            state.cueFrame[deck] = renderer_.cueFrame(deck);
            state.playing[deck] = renderer_.isPlaying(deck);
            state.cueHeld[deck] = renderer_.isCueHeld(deck);
            state.cuePreview[deck] = renderer_.isCuePreviewing(deck);
            state.cueSelected[deck] = renderer_.isCueSelected(deck);
            state.buffering[deck] = renderer_.isBuffering(deck);
            state.starvedFrames[deck] = renderer_.starvedFrames(deck);
            state.sourceReadErrorObserved[deck] = renderer_.readFailed(deck);
            state.scratch[deck] = renderer_.scratchState(deck);
            state.residentHold[deck] = renderer_.residentHoldState(deck);
            state.sourceReadError[deck] = activePlaybacks_[deck]
                ? bool(activePlaybacks_[deck]->errorCode()) ||
                    (state.scratch[deck].active && state.scratch[deck].errorCode != 0)
                : renderer_.readFailed(deck);
            state.deckGain[deck] = renderer_.deckGain(deck);
            for (std::size_t band = 0; band < RenderCore::eqBandCount; ++band)
                state.eqGainDb[deck][band] = renderer_.eqGainDb(deck, band);
            state.stemBank[deck] = renderer_.isStemBank(deck);
            state.loops[deck] = renderer_.loopState(deck);
            state.tempo[deck] = renderer_.tempoState(deck);
            for (std::size_t stem = 0; stem < RenderCore::stemCount; ++stem)
                state.stemGain[deck][stem] = renderer_.stemGain(deck, stem);
        }
        state.masterGain = renderer_.masterGain(); state.cueGain = renderer_.cueGain(); state.meters = meters_;
        state.crossfader = renderer_.crossfader();
        state.applied = applied_; state.rejected = rejected_; state.lastCommandId = lastCommandId_;
        state.callbacks = callbacks_; state.invalidBuffers = invalidBuffers_; state.snapshotDrops = snapshotDrops_;
        const auto& timing = timing_.counters();
        state.maxExecutionNs = timing.maxExecutionNs; state.deadlineMisses = timing.deadlineMisses;
        state.streamFramesMin = streamFramesMax_ ? streamFramesMin_ : 0; state.streamFramesMax = streamFramesMax_;
        state.cueReady = cueGate_.load(std::memory_order_acquire) &&
            cueBuffersReady_.load(std::memory_order_acquire) && ready();
        state.cueRouteStatus = cueStatus_.load(std::memory_order_acquire);
        state.graphRate = graphRate_.load(std::memory_order_relaxed); state.ready = ready();
        return state;
    }
    void accumulateMeters(const RenderCore::Meters& block) noexcept
    {
        auto add = [&](RenderCore::StereoMeter& total, const RenderCore::StereoMeter& part) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                total.peak[channel] = std::max(total.peak[channel], part.peak[channel]);
                total.rms[channel] += part.rms[channel] * part.rms[channel] * block.blockFrames;
                total.overrangeSamples[channel] = part.overrangeSamples[channel];
            }
        };
        for (std::size_t deck = 0; deck < 2; ++deck) add(meters_.deck[deck], block.deck[deck]);
        add(meters_.master, block.master); add(meters_.cue, block.cue);
        meters_.blockFrames += block.blockFrames;
    }
    void finishMeters() noexcept
    {
        if (!meters_.blockFrames) return;
        auto finish = [&](RenderCore::StereoMeter& meter) {
            for (auto& rms : meter.rms) rms = std::sqrt(rms / meters_.blockFrames);
        };
        for (auto& meter : meters_.deck) finish(meter);
        finish(meters_.master); finish(meters_.cue);
    }

    static void process(void* data, spa_io_position* position) noexcept
    {
        auto& self = *static_cast<Impl*>(data);
        const auto began = Clock::now();
        self.tap_.beginBlock();
        SourceHandoff::Request replacement;
        const bool replacementPending = self.handoff_.readPending(replacement);
        bool replacementCompleted = false, replacementInstalled = false;
        SourceHandoff::RetiredState retiredState;
        ++self.callbacks_;
        CallbackTiming::DriverClock clock;
        if (position) {
            clock.id = position->clock.id; clock.position = position->clock.position;
            clock.duration = position->clock.duration; clock.rateNum = position->clock.rate.num;
            clock.rateDenom = position->clock.rate.denom; clock.flags = position->clock.flags;
#ifdef SPA_IO_CLOCK_FLAG_XRUN_RECOVER
            clock.cycle = position->clock.cycle;
#endif
        }
        const bool durationValid = position && clock.duration && clock.duration <= std::numeric_limits<std::uint32_t>::max();
        const auto frames = durationValid ? static_cast<std::uint32_t>(clock.duration) : 0;
        const bool rateValid = position && clock.rateNum &&
            std::uint64_t(clock.rateDenom) == std::uint64_t(clock.rateNum) * RenderCore::sampleRate;
        if (position && clock.rateNum)
            self.graphRate_.store(clock.rateDenom / clock.rateNum, std::memory_order_relaxed);
        if (!rateValid && position) self.graphFault_.store(true, std::memory_order_relaxed);
        if (rateValid) self.graphValid_.store(true, std::memory_order_release);
        std::array<pw_buffer*, 4> buffers{};
        std::array<float*, 4> outputs{};
        std::array<bool, 4> valid{};
        for (std::size_t index = 0; index < 4; ++index) {
            auto* buffer = buffers[index] = pw_filter_dequeue_buffer(self.portData_[index]);
            if (!buffer || !buffer->buffer || buffer->buffer->n_datas != 1) continue;
            auto& plane = buffer->buffer->datas[0];
            if (plane.chunk) { plane.chunk->offset=0; plane.chunk->stride=sizeof(float); plane.chunk->size=0; }
            buffer->size = 0;
            if (frames && plane.data && plane.chunk && frames <= plane.maxsize / sizeof(float) &&
                reinterpret_cast<std::uintptr_t>(plane.data) % alignof(float) == 0) {
                outputs[index] = static_cast<float*>(plane.data); valid[index] = true;
                std::fill_n(outputs[index], frames, 0.F);
            }
        }
        const bool masterValid = valid[0] && valid[1];
        if (masterValid) self.masterBuffersReady_.store(true, std::memory_order_release);
        bool cue = self.cueGate_.load(std::memory_order_acquire);
        if (cue && self.ready() && rateValid && masterValid && (!valid[2] || !valid[3])) {
            // Initial link allocation can precede the first usable cue pair.
            // Only a pair that was actually usable can subsequently be lost.
            if (self.cueBuffersReady_.load(std::memory_order_acquire)) {
                self.cueGate_.store(false, std::memory_order_release);
                self.cueBufferFault_.store(true, std::memory_order_relaxed);
            }
            cue = false;
        }
        if (cue && valid[2] && valid[3]) self.cueBuffersReady_.store(true, std::memory_order_release);
        std::uint64_t rendered = 0;
        if (!durationValid || (self.ready() && !masterValid)) {
            ++self.invalidBuffers_; self.bufferFault_.store(true, std::memory_order_relaxed);
        } else if (self.ready() && rateValid && masterValid) {
            self.consumeCommands(); self.meters_ = {};
            if (replacementPending) {
                if (replacement.kind == SourceHandoff::Kind::Continuation) {
                    // Resolve once, after controls: stale intent/cursor is a rejection,
                    // never an instruction to pause or seek the retained source.
                    replacementCompleted = true;
                    const auto* previous = self.activePlaybacks_[replacement.deck];
                    if (previous && replacement.playback &&
                        self.sourceGeneration_[replacement.deck] == replacement.expectedSourceGeneration &&
                        self.renderer_.canContinueSource(replacement.deck, replacement.source,
                            replacement.frame, replacement.expectedTransportRevision) &&
                        !replacement.playback->errorCode() &&
                        replacement.playback->primedForContinuation(replacement.frame) &&
                        replacement.playback->copyStemMixStateFrom(*previous)) {
                        retiredState = {self.renderer_.sourceFrame(replacement.deck),
                            self.renderer_.starvedFrames(replacement.deck), self.renderer_.readFailed(replacement.deck) ||
                            previous->errorCode()};
                        replacementInstalled = self.renderer_.continueSource(replacement.deck, replacement.source,
                            replacement.frame, replacement.expectedTransportRevision);
                    }
                } else {
                    if (self.stagedGeneration_ != replacement.generation) {
                        if (self.renderer_.sourceFrames(replacement.deck)) {
                            (void)self.renderer_.cueRelease(replacement.deck);
                            (void)self.renderer_.setPlaying(replacement.deck, false);
                        }
                        self.stagedGeneration_ = replacement.generation;
                    }
                    if (self.renderer_.isDeckSilentForReplacement(replacement.deck)) {
                        retiredState = {self.renderer_.sourceFrame(replacement.deck),
                            self.renderer_.starvedFrames(replacement.deck), self.renderer_.readFailed(replacement.deck) ||
                            (self.activePlaybacks_[replacement.deck] && self.activePlaybacks_[replacement.deck]->errorCode())};
                        replacementInstalled = self.renderer_.replaceSource(replacement.deck, replacement.source);
                        replacementCompleted = true;
                    }
                }
                if (replacementInstalled) {
                    self.activePlaybacks_[replacement.deck] = replacement.playback;
                    self.sourceGeneration_[replacement.deck] = replacement.generation;
                }
            }
            for (std::uint32_t offset = 0; offset < frames;) {
                const auto count = std::min<std::uint32_t>(scratchFrames, frames - offset);
                auto master = std::span(self.masterScratch_).first(count * 2U);
                auto cuePcm = std::span(self.cueScratch_).first(count * 2U);
                if (!self.renderer_.renderBuses(master, cuePcm)) {
                    ++self.invalidBuffers_; self.bufferFault_.store(true, std::memory_order_relaxed); break;
                }
                self.accumulateMeters(self.renderer_.meters());
                self.tap_.push(master);
                cue = cue && self.cueGate_.load(std::memory_order_acquire);
                for (std::size_t frame = 0; frame < count; ++frame) {
                    outputs[0][offset + frame] = master[frame * 2];
                    outputs[1][offset + frame] = master[frame * 2 + 1];
                    if (cue) {
                        outputs[2][offset + frame] = cuePcm[frame * 2];
                        outputs[3][offset + frame] = cuePcm[frame * 2 + 1];
                    }
                }
                rendered += count; offset += count;
            }
            self.finishMeters();
            self.streamFramesMin_ = std::min<std::uint64_t>(self.streamFramesMin_, rendered);
            self.streamFramesMax_ = std::max<std::uint64_t>(self.streamFramesMax_, rendered);
        }
        // A control-loop link notification may close the cue gate while this
        // block is rendered. Recheck before handing either cue buffer back.
        if (!self.cueGate_.load(std::memory_order_acquire))
            for (std::size_t index = 2; index < 4; ++index)
                if (valid[index]) std::fill_n(outputs[index], frames, 0.F);
        for (std::size_t index = 0; index < 4; ++index) {
            if (!buffers[index]) continue;
            if (valid[index]) {
                auto& plane = buffers[index]->buffer->datas[0];
                plane.chunk->size = frames * sizeof(float); buffers[index]->size = frames;
            }
            if (pw_filter_queue_buffer(self.portData_[index], buffers[index]) < 0) {
                if (index < 2) { ++self.invalidBuffers_; self.bufferFault_.store(true, std::memory_order_relaxed); }
                else { self.cueGate_.store(false, std::memory_order_release); self.cueBufferFault_.store(true, std::memory_order_relaxed); }
            }
        }
        // The deadline covers initial playback only. A later graph-state
        // transition must not turn an established session into a startup timeout.
        if (rendered && !self.bufferFault_.load(std::memory_order_relaxed))
            self.playbackInitialized_.store(true, std::memory_order_release);
        self.tap_.endBlock();
        if (!self.states_.push(self.capture())) ++self.snapshotDrops_;
        if (replacementCompleted)
            (void)self.handoff_.complete(replacementInstalled, retiredState, self.renderer_.frameClock());
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-began).count());
        self.timing_.observe(elapsed, rendered, position ? &clock : nullptr);
    }

    bool start(std::string& error, const RoutingConfig& config, bool discoveryOnly = false)
    {
        error.clear();
        if (readersStopped_ || startFailed_) { error="A stopped or failed session must be reconstructed before restarting"; return false; }
        if (loop_) {
            if (discoveryOnly != discoveryOnly_) {
                error="Stop the current session before changing discovery/playback mode"; return false;
            }
            error=currentError(); if (error.empty()) return true; failedStart(); return false;
        }
        static std::once_flag initialized;
        std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
        config_=config; discoveryOnly_=discoveryOnly; error_.fill(0); routes_={}; cueError_.clear(); defaultSinkName_.clear();
        shuttingDown_=activated_=routesCreated_=linksConfirmed_=discoveryReady_=cueFailed_=false;
        discoveryRound_=0; discoverySequence_=linksSequence_=cueCleanupSequence_=-1; filterState_=PW_FILTER_STATE_UNCONNECTED;
        ready_.store(false); masterGate_.store(false); cueGate_.store(false); graphValid_.store(false);
        masterBuffersReady_.store(false); cueBuffersReady_.store(false);
        playbackInitialized_.store(false);
        bufferFault_.store(false); graphFault_.store(false); cueBufferFault_.store(false); graphRate_.store(0);
        cueStatus_.store(config.cue ? CueRouteStatus::Pending : CueRouteStatus::Disabled);
        started_=Clock::now();
        loop_=pw_thread_loop_new("conflux-audio", nullptr);
        if (!loop_) setError("Cannot create PipeWire thread loop");
        else {
            filter_=pw_filter_new_simple(pw_thread_loop_get_loop(loop_), "CONFLUX manual audio",
                pw_properties_new(PW_KEY_MEDIA_TYPE,"Audio", PW_KEY_MEDIA_CATEGORY,"Filter", PW_KEY_MEDIA_ROLE,"DSP",
                    PW_KEY_MEDIA_CLASS,"Stream/Output/Audio", PW_KEY_NODE_NAME,"conflux-manual-audio",
                    PW_KEY_NODE_AUTOCONNECT,"false", PW_KEY_NODE_DONT_RECONNECT,"true",
                    PW_KEY_NODE_LATENCY,"256/48000", PW_KEY_NODE_RATE,"1/48000", nullptr), &filterEvents(),this);
            if (!filter_) setError("Cannot create PipeWire output filter");
            else {
                for (std::size_t index=0;index<4;++index) {
                    portData_[index]=pw_filter_add_port(filter_,PW_DIRECTION_OUTPUT,PW_FILTER_PORT_FLAG_MAP_BUFFERS,1,
                        pw_properties_new(PW_KEY_FORMAT_DSP,"32 bit float mono audio",PW_KEY_PORT_NAME,outputNames[index],
                            PW_KEY_AUDIO_CHANNEL,index%2 ? "FR":"FL","port.group",index<2 ? "master":"cue",nullptr),nullptr,0);
                    if (!portData_[index]) { setError("Cannot create four mono output ports"); break; }
                }
                if (!error_[0] && pw_filter_connect(filter_,static_cast<pw_filter_flags>(PW_FILTER_FLAG_RT_PROCESS|PW_FILTER_FLAG_INACTIVE),nullptr,0)<0)
                    setError("Cannot connect the inactive output filter");
                if (!error_[0]) {
                    core_=pw_filter_get_core(filter_);
                    if (!core_) setError("Cannot access the output filter core");
                    else {
                        pw_core_add_listener(core_,&coreHook_,&coreEvents(),this);
                        registry_=pw_core_get_registry(core_,PW_VERSION_REGISTRY,0);
                        if (!registry_) setError("Cannot discover audio playback ports");
                        else {
                            pw_registry_add_listener(registry_,&registryHook_,&registryEvents(),this);
                            discoverySequence_=pw_core_sync(core_,PW_ID_CORE,0);
                            if (discoverySequence_<0) setError("Cannot synchronize audio routing discovery");
                        }
                    }
                }
                if (!error_[0]) {
                    if (pw_thread_loop_start(loop_)<0) setError("Cannot start the audio routing loop");
                    else { loopStarted_=true; return true; }
                }
            }
        }
        error=currentError(); failedStart(); return false;
    }
    std::string currentError()
    {
        if (loopStarted_) pw_thread_loop_lock(loop_);
        try {
            if (graphFault_.load(std::memory_order_relaxed)) setError("Audio graph rate is not 48000 Hz; output muted");
            if (bufferFault_.load(std::memory_order_relaxed)) setError("PipeWire master output buffer validation or queueing failed");
            if (loopStarted_) {
                refreshRouting();
                const bool pending = discoveryOnly_ ? !discoveryReady_ :
                    !playbackInitialized_.load(std::memory_order_acquire);
                if (pending && !error_[0] && Clock::now()-started_>=std::chrono::seconds(5))
                    setError(discoveryOnly_ ? "PipeWire routing discovery did not complete within five seconds" :
                        routes_.masterError.empty() ? "PipeWire output routing did not become ready within five seconds" : routes_.masterError.c_str());
            }
            std::string result=error_.data();
            if (loopStarted_) pw_thread_loop_unlock(loop_);
            return result;
        } catch (...) {
            if (loopStarted_) pw_thread_loop_unlock(loop_);
            throw;
        }
    }
    void failedStart() noexcept
    {
        // A start caller may be the GUI/control owner. Do not run producer stop
        // callbacks or join here; retain whole owners for off-thread retirement.
        quiesce();
        startFailed_ = true;
        stopped_ = capture();
    }
    void quiesce() noexcept
    {
        if (loopStarted_) pw_thread_loop_lock(loop_);
        shuttingDown_=true; ready_.store(false); masterGate_.store(false); cueGate_.store(false);
        if (cueStatus_.load(std::memory_order_relaxed) == CueRouteStatus::Ready)
            cueStatus_.store(CueRouteStatus::Pending, std::memory_order_release);
        if (filter_) pw_filter_set_active(filter_,false);
        for (auto& link:owned_) {
            if (link.proxy) { spa_hook_remove(&link.infoHook); spa_hook_remove(&link.proxyHook); pw_proxy_destroy(reinterpret_cast<pw_proxy*>(link.proxy)); }
            link.proxy=nullptr; link.globalId=invalidId; link.seen=false; link.state=PW_LINK_STATE_INIT;
        }
        if (registry_) {
            spa_hook_remove(&registryHook_);
            for (auto& [id,record]:links_) { (void)id; destroyWatch(*record); } links_.clear();
            for (auto& [id,record]:ports_) { (void)id; destroyWatch(*record); } ports_.clear();
            for (auto& [id,record]:nodes_) { (void)id; destroyWatch(*record); } nodes_.clear();
            if (metadata_) { spa_hook_remove(&metadataHook_); pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata_)); metadata_=nullptr; }
            metadataId_=invalidId; pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_)); registry_=nullptr;
        }
        if (core_) { spa_hook_remove(&coreHook_); core_=nullptr; }
        if (filter_) { pw_filter_disconnect(filter_); pw_filter_destroy(filter_); filter_=nullptr; }
        if (loopStarted_) { pw_thread_loop_unlock(loop_); pw_thread_loop_stop(loop_); loopStarted_=false; }
        if (loop_) { pw_thread_loop_destroy(loop_); loop_=nullptr; }
        tap_.quiesce();
        handoff_.quiesce();
        consumeCommands();
        for (std::size_t deck=0;deck<RenderCore::deckCount;++deck)
            if (renderer_.sourceFrames(deck)) renderer_.cueRelease(deck);
        // Callback ownership is now exclusive, including queued transport/CUE
        // drain. Disarm all retained borrows: active, pending, or an installed
        // completion whose old owner has not yet been collected. Persistent
        // playing flags stay intact for a later explicit whole-owner rebuild.
        auto disarm = [](const auto& playback) noexcept {
            if (!playback) return;
            const auto source = playback->sourceRef();
            if (source.residentHoldControl)
                (void)source.residentHoldControl(source.context, AudioResidentHoldCommand::Cancel, 0, 0);
            if (source.setRenderActive) source.setRenderActive(source.context, false);
        };
        for (const auto& playback : playbacks_) disarm(playback);
        disarm(replacementPlayback_);
        State discarded; while (states_.pop(discarded)) {}
    }
    void stop() noexcept
    {
        if (detached_) return;
        quiesce();
        for (auto& playback:playbacks_) if (playback) { playback->stop(); readersStopped_=true; }
        if (replacementPlayback_) { replacementPlayback_->stop(); readersStopped_=true; }
        stopped_=capture();
    }
    StoppedPlaybacks stopAndTakePlaybacks() noexcept
    {
        if (detached_) return {};
        quiesce();
        stopped_ = capture(); // Every borrowed source/health callback is retained here.
        auto replacement = takeSourceReplacementResult();
        StoppedPlaybacks result{std::move(playbacks_), std::move(replacement)};
        activePlaybacks_.fill(nullptr);
        readersStopped_ = detached_ = true;
        return result;
    }

    PlaybackSources playbacks_;
    std::array<PreparedPlayback*,RenderCore::deckCount> activePlaybacks_{};
    std::array<std::uint64_t,RenderCore::deckCount> sourceGeneration_{};
    SourceHandoff handoff_;
    std::unique_ptr<PreparedPlayback> replacementPlayback_;
    std::size_t replacementDeck_ = 0;
    SourceHandoff::Kind replacementKind_ = SourceHandoff::Kind::Replacement;
    std::uint64_t stagedGeneration_ = 0, lastRequestedGeneration_ = 0;
    RenderCore renderer_;
    RecorderTap tap_;
    CallbackTiming timing_{clockCapabilities()};
    RenderCore::Meters meters_{};
    std::array<float,scratchFrames*2> masterScratch_{},cueScratch_{};
    bool readersStopped_=false, detached_=false, startFailed_=false;
    TransportQueue commands_; StateQueue states_;
    pw_thread_loop* loop_=nullptr; pw_filter* filter_=nullptr; pw_core* core_=nullptr; pw_registry* registry_=nullptr;
    pw_metadata* metadata_=nullptr; std::uint32_t metadataId_=invalidId;
    spa_hook coreHook_{},registryHook_{},metadataHook_{};
    std::map<std::uint32_t,std::unique_ptr<NodeWatch>> nodes_;
    std::map<std::uint32_t,std::unique_ptr<PortWatch>> ports_;
    std::map<std::uint32_t,std::unique_ptr<LinkWatch>> links_;
    std::array<OwnedLink,4> owned_{}; std::array<void*,4> portData_{};
    std::array<std::uint32_t,4> outputs_{invalidId,invalidId,invalidId,invalidId};
    RoutingConfig config_; ResolvedRouting routes_;
    std::string defaultSinkName_,cueError_;
    std::array<char,256> error_{};
    bool loopStarted_=false,shuttingDown_=false,activated_=false,routesCreated_=false,linksConfirmed_=false,discoveryReady_=false,cueFailed_=false,discoveryOnly_=false;
    int discoveryRound_=0,discoverySequence_=-1,linksSequence_=-1,cueCleanupSequence_=-1;
    pw_filter_state filterState_=PW_FILTER_STATE_UNCONNECTED;
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<CueRouteStatus>::is_always_lock_free);
    std::atomic<bool> ready_{false},masterGate_{false},cueGate_{false},graphValid_{false},bufferFault_{false},graphFault_{false},cueBufferFault_{false};
    std::atomic<bool> masterBuffersReady_{false},cueBuffersReady_{false};
    std::atomic<bool> playbackInitialized_{false};
    std::atomic<CueRouteStatus> cueStatus_{CueRouteStatus::Disabled};
    std::atomic<std::uint32_t> graphRate_{0};
    Clock::time_point started_{};
    std::uint64_t applied_=0,rejected_=0,lastCommandId_=0,callbacks_=0,invalidBuffers_=0,snapshotDrops_=0;
    std::uint64_t streamFramesMin_=std::numeric_limits<std::uint64_t>::max(),streamFramesMax_=0;
    State stopped_{};
};

LiveSession::LiveSession(std::span<const float> a,std::span<const float> b):LiveSession(a,b,InitialState{}) {}
LiveSession::LiveSession(std::span<const float> a,std::span<const float> b,const InitialState& initial):impl_(std::make_unique<Impl>(a,b,initial)) {}
LiveSession::LiveSession(PreparedSources sources,const InitialState& initial):impl_(std::make_unique<Impl>(sources,initial)) {}
LiveSession::LiveSession(PlaybackSources sources,const InitialState& initial,std::stop_token stop):impl_(std::make_unique<Impl>(std::move(sources),initial,stop)) {}
LiveSession::~LiveSession()=default;
bool LiveSession::start(std::string& error) { return impl_->start(error,{}); }
bool LiveSession::start(std::string& error,const RoutingConfig& routing) { return impl_->start(error,routing); }
bool LiveSession::startDiscovery(std::string& error) { return impl_->start(error,{},true); }
bool LiveSession::ready()const noexcept { return impl_->ready(); }
std::string LiveSession::error() { return impl_->currentError(); }
bool LiveSession::submit(const RenderCore::Command& command)noexcept { return impl_->submit(command); }
bool LiveSession::requestSourceReplacement(std::size_t deck,std::unique_ptr<PreparedPlayback>& candidate,std::uint64_t generation)noexcept
{ return impl_->requestSourceReplacement(deck,candidate,generation); }
bool LiveSession::requestSourceContinuation(std::size_t deck,std::unique_ptr<PreparedPlayback>& candidate,
    std::uint64_t expectedSourceGeneration,std::uint64_t expectedTransportRevision,std::uint64_t frame,
    std::uint64_t generation)noexcept
{ return impl_->requestSourceContinuation(deck,candidate,expectedSourceGeneration,expectedTransportRevision,frame,generation); }
bool LiveSession::sourceReplacementPending()const noexcept { return impl_->handoff_.busy(); }
std::optional<LiveSession::SourceReplacementResult> LiveSession::takeSourceReplacementResult()noexcept
{ return impl_->takeSourceReplacementResult(); }
bool LiveSession::pollState(State& state)noexcept
{
    if (!impl_->states_.pop(state)) return false;
    state.ready=impl_->ready(); return true;
}
RoutingSnapshot LiveSession::routingSnapshot()const { return impl_->routingSnapshot(); }
CallbackTiming::Summary LiveSession::stoppedTimingSummary()const
{
    if (impl_->loop_) throw std::logic_error("Timing summary requires a stopped audio session");
    return impl_->timing_.summary();
}
bool LiveSession::requestRecorder(StereoRecorder* recorder)noexcept { return !impl_->detached_ && !impl_->startFailed_ && impl_->tap_.request(recorder); }
bool LiveSession::recorderChangePending()const noexcept { return impl_->tap_.changePending(); }
bool LiveSession::recorderAttached()const noexcept { return impl_->tap_.attached(); }
void LiveSession::stop()noexcept { impl_->stop(); }
LiveSession::StoppedPlaybacks LiveSession::stopAndTakePlaybacks()noexcept { return impl_->stopAndTakePlaybacks(); }
LiveSession::State LiveSession::stoppedState()const noexcept { return impl_->stopped_; }

} // namespace conflux::audio

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/AudioRouting.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace conflux::audio;

void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

PortIdentity port(std::uint32_t node, std::uint32_t id, const char* name, const char* channel)
{
    PortIdentity result;
    result.globalId=id; result.nodeId=node;
    result.serial=10000+id; result.nodeSerial=1000+node;
    result.nodeName=name; result.portName=std::string("playback_")+channel; result.channel=channel;
    result.input=result.audioSink=result.monoDsp=true;
    return result;
}
RoutingSnapshot topology()
{
    RoutingSnapshot result;
    result.defaultSinkName="interface";
    result.playbackPorts={port(10,101,"interface","FL"),port(10,102,"interface","FR"),
        port(10,103,"interface","RL"),port(10,104,"interface","RR"),
        port(20,201,"headphones","FL"),port(20,202,"headphones","FR")};
    return result;
}
RoutePair pair(const RoutingSnapshot& snapshot, std::size_t left, std::size_t right)
{ return {snapshot.playbackPorts[left],snapshot.playbackPorts[right]}; }

void validTopologiesAndDefaultPolicy()
{
    const auto snapshot=topology();
    auto result=resolveAudioRouting(snapshot,{});
    require(result.master && result.master->left.globalId==101 && result.master->right.globalId==102,
        "Default metadata did not select its unique FL/FR inputs");
    require(!result.cue && result.cueStatus==CueRouteStatus::Disabled && result.cueError.empty(),
        "Cue was inferred without an explicit complete selection");
    RoutingConfig config;
    config.cue=pair(snapshot,2,3);
    result=resolveAudioRouting(snapshot,config);
    require(result.master && result.cue && result.cue->left.globalId==103 && result.cue->right.globalId==104 &&
            result.cueStatus==CueRouteStatus::Pending,
        "One four-channel sink did not resolve to distinct master/cue pairs");
    config.cue=pair(snapshot,4,5);
    result=resolveAudioRouting(snapshot,config);
    require(result.master && result.cue && result.master->left.nodeId!=result.cue->left.nodeId,
        "Separate stereo sinks were rejected");
    config.master=pair(snapshot,2,3);
    config.cue=pair(snapshot,5,4);
    auto noDefault=snapshot; noDefault.defaultSinkName.clear();
    result=resolveAudioRouting(noDefault,config);
    require(result.master && result.master->left.globalId==103 && result.cue && result.cue->left.globalId==202,
        "Explicit port ordering was replaced by default/channel inference");

    RoutingConfig pinned{pair(snapshot,0,1),pair(snapshot,2,3)};
    auto changedDefault=snapshot; changedDefault.defaultSinkName="headphones";
    result=resolveAudioRouting(changedDefault,pinned);
    require(result.master && result.master->left.globalId==101 && result.cue,
        "Changing default metadata retargeted an explicit pinned pair");
}

void invalidSelectionsFailClosed()
{
    const auto snapshot=topology();
    for (const auto& selected : {pair(snapshot,0,1),pair(snapshot,2,0),pair(snapshot,1,3)}) {
        auto result=resolveAudioRouting(snapshot,RoutingConfig{{},selected});
        require(result.master && !result.cue && result.cueStatus==CueRouteStatus::InvalidSelection && !result.cueError.empty(),
            "Overlapping master/cue ports were allowed or invalid cue removed master");
    }
    for (const auto& selected : {pair(snapshot,2,2),pair(snapshot,2,5)}) {
        const auto result=resolveAudioRouting(snapshot,RoutingConfig{{},selected});
        require(result.master && !result.cue && result.cueStatus==CueRouteStatus::InvalidSelection,
            "A duplicated channel or cross-node stereo pair was accepted");
    }
    auto stale=pair(snapshot,4,5); stale.right.serial++;
    auto result=resolveAudioRouting(snapshot,RoutingConfig{{},stale});
    require(result.master && !result.cue && result.cueStatus==CueRouteStatus::MissingEndpoint,
        "Reused port global ID silently recovered the cue selection");
    stale=pair(snapshot,4,5); stale.left.nodeSerial++;
    result=resolveAudioRouting(snapshot,RoutingConfig{{},stale});
    require(!result.cue && result.cueStatus==CueRouteStatus::MissingEndpoint,
        "Reused node global ID silently recovered the cue selection");
    auto missing=snapshot; missing.playbackPorts.pop_back();
    result=resolveAudioRouting(missing,RoutingConfig{{},pair(snapshot,4,5)});
    require(result.master && !result.cue && result.cueStatus==CueRouteStatus::MissingEndpoint,
        "A missing right cue channel fell back to another destination");
    result=resolveAudioRouting(missing,RoutingConfig{pair(snapshot,4,5),{}});
    require(!result.master && !result.masterError.empty(), "A stale explicit master fell back to the default");

    for (int condition=0;condition<6;++condition) {
        auto invalid=snapshot;
        auto& candidate=invalid.playbackPorts[4];
        switch(condition) {
        case 0:candidate.input=false;break;
        case 1:candidate.monitor=true;break;
        case 2:candidate.audioSink=false;break;
        case 3:candidate.monoDsp=false;break;
        case 4:candidate.serial=0;break;
        case 5:candidate.nodeSerial=0;break;
        }
        result=resolveAudioRouting(invalid,RoutingConfig{{},pair(invalid,4,5)});
        require(result.master && !result.cue && !result.cueError.empty(),
            "An output/monitor/non-sink/non-DSP/unidentified port was accepted");
    }
}

void missingAndAmbiguousMetadata()
{
    auto snapshot=topology();
    snapshot.defaultSinkName.clear();
    snapshot.defaultMaster=pair(snapshot,0,1); // Convenience display data is not authority.
    auto result=resolveAudioRouting(snapshot,{});
    require(!result.master && !result.masterError.empty(), "Missing default metadata silently selected an arbitrary sink");
    snapshot=topology(); snapshot.defaultSinkName="missing";
    result=resolveAudioRouting(snapshot,RoutingConfig{{},pair(snapshot,4,5)});
    require(!result.master && !result.cue && !result.cueError.empty(), "Cue activated without a valid distinct master");
    snapshot=topology(); snapshot.playbackPorts.erase(snapshot.playbackPorts.begin()+1);
    require(!resolveAudioRouting(snapshot,{}).master, "An incomplete default stereo pair was accepted");
    snapshot=topology(); snapshot.playbackPorts.push_back(port(10,105,"interface","FL"));
    require(!resolveAudioRouting(snapshot,{}).master, "An ambiguous default FL pair was guessed");
    snapshot=topology(); snapshot.playbackPorts.push_back(snapshot.playbackPorts[4]);
    result=resolveAudioRouting(snapshot,RoutingConfig{{},pair(snapshot,4,5)});
    require(!result.cue && result.cueStatus==CueRouteStatus::InvalidSelection,
        "Duplicate runtime endpoint identities were accepted");

    snapshot=topology();
    const auto selected=pair(snapshot,4,5);
    snapshot.playbackPorts[4].nodeName="renamed-label";
    snapshot.playbackPorts[4].portName="renamed-port-label";
    result=resolveAudioRouting(snapshot,RoutingConfig{{},selected});
    require(result.cue && result.cue->left.portName=="renamed-port-label",
        "Display-label changes overrode exact runtime identity or retained stale labels");
}
} // namespace

int main()
{
    try {
        validTopologiesAndDefaultPolicy();
        invalidSelectionsFailClosed();
        missingAndAmbiguousMetadata();
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n'; return 1;
    }
}

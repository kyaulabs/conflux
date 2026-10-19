# Proposed M0 decision and M1 entry

Status: proposed; no milestone gate has changed.

Conclude M0 feasibility with the pinned HTDemucs/audio.cpp Vulkan pair selected
for prepared local-file stems. Record on-demand live headroom as failed against
the unchanged 4.095-second service p99 target. Keep CPU execution as the
numerical reference. The user's listening acceptance remains a starting basis
for musical feasibility.

The measured music/audio/UI hour reached 4.266-second continuous service p99,
with 23/1,533 misses. All eleven cancellation/seek checks passed with exact
trimmed output, and all six independent audio fault counters were zero. A later
two-thread paced validation reached 5.172-second p99 with 8/102 misses; all 101
ordinary demands were ready, and its cancellation, output identity and audio/UI
checks passed. These are different workloads and sequential observations, not
a controlled claim of improvement. A new repeat is in progress and must be
reported regardless of its outcome.

The 60-second startup diagnostic did not reproduce the thermal transition:
native launch-to-ready was 0.367 seconds and first-hop service was 1.879 seconds.
No guard warning occurred. Additional warnings while no native worker was
running show that native startup is not necessary for a warning. Their cause
remains unresolved. Thermal protection stays active and unchanged.

This proposal replaces the later M0 rule requiring sustained live headroom
before M1. It follows the original plan's feasibility exit shape, which permits
documenting failed live targets. It does not claim the latency target passed.
Live local-file stems remain a product requirement and an explicit M2 blocker
before a first usable release. No remote offload, production stem playback or
CPU live fallback is approved by this decision.

Begin M1 with one local file decoded outside the audio callback and rendered
through the shared live/offline audio core. Verify stereo samples, partition
independence, seek behavior and EOF silence, then run a short laptop playback
check. Two decks, transport, cue routing, meters and stereo recording remain
M1 work; starting this slice does not mark M1 complete.

If this proposal is declined, retain the existing M0 gate and investigate a
changed runtime, model, chunking or supported workload before entering M1.

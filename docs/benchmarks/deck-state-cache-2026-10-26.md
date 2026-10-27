# Deck-state cache comparison, 26 October 2026

The baseline and cached builds both completed their 120.006-second native
measurements and failed the strict 4,095 ms service target. Baseline p99 was
9,115.679 ms; cached p99 was 9,246.574 ms. Both had steady playback starvation.
The cached run also failed automated process-ownership verification. Independent
host checks closed both scopes without changing those failed verdicts. M2 and
the first release remain blocked.

The fixed order was baseline then cache, using signed sources
`a861c88b26491f8c11068f6c370a19f3d09f42b7` and
`4cc8d743e06412515a2fd765c39e3d8af1558881`. The actual-player binaries used
identical normalized O2 compile commands. Only the deck-state snapshot cache
and its invalidation changed the product runtime. Both used the same visible
UI, normal promoted Vulkan stem worker, two runtime CPU threads, Best
conversion, Club XL on A, a finite quiet peer on B, recording and sixteen
scripted controls. No getter counters, Qt profiler or diagnostic GPU logging
were enabled. The thermal guard and 15 W ceiling remained unchanged.

| Recorded measurement | Baseline | Cached |
|---|---:|---:|
| Complete validated deliveries | 17 | 17 |
| Service p99 / maximum | 9,115.679 ms | 9,246.574 ms |
| Deliveries at or above 4,095 ms | 8 | 9 |
| Steady starvation | 8.741 s | 11.381 s |
| Control-interval starvation | 27.312 s | 26.843 s |
| Peer-deck starvation | 0 frames | 0 frames |
| Incomplete-generation seek recovery | 7.875 s | 7.292 s |
| Three replay recoveries | 4.303 / 7.165 / 8.160 s | 4.451 / 7.666 / 7.619 s |
| Manual controls acknowledged | 16 / 16 | 16 / 16 |
| Sampled GUI-thread CPU / observed wall span | 56.34 / 133.161 s | 30.23 / 135.142 s |
| Healthy visible UI checks | 9,487 | 11,651 |
| Finalized stereo WAV frames at 48 kHz | 6,066,688 | 6,065,664 |
| CPU/ACPI sampled maximum | 90°C | 92°C |
| Automated ownership / cleanup proof | Passed | Failed |

With seventeen deliveries, the nearest-rank p99 is the largest observed value.
All thirteen phase fields were finite for every delivery. Both captures ended
with `ServiceClosed`, zero service-telemetry drops and 1,143 of 1,143 native
observations retained. The worst deliveries spent 8,814.226 and 8,982.818 ms in
native graph computation. Source reading, overlap work and lease validation
were much shorter; the measured GUI CPU reduction did not resolve delivery
latency. One fixed-order pair with changing thermal state cannot establish a
causal cache speedup or regression. A pooled native thread also consumed about
41.9 sampled CPU seconds in each run; its name does not identify the work.

Each run sought away from an accepted generation before raw completion was
observed, then recovered all three replay operations. Recovery times are polling
upper bounds to observed cursor advancement. They do not measure physical
output latency or prove that an executing GPU request was preempted. Later
generation publication and discard checks retained their ordinary validation.

Both WAVs have valid finalized RIFF/data lengths, finite PCM, exact native
frame/peak agreement and zero recorder overflow. Callback execution deadline
misses, invalid buffers, rejected commands and source-read failures were zero.
Observed driver discontinuities and recovery flags were zero. Every measured
UI check was healthy, with no scene error or visibility gap. These results
preserve useful recording and control evidence; the recorded starvation still
fails integrated audio playback.

The guard engaged power saving during both workloads. Baseline retained one
isolated CPU/ACPI sample at or above 85°C; cached retained two isolated samples.
Neither had a sample at or above 95°C. Roughly two-second sampling cannot
establish the duration of a brief peak. Baseline sampled balanced/15 W five
times and low-power/7.5 W sixty-one times; cached sampled them seven and sixty
times. No profile was selected and no cap was raised. Brief temperature spikes
alone are not classified as the failure; latency and starvation failed under
the unchanged guarded power policy.

The cached wrapper lost an already observed short-lived `export.py --describe`
child during its process walk and recorded
`process_or_parent_disappeared_during_walk`. It retained
`ownership_complete=false`, `cleanup_proven=false` and its private cache.
The native report completed and finalized its WAV, so the service population
and audio measurements above remain available. The overall diagnostic did not
complete its ownership gate. Independent host verification found all 49 known
lifetimes and eight groups, the original cgroup, owned PipeWire resources and
temporary idle blocker absent. Baseline closed all 54 known lifetimes through
the ordinary checks. Both closures finished within the original shared
1,200-second window; the last completed at 595.708 seconds. Neither scope was
restarted, and physical closure does not replace automated ownership proof.

An earlier attempt stopped before either player started. A new guard recovery
left only 58.098 seconds before that preparation deadline, short of the full
60-second stable interval. The next packet allowed 300 rather than 270 seconds
of preparation within the unchanged 540-second command and 1,200-second pair
limits. Its 210-second minimum native working reserve, temperature rules,
single cooldown and original clock checks remained enforced. Three late-guard
regressions reproduced the old behavior; all 87 offline checks per arm and four
pair-entry checks passed before fresh operator readiness and dispatch.

The [verification record](data/m2-deck-cache-player-2026-10-26/verification.json)
retains 51 compressed text artifacts, including both original failed results,
complete service observations, native-thread/thermal projections, analysis,
launch clocks, admission regressions and cleanup receipts. Every archive
round-tripped and the decompressed text passed Gitleaks. Audio, models,
executables, caches and unrelated process inventories remain private.
The process-ownership race, sustained current-player latency and remaining
musical gates still require work before M2/M3 completion and release 0.1.0.

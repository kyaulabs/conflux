# Visible-player repeat, 26 October 2026

The completed two-minute Best-quality repeat delivered 21 service hops with
nearest-rank p99 of 3,764.509 ms, below the unchanged 4,095 ms target. Steady
playback had no starvation, all sixteen controls were acknowledged, and every
scheduled seek/replay recovered. Recovery buffering totaled 20.955 seconds.
This short diagnostic does not qualify sustained latency or close M2.

The player used the fixed stereo-meter UI, default renderer, four-row full-F32
Vulkan stem runtime, Best model, two runtime CPU threads and the same quiet
Club XL/peer workload. Getter CPU counters and Qt QML profiling were disabled.
The private runtime still collected coarse GPU timing intervals. Its exact
audio-output prerequisite remained valid; the normal promoted runtime needs
a separate check before the hour-long qualification run. The 15 W ceiling and
thermal guard were unchanged, with no power-profile selection or cap writes.

| Check | Completed repeat |
|---|---:|
| Measured workload | 120.011 s |
| Complete service hops | 21 |
| Median service latency | 2,747.666 ms |
| Nearest-rank p99 / maximum | 3,764.509 ms |
| Hops at or above 4,095 ms | 0 |
| Steady / peer-deck starvation | 0 / 0 frames |
| Seek/replay starvation | 1,005,824 frames / 20.955 s |
| Initial seek recovery, polling upper bound | 8.268 s |
| Four replay recovery bounds | 3.298 / 3.200 / 3.148 / 3.203 s |
| Manual controls acknowledged | 16 / 16 |
| Healthy visible UI checks | 12,362 / 12,362 |
| Recorded stereo frames at 48 kHz | 6,138,624 |
| Recorder overflow | 0 frames |
| Callback deadline misses / invalid buffers | 0 / 0 |
| Rejected commands / source-read failures | 0 / 0 |
| Driver recovery flags / position discontinuities | 0 / 0 |
| Highest sampled workload CPU/ACPI temperature | 67°C |

All thirteen service-phase fields were finite and present for every hop.
The final `ServiceClosed` record, producer counters and finite diagnostic tail
closed without observation loss. With 21 observations, nearest-rank p99 equals
the maximum. Seeking invalidated an incomplete generation; an executing GPU
request may finish before its result is discarded. The recovery bounds do
not establish hard GPU preemption or acceptable seek latency. No steady
starvation does not remove the reported recovery holds.

An independent WAV scan found all 12,277,248 samples finite. Frame counts,
nonzero counts and channel peaks matched the native summary, with no overflow.
This establishes recording integrity within the test, without a new listening
verdict or measurement of physical-output latency. All seventy two-second
workload host samples were below 85°C; the sampled maximum does not exclude
shorter peaks. Only balanced mode was observed during the workload.

The first attempt stopped after 20.850 measured seconds when CONFLUX became
inactive and unexposed. The user subsequently confirmed switching, covering
or minimizing it. Its original failure remains recorded. Five delivered hops
had a maximum of 3,765.559 ms, but no p99 is assigned to the incomplete requested
120-second workload. Six of sixteen controls were acknowledged; replays had
not run. The partial recording was finite, and the sampled workload peak was
76°C. The completed repeat used fresh readiness and a separate output directory;
the visibility rule was unchanged.

Preparation reused 71 passing offline checks and verified all file and dry
entry bindings. Automated cleanup finished at 200.107 and 292.145 seconds into
the two separate original 600-second readiness windows. Root verification
finished at 463.438 and 321.132 seconds respectively, confirming known process
lifetimes, owned groups, original unit cgroups and scoped KDE idle blockers
absent. Guard identity and files were unchanged and the frozen cap ceilings
were respected. Two failed root-verification attempts on the interrupted run
used a completion-only field and compared natural recovery to 15 W against
the initial guard-reduced 7.5 W snapshot. The corrected verification used
the public input-integrity proof and the frozen 15 W ceiling. Original failed
receipts were retained; neither the runtime nor thermal rules changed.

The [verification record](data/m2-player-repeat-2026-10-26/verification.json)
binds both attempts, the user's interruption confirmation, original failures,
plans, observations, analysis and cleanup proofs to compressed evidence and
raw hashes. Audio, models, executables and caches remain private. The cooler
completed run does not establish a causal speedup from removing getter
counters. Normal-runtime confirmation, sustained testing, recovery latency
and the remaining musical-analysis gates stay open.

# Live-stem tempo listening and short-player results

The primary laptop completed two 120-second Best-quality player runs at
**3.088/3.200-second service p99**, with Master Tempo off/on. Both stayed below
the unchanged **4.095-second target**, with zero steady starvation. Apollo's
matching private-virtual-audio runs reached 0.918/0.905 seconds. These short
checks do not complete the current-build hour, cancellation or release gates.

All four runs used source `9e35bda2eeb75c54630b392a9b631d3fca212684` on
30 October 2026 Pacific time; their UTC records fall on 31 October. Each host
ran Master Tempo off, then on, while the hosts ran concurrently. The fixed
106 BPM setting used an explicit 100 BPM reference to exercise +6% playback;
it was not a BPM-analysis accuracy test. The workload played Club XL with a
peer, recording, seeks/replays and 16 automated UI control changes.

| Observation | Laptop, MT off | Laptop, MT on | Apollo, MT off | Apollo, MT on |
| --- | ---: | ---: | ---: | ---: |
| Closed measured interval | 120.012 s | 120.011 s | 120.005 s | 120.006 s |
| Completed service deliveries | 21 | 21 | 23 | 23 |
| Nearest-rank p99 and maximum | 3.088341 s | 3.199605 s | 0.917944 s | 0.904574 s |
| Deliveries at or above 4.095 s | 0 | 0 | 0 | 0 |
| Steady starvation | 0 s | 0 s | 0 s | 0 s |
| Control buffering | 22.827 s | 24.677 s | 14.795 s | 15.621 s |
| Replays / control changes acknowledged | 4 / 16 | 4 / 16 | 4 / 16 | 4 / 16 |
| Cold load to first observed advance, polling upper bound | 6.746 s | 6.050 s | 10.797 s | 10.747 s |
| Maximum sampled laptop temperature | 72°C | 76°C | Unmeasured | Unmeasured |

The small delivery populations make each p99 equal its maximum. Startup and
seek/replay buffering remain reported; passing steady playback does not resolve
those delays. The five measured control transitions include initial selected-
tempo startup. No measured startup interval was discarded. The 16 UI actions
changed channel levels, HI EQ, headphone CUE selection and the crossfader;
they were not 16 tempo changes or physical pointer/touch trials.

All four native functional checks and source validations passed. Callback
execution deadline misses, invalid buffers, source-read failures, recording
overflow and scene errors were zero. Each recording finalized. Laptop UI
snapshots recorded no missing, stale or unexposed window observations during
the measured intervals. Requested basic render-loop selection lacked confirming
output in the captured native streams; the record does not claim that backend
was independently established. Apollo used an offscreen window and private
virtual audio, so it supplies no physical-device or laptop qualification.

The laptop retained Balanced mode, the thermal guard and a 15 W long-term
allowance. The approved temporary short-term cap was also 15 W, and OneDrive
was paused through native cleanup. The cap returned to its prior 64 W allowance
and OneDrive was restored afterward. There were 67/68 two-second temperature
samples, with none at or above 85°C or 95°C. Those samples cannot exclude
shorter spikes. This does not establish performance with the normal burst
allowance, active sync or an empty driver cache.

Seeking exercised logical invalidation of incomplete generations. Its
accepted/RawComplete timestamps do not establish when native inference started.
Hard worker termination and shutdown during unfinished GPU work were not
exercised by these runs. Earlier unsuccessful attempts remain failures; these
short results do not replace them or the separate sustained requirement.

The musical verdicts have narrower, separately pinned scopes. In source
`e86585b537175ea5b5570c6c05565f1f103db050`, the user accepted Club XL live-stem
BPM changes, Master Tempo off/on and Reset: “Acceptable—finished and client
closed.” In offline renders from
`212b892e46e90ce118fc740b2d4ba32a2af785e7`, the user accepted recovery and
prepared-return joins for Club XL and When the Light Breaks at faster/slower
BPM with Master Tempo off/on: “Recovery and return sound acceptable.” Those
renders wait for PCM and precede the startup-prefill change. They do not prove
current-player real-time recovery continuity or sustained performance.

[Generated recovery](live-tempo-recovery-2026-10-30.md) and
[prepared-continuation checks](tempo-continuation-2026-10-30.md) retain the
selected BPM and Master Tempo through approved recovery and return. Tempo
editing while recovery is active remains unavailable in both UI and engine;
leave recovery first. A prepared takeover uses the same four-stem bank and
preserves the fractional nominal cursor and controls. Fresh DSP starts at the
containing PCM frame; it does not transfer the old processor's filter history
or sub-frame audio phase. Manual transport still revokes recovery consent and
pending continuation. The short-player workload did not force this recovery
handoff.

The [summary](data/live-tempo-player-2026-10-30/summary.json), separate
[baseline listening](data/live-tempo-player-2026-10-30/baseline-listening.json)
and [recovery listening](data/live-tempo-player-2026-10-30/recovery-listening.json)
records retain those results. The
[manifest](data/live-tempo-player-2026-10-30/manifest.json) binds original text
receipts and compressed native results to byte counts and SHA-256 hashes.
No audio, executable, model or mutable cache is included. The earlier
[fixed-mix hour](current-player-hour-2026-10-29.md) remains valid only for its
own source and conditions. M2, M3 and `0.1.0` remain open.

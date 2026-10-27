# Best-quality player checks, 26 October 2026

M2 remains open. The completed player checks missed the unchanged **4.095-second service p99 target** and recorded substantial stem starvation. These results do not qualify a release or a sustained run.

All three checks used the signed clock-fix candidate `b512dd668973cb708121a70c7de9f34e5326f620`, the same optimized player, Best-quality Vulkan runtime and model, visible UI, quiet Club XL excerpt, finite peer, recording and sixteen automated mixer controls. The thermal guard and 15 W ceiling stayed unchanged. Diagnostic admission allowed the guard's natural balanced or power-saving state while retaining the full 60 seconds at or below 55°C and fresh checks at or below 70°C.

| Check | Measured duration | Completed hops observed | Observed service p99 | Steady stem starvation | Control ACKs |
|---|---:|---:|---:|---:|---:|
| Empty private Mesa cache | 120.012 s | 13 | 13.142 s | 16.011 s | 16/16 |
| Retained private Mesa cache | 120.010 s | 12 | 10.661 s | 19.835 s | 16/16 |
| Standalone two-core client | 120.008 s | 14 | 9.823 s | 9.856 s | 16/16 |

The retained-cache run failed its final drain with 4,352 additional starved frames and no final `ServiceClosed` event. Its p99 describes the twelve observed completed hops, not a successfully closed service population. The empty-cache and two-core populations closed with zero reported service-event loss; the two-core population includes one hop completed during the final drain. With these small populations, nearest-rank p99 is the maximum. All original failed verdicts remain retained.

The cache pair used one private directory with the same `MESA_SHADER_CACHE_MAX_SIZE=128M`. Its approximately 2.4 MB of retained contents matched exactly before the second dispatch. The later all-CPU affinity control expired in preflight, so the two-core run is standalone evidence. Its service owner was observed on CPUs 0 and 1 at launch; attempts to observe native thread masks occurred after the native processes had exited. Neither cache reuse nor affinity has a demonstrated causal improvement from these runs.

Power state also differed. Read-only workload snapshots observed the 7.5 W / 15 W MMIO long-term limits 51/24 times for empty cache, 70/0 for retained cache, and 69/6 for the two-core case. These counts include setup and drain; they are not duration fractions. Sampled workload peaks were 85°C, 67°C and 84°C respectively. The 85°C observation was isolated, and no workload sample reached 95°C. Brief temperature peaks are not the reason these checks fail.

All issued seeks and replays in the three full checks recovered. The incomplete-generation seek's new cursor was observed after 21.289, 22.560 and 28.378 seconds respectively. Replay recovery observations were 4.504/13.499, 11.857/15.010 and 11.957/14.583 seconds. These are polling observations of resumed consumption, not measurements of hard GPU preemption or physical output latency.

All four generated WAVs, including the earlier partial attempt, have finalized headers and finite stereo float samples at 48 kHz. Final native recording metadata confirms zero overflow for the empty-cache and two-core runs. That metadata is missing from the failed retained-cache report, so its valid WAV header is not a recording-health pass. All three full runs reported zero callback deadline misses, invalid buffers, rejected commands and source-read failures. The two-core case observed two driver-position discontinuities; the cache pair observed none. The finite peer did not starve. Finite recordings and clean callback counters do not undo the measured stem gaps.

An earlier attempt stopped at 78.024 seconds when starvation overlapped a nontransport mixer control. The old assertion expected every output frame to consume source audio. [PR #113](https://github.com/kyaulabs/conflux/pull/113) subtracts the recorded starvation when checking that clock relationship, rejects invalid counters and source jumps, and retains the final starvation and latency gates. Its regression failed against the previous predicate and passed after correction. That partial attempt remains separate from the completed checks.

The empty-cache run also retained an automated ownership-observation failure. Independent host checks verified closure of every client scope, its original cgroup, audio resources and per-run idle inhibitors. The later runs' automated cleanup passed. This independent closure does not rewrite the earlier ownership verdict.

The user's test authorization ended at 20:00 on the laptop's Pacific clock. No client remained running. The temporary screen, lock and sleep inhibitor released automatically at the cutoff, and normal idle settings resumed. Best quality, the latency target and the M2/release gates remain unchanged.

Machine-readable results, original failures and checksum-verified compressed evidence are listed in [verification.json](data/m2-store-window-player-2026-10-26/verification.json). Audio, models, binaries and unrelated process inventories are excluded.

# Best-quality current-player hour results

The primary laptop completed the full hour at **2.729-second service p99**, passing the unchanged **4.095-second target** across 537 deliveries. Apollo completed its private-virtual-audio hour at **0.994 seconds**. The laptop result qualifies this fixed-mix hour under the tested configuration. M2, M3 and the first `0.1.0` release still have open requirements.

| Observation | Laptop, physical audio and visible UI | Apollo, private virtual audio |
| --- | ---: | ---: |
| Closed measured interval | 3,600.011 s | 3,600.005 s |
| Completed service deliveries | 537 | 555 |
| Nearest-rank p99 | **2.728795 s** | **0.994327 s** |
| Maximum service delivery | 2.833984 s | 1.026122 s |
| Deliveries at or above 4.095 s | 0 | 0 |
| Steady stem starvation | 0 s | 0 s |
| Seek/replay buffering | 478.251 s | 386.421 s |
| EOF/replay cycles | 133 | 137 |
| Peer rewinds | 8 | 8 |
| Callback deadline misses | 0 | 0 |
| Driver discontinuities / recovery flags | 0 / 0 | 0 / 0 |
| Recording overflow frames | 0 | 0 |
| Recording frames accepted and written | 173,079,296 | 173,330,432 |

Both hosts used Best quality and frozen current-player source `6e1623ff0261d3cc5200380cd8f1b15260bc57f4`. The workload repeats Club XL with a quiet peer, seeks/replays, recording and visible 16 ms UI load. It does not exercise every M3 control. Apollo completed its original hour while laptop retries continued, so the successful hours have different start times. These observations do not establish a counterbalanced causal comparison.

The laptop's p99 and maximum include every completed hop, including the final hop in the bounded finite-generation drain. No late delivery was censored. Its 19.853-second diagnostic tail completed without additional starvation or callback misses; the measured boundary and finalized WAV frame count stayed fixed. All 38,063 observation records were retained, with no invalid clock reads. Service and cursor timings do not measure physical acoustic latency.

All 1,761 laptop host samples retained Balanced mode and the approved **15 W MMIO long/short allowances**. The sampled maximum was **77°C**, with no sample at or above 85°C or 95°C. Two-second samples cannot exclude shorter spikes. The existing thermal guard stayed active and unchanged. Qualification depends on the temporary 15 W short-term cap and copied verified shader-cache setup. It does not establish the same result with the normal 64 W burst allowance, an empty cache or another workload. No permanent host policy was installed.

Both hosts exercised logical invalidation of an accepted generation without an observed RawComplete and discarded one stale packet each. Neither measured a native inference-start instant or hard GPU-worker termination. Seek/replay buffering remains an unresolved usability cost, separate from steady starvation. Load to first observed cursor advance was 5.820 seconds on the laptop and 11.010 seconds on Apollo.

Invalid buffers, rejected commands, source-read failures and scene errors were zero. The finalized stereo 48 kHz float32 WAV headers, payload lengths and frame counts match the native whole-recording scans. No new listening verdict was obtained. The laptop's basic render loop was independently confirmed from the exact native process's original journal; the original empty-stream assessment remains in the evidence.

The short-term allowance returned to its original **64 W before OneDrive resumed**. Independent identity checks confirmed the pinned OneDrive executable running, the same thermal-guard process active, the root helper gone, exact completed test units and their original cgroups gone, and KDE's inhibition list empty. Apollo's owned private container is absent. Its secondary virtual-audio result does not replace laptop or physical-audio evidence.

Earlier laptop attempts remain retained. Two administrator-authentication timeouts stopped before playback and cap installation. The next run stopped after 2,222.237 seconds when the user switched desktops; its partial 2.772-second p99 is not a completed hour. Failed offline setup checks and the copied-log admission failure remain separate from later passing checks. A later retirement attempt exposed a cleanup observer that could skip already-restored terminal proof after its deadline. The private launcher now observes that proof once without granting an unfinished owner more time. Three of four regression cases reproduce the prior failure; all four and the existing 92 checks pass, as does the real unarmed desktop entry. This changes no product audio or host power policy.

The [summary](data/current-player-hour-2026-10-29/summary.json) records both complete hours. The [evidence manifest](data/current-player-hour-2026-10-29/manifest.json) maps deterministic gzip members to their original text paths, byte counts and SHA-256 hashes. It includes raw observations, native results, launch/source bindings, journals, rollback proofs, tests and retained unsuccessful attempts. Audio, binaries, models and mutable caches stay outside Git. Source bodies omitted after decoded secret scanning retain immutable pins and the original test outcomes.

The earlier [failed hour](onedrive-player-hour-2026-10-28.md), [short burst-cap failure](short-burst-cap-player-2026-10-29.md) and [corrected cache check](corrected-cache-player-2026-10-29.md) remain recorded. This completed hour closes only the fixed-mix sustained requirement for its explicit conditions. The later [native-worker cancellation check](native-worker-cancel-2026-10-29.md) verified forced cleanup after input consumption, and the user accepted the [combined prepared-loop/Master Tempo listening check](data/m3-loop-tempo-2026-10-29/acceptance.json). Remaining cancellation coverage, musical-analysis/fallback and live-stem tempo work remain open. No milestone was marked complete and no release was published.

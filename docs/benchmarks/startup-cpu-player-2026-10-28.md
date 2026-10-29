# Player CPU/thread diagnostic — 28 October 2026

Both two-minute traces completed and cleaned up. The laptop recorded a 2.964-second nearest-rank p99; Apollo recorded 0.920 seconds. These are short, instrumented diagnostics. M2 remains open, and this result does not qualify latency or authorize the 0.1.0 release.

The player source was `dfff55ea78b6fcbe633a178e996371238803013d`. Both hosts used the existing Best-quality runtime, two decks, sixteen automated control changes, seeks, replay and recording. The laptop retained its installed three-sample thermal rule, immediate 95°C response and normal 15 W ceiling. No power profile was selected. Read-only process/task counters were added; their overhead remains in the measurements. The frozen packet passed 42 offline checks and the real desktop entry check before launch.

| Result | Laptop | Apollo, virtual audio |
|---|---:|---:|
| Measured workload | 120.005 s | 120.005 s |
| Complete service hops | 21 | 22 |
| p99 / maximum | 2.964 s | 0.920 s |
| Hops at or above 4.095 s | 0 | 0 |
| Steady starvation | 0 frames | 0 frames |
| Seek/replay starvation | 786,688 frames / 16.389 s | 683,776 frames / 14.245 s |
| Prepared peer and final tail starvation | 0 frames | 0 frames |
| Automated controls acknowledged | 16/16 | 16/16 |
| Replays completed | 4 | 4 |
| Stale packets discarded | 1 | 1 |
| Recorded frames accepted and written | 6,020,864 | 6,268,928 |
| Recording overflow | 0 frames | 0 frames |
| Callback execution deadline misses | 0 | 0 |
| Driver discontinuities / recovery flags | 0 / 0 | 0 / 0 |
| Invalid buffers / rejected commands / source-read failures | 0 / 0 / 0 | 0 / 0 / 0 |
| Maximum sampled GUI heartbeat gap | 62.735 ms | 62.754 ms |

Each incomplete-generation seek was issued after acceptance and before that generation completed. Playback recovered after the seek and all replays, with final recovery complete. This verifies logical invalidation and stale-result rejection; hard worker cancellation was not exercised. Seek/replay starvation remains recorded separately from steady playback. No sound-quality listening verdict was collected during these automated tests.

The laptop reached a sampled maximum of 96°C. Of 67 host samples, 58 showed Balanced with the 15 W MMIO long-term limit and nine showed Power Saver with the guard's 7.5 W reduction. All 21 complete hops preceded the first observed reduction at 00:13:31.877 UTC on 29 October; the last hop preceded that sample by 1.531 seconds. The journal records the immediate critical response at 00:13:30 and recovery at 00:15:37. Point samples do not establish continuous exposure. The passing hop population therefore provides no sustained evidence under the reduced limit. The previous uninstrumented guard-only run's 8.503-second p99 and steady starvation remain valid failed results.

Across the laptop's sampled process lifetimes, the native player accumulated 72.54 CPU seconds over 135.357 seconds. Its largest sampled task totals were a pooled thread (22.05 CPU seconds), the main thread (19.22) and `QSGRenderThread` (14.62). The stem runtime accumulated 3.25 CPU seconds over its shorter 129.192-second observed span. These are counter deltas between each task's first and last retained sample. Their boundaries differ, short-lived work may be missed, and thread names do not identify functions or explain GPU waits.

All 67 laptop and 71 Apollo native task captures matched the expected executable and were complete. The laptop process scan retained five `FileNotFoundError` reads as processes exited; the native task captures had no read errors. The scan's retained CPU cost totaled 1,464.966 ms on the laptop and 617.495 ms on Apollo, excluding filtering and log-writing costs. These counters do not make the observer free.

A separate passive trace after CONFLUX had closed sampled the existing system 117 times from 00:23:35.405 to 00:25:34.715 UTC. At the two hottest samples, 88°C and 89°C, the existing OneDrive process used approximately one CPU core; other retained process rates were much lower. That observation identifies competing background work to isolate. It does not prove OneDrive caused the earlier 96°C event or previous stalls. Three passive process-stat read errors are retained.

The next proposed comparison repeats the uninstrumented player workload with only the verified laptop OneDrive user service temporarily paused. Apollo remains the virtual-audio control. The pause/restore helper passed 37 offline checks and an unarmed desktop entry check; OneDrive remains active and unchanged. Explicit approval and fresh physical conditions are pending. The 60-minute qualification, hard-cancellation evidence and remaining M2 musical-analysis gates are still outstanding.

The [evidence manifest](data/startup-cpu-player-2026-10-28/manifest.json) binds the full text captures, observer source and checks, raw counters, independent arithmetic, guard journal and exact cleanup. Audio, models, binaries and driver caches are excluded. All service hops remain in the population; with 21 and 22 observations, nearest-rank p99 equals the maximum.

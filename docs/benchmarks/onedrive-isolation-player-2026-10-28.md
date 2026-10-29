# OneDrive-isolated player check — 28 October 2026

Both synchronized two-minute captures completed. The laptop reached a 3.070-second nearest-rank service p99, below the unchanged 4.095-second target, with no steady starvation. Apollo's private virtual-audio control reached 0.915 seconds. This short result supports preparing the current-player hour test; it does not complete M2, M3 or release qualification.

The player source was `dfff55ea78b6fcbe633a178e996371238803013d`. Both hosts retained Best quality, the same runtime, inputs, UI and driver-cache seeds as the preceding uninstrumented guard-only test. Only the verified laptop OneDrive user service was temporarily paused, with explicit approval. The normal 15 W ceiling, installed three-sample warning rule, immediate 95°C protection and cooling recovery remained unchanged. No power profile was selected, and no CPU observer was added. The frozen packet passed 37 offline checks and an unarmed desktop entry check before launch.

| Result | Laptop | Apollo, virtual audio |
|---|---:|---:|
| Measured workload | 120.007 s | 120.018 s |
| Complete service hops | 21 | 22 |
| p99 / maximum | 3.070 s | 0.915 s |
| Hops at or above 4.095 s | 0 | 0 |
| Steady starvation | 0 frames | 0 frames |
| Seek/replay starvation | 798,208 frames / 16.629 s | 684,288 frames / 14.256 s |
| Prepared peer and final tail starvation | 0 frames | 0 frames |
| Automated controls acknowledged | 16/16 | 16/16 |
| Replays completed | 4 | 4 |
| Stale packets discarded | 1 | 1 |
| Recorded frames accepted and written | 6,030,080 | 6,268,672 |
| Recording overflow | 0 frames | 0 frames |
| Callback execution deadline misses | 0 | 0 |
| Driver discontinuities / recovery flags | 0 / 0 | 0 / 0 |
| Invalid buffers / rejected commands / source-read failures | 0 / 0 / 0 | 0 / 0 / 0 |
| Maximum sampled GUI heartbeat gap | 86.682 ms | 68.785 ms |

Each unfinished-generation seek occurred after acceptance and before that generation completed. Both hosts recovered after the seek and all four replays, with final recovery complete. This verifies logical invalidation and stale-result rejection; hard worker cancellation was not exercised. Seek/replay buffering remains separate from steady playback. Initial load to the first observed cursor advance was at most 5.606 seconds on the laptop and 10.546 seconds on Apollo, bounded by polling. These automated checks add no listening verdict.

All 67 laptop host samples showed Balanced mode and the 15 W MMIO long-term allowance. The sampled maximum was 94°C, with three isolated readings at or above 85°C and none at or above 95°C. The largest sampling gap was 2.103 seconds. These point samples do not establish continuous temperature exposure. Native cleanup finished at 01:39:28 UTC on 29 October. The guard journal subsequently records a 93°C warning at 01:39:53 and recovery at 01:41:58, outside the native capture. Later heat is not part of the passing hop population and has no established cause here.

The retained guard-only result was 8.503-second p99 with steady starvation. This later passing run isolates a plausible competing load, but its sequential order and short duration do not establish that OneDrive caused every earlier stall or thermal event. Neither the earlier failed result nor the instrumented CPU trace is reclassified.

The OneDrive pause remained intact through native cleanup. Automatic restoration then encountered a helper defect: the unit's existing `ExecStartPre` sleeps for 15 seconds, while the helper also allowed only 15 seconds for `systemctl start`. The helper timed out while the existing start job completed independently. OneDrive was verified active and running, and the restoration token was confirmed without issuing another start. The original traceback and incomplete token are retained alongside that confirmation. KDE's inhibitor wrapper returned zero despite the child exception; its exit status is not evidence that restoration succeeded. A longer bounded start wait and sufficient shutdown allowance are required before another test.

Both exact benchmark units, owned player/service processes and routing resources were retired. OneDrive is running again, and the temporary dimming, locking and sleep blockers are released. The original twenty-minute testing window is closed. Another run requires fresh physical readiness and approval for its OneDrive pause.

The current-player 60-minute qualification and remaining M2 musical-analysis requirements remain open. No product, model or runtime was promoted, and no release was created.

The [evidence manifest](data/onedrive-isolation-player-2026-10-28/manifest.json) binds complete text captures, source/build/owner pins, the approved pause, raw native results, independent arithmetic and restoration evidence. Audio, models, binaries and driver caches are excluded. All 21/22 complete service hops remain in the population; nearest-rank p99 equals the maximum at these sample sizes.

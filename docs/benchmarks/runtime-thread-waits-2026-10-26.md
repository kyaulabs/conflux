# Player and runtime thread waits, 26 October 2026

M2 remains open. The authorized 120.016-second visible-player trace measured **8.810-second service p99**, above the unchanged **4.095-second target**, and recorded steady stem starvation. The runtime's main thread was sleeping in a DRM synchronization wait at every snapshot inside the worst hop. This narrows the investigation to graphics completion; it does not identify the cause of that wait.

| Measure | Result |
|---|---:|
| Completed service hops | 17 |
| Service p99 / maximum | 8.810 s |
| Hops at or above 4.095 s | 9 |
| Steady stem starvation | 10.069 s |
| Starvation during seek/replay intervals | 21.979 s |
| Mixer controls acknowledged | 16/16 |

The check ran at 22:17 Pacific on 26 October, or 05:17 UTC on 27 October. Signed source `303cf6209c3573ff74cc5fad2c5852ea85246e2b` reused the verified player binary from the [previous check](frame-count-player-2026-10-26.md). Actual PCM stayed at Best quality with the normal two-thread Vulkan stem runtime. The default UI renderer, retained driver cache, 15 W ceiling and thermal guard were unchanged. No power profile, cap or kernel setting was written.

All seventeen completed hops retained thirteen finite phase fields. The final `ServiceClosed` event reported no dropped telemetry. The separate EOF drain reached `ReadyForStop` in 695 ms without either deck starving. The original native and wrapper failures remain retained; complete evidence and clean shutdown do not constitute a playback pass.

The worst hop took 8,810.374 ms. Graph compute accounted for 8,570.459 ms, source reading 4.845 ms and overlap handling 37.394 ms. Native inference used 231.907 ms of process CPU within 8,716.326 ms of wall time. Four runtime-main-thread snapshots fell entirely inside that hop's reconstructed envelope; all four recorded sleeping state `S` and `drm_syncobj_array_wait_timeout.constprop.0`.

Across 61 runtime-main-thread snapshots, 43 recorded that DRM wait, 17 recorded `anon_pipe_read`, and one recorded running state with an unavailable wait symbol. Linux documents sync-object waits as [host-side waits on graphics fences](https://docs.kernel.org/gpu/drm-mm.html#host-side-wait-on-syncobjs). The observed DRM symbol supports a graphics-synchronization finding. It does not distinguish slow kernels, dependency or queue waits, power limits, or driver behavior. Pipe-read snapshots can occur between requests; they are not evidence that a slow graph was waiting for input.

The observer collected 66 host snapshots, with intervals of 1.987–2.172 seconds, and matched the executable, user, PID and birth time of both processes. Every matched player and runtime task capture completed. Unknown symbols and changes between the before/after task states remain explicit. These point counts are not time fractions. Hop envelopes use worker-ready time minus service duration and include source reading, the request and overlap work; individual GPU compute boundaries were not traced. Detailed scheduler statistics were disabled, so exact CPU runqueue wait was not measured. The observer itself used 4.745 seconds of scan-thread CPU; this instrumented run does not qualify latency or establish improvement over the previous check.

The incomplete-generation seek recovered consumption after 6.546 seconds. Three replays recovered after 2.801, 6.499 and 6.358 seconds. These polling observations verify resumed consumption after generation changes, not hard GPU preemption or physical output latency. Whole-test user cancellation was not invoked.

Recording finalized with 6,218,752 stereo frames at 48 kHz, accepted and written frame counts equal, and zero overflow. A bounded independent scan found all samples finite. Callback deadline misses, source-read failures, invalid buffers, rejected commands, driver-position discontinuities and recovery flags were zero. The prepared peer did not starve. All 11,088 visible-UI checks were healthy. Those results do not erase the measured stem gaps.

The sampled temperature peak was 91°C in one observation, with no consecutive sampled span at or above 85°C and none at 95°C. Brief temperature spikes are not the failed gate. The guard naturally changed profiles during the run. Read-only workload cap snapshots observed 7.5 W 65 times and 15 W three times, including setup and drain; these are not duration fractions.

Independent verification closed all 52 observed process lifetimes, eight process groups, the original unit cgroup, audio resources and per-run idle inhibitors in 244.298 seconds of the original 1,200-second envelope. Normal dimming, locking and sleep behavior resumed. The user's reply authorized this single trace; it does not authorize another client test. Best quality, the latency target, musical gates and required completed hour remain unchanged. M2 and the first release remain blocked.

The [evidence manifest](data/m2-runtime-thread-waits-2026-10-26/verification.json) retains checksum-verified compressed text, original failures, phase records, observer source and launcher checks. Host snapshots retain only the relevant player/runtime task evidence; unrelated process inventories are omitted. Audio, models, binaries and driver caches are excluded.

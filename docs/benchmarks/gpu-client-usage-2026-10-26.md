# Player GPU usage and clocks, 26 October 2026

M2 remains open. The 120.009-second visible-player trace measured **8.889-second service p99** against the unchanged **4.095-second target**, with **7.147 seconds of steady stem starvation**. All five GPU snapshots inside the worst hop reported PL1 limiting and actual frequencies of 350–450 MHz. These observations narrow the investigation without qualifying latency or accounting for every part of the delay.

| Measure | Result |
|---|---:|
| Completed service hops | 17 |
| Service p99 / maximum | 8.889 s |
| Hops at or above 4.095 s | 11 |
| Steady stem starvation | 7.147 s |
| Starvation during seek/replay intervals | 29.131 s |
| Mixer controls acknowledged | 16/16 |
| Recorder overflow / callback deadline misses | 0 / 0 |
| Driver-position discontinuities | 1 |

Playback began at 23:30 Pacific on 26 October, or 06:30 UTC on 27 October. Verified source `bb5db7af5490035778f8ba1772e42dbbed0917bd` reused the same optimized player binary as the [preceding wait trace](runtime-thread-waits-2026-10-26.md). Actual PCM retained Best quality with the normal two-thread Vulkan stem runtime, default UI renderer and retained driver cache. The 15 W ceiling and thermal guard stayed in place; the launcher made no profile, cap or kernel writes.

The worst hop took 8,888.825 ms: graph compute accounted for 8,616.798 ms, source reading 4.501 ms and overlap handling 40.763 ms. Native inference used 219.609 ms of process CPU within 8,752.435 ms of wall time. Five runtime-main-thread snapshots inside its reconstructed envelope all showed sleeping state and `drm_syncobj_array_wait_timeout.constprop.0`. Across the run, 43 of 64 runtime-main snapshots showed that synchronization wait. Point counts are not duration fractions or exact CPU runqueue measurements.

During those five worst-hop observations, `throttle_reason_pl1` and `throttle_reason_status` were one, while the other captured throttle reasons, including the thermal bit, were zero. `gt_act_freq_mhz` ranged from 350 to 450; `gt_cur_freq_mhz` was 1,350. The [Linux i915 implementation](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/intel_gt_sysfs_pm.c) distinguishes actual from requested frequency and exposes the PL1 hardware limit bit. The trace therefore supports power-limited GPU operation at those instants. It does not prove a continuous clock, exclude other limits between samples, or allocate the whole hop delay to PL1.

The observer matched executable, user, PID and birth time before and after each DRM capture. It retained three device/client identities: one player client and two runtime clients. The player exposed the same client through four descriptors; those readings were not added together. All 67 player and 64 runtime process captures completed, with no counter regressions or clock-read errors.

Inside the worst hop's sampled endpoints, the active runtime render client accumulated 4.751 GPU-busy seconds over 8.121 seconds, or 58.5% engine-active time. The player client accumulated 1.260 seconds over a nearby 8.121-second span, or 15.5%. These client intervals omit the hop's unsampled edges. They neither measure the whole device's remaining work nor distinguish useful computation, memory stalls, scheduling and other clients. The [DRM usage ABI](https://docs.kernel.org/gpu/drm-usage-stats.html) requires device/client deduplication and high-water handling for regressing counters; engine busy time is not normalized to maximum frequency.

There were 67 host snapshots at intervals of 1.999–2.213 seconds. The observer used 5.319 seconds of scan-thread CPU. This instrumented run is diagnostic evidence, with no causal improvement claim against the previous trace. All seventeen completed hops retained thirteen finite phase fields, and final `ServiceClosed` telemetry reported no dropped events. The EOF drain reached `ReadyForStop` in 4.532 seconds without either deck starving. The original native and wrapper failures remain failures.

The incomplete-generation seek resumed consumption after 6.418 seconds. Three replays recovered after 6.900, 8.406 and 7.598 seconds. These are polling observations of generation recovery, not hard GPU preemption or physical output latency. Whole-test user cancellation was not invoked.

Recording finalized with 6,227,456 stereo frames at 48 kHz, accepted and written counts equal, and zero overflow. An independent bounded WAV scan found all samples finite and matched native frame counts and peaks. Callback deadline misses, source-read failures, rejected commands and invalid buffers were zero. One driver-position discontinuity was observed, with no recovery flags. The prepared peer did not starve. All 11,485 visible-UI checks were healthy. The finite recording and healthy UI do not erase the stem gaps or establish gap-free audio.

One temperature sample reached 99°C; no consecutive sampled span remained at or above 85°C. Two-second sampling cannot establish the spike's exact duration. The guard naturally reduced the workload allowance to 7.5 W and later restored balanced mode. Workload cap snapshots recorded 7.5 W 60 times and 15 W nine times, including setup and drain; these counts are not time fractions. No sustained-temperature pass is inferred from this short run.

Two earlier attempts stopped before the player or timed workload started. The first runner lacked executable permission, and the root observer lost its PID before saving a receipt. The second reached the owner's draft-plan guard. Both failures and independent cleanup receipts are retained. The final packet sets and checks executable permission, requires a frozen plan before readiness or dispatch, and saves the root unit receipt even if process observation fails. It passed 110 offline checks, file verification, source scanning and an actual desktop-wrapper armed-entry check that stopped at the expected missing-readiness boundary before dispatch. Five generated analysis checks cover duplicate descriptors, counter regressions, missing values, engine capacity and invalid units.

Independent closure verified all 42 observed process lifetimes, nine process groups, the original unit cgroup, audio resources and per-run idle inhibitors within 269.078 seconds of the final attempt's original 1,200-second window. Normal dimming, locking and sleep behavior resumed. This authorization covered one trace; no further client test is authorized. Best quality, the latency target, musical gates and required completed hour remain unchanged. M2 and the first release remain blocked.

The [evidence manifest](data/m2-gpu-client-usage-2026-10-26/verification.json) retains compressed, checksum-verified text: the original launch failures, complete-hop observations, relevant GPU/task snapshots, observer and analysis source, launch checks, build bindings and closure receipts. Unrelated process inventories, audio, models, binaries and driver caches are excluded. Next work will examine startup preparation and GPU submission/synchronization costs under the retained power ceiling.

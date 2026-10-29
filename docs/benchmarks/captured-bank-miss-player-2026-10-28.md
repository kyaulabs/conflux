# Captured-bank miss player check — 28 October 2026

The current player completed both two-minute checks. The laptop failed the unchanged strict service target below 4.095 seconds: nearest-rank p99 was 9.045 seconds across 17 complete hops, with seven misses and 7.013 seconds of steady stem starvation. Apollo reached 0.920-second p99 across 22 hops with no steady starvation. Its private virtual audio and offscreen software UI do not qualify the primary laptop or physical audio output. M2, M3 and the first release remain open.

Both hosts were dispatched concurrently after fresh approval, including the temporary laptop OneDrive pause, within one 20-minute window. The laptop settled naturally into Balanced mode before playback. Product source was `48b82030fcf6ab768c4f7b89d65021485cbf9b40`, with the captured-key continuation-miss fix from PR #147. The O2 players used the unchanged Best-quality runtime, model, tracks, sixteen-control schedule, 16 ms redraw and driver-cache seeds. The normal 15 W ceiling and installed thermal guard remained active; no power profile was selected.

| Result | Laptop | Apollo, private virtual audio |
|---|---:|---:|
| Measured interval | 120.009 s | 120.014 s |
| Complete service hops | 17 | 22 |
| Nearest-rank p99 / maximum | 9.045 s | 0.920 s |
| Hops at or above 4.095 s | 7 | 0 |
| Steady starvation | 336,640 frames / 7.013 s | 0 frames |
| Seek/replay starvation | 1,056,000 frames / 22.000 s | 683,520 frames / 14.240 s |
| Completed replays | 3 | 4 |
| Stale packets discarded | 1 | 1 |
| Recording accepted and written frames | 6,036,736 | 6,274,304 |

All sixteen controls were acknowledged on each host. The incomplete-generation seek and every scheduled replay recovered. These captures exercise logical invalidation and stale-result rejection; hard worker cancellation was not exercised. Both prepared peers had zero starvation. Both captures had zero callback execution deadline misses, driver-position discontinuities, recovery flags, invalid buffers, rejected commands, source-read failures or recording overflow. Each stereo 48 kHz float WAV finalized with independently matching header and frame counts. The finite diagnostic tails had zero starvation. Recording integrity does not establish uninterrupted stem audio, and no new listening verdict was obtained.

GPU graph execution still dominated the laptop's slowest hop: 8.768 of 9.045 seconds, with native inference wall time of 8.934 seconds. Source-read p99 was 22.931 ms, frontend p99 60.951 ms and postprocessing p99 108.551 ms. These nested durations are not additive costs.

The guard journal recorded a 97°C critical reading and immediate Power Saver selection. The separate two-second host observer peaked at 88°C; its samples can miss shorter peaks. Eighteen of its 63 samples showed 15 W, and 45 showed the guard's 7.5 W allowance. All seven misses completed while the preceding host sample showed Power Saver. Seven other hops completed against Balanced samples at 1.951–3.057 seconds. This association does not isolate the GPU clock, a shader or a driver as the cause. Brief heat peaks are recorded separately from the failed latency and audio gates; thermal protection was unchanged.

The retained command history contains one expected initial exporter `--describe` process. Repeated continuation-miss descriptors did not recur, and the complete ownership walk finished without failures. This verifies the intended reduction in unused work; the historical short run and failed hour are not a fresh counterbalanced comparison proving a latency improvement.

An earlier launch selected the previous isolation packet's OneDrive helper and stopped before laptop playback or any new pause. Its old token flags are not evidence of isolation for that attempt. Apollo independently completed that attempt at 0.978-second p99; its complete capture is retained. The corrected packet binds its own helper and proposal, rejects foreign references before host actions and refuses to restore an existing token from another attempt. All 50 offline checks passed, including the actual guarded import path with service actions mocked. The real unarmed KDE/systemd entry passed and released its inhibitors before fresh readiness was requested.

For the corrected run, OneDrive stayed paused through native cleanup and returned to its original active state. Both exact test units are retired, their cgroups are empty, the private container is gone, and the temporary dimming, locking and sleep blockers are released. The guarded outcome records successful lifecycle completion; it does not override the failed laptop performance and audio assessment. The earlier failed hour remains failed. No further hardware run, runtime promotion, milestone waiver or release is authorized by this closed test window.

The [evidence manifest](data/captured-bank-miss-player-2026-10-28/manifest.json) retains both attempts, every completed hop, raw native and host observations, source/build/launcher pins, regression checks, independent arithmetic, WAV headers and cleanup evidence. Audio, models, binaries and driver caches are excluded. No slow or failed capture was removed to improve the result.

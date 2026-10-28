# Laptop redraw comparison

The 16 ms Best-quality run completed its two-minute measurement but failed the unchanged **4.095-second service p99** target at **10.323225 seconds**. It also starved during steady playback. The 50 ms arm stopped during thermal admission before starting the public wrapper, player, model or audio output. This pair therefore provides **no measured 16/50 ms performance comparison** and closes no M2, M3 or release gate.

| Observation | 16 ms forced redraw | 50 ms forced redraw |
|---|---:|---:|
| Measured wall interval | 120.003 s | Not started |
| Complete validated service hops | 13 | None |
| Nearest-rank service p99 / maximum | 10.323225 s | Unavailable |
| Hops at or above 4.095 s | 9/13 | Unavailable |
| Load to first observed cursor advance, polling upper bound | 4.935089 s | Unavailable |
| Steady-playback starvation | 22.544 s | Unavailable |
| Seek/replay interval starvation | 32.896 s | Unavailable |
| Unrecorded final-drain starvation | 0 s | Unavailable |
| Incomplete-generation seek recovery, polling upper bound | 8.275069 s | Unavailable |
| Two replay recoveries, polling upper bounds | 13.001245 s; 11.758657 s | Unavailable |
| UI controls acknowledged | 16/16 | Not started |
| Application callback deadline misses | 0 | Not started |
| Maximum callback execution | 1.002324 ms | Not started |
| Invalid buffers / source-read failures | 0 / 0 | Not started |
| Recording written / overflow frames | 5,995,008 / 0 | Not started |

All thirteen validated hops through normal service close are retained. Nearest-rank p99 equals the maximum for this small population; it is not a sustained percentile estimate. The measured output interval contained 119.930667 seconds of callback frames. The WAV includes the earlier recording setup interval as well, so its frame count is not the measured interval's frame count. Accepted and written recording counts matched, and the WAV finalized before the unrecorded final drain.

The UI stayed exposed with no recorded presentation gap, all sixteen control changes were acknowledged and no command was rejected. Driver discontinuity and recovery flags were zero. Those results do not establish continuous musical output: the starvation counts remain audio failures. The seek and both replay recoveries completed, but this run did **not** exercise hard cancellation or measure cancellation acknowledgement. Normal shutdown is separate evidence.

The native failure was `Steady playback starved; control intervals retained separately`. The public wrapper also retained its generic `Active controls: incomplete/wrong mode` error because native health was false. Independent assessment confirmed the expected default mode and integer 16 ms interval; the generic message does not establish a cadence mismatch. The legacy baseline cleanup label says `incomplete_run_independently_closed`, but the native duration, closed intervals and diagnostic-complete fields establish that the requested 120-second measurement completed. Neither label is used to turn the failed health result into a pass.

The native blocking graph dispatch/wait interval reached 9.869516 seconds at p99. Request p99 was 10.228390 seconds, source-read p99 was 23.241 ms, graph rebuild time was zero and maximum native inference CPU time was 373.110 ms. These phase statistics need not describe the same hop and must not be added. The graph interval includes waiting; it is not a per-kernel timing or proof of continuous GPU execution.

Both frozen arms used the same signed-source executable (`9661b8e7f26f9d97ebd8694d36a4b13b5cc1f41b`), product UI, accepted Best Vulkan runtime, HTDemucs F16 model, two runtime CPU threads, Club XL excerpt, quiet finite peer, gains and sixteen-control schedule. Their private Mesa cache seeds contained the same 81 files. Only the explicit forced `window.update()` request interval differed. Request intervals are not guaranteed presentation rates. The implementation and its offline checks are recorded in [the redraw diagnostic report](normal-redraw-diagnostic-2026-10-28.md).

No power profile or cap was selected or written. The completed run started in Balanced at the normal 15 W MMIO long-term ceiling; the thermal guard reduced it to 7.5 W. Host samples recorded 15 W four times and 7.5 W fifty-nine times. Among 63 CPU/ACPI observations, the peak was 91°C, with one sample at or above 85°C and none at or above 95°C. Sampled observations do not establish continuous exposure or exclude shorter spikes. They cannot isolate power reduction, driver scheduling or UI contention as the cause of the service delay.

The 50 ms arm then waited up to its unchanged 150-second initial natural-recovery bound. The guard remained in Power Saver and logged a new 95°C critical observation during that admission interval. The admission sampler's package readings were lower at its separate observation times. These are different samples, not evidence that one sensor was wrong. The arm stopped with `Initial natural guard recovery not observed within original150s bound`; it produced no native or latency result. The admission failure remains unchanged.

Before dispatch, each arm passed 88 packet tests and file verification. Thirty-nine shared draft checks and the actual frozen-owner entry check passed. Both actual KDE dry/missing-readiness entry paths were checked, and their temporary idle inhibitors released. Source PR #131 passed all nine push, PR and post-merge CI jobs before the human supplied fresh physical conditions.

One original monotonic 1,200-second window governed the pair. Neither arm retried or renewed that clock. Independent closure verified the owned process lifetimes and groups, original unit cgroups, PipeWire resources, KDE idle inhibitors, unchanged guard identity/files and cap ceilings. Baseline closure completed 521.106 seconds after readiness; candidate admission-only closure completed at 960.858 seconds. The latter independently verified all 428 frozen file pins. Both units were unloaded. The original candidate assessor's `cleanup_complete=false` remains because the public wrapper never entered; the separate admission-only closure proves host cleanup without inventing a public cleanup receipt.

The [evidence manifest](data/redraw-player-2026-10-28/manifest.json) retains the full packet text, offline failures and corrections, all service/host observations, analysis and both cleanup receipts. Audio, executable/model, source-tree and driver-cache payloads are omitted; their hashes and manifests remain. The physical test window is closed. A fresh 50 ms run is still needed before interpreting this diagnostic, followed by unchanged laptop service, audio and sustained gates before release.

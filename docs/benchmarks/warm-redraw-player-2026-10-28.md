# Laptop redraw diagnostic at a 70°C start bound

The Best-quality 16 ms run completed 120.011 seconds with **2.099403-second service-hop p99**, below the unchanged **4.095-second target**. All 21 validated hops were below target. This is one short diagnostic; the 50 ms arm never launched, and the sustained laptop gate remains open.

| Observation | 16 ms forced redraw | 50 ms forced redraw |
|---|---:|---:|
| Measured wall interval | 120.011 s | Not started |
| Complete validated service hops | 21 | Unavailable |
| Nearest-rank service p99 / maximum | 2.099403 s | Unavailable |
| Hops at or above 4.095 s | 0/21 | Unavailable |
| Steady-playback starvation | 0 s | Unavailable |
| Seek/replay interval starvation | 16.277333 s | Unavailable |
| Unrecorded final-drain starvation | 0 s | Unavailable |
| UI controls acknowledged | 16/16 | Not started |
| Application callback deadline misses | 0 | Not started |
| Invalid buffers / source-read failures | 0 / 0 | Not started |
| Recording written / overflow frames | 5,983,744 / 0 | Not started |

Nearest-rank p99 equals the maximum for 21 hops. The complete population includes the final service drain; it is not a sustained percentile estimate or a measurement of audible seek latency. The measured output interval contained 119.973333 seconds of callback frames. Recording accepted and written frame counts matched, and the WAV finalized before the unrecorded final drain. The seek/replay starvation remains part of the result. Hard cancellation and its acknowledgement latency were not exercised.

Both arms were prepared with one signed-source executable (`9661b8e7f26f9d97ebd8694d36a4b13b5cc1f41b`), product UI, accepted Best Vulkan runtime, HTDemucs F16 model, two runtime CPU threads, Club XL excerpt, finite quiet peer, gains and sixteen-control schedule. Their initial private Mesa caches matched. Only the requested `window.update()` interval differed; it does not specify the actual presentation rate.

This separate protocol used the existing 70°C fresh-start bound for a full sixty-second sampled settling interval as well. It replaced the earlier 55°C settling criterion for both new arms. The earlier [55°C-admission result](redraw-player-2026-10-28.md), including its 10.323225-second p99 and starvation, remains unchanged. Different admission conditions and separate attempts prevent attributing the improvement to redraw load, temperature, power or any single cause. There is still no completed 16/50 ms comparison.

No profile or power cap was selected or written. Admission first observed the thermal guard's 7.5 W reduction, waited for natural recovery and started playback in Balanced under the normal 15 W ceiling. Among 67 CPU/ACPI samples at two-second intervals, four reached at least 85°C and one reached 95°C. Each was isolated in the sampled sequence. These observations neither measure continuous exposure nor exclude shorter spikes; this was not the required hour soak.

The native and independent ordinary assessments passed their short-run checks. Independent closure verified 41 owned process lifetimes, six groups, the unit cgroup, PipeWire resources, idle inhibitors and unchanged guard/cap policy. Its legacy status text says `incomplete_run_independently_closed`; the duration, closed intervals and `requested120s_complete=true` fields establish completion. That text is not used as the measurement verdict.

The owner finished about 307 seconds after readiness. Independent closure occurred about 652 seconds after readiness, beyond the original six-hundred-second allowance for dispatching the second arm. Its later non-dispatch receipt recorded only 431.245 seconds left in the original 1,200-second window. The candidate unit, player, model and audio output never started. No clock was renewed and no retry occurred.

The [evidence manifest](data/warm-redraw-player-2026-10-28/manifest.json) retains the protocol, human clock, complete native/service observations, assessments, closure and non-dispatch records. Audio and runtime payloads are excluded. The physical window is closed. The prepared follow-up uses an autonomous owner for sequential variants on each host, with laptop and Apollo jobs dispatched together. Apollo remains secondary virtual-audio evidence. M2, M3 and `0.1.0` remain open.

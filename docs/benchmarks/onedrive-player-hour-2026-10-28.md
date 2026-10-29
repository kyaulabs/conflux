# Player hour with OneDrive paused — 28 October 2026

The laptop closed its 3,600.021-second measured interval but failed qualification. All 489 completed service hops give a 9.330-second nearest-rank p99 against the unchanged strict target below 4.095 seconds. Steady stem starvation totaled 206.725 seconds. Pausing OneDrive was insufficient for this hour gate. M2, M3 and the first release remain open.

Both hosts started concurrently after explicit approval for the hour and the temporary laptop sync-service pause, within one 73-minute window. The product source remained `dfff55ea78b6fcbe633a178e996371238803013d`, with the same Best-quality runtime, model, inputs, UI and driver-cache seeds as the preceding short run. The established hour schedule used repeated live-deck seeks/replays, a finite prepared peer and recording; the short sixteen-control schedule was not extended to an hour. The laptop retained visible UI, 16 ms forced redraw, its normal 15 W ceiling and the installed thermal guard. No power profile was selected. Seventy offline checks, a final launcher check and the unarmed desktop entry check passed before launch.

| Result | Laptop | Apollo, private virtual audio |
|---|---:|---:|
| Measured interval | 3,600.021 s | 182.297 s, incomplete |
| Complete service hops | 489 | 33 |
| Nearest-rank p99 | 9.330 s | 0.946 s |
| Maximum hop | 10.365 s | 0.946 s |
| Hops at or above 4.095 s | 169 | 0 |
| Steady starvation | 9,922,816 frames / 206.725 s | 0 frames |
| Seek/replay and peer-control starvation | 26,194,688 frames / 545.723 s | 1,120,256 frames / 23.339 s |
| Prepared-peer starvation, included above | 2,048 frames | 0 frames |
| Replays / peer rewinds | 121 / 8 | 7 / 0 |
| Stale packets discarded | 1 | 2 |
| Final scheduled recovery complete | Yes | Yes |
| Recording accepted and written frames | 173,058,048 | 9,262,336 |
| Recording overflow | 0 frames | 0 frames |
| Callback execution deadline misses, capture total | 2 | 0 |
| Driver-position discontinuities / recovery flags | 4 / 0 | 0 / 0 |
| Invalid buffers / rejected commands / source-read failures | 0 / 0 / 0 | 0 / 0 / 0 |
| Maximum GUI heartbeat gap | 115.342 ms | 69.082 ms |
| Maximum frame-presentation gap | 148.198 ms | 55.352 ms |

Starvation seconds are frame totals divided by 48,000; counts from separate decks can overlap in wall time. The recording includes startup before the measured interval. Its stereo 48 kHz float WAV finalized with matching accepted/written totals, nonzero content and no overflow. That verifies recording integrity, not uninterrupted live-stem audio or a new listening verdict.

Both captures prove a seek after generation acceptance and before completion, followed by recovery and stale-result rejection. All scheduled laptop recoveries settled. Hard worker cancellation was not exercised. The laptop's finite diagnostic tail retained a further 226,560 starved frames, or 4.720 seconds, after the measured boundary and WAV were closed. That unrecorded tail completed its raw/converter/EOF drain but failed its final health check. Its two later completed hops remain in the population; no slow final work was censored.

GPU graph execution remained the largest measured stage: its p99 was 9.032 seconds, compared with 14.955 ms for source reads, 68.787 ms for the native frontend and 143.613 ms for postprocessing. Native inference wall p99 was 9.231 seconds. These are nested stage distributions, not additive costs, and they do not identify a particular shader, driver wait or thermal cause.

The laptop retained 1,764 host samples: 801 showed Balanced/15 W and 963 showed Power Saver/7.5 W. The sampled maximum was 100°C. There were 58 readings at or above 85°C, with a longest consecutive sampled span of 2.102 seconds, and 13 isolated readings at or above 95°C. The largest sampling gap was 2.412 seconds. The journal retains nine immediate critical responses and the subsequent recoveries. These point samples do not establish continuous temperature exposure or exclude shorter spikes. Brief peaks are reported separately from the failed latency and audio gates; no thermal policy was changed.

Apollo stopped because the private sink still had its short-test 200-second lifetime. The sink closed while the player was active, and the owned master-link check failed. Its 0.946-second p99 is from an incomplete attempt, not an hour or laptop qualification. The private daemon, sink and container were cleaned up. The sink lifetime must cover the nested hour and cleanup budgets before another control run. The unchanged functional-only native mode also retains its shorter replay end margin; Apollo is an offscreen software-UI and virtual-audio control throughout.

The laptop wrapper retained five process-exit observation races while walking short-lived children. Known process groups, jobs and routing resources were absent at the end, and the native leader was reaped, but historical ownership proof remained incomplete. The original `cleanup_proven=false` result is preserved. Retiring the exact unit afterward does not retroactively complete that proof.

The approved OneDrive pause held through the native cleanup attempt. Restoration used the corrected 45-second start wait, then failed its immediate executable-identity assertion. The same service invocation and PID were later verified running the pinned OneDrive executable, and the token was confirmed without another start. The original traceback, incomplete token and failed guarded outcome remain intact. The unit uses `Type=simple`; systemd documents that startup can be reported before execution of the service binary. This is consistent with the observed assertion, but the transient mismatched executable was not logged, so the exact interleaving is unproven. [systemd service lifecycle](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html#Type=)

Both exact test units are retired, OneDrive is running, and the temporary dimming, locking and sleep blockers are released. The original testing window is closed. No automatic retry, runtime promotion, milestone waiver or release occurred.

The [evidence manifest](data/onedrive-player-hour-2026-10-28/manifest.json) binds complete text captures, source/build/owner pins, the approved pause, raw native results, independent arithmetic and restoration evidence. Audio, models, binaries and driver caches are excluded. All 489 laptop and 33 Apollo completed service hops remain in their respective populations. The laptop hour and incomplete Apollo attempt remain failed; no observation is dropped to improve p99.

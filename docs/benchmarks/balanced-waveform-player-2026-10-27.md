# Balanced laptop waveform-cache comparison

Both Best-quality laptop runs failed the unchanged **4.095-second service p99** target: **11.206 s** without waveform caching and **11.299 s** with it. Both also starved during steady playback and final drain. All sixteen UI changes, the seek and three replays completed in each run. This pair does not close M2, M3 or the 0.1.0 release gate.

The user explicitly chose “Keep the laptop live-stem gate” after the secondary Apollo diagnostics. The X1 Carbon remains the primary target. Apollo's short private-audio result does not substitute for laptop qualification. The laptop's temporary 25 W Performance override was already removed; this comparison selected no power profile and wrote no power caps.

| Observation | Caching off | Caching on |
|---|---:|---:|
| Measured wall interval | 120.019 s | 120.004 s |
| Complete validated service hops | 17 | 17 |
| Service p99 / maximum | 11.205946 s | 11.298825 s |
| Hops at or above 4.095 s | 11 | 11 |
| Load to first observed cursor advance, polling upper bound | 4.780584 s | 4.746821 s |
| Steady starvation | 20.208 s | 15.184 s |
| Seek/replay interval starvation | 29.312 s | 34.314667 s |
| Unrecorded final-drain starvation | 4.288 s | 8.661333 s |
| Incomplete-generation seek recovery, polling upper bound | 6.128117 s | 6.005952 s |
| Replay recoveries, polling upper bounds | 4.572171–9.656122 s | 4.608729–13.957162 s |
| UI controls acknowledged | 16/16 | 16/16 |
| Application callback deadline misses | 0 | 0 |
| Maximum callback execution | 1.237825 ms | 0.831179 ms |
| Invalid buffers / source-read failures | 0 / 0 | 0 / 0 |
| Recorded frames, stereo float32 at 48 kHz | 5,990,912 | 5,987,328 |
| Recording overflow frames | 0 | 0 |

Nearest-rank p99 equals the maximum for seventeen hops. Every validated hop through normal service close is included, including the two delivered during each final drain. No warmup or late observations were removed. The measured interval and WAV boundaries stayed fixed while raw output, conversion, render EOF and service close finished. Native health remained failed with `Finite diagnostic tail final health/close failed`; complete evidence and cleanup do not turn that failure into a pass.

Both WAVs finalized with accepted and written counts equal. Driver discontinuity and recovery flags were zero, and no UI exposure gaps were observed. Those results do not establish continuous musical output: the starvation counts above remain audio failures. No new listening verdict was requested. This pair did not run a hard-cancellation or cancellation-acknowledgement probe; seek/replay recovery and normal shutdown are separate evidence.

The runs used the same accepted Best Vulkan runtime and HTDemucs F16 model, two runtime CPU threads, Club XL excerpt, finite quiet peer, gains, sixteen-control schedule and equal private Mesa cache seeds (81 files each). The original pinned builds were reused: baseline `c3105dfd7f3ad0d710f51fba594f7847e32e3906`, candidate `894bcda423b75debc2f3a07d761d63b5083e5729`. The production source differences are limited to `DeckSurface.qml` and the controller's waveform revision/cache fields. No Apollo runtime, NVIDIA cache or private-audio mode was used on the laptop.

Both started in Balanced with a 15 W MMIO long-term ceiling and naturally changed to Power Saver. Baseline host samples recorded 15 W eleven times and 7.5 W sixty-two times; candidate samples recorded seven and sixty-eight, respectively. Observed peak temperatures were 96°C and 95°C. Baseline had two consecutive samples at or above 95°C spanning 2.102 seconds; candidate had one such sample. These sampled spans do not establish continuous exposure or exclude shorter spikes. The original thermal guard, file hashes and process lifetime stayed unchanged; lower limits were preserved during cleanup.

The native blocking GPU graph dispatch/wait interval reached **10.887082 s** without caching and **10.895060 s** with it. Graph rebuild time was zero. Source-read p99 was 19.299 ms and 56.543 ms, while native inference CPU time reached 363.213 ms and 449.297 ms. These phase maxima can belong to different hops and must not be added. The graph interval includes waiting; it is not a per-kernel timestamp or proof of continuous GPU execution. The observations point to runtime/GPU delivery as the next investigation. This single ordered pair does not isolate power reduction, driver scheduling or UI contention as the cause, and does not establish a waveform-cache performance improvement.

Each arm passed 88 Python packet checks, file verification and the offscreen controller/UI suite. Thirty-seven draft root checks passed before freezing; the actual frozen-owner boundary check passed afterward. Both real desktop wrappers stopped at missing readiness during verification without starting a player. An initial temporary fixture indentation error was corrected and retained in the evidence. No product code changed for these checks.

The user then confirmed fresh AC/open-lid/minimized-Codex/visible-window conditions for both runs. One original monotonic 1,200-second window governed both arms; no retries or renewed timestamps were used. KDE idle inhibition was scoped to each owned test. Independent cleanup verified process lifetimes, cgroups, PipeWire resources, idle inhibitors, unchanged guard files and cap ceilings. Both arms were closed within **524.935 seconds** of the original readiness record. No further laptop run is authorized by that record.

The [evidence manifest](data/balanced-waveform-player-2026-10-27/manifest.json) binds complete observations, host samples, failed and passing offline checks, source/build pins, commands, analysis and independent cleanup. The archive excludes audio, executable/model and driver-cache payloads; the original WAVs remain local. Further work must retain Best quality, the laptop target and thermal protection, reduce the runtime graph/wait cost, then pass fresh short checks before another sustained qualification attempt.

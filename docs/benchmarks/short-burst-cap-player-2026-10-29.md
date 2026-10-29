# Temporary burst-cap player diagnostic

The laptop still missed the strict 4.095-second Best-quality service target: **4.231-second p99**, with one late delivery among 21 completed hops. Both hosts completed their two-minute measurements without steady-playback stem starvation, and all scheduled seeks, replays and sixteen controls settled. Apollo reached 1.005-second p99 on private virtual audio. These short captures do not qualify the current-player hour, hard cancellation or M2/M3 and release gates. The temporary cap was restored; no host policy or runtime was promoted.

| Observation | Laptop | Apollo control |
| --- | ---: | ---: |
| Closed measured interval | 120.013 s | 120.010 s |
| Completed service hops | 21 | 22 |
| Nearest-rank service p99 / maximum | **4.231 s** | 1.005 s |
| Hops at or above 4.095 s | **1** | 0 |
| Steady starvation | 0 s | 0 s |
| Seek/replay starvation | 23.280 s | 14.293 s |
| Accessible controls acknowledged | 16/16 | 16/16 |
| Scheduled replays recovered | 4/4 | 4/4 |
| Callback execution deadline misses | 0 | 0 |
| Driver discontinuities / recovery flags | 0 / 0 | 1 / 0 |
| Recording overflow frames | 0 | 0 |
| Recorded stereo frames accepted and written | 6,142,720 | 6,274,048 |

These small populations make nearest-rank p99 equal the maximum. Every completed hop and every measured interval is retained. Logical seek invalidation while the previous generation remained unfinished and one stale-packet rejection were observed on each host. This does not exercise hard worker termination. Invalid buffers, rejected commands, source-read failures and scene errors were zero. Both diagnostic tails closed without added starvation. The finalized stereo 48 kHz float32 WAV headers, payload sizes and frame counts match the native recording scans. Recording integrity does not remove the seek/replay buffering or Apollo's virtual-driver discontinuity; no new listening verdict was obtained.

The laptop's sole late hop was the first request. Its 4,231.283 ms service duration included a 4,093.944 ms GPU graph interval, of which 4,031.613 ms was graph compute/wait. Native inference CPU time was 2,716.597 ms. The remaining twenty service deliveries ranged from 2,574.055 to 2,926.856 ms. Stage durations are nested and must not be added. They identify first-request work as the next profiling target without establishing compilation, scheduling or clocks as its cause. Cold startup remains counted: load to the first observed cursor advance was 7.962 seconds on the laptop and 10.654 seconds on Apollo. The initial seek required another 7.841 seconds from command to published reserve on the laptop.

The human-approved change lowered only the laptop's MMIO short-term allowance from 64 W to 15 W for the bounded test. The MMIO long-term ceiling remained 15 W, all MSR allowances and time windows were preserved, and the existing three-sample thermal guard with immediate 95°C response stayed active. The laptop first settled for sixty sampled seconds at or below 70°C. During the native capture all 71 host samples retained Balanced mode and 15 W for both MMIO allowances. The observed maximum was 69°C, with no sample at or above 85°C. Two-second samples cannot exclude shorter spikes. This is a single follow-up without a newly matched 64 W control, so it does not establish a causal improvement over the earlier failed runs.

Both hosts reused the frozen O2 player at source `48b82030fcf6ab768c4f7b89d65021485cbf9b40`, unchanged Best model/runtime, tracks, cache seed and 16 ms forced redraw. The exact laptop native PID, executable, UID, service invocation, logging category and clock window confirm Qt's basic render loop in the original journal. Its empty captured stream files and original unconfirmed stream-only assessment remain preserved. Apollo retained its offscreen software UI and private audio graph, without host audio output, profile changes or changes to unrelated services.

The first attempt stopped on the laptop before native playback because disk space was below the wrapper's bound. Its temporary cap was held for about one second and restored; no laptop latency or audio measurement exists for that attempt. Its complete Apollo control reached 1.040-second p99 and remains separate. A retained interrupted-hour recording was moved to the dedicated Apollo archive after byte-count and SHA-256 verification, freeing 1.12 GiB locally; the original recording's pointer and transfer receipts remain. The fresh retry passed 91 offline checks and the actual unarmed KDE/systemd entry before new human readiness. It checked at least 3 GiB free before dispatch, before pausing sync and again before applying the cap. Both hosts were then dispatched concurrently within the same twenty-minute operator budget, without an automatic retry.

The approved OneDrive pause held through native cleanup. The cap helper restored the exact 64 W short-term value before OneDrive returned to its original active state. The guard later recorded a 96°C critical reading, after native capture and cap restoration; its lower 7.5 W long-term allowance was preserved by final verification. An initial exact-snapshot verification rejected that lower value; the corrected read-only check verifies restoration of the owned setting while accepting lower external power limits. Both exact owner units are gone with empty original cgroups, the root cap helper is terminal, the private Apollo container is absent and KDE's temporary inhibitors are released. A root-owned Apollo unit log required a second read through sudo; no workload restarted. Cleanup success is separate from the retained laptop latency failure.

The [evidence manifest](data/short-burst-cap-player-2026-10-29/manifest.json) binds the frozen packets and checks, original two-host observations, completed audio/recording results, prior disk-space failure, journals, derived phase timings and restoration proofs. Audio, models, executable binaries and generated caches are excluded. The next experiment must isolate first-request overhead or supply a matched cap comparison before any permanent policy change or renewed hour qualification.

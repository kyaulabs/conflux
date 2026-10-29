# Corrected shader-cache player follow-up

The laptop passed the unchanged **4.095-second target in this two-minute diagnostic**, reaching **3.279-second p99** with no late service deliveries or steady starvation. Apollo reached **0.894 seconds** on private virtual audio. This follows the retained 4.231-second laptop failure; it does not close the current-player hour, hard cancellation or M2/M3 and release gates.

| Observation | Laptop | Apollo control |
| --- | ---: | ---: |
| Closed measured interval | 120.014 s | 120.005 s |
| Completed service deliveries | 21 | 22 |
| Nearest-rank p99 / maximum | **3.279 s** | **0.894 s** |
| Deliveries at or above 4.095 s | 0 | 0 |
| Steady stem starvation | 0 s | 0 s |
| Seek/replay starvation | 25.584 s | 14.261 s |
| Accessible control acknowledgements | 16/16 | 16/16 |
| Scheduled replay recoveries | 4/4 | 4/4 |
| Callback deadline misses | 0 | 0 |
| Driver discontinuities / recovery flags | 0 / 0 | 0 / 0 |
| Recording overflow frames | 0 | 0 |
| Recording frames accepted and written | 6,075,648 | 6,276,352 |

The small service populations make nearest-rank p99 equal the maximum. All completed deliveries and measured intervals remain counted. Both hosts exercised a logical seek while the previous generation was unfinished and rejected one stale packet. Hard worker termination was not exercised. Invalid buffers, rejected commands, source-read failures and scene errors were zero. Both diagnostic tails closed without extra starvation. Recording headers, payload sizes and frame counts match the native stereo 48 kHz float32 scans. No new listening verdict was obtained; seek/replay buffering remains an unresolved usability cost.

The launcher had placed the copied seed under `driver-cache/mesa_shader_cache` and also set that directory as `MESA_SHADER_CACHE_DIR`. [Mesa appends its cache subdirectory](https://docs.mesa3d.org/envvars.html#envvar-MESA_SHADER_CACHE_DIR), so the previous run wrote a second nested cache away from the intended seed. The fresh packet changes the environment value to the copied seed's parent. The actual completed run now has one cache level with 84 files and no second nested cache. Cache hit statistics were not collected.

Three generated filesystem/child checks reproduce the previous missing-seed lookup, verify the corrected child reaches the original marker and shader fixture, preserve independent per-run copies and refuse an existing destination. All 94 offline checks, root-unit syntax and the actual unarmed KDE/systemd entry passed before fresh human readiness. These checks do not execute Mesa or a GPU. The frozen model, runtime, player, UI, seed bytes, 16 ms redraw, basic render loop, temperature rules and original twenty-minute operator deadline stayed unchanged. Both hosts launched concurrently; no automatic retry occurred.

The first laptop delivery took 3,053.072 ms, including 208.766 ms of native inference CPU time. The prior first delivery took 4,231.283 ms with 2,716.597 ms of native inference CPU time. The corrected run's worst delivery was 3,278.613 ms. This sequential follow-up supports investigating the cache setup without proving a counterbalanced causal performance estimate. Cold startup remains counted: load to first observed cursor advance was 6.534 seconds on the laptop and 10.711 seconds on Apollo. Nested phase durations must not be added together.

All 72 sampled laptop host readings retained Balanced mode and 15 W for the MMIO long-term allowance; the approved temporary short-term allowance was also 15 W. The observed maximum was 69°C, with no sample at or above 85°C. Two-second samples cannot exclude shorter spikes. The thermal guard, immediate 95°C response, MSR limits and power time windows stayed unchanged.

The cap helper restored the original 64 W short-term allowance before the previously active OneDrive service resumed. Final readback confirmed the original constraints, the same active guard and released KDE inhibitors. Both exact test units are gone with empty original cgroups; the root helper is terminal and Apollo's owned private container is absent. No permanent host policy was installed.

The [evidence manifest](data/corrected-cache-player-2026-10-29/manifest.json) binds the original observations, journals, phase profile, WAV audit, actual cache layout and exact cleanup/restoration receipts. Audio, models, executable binaries and generated caches are excluded. The manifest records the omitted temporary Path fixture, whose unchanged code triggered the same generic-key false positive as the previous report; its immutable pin and test results are retained. The earlier failed result remains in [PR #157](https://github.com/kyaulabs/conflux/pull/157). A new sustained run and hard-cancellation evidence are still required; this report does not publish a release or enable an unqualified analysis backend.

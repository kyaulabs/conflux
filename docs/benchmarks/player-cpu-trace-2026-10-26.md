# Player CPU trace, 26 October 2026 UTC

The 120.009-second visible-player trace passed the short service and functional
checks. Its 21 complete hops had median 1,992.862 ms and nearest-rank p99/maximum
2,968.023 ms, with none at or above the unchanged 4,095 ms target. Steady and
peer-deck starvation were zero. M2 remains open: the guard warning occurred
only 13.744 seconds before the measured loop ended, and just two hop validations
followed it. This does not qualify sustained low-power playback or resolve the
earlier failures.

| Check | Measured result |
|---|---|
| Quality and runtime | Best, full-F32 four-row baseline with coarse graph timestamps |
| UI and audio workload | Same player, default visible UI, source, gains, two runtime CPU threads and sixteen controls as the preceding baseline |
| Complete service phases | All thirteen fields on every hop; final closure retained without observation loss |
| Seek/replay starvation | 781,056 frames / 16.272 seconds at 48 kHz |
| Incomplete-generation seek recovery | 5.868-second poll-observed upper bound |
| Four replay recovery bounds | 2.646, 2.646, 2.591 and 2.702 seconds |
| Control acknowledgements | 16 / 16 |
| Healthy visible UI observations | 11,115 / 11,115 |
| Recording | 6,062,080 stereo frames; accepted and written counts match; zero overflow |
| Independent float-WAV scan | All 12,124,160 samples finite; counts, nonzero count and peaks match native |
| Callback/driver errors | Zero execution deadline misses, invalid buffers, rejected commands, source-read failures, recovery flags or position discontinuities |
| Cleanup | All known owned lifetimes, groups, original unit cgroup and scoped idle blockers gone within 376.034 seconds of the original ten-minute readiness |

The seek superseded an accepted generation before its observed raw completion.
Native inference-start was not observed, so interruption of executing GPU work
remains unverified. Recording includes setup audio; its integrity checks do not
remove the measured control holds or establish physical output latency. No new
listening verdict was requested.

The added observer sampled process CPU counters and thread counts at the
existing two-second host-poll cadence. Rates require matching PID/start-tick
identities at consecutive observations. First-seen processes retain raw totals
without an invented interval rate. Sixty-seven observations cost 1.340 seconds
of observer-thread CPU in total: mean 20.000 ms and maximum 60.397 ms per scan.
This overhead is retained in the diagnostic and is not assumed free.

The native player process accumulated 98.88 seconds of CPU between its first
and last observations, spanning about 136 seconds. That process contains audio,
conversion, controller and rendering threads; this capture does not separate
their costs. The separate native stem runtime accumulated 2.37 seconds over its
observed span, with another 1.86 seconds already present at its first sample.
Its maximum observed thread count was thirteen. These measurements do not
identify shader compilation as the cause of the earlier stalls.

Other observed CPU changes included OneDrive at 12.27 seconds, the compositor
at 12.15, Plasma at 10.72, Baloo at 9.05, ChatGPT at 7.82 and one later Baloo
extractor at 3.08 seconds. Observation spans differ, and work outside each
process's first/last samples is omitted. Background activity was present;
these records do not establish which process caused a temperature spike.

Two host samples reached 93°C, with none at or above 95°C and no consecutive
high-temperature sample span. The unchanged guard later recorded an 85°C
warning and selected low-power mode, then recovered naturally before final
closure. Point samples do not establish continuous exposure. No launcher
selected a profile, raised a configured limit or changed the guard. The retained
15 W ceiling and normal promoted runtime remain unchanged.

Preparation passed 58 generated-fixture checks and the complete input preflight.
The prior 1,200-second paired envelope was reduced to a single 600-second human
window, while retaining the 540-second owner, original recovery/cooldown rules
and full 120-second measurement. An initial stale owner-clock fixture still
used the old 630-second boundary; its failed log is retained alongside the
corrected 30-second boundary checks. No model ran during those offline checks.
The player binary remains the build from
`119a579e4ca717a1d59d7a7a6f9752534d68dcaf`; intervening tracked changes were
documentation only, and its compiled-source and binary proof was reused.

The [verification record](data/m2-player-cpu-trace-2026-10-26/verification.json)
binds compressed UTF-8 sources, plans, observations, original verdicts,
independent analysis and cleanup to their original hashes. Music, PCM, models,
executables and caches remain private. Fixed order and different thermal
histories preclude attributing the improvement over earlier runs to this
observer. No runtime is promoted. The next diagnostic should separate CPU use
within the native player's threads before choosing an optimization.

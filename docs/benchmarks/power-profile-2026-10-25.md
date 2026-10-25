# Guarded power-profile diagnostic

The 25 October 2026 UTC balanced-start player run completed its two-minute
measurement but missed the unchanged service target of p99 < 4,095 ms. Best
quality, the normal Vulkan runtime and two runtime CPU threads stayed enabled.
The player was built from `3629036363c3a3d2c861293e7806e7466524e34f`.
M2's latency and sustained-testing gates remain open; `0.1.0` is not released.

| Balanced-start result | Observed value |
|---|---:|
| Measured wall interval | 120.016 s |
| Published service hops | 17 |
| Service median / nearest-rank p99 and maximum | 6,521.568 / 7,362.000 ms |
| Hops at or above 4,095 ms | 11 of 17 |
| Steady / peer-deck starvation | 0 / 0 frames |
| Seek/replay recovery starvation at 48 kHz | 1,291,776 frames / 26.912 s |
| Automated control acknowledgements | 16 of 16 |
| Healthy visible-window observations | 7,443 of 7,443 |
| Recording frames accepted = written | 6,562,560 |
| Recording overflow / callback execution deadline misses | 0 / 0 |

All thirteen service timing fields covered the complete hop population, with
zero telemetry loss and a final `ServiceClosed` record. The incomplete-generation
seek recovered within its 6.848-second poll-observed issue-to-cursor bound.
Three replays recovered in 4.950, 8.154 and 7.289 seconds. The seek was issued
before an observed `RawComplete`; native inference-start was not observed.
These records establish generation supersession and recovery, while executing
GPU preemption remains unverified.

Invalid buffers, rejected commands, source-read failures, driver recovery flags
and driver position discontinuities were zero. The complete recorded WAV was
checked independently: all 13,125,120 float samples were finite, its counts and
peaks matched the native report, and no recording overflow occurred. Its
136.720 seconds include setup audio outside the measured interval. These checks
leave the reported seek/replay interruptions intact; physical output latency
and a new listening verdict were not measured.

The unchanged thermal guard recorded an 87°C warning at 20:20:32 UTC and restored
balanced mode at 20:22:58 UTC. Runtime point samples included two balanced and
67 low-power platform observations. The sampled package maximum was 75°C; the
highest selected sensor value was 87°C. These samples do not establish continuous
excursion duration or a causal profile effect. This was a balanced-start run
under the existing guard, not sustained balanced-mode evidence.

The performance arm completed cooldown, then stopped before any player, audio
or model inference because selecting that profile violated the retained cap
ceiling. Its original logger did not preserve the offending cap snapshot.
A separately authorized profile-only probe reproduced the failure and recorded
the MMIO long-term configured limit changing from 15 W to 64 W. The other nine
audited enable, limit and time-window fields stayed unchanged. The probe
completed in 0.152 seconds, restored balanced mode and all ten original values,
and started no workload. These are configured limits; effective hardware limits
and actual consumption were not measured. There is no completed current-build
performance workload or paired latency comparison.

Both player arms used a temporary KDE idle blocker. The completed measurement
had no hidden, unexposed or stale-frame observations, and the blocker released
afterward. Earlier retained attempts include a 102.443-second run stopped by Qt
exposure reporting and a later setup-only stop with insufficient native working
time. Neither becomes a completed test. Dimming was a possible cause of the
first exposure stop, without a matching journal event to establish it.

The public wrapper rejected the failed service target before writing its final
input, guard and power fields. The private driver therefore also failed its
metadata gate. A separate root check verified the complete raw measurement,
original file identities, unchanged guard lifetime, capture bounds and process
absence before continuing the authorized second arm within the original
20-minute window. Original failed receipts remain unchanged. The runner now
collects final metadata before validating active controls; a regression first
reproduced the missing fields, then all 27 runner tests passed. The strict target
and failed latency verdict remain unchanged.

The [verification record](data/m2-power-profile-2026-10-25/verification.json)
binds 98 compressed UTF-8 metadata, log and source files to their original hashes.
It retains the failed attempts, separate completion proof, exact cap observation
and final unit, inhibitor and profile cleanup. It publishes no audio, PCM,
model, executable or driver cache. This publication starts no new test and
changes no power setting.

# Three-sample thermal guard player check

The approved three-sample warning rule is installed on the laptop. The next
two-minute Best-quality player check still failed the laptop's strict
4.095-second service p99 and steady-audio gates. Apollo's concurrent virtual-audio
control passed its short native checks. Neither result closes M2 or qualifies
the first release.

The laptop guard now requires three consecutive readings at or above 85°C
before entering Power Saver. A reading at or above 95°C still acts immediately.
Two-second polling, the normal 15 W long-term MMIO ceiling, sixty consecutive
cool readings at or below 70°C before recovery, and manually selected Power
Saver behavior remain unchanged. Eight isolated tests exercised the actual
Bash decision loop, including critical response and recovery. The exact tested
candidate was installed atomically, retaining file ownership and permissions;
the original guard had recovered before its user service was restarted.
The service stayed active with the new identity throughout this capture.

Fresh physical readiness was accepted at 2026-10-28 23:41:15 UTC. The two hosts
were dispatched together, once, within the twenty-minute operator window. The
laptop first completed 60.811 sampled seconds at or below 70°C in naturally
recovered Balanced mode. The player, model, Best quality, validated baseline runtime,
inputs, retained driver-cache seeds, sixteen automated controls and 16 ms
forced redraws match the preceding descriptor-thread check. No player rebuild
or power-profile selection occurred. Apollo used the same private Docker
daemon, offscreen software UI and virtual audio as its preceding control;
Kairos and Hexforged were untouched.

| Measurement | Laptop | Apollo virtual audio |
| --- | ---: | ---: |
| Closed measured interval | 120.007 s | 120.017 s |
| Complete validated service hops | 17 | 22 |
| Nearest-rank service p99 | **8.503 s** | **0.907 s** |
| Hops at or above 4.095 s | 10 | 0 |
| Steady starvation | **5.659 s** | 0 s |
| Seek/replay starvation | 28.176 s | 14.240 s |
| Replays recovered | 3 | 4 |
| Accessible control acknowledgements | 16/16 | 16/16 |
| Recorded stereo frames accepted and written | 6,020,864 | 6,269,184 |
| Recording overflow | 0 | 0 |
| Callback execution deadline misses | 0 | 0 |

Both populations are small, so each nearest-rank p99 equals its maximum. The
preceding run measured 9.363/0.967-second p99 and 7.979/0-second steady starvation
on laptop/Apollo. These are sequential observations, not a randomized or
counterbalanced estimate of the guard change's effect.

The laptop reached **96°C** in two consecutive host samples, 2.127 seconds
apart. The guard journal records an immediate critical response, followed by
natural recovery after the workload. Thus the retained critical rule engaged
before a third warning sample was needed. Eight of sixty-four host samples
showed Balanced/15 W; fifty-six showed Power Saver/7.5 W. Sampling does not
establish continuous temperature exposure or exclude shorter spikes. The
unchanged MSR and short-term firmware allowances are distinct from that MMIO
long-term ceiling.

The worst laptop hop included 8,129 ms in the native graph-compute interval
within 8,503 ms of service time. That interval includes waits; it does not
isolate GPU kernel execution or prove the source of the heat. Initial observed
playback was bounded above by 5.421 seconds after load on the laptop and
10.549 seconds on Apollo. GUI heartbeat gaps peaked at 67.101/62.845 ms.

Raw observations prove an accepted, unfinished generation before the seek,
one stale-packet discard per host and final recovery. Both peer decks and
diagnostic tails had zero starvation. Invalid buffers, rejected commands,
source-read failures and scene errors were zero. Hard worker cancellation
was not exercised. Apollo retained one cumulative virtual-driver position
discontinuity of unknown interval, with no recovery flag; the laptop retained
neither. Recording integrity does not establish uninterrupted audio.

Both owners exited, private processes and routing resources were cleaned up,
and exact unit invocation identities were checked before retirement. KDE's
temporary dimming, locking and sleep inhibitors were released. This readiness
window is closed. The three-sample rule remains installed; further host-policy
changes require a separate decision. The current-player hour, hard cancellation,
musical-analysis qualification and remaining M2/M3 release gates stay open.

The [evidence manifest](data/three-sample-guard-player-2026-10-28/manifest.json) binds original and compressed hashes for the complete
text captures, approval and installation record, tested guard sources,
critical-response journal, independent arithmetic and cleanup. Audio, models,
binaries and driver caches are excluded.

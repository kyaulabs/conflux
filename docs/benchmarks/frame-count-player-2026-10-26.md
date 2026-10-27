# Best-quality player after frame-count optimization, 26 October 2026

M2 remains open. The 120.006-second player check on signed source `82cf1a8603d40c04aef9a46630c4ca3e2988d88a` missed the unchanged **4.095-second service p99 target** and recorded stem starvation. Actual audio retained Best quality. The thermal guard and 15 W ceiling stayed unchanged, with no profile selection or cap writes.

| Measure | Result |
|---|---:|
| Completed service hops | 17 |
| Service p99 / maximum | 9.767 s |
| Hops at or above 4.095 s | 9 |
| Steady stem starvation | 9.749 s |
| Starvation during seek/replay intervals | 29.008 s |
| Mixer controls acknowledged | 16/16 |

The full measured interval and service population closed. All seventeen hops have finite phase fields, the final `ServiceClosed` event reports no lost telemetry, and the separate EOF drain completed without starvation. The original native and wrapper failure verdicts remain retained. This standalone check has no paired control and does not demonstrate a causal latency improvement from the [discarded frame-count optimization](stem-frame-count-2026-10-26.md).

The worst hop spent 9.352 seconds in the native graph-compute phase, within 9.767 seconds of service time. Source reading took 4.910 ms and the overlap step 62.712 ms for that hop. The delay remains inside graph execution; this phase measurement does not distinguish slow kernels from queue waits or throttling.

The seek superseded a generation that was still incomplete and recovered consumption after 6.572 seconds. All three replays recovered, after 4.403, 11.814 and 6.348 seconds. These are polling observations of resumed consumption after generation changes, not measurements of hard GPU preemption or physical output latency. The check did not invoke whole-test user cancellation.

Recording completed with zero overflow. Its finalized 48 kHz stereo WAV contains 6,130,944 frames, and an independent bounded scan found every float sample finite. Callback deadline misses, invalid buffers, rejected commands, source-read failures, driver-position discontinuities and driver-recovery flags were zero. The finite peer did not starve. These recording and callback results do not undo the measured stem gaps. All visible-UI observations were healthy.

The sampled temperature peak was 85°C, with one observation at that threshold and none at 95°C. It was not sustained across consecutive samples. The guard naturally reduced power during the run; read-only workload snapshots observed the 7.5 W and 15 W MMIO long-term limits 59 and 10 times respectively. Those counts include setup and drain and are not duration fractions. Brief temperature peaks are not the reason this check fails.

Fresh user readiness authorized this single check after the earlier 20:00 window expired. Independent host verification closed all 45 observed process lifetimes, eight process groups, the original unit cgroup, audio resources and per-run idle inhibitors in 349.049 seconds of the 1,200-second envelope. Normal screen, lock and sleep behavior resumed. No further client test was authorized by this reply. Best quality, the latency target, musical gates and required completed hour remain unchanged; M2 and the first release remain blocked.

A separate [virtual producer-exit CI check](https://github.com/kyaulabs/conflux/actions/runs/40449505115) failed its held-peer continuity assertion. The same source passed PR CI and an isolated local check, so the cause remains unconfirmed. The failure output omitted the current audio and recorder state. The helper now retains that context before shutdown; a controlled virtual failure verified the capture, and both normal stall/exit scenarios passed afterward. The original failed verdict remains in the evidence. No continuity predicate, timeout or qualification gate was relaxed.

Original failures, measurements, the launcher checks and checksum-verified compressed text evidence are listed in [verification.json](data/m2-frame-count-player-2026-10-26/verification.json). Audio, models, binaries, shader caches and unrelated host process inventories are excluded.

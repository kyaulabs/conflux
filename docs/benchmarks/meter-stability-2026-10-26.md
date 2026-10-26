# Stereo-meter comparison, 26 October 2026 UTC

Both two-minute Best-quality player runs missed the unchanged service target
of p99 < 4,095 ms and had steady-playback starvation. Original-meter p99 was
7,306.474 ms; fixed-meter p99 was 8,490.611 ms. This pair shows no service-latency
improvement. M2 and the first release remain gated.

The runs used the same four-row full-F32 stem runtime, model, two runtime CPU
threads, default visible UI, audio excerpts and sixteen automated controls.
Qt UI profiling was disabled. Separate optimized player binaries loaded
immutable UI snapshots; only `qml/StereoMeter.qml` differed. The fixed meter
keeps its two channel objects across peak/RMS updates. Its earlier red/green
regression and full UI-suite results are recorded in the
[profiling report](player-thread-cpu-trace-2026-10-26.md).

| Check | Original meter | Fixed meter |
|---|---:|---:|
| Measured workload | 120.008 s | 120.005 s |
| Complete service hops | 17 | 17 |
| Median service latency | 3,613.495 ms | 6,070.028 ms |
| Nearest-rank p99 / maximum | 7,306.474 ms | 8,490.611 ms |
| Hops at or above 4,095 ms | 8 | 11 |
| Steady starvation | 207,104 frames / 4.315 s | 474,880 frames / 9.893 s |
| Seek/replay starvation | 1,171,712 frames / 24.411 s | 1,295,360 frames / 26.987 s |
| Peer-deck starvation | 0 frames | 0 frames |
| Initial seek recovery, polling upper bound | 5.800 s | 6.957 s |
| Replay recovery bounds | 4.490 / 7.398 / 6.963 s | 4.214 / 8.297 / 7.714 s |
| Manual controls acknowledged | 16 / 16 | 16 / 16 |
| Healthy visible UI checks | 7,386 / 7,386 | 9,437 / 9,437 |
| Recorded stereo frames at 48 kHz | 6,064,128 | 6,270,208 |
| Recorder overflow | 0 frames | 0 frames |
| Callback deadline misses / invalid buffers | 0 / 0 | 0 / 0 |
| Rejected commands / source-read failures | 0 / 0 | 0 / 0 |
| Driver recovery flags / position discontinuities | 0 / 0 | 0 / 1 |
| Highest sampled CPU/ACPI temperature | 94°C | 91°C |

Every delivered hop retained all thirteen finite service-phase fields. Final
`ServiceClosed` records and producer counters showed no observation loss, and
both runs completed their finite diagnostic tails. With seventeen hops, the
nearest-rank p99 equals the maximum. These complete short-run populations do
not establish sustained or production p99. The original failed native, wrapper
and assessor receipts remain unchanged. The wrapper's generic control-failure
message does not mean the sixteen actions were ignored: all were acknowledged;
the native failure was steady starvation.

Every scheduled seek and replay recovered. These bounds include polling,
worker completion and buffering. Cancellation invalidates a generation; an
executing GPU request may finish before its output is discarded. This evidence
does not establish hard GPU preemption or acceptable recovery latency.

Independent WAV scans found all 12,128,256 original-meter samples and all
12,540,416 fixed-meter samples finite. Frame counts, nonzero counts and channel
peaks matched the native recording summaries. Neither file lost recorder
frames. This verifies recording integrity, while the measured starvation still
represents missing deck audio. The fixed run also recorded one driver-position
discontinuity. No physical-output latency or new musical acceptance is claimed.

The sampled main thread used 70.30 CPU seconds over 127.680 observed seconds
with the original meter, and 59.40 over 137.300 seconds with the fixed meter.
Scene-graph thread totals were 24.11 and 20.95 CPU seconds over their respective
125.610- and 135.232-second spans. All 63 original and 68 fixed native-thread
capture batches were complete, with no capture errors. These different spans
include setup/tail work and exclude CPU outside their first/last observations;
they are not matched 120-second CPU measurements. The lower sampled UI cost
does not prove a causal latency benefit. At the slowest hop, graph compute/backend
waiting occupied 6,870.996 ms and 8,203.898 ms respectively; the meter change
did not remove that delay.

Both runs started after natural recovery to balanced mode. The unchanged
thermal guard switched to power-saver early in each workload, at 94°C and
91°C. Two-second host polling saw three and one samples at or above 85°C,
respectively, and none at or above 95°C. Each hot observation was isolated in
that sample sequence. Sampling does not establish the exact duration of a
spike or exclude shorter peaks. No profile was selected or cap raised; the
retained 15 W ceiling and natural guard reductions remained in force.

Preparation passed 71 offline checks per arm and both file preflights. Both
runs used one original twenty-minute human readiness window. All owned
lifetimes, process groups, original nonempty unit cgroups and scoped KDE idle
blockers were absent within 647.718 seconds of its start. Automatic dimming
and locking inhibition ended with the tests. No retry or clock renewal occurred.

The [verification record](data/m2-meter-stability-2026-10-26/verification.json)
binds compressed plans, private diagnostic sources, complete observations,
original failed receipts, analyses and cleanup to their original hashes.
Audio, models, executables and caches remain private. The meter fix is retained
for its verified object-lifetime behavior; this pair does not justify a runtime
promotion, relaxed latency target or M2/M3 release-gate change.

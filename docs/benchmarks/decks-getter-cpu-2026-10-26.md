# Deck-state getter CPU probe, 26 October 2026

The two-minute Best-quality player diagnostic completed with service p99 of
3,878.167 ms across 21 complete hops, below the unchanged 4,095 ms target.
There was no steady-playback starvation. This is one instrumented short run;
it does not qualify sustained latency, close M2 or permit the first release.

The private build added wall and thread-CPU clocks around
`DeckController::decks()` and one final JSON counter field. The getter's map
construction, controller behavior and audio engine were unchanged. The player
used the fixed stereo-meter UI, default renderer, four-row full-F32 Vulkan
runtime, Best model, two runtime CPU threads, the same Club XL/quiet-peer
excerpts and sixteen automated control changes. Qt QML profiling was disabled.
The 15 W ceiling and thermal guard remained unchanged; no power profile was
selected.

| Check | Result |
|---|---:|
| Measured workload | 120.009 s |
| Complete service hops | 21 |
| Median service latency | 2,668.824 ms |
| Nearest-rank p99 / maximum | 3,878.167 ms |
| Hops at or above 4,095 ms | 0 |
| Steady / peer-deck starvation | 0 / 0 frames |
| Seek/replay starvation | 997,632 frames / 20.784 s |
| Initial seek recovery, polling upper bound | 7.853 s |
| Four replay recovery bounds | 3.247 / 3.096 / 3.599 / 3.149 s |
| Manual controls acknowledged | 16 / 16 |
| Healthy visible UI checks | 12,276 / 12,276 |
| Recorded stereo frames at 48 kHz | 6,141,440 |
| Recorder overflow | 0 frames |
| Callback deadline misses / invalid buffers | 0 / 0 |
| Rejected commands / source-read failures | 0 / 0 |
| Driver recovery flags / position discontinuities | 0 / 0 |
| Highest sampled workload CPU/ACPI temperature | 80°C |

All thirteen service-phase fields were finite and present for every hop.
The final `ServiceClosed` record and observation counters showed no loss; the
finite diagnostic tail closed. With 21 observations, nearest-rank p99 equals
the maximum. Every scheduled seek and replay recovered. The seek exercised
generation invalidation while the previous generation remained incomplete;
an executing GPU request may finish before its output is discarded. This
does not establish hard GPU preemption or acceptable recovery latency. The
20.784 seconds of recovery buffering remain material despite the absence of
steady starvation.

An independent WAV scan found all 12,282,880 samples finite. Frame counts,
nonzero counts and both channel peaks matched the native recording summary;
no recorder frames were lost. Recording integrity does not establish seamless
deck audio during the reported recovery holds, physical-output latency or
new musical acceptance.

The getter ran 119,083 times on the GUI main thread. Its instrumented body
used 11.396 CPU seconds and 11.885 wall seconds: means of 95.696 µs CPU and
99.803 µs wall per call. Maximum single-call CPU was 2.082 ms; maximum wall
time was 23.833 ms. All clock reads succeeded. These counters include their
own clock overhead and setup/tail calls; they exclude QVariant-to-JavaScript
conversion and other GUI work. Separately sampled main-thread CPU was
34.33 seconds over 141.954 observed seconds. Those boundaries differ from
the getter counters and the measured 120 seconds, so they do not provide a
matched-interval CPU share. Repeated getter work has measurable cost; this
run does not establish that caching it would improve service latency.

During admission, the guard reacted to a 92°C spike and naturally returned to
balanced mode before playback began. All 70 workload host samples were below
85°C, and no guard events occurred in the workload interval. Two-second
sampling does not exclude shorter peaks. This cooler run is not comparable
as a causal speedup with the previous meter pair, whose workloads entered
power-saver. No cap, thermal rule or latency target changed.

Preparation passed 75 unique offline checks, file verification and the
CPU-clock fixture. The initial private native compile failed on a relative
header lookup; a separate retained build receipt records the corrected
include path and successful build. The original failed receipt remains.
Automated owner cleanup finished 382.844 seconds into the original
600-second readiness window. A later root verification after context recovery
confirmed all 34 known lifetimes, owned groups, the original unit cgroup and
scoped KDE idle blockers absent. That verification occurred at 814.609
seconds and is explicitly recorded outside the window; the clock was never
renewed. No retry occurred.

The [verification record](data/m2-decks-getter-2026-10-26/verification.json)
binds the private instrumentation, build and fixture records, plans, tests,
complete observations, original receipts and separate cleanup verification
to compressed evidence and raw hashes. Audio, models, executables and caches
remain private. The next performance check must use the player without the
getter counters; the sustained, musical-analysis and remaining M2 gates stay
open.

# M0 music under audio/UI load, 17 October 2026

The full-hour music run missed the **4.095-second continuous-hop p99 target**.
Observed p99 was **4.266 seconds**, maximum **5.008 seconds**, with
**23/1,533 hops above target**. All eleven cancellation/seek checks passed,
all six independent audio fault counters were zero, and UI coverage passed.
M0 remains open for the sustained delivery tail; M1 has not started.

| Run | Continuous hops | p50 / p95 / p99 (s) | Maximum (s) | Target misses |
|---|---:|---:|---:|---:|
| 90-second validation | 36 | 2.138 / 2.295 / 2.326 | 2.326 | 0 |
| 3,600-second music/audio/UI run | 1,533 | 1.977 / 3.625 / 4.266 | 5.008 | 23 |

All 1,544 delivered hour hops, including eleven trimmed seek-first hops, have
the same p99, maximum and miss count. The first five-minute bin reached
2.493-second p99; the worst complete bin, minutes 25–30, reached 4.673 seconds.
Five of twelve complete bins missed the p99 target. The final bin reached
4.057 seconds with one individual miss. Bins describe the tail over time;
they add no acceptance gate. Percentiles use nearest rank on dependent,
repeated windows and do not establish a population confidence bound.

| Track | Continuous hops | p99 (s) | Maximum (s) | Target misses |
|---|---:|---:|---:|---:|
| Club XL | 389 | 4.079 | 4.594 | 3 |
| When the Light Breaks | 386 | 4.614 | 5.008 | 9 |
| Under My Skin | 380 | 4.467 | 4.819 | 10 |
| STFU | 378 | 3.727 | 4.296 | 1 |

The benchmark now accepts repeated `--source-wav` arguments. It rotates the
four existing stereo 44.1 kHz PCM16 excerpts, original track time 30.0–53.4
seconds, only after each file completes. Each source retains its own identity,
normalization, reference hashes and completion counters. There is one
persistent separator, four native threads, a two-hop queue, 7.8-second windows,
5.85-second hops and 1.95-second overlap. Sources are prepared WAVs; original
FLAC decoding is outside this run. Files are not concatenated. Compared with
the earlier synthetic hour, source rotation and the reference-cache workload
also change, so this is not a causal content-only comparison.

Signed producer HEAD was `850225c998b3af40d27d21e496178bed7eb91d7f`.
The runtime, model, worker, UI and audio binaries match the existing
[runtime decision](../decisions/0002-m0-stem-runtime.md) and
[software-UI experiment](software-ui-2026-10-17.md). The benchmark-only corpus
mode leaves the default synthetic path available. Four new regression cases
cover source rotation, identity/parity isolation, invalid read-only inputs and
synthetic cleanup; all four CTest entries passed before measurement. Both
producer push and PR CI passed. The archived launch records retain exact
arguments, environment and artifact hashes.

Misses formed twelve groups of adjacent late continuous hops. Native graph
execution/wait occupied 93.5–97.3% of their service time; graph duration alone
exceeded 4.095 seconds on fourteen misses. The worst hop was When the Light
Breaks at excerpt frame 257,985: 5.008-second service, 4.724-second graph and
0.555 ms ready-queue delay. Queue delay on all misses stayed below 2.318 ms.
This localizes most delay to native execution/wait without identifying a
specific kernel or proving a power-management cause.

All 7,069 native policy snapshots were balanced with `balance_performance`
EPP. Misses had per-hop mean sampled active GPU frequencies of 410–531 MHz,
against requested means of 1,194–1,350 MHz. Fifty-five passing hops also had
active means at or below 531.25 MHz. The PL1 reason was asserted in 6,534
snapshots, including many passing periods; sampled thermal reason was always
zero and CPU thermal-throttle counters did not advance. Sparse samples,
at most 0.575 seconds apart, describe association rather than effective
clocks or a causal explanation. The guard was enabled and no runner changed
profiles or services.

Sampled package temperature peaked at 79°C and TCPU at 73.05°C; neither had
a sample at or above 85°C. The bounded guard journal contains no warning
during either measured workload. It records an 85°C warning at
21:56:50.758306 UTC between runs and recovery 121.181 seconds later. That
message spacing describes the guard latch, not hot duration or the exact
triggering sensor. Hour readiness took 209.420 seconds, outside the measured
hour, and the fresh admission reading was 41°C. Brief heat remains acceptable
under the user's stated criterion; this run's failed qualification is the
latency target.

Every cancellation began at least 150 ms after a revalidated active request.
All eleven held stale chunks were rejected, matching eleven discarded
request/generation pairs. Logical cancellation acknowledgement took
0.025–0.061 ms; active native work drained in 1.699–3.254 seconds. Replacement
delivery took 5.650–10.182 seconds after cancellation. Seek-first readiness
after resume was 3.836–6.820 seconds. All eleven trimmed outputs were
sample-exact with zero maximum difference. One backend remained alive across
the corpus. No stale chunks were delivered and no telemetry was lost; queue
high-water was one of the permitted two. GPU preemption is not demonstrated.

All four sources completed and repeated. Every continuous grid hash matches
the prior Vulkan output supporting the user's initial listening acceptance.
This adds output identity, not a new listening verdict. Separately, 383 source
loads produced first output in 1.697–4.919 seconds, with 4.224-second p99;
source preparation took 18.6–141.4 ms. Three loads were cancelled before first
output and the final load reached workload end. These source-admission times
include preparation and first inference, which hop service excludes; they
are retained separately from the hop target and seek results.

The silent PipeWire probe covered all 3,600.000356 measured seconds at
48 kHz stereo with negotiated 256-frame stream/graph quanta. It recorded
675,296 callbacks, six zero fault counters, callback p99.9 histogram upper
bound 20 µs and maximum 1,971.729 µs. These are callback and reported driver
timing measurements; stem playback and physical loopback latency are outside
the test. The software UI used native Wayland/API 1 and the existing 33 ms
QTimeLine cadence. Its 112,559 frame notifications include sixty complete
minute bins at 31.25–31.35/s, maximum gap 50.049 ms, eighty motion observations
and zero sampled exposure loss. Frame notifications are not physical
presentation latency. All 86 AC/lid snapshots were online/open.

Native VmHWM was 281,136 kB and supervisor VmHWM 211,220 kB, both with zero
sampled swap. The separate DRM resident-system maximum was 535,780 KiB.
These maxima are not added into a total. Four RAPL energy fields remained
permission-denied, so energy/power measurements were unavailable.

The [evidence manifest](data/music-load-2026-10-17/archive-manifest.json)
binds 68 files, 23 exact prior references, nine compressed JSONL streams with
13,398 records and two compressed analysis JSON files. Stored evidence is
3,439,102 bytes plus the manifest. Six packaged replays reproduce saved
event/control, coverage/cadence, timing, thermal and descriptive-analysis
facts. Models, binaries, audio, arrays and the listening page are excluded;
saved-data replay cannot recompute excluded PCM or human acceptance.
All owned test processes and inhibitors were released. Further benchmarks
are stopped; the listening server remains available. The next M0 work is to
reduce or budget the sustained native execution tail while preserving these
music, cancellation and audio/UI checks.

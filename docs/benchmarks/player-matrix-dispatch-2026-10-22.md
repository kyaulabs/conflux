# Matrix-dispatch diagnostic, 22 October 2026

The latest completed Best-quality player diagnostic missed the unchanged
4.095-second live-stem service target. Its 21 validated hops had a nearest-rank
p99 and maximum of **5.161 seconds**, with one hop at or above the limit. This
short, instrumented sample does not qualify normal or sustained latency.
M2 remains open; Best quality and the two-thread default are unchanged.

The [verification record](data/m2-matrix-dispatch-2026-10-22/verification.json)
retains the original plan, build and readiness receipts, exact PCM results,
native traces, observations and cleanup evidence. This report publishes saved
data from the completed run; it starts no new test.

| Check | Observed result |
| --- | --- |
| Best-quality PCM comparison | All 16 cases matched the reference exactly; capture and cleanup passed |
| Player interval | 120.008 seconds; functional checks passed, overall latency gate failed |
| Service observations | 21 validated hops from 22 native attempts; p99/max 5,161.067 ms |
| Steady / peer starvation | Zero / zero frames |
| Control buffering | 879,104 frames, or 18.315 seconds |
| Source-read failures / invalid buffers | Zero / zero |
| Callback deadline misses / recording overflow | Zero / zero |
| UI | Hidden throughout 12,406 checks; no frame notifications or visible-UI qualification |

The incomplete-generation seek and four replay starts recovered. Their
poll-observed recovery upper bounds were 4.641 seconds for the seek and
2.253–4.210 seconds for replay. The old native attempt completed without a
validated hop. This exercises generation retirement; it does not measure a
hard-cancel or kill deadline.

The saved WAV contained 6,009,856 finite stereo frames at 48 kHz, with a peak
near 0.0536. It includes time outside the measured player interval; a further
native EOF/idle-close tail was unrecorded. Callback execution had a 131 µs p99
upper bound and a 432.842 µs maximum. These counters and sample checks do not
establish audible continuity or replace listening, particularly during the
reported control buffering. Owned processes, groups, audio objects and the
application cache were gone after cleanup. Audio, model and runtime binaries
remain private; the record publishes their relevant hashes and metadata.

The worst request spent 5,093.294 ms in the native runtime and 4,869.607 ms in
graph compute. Its GPU query intervals totaled 4,849.321 ms: 2,129.700 ms in
matrix queries and 2,719.620 ms in the remaining queries. Flash queries accounted
for 1,005.932 ms within that remainder. That Flash figure excludes attention
head-copy intervals and must not be added again to matrix plus remainder.
Matrix geometry, routes and dispatch signatures were unchanged across the
22 attempts. Both matrix and nonmatrix intervals grew; the trace does not
identify a single cause or measure exclusive GPU time.

There were 67 temperature, GPU and package snapshots at two-second intervals.
The highest observed temperature was 92°C; one sample reached at least 85°C,
and none reached 95°C. These samples do not establish continuous excursion
duration. The observed profile changed from balanced to low-power, while the
reported MMIO long-term limit changed from 15 W to 7.5 W. The separate MSR
interface reported 64 W. These settings and intermittent clocks are
confounders, not measured consumption or proof of the effective limit.

The combined diagnostic finished 413.278 seconds after its original readiness
reply, within the 960-second operator window. The record pins the existing
harness binary separately from its runtime-loaded QML; it does not claim that
the older binary was built from the recorded UI source commit.

The next proposed experiment changes the tile for one recurring matrix shape.
Its candidate remains unpromoted and untested on the GPU. It must first pass
exact Best-quality PCM comparison under fresh laptop test conditions.

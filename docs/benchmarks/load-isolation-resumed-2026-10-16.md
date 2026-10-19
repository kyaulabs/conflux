# M0 remaining load isolation

The three remaining laptop hours completed in order: UI only, worker only,
then audio only. Worker-only and audio-only continuous-hop p99 met the
**4.095-second target**. UI-only p99 was **4.927 seconds**; the earlier combined
hour was **5.941 seconds**. M0 remains open for delivery under UI load.
All 33 new cancellation/seek checks passed, and the audio-only probe recorded
zero observed faults. M1 has not started.

| Selected load alongside the worker | Continuous hops | p50 / p95 / p99 (s) | Maximum (s) | Target misses | Hour p99 |
|---|---:|---:|---:|---:|---|
| Audio + UI, before pause | 1,296 | 2.194 / 4.790 / 5.941 | 7.476 | 228 | Fail |
| UI only | 1,386 | 2.155 / 4.205 / 4.927 | 6.189 | 107 | Fail |
| Worker only | 1,692 | 1.962 / 3.333 / 3.833 | 4.466 | 3 | Pass |
| Audio only | 1,649 | 1.992 / 3.424 / 3.915 | 4.611 | 9 | Pass |

Each resumed workload lasted at least 3,600 seconds. Percentiles use nearest
rank; repeated windows are not independent tail samples. All-hop p99, including
eleven seek replacements, was 4.927 seconds for UI only, 3.801 for worker only
and 3.915 for audio only. The target comparison above uses continuous hops.

Every complete five-minute all-hop bin in the UI-only hour failed the target.
Worker-only passed eleven of twelve bins; audio-only passed ten. The whole-hour
passes therefore include intermittent tail excursions. The tiny final partial
bins contain no hops and are excluded from these counts.

| Resumed condition | Controls passed | Cancel acknowledgement (ms) | Active native drain (s) | Replacement delivered after cancel (s) |
|---|---:|---:|---:|---:|
| UI only | 11/11 | 0.023–0.070 | 1.770–4.522 | 5.985–13.584 |
| Worker only | 11/11 | 0.031–0.097 | 1.672–3.326 | 5.646–10.143 |
| Audio only | 11/11 | 0.020–0.085 | 1.624–2.618 | 5.561–9.323 |

Every invalidated held chunk was rejected, every discarded native result
matched its superseded generation/request pair, and no stale chunk reached the
consumer. Recorded exact and numerical seek parity passed with zero maximum
absolute difference. Successful PCM arrays are not retained for a fresh
numerical comparison. Each run loaded one backend, reached ready-queue high
water one against a two-hop limit, and dropped no telemetry. Cancellation is
logical; active GPU work drains before replacement work starts. Hardware
submission time and GPU preemption are not measured.

The audio-only probe covered the full workload with 675,305 callbacks and
172,878,080 frames. It negotiated 48 kHz stereo and 256-frame stream/graph
quanta. All six counters were zero: callback execution deadline misses,
missing/invalid buffers, truncated frames, observed graph-position
discontinuities and observed clock recovery flags. Callback p99.9 had a
23-microsecond histogram upper bound; maximum execution was 372.847 microseconds.
This is independent silent output on the available laptop audio path. The user
has no external interface and expects realistic timing; an interface and perfect
device timing are not requirements for this laptop assessment. These checks
measure callback/transport stability. Audible stem playback, physical latency
and complete graph xrun accounting were not measured. Audio was disabled and
unmeasured in the UI-only and worker-only hours.

The UI-only window produced 215,844 final frame notifications. All sixty
complete minute bins were nonzero, maximum notification gap was 62.297 ms,
and no sampled exposure loss occurred. These are GUI-thread notifications,
without a physical presentation measurement. UI was disabled in the other two
resumed conditions.

Package temperature peaked at 82°C for UI only, 80°C for worker only and 85°C
for audio only. The audio-only 85°C observation occurred at workload +34.071 s;
its neighboring package observations were 73°C and 74°C, 1.021 s apart.
This supports treating the observed event as a brief spike under the user's
thermal guidance. Actual continuous hot duration is unresolved between reads.
No sampled sensor reached 95°C. The native trace records only package and
TCPU sensors, with maximum gaps of 0.575, 0.566 and 0.563 s
respectively. Six other mapped thermal zones were not sampled. These records
cannot reconstruct every spike or establish how long those other sensors
stayed hot. A peak alone is not assigned a thermal failure.

The normal laptop thermal guard remained active. Its warnings during idle
readiness delayed launch until recovery; they were not benchmark failures.
All recorded worker-only and audio-only policy snapshots were balanced.
UI-only snapshots
included a natural low-power phase, from about +34.056 to +157.326 seconds.
All measured policy phases remain in the percentiles. The runner never set or
restored a power profile or changed guard, kernel or driver settings.

The sequence preserves the frozen ThinkPad/Vulkan runtime, model, fixture,
four threads, blocking fence waits, child-only passive OpenMP settings,
overlap grid, worker and sampler from the
[initial isolation report](load-isolation-2026-10-16.md). Source commit
`b375522` produced the harness; resumed runs record documentation-only HEAD
`aec8501`. Source/binary hashes and the nine retained golden window hashes
match across completed conditions. No inference or delivery implementation
changed during this continuation.

Each resumed condition received the same finite ten-minute minimum cooldown
and separate sixty-second balanced/no-hold/recovery readiness gate. The laptop
was first found on battery; no workload began until the user connected AC and
confirmed the lid open. The monitor recorded AC online in all 292 snapshots
during the resumed series, with a maximum gap of 46.917 seconds. Continuous AC between reads is
not directly observed. The earlier combined hour preceded a pause and travel.
Fixed order,
charging/background activity, cache/RAM history, natural policy transitions
and one run per condition limit causal attribution. The results point to UI
load as the next M0 profiling focus; they do not quantify an isolated causal
cost for each probe.

The earlier approximately twenty-second stall and output rejection did not
recur in these three hours. Bounded launch-to-wrapper-exit kernel queries
completed with no matching graphics/fence/reset records. Their different host
history and limited query scope leave the earlier root cause unconfirmed.
Musical quality/seam checks, stem routing and full-deck load remain pending.

Independent audits recomputed event/trace timing, cancellation pairs, coverage,
policy and thermal observations. Replaying the copied audits against staged
compressed evidence reproduced the recorded facts. The
[evidence manifest](data/isolation/manifest.json) covers raw compressed logs,
executed sources, pins, independent audits, resume instructions and lifecycle
records. It excludes model weights, binaries and generated WAV/F32 files.

All workers and selected probes exited, all generated fixtures were removed,
and owned window, desktop-idle and system sleep guards were released. The
finite scheduler and monitor exited successfully. No further benchmark is
running or scheduled; the next work remains in M0.

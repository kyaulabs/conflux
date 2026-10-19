# M0 software UI comparison

The measured software-rendered UI configuration met the full-hour synthetic
delivery target on the laptop. Continuous-hop p99 was **3.971 seconds** against
**4.095 seconds**, with **7/1,615 misses** and a **4.612-second maximum**.
All eleven cancellation/seek controls passed, all six audio fault counters
were zero, and UI coverage passed. This result applies to the tested synthetic
configuration. M0 still needs evaluation of musical quality and audible seams on
representative permitted music, followed by a documented runtime choice.
M1 has not started. All benchmark processes and owned guards are stopped.

| UI renderer, 33 ms updates | Workload | Observed frames/s | Continuous hops | p50 / p95 / p99 (s) | Maximum (s) | Target misses |
|---|---:|---:|---:|---:|---:|---:|
| Vulkan | 15 min | 31.250–31.267 | 406 | 2.123 / 2.901 / 3.916 | 4.389 | 1 |
| Software | 15 min | 31.233–31.350 | 423 | 2.039 / 2.736 / 3.782 | 3.949 | 0 |
| Software | 60 min | 31.250–31.267 | 1,615 | 1.932 / 3.485 / 3.971 | 4.612 | 7 |

Frame rates are complete-minute GUI observations. Percentiles use nearest
rank; serial windows are not independent tail samples. Both 900-second trials
passed their observed p99 threshold and two controls. The software result
selected the conditional hour; short results do not qualify as full-hour
measurements. Including seek replacements gives 408/425 short-run hops with
the same respective p99, maximum and miss count as the continuous populations.

The hour lasted 3,600.000227 seconds. Its p99 was 3.971408544 seconds,
123.591 ms below the target. Including eleven seek replacements gives 1,626
hops with the same p99, maximum and seven misses. Eleven of twelve complete
five-minute all-hop bins had p99 at or below 4.095 seconds. The last bin,
+3,300 to +3,600 seconds, had **4.174994413-second p99**, two misses and the
hour's 4.611940480-second maximum. This later excursion remains in the result.
No startup interval or policy phase was removed; the empty final partial bin
is retained. Per-bin values describe the run and do not add an acceptance gate.

Signed commit `da9575c5` adds `--renderer software` to both benchmark CLIs.
The UI child receives `QT_QUICK_BACKEND=software` with `QSG_RHI_BACKEND` cleared.
The existing Vulkan path still passes `QSG_RHI_BACKEND=vulkan` to both load
children; the audio probe does not use Qt. Native inference runs independently
with its environment unchanged. OpenGL remains the default. Final UI checks
require Wayland, successful rendering/capture and the requested graphics API
enum, 1 for software or 5 for Vulkan. This rejects silent renderer fallback.

All three runs used the same eight pinned artifacts/sources, generated fixture
and nine reference-window hashes. UI/audio binaries, native GPU runtime, model,
worker and sampler remain identical to the
[previous cadence hour](cadence-2026-10-17.md). The two benchmark source hashes
changed to support renderer selection. Four inference threads, blocking fence
waits, passive child-only OpenMP policy, 33 ms QTimeLine motion, 300-second
controls, 30-second request timeout and 0.5-second sampling stayed fixed.
All three CTests passed, including eighteen Python cases. CLI checks and a
native software render/capture check passed; its saved screenshot was visually
reviewed for the logo, fonts, controls and indicator.

Vulkan ran first, followed by software and then the conditional software hour,
with finite cooldowns before the latter two. Each trial passed the consecutive
sixty-second balanced/no-hold/recovery admission gate. Fixed order, background
activity and differing cache/thermal/policy history prevent attributing the
numerical difference to rendering. Software rendering shifts UI work to the
CPU, while desktop composition can still use the GPU. The previous Vulkan
cadence hour's 5.087-second p99 is context, not a matched causal comparison.

The hour's native graph p99 was 3.732 seconds, queue-delay p99 0.640 ms and
hop time minus graph time p99 175.731 ms. The interval difference is not an
isolated CPU/transport measurement. All 1,615 continuous hops had balanced/
`balance_performance` interior policy snapshots. The seven misses had
unweighted mean interior GPU-frequency samples of 411.1–518.8 MHz, median
487.5 MHz; non-miss hops had median 1,150 MHz. These discrete observations do
not establish effective frequency or the cause of a stall. Multi-second
tails remain in native execution/wait. The earlier 10.813-second hop stall did
not recur. All three bounded kernel queries returned no matching
graphics/fence/reset records and no query or parse errors.

All fifteen controls across the three runs rejected invalidated held chunks
and matched discarded generation/request pairs. No stale or post-cancel chunk
reached the consumer. Recorded exact/numerical seek parity had zero maximum
absolute difference; successful PCM arrays were not retained for recomputation.
In the hour, cancellation acknowledgement took 0.036–0.094 ms, active native
drain 1.701–3.799 seconds and replacement delivery after cancellation
5.575–10.122 seconds. Cancellation remains logical; hardware submission and
GPU preemption were not observed. Each run loaded one backend, reached queue
high water one against a two-hop bound, dropped no telemetry and closed cleanly.

The independent silent audio probe covered the full hour with 675,315 callbacks
and 172,880,640 frames at 48 kHz stereo and 256-frame stream/graph quanta.
Deadline misses, missing/invalid buffers, truncation, observed graph-position
discontinuities and clock recovery flags were all zero. Callback p99.9 had a
22-microsecond histogram upper bound; maximum execution was 1,768.185
microseconds. The short Vulkan/software probes recorded 169,069/169,050
callbacks, also with all six fault counters zero. These assess the available
laptop callback/transport path without requiring an external interface or
perfect timing. Actual stem playback, physical latency and complete graph
xrun accounting remain unmeasured.

The hour's UI produced 112,554 final frame notifications across sixty complete
minute bins, with a 48.081 ms maximum gap and zero sampled exposure loss.
Eighty saved position samples spanned x=1.258–298.742 with Reduce motion
disabled. Native Wayland, software graphics API 1, 640×400 geometry and
device-pixel ratio 2 were confirmed. Both short trials had fifteen complete
nonzero minute bins and observed motion at the same geometry. These are GUI
observations, not physical presentation or interaction-latency measurements.

The hour's 7,066 workload package/TCPU snapshots peaked at 80°C/73.05°C, with
no sample at or above 85°C or 95°C. Maximum adjacent gap was 0.587 seconds.
Each short workload retained 1,768 thermal snapshots; package peaks were
78°C for Vulkan and 79°C for software. Six other mapped sensors were not
sampled. Brief ultrabook spikes remain acceptable; peak alone is not a failure,
and no sustained-duration threshold was invented. The hour's 7,067 retained
native policy snapshots were balanced. The short software run separately
retained four low-power snapshots around +849.003 to +851.089 seconds; they
remain included. The existing thermal guard stayed unchanged, and the runner
never set or restored a profile.

All 79 workload AC/lid snapshots showed AC online and lid open, with no
observation errors. Maximum adjacent gap was 46.993 seconds; the first was
15.494 seconds after workload start and the last 23.725 seconds before stop.
Conditions between reads remain unobserved.

Observed native/supervisor process VmHWM maxima were 280,836/166,252 kB,
with maximum sampled VmSwap zero. Native DRM total/resident system-allocation
maxima were each 535,780 KiB. These overlapping fields must not be added to
each other or process RSS. Individual fdinfo read errors, two native and 7,199
supervisor, remain recorded. All four RAPL energy fields were denied on
7,067 reads each; they do not provide zero-valued energy or a power measurement.

The [evidence manifest](data/software-ui-2026-10-17/manifest.json) covers
compressed raw events/traces, producer and verification sources, independent
audits, descriptive analysis and cleanup records. All twelve packaged
cadence/event/thermal/native replays reproduced the saved facts. The
[hour result](data/software-ui-2026-10-17/software-33ms-hour/result.json)
retains full precision and all observed target excursions. Model weights,
binaries and generated WAV/F32 audio are excluded. All fixtures were removed,
workers/probes exited, owned window and sleep/idle guards were released, and
the finite sequence/monitors exited. No benchmark is running or scheduled.

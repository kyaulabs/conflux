# M0 UI profiling and Vulkan soak

The renderer-only candidate did not close M0. The laptop completed an untraced
60-minute worker/audio/UI run with Vulkan rendering: continuous-hop p99 was
**5.570 seconds** against the **4.095-second target**, with **166/1,372 misses**
and a **6.296-second maximum**. All eleven cancellation/seek checks passed and
all six audio fault counters were zero. No sustained high heat was observed in
the sampled sensors. Benchmarking is stopped; M1 has not started.

| Combined audio/UI condition | Workload | Continuous hops | p50 / p95 / p99 (s) | Maximum (s) | Target misses |
|---|---:|---:|---:|---:|---:|
| Earlier OpenGL hour | 60 min | 1,296 | 2.194 / 4.790 / 5.941 | 7.476 | 228 |
| OpenGL timing diagnostic | 5 min | 126 | 2.221 / 2.368 / 2.431 | 2.474 | 0 |
| Vulkan timing diagnostic | 5 min | 91 | 2.205 / 6.938 / 8.720 | 8.720 | 15 |
| Untraced Vulkan hour | 60 min | 1,372 | 2.209 / 4.832 / 5.570 | 6.296 | 166 |

Percentiles use nearest rank; repeated windows are not independent tail
samples. The new hour lasted 3,600.000731 seconds. Including eleven seek
replacements gives 1,383 hops, the same p99 and maximum, and 167 misses.
The first complete five-minute all-hop bin passed at 2.451 seconds p99 with
zero misses. All eleven subsequent complete bins exceeded the target, with
p99 from 4.895 to 6.016 seconds. The empty final partial bin is excluded.
Short runs therefore did not predict sustained delivery.

The UI has one continuously animated rectangle and static labels, logo and
layout. The two short diagnostics kept that animation and enabled Qt's
[render-loop and renderer timing categories](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html#logging-support),
with one-second owned-process thread/DRM snapshots. Qt routed the timing output
to the user journal; bounded queries retained 84,834 OpenGL and 83,087 Vulkan
entries. The parser was corrected for Qt 6.12's actual message grammar; its
original source/output and revision hashes are retained.

GUI animation and layout timing p99 were reported as 0 ms at one-millisecond
resolution in both diagnostics. GUI synchronization p99 was 0 ms with OpenGL
and 3 ms with Vulkan. Render-loop total p99 was 18/20 ms and swap p99 18/19 ms,
respectively. Observed GUI/render-thread CPU totals were 5.88/14.56 seconds
with OpenGL and 6.93/16.78 seconds with Vulkan over approximately 299 seconds
of process samples. These records provide no evidence of a large GUI update
stall in the short profiles. Integer timing quantization, journal omissions,
disabled scheduling-wait counters and diagnostic overhead limit that finding;
render/swap elapsed time is not GPU-kernel or physical presentation latency.

The short Vulkan diagnostic entered natural low-power mode around +4.589 to
+139.442 seconds. All fifteen continuous misses overlapped low-power samples.
Its seventy sampled-balanced hops had p99 2.557 seconds with no misses;
OpenGL's 126 hops were sampled balanced and had p99 2.431 seconds. A redundant
initial readiness observer also ran beside the first OpenGL diagnostic and
was interrupted afterward; its records remain separate from trial admission.
These differences prevent a matched renderer ranking. No speculative animation
rewrite or application-wide renderer default was introduced.

The full hour disabled verbose Qt timing and the extra UI process sampler.
The existing `--renderer vulkan` selection changed only the UI child, whose
final report confirmed native Wayland and graphics API 5. Worker/native/model,
UI/audio binaries, four inference threads, blocking fence waits, passive
child-only OpenMP policy, fixture, overlap grid and sampler stayed frozen.
All six common producer/runtime/model hashes, the fixture hash and all nine
reference-window hashes match the
[earlier isolated hours](load-isolation-resumed-2026-10-16.md). Results record
HEAD `38be36a`; producer code remains the previously measured implementation.

Native graph p99 was 5.379 seconds, while queue-delay p99 was 0.711 ms and
hop time minus graph time p99 was 172.917 ms. Graph time versus mean sampled
GPU frequency had correlation −0.913. All 7,066 native policy snapshots were
balanced; low-frequency stalls therefore persisted without a sampled
power-saver phase. These observations point toward shared GPU/frequency/power
behavior, without establishing its cause or proving a renderer improvement.
The earlier 10.813-second stall did not recur, but recurring delivery tails
remain unresolved. The bounded launch-to-wrapper-exit kernel query returned
no matching graphics/fence/reset records and no parse/query errors.

All eleven controls rejected invalidated held chunks and matched each
discarded result to its superseded generation/request. No post-cancel or stale
chunk reached the consumer. Recorded exact/numerical seek parity had zero
maximum absolute difference; successful PCM arrays were not retained for
independent recomputation. Cancellation acknowledgement ranged from
0.023–0.068 ms, active native drain from 1.980–5.931 seconds and replacement
delivery after cancellation from 6.431–16.434 seconds. Cancellation remains
logical: active GPU work drains; hardware submission and GPU preemption were
not observed. The worker loaded one backend, reached queue high water one
against a two-hop bound, dropped no telemetry and closed cleanly.

The independent silent audio probe covered the full workload with 675,325
callbacks and 172,883,200 frames. It negotiated 48 kHz stereo and 256-frame
stream/graph quanta. Deadline misses, missing/invalid buffers, truncation,
observed graph-position discontinuities and clock recovery flags were all
zero. Callback p99.9 had a 22-microsecond histogram upper bound; maximum
execution was 169.716 microseconds. This assesses callback/transport stability
on the available laptop path. An external interface or perfect device timing
is not required. Audible stem playback, physical latency and complete graph
xrun accounting were not measured.

The UI produced 215,787 final frame notifications, with all sixty complete
minute bins nonzero, a 53.141 ms maximum notification gap and no sampled
exposure loss. These are GUI-thread notifications, not physical presentation
measurements.

The hour's sampled package peak was 82°C; none of the 7,066 package/TCPU
snapshots reached 85°C or 95°C. The maximum sample gap was 0.615 seconds.
The short Vulkan diagnostic separately captured
two package samples at or above 85°C, peaking at 91°C. Their first-to-last hot
sample span was 0.507 seconds, bracketed by cooler observations 1.522 seconds
apart. That is consistent with the brief spikes the user accepts. It does not
prove continuous temperature between reads. Six other mapped sensors were
not sampled, and no numeric sustained-duration threshold was invented.
Heat is not assigned a failure from a brief peak; delivery p99 still fails.

The existing thermal guard remained active and unchanged. Each trial had a
finite sixty-second balanced/no-hold/recovery admission gate; the full hour
also received the existing ten-minute minimum cooldown. The runner never set
or restored a profile. AC was online in all 79 workload snapshots, with a
46.073-second maximum adjacent gap. The first was 39.277 seconds after workload
start and the last 0.905 seconds before stop; AC between reads is unobserved.
Fixed order, background activity, cache/power history and one hour per renderer
prevent causal attribution of the difference from the earlier 5.941-second
OpenGL result.

The [evidence manifest](data/ui-profile-2026-10-17/manifest.json) covers compressed
raw events/traces, diagnostic Qt/thread records, exact producer snapshots,
independent audits, analysis sources and cleanup records. Model weights,
binaries, generated WAV/F32 audio and unrelated journal entries are excluded.
All generated fixtures were removed, workers/probes exited, owned window and
sleep/idle guards were released, and the finite sequence/monitor exited.
The next M0 experiment should control repaint cadence while preserving visible
animation, audio and worker settings. The renderer switch alone has not met
the target; no further benchmark is running or scheduled.

# Follow-up graph stall tracing, 16 October 2026

Clean, untraced balanced-profile runs recorded hop p99 values of 2.669 seconds
with OpenGL UI and 2.630 seconds with Vulkan UI. All 61 hops in each run stayed
below the 4.095-second target. The investigation also reproduced a 10.813-second
stall under the performance profile and captured a long 100 MHz/PL1 phase.
These are results under recorded operating conditions; they do not establish
reliable delivery across every power profile. M0 remains open.

This continues the [latency investigation](latency-2026-10-16.md) using the same
persistent worker, blocking fence wait and child-local PASSIVE/zero-spin OpenMP
policy. The native binary remains SHA-256
`8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6`;
model, graph, shaders and overlap-add are unchanged. This follow-up adds
diagnostic evidence and analysis scripts, with no new runtime policy or
latency workaround. M1 has not started.

The long runs use 120 full hops and a 12,345-frame tail: 30,970,545 source frames,
702.280 seconds of deterministic synthetic stereo 44.1 kHz PCM16 input. This
longer fixture has different whole-source normalization and a different tempo
change position from the earlier 351.280-second fixture. Compare output hashes
between these new runs, rather than against the old fixture. The harness also
tests paced delivery, seeks, source replacement, cancellation and hard stop;
the failed balanced diagnostic did not reach those control checks.

The laptop is the same ThinkPad X1 Carbon Gen 9 with i7-1185G7/Iris Xe and 16 GB
RAM, Arch Linux, Mesa 26.2.4-arch1.1 and Qt 6.12.0. The running kernel reports
`7.2.10-hardened1-1-hardened`. The first two runs hold the whole machine in
`performance` temporarily, with AC online, performance firmware/EPP settings
and a `powersave` CPU governor. Each uses native Wayland/OpenGL animation plus the
independent silent 48 kHz/256-frame PipeWire probe. No competing benchmark or
build runs during inference; other desktop activity is not isolated.

| Profile / trace | First ready / delivered | Hop p50 | p95 | p99 | Maximum | Hops above 4.095 s | Run scope |
| --- | --- | --- | --- | --- | --- | --- | --- |
| [Performance / host](data/stalls/host-opengl/result.json) | 3.031 / 3.039 s | 1.930 s | 2.138 s | 2.215 s | 2.262 s | 0/121 | All scenarios complete |
| [Performance / host + GPU](data/stalls/mesa-opengl/result.json) | 3.200 / 3.208 s | 2.100 s | 2.905 s | 3.166 s | 10.813 s | 1/121 | All scenarios complete |
| [Balanced / host + GPU](data/stalls/mesa-balanced-opengl/result.json) | 2.877 / 2.886 s | 2.043 s | 2.657 s | 3.745 s | 3.911 s | 0/121 | Throughput only; later error |

Hop service and first-ready/delivered retain the previous report's definitions.
At 121 samples, nearest-rank p99 is the second-highest value. Maximum and target
miss counts are also reported. These instrumented runs do not replace the
earlier failed untraced result or establish long-term tail behavior.

The first run samples native CPU/memory, i915 client engine activity, clocks,
throttle flags and temperatures. The archived
[analysis script](data/stalls/analysis.py) joins each output hop to its matching
non-preroll `started` event by generation and request ID, then selects trace
samples strictly inside the supervisor request span. It records unsampled
edges and sample gaps. These spans exclude source read and overlap-add but
include IPC; they are not exact GPU graph boundaries. Missing clients or
decreasing counters invalidate busy fractions. Frequency and flag summaries
count snapshots equally rather than weighting them by elapsed time.

Across the first run's continuous-delivery bracket, 2,209 snapshots showed a
1300 MHz median actual GPU clock, no asserted PL1/PL2 flags, PL4 asserted in
58.04% and the GPU thermal flag in 3.30%. Package temperature ranged from
52–100 °C; CPU0 core/package thermal counters rose by 359/2705. Flags and high
temperature also occur during passing work. They do not individually establish
the cause of the earlier stall.

The first run's per-hop interior i915 render-busy fraction ranged from
77.56–95.72%, with a 90.04% median. There were no flat counter intervals,
missing-client intervals or counter decreases; the largest sample gap was
118.553 ms. The [kernel accounting definition](https://docs.kernel.org/gpu/drm-usage-stats.html#utilization)
describes engine time on the client's workloads. It does not measure useful
shader arithmetic or normalize for GPU frequency. Native inference consumed
9.931 CPU seconds over 231.253 wall seconds; graph time occupied 97.15% of hop
service. Disabled scheduler statistics and denied RAPL reads retain the
limitations described in the previous report.

The old untraced spike remains localized to graph execution. Requests 40–48
had median service/graph times of 2.031/1.975 seconds. Request 49 took
9.182/9.091 seconds, with only 0.288 ms of queue delay and no rebuild; requests
50–61 then had medians of 2.675/2.584 seconds. The same source window in the
earlier passing trace took 2.006 seconds, at a 1300 MHz median GPU clock. Thus
source content, a graph rebuild and supervisor backpressure do not explain
the isolated delay. No trace covers the failed graph itself.

Read-only driver inspection found a 7,500 ms render-engine preemption timeout,
2,500 ms heartbeat and 1 ms timeslice. The timeout's numerical similarity to
the excess delay is insufficient evidence. [Upstream i915](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/i915/gt/sysfs_engines.c)
defines it as the time an already resident context gets to vacate after a
higher-priority preemption request; expiry can reset the engine. The accessible
kernel journal had no matching graphics/reset entries in the queried interval,
and the i915 error-state file was unreadable. This does not establish or exclude
a reset. No driver settings were changed. During the first trace, the desktop
profile reported no degradation and ThinkPad lap mode read zero.

The second run enables [Mesa ANV u_trace](https://docs.mesa3d.org/u_trace.html)
only inside the native child through the archived
[launcher](data/stalls/native-mesa-trace.sh). `MESA_GPU_TRACES=print` and an
explicit `MESA_GPU_TRACEFILE` keep GPU timing text out of the JSONL protocol;
the UI does not inherit these trace variables. Text output is retained because
the cancellation/hard-stop scenarios can terminate the child without normal
Vulkan device destruction. GPU timestamps use a different clock domain from
Python `time.monotonic`; the trace has no native request IDs. Command-buffer
groups cannot be equated with whole graphs or joined directly to CPU spans.

The installed Mesa writes `%p` and `%i` literally in the filename, despite the
current documentation describing substitution. Only one child writes that
file in this run. Preserve it before rerunning; the archived trace is renamed
for readability. The second run has both host and GPU tracing overhead and is
a diagnostic comparison, not an untraced performance claim.

The GPU trace contains 3,848 complete command-buffer groups. Two adjacent
groups span 4.648 and 3.211 seconds; multiple shader/workgroup signatures have
long intervals nearby. The largest compute interval is 908.238 ms; its same
shader/workgroup signature has a 2.769 ms median across 864 observations.
These intervals can include preemption and synchronization, so they do not
identify one expensive shader. Their association with host request 94 is not
proven by an uncalibrated clock comparison. The text contains 145 small backward
adjacent timestamps at blorp-end/compute-begin transitions and one truncated
final group;
the parser preserves those warnings. It found no negative compute durations
or backward transitions between groups.

Host request 94 took 10.813 seconds, including 10.698 seconds in the graph and
191.159 ms of native inference CPU time, with no rebuild. In its
[joined host samples](data/stalls/mesa-opengl/req94-phases.json), the GPU begins
near 1300 MHz. It then spends 7.742 seconds with all 62 snapshots at 100 MHz and
PL1 asserted, despite a 1350 MHz requested frequency. During that phase, render
accounting advances in every interval but covers only 9.31% of the sampled
span. The package cools from 77 to 67 °C; the GPU thermal flag stays clear.
Firmware/EPP remain `performance` throughout the request. Clock collapse,
power-limit flags and reduced engine activity coincide; these snapshots do
not establish which caused the others. This is a distributed low-activity
phase, rather than an observed single 7.5-second flat-counter pause.

The numerical p99 still passes in that run because only one of 121 hops exceeds
4.095 seconds. Its 10.813-second maximum remains relevant to buffer depletion
and replacement latency. The short paced test does not exercise every
continuous-run stall. No driver synchronization flag or submission-batching
patch was applied on the basis of this trace.

The next comparison uses the existing `balanced` policy while keeping model,
workload and both trace layers the same. This daemon rejects balanced holds, allowing only performance and
power-saver. The archived runner therefore selects balanced temporarily,
records firmware `balanced` and EPP `balance_performance`, then restores the
prior profile in its cleanup handler. The worker and application do not change
power policy. Parsing of the prior Mesa trace overlapped the early part of
this diagnostic, including a final parser invocation spanning about 3.85 wall
seconds; its CPU time was not measured. Clean untraced validation follows after
all archival work finishes. The balanced diagnostic's complete throughput phase
recorded p99 3.745 seconds, maximum 3.911 seconds and zero target misses.

The balanced diagnostic then failed while writing native output for request
127 during paced delivery. `/tmp` was a full 4 GiB tmpfs, with only 13 MiB free
after worker cleanup; generated WAVs from repeated experiments had accumulated.
Its raw result retains `status: error`, no completed paced result and an empty
control list. The wrapper restored power-saver. The continuous measurements
remain recorded, but this run is not a complete protocol/control pass.

The [scratch cleanup record](data/stalls/scratch-cleanup.json) lists 20 generated
WAVs removed after their hashes matched the stored fixture metadata, recovering
867,327,360 bytes. The benchmark tools now remove their generated fixtures from
the fresh output directory after use, including error paths. Reports, generator
parameters and source hashes remain. This fixes the benchmark's scratch-space
growth without changing source handling in the worker API.
The final validation also starts with more free tmpfs space and RAM than the
earlier traces. These sequential tests therefore do not isolate a causal
latency effect from the balanced profile alone; system memory pressure was not
recorded during the captured stall.

After archival work stopped, two clean untraced runs used the original
351.280-second fixture: 60 full hops plus the 12,345-frame tail. Both recorded
firmware `balanced`, EPP `balance_performance`, AC online and no system/GPU
trace flags. No compilation, compression or full-trace parser ran during these
validation workloads.

| Qt UI | First ready / delivered | Hop p50 | p95 | p99 / max | Total / RTF | Hops above 4.095 s |
| --- | --- | --- | --- | --- | --- | --- |
| [OpenGL](data/stalls/balanced-opengl/result.json) | 2.811 / 2.819 s | 1.998 s | 2.382 s | 2.669 s | 124.403 s / 0.354 | 0/61 |
| [Vulkan](data/stalls/balanced-vulkan/result.json) | 2.675 / 2.684 s | 2.054 s | 2.380 s | 2.630 s | 126.823 s / 0.361 | 0/61 |

Both clean runs completed all scenarios. Forward/backward seek samples matched
their continuous-grid references exactly. Each loaded one backend, bounded
ready output to two hops, discarded six superseded requests and delivered zero
stale chunks. Short paced delivery had zero late-ready handoffs across five
intervals (29.25 seconds).

| Control during active work | OpenGL | Vulkan |
| --- | --- | --- |
| Forward seek acknowledgement / replacement | 0.050 ms / 5.236 s | 0.066 ms / 5.319 s |
| Backward seek acknowledgement / replacement | 0.090 ms / 5.206 s | 0.173 ms / 5.334 s |
| Cancel acknowledgement / residual finish | 0.059 ms / 1.592 s | 0.058 ms / 1.501 s |
| Replace source, first delivered | 6.225 s | 6.113 s |
| Cancel full queue | 0.050 ms | 0.035 ms |
| Hard shutdown during inference | 83.647 ms | 84.374 ms |

All three long runs matched their 121 continuous output-hop hashes exactly.
Both clean runs matched all 61 hashes from the original fixture, including the
earlier failed performance-profile run. Recombination and join diagnostics
remain identical within each fixture; they are not musical-quality judgments.

The clean OpenGL/Vulkan silent audio probes ran for 187.390/190.892 seconds,
with zero observed deadline, buffer or graph-position faults. Callback p99.9
upper bounds were 21/22 µs; maxima were 539.429/76.016 µs. Native Wayland
confirmed graphics APIs 3/5 and 11,224/11,438 frame swaps. These counters do not
establish physical audio latency, complete graph xrun coverage or worst UI
frame latency. Each lifecycle record verifies restored power-saver, low-power
firmware, power EPP and no holds after the run. Both generated WAVs were absent
after the new cleanup path.

Reproduce the host run with the existing
[worker instructions](../../tools/stems/README.md), using `--hops 120`,
`--trace-system`, `--renderer opengl` and a fresh output directory inside the
temporary performance hold. To reproduce the GPU diagnostic, use the archived
launcher as `--runtime` after adjusting its native binary and trace output
paths. The archived analysis scripts take plain/gzipped host traces or the
Mesa text trace and produce the retained summaries. The
[balanced runner](data/stalls/balanced-run.py) takes a lifecycle log path followed
by the worker benchmark arguments. Keep that log outside the fresh benchmark
output directory:

```sh
python3 -B docs/benchmarks/data/stalls/balanced-run.py /tmp/conflux-policy.json \
  --runtime "$stem_root/build/conflux-stem-runtime" \
  --model /tmp/conflux-stem-m0/models/htdemucs-f16.gguf \
  --ui build/dev/conflux --audio-probe build/dev/conflux-audio-probe \
  --renderer opengl --hops 60 --output /tmp/conflux-balanced-check
```

Use `--renderer vulkan` for the other clean validation. Manifests preserve
commands, file sizes, hashes and compression details. No weights, generated
audio or third-party binaries are committed.

The application build, all three CTest suites (16 worker cases) and offscreen
QML pass. Lightweight mocked checks also pass for fixture cleanup on preparation
and runtime errors, profile restoration/interruption, temporal joins, counter
validity and GPU trace parsing. The brand pack, LICENSE and README structure
remain unchanged. No new package, model/API charge or Apollo work was used.

These measurements retain a small animated UI and a separate silent audio
stream. Full deck load, actual stem routing, annotated music/listening, audible
seams and the 60-minute thermal soak remain pending. The captured low-clock
phase is evidence for further M0 work; its underlying cause is not proven.
M1 remains unstarted.

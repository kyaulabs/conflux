# M0 startup investigation and M1 decision, 17 October 2026

The 60-second startup diagnostic did not reproduce the earlier 95°C warning or
low-power transition. Native construction to readiness took **0.367 seconds**
and first-hop service took **1.879 seconds**. All six independent audio fault
counters were zero and software UI coverage passed. This single observation
does not qualify sustained live headroom or identify the earlier spike's cause.

After reviewing the results, the user approved closing M0 feasibility and
entering M1 manual audio with the **failed 4.095-second live-stem service p99
target retained as a mandatory M2 blocker before a first usable release**.
The [runtime decision](../decisions/0002-m0-stem-runtime.md) selects the pinned
HTDemucs/audio.cpp Vulkan pair for prepared local-file stems and keeps CPU as
the numerical reference. Musical acceptance remains a starting basis. Prepared
and on-demand local-file stems remain required; production stem playback is
unqualified.

| Completed music workload | Continuous service p99 (s) | Target misses | Cancellation/seek | Independent audio |
|---|---:|---:|---|---|
| [Full hour, four threads](music-load-2026-10-17.md) | 4.2655 | 23/1,533 | Eleven exact checks | All six fault counters zero |
| [600-second paced run, two threads](two-thread-music-2026-10-17.md) | 5.1722 | 8/102 | One exact check | All six fault counters zero |

Both missed 4.095 seconds. The two-thread run kept all 101 ordinary virtual
demands ready on time; buffered availability does not change its failed service
target. These sequential workloads do not establish a causal improvement from
thread count. The two-thread validation's conditional hour was withheld, and
the startup diagnostic adds no active-cancellation, seek or sustained-performance
qualification.

The diagnostic used the same producer artifacts as the two-thread run, bound
to signed source HEAD `825841849ea157272a7b27e8481d9a8565c47cb6`. It observed
two seconds of ambient activity, imported the worker, then started the existing
software Wayland UI at 33 ms and silent PipeWire probe at 48 kHz stereo/256
frames. A labeled ten-second loaded baseline preceded one Club XL first output,
logical cancellation, residual work draining and idle observation. This added
pause was diagnostic; it is not established as a fix. No runtime, model,
production source, guard, power profile or service setting changed.

Imports took 68 ms. Source load issue to native construction took 71 ms, an
interval containing model identity hashing and worker scheduling. Native
construction to readiness took 366.710 ms, including the first observer wait;
the runtime reported 363.982 ms of loading. The first request's bridge interval
was 1.828 seconds and hop service was 1.879 seconds. A second request had begun
before logical cancellation and was discarded after draining. GPU preemption
was not tested. Receive-only wait excludes native work before that wait;
constructor-to-ready is the observed startup bound.

All 229 host observations remained balanced with `balance_performance` EPP.
The unchanged guard had the same PID, process start identity and invocation
before and after the diagnostic, with zero restarts and no new records in the
bounded journal. The observation-window temperature peak was 73°C in the first
ambient sample, before the native process existed. A final cleanup sample
reached 74°C. No sampled guard sensor reached 85°C. These are sparse serial
readings, not continuous hot-duration measurements.

The inspected guard acts on a single sampled maximum at or above 85°C across
ACPI, TCPU, package and coretemp inputs. It requests power-saver immediately;
95°C adds a critical log. Recovery needs 60 consecutive samples at or below
70°C after two-second sleeps, so a brief spike can leave roughly two minutes
of reduced power. Journal warning-to-recovery spacing describes that latch,
not time spent hot. The guard remained active and unchanged. Brief heat adds no
failure gate under the user's stated criterion.

Host, native and supervisor traces contain 229, 94 and 117 successful rows;
maximum gaps were 0.291, 0.508 and 0.505 seconds. Eight guard sensor paths were
captured without discovery/read errors. The 68 ms import and 71 ms model-identity
intervals contain no sample-end timestamps, so their temperature transients
cannot be resolved. Native fdinfo enumeration recorded one partial-read error
and supervisor enumeration recorded 119; successful CPU samples do not prove
complete GPU-client coverage. Observer CPU is included in supervisor CPU, and
process-scan CPU is a subset of host observer CPU. Those counters are not added.

The independent silent stream ran for 57.890 seconds and 10,838 callbacks,
with six fault counters at zero, callback p99.9 histogram upper bound 11 µs and
maximum 30.150 µs. The software UI recorded 1,806 frame notifications and a
35.661 ms maximum notification gap. These cover callback and GUI notifications,
without physical audio latency, audible stems or physical presentation claims.
The worker closed, the native child was reaped, and the owned window guard and
idle inhibitor were released.

A planned `threads2-both-repeat-10min` was stopped with SIGINT during normal
readiness after the user's M1 decision. Its launcher exited 130 with
`child_started=false`, no remaining owned process and the child reaped. No
measured worker/audio/UI workload began, so this is neither a benchmark pass
nor a benchmark failure. The valid 29-row readiness trace includes a 92°C guard
warning at 20:01:17 PDT without a native worker, followed by recovery. That
shows native startup is not necessary for a warning; it does not identify the
warning's cause. The final readiness JSON was truncated during interrupt
handling; stderr records `KeyboardInterrupt` while writing it. The raw file and
stderr remain preserved; they do not provide a completed readiness verdict.

The [evidence manifest](data/startup-profile-2026-10-17/archive-manifest.json)
binds the diagnostic, source pins, independent audit, guard inspection and
interrupted-repeat evidence. Models, binaries, music, PCM and arrays remain
outside source history. M0 closes as feasibility under the user's approved
scope change; the failed live-stem target remains explicit. M1 manual audio is
next, beginning with one local file decoded outside the callback and rendered
through the shared live/offline core, followed by stereo, partition, seek, EOF
and laptop playback checks. This entry does not mark M1 complete.

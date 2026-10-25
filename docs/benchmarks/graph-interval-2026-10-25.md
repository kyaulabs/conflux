# Best-quality GPU interval diagnostic

The 25 October 2026 UTC visible-player diagnostic missed the unchanged live-stem
service target of p99 < 4,095 ms and starved during steady playback. Its coarse
GPU timestamps place almost all slow graph time inside the GPU command interval.
Best quality remains enabled; M2, M3 and the first `0.1.0` release remain open.

| Result | Observed value |
|---|---:|
| Measured player interval | 120.012 s |
| Published service hops / completed native requests | 17 / 18 |
| Nearest-rank service p99 / maximum | 7,836.059 ms |
| Hops at or above 4,095 ms | 8 of 17 |
| Steady-playback starvation at 48 kHz | 210,432 frames / 4.384 s |
| Seek/replay recovery starvation | 1,303,296 frames / 27.152 s |
| Peer-deck starvation | 0 |
| Automated control acknowledgements | 16 of 16 |
| Healthy visible-window observations | 7,999 of 7,999 |
| Recording frames accepted = written | 6,074,112 |
| Recording overflow / callback execution deadline misses | 0 / 0 |

The [verification record](data/m2-graph-interval-2026-10-25/verification.json)
binds 66 compressed metadata, source, patch and trace files to their original
hashes. It retains failed results alongside the separate root recomputation and
cleanup proofs. It publishes no audio, PCM, model, executable or driver cache.
This report starts no new workload and changes no power setting.

The silent prerequisite first matched all 16 complete Best-quality outputs to
the accepted normal-runtime reference exactly. All 16 requests had matching
producer/written input identities and valid GPU intervals. Native elapsed p99
was 1,673.129 ms, with no audio or UI load. These native-only timings omit the
player service path and cannot qualify its latency or establish instrumentation
overhead. The later player used the same diagnostic runtime with the optimized
player from `ae75c1f8f0054ee5f008858039a9f815be5e192c`, Best conversion,
full-F32 Flash attention, Vulkan and two runtime CPU threads.

The private instrumentation adds two timestamps around the existing graph
commands, inside the existing submissions. It reads the available timestamps
after the original synchronization, without an extra submission, wait or poll.
All 18 completed player requests had valid intervals and matching producer and
written-input hashes. These hashes do not attest to the native runtime's reads.
The inclusive interval can contain queue, synchronization and feeding gaps; it
is not exclusive kernel execution time.

For all eight service misses, that GPU interval occupied 99.905–99.983% of graph
wall time. The worst hop spent 7,443.306 ms in graph compute, including a
7,441.970 ms GPU interval. Host command recording took 11.370 ms and total
native process CPU time was 279.344 ms. The first request separately spent
1,678.997 ms recording commands, including cold shader creation. Later misses
had 10–17 ms command recording. Cold compilation therefore does not explain
the sustained slow period. The same normalized source-window hashes occurred
in fast and slow requests.

The incomplete-generation seek and three replay starts recovered, including
the final generation. Poll-observed issue-to-cursor recovery bounds were
7.848 seconds for the seek and 4.456, 7.898 and 7.159 seconds for replay. One
obsolete native request completed without publication, and one stale packet
was discarded. This proves supersession and observed recovery, not preemption
of an executing GPU request or a general cancellation-time bound. Native QUIT
completed normally with exit zero; no final active-request abort was exercised.

The player reported zero invalid buffers, rejected commands, source-read
failures, driver recovery flags or driver position discontinuities. Its WAV
finalized with equal accepted/written counts and finite samples. The recording
includes setup audio outside the measured interval. These checks do not erase
the reported starvation or establish physical output or listening acceptance.
Visible-window frame observations remained healthy, with maximum observed frame
age 104.119 ms; they do not measure physical scanout or every UI event.

The unchanged thermal guard recorded a 95°C warning and engaged power-saver
early in playback. Runtime point samples included five balanced and 58 low-power
observations, with a sampled package maximum of 96°C. The worst request's three
interior package observations were 46°C. Passing requests also occurred in
low-power mode. The records establish neither continuous excursion duration
nor a causal power-profile effect. No actual GPU clock trace was collected in
this run.

A separate read-only host audit at 16:55:29 UTC found power-profiles-daemon and
the user thermal guard active. No other checked power-management service was
active; cpupower was disabled and inactive. Performance was available and turbo
was allowed. The MMIO long-term package limit read 15 W, while the separate MSR
interface read 64 W. Those interfaces do not establish the effective limit or
actual consumption. The audit checked common configuration paths, but cannot
exclude previous manual changes, other custom services or BIOS settings. The
guard switches at 85°C and requires about two continuously cool minutes at or
below 70°C before recovery. Its policy and files stayed unchanged.

Earlier [M0 policy runs](policy-2026-10-16.md) already exercised performance mode
and still missed their latency target. They used older runtime conditions and
are not a current-build paired comparison. The current audit therefore does
not justify increasing power limits or disabling the guard. The
[earlier operation profile](player-matrix-dispatch-2026-10-22.md) showed matrix
and nonmatrix intervals growing together. Final attention copies were too
small to explain the delay, the larger matrix-tile candidate was rejected,
and retaining the driver cache did not fix later misses. Further runtime work
must address the measured graph cost under the laptop's retained limits.

The original outer assessment also hit a secondary `pins_after` KeyError after
rejecting steady starvation. Failed health validation had prevented the public
wrapper from creating its final pin/guard fields. Separate frozen-file and
actual-host checks proved unchanged inputs, the same guard lifetime and absent
owned processes/groups; they do not fabricate the missing public fields or
change the failed result. A fresh, unarmed reporting-only copy now treats
missing final fields as an explicit failed evidence gate. Its regression first
reproduced the KeyError, then all 54 generated checks passed. No new player run
or latency qualification followed that reporting fix.


A later Conv1d bias-broadcast candidate removed a materialized repeat before the
existing Vulkan ADD for F32 channel bias. Other backends, weights, precision,
convolution math and all other helpers stayed unchanged. The private runtime
replaced one archive member and retained the other 186 byte-for-byte. This
candidate remains experimental and is not promoted into the normal runtime.
Its [separate verification record](data/m2-conv-bias-2026-10-25/verification.json)
retains 53 compressed metadata, patch, source and trace files, including both
failed receipts and the later parse-only verification.

The actual-module CPU check matched 525,456 output bytes exactly across the
original and candidate libraries, with 64 graph cases and 32 rejected malformed
bias shapes per library. The silent laptop check then matched all 16 complete
Best-quality reference outputs exactly. Its graph had 2,823 nodes rather than
2,871, with the same ten full-F32 Flash signatures. Native-only p99 was
1,645.792 ms, with no audio, UI or cancellation exercise. That result proves
these output comparisons and graph reduction, not a paired speedup or the
loaded service target. The quiet run's runtime point samples were balanced;
the highest selected temperature observation was 87°C.

The subsequent visible-player run used the same frozen optimized player,
Best conversion, Vulkan and two runtime CPU threads. It completed its requested
120-second measured interval but failed latency and steady audio availability.

| Bias-broadcast player result | Observed value |
|---|---:|
| Measured wall interval | 120.003 s |
| Published hops / completed native requests | 17 / 18 |
| Service median / nearest-rank p99 and maximum | 3,108.496 / 7,677.237 ms |
| Hops at or above 4,095 ms | 8 of 17 |
| Steady starvation at 48 kHz | 94,720 frames / 1.973 s |
| Seek/replay recovery starvation | 1,285,888 frames / 26.789 s |
| Peer-deck starvation | 0 |
| Control acknowledgements / healthy visible observations | 16 of 16 / 8,039 of 8,039 |
| Recording frames accepted = written | 6,064,384 |
| Recording overflow / callback execution deadline misses | 0 / 0 |

All 18 requests had valid coarse GPU intervals and matching producer/written
input identities. Across the eight misses, the inclusive GPU interval occupied
99.914–99.983% of graph wall time. The worst service hop's graph compute took
7,313.715 ms, including a 7,312.416 ms GPU interval. The earlier baseline and
this run had different thermal histories and were not counterbalanced. Their
p99 or starvation difference cannot establish a causal improvement.

The incomplete-generation seek recovered within its 8.310-second observed
issue-to-cursor bound. Three replays recovered in 4.465, 7.371 and 6.909 seconds.
Request 2 completed without publication, and one obsolete packet was discarded.
This remains supersession evidence, not executing-GPU preemption. Native QUIT
completed normally without a kill. Invalid buffers, source-read failures and
rejected commands remained zero. The WAV completed without overflow; none of
these observations remove the measured starvation or qualify physical audio.

The unchanged guard engaged power-saver on an 85°C warning at 18:00:49 UTC and
reported recovery at 18:02:58 UTC. Runtime observations included five balanced
and 59 low-power samples, with a sampled package maximum of 78°C. These point
samples differ in time from the guard event and do not establish the duration
of high temperatures. No power profile, limit or guard rule was changed, and
this was not a current-build performance-profile comparison.

Two support failures also remain in the original evidence. The outer tool
session ended with exit 143 and left no completed owner receipt; its cause is
unknown. The inner workload continued and shut down within the original time
limit. Separate actual-host checks found all eight retained process lifetimes
and seven groups absent, with the same guard invocation and unchanged files.
That check does not restore the missing outer completion proof.

The trace consumer also retained the earlier experiment's run identifier. New
generated regressions reproduced rejection of this run and acceptance of a
foreign earlier run. A one-line change in a separate unarmed copy binds the
consumer to its caller's exact identifier. All four regression methods then
passed, including completed/aborted captures, foreign-run rejection and retained
input/GPU validation. Re-parsing the saved capture with that fix verified the
18-request population; it started no workload and changed no original receipt.
The candidate therefore adds exact-output and diagnostic evidence while M2,
M3 and `0.1.0` remain open under the same Best-quality and latency requirements.

# Tempo output packing, 5 November 2026

A private two-minute queue diagnostic reproduced empty tempo-output reads on the
primary laptop. It reached 7.133-second service p99 and lost
20.614 seconds of steady output; both exceed the retained performance and
continuity requirements. Apollo's matching run reached 1.007-second p99 with no steady
starvation. This instrumented, offscreen, private-virtual-audio workload does not
qualify physical audio, current laptop performance, M2/M3 or release 0.1.0.

| Observation | Laptop | Apollo |
| --- | ---: | ---: |
| Measured interval | 120.009 s | 120.012 s |
| Completed service deliveries | 17 | 23 |
| Service p99 / maximum | 7132.769 ms | 1006.625 ms |
| Deliveries at or above 4,095 ms | 6 | 0 |
| Steady missing frames | 989,460 | 0 |
| Control/startup/replay buffering | 21.739 s | 15.669 s |
| Callback deadline misses / source-read errors | 0 / 0 | 0 / 0 |
| Recording accepted and written frames | 6,020,608 | 6,274,048 |
| Recording overflow | 0 | 0 |
| Short-read trace attempts / retained / overflow | 4,912 / 64 / 4,848 | 0 / 0 / 0 |

Every retained laptop event stopped at an empty prepared view, before the bounded
reader exhausted its packet count. None of those views carried EOF or an error.
At the first event, the raw bridge had accepted a contiguous prefix through source
frame 612,576 while the nominal consumed cursor was 314,519.66, a difference of
6.210 source seconds. The tempo worker's actual input cursor was not recorded.
These observations narrow the queue problem; they do not establish one cause for
all 4,912 events, the service misses, or the earlier Apollo hour's 14 ms gap.

The laptop also failed final diagnostic-tail audio health with 5,470 additional
missing A frames outside the closed measured interval. Its WAV was finalized
before that unrecorded drain. The tail, all completed service hops and the failed
native result remain retained. Hard worker cancellation was not exercised.
Both completed units retired, and both private sinks/daemons and the Apollo
container were reaped. No power, sync or thermal-policy setting changed. The
laptop's sampled workload maximum was 89°C; sparse samples do not exclude brief
spikes or establish which profile was active throughout.

The candidate packs compatible DSP fragments into the existing eight output
slots. It publishes a partial packet before waiting for unavailable input,
changing configuration metadata, or publishing EOF/error. New generations discard
unpublished old fragments. Processing and any wait remain on the worker; the
callback, queue allocation, model and DSP settings are unchanged.

A generated four-stem +6% Master Tempo case supplies legal 37-frame input prefixes,
waits for startup, then freezes the worker and consumes 100 ms in 256-frame reads.
The old worker ran out of queued PCM; the candidate passed with the expected
consumed source clock. The initial full-input-read case passed before this change
and is not presented as a reproduction. Existing gap, EOF/error, configuration and
cancellation cases also passed in the stream suite.

An independent old/new comparison matched every output byte in 48 generated cases:
one/four stems, -6%/unity/+6%, Master Tempo off/on, 37/1,024-frame input limits and
31/12,011-frame sources. Generated PCM was removed after file-hash verification.
This confirms sample preservation for those fixed configurations on the tested
laptop DSP libraries; it is not a new music or hardware benchmark.

All 61 normal CTest targets passed on the laptop. Apollo passed targets 1–59
before the parent harness reached its 360-second deadline during the UI test;
the remaining two targets passed in a separate invocation with their per-test
deadlines unchanged. Earlier Apollo mount/build and parent-timeout failures remain
in the evidence. Seven audio suites passed ASan/UBSan with leak detection on each
host. A later regression verifies that all 333 valid frames in a partially packed
packet are delivered before EIO; the final stream suite passed again in normal and
sanitizer builds on both hosts. External DSP libraries were not instrumented.

The strict generated processed-continuation controller case passed on both hosts.
After 4,096/2,048 initial setup missing frames on laptop/Apollo, neither added an A
underrun through held recovery, prepared takeover or the settled interval. The
consumed fractional clock and controls were preserved. All 290,304/290,560 recorded
frames matched the unchanged peer exactly. A is deliberately muted in that fixture;
its recorded master does not establish affected-deck listening quality. Core tempo
and callback tests supply complementary A DSP/bus checks. Both private daemons were
reaped and their remaining graph contained only the dummy driver.

Matched unarmed baseline/candidate players are built on both hosts. The local
comparison verifies identical Release `-O3` compiler flags for both workers and
drivers, an unchanged public header and common product archives. The earlier
7.133-second diagnostic used its own `-O2` build and is not a one-factor causal
control for these players. Fresh physical readiness and a new matched comparison
are still required.

The [evidence manifest](data/tempo-output-packing-2026-11-05/manifest.json) binds
source pins, diagnostic results and traces, the failed-before/passed-after regression,
48 PCM hashes, normal/sanitizer logs and both strict controller results. Diagnostic
service figures include all retained completed deliveries; starvation figures above
refer to the closed measured interval. Compact evidence retains failed results and
explicit trace overflow. Generated PCM and retired diagnostic caches are cleaned
after hash verification; original music, recordings and library data are untouched.

Candidate laptop/Apollo performance has not run. The strict 4.095-second p99 target,
Best quality, automatic musical-analysis and integrated-fallback requirements, and
full M2/M3-before-0.1.0 decision remain unchanged.

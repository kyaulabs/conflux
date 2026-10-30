# Tempo playback startup queue, 30 October 2026

Processed playback previously became ready when its worker published one audio
packet. The callback could consume that prefix before the next DSP burst arrived.
Playback now waits for the existing eight-slot queue to fill once per generation,
or for an earlier EOF or error. It does not allocate a larger queue, sleep in the
callback or change Best quality. Normal reads do not repeat startup priming.
A seek, backend change or cancellation invalidates the old readiness proof.

This addresses a reproducible readiness defect. It does not establish that every
previous laptop underrun had that cause. Queue filling can add startup or mode-
switch buffering; that cost remains part of the actual-player acceptance work.

| Check | Result and scope |
| --- | --- |
| Controlled startup regression | Both hosts rejected the old first-packet predicate and passed the candidate. Checks cover exact PCM/cursor, generation invalidation, cancellation, short EOF and explicit errors. The integrated stream suite also checks that consuming a complete slot does not restart priming. |
| Existing audio suites, private candidate | Ten suites passed per host. Product archives were unchanged during the prototype comparison. |
| Short strict controller comparison | Baseline → candidate → candidate → baseline passed on both hosts. Both baselines also passed, so this sample does not prove improved reliability. |
| Five-second direct callback hold and return | All sixteen host/cases passed: baseline and candidate, ±6%, Master Tempo off/on. Each held at least 240,128 frames after independent producer EIO, then returned through approved OUT. No additional affected-deck starvation occurred during hold/return; stale tickets, fractional transport, controls, peer clocks, recording and owner retirement remained checked. |
| Full applied build and CTest | 59/60 passed initially; the sole scratch fixture expected prefill-incompatible first-packet delivery. After its setup was corrected, that target passed on both hosts. Product code and the other 59 test binaries remained unchanged. Apollo also passed the ten original audio suites. |
| Instrumented verification | All eight selected suites passed ASan/UBSan/leaks on both hosts and TSan on the laptop. External DSP libraries were not instrumented. Apollo TSan was not retried after its earlier environment initialization failure; no new Apollo TSan test body ran. |

The direct callback fixture master-mutes the affected deck while processing it.
Every peer/master recording sample is checked exactly; complementary audio suites
check the affected processor PCM and both buses. Before the measured hold, tempo
setup buffering ranged from 768–4,864 frames for the baseline and 512–7,680 for the
candidate across hosts. Those counts remain visible. This is generated private
virtual audio, not musical, physical-device or thermal-soak qualification.

The first extended controller experiment lengthened its held span to five
seconds but still expected to observe a cache lookup in its earlier Checking
phase. That lookup intentionally held the cache lock; its existing deadline is
two seconds. All eight full scenarios failed the late observer, despite no added
starvation during their recorded held spans. The deadline explains the missed
phase from the fixture code and timing; these runs did not capture final lookup
status. They remain full failures. Product cache deadlines were not extended. The later direct callback
experiment separates prolonged hold/return coverage from the short controller
fixture's automatic cache-lookup coverage.

The scratch error fixture still tests Stop before any processed delivery and
explicit End with an actual queued processed prefix. Its later-read setup now
counts every consumed SRC frame before injecting EIO, rather than requiring the
first packet to become audible. Its original two-second deadline and exact peer,
CUE, retained-error, source-replacement and descriptor-cleanup checks remain.
The original full-suite failure is retained.

A separate prewarming prototype was rejected. One laptop candidate run failed
held-audio continuity; another stopped during fixture preparation before playback.
Those outcomes are distinct. They are retained alongside original strict
controller failures. Native CPU/thread traces were inconclusive about a single
cause: the first could not identify protected executables, and a later passing
run had more quota throttling than an earlier failure. No process protection,
ASLR, host power or thermal policy was weakened.

The [verification record](data/tempo-startup-queue-2026-10-30/verification.json)
and [archive manifest](data/tempo-startup-queue-2026-10-30/manifest.json) bind the
source, protocols and retained results. No music, model, executable or cache is
included. Current-build recovery listening, laptop/Apollo audio/UI measurements
and sustained workload qualification remain open. The Best-quality service
requirement remains p99 below 4.095 seconds on the primary laptop. M2, M3 and
0.1.0 remain open.

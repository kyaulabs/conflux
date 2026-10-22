# Exact-shape matrix-tile diagnostic, 22 October 2026

The private 128-by-128 tile candidate is rejected. Twelve completed Best-quality
outputs matched the historical reference exactly, but the required 16-case gate
did not finish. Its target GPU query intervals were also much longer than the
historical 64-by-64 route. Keep the original Best-quality runtime and tile
selection. This result provides no reason to extend the run to rescue this
candidate; M2 remains open.

The [verification record](data/m2-matrix-tile-2026-10-22/verification.json)
retains the verbatim result, native stdout/stderr, owned DRM trace, plan,
readiness, build, source-review and execution records, plus the exact patch.
The [earlier diagnostic](player-matrix-dispatch-2026-10-22.md) retains the
historical baseline. This report publishes saved evidence and starts no new
workload. It publishes no PCM, audio, model or runtime binary.

| Check | Observed result |
| --- | --- |
| Exact PCM comparison | Requests 1–12 matched case/input/output hashes, shape, dtype and byte count |
| Remaining cases | Request 13, `stfu:0`, incomplete; requests 14–16 unattempted |
| Candidate activation | One target large-tile dispatch in each of 12 complete matrix graphs; no completed graph for request 13 |
| Capture | Both streams reached EOF; no truncation, overflow or read/write error |
| Termination | Native return code −9 (SIGKILL); owned process/group and resource cleanup passed |
| Admission | Overall result failed; neither the 16-output nor 16-activation gate passed |

The patch changes only the recorded unfused, contiguous F16/F16 matrix route
with F32 output, default precision, M=172032, N=96 and K=432. Node 2026,
query 1077, item 105 selected `matmul_f16_f16acc_l`, denominators `(128,128,1)`
and workgroups `(1344,1,1)` in all 12 complete graphs. Split-K stayed at one.
The dedicated scalar shader retained its 17,408-byte shared-memory guard.
Before/after source, model and thermal-guard pins matched.

For the first six completed cases, the target query measured
**717.954–729.281 ms**; the next six measured **1,476.657–2,029.842 ms**.
The corresponding 12 historical baseline queries measured **46.643–48.448 ms**.
These are instrumented query intervals from separate runs, not paired or
counterbalanced measurements, individual dispatch timings, or exclusive GPU
time. The candidate run recorded no in-run clock, power or temperature series,
so the later growth cannot be assigned a thermal or frequency cause.

The **60-second shared profile budget** expired. Request 13 had at most
**3.286 seconds** left under that budget, despite the configured 30-second
maximum per request. The retained generic error says
`native stem request timed out; child terminated`; the watchdog separately
records `comparison total time budget exhausted`. This does not establish a
30-second native stall. A returned cleanup method is not normal native exit:
the recorded return code is SIGKILL.

Original human readiness was 21:23:24 UTC. The root command finished at
21:26:32.656966 UTC, **188.657 seconds** later and within the unchanged
300-second window; the command itself took **140.871 seconds**. No player or
audio/UI workload ran. This partial, instrumented result qualifies no service
p99, cancellation latency, audible continuity, normal shutdown or 60-minute
playback. Best quality, the two-thread default, the strict 4,095 ms service
target and all remaining release gates are unchanged.

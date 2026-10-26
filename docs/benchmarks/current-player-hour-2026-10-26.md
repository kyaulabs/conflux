# Interrupted current-player hour, 26 October 2026

The Best-quality actual-player soak stopped after 52 minutes 22.852 seconds of
the requested hour. The retained 402 deliveries had a nearest-rank p99 of
8,609.980 ms against the unchanged strict 4,095 ms target. This is an incomplete,
failed attempt. No p99 is assigned to the requested hour, and M2 remains open.

The player used signed source `a861c88b26491f8c11068f6c370a19f3d09f42b7`, the
normal promoted Vulkan runtime, two runtime CPU threads, Best conversion, the
visible UI, Club XL on deck A, a finite quiet peer on B and master recording.
The thermal guard and 15 W ceiling remained active. The operator allowed 73
minutes including setup and cleanup; that clock was never renewed.

| Retained measurement | Result |
|---|---:|
| Last progress / requested workload | 3,142.852 / 3,600 s |
| Validated service deliveries | 402 |
| Median service latency | 4,500.461 ms |
| Observed nearest-rank p99 | 8,609.980 ms |
| Observed maximum | 9,824.737 ms |
| Deliveries at or above 4,095 ms | 213 |
| Maximum native graph-compute duration | 9,488.824 ms |
| Accepted generations / raw completions | 102 / 100 |
| Retained stereo recording frames | 150,756,352 |
| Host samples / sampled package maximum | 1,532 / 97°C |

All thirteen phase fields were finite for every retained delivery. The 28,414
retained observation records had contiguous ordinals, and the 100 observed
service-telemetry reports recorded zero drops. The process was terminated
before its final native report and `ServiceClosed` record, so those facts do
not establish a complete observation population. The overall partial p99
includes every retained delivery, including power-transition intervals.

The private outer owner stopped when its cumulative process-group set reached
64. Exited helper groups consumed that limit even though the live process
population was bounded separately. The later helper triggered a TERM cleanup;
the native process exited with signal 15 and produced no final report. A new,
unarmed private packet uses the existing 4,096-entry history limit for groups.
The regression reproduces the old failure with 90 sequential exited helper
sessions and only two live processes. All eight checks pass after the fix,
including rejection of 65 simultaneous live processes, history/byte overflow,
changed lineage, PID reuse and an expired deadline. The original packet and
failed receipts remain unchanged. A separate public ownership observation
also lost a short-lived child during its walk; its failed ownership receipt
has not been reclassified by the outer-owner fix.

The package sensor recorded nine isolated samples at or above 85°C, including
two at or above 95°C. Each was surrounded by cooler package samples; sampling
roughly every two seconds cannot establish continuous peak duration. Brief
heat readings alone are not the reason for the failed latency result.
The sampled profile was balanced at 15 W in 341 observations and low-power at
7.5 W in 1,191. No profile was selected and no cap was raised by the test.

An observational split uses only Python monotonic worker/service stamps and
host samples. Ninety-six deliveries whose service intervals were bracketed by
balanced/15 W samples had p99/max 2,996.204 ms, with none reaching 4,095 ms.
The 260 deliveries bracketed by low-power/7.5 W samples had p99 8,667.686 ms,
maximum 9,824.737 ms and 203 overruns. Forty-six boundary or unbracketed
observations remain separate. This association does not establish a causal
profile comparison or prove settings stayed fixed between samples. Every
observation remains in the overall partial result.

The retained events show 102 accepted generations, 100 raw/converter
completions and one discarded packet. Queue-to-first-validated-chunk p99 was
6,131.960 ms across 102 generations. That interval is not cursor recovery,
hard GPU preemption or physical output latency. Final seek/replay recovery,
steady/controlled starvation, callback-deadline, driver and UI-health summaries
are unavailable because the final native report was lost. The last progress
sample recorded audio ready, the window exposed, 166,308 presented frames
and 588,815 callbacks; it is not a complete health verdict.

A streaming scan found all 301,512,704 retained PCM samples finite, with
channel peaks 0.053627 and 0.053402. The last progress sample reported no
recorder overflow. Forced termination left the RIFF length declaring 16 bytes
and the data length zero in the 1,206,050,904-byte file. The WAV is unfinalized;
there are no final native counts to reconcile. The original recording was
neither repaired nor treated as a successful complete recording.

Independent host cleanup finished at 3,397.609 seconds within the original
4,380-second window. All 732 known lifetimes, 66 groups, the original unit
cgroup, owned PipeWire resources and scoped idle blocker were absent. The
guard identity/files and observed cap ceilings were preserved. This physical
closure does not rewrite the automated ownership/capture failures or make
the workload complete. No automatic workload retry ran.

The [verification record](data/m2-current-player-hour-2026-10-26/verification.json)
retains compressed observations, the full thermal projection, original
failure receipts, partial analysis, independent cleanup and owner regressions.
Audio, models, executables, caches and the full per-process CPU trace remain
private, with hashes recorded where used. Further performance work must
preserve Best quality, the 15 W ceiling, thermal protection and the 4,095 ms
target. A completed hour, remaining musical gates and M2/M3 completion are
still required before the first release.


The next candidate reuses the deck-state map between GUI state publications.
Invalidation precedes existing notifications, including when Qt signals are
blocked; library publication and quiet shutdown preserve fresh reads. The
controller and offscreen UI suites passed. A separate offline empty-deck probe
made 15,000 reads per arm in three batches. GUI-thread CPU time was 687.977 ms
for the original getter and 83.617 ms for the cache, with identical serialized
snapshot values. Both helpers used ordinary O2 optimization. The
[cache verification record](data/m2-deck-snapshot-cache-2026-10-26/verification.json)
retains those results, the functional-check log and the corrected probe's
original Qt integer-overload compile failure. This narrow check starts no
audio, window, media or model. It does not measure QML conversion, loaded decks,
thermal behavior or stem latency; a controlled actual-player comparison is
still required.

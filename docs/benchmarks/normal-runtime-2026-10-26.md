# Normal-runtime interruption, 26 October 2026

The normal-runtime player attempt stopped after 73.574 measured seconds of
the requested 120 when Qt reported the window unexposed. The user confirmed
CONFLUX stayed visible and untouched. The partial run also had six of eleven
observed service hops at or above 4,095 ms, a maximum of 10,041.249 ms and
3.739 seconds of steady starvation. It does not qualify normal-runtime or
sustained performance; no p99 is assigned to the requested 120-second workload.

Only the stem runtime changed from the preceding completed repeat: the
promoted normal binary replaced the private GPU-interval build. Both had
retained exact-output proofs. The fixed-meter player, default UI, Best quality,
two runtime CPU threads, Club XL/quiet-peer audio, scheduled controls, thermal
guard, 15 W ceiling and strict 4,095 ms target were preserved. The normal
runtime supplied all thirteen finite service-phase fields for the eleven
observed hops. The slowest native graph-compute duration was 9,730.183 ms;
nested phase times are not additive.

| Check | Partial result |
|---|---:|
| Measured / requested interval | 73.574 / 120 s |
| Observed service hops | 11 |
| Observed maximum service latency | 10,041.249 ms |
| Observed hops at or above 4,095 ms | 6 |
| Steady starvation | 179,456 frames / 3.739 s |
| Seek/replay starvation | 967,168 frames / 20.149 s |
| Controls acknowledged before stop | 10 / 16 |
| Initial seek recovery, polling upper bound | 7.886 s |
| Two exercised replay recovery bounds | 4.258 / 8.155 s |
| Healthy visible UI checks | 5,594 / 5,595 |
| Recorded stereo frames at 48 kHz | 4,177,408 |
| Recorder overflow | 0 frames |
| Callback deadline misses / invalid buffers | 0 / 0 |
| Rejected commands / source-read failures | 0 / 0 |
| Driver recovery flags / position discontinuities | 0 / 0 |
| Highest sampled workload CPU/ACPI temperature | 85°C |

The guard journal recorded an 86°C startup warning and selected power-saver
about four seconds after the window started, before the measured interval.
The sampled profile then remained low-power. Only one of the 44 two-second
temperature samples reached 85°C; none reached 95°C. This does not establish
prolonged high temperature or make brief heat spikes a failure by themselves.
The latency overruns and starvation remain material. The earlier completed
repeat stayed balanced and peaked at a sampled 67°C, so this pair cannot
establish a causal cost from removing instrumentation.

The failed exposure event followed a 104.687 ms frame age. Another frame was
observed about 0.158 ms after that event; at the guard failure the window was
active, application-visible and had a 10.123 ms-old frame. No two-second stale
frame was observed. The user reported no window, desktop or screen change.
The retrospective compositor-unit journal contained no entries for the
bounded interval.

Qt 6.12's [Wayland window header](https://github.com/qt/qtbase/blob/v6.12.0/src/plugins/platforms/wayland/qwaylandwindow_p.h)
sets a default 100 ms frame-callback timeout. Its
[window implementation](https://github.com/qt/qtbase/blob/v6.12.0/src/plugins/platforms/wayland/qwaylandwindow.cpp)
can mark exposure false after a missed callback and restore it when the late
callback is processed. This is consistent with the recorded event timing,
but the run did not capture Qt's timeout flag or a subsequent exposure
recovery. No timeout override was present in the retrospectively queried
user-manager environment; the exited native process's environment was not
observed. The explanation remains a hypothesis.

The harness now observes queued window events for a requested 250 ms after
an existing window failure and before cleanup. It records elapsed time and
before/after snapshots outside the measured interval. The original failure,
measured boundaries, UI-gap accounting and whole-command deadline remain
unchanged. Healthy runs and unrelated errors gain no event-loop delay or
report field. The QtCore-only regression failed on missing followup before
implementation and passed afterward, covering persistent and recovered
exposure, preserved failures, unrelated errors and repeated freeze calls.
This diagnostic addition has not yet been tested in the actual player.

All 8,354,816 recorded samples were finite, with native frame/nonzero counts
and channel peaks matching the independent WAV scan. Recording integrity
does not remove the reported starvation or establish new musical acceptance.
Every exercised seek/replay recovered; incomplete control coverage does not
qualify cancellation latency or hard GPU preemption.

Owned cleanup and independent verification finished within the original
600-second window; root verification completed at 235.057 seconds. All 35
known process lifetimes, owned groups, the original unit cgroup and scoped
idle blockers were absent. Guard identity/files and frozen cap ceilings were
preserved. No automatic retry occurred.

The [verification record](data/m2-normal-runtime-2026-10-26/verification.json)
retains the failed run, original assessment, user's window observation,
partial audio/thread analysis, bounded journals, source references and
regression evidence. Audio, models, binaries and full third-party source
remain private. Preparation passed 79 offline checks and file/dry-entry
verification. An initial standalone regression compile lacked Qt's required
position-independent flag; the corrected build retained that error and
then reached the expected behavioral failure. M2, the hour-long test and
first-release gates remain open.

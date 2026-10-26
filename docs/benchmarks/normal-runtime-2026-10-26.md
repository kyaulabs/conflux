# Normal-runtime player checks, 26 October 2026

The later current-source check completed 120.010 measured seconds with the
normal Best-quality runtime. All 21 service hops were below the unchanged
4,095 ms target; nearest-rank p99 and maximum were 3,621.203 ms. Steady
starvation was zero. All sixteen controls were acknowledged and every
seek/replay recovered, with 19.851 seconds of recovery holds in total.
This is short-run evidence. The hour-long test and M2 remain open.

The preceding normal-runtime attempt stopped after 73.574 measured seconds of
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

| Check | Earlier partial run | Completed short run |
|---|---:|---:|
| Measured / requested interval | 73.574 / 120 s | 120.010 / 120 s |
| Observed service hops | 11 | 21 |
| Requested-workload service p99 | Unassigned | 3,621.203 ms |
| Observed maximum service latency | 10,041.249 ms | 3,621.203 ms |
| Observed hops at or above 4,095 ms | 6 | 0 |
| Steady starvation | 179,456 frames / 3.739 s | 0 |
| Seek/replay starvation | 967,168 frames / 20.149 s | 952,832 frames / 19.851 s |
| Controls acknowledged | 10 / 16 | 16 / 16 |
| Initial seek recovery, polling upper bound | 7.886 s | 7.505 s |
| Exercised replay recovery bounds | Two, 4.258 / 8.155 s | Four, 2.998–3.299 s |
| Healthy visible UI checks | 5,594 / 5,595 | 12,287 / 12,287 |
| Recorded stereo frames at 48 kHz | 4,177,408 | 6,135,808 |
| Recorder overflow | 0 frames | 0 frames |
| Callback deadline misses / invalid buffers | 0 / 0 | 0 / 0 |
| Rejected commands / source-read failures | 0 / 0 | 0 / 0 |
| Driver recovery flags / position discontinuities | 0 / 0 | 0 / 0 |
| Highest sampled workload CPU/ACPI temperature | 85°C | 77°C |

The completed run used the signed current-source native build, the same
fixed-meter UI, normal runtime, audio and controls. Its only new native code
observes queued events after an existing window failure. No failure occurred,
so that observation path was not exercised. There were no measured UI gaps
or scene-graph errors. The largest observed frame-presentation gap was
66.672 ms and the largest GUI-heartbeat gap was 62.342 ms. This does not
establish the cause or recovery duration of the earlier exposure failure.

All thirteen service-phase fields were finite for every completed-run hop,
with no observation or service-telemetry loss. Independent WAV analysis found
12,271,616 finite samples and matching frame/nonzero counts and channel peaks.
Every exercised generation invalidation recovered; this does not establish
hard GPU preemption or physical output latency. Seventy two-second host
samples peaked at 77°C, with none at or above 85°C. The profile stayed
balanced during the workload after natural recovery and the required quiet
cooldown. Guard identity, files and the 15 W ceiling were preserved.

An intervening launch stopped before the inner driver, player, model or audio
started. Root dispatch used 29.744 seconds of the startup allowance, followed
by 5.032 seconds for KDE inhibition registration; the owner correctly refused
to spend its reserved time. The retry used one prewritten entry command and
dispatched in 0.184 seconds. It retained the same 600-second operator window,
540-second owner limit, idle-inhibition requirement and all pass criteria.
Independent cleanup finished at 164.292 seconds for the stopped launch and
280.700 seconds for the completed run, each against its own original clock.
All thirty completed-run lifetimes/groups, the owned cgroup and scoped idle
blockers were absent. No automatic retry occurred.

The [followup verification record](data/m2-window-followup-2026-10-26/verification.json)
retains both launches, the full measured population, WAV/thread analysis,
offline checks, launch records and independent cleanup. Audio, models and
binaries remain private. The original failure and its evidence remain recorded below.

In the earlier partial run, the guard journal recorded an 86°C startup warning
and selected power-saver
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
The completed actual-player run did not exercise the failure-only path.

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

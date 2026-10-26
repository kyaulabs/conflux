# Player thread CPU trace, 26 October 2026 UTC

The 120.012-second visible-player trace failed because its last scheduled replay
had not recovered when measurement ended. The finite diagnostic tail was not
entered, so the original assessor correctly withheld a complete-population p99.
The 18 retained chunk deliveries had an observed median of 4,211.945 ms and
observed nearest-rank p99/maximum of 7,813.499 ms; nine reached or exceeded the
unchanged 4,095 ms target. These partial statistics do not qualify service
latency. M2 and the first release remain gated.

| Check | Recorded result |
|---|---|
| Quality and workload | Same Best, full-F32 four-row runtime, default visible UI, two runtime CPU threads and sixteen controls |
| Retained service phases | All thirteen fields on every observed hop; complete final population unavailable |
| Steady / peer starvation | Zero / zero frames |
| Seek/replay starvation | 1,527,808 frames / 31.829 seconds at 48 kHz |
| Seek recovery | 7.587-second poll-observed upper bound |
| Replay recoveries | 4.301, 7.229 and 6.724 seconds; final replay unrecovered |
| Final replay's measured output | 6.315 seconds remained in recovery; no recovered cursor was observed |
| Control acknowledgements | 16 / 16 |
| Healthy visible UI observations | 7,219 / 7,219; no missing, stale or unexposed observation |
| Recording | 6,064,640 stereo frames; accepted and written counts match; zero overflow |
| Independent WAV scan | All 12,129,280 samples finite; frames, nonzero count and channel peaks match native |
| Callback / driver errors | Zero execution deadline misses, invalid buffers, rejected commands, source-read failures, recovery flags or position discontinuities |
| Cleanup | All 44 known owned identities, groups, original unit cgroup and scoped idle blockers absent within 436.527 seconds of the original ten-minute readiness |

Seek supersession and the three completed replay recoveries were observed, but
the final recovery was incomplete. Recovery bounds include polling, worker
completion and buffering; they are not measurements of immediate interruption
of executing GPU work. The recording includes setup audio, and its integrity
does not remove the measured recovery holds or establish physical output
latency. No new listening verdict was requested.

The observer added native task CPU counters and TID lifetimes to the existing
two-second process sampling. All 63 native-thread batches were complete, with
no capture errors. The main thread accumulated 71.19 CPU seconds over its
127.195-second observed span. A pooled thread accumulated 43.20 CPU seconds
over 123.084 seconds, the scene-graph render thread 24.30 over 125.125 seconds,
and a graphics-driver thread 11.09 over 125.125 seconds. Thread names do not
identify the functions running in pooled threads. Spans differ and exclude
work before first observation, after last observation or in vanished threads.

The observer itself cost 3.699 seconds of thread CPU: mean 58.718 ms and maximum
119.127 ms per process/thread scan. This overhead remains part of the diagnostic.
The capture identifies substantial main-thread activity but does not attribute
it to specific UI bindings, decoding or another function, nor establish which
work caused a temperature spike.

The guard recorded an 86°C warning and selected power-saver early in the workload;
recovery to balanced mode followed playback. Host polling observed a
maximum of 85°C, one sample at or above 85°C and none at or above 95°C. These
different observation cadences do not establish continuous temperature exposure.
Most of this workload ran in low-power mode, unlike the earlier short passing
trace whose warning occurred near its end. Different thermal histories and
observer costs prevent attributing the changed latency to one factor. No profile
was selected, no cap was raised and the thermal guard stayed unchanged. The
retained 15 W ceiling and normal promoted runtime remain in place.

Preparation passed 64 offline checks and the full file preflight. The original
600-second human window, 540-second owner, recovery/cooldown rules and strict
failure checks were retained. The player binary still comes from source commit
`119a579e4ca717a1d59d7a7a6f9752534d68dcaf`; later intervening changes were
documentation only. No optimization or runtime promotion follows this result.

A separate GNU profiler smoke test failed admission: a function measured by its
own thread CPU clock at 1.078 seconds received only 0.100 seconds of samples,
while the other thread received 1.900 seconds against 1.905 seconds measured.
That profiler is not used to attribute CONFLUX's main-thread cost. Qt's profiler
captured a known JavaScript function in a bounded synthetic fixture, including
early flushing while the target continued. A private copy of the native
diagnostic subsequently captured 886,118 events over 30.062 seconds of QML
activity, with no trace clock-boundary errors.
[Qt's documentation](https://doc.qt.io/qtcreator/creator-qml-performance-monitor.html)
describes these events as execution durations and explicitly excludes C++
profiling; they will not be presented as native thread CPU measurements.

The largest individual binding was the stereo meter's array-valued channel
model: 4,976 evaluations and 2.666 seconds of inclusive wall time. Each peak or
RMS update replaced that model and recreated its channel items. The trace
contained 84,510 QML creation events in total; these include creation phases,
not a count of distinct objects. Binding, signal and JavaScript durations nest
and must not be added together as CPU time. This identifies avoidable UI work,
without proving its share of the earlier chunk stalls.

That profiler run stopped after 17.172 measured seconds. Its window remained
visible, active and exposed, but frame presentation paused for 3.155 seconds
immediately after the thirty-second trace flush. The approximately 8 ms between
flush and last observed frame establishes timing, not causal proof. The original
failure is retained; the visibility guard is unchanged. Five observed chunk
deliveries reached 6,864.849 ms, with one target miss; seek recovery held audio
for 6.960 seconds. Only two of the three submitted manual controls were
acknowledged before the stop. Steady and peer starvation, callback/driver errors
and recording overflow were zero. All 2,995,200 recorded float samples were
finite, with counts and peaks matching native. All 27 known owned identities,
groups, cgroup and idle blockers were gone within 331.492 seconds of the
original readiness. This instrumented, aborted run cannot qualify latency or
the full control workload.

The meter now keeps two channel objects and binds their values independently.
A regression check failed against the original component because its channel
objects were destroyed, then passed with the same test binary after this QML
change. It also checks independent peak/RMS display, overrange clamping and
return to silence. The complete existing offscreen UI suite passed in 154.30
seconds. Layout, accessibility, audio, model quality and thermal protection are
unchanged. The subsequent [meter comparison](meter-stability-2026-10-26.md)
missed the service target with both implementations. The separate
[QML verification record](data/m2-player-qml-profile-2026-10-26/verification.json)
retains the failed run, profiler trace and regression evidence.

The [verification record](data/m2-player-thread-cpu-trace-2026-10-26/verification.json)
binds compressed plans, sources, observations, failed verdicts, analysis and
cleanup to their original hashes. Audio, PCM, models, executables and driver
caches remain private. Sustained service latency, musical analysis and the
remaining M2/M3 release gates are unchanged.

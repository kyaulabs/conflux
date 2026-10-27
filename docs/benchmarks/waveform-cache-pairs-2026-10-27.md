# Waveform cache comparison attempts

Neither attempt reached the waveform-cache candidate, so these runs establish no
cache improvement. The first baseline missed the unchanged 4.095-second target
with a 12.503-second p99. The second completed its measured interval but stopped
during the finite diagnostic drain; its incomplete latency population has no
admissible full-run p99. Both failed attempts remain recorded.

| Observation | First baseline | Corrected-clock baseline |
|---|---:|---:|
| Measured playback | 120.005 s | 120.008 s |
| Service-hop population | 13, complete including final drain | 16 retained, incomplete final drain |
| Nearest-rank service p99 | 12.503377 s | Unavailable |
| Hops at or above 4.095 s | 10 of 13 | Not assessed for the incomplete population |
| Steady stem starvation | 739,328 frames / 15.403 s | 440,576 frames / 9.179 s |
| Seek/replay interval starvation | 2,995,712 frames / 62.411 s | 1,710,592 frames / 35.637 s |
| Incomplete-generation seek recovery, polling upper bound | 30.129 s | 15.769 s |
| Replay recovery, polling upper bounds | 16.360 and 16.108 s | 2.749, 2.746 and 14.555 s |
| Accessible control changes acknowledged | 16/16 | 16/16 |
| Audio callback deadline misses | 1 | 0 |
| Driver position discontinuities / recovery flags | 0 / 0 | 0 / 0 |
| Source read failures / invalid buffers / rejected commands | 0 / 0 / 0 | 0 / 0 / 0 |
| Healthy measured UI observations | 11,002/11,002 | 10,162/10,162 |
| Peak sampled temperature | 71°C | 84°C |
| Recorded stereo float32 frames at 48 kHz | 6,370,560 | 6,288,640 |
| Hard cancellation | Not exercised | Not exercised |
| Waveform-cache candidate | Not dispatched | Not dispatched |

The complete first population includes the three hops retained after the
measurement boundary; none was removed because it arrived late. Its p99 equals
the maximum of this small population. The second population cannot substitute
for a completed run: stopping at starvation can omit the slow final hop itself.
Acknowledged controls do not establish prompt seek recovery or uninterrupted
playback. Both runs used Best-quality Vulkan stems on Deck A and the same quiet
PCM peer on Deck B. The WAVs include setup audio outside the measured interval.

The first WAV passed native inspection. For the second, the final health
exception occurred before the native recorder summary was written. Independent
offline inspection verified its RIFF/data lengths, stereo 48 kHz IEEE float32
format, 6,288,640 frames and finite samples, with peaks below 0.054. The saved
progress reports finalized recording without overflow. These checks do not
establish continuous or musically acceptable playback; starvation remains a
failure. The second drain added 2,816 starved frames after the measured/WAV
boundary and stopped before normal service close. Its observation queue lost
zero records, but the raw service population was unfinished.

Both runs began in the thermal guard's natural power-saver state. Observed
profile and package limits later changed under that existing guard. The first
run ended balanced; the second returned to power-saver. The MMIO long-term cap
remained at or below the approved 15 W ceiling, including its natural 7.5 W
reduction. No profile was selected and no guard, cap or kernel setting was
written. No observed sample reached 85°C; two-second samples cannot exclude
shorter spikes or qualify sustained behavior. These observations do not support
a causal comparison between the runs.

The first pair stopped at a launcher assertion before its candidate could
start. Reprojecting the original UTC age yielded a deadline 0.146667781 seconds
earlier than the shared monotonic deadline. The helper incorrectly required
exact equality. The corrected helper binds the original readiness time and
reply, accepts only a shorter deadline and requires completed cleanup within it.
Its regression replays the actual retained receipts: the original function
rejects them and the corrected function accepts them without altering any
record or renewing the clock. Nine clock checks, 35 combined clock/owner/observer
checks, 84 checks per arm and both real KDE missing-readiness entry paths passed.
The second pair remained gated because its baseline evidence was incomplete;
no candidate dispatch was attempted.

The capture correction allows the same finite generation to finish within its
existing 60-second drain and three-second close limits after recording an audio
health failure. Final success still requires zero tail starvation and zero
callback deadline misses. It also retains the finalized WAV inspection before
rejecting tail health. Cancellation, a stalled output, changed owner or source,
window policy failure and the original deadlines still stop the run. This
correction improves evidence capture; it does not improve stem latency or waive
an audio failure. The corrected harness builds; the existing generated IPC/bridge suite passed
165 cases, including delayed final ACK/drain, shortage recovery, cancellation and
timeout cleanup. All 33 runner checks also passed. These tests use generated
service fixtures and do not establish the corrected harness's real-player
latency or capture behavior; its laptop verification is still pending.

Independent closure verified both original unit/cgroup lifetimes, PipeWire
resources and temporary KDE inhibitions were gone. Frozen source/build/launcher
pins matched, and closure stayed within each original 20-minute authorization.
No automatic retry or renewed readiness was used within either pair.

The [compressed text evidence](data/waveform-cache-pairs-2026-10-27/manifest.json)
contains both original plans, raw observations, native/wrapper assessments,
clock regression and independent cleanup/audio checks. It excludes source media,
PCM, shader caches, models and executables. M2/M3, Best quality, the 4.095-second
target, live cancellation and the 60-minute actual-player soak remain open.

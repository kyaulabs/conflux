# 25 W Performance-start player check

The Best-quality baseline played for 120.011 measured seconds but failed audio
continuity and final replay recovery. The waveform-cache candidate did not run.
No full-population p99 is admissible: the final recovery was still pending when
the harness stopped, before normal service close. The 4.095-second target,
M2/M3 completion and the first 0.1.0 release remain blocked.

| Observation | Baseline |
|---|---:|
| Measured playback | 120.011 s |
| Retained service hops | 14; final generation incomplete |
| Largest retained service time | 11.354287 s |
| Admissible full-run p99 | Unavailable |
| Steady stem starvation | 841,984 frames / 17.541 s |
| Seek/replay interval starvation | 1,812,992 frames / 37.771 s |
| Incomplete-generation seek recovery, polling upper bound | 8.932 s |
| Completed replay recoveries, polling upper bounds | 11.561 and 11.346 s |
| Final replay | Pending at measurement end |
| Accessible UI changes acknowledged | 16/16 |
| Application callback deadline misses | 0 |
| Maximum callback execution | 0.992 ms |
| Driver discontinuities / recovery flags | 0 / 0 |
| Source read failures / invalid buffers / rejected commands | 0 / 0 / 0 |
| Healthy measured UI observations | 10,392/10,392 |
| Maximum observed frame gap | 82.211 ms |
| Stereo float32 recording at 48 kHz | 6,266,112 frames / 130.544 s |
| Recording overflow | 0 frames |
| Hard cancellation | Not exercised |

The final replay began 113.929 seconds into measurement. Earlier replays had
needed about 11 seconds to recover, and this one remained pending at the
120-second boundary. The harness's completion assertion then retired the live
owner before its finite diagnostic drain. The retained 14-hop summary is an
unfinished subset, even though the wrapper's intermediate service-hop summary
labels it complete. Zero local observation loss cannot establish that the final
service computation finished. There was no `ServiceClosed` record. No late hop
or failed interval is excluded to obtain a passing latency result.

The callback and UI checks passed, but the stem deck exhausted its available
audio. Recording completed and passed native format/count/sample inspection;
it includes setup audio and the failed playback. These facts do not establish
uninterrupted sound. The incomplete-generation seek exercises a superseded
generation, without proving native inference preemption or hard cancellation.

The requested Performance profile reached the authorized 25 W long-term MMIO
ceiling before native launch. Of 66 host point samples, the first showed
Performance/25 W and the remaining 65 showed Power Saver/7.5 W after the
unchanged thermal guard intervened. CPU/ACPI observations peaked at 85°C, with
one sample at or above 85°C and none at or above 95°C. Two-second point samples
cannot establish continuous heat exposure or exclude shorter spikes. This is a
Performance-start run dominated by a guard reduction; it does not measure
sustained 25 W operation or establish a cache improvement.

The user authorized the current-boot Performance ceiling. Its root-owned
transient limiter only lowers Performance PL1 values above 25 W, preserves lower
limits, and leaves the existing thermal guard active. A separate profile-only
check observed five consecutive 25 W readings and restored Balanced afterward.
During player cleanup, lower guard settings were preserved. The limiter remains
installed for this boot; reboot removes it.

Two earlier attempts stopped before any native player, inference or audio.
Packet 03 rejected an observer/host pair of 74°C/63°C before public setup.
Packet 04 selected capped Performance, then rejected 50°C/72°C immediately after
selection. Their baseline scopes were independently closed; neither candidate
started. Packet 05 applied the existing five-second/six-pair heat-only allowance
to all four fresh startup checks. It retained rejected spikes, required both
readings to reach 70°C or less, and kept the original guard, cooldown, power,
native-reserve and 20-minute pair deadlines. Nonthermal faults and persistent
heat still fail. Both 106-check launcher suites, 48 shared regression/entry
checks and both real missing-readiness desktop entries passed before this run.

Independent closure verified the original baseline process/group/cgroup,
PipeWire resources and temporary KDE inhibitions were gone within 286.572
seconds of the original readiness record. The candidate unit was never created.
No automatic retry or renewed clock was used. Codex and ordinary desktop work
could resume after closure.

The capture correction retains the same failed measurement-boundary verdict
while allowing the already bounded finite generation/idle-close drain to
collect its remaining evidence. A later tail completion cannot turn the
unfinished measured replay into a pass. The original audio, window, owner,
source, stop-file and deadline checks remain active. Verification of that
correction is recorded with the compressed evidence; another laptop validation
and the actual-player hour remain outstanding.

The corrected native harness builds, and all 33 runner checks pass, including
rejection of an unfinished recovery. An isolated generated-service regression
holds the third generation for four seconds across a ten-second playback
boundary. The original harness loses its tail; the corrected harness retains
the later hop, raw/converter completion and normal service close. Its measured
recovery remains failed, its native exit stays 1, and the measured/WAV boundaries
do not change. Both private daemon graphs and service owners are reaped. An
initial fixture did not reach the delayed generation and failed to reproduce
the condition; that setup evidence is retained separately. These generated
checks use a virtual audio daemon with no host output, model or GPU inference.

The [compressed text evidence](data/performance25-player-2026-10-27/manifest.json)
contains original plans, observations, assessments, launcher regressions,
power-policy checks and cleanup receipts. It excludes tracks, PCM, shader
caches, models and executables.

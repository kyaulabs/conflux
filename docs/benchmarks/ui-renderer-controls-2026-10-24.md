# Visible player renderer diagnostics

Both 24 October 2026 UTC laptop runs missed the unchanged Best-quality live-stem
service target of p99 < 4,095 ms. The software-renderer run had no steady-playback
starvation, but still paused while recovering from deliberate seeks and replay.
These results do not select a renderer or close M2, M3 or the first-release gate.

| Result | Default UI | Software UI |
|---|---:|---:|
| Measured player duration | 120.004 s | 120.028 s |
| Completed service hops | 17 | 22 |
| Nearest-rank p99 / maximum | 7,941.390 ms | 6,472.509 ms |
| Hops at or above 4,095 ms | 10 | 7 |
| Steady-playback starvation at 48 kHz | 104,704 frames / 2.181 s | 0 |
| Seek/replay recovery starvation | 1,356,032 frames / 28.251 s | 1,117,952 frames / 23.291 s |
| Automated mix controls acknowledged | 16/16 | 16/16 |
| Master recording frames accepted = written | 6,076,160 | 6,344,704 |
| Recording overflow | 0 | 0 |

The [verification record](data/m2-ui-renderers-2026-10-24/verification.json)
includes compressed original results, complete service observations, host
samples, plans, failed assessments and independent recomputation. Recordings
include setup audio; their frame totals are not a 120-second-only measurement.
No music, stems, model or recording payload is copied into Git.

Both runs used source `ae75c1f8f0054ee5f008858039a9f815be5e192c`, the same
optimized player, promoted full-F32 Flash Vulkan worker, Best conversion and two
runtime CPU threads. The software run requested `QT_QUICK_BACKEND=software`;
the stem backend remained Vulkan. AC and open-lid observations passed. The
unchanged thermal guard remained active.

This was a sequential diagnostic, with different completed-hop populations,
replay counts, setup budgets and thermal histories. Default retained one hop
before the first measurement cursor poll and sixteen during measurement.
Software retained two before, nineteen during and one in the diagnostic tail
after recording had ended. Startup and tail hops remain in the strict service
population; missing or superseded work is not counted as fast. The sample sizes
make nearest-rank p99 equal to the maximum. The results cannot establish a
causal speedup from the UI renderer.

Every latency miss contained graph-compute/backend-wait time above 4,095 ms
by itself. The maximum was 7,537.719 ms for default and 5,866.286 ms for software;
graph rebuild was zero throughout. Source reads, upload and readback were much
smaller. Software's first inference also used 5,637.309 ms of process CPU time;
its six later misses had much lower CPU time and graph-compute spans of
4,218.590–5,737.682 ms. These nested wall and CPU measurements do not isolate
GPU execution, queue delay or fence waiting. They support adding request-bound
GPU/submission timing before changing the runtime again.

The deliberate incomplete-generation seek recovered within observed upper
bounds of 5.771 seconds for default and 7.547 seconds for software. Default's
three replay recoveries were 4.250, 10.019 and 8.422 seconds; software's four
were 3.652, 4.458, 3.895 and 4.005 seconds. Both final generations recovered and
closed. One stale packet was discarded in each run: 2,116 frames for default
and 4,096 for software. This proves supersession of obsolete work, not
interruption of an in-flight native/GPU inference.

The peer clock continued without starvation. Both captures reported zero
invalid buffers, rejected commands, source-read failures, callback execution
deadline misses, driver position discontinuities and driver recovery flags.
No scene-graph errors were reported. Service telemetry was complete with all
thirteen phase fields and zero dropped service records. The separate bounded
window-event ring dropped thirteen default and nineteen software auxiliary
events. These checks do not establish physical listening quality, hardware
xrun counts, sample-exact full-output playback or every UI event.

The default package-temperature trace reached 96°C in one sample; software
reached 72°C on that package sensor and 87°C on a selected core sensor. Samples
were about two seconds apart, so neither continuous excursion duration nor
the true peak is established. Both runs transitioned from balanced to
low-power during playback; software returned to balanced in its tail. No
clock trace in these captures identifies the cause or attributes it to the
renderer.

Earlier software launches remain separate failures. One lacked the required
inherited renderer environment and stopped before launching the player. The
corrected entry then exhausted its 210-second admission budget after a guard
warning and recovery, also before playback. The final run prospectively allowed
270 seconds for admission within a 480-second owner and 600-second operator
budget. Its 55°C/60-second cooldown, fresh checks, guard policy, 120-second
workload, Best quality and 4,095 ms target stayed unchanged.

The software native functional result passed, while its public strict service
gate failed. Default failed both steady starvation and the strict service
gate. Both private assessments also retained a secondary `pins_after` KeyError;
separate full-file post-run checks verified unchanged inputs. Actual-host
checks proved all observed owned process groups absent, with clean private
audio cleanup. These additional proofs preserve the failed result flags.

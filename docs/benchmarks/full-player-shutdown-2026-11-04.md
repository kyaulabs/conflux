# Full-player shutdown during an unfinished GPU wait

The laptop completed production window-close and controller shutdown in
**324.103 ms**, within the existing **3,000 ms** limit. This 4 November run
completes the remaining laptop shutdown check for source
`9e35bda2eeb75c54630b392a9b631d3fca212684`. Apollo passed the same boundary
on 31 October at 505.858 ms. These are separate captures, not a simultaneous
or causal comparison.

| Observation | Laptop, 4 November | Apollo, 31 October |
| --- | ---: | ---: |
| Window close to shutdown returned | 324.103 ms | 505.858 ms |
| Controller shutdown interval | 323.332 ms | 504.484 ms |
| Observed unfinished native graph-fence wait at close | Yes | Yes |
| Selected wait completed during cancellation | Yes | Yes |
| Native worker and original descendants reaped | Yes | Yes |
| Native spool removed | Yes | Yes |
| Partial source installed | No | No |
| Fallback process cleanup used | No | No |

The private probe loads the real `Main.qml`, accepts the normal window-close
path, returns from `app.exec()` and calls production controller shutdown while
the first live preparation is still busy. It uses Best-quality weights and
the pinned Vulkan runtime. PID identity, ancestry, pidfd readiness and the
submitted/unreturned native fence marker establish the observed boundary.
Source frames were zero at close and after shutdown. The native completion
marker followed the window-close timestamp in both captures. Neither result
proves preemption of a GPU command or every possible driver-hang state.

Both windows used offscreen software UI and private virtual PipeWire. No audio
was played through the physical device. This check does not qualify musical
quality, service p99, sustained playback or visible Wayland controls. The
original failed laptop attempt remains a failure: its 82 preflight samples
never supplied the required 60-second interval at or below 70°C; its longest
sampled interval was 54.321 seconds, and no player or model started.

The retry moved all mutable work to `/srv`. The player, native runtime, model,
worker/service/exporter and thermal-guard bytes matched their original pins.
The cleared `/tmp` model was restored from the checksum-verified archive and
matched the original 84,059,168-byte weight hash. Python changed from 3.14.8
to 3.14.9; NumPy remained 2.5.4. The new software pins were recorded, with no
model, quality, timeout or temperature-rule substitution.

Before dispatch, 19 admission/ownership checks, five mocked laptop-dispatch
behavior checks and seven CPU-only real-player lifecycle cases passed. The
real unarmed entry also passed. The shorter task-local temporary path keeps
the private manager socket within Linux's pathname limit. Fresh human
readiness created one original five-minute deadline; dispatch reserved one
attempt, allowed no retry and derived the unit lifetime from remaining time.
All input hashes were checked again before launch.

The laptop retained Balanced mode and its existing power caps and thermal
guard. OneDrive was unchanged. Across 34 two-second setup/workload readings,
the maximum was 88°C; one reading reached 85°C or higher and none reached
95°C. This short capture does not establish a sustained thermal result or
exclude unsampled spikes. The original invocation and cgroup were checked
empty, and the completed transient unit was retired. Apollo's earlier owned
container was already absent. Finished fixture inputs, caches and temporary
runtime directories were cleaned up; compact receipts and still-needed inputs
remain under `/srv`.

On 4 November the user chose “Keep full M2/M3 qualification before 0.1.0
(original goal).” Automatic musical-analysis qualification, integrated fallback
qualification and the current-build sustained workload remain open. The strict
Best-quality primary-laptop service target remains p99 below 4.095 seconds.
No milestone, release gate or release scope was relaxed.

The [summary](data/full-player-shutdown-2026-11-04/summary.json) and
[scope decision](data/full-player-shutdown-2026-11-04/release-scope.json) retain
those measured and human verdicts. The
[manifest](data/full-player-shutdown-2026-11-04/manifest.json) binds original
text receipts, the earlier failure and Apollo result, and protocol sources to
byte counts and SHA-256 hashes. No music, model, executable or mutable cache
is included.

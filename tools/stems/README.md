# M0 persistent stem worker

`worker.py` runs one Python 3.11+/NumPy supervisor thread and one persistent
C++ separator. This M0 tool is separate from the Qt application and must stay
outside the audio callback. The benchmark's animated Wayland window and silent
PipeWire probe measure contention; its paced consumer does not play stems.

Use the F16 HTDemucs GGUF, checksum and license notices recorded in the
[initial benchmark report](../../docs/benchmarks/stems-2026-10-16.md).
The native adapter builds against audio.cpp v0.9.2, commit
`0097d365a00b52a6991ad7618f0a04e8d2935e47`. It calls
`HTDemucsPipeline::separate_chunk()` directly. The upstream offline session
does not expose streaming output or Vulkan graph cancellation.

The standalone build requires CMake 3.25+, Ninja, a C/C++ compiler, Vulkan
headers/loader, `glslc` and SPIR-V headers. Keep sources, weights and binaries
outside the repository:

```sh
stem_root=/tmp/conflux-stem-worker
mkdir -p "$stem_root/include/spirv/unified1"
curl -fL https://github.com/0xShug0/audio.cpp/archive/0097d365a00b52a6991ad7618f0a04e8d2935e47.tar.gz \
  -o "$stem_root/audio.cpp.tar.gz"
printf '%s  %s\n' 28310a67e1fc1324a9d10da34e173730695716a94720ee614d4030f86f1b3114 \
  "$stem_root/audio.cpp.tar.gz" | sha256sum -c -
tar -xzf "$stem_root/audio.cpp.tar.gz" -C "$stem_root"
curl -fL https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/04f10f650d514df88b76d25e83db360142c7b174/include/spirv/unified1/spirv.hpp \
  -o "$stem_root/include/spirv/unified1/spirv.hpp"
printf '%s  %s\n' f1c7707b4fa073bb61706eecf3583163651504a3abac17f3e3f2420313082aac \
  "$stem_root/include/spirv/unified1/spirv.hpp" | sha256sum -c -
curl -fL https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/04f10f650d514df88b76d25e83db360142c7b174/LICENSE \
  -o "$stem_root/SPIRV-Headers-LICENSE"
cmake -S tools/runtime -B "$stem_root/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAUDIOCPP_SOURCE_DIR="$stem_root/audio.cpp-0097d365a00b52a6991ad7618f0a04e8d2935e47" \
  -DSPIRV_HEADERS_INCLUDE_DIR="$stem_root/include"
cmake --build "$stem_root/build" --target conflux-stem-runtime --parallel 4
```

The temporary SPIR-V header comes from Khronos tag `vulkan-sdk-1.4.341.0` and
declares MIT licensing. To use an installed header, set
`SPIRV_HEADERS_INCLUDE_DIR` to its include directory. CMake performs no network
downloads and checks the pinned audio.cpp build-interface files.

CMake defaults `CONFLUX_STEM_BLOCKING_WAIT` to `ON`. It verifies the pinned
Vulkan backend SHA-256 and generates a build copy that replaces the final CPU
polling loop with a blocking fence wait. The upstream checkout stays unchanged.
Configure with `-DCONFLUX_STEM_BLOCKING_WAIT=OFF` to build the upstream wait
baseline.

The Vulkan Flash path is disabled by default. To build it against the same
verified source and headers, use a separate build directory:

```sh
cmake -S tools/runtime -B "$stem_root/build-flash-f32" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAUDIOCPP_SOURCE_DIR="$stem_root/audio.cpp-0097d365a00b52a6991ad7618f0a04e8d2935e47" \
  -DSPIRV_HEADERS_INCLUDE_DIR="$stem_root/include" \
  -DCONFLUX_STEM_FLASH_F32=ON
cmake --build "$stem_root/build-flash-f32" --target conflux-stem-runtime --parallel 2
```

This option requires blocking fence waits. It checks the pinned sources and ten
HTDemucs attention-node signatures, then selects the existing scalar shader with
F32 intermediates and accumulation. The F16 model weights stay unchanged. Its
output differs numerically from the default runtime; [M2](../../docs/M2.md)
records the comparisons and remaining qualification.

Import `StemWorker` from `tools.stems.worker`. Sources must be nonempty,
uncompressed stereo 44.1 kHz PCM16 WAV files. Frame positions are source frames.
The supervisor sets `OMP_WAIT_POLICY=PASSIVE` and `GOMP_SPINCOUNT=0` only in the
Vulkan child's environment so idle OpenMP workers sleep during GPU inference.
CPU children inherit the caller's environment. These settings do not change
the desktop or system power policy.

| Method | Behavior |
| --- | --- |
| `load(path, start_frame=0)` | Replace the source and return a new generation. |
| `seek(frame)` | Seek the loaded source and return a new generation. |
| `cancel()` | Invalidate the current generation and clear queued chunks. |
| `poll(timeout=0)` | Return one metadata/chunk event, or `None`. |
| `is_current(event)` | Check whether a held chunk still belongs to the accepted generation and source. |
| `statistics()` | Read queue, cancellation, discard and process counters. |
| `close(timeout=2)` | Stop the supervisor; terminate the child if it has not stopped after the join interval. |

Control methods do not wait for source I/O or inference. Recheck
`is_current(event)` before scheduling a held chunk after a control change.
Chunks carry generation, source/model SHA-256 identities, source frame range
and `samples` shaped `[stem, frame, 2]`. Stem order is `drums`, `bass`, `other`,
`vocals`.

Source preparation hashes the file and computes the whole-source mono sample
mean/std in bounded passes. Unchanged source statistics are reused on seek.
Valid samples are normalized before trailing zero padding. Separation uses
343,980-frame windows (7.8 s), a 257,985-frame hop (5.85 s), and triangular
overlap-add over 85,995 frames (1.95 s). Windows stay on the source grid
`0, HOP, 2*HOP, ...`, including the padded tail. A seek recomputes its predecessor
window when needed, then trims output to the requested frame. This grid differs
from the upstream offline session's centered final window. Output restores the
whole-source std and mean to each stem, matching audio.cpp's normalization.

The private native protocol uses JSONL and F32LE temporary files:
`{"id":1,"input":"/tmp/window.f32","output":"/tmp/stems.f32"}`. Input is one
normalized interleaved stereo window; output is source-major interleaved
stereo, shaped `[4, 343980, 2]`. Responses are `ready` with geometry, PID and
settings, then `result` with ID and timings. The supervisor validates IDs,
shapes and finite samples. Graph timing includes transfers, synchronization
and driver work. Result metadata also reports `getrusage` CPU milliseconds and
voluntary/involuntary context switches around `separate_chunk()`. CPU time
includes all native process threads; it excludes request file I/O and output
packing. The `ready` metadata records the fence wait mode and child OpenMP
environment.

One native request runs at a time; newer controls replace the pending control.
The ready queue defaults to two hops and pauses production when full.
Telemetry retains 256 events and counts dropped entries. Ordinary seeks and
cancellations retain the model. Cancellation invalidates results immediately;
an active Vulkan graph finishes and its output is discarded. A request timeout
terminates the child and reports an error. A later load/seek can restart it.

Build the application harness using [M0.md](../../docs/M0.md), then run from the
repository root with GPU, Wayland and PipeWire access. Use a new or empty output
directory:

```sh
python3 tools/benchmarks/stem_worker.py \
  --runtime "$stem_root/build/conflux-stem-runtime" \
  --model /tmp/conflux-stem-m0/models/htdemucs-f16.gguf \
  --ui build/dev/conflux --audio-probe build/dev/conflux-audio-probe \
  --renderer opengl --hops 60 --threads 4 --quantum 256 \
  --output /tmp/conflux-worker-run
```

The benchmark tools remove their generated fixture WAVs after use and retain
raw reports, generator parameters and source hashes. This keeps repeated runs
from filling scratch storage.

Use `--renderer vulkan` for the other Qt renderer. Results cover synthetic
fixtures, continuous/paced delivery, seeks, cancellation, source replacement,
process memory/CPU and UI/audio reports. They do not establish musical quality,
audible seams, stem playback or the thermal-soak target. Retain cold startup
and latency spikes in the measured report.

For a power-profile comparison, an installed `powerprofilesctl` can hold the
whole machine in `performance` while the benchmark command runs. Check that
this profile is available with `powerprofilesctl list`. Record the active
profile and holds before and after the run:

```sh
powerprofilesctl get
powerprofilesctl list-holds
powerprofilesctl launch --profile performance \
  --reason 'CONFLUX M0 stem benchmark' --appid com.kyaulabs.conflux \
  python3 -B tools/benchmarks/stem_worker.py \
  --runtime "$stem_root/build/conflux-stem-runtime" \
  --model /tmp/conflux-stem-m0/models/htdemucs-f16.gguf \
  --ui build/dev/conflux --audio-probe build/dev/conflux-audio-probe \
  --renderer opengl --hops 60 --threads 4 --quantum 256 \
  --output /tmp/conflux-worker-performance-run
powerprofilesctl get
powerprofilesctl list-holds
```

The hold is released when the command exits; verify the previous profile has
returned. The application and worker do not request power-profile changes.
Results capture AC status, the firmware platform profile, CPU energy-performance
preference and governor at startup and finish. The governor alone does not
describe the active power policy.

Add `--trace-system` to record native task scheduling, CPU/GPU clocks,
temperatures, platform profile, energy-performance preference and available
throttle fields in `native-trace.jsonl`. The trace preserves kernel units. RAPL
energy counters, when readable, are cumulative microjoules rather than watt
readings. Compare scheduling deltas only when task IDs stay unchanged and reads
succeed. A disabled `sched_schedstats` flag means wait counters cannot rule out
scheduling contention. Tracing adds observation overhead.

Use the identical-window profiler to separate graph stalls from changing input
and chunk assembly:

```sh
python3 tools/benchmarks/stem_profile.py \
  --runtime "$stem_root/build/conflux-stem-runtime" \
  --model /tmp/conflux-stem-m0/models/htdemucs-f16.gguf \
  --ui build/dev/conflux --audio-probe build/dev/conflux-audio-probe \
  --renderer opengl --requests 30 --threads 4 --quantum 256 \
  --passive-openmp --output /tmp/conflux-profile-run
```

The profiler always records a system trace and checks that repeated input
produces numerically identical output. Its `--passive-openmp` flag opts into the
worker's Vulkan child policy; without it, the profiler inherits the caller's
OpenMP environment. For an upstream baseline, use the build with blocking waits
disabled, omit this flag and unset caller `OMP_WAIT_POLICY`/`GOMP_SPINCOUNT`.
Profiler round trips diagnose stalls; the chunk benchmark measures delivery
against the 4.095-second p99 target. See the
[latency profiling report](../../docs/benchmarks/latency-2026-10-16.md) for the
experiments and rerun results.

The [follow-up stall trace](../../docs/benchmarks/stall-trace-2026-10-16.md)
includes GPU command-buffer timings, balanced-profile comparisons and the
guarded profile runner used when this daemon rejects a balanced hold.

Run the finite thermal soak with `tools/benchmarks/stem_soak.py`. By default it
reuses one loaded backend and a small repeating fixture, saturates delivery for
the stated wall duration, and checks cancellation/seek parity every five minutes.
The UI and audio probes receive startup/shutdown margin beyond the workload duration.
Their monotonic timestamps verify overlap; an atomic UI heartbeat and final
minute bins reject missing rendering coverage. Brief sampled exposure changes
are retained; a heartbeat, frame or exposure gap over 30 seconds fails the run.

To cycle prepared music files, repeat `--source-wav /absolute/path/to/track.wav`
once per source. Each input must be stereo 44.1 kHz PCM16 WAV with more than
516,107 frames. Files advance after completion and repeat for the stated wall
duration; normalization and parity references remain separate by source hash.
The worker retains one backend. External WAVs are read without modification or
deletion. Source preparation and first-output gaps remain in the event log,
separate from hop-service timings. This exercises repeated excerpt delivery;
the audio probe remains an independent diagnostic stream.

Use `--delivery-mode playback-paced` to consume chunks on a virtual playback
clock with the existing two-hop queue. Deadlines advance by each chunk's actual
frame count across source changes and late arrivals. Initial startup and
explicit cancellation/seek recovery are recorded as separate priming epochs.
Availability misses, consumer scheduling lateness and intentional queue
residence are separate from native hop-service latency. Paced qualification
requires both the unchanged service p99 target and no missed ordinary playback
demands. This mode still uses the independent diagnostic audio stream.

Use `--load none`, `audio`, `both` or `ui` for sustained load isolation; `both`
is the default. Only selected probes start, and only their executable paths are
required. Disabled probes report `not_run` with null coverage measurements.
An isolated condition cannot qualify the full audio/UI soak. Keep worker,
fixture, controls, sampling and power policy fixed when comparing conditions;
disabling a benchmark child does not stop the desktop or PipeWire services.

Use the guard's `--idle-only` option without `--ui-progress` when no UI probe is
selected. This retains the same live desktop idle inhibitor while skipping
window placement. Keep the outer `systemd-inhibit --what=sleep:idle` wrapper in
every condition, and record each guard/profile lifecycle before comparing runs.

On the measured KDE laptop, the benchmark-only `window_guard.py` uses the
installed PyGObject `Gio`/`GLib` session-bus bindings and KWin scripting. It
temporarily keeps only the owned smoke-test window above other windows on all
desktops and holds a desktop idle inhibitor. The wrapper restores/unloads its
window policy and releases the inhibitor on exit. The lifecycle log stays
outside the new soak directory. Inspect that log and the final coverage checks
before accepting a result:

```sh
soak_out="$PWD/benchmarks-local/thermal-soak"
mkdir -p "$PWD/benchmarks-local"
systemd-inhibit --what=sleep:idle --mode=block --who=CONFLUX \
  --why='CONFLUX M0 thermal soak' \
  powerprofilesctl launch --profile performance \
  --reason 'CONFLUX M0 thermal soak' --appid com.kyaulabs.conflux \
  python3 -B tools/benchmarks/window_guard.py \
  --output "$soak_out-guard.json" --ui-progress "$soak_out/ui-progress.json" -- \
  python3 -B tools/benchmarks/stem_soak.py \
  --runtime "$stem_root/build/conflux-stem-runtime" \
  --model /tmp/conflux-stem-m0/models/htdemucs-f16.gguf \
  --ui build/dev/conflux --audio-probe build/dev/conflux-audio-probe \
  --renderer opengl --seconds 3600 --control-interval 300 \
  --request-timeout 30 --sample-interval 0.5 --output "$soak_out"
```

Use the host environment with GPU/Wayland/PipeWire access. This machine's
restricted execution namespace hides the Vulkan device; that failed preflight
is preserved separately. The wrapper is specific to the measured KDE desktop;
the soak's coverage gates still require actual rendering when run directly.
The [thermal report](../../docs/benchmarks/thermal-2026-10-16.md) records the
failed latency target, passing control/audio checks, observation limits and
cleanup evidence. A completed soak does not by itself mean the p99 target passed.

The [balanced-policy comparison](../../docs/benchmarks/policy-2026-10-16.md)
records the second hour and a native replay of its worst window. To replay that
normalized input without the supervisor, audio or UI children, use the frozen
runtime/model hashes required by this diagnostic:

```sh
python3 -B tools/benchmarks/native_replay.py \
  --runtime /tmp/conflux-stem-m0/conflux-stem-runtime-blocking \
  --model /tmp/conflux-stem-m0/models/htdemucs-f16.gguf \
  --window-start-frame 773955 --requests 120 --max-seconds 600 \
  --request-timeout 30 --output /tmp/conflux-native-replay
```

Run from the repository root with host Vulkan access after other benchmark
work stops. The tool uses one persistent Vulkan process with four threads,
blocking fence waits and PASSIVE/zero-spin OpenMP. It records exact source,
normalized input and output hashes, native timings and a 0.5-second host trace.
The post-ready wall budget limits request admission and response waits; setup,
file operations and cleanup are not interrupted by that deadline. The final
report records actual elapsed time and any overrun. The tool changes no power
policy. Record the active profile separately; the measured replay used the
scoped balanced wrapper archived with the report. The generated WAV is deleted,
while normalized `input.f32` stays in the local output directory. Keep it out of
Git. Native round trips diagnose the graph; they do not measure chunk delivery
or establish sustained audio/UI acceptance.

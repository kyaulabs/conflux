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

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

Import `StemWorker` from `tools.stems.worker`. Sources must be nonempty,
uncompressed stereo 44.1 kHz PCM16 WAV files. Frame positions are source frames.

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
and driver work.

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

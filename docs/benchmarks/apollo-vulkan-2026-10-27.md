# Best-quality Vulkan diagnostics on Apollo

Apollo's retained-cache worker reached **0.827 s service p99**, and the two-minute actual-player control diagnostic reached **0.931 s**. Both short samples are below the unchanged **4.095 s** target. The player used private virtual PipeWire audio and offscreen software Qt rendering. These results do not qualify physical audio, sustained operation or the primary laptop, and do not close M2, M3 or the 0.1.0 release gate.

The isolated environment is `/srv/conflux-tests` on `apollo.kyaulabs`: eight virtual CPUs, about 16 GiB RAM and an NVIDIA GTX 1660 SUPER with 6 GiB VRAM, driver 615.78.08. NVIDIA CDI exposes the GPU to a separate Docker/containerd instance. The default Docker store and every saved Kairos/Hexforged container identity, state, start time and restart count were unchanged after testing. Host packages and the GPU's existing 130 W limit were unchanged.

The fixed image is `sha256:f413527ef82728332244bec1dab8ea99f62dc678514340e2c569f4ce97bae919`. The runtime was rebuilt for Apollo's exposed CPU from audio.cpp revision `0097d365a00b52a6991ad7618f0a04e8d2935e47`, with blocking waits, two CPU threads, Vulkan and full-F32 Flash. The unchanged HTDemucs F16 model is `f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6`. Model and computation precision match the accepted Best configuration.

| Observation | Retained-cache worker | Private actual player |
|---|---:|---:|
| Measured interval | 17 contiguous output chunks | 120.009 s |
| Service p99 / maximum | 0.826682 s | 0.931207 s; 22 validated hops |
| First delivered worker chunk | 2.332436 s | Not measured by this worker harness |
| Load to first observed cursor advance, polling upper bound | Not measured | 10.405233 s |
| Forward/backward seek delivery | 1.550496 / 1.487815 s | One incomplete-generation seek recovered in 3.238339 s |
| Replay recovery, polling upper bounds | Not exercised | 2.753050–2.854917 s; four replays |
| Steady starvation | No late paced-consumer chunks | 0 frames |
| Seek/replay interval starvation | No audio routing | 679,936 frames / 14.165 s |
| Application callback deadline misses | No audio routing | 0 |
| Callback p99.9 histogram upper bound / maximum | No audio routing | 0.259 / 0.468593 ms |
| UI changes acknowledged | No UI | 16/16; gains, crossfader, EQ and CUE selection |
| Invalid buffers / source-read failures | 0 stale chunks delivered | 0 / 0 |
| GPU peak observed temperature | Recorded in raw samples | 55°C |

All 17 worker chunks matched the uncached NVIDIA reference byte for byte, including the first and final partial chunk. Same-grid seeks matched exactly. In-flight cancellation acknowledged in 0.041309 ms, with 330.793367 ms of residual request work; backpressured cancellation acknowledged in 0.137853 ms. Hard shutdown took 211.034855 ms. One persistent backend served the worker run, with no stale deliveries or telemetry loss. New-source replacement took 5.466321 s; it is retained separately from hop service and seek timings. These observations do not establish native inference preemption.

The player exercised one live stem deck beside a finite stereo peer. It rendered 8,455 Qt frames, completed every control and recovery, and finalized a stereo float32 48 kHz WAV containing 6,262,016 frames, including setup. Recording counts matched, with no overflow or nonfinite samples. The measured/WAV boundaries stayed fixed during the finite tail; raw output, conversion, render EOF and normal service close completed with no observation loss. This is numerical and functional evidence, not a listening verdict. The 14.165 seconds of control buffering remain part of the result; it does not imply gapless seeks or replays.

The 22-hop population includes every validated hop through final service close, without warmup or late-hop exclusions. Its separate worker-ready-to-service-pre-emit p99 was **4.582661 s**; packet-ready-to-publication p99 was **58.782041 ms**, with a **78.483099 ms** maximum. Those intervals include different buffering/transport stages and are not additive across clock domains. A passing short service-hop sample does not qualify total startup, every queue delay or sustained end-to-end delivery.

The original startup-inclusive worker sample failed at 16.742505 s p99. Merely retaining HOME produced no driver-cache files and did not remove the delay. Explicitly enabling a writable NVIDIA driver cache produced two cache files. Two independent fresh/retained pairs changed the first identical request from **17.466214 to 0.714171 s** and **17.271500 to 0.699266 s**, with identical Best output bytes. Empty-cache startup remains slow; the passing worker/player samples used a verified copy of that retained cache. The original cache seed was left unchanged.

Each container receives these settings and a dedicated writable bind mount:

```text
__GL_SHADER_DISK_CACHE=1
__GL_SHADER_DISK_CACHE_PATH=/home/conflux/driver-cache
```

Use the private daemon explicitly; do not use the default Docker store:

```sh
sudo -n env DOCKER_CONFIG=/srv/conflux-tests/docker-client \
  docker --host unix:///srv/conflux-tests/daemon/docker.sock ps -a
```

The `--private-live-stems-controls` benchmark mode requires an owned private PipeWire socket, offscreen/minimal Qt and exactly 120 seconds. It uses the normal control schedule while retaining `functional_only=true`, `host_audio=false` and `qualified=false`. Existing ten-second fixture and host qualification bounds are unchanged. Eight executable boundary checks, the existing 33 runner tests and the original generated 16-control sequence passed.

Failed setup attempts are retained: the original 90-second UI wrapper, exact floating-point annotation assertion, sink-node identity lookup, copied stale object file and two regression-wrapper command errors. The ten-second real-model check also remains failed because its last replay crossed the measurement boundary and its tail starved. The later passing diagnostic does not replace those results. Original headless checks passed 59 deck-controller and 232 UI results; those remain separate from hardware acceptance.

The [text evidence manifest](data/apollo-vulkan-2026-10-27/manifest.json) binds source overlays, binaries, complete observations, commands, package versions, negative results and cleanup checks. It excludes models, audio, executables and generated caches. Both original evidence archives remain on Apollo. The source overlay changes only benchmark dispatch/scheduling, not the player, model, audio engine or stem algorithms.

On the user's later request, the laptop's temporary 25 W Performance service and `/run` script were removed. Balanced read back at 15 W, and the original thermal guard remained active with unchanged script/unit hashes. Its unarmed follow-up packet was retired. Apollo remains a secondary test machine; no remote processing requirement or laptop qualification follows from this record.

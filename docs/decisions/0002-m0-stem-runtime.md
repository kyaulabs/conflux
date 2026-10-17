# M0 stem runtime decision

Conditionally selected for local-file preparation and continued bounded-worker development,
17 October 2026. Use the pinned HTDemucs F16 model through audio.cpp v0.9.2's
native Vulkan backend on the laptop. Keep CPU execution as a numerical
reference. The user accepts the auditioned material as a starting basis
for musical feasibility; this decision does not qualify production stem
playback or live headroom.
M0 remains open and M1 has not started.

The runtime is audio.cpp commit
`0097d365a00b52a6991ad7618f0a04e8d2935e47`, built with the existing blocking
fence wait. The measured binary SHA-256 is
`8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6`.
The HTDemucs GGUF SHA-256 is
`f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6`.
The [initial feasibility report](../benchmarks/stems-2026-10-16.md) records the
checkpoint provenance and license evidence: Apache-2.0 for audio.cpp, MIT for
bundled ggml and the HTDemucs weights. Existing notices remain applicable.
This decision adds no production dependency and packages no weights or music.

The [music assessment](../benchmarks/stem-quality-2026-10-17.md) compares the
same source files, model, window grid and worker on CPU and Vulkan. Both
produced complete, finite outputs on the same source grid for three permitted
23.4-second excerpts. All twelve bounded CPU/Vulkan correlation peaks were
at zero lag, and three Vulkan seek suffixes matched continuous output exactly.
These checks support the implementation boundary. They do not establish
source separation quality, phase audibility or acceptable leakage. The user
reported intermittent cutouts in all three assembled previews. Decoding
verified the declared sections and gaps; raw CPU/Vulkan analysis found no
all-output zero frames or 20 ms collapse below −60 dBFS while source RMS was
at least −40 dBFS. Quiet individual stems remain, and these checks cannot
exclude partial damage or missing content. Continuous stem files and shared-
scale waveform plots support further listening. The original preview
feedback alone established neither musical acceptance nor a player/device
explanation.

The [four user-track follow-up](../benchmarks/user-tracks-2026-10-17.md)
retains the same producer artifacts and adds sixteen continuous hops per
backend. Vulkan p99/maximum was 5.258 seconds with 3/16 target misses; CPU
was 22.505 seconds with 16/16. All sixteen CPU/Vulkan stem lag peaks were
zero and three completed-source Vulkan seek suffixes matched exactly.
The user accepted the auditioned material from the separate continuous files
as a starting basis for musical feasibility. The [listening record](../benchmarks/data/user-tracks-2026-10-17-listening.json)
preserves the exact statement and scope without changing the execution-time
archive. This adds no active-cancellation, production playback or latency
acceptance claim. The conditional preparation choice and open M0 status
remain unchanged.

Vulkan is the useful preparation candidate because it executes the pinned
graph on Iris Xe, retains one backend across controls and has substantially
shorter measured requests than CPU. Its twelve continuous music hops had
4.274-second p99/maximum, exceeding the 4.095-second target. CPU p99/maximum
was 20.622 seconds. With twelve samples, p95/p99 are observed maxima rather
than sustained tail estimates. The earlier software-UI synthetic hour reached
3.971-second p99, but that separate workload does not qualify the music runs
or production playback. The live target remains unmet for these music
measurements even without concurrent benchmark UI/audio load.
Both sequential runs spent most sampled time in naturally selected low-power
policy. No profile was changed by the runner; fixed order and policy/cache
history prevent a causal backend or genre comparison.

| Candidate | Decision and evidence limit |
|---|---|
| HTDemucs/audio.cpp Vulkan | Conditional preparation candidate for continued bounded-worker development. Musical feasibility accepted as a starting basis; useful live headroom and production integration remain unqualified. |
| Same model/runtime on CPU | Retained as the numerical reference. Its measured request times do not support treating it as a live fallback. |
| ncnn or native ONNX Runtime WebGPU | Not exported or benchmarked. No comparative quality, operator coverage or performance claim is made. Revisit if the selected pair cannot satisfy the required workload or a better-supported model is needed. |

The supported experimental boundary is one persistent separator for local
stereo 44.1 kHz PCM16 files. It emits `drums`, `bass`, `other`, `vocals` in
sample alignment, using 7.8-second windows, 5.85-second hops and 1.95-second
triangular overlap. First output needs the first complete window; whole-source
hashing and normalization statistics are prepared before it. This is a
read-ahead file worker, not separation of an unknown live input. The ready
queue holds at most two hops. Seeks invalidate a generation immediately and
recompute preceding context; active GPU work drains before replacement work.
Hardware GPU preemption was not demonstrated. Keep inference outside the
audio callback and preserve source/model/settings identities with cached output.

The inspected pinned graph executes without an unsupported-operator failure
or automatic CPU operator fallback. Host normalization, transforms and
reconstruction are explicit pipeline stages. This is evidence for this exact
artifact, not general model/export compatibility. No four-separator workload,
full deck engine, audible stem playback or physical latency has been qualified.
The software UI used in the successful synthetic soak is an experimental
condition; it does not change the application's default renderer.

Retain the [small shared audio-core decision](0001-m0-engine-boundary.md).
Before closing M0, address the failed music delivery target and preserve the
scope of the user's acceptance as a starting basis. Live local-file
separation is a product requirement; this preparation choice does not silently remove it or
authorize remote offload. Further model/chunk/runtime work must address the
failed music headroom result and the supported workload. M1 work has not begun.

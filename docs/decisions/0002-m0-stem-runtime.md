# M0 stem runtime decision

Selected for local-file preparation and continued bounded-worker development,
17 October 2026. Use the pinned HTDemucs F16 model through audio.cpp v0.9.2's
native Vulkan backend on the laptop. Keep CPU execution as a numerical
reference. The user accepts the auditioned material as a starting basis
for musical feasibility; this decision does not qualify production stem
playback or live headroom. On 17 October 2026 the user approved closing M0
feasibility and entering M1 manual audio. The failed **4.095-second live-stem
service p99 target remains a mandatory M2 blocker before a first usable
release**. Prepared stems and on-demand local-file separation remain product
requirements.

The measurements below retain their status when reported. Earlier open-M0
statements are historical; the approved decision here and at the end is current.

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

The [latency profile](../benchmarks/music-profile-2026-10-17.md) subsequently
recorded a 2.161-second continuous-hop p99/maximum in a short rerun of the same
four tracks, with zero misses across sixteen hops. All four output-file hashes
matched the earlier Vulkan arrays. A fixed-window comparison reached
2.346-second music and 2.082-second synthetic native p99/maximum. The earlier
misses coincided with low sampled GPU clocks after a guard warning and policy
transition; input order and machine history remain confounded. No runtime or
model change was needed for the short rerun. This adds no sustained music under
audio/UI load, active-cancellation or production-playback qualification, so the
conditional choice and open M0 status remain.

The [sustained music/audio/UI hour](../benchmarks/music-load-2026-10-17.md)
then reached 4.266-second continuous-hop p99, maximum 5.008 seconds, with
23/1,533 misses against 4.095 seconds. All eleven active cancellation/seek
checks passed with exact trimmed output and no stale delivery; all six
independent silent-audio fault counters were zero and UI coverage passed.
Every continuous source-grid hash matched the accepted listening material.
All 7,069 native policy samples were balanced, sampled package peak was 79°C,
and no guard warning occurred within the measured hour. Native graph
execution/wait occupied 93.5–97.3% of miss service, with sparse low-frequency
GPU observations that do not prove cause. The preparation choice remains
conditional: sustained music headroom is still unmet, M0 remains open and
M1 has not started.

The [playback-paced validation](../benchmarks/paced-music-2026-10-17.md)
then kept all 101 ordinary virtual demands ready on time over 600 seconds,
while continuous service p99 reached 6.822 seconds, maximum 7.446 seconds,
with 21/102 target misses. The active cancellation/seek check, exact output
identity and independent audio/UI coverage passed. All misses had low-power
interior samples following guard policy changes; passing hops also used that
policy. Brief sampled heat adds no failure gate. The service target failed,
so the conditional hour was withheld. Short-run buffered readiness does not
qualify sustained headroom or production stem playback; the conditional
preparation choice and open M0 status remain.

The [two-thread validation](../benchmarks/two-thread-music-2026-10-17.md)
changed only the native thread setting. Its 600-second continuous p99 was
5.172 seconds, maximum 5.398 seconds, with 8/102 target misses. All 101
ordinary virtual demands were ready on time; cancellation/seek, exact output
identity and independent audio/UI coverage passed. Guard policy transitions
still occurred, including a warning just before the first timed request.
This lower sequential result proves neither a thermal fix nor sufficient
headroom; the conditional hour was withheld. No producer source, binary or
default changed. The preparation choice remains conditional and M0 stays open.

The [startup investigation](../benchmarks/startup-profile-2026-10-17.md)
did not reproduce the earlier 95°C transition. Native construction to readiness
took 0.367 seconds and first-hop service took 1.879 seconds. The unchanged guard
logged no warning during that diagnostic. A subsequent two-thread repeat was
stopped during readiness when the user approved M1 entry; no measured workload
started. Readiness included a 92°C warning without a native worker, so native
startup is not necessary for a warning. The cause remains unresolved, and no
thermal fix or sustained headroom pass follows from these observations.

Vulkan is the useful preparation candidate because it executes the pinned
graph on Iris Xe, retains one backend across controls and has substantially
shorter measured requests than CPU. In the initial three-excerpt assessment,
its twelve continuous music hops had
4.274-second p99/maximum, exceeding the 4.095-second target. CPU p99/maximum
was 20.622 seconds. With twelve samples, p95/p99 are observed maxima rather
than sustained tail estimates. The earlier software-UI synthetic hour reached
3.971-second p99, but that separate workload does not qualify the music runs
or production playback. The live target remains unmet for these music
measurements even without concurrent benchmark UI/audio load.
Those earlier sequential runs spent most sampled time in naturally selected low-power
policy. No profile was changed by the runner; fixed order and policy/cache
history prevent a causal backend or genre comparison.

| Candidate | Decision and evidence limit |
|---|---|
| HTDemucs/audio.cpp Vulkan | Selected for preparation and continued bounded-worker development. Musical feasibility accepted as a starting basis; failed live headroom remains an M2 release blocker and production integration remains unqualified. |
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
The user approved M0 feasibility closure with the failed live target explicitly
carried into M2. This replaces the later requirement to pass sustained music
headroom before M1; it follows the original feasibility exit requiring a
reasoned runtime choice and reported failed live targets. The measured hour
still has 4.2655-second p99 with 23/1,533 misses. The latest completed two-thread
validation still has 5.1722-second p99 with 8/102 misses, despite all 101 ordinary
virtual demands being ready. Their eleven and one active cancellation/seek
checks passed exactly, and both recorded all six independent audio fault
counters at zero.

M1 manual audio is next. It begins with local-file decoding outside the callback
and the shared live/offline render core. M2 must address the failed 4.095-second
service p99 target, sustained workload, stem delivery and approved fallback
behavior before the first usable release. Preserve the user's listening
acceptance as a starting basis. This decision approves neither production stem
playback, a CPU live fallback nor remote offload, and it does not mark M1 complete.

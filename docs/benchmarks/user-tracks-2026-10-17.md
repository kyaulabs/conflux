# M0 assessment of four user tracks — 17 October 2026

The pinned HTDemucs/audio.cpp worker completed CPU and Vulkan separation of
four user-provided excerpts. Vulkan's sixteen continuous hops had
5.258-second p99/maximum against the 4.095-second target, with 3/16 misses;
CPU reached 22.505 seconds, with 16/16 misses. Saved outputs passed geometry,
finite-value and same-grid seek checks. The user accepted the auditioned material
as **a starting basis for musical feasibility**. The
[listening record](data/user-tracks-2026-10-17-listening.json) preserves that
scope separately from the earlier benchmark evidence. Vulkan remains the
[conditional preparation candidate](../decisions/0002-m0-stem-runtime.md).
M0 is open and M1 has not started.

The filenames below are labels supplied by the user. Artist/title metadata
was not independently verified, and no redistribution license or isolated
reference stems were supplied. Original FLACs were unchanged. No audio was
downloaded or added to Git.

| ID | User-provided filename | Preparation gain |
|---|---|---:|
| `club_xl` | `Excision & Level Up - Club XL ｜ Subsidia.flac` | 0.787768 |
| `when_the_light_breaks` | `Said The Sky, ILLENIUM, Heather Sommer - When The Light Breaks (Lyric Video).flac` | 0.915314 |
| `under_my_skin` | `SLANDER, Spiritbox, Vastive - Under My Skin.flac` | 0.828490 |
| `stfu` | `T-Pain, DJ DIESEL, San Holo, NGHTMRE, Kompany, SampliFire, IVORY - STFU [Dubstep ｜ Monstercat].flac` | 1.000000 |

All originals were stereo 48 kHz, 24-bit FLAC. The excerpt plan was saved
before decoding: convert with installed ffmpeg to stereo 44.1 kHz float32,
then retain frames `[1323000, 2354940)`, or source 30.0–53.4 seconds. Each
prepared input has 1,031,940 frames, 23.4 seconds and four full hops. One
stereo gain of `0.95 / peak` was applied only when the decoded excerpt peak
exceeded 1.0; quantization used
`numpy.rint(float64(decoded_float32) * gain * 32767)` to PCM16, rejecting
clipping. Excerpt decoding, conversion and WAV verification took
0.112–0.202 seconds per excerpt, outside worker timings. No full-track decoded copies or alternate
listening excerpts were prepared.

Source HEAD was `f4dc5eef82ee42b7ebe052ab449780fd4c0a2e02`. The five
producer/runtime/model hashes matched the
[previous music assessment](stem-quality-2026-10-17.md): audio.cpp v0.9.2,
the blocking-wait native binary, HTDemucs F16 GGUF, worker and shared
benchmark/sampler sources. The unchanged driver requested four threads and
a 120-second request timeout. Each backend retained one native process;
CPU ran first, followed by Vulkan. Normal readiness and fresh AC/open-lid
checks preceded both workloads. The runner did not change host settings;
saved-output analysis ran after the benchmark guard finished.

The fixed worker still uses 7.8-second windows, 5.85-second hops, 1.95-second
triangular overlap, whole-source statistics and a zero-padded final window.
CPU expands the same model weights to F32; Vulkan uses native storage. This
compares execution of the same graph, rather than an independent separator
or isolated-stem ground truth. It is local-file preparation with read-ahead,
not an unknown live-input test. No concurrent benchmark audio/UI load or
new active-cancellation scenario ran.

| Excerpt ID | First delivered CPU / Vulkan (s) | Continuous maximum CPU / Vulkan (s) |
|---|---:|---:|
| `club_xl` | 15.244 / 2.404 | 19.729 / 1.873 |
| `when_the_light_breaks` | 21.879 / 2.169 | 22.505 / 5.258 |
| `under_my_skin` | 8.099 / 4.107 | 8.133 / 4.013 |
| `stfu` | 8.997 / 4.011 | 8.950 / 4.380 |

First delivery includes source preparation, polling and cold backend loading
on `club_xl`; later tracks reuse the backend. WAV preparation took
22.741–77.314 ms on CPU and 21.459–72.265 ms on Vulkan. Pooled continuous
p50/p95/p99 were CPU 8.778/22.505/22.505 seconds and Vulkan
3.926/5.258/5.258 seconds. With sixteen samples, nearest-rank p95/p99 equal
the observed maximum, not a sustained tail estimate. Vulkan's three misses
were 4.866 and 5.258 seconds on `when_the_light_breaks`, and 4.380 seconds
on `stfu`. Including eight delivered seek-suffix hops gives 24 Vulkan hops,
p95 4.866 seconds and p99/maximum 5.258 seconds. The earlier synthetic
software-UI hour's 3.971-second p99 remains separate evidence and does not
qualify these music results.

After continuous `club_xl` output completed, Vulkan sought to frames 257,984,
343,981 and 1,027,844. All three suffixes matched continuous output
sample-exactly, with zero RMS/maximum error. First-ready times were 1.825,
3.619 and 3.878 seconds. These checks do not exercise interruption of an
active graph or demonstrate hardware preemption, audio callback behavior,
physical audio latency or production stem playback.

All eight arrays had complete shape `[4, 1031940, 2]`, dtype float32 and
finite values. All sixteen bounded CPU/Vulkan correlation peaks were at zero
lag. Unshifted per-stem RMS differences were 0.236–1.527% of CPU stem RMS;
no time or gain fitting was applied. Vulkan sum-of-stems minus source RMS
was 3.660%, 2.102%, 3.324% and 2.380% of source RMS in manifest order.
Recombination includes model behavior and the worker's per-stem global-mean
restoration convention; neither comparison is a separation-quality score.
Raw results also retain absolute errors, bias, ±256-sample lag diagnostics
and adjacent-sample/local-derivative measurements at all 24 interior hop
and overlap-exit locations. Nine deterministic hand checks passed before
saved-output analysis.

There were no all-stem or recombined exact-zero frames on either backend.
In 20 ms bins where source RMS was at least −40 dBFS, neither all four stems
nor their sum fell below −60 dBFS. That covers 1,170 source-active bins per
clip except `stfu`, which had 1,166. Individual quiet spans remain: Vulkan
`when_the_light_breaks` bass at 0.02–3.12 seconds, `club_xl` vocals at
10.52–13.30 seconds and `stfu` drums at 8.80–9.88 seconds. Times refer to
the prepared clip; add 30 seconds for the original timeline. These are
descriptive thresholds. They cannot exclude shorter/partial damage or show
whether quiet output reflects absent instruments, routing or lost content.

The new listening set contains 24 separate 23.4-second float32 WAVs: source,
four Vulkan stems and recombination for each track. There are no synthetic
switches, inserted gaps, fades, loops or time shifts. All WAVs decoded
sample-exactly. One common listening gain applies to all six signals per
track: 0.976113094 for `when_the_light_breaks`, 1.0 for the others. These
gains are separate from source preparation. The shared-scale stereo plots
for [Club XL](data/user-tracks-2026-10-17/analysis/club_xl-continuous-waveforms.png),
[When The Light Breaks](data/user-tracks-2026-10-17/analysis/when_the_light_breaks-continuous-waveforms.png),
[Under My Skin](data/user-tracks-2026-10-17/analysis/under_my_skin-continuous-waveforms.png)
and [STFU](data/user-tracks-2026-10-17/analysis/stfu-continuous-waveforms.png)
use one ±1 scale without per-stem normalization. They cannot establish
audible seams, intelligibility, bleed or stereo acceptability. No player or
device output was captured. After confirming content from all stems, the
user reported, "everything sounds good for a starting basis." This accepts
the auditioned material for initial musical feasibility. The statement does not
specify listening hardware or establish artifact-free separation, physical
audio performance or the unmet latency target.
A local HTML page provides audio controls and embedded waveforms after the
Markdown viewer treated WAV embeds as images. Browser checks loaded all
24 audio files and four plots; that does not establish audible quality.

Policy changed naturally during collection: CPU had 166 balanced/237
low-power snapshots; Vulkan had 47/97. Fixed order, caches, background load
and differing policy histories prevent causal backend or track conclusions.
CPU package temperature peaked at 96°C, with six ≥85°C observations in two
episodes. Their hot-sample spans were approximately 1.011 seconds,
bracketed by cooler observations 2.023 and 2.024 seconds apart; only one sample
was ≥95°C. Vulkan package temperature peaked at 78°C with no ≥85°C samples.
TCPU peaks were 65.05/72.05°C. Maximum observation gaps were 0.524/0.516
seconds across 403/144 native samples, including seeks where applicable.
These brief sampled spikes do not establish continuous hot duration or a
thermal cause for latency. Other sensors had only start/end snapshots.

Native VmHWM was CPU/Vulkan 934,436/280,696 kB and supervisor VmHWM
143,864/192,576 kB, with zero sampled swap. Vulkan DRM resident-system
maximum was separately 535,780 KiB; these maxima cannot be added into total
memory. Four RAPL fields were permission-denied on every native snapshot,
so energy/power remain unknown. Native fd inspection had no errors;
supervisor fd inspection recorded 412/150. Both native workers closed, the
sequence and guard returned zero, and the idle inhibit was released. This
short assessment adds no sustained music, audio/UI or thermal-soak pass.

The [evidence manifest](data/user-tracks-2026-10-17/archive-manifest.json)
covers 37 files, including six compressed JSONL streams with 686 records,
executed helpers, safe corpus facts, numerical results and four plots.
A separate audit of the published copy passed saved-event control/currentness,
coverage, timing and thermal reductions plus source, input and image identities.
Eight exact references retain the unchanged producer/build sources from the
previous archive. Original media, arrays, full tags and the local listening
page are excluded. The archive cannot replay excluded PCM calculations or
verify musical acceptance. The later user verdict is recorded in the linked
sidecar without changing this execution-time archive. Benchmarking remains
stopped; the local playback server has no sleep or idle inhibitor.

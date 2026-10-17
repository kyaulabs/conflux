# M0 music assessment and runtime choice — 17 October 2026

The pinned HTDemucs/audio.cpp Vulkan worker produced complete, finite stems
for three permitted music excerpts and reproduced three seek suffixes exactly.
The user reported intermittent cutouts in the assembled listening previews;
musical acceptance and the listening issue remain **open**.
Vulkan's twelve continuous music hops had a 4.274-second p99/maximum, above
the 4.095-second target; CPU reached 20.622 seconds. The
[runtime decision](../decisions/0002-m0-stem-runtime.md) conditionally selects
Vulkan for local-file preparation and continued bounded-worker development. Production
stem playback and live headroom remain unqualified. M0 is open; M1 has not
started.

Each input is the 30.0–53.4-second excerpt of its original full mix: 23.4
seconds, 1,031,940 frames, stereo 44.1 kHz PCM16. Source MP3s were decoded and
resampled with installed ffmpeg, cut on the sample grid, given one common
stereo gain when the decoded excerpt exceeded full scale, then quantized with
`numpy.rint(samples * 32767)`. Downloaded source files totalled 19,025,996
bytes. Credits, source and prepared-file hashes, commands and annotations are
retained in the evidence. No isolated reference stems were supplied.

| Music and credit | Recorded license | Structured coverage | Preparation gain |
|---|---|---|---:|
| [The Parting Glass](https://ccmixter.org/files/doxent/56347), Doxent Zsigmond; traditional music/lyrics, Doxent vocals and piano | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) | Acoustic/traditional, piano, male vocals, rubato | 1.000000 |
| [Funkorama](https://incompetech.com/music/royalty-free/index.html?isrc=USUAN1100474), Kevin MacLeod | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | Funk, 101 BPM; guitar, bass, drum kit, organ, synth | 0.941085 |
| [Club Diver](https://incompetech.com/music/royalty-free/index.html?isrc=USUAN1200020), Kevin MacLeod | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | Electronica, 140 BPM; synths, percussion, drum kit | 0.936057 |

This is three short excerpts with one vocalist, rather than broad musical
coverage. The full-mix sources cannot establish stem accuracy. Source,
prepared audio, stems, previews and model weights remain outside Git.

CPU and Vulkan used the same pinned worker, audio.cpp v0.9.2 commit
`0097d365a00b52a6991ad7618f0a04e8d2935e47`, blocking-wait native binary
SHA-256 `8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6`
and HTDemucs F16 GGUF
`f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6`.
The [initial report](stems-2026-10-16.md) records model and license provenance.
Each backend retained one native process across its clips; CPU ran first,
then Vulkan, with four requested threads and no benchmark UI/audio load.
CPU expanded weights to F32; Vulkan used native storage. The worker used
7.8-second windows, 5.85-second hops, 1.95-second triangular overlap,
whole-source normalization and a zero-padded final source-grid window. These
tail-grid rules differ from the earlier offline centered-tail comparison;
normalization mirrors the pinned offline session. Vulkan retained the
existing passive OpenMP settings; no
production source or host power setting changed.

The timing table separates first delivered output from continuous hop
service. First delivery includes cold model loading on the first clip,
source preparation and delivery polling; later clips reuse the backend.
Hop service measures the native request/reconstruction path recorded by the
worker. Each clip contributes only four continuous hops.

| Excerpt | First delivered CPU / Vulkan (s) | Continuous maximum CPU / Vulkan (s) |
|---|---:|---:|
| The Parting Glass | 19.926 / 2.350 | 20.067 / 2.058 |
| Funkorama | 19.008 / 4.365 | 20.622 / 4.274 |
| Club Diver | 20.626 / 3.657 | 20.540 / 4.064 |
| Pooled 12 continuous hops | — | 20.622 / 4.274 |

Pooled CPU p50/p95/p99 were 19.331/20.622/20.622 seconds, with 12/12 hops
over 4.095 seconds. Vulkan was 3.515/4.274/4.274 seconds, with 1/12 over.
With twelve samples, nearest-rank p95 and p99 equal the observed maximum;
they are not sustained tail estimates. Source hashing/statistics preparation
took 21.834–64.113 ms on CPU and 22.421–64.118 ms on Vulkan. Full MP3
decode/resample wall time was not recorded. The separate
[synthetic software-UI hour](software-ui-2026-10-17.md) reached 3.971-second
p99; it does not qualify music performance or stem playback.

Both runs were admitted on AC with the lid open and balanced policy, but
policy changed naturally during collection. CPU snapshots recorded 7
balanced/441 low-power observations; Vulkan recorded 4/128. The runner made
no profile mutation. Fixed order, caches, background activity and changing
policy prevent causal conclusions about genre or backend speed. The failed
music budget remains the measured result under these conditions.

All six saved arrays had shape `[4, 1031940, 2]`, complete source-grid coverage
and finite values. Raw CPU/Vulkan errors use full-length unshifted samples,
float64 reductions and CPU as reference: RMS is
`sqrt(mean((Vulkan - CPU)^2))`; relative RMS divides by CPU RMS. No gain or
time alignment was fitted. Maximum absolute error and signed bias are also
retained in the raw results.

| Excerpt | Drums RMS error (% of CPU RMS) | Bass | Other | Vocals |
|---|---:|---:|---:|---:|
| The Parting Glass | 0.000054662 (15.256%) | 0.000031153 (13.562%) | 0.000270968 (0.202%) | 0.000240766 (0.263%) |
| Funkorama | 0.000982632 (1.345%) | 0.000849470 (0.786%) | 0.000236456 (0.628%) | 0.000086956 (0.656%) |
| Club Diver | 0.000766245 (0.393%) | 0.000604117 (0.981%) | 0.000502489 (0.903%) | 0.000049835 (3.968%) |

The weak Parting Glass drums/bass have small absolute errors and larger
relative errors. Across all comparisons, maximum absolute error reached
0.034322806. All twelve channel-demeaned FFT correlation peaks within
±256 samples were at zero lag. This checks numerical agreement on this
grid; it supplies neither isolated-stem quality scores nor an audible phase
judgment.

Sum-of-four-stems minus source RMS was CPU/Vulkan 0.002481284/0.002487584
for The Parting Glass, 0.004798620/0.004799537 for Funkorama and
0.011197465/0.011178664 for Club Diver. Vulkan's signed mean residuals were
0.000033989, 0.000062921 and 0.004240987 respectively. These diagnostics
include model behavior and restoring the global source mean to each stem;
they are not separation-quality scores. At all eighteen interior hop and
overlap-exit locations, the analysis retained signed adjacent-sample jumps
and ±20 ms local derivative statistics. Musical transients can dominate
these values; no artifact threshold or audible-seam verdict was assigned.

Three Vulkan seeks on The Parting Glass began at frames 257,984, 343,981 and
1,027,844. Each delivered suffix matched its continuous Vulkan output
sample-exactly, with zero RMS/maximum difference. First-ready times were
2.047, 3.932 and 8.571 seconds; the maximum hop across continuous and seek
work was 4.996 seconds. This is seek parity evidence. It adds no new
cancellation, hardware preemption, audio callback or playback qualification.

The original three lossless float32 WAV previews each last 31.25 seconds:
source, drums, bass, other, vocals, recombined, in six 5-second sections with
0.25-second gaps. Every section uses prepared-clip 3.35–8.35 seconds, or
original-source 33.35–38.35 seconds, covering the first hop and overlap exit.
Only outer 221-frame preview edges have fades; interior joins are untouched.
Common preview gain is 1.0. Decoding verified all eighteen sections bit-exact
against the declared assembly and all fifteen inserted gaps as exact zero.
FFmpeg-decoded MP3 previews had no 20 ms bins falling below −60 dBFS where
the expected assembly was at least −40 dBFS. These saved-file checks capture
no browser/player/device output.

The [original preview layout](data/stem-quality-2026-10-17/waveform-review/original-preview-layout.png)
shows abrupt changes between signals. The Parting Glass drums at preview
5.25–10.25 seconds and bass at 10.50–15.50 seconds are very quiet, with
section RMS −69.184 and −73.278 dBFS. Its vocals are quiet at 22.57–22.79
seconds; Funkorama bass is quiet at 14.09–15.45 seconds. Quiet individual
stems may reflect absent instruments, model routing or separation loss.
Neither the layout nor these measurements establish that deliberate gaps
solely caused the reported cutouts.

Raw CPU/Vulkan outputs had no all-stem or recombined exact-zero frames. In
20 ms bins with source RMS at least −40 dBFS, neither all four stems nor
their sum fell below −60 dBFS. At the lower source-activity threshold of
−60 dBFS, Funkorama crossed the output threshold at 16.98–17.00 seconds.
Its strongest observed recombined/source RMS attenuation was approximately
0.708 at 19.80–19.82 seconds on both backends. Both intervals are outside
the original preview excerpt and do not explain its reported cutouts.
These descriptive thresholds are not quality gates and cannot exclude
shorter or partial dropouts or missing content.

Eighteen separate 23.4-second float32 WAVs now preserve each source, four
Vulkan stems and recombination continuously, with no inserted silence,
fades, cuts or time shifts. All use common gain 1.0 and round-trip exactly.
The shared-scale stereo plots for
[The Parting Glass](data/stem-quality-2026-10-17/waveform-review/parting_glass-continuous-waveforms.png),
[Funkorama](data/stem-quality-2026-10-17/waveform-review/funkorama-continuous-waveforms.png)
and [Club Diver](data/stem-quality-2026-10-17/waveform-review/club_diver-continuous-waveforms.png)
show peaks on one ±1 scale, without per-stem normalization. Human review of
continuous stems, missing content, joins, bleed and stereo acceptability
remains necessary. No musical-quality pass is recorded.

Native VmHWM was CPU/Vulkan 934,624/280,900 kB; supervisor VmHWM was
143,640/193,684 kB, with zero sampled swap. Vulkan DRM resident-system
maximum was separately 535,780 KiB; it must not be added to RSS as total
memory. Supervisor fd inspection recorded 460/137 read errors; native
inspection recorded none. Four RAPL energy fields were permission-denied
on every native snapshot, so energy and power are unknown.

Package peaks were CPU 100°C and Vulkan 90°C. CPU had six observations at
or above 85°C in two episodes; its longest first-to-last hot-sample span was
1.519 seconds, bracketed by below-85°C observations 2.536 seconds apart.
Vulkan had two such observations spanning 0.507 seconds, with a 1.525-second
cool-observation bracket. TCPU peaks were 57.05/64.05°C; maximum sampling
gaps were 0.555/0.542 seconds. These are brief sampled spikes, acceptable
under the user's stated tolerance, rather than measured continuous hot
durations. Other mapped sensors were not sampled through the workload.
Both backends closed, the sequence and guard returned zero, and the idle
inhibit was released. No workload remains running.

The [evidence manifest](data/stem-quality-2026-10-17/archive-manifest.json)
covers sixty files, including six compressed JSONL streams with 693 records,
the executed helpers, producer snapshots, structured provenance, waveform
diagnostics and four composite plots. The collected producer HEAD was
`fd4aee1bba0c529277b20ae15ffc4484ca8d7dc7`. A separate audit of the published
copy passed file/raw-stream identities, saved timing reductions, control and
seek geometry, and image/input bindings. Audio, arrays, binaries and weights
are excluded; this archive cannot replay the numerical waveform calculations
or establish musical acceptance. The user offered replacement tracks for the
next assessment; those inputs and a listening verdict remain outstanding.

# M2 live runtime comparison, 21 October 2026

All four planned runs completed, but the packed-view candidate did not establish
a delivery improvement. Both candidate runs missed the **4.095-second target**;
the baseline passed once and missed once. Keep the candidate unpromoted and
M2 open. Best quality, Vulkan, two runtime CPU threads, unpaced requests and the
existing thermal policy remain unchanged.

| Run, in execution order | Validated hops | Sample p99 / maximum | Hops ≥4.095 s | Seek/replay holds | Periodic CPU/ACPI maximum |
|---|---:|---:|---:|---:|---:|
| A1, baseline | 21 | 3.908 s | 0 | 19.349 s | 79°C |
| B1, packed candidate | 21 | 6.935 s | 9 | 28.277 s | 94°C |
| B2, packed candidate | 22 | 6.606 s | 9 | 30.987 s | 73°C |
| A2, baseline | 21 | 5.550 s | 2 | 17.184 s | 83°C |

Each hidden-window run measured 120 seconds with quiet Club XL playback.
Nearest-rank p99 equals the maximum at these sample sizes. All completed hops
are retained, including two tail hops in each candidate run. The results are
reported separately; no pooled p99 or sustained-performance qualification is
claimed. The runtime-only order was baseline → candidate → candidate → baseline,
with a fresh copy of the same immutable driver-cache seed for every run.

The shutdown fix in [PR #49](https://github.com/kyaulabs/conflux/pull/49) allowed
each current finite generation to finish after the measured interval and WAV
finalization. The four tails lasted 11.139, 19.984, 22.798 and 9.239 seconds,
within their fixed 60-second limits. All reached raw completion, converter
completion and render EOF for the same generation, then normal runtime exit
and ready-for-stop state. The measured interval and recording frame count stayed
fixed throughout each tail. Source, model, runtime and cache checks passed;
all five root audits passed, including the final owned-process, group,
PipeWire and cache cleanup audit. No retry or test-window renewal occurred.

All runs reported zero steady-playback and tail starvation, execution deadline
misses, invalid buffers, source-read failures and recording overflow. The live
deck still held during seek and replay recovery: the table gives aggregate
control-hold time per run, separate from initial loading. A prepared peer kept
advancing. These holds remain delays in live-deck availability during seeks.

Independent scans checked all 48,737,792 float samples in the four finalized
stereo 48 kHz WAVs. All were finite; none reached absolute 1. Header/data/fact
geometry, frame counts, peaks, nonzero counts and recorder accepted/written
counts matched exactly. These checks establish recording integrity, not a new
listening verdict or waveform equivalence between the runs.

Each run exercised an accepted generation that was incomplete when a seek was
issued. One request returned from generation 1 after generation 2 was current and
was suppressed: request 3 in B2 and request 2 in the other runs. Four replay
recoveries completed per run. This demonstrates
generation isolation and stale-result suppression; no hard GPU cancellation
acknowledgement or deadline was measured.

Only one periodic two-second CPU/ACPI sample reached 85°C: B1 peaked at 94°C.
The table uses those periodic summaries; boundary snapshots are separate.
For example, A1's final boundary snapshot recorded 85°C, while its periodic
maximum was 79°C. There were no consecutive periodic hot samples or periodic
samples at or above 95°C. Sampling cannot establish continuous spike duration
or qualify a thermal soak. The existing host policy changed the profile during
every run; the policy was not modified.
These short runs do not establish a causal thermal or power-limit
explanation for the latency variation, and hidden windows do not qualify
visible UI rendering.

The worst validated hops spent 3.651, 6.662, 6.048 and 5.230 seconds in their
native graph-compute brackets. Source reads, uploads, readback and lease handling
were much smaller, and no graph was rebuilt. These nested or overlapping durations
use their own clock domains and are not an additive phase partition. The next offline step is to
inspect the candidate's tensor layout and Vulkan graph materialization before
proposing another laptop run. The current evidence does not support further
delivery-queue tuning or adopting the candidate.

The source HEAD was `3d56de316fe4c2bf1b9dd67e15eff3e54b0d386c`.
The frozen plan SHA-256 was
`d548b3eebc3b49b2025fceb5bfec49443395d9052c94e33e3cc0b8af91c57bb9`.
The baseline runtime SHA-256 was
`ca781657d698119b298c402cb716329b10ce113f10c31633be4b8fade8fa0e71`;
the candidate was
`82e5bcf77e70353a82bec88c892eb420d6d565382cbf7fadfaf00cbad0d61a6c`,
as in the [previous report](packed-attention-views-2026-10-20.md).
Their historical 16-case exact-output checks were reused, not rerun during
this comparison.

The [evidence manifest](data/m2-live-runtime-comparison-2026-10-21/archive-manifest.json)
binds the frozen recipe, readiness, saved results, traces, cleanup and independent
audits. [Archive verification](data/m2-live-runtime-comparison-2026-10-21/verification.json)
checks stored and decompressed hashes. Music, recordings, models, executables
and caches remain outside source history; recording hashes are retained.
No further laptop test is queued.

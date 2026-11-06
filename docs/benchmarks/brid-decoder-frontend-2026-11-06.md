# Beat/downbeat decoder and frontend diagnostics, 6 November 2026

Neither decoder candidate nor the upstream mono frontend qualifies automatic
beat/downbeat analysis. On all 93 BRID ensemble recordings, the current minimal
decoder passed both numeric screens on 4 recordings. The optional three/four-beat
DBN passed both on none; adding two-beat meter support returned to 4. Mono input
preparation also remained at 4 while reducing downbeat micro F1. Keep the current
model, frontend and decoder; M2, M3 and release `0.1.0` remain open.

| Fixed variant | Beat screen / 93 | Downbeat screen / 93 | Both / 93 | Downbeat micro F1 | TP / FP / FN |
| --- | ---: | ---: | ---: | ---: | --- |
| Current per-channel frontend, minimal decoder | 81 | 4 | 4 | 0.518031 | 984 / 610 / 1,221 |
| Same logits, official optional DBN, meters 3/4 | 85 | 1 | 0 | 0.431750 | 718 / 403 / 1,487 |
| Same logits, DBN with meters 2/3/4 | 84 | 6 | 4 | 0.456458 | 781 / 436 / 1,424 |
| Upstream mono frontend, same minimal decoder | 81 | 4 | 4 | 0.508646 | 956 / 598 / 1,249 |

All variants returned complete results for every recording; operational failures
are zero and the denominator stays 93. Screens use the original source-relative
annotations and scoring functions: beat F1 at least 0.90 within 70 ms, matched
median error at most 35 ms, nearest-rank p95 error at most 70 ms and median relative
adjacent BPM error at most 3%; downbeat F1 is at least 0.90 within 70 ms. There is
no fitted shift, octave correction, threshold sweep or post-output exclusion.
These are in-sample diagnostic screens, not musical qualification or calibrated
confidence. Recording-level training independence remains unknown.

The CPU-only Apollo capture ran the production native preparation and Final0
worker on all 93 recordings. Its prepared PCM identities, BPM summaries and beat
maps exactly reproduced the retained laptop baseline. Each of the 93 saved raw
beat/downbeat arrays and source masks round-tripped through the unchanged minimal
decoder to the same result. This establishes a fixed input for decoder comparison;
it does not measure the live stem service or audio/UI contention.

The optional DBN uses the pinned upstream postprocessor with 50 FPS, 55–215 BPM,
60 tempo states, transition lambda 100, observation lambda 16, threshold 0.05,
correction enabled and one thread. Only the allowed meter set changes between the
two DBN variants. The two/three/four-beat candidate gained joint passes on cases
49, 81 and 91 and lost 10, 45 and 46. Its higher isolated downbeat pass count does
not offset the lower aggregate F1 or increase joint passes. A fixed nine-case
synthetic meter/tempo check had passed for the candidate; the real corpus shows
why that synthetic result was insufficient for adoption.

The current prediction count alone prevents 73 of 93 recordings from reaching
0.90 downbeat F1 even with perfect event placement at the same count. Summing each
recording's maximum possible count matches gives a micro-F1 ceiling of 0.800737.
This is a bound, not a proposed repair or a rescored prediction.

A descriptive logit audit used the unchanged source mask and exact ±70 ms windows
around the 4,342 reference beats. For the 2,205 position-one references, 985 windows
(44.67%) contain a positive downbeat logit while 2,146 (97.32%) contain a positive
beat logit. The median maximum downbeat logit is −0.735350; in 28 recordings the
position-two median is higher than the position-one median. These observations
suggest a limitation beyond simply choosing meter states. A window maximum is
not a decoded event, a one-to-one match, calibrated confidence or proof of the
cause of any individual miss. Do not relabel beats or infer reliable bars from it.

The frontend comparison changes one factor. Production resamples each stereo
channel, computes linear Mel features, averages them, then applies `log1p`.
The pinned upstream `Audio2Frames.signal2spect` averages stereo samples first,
then resamples and computes log-Mel features. All 93 BRID recordings have unequal
channels; minimum channel correlation is 0.914422 and minimum mono-to-mean-channel
energy ratio is 0.956708. The frontend operations therefore need not agree, despite
limited cancellation in these inputs.

The diagnostic mono worker uses that upstream frontend with the same Final0
checkpoint, float32 CPU inference, minimal decoder, source mask and scoring gates.
Equal, unequal and opposite-channel frontend fixtures matched upstream exactly;
checkpoint loading was forbidden during those generated checks. Native PCM export
matched the frozen first-case identity. After the pilot, all remaining 92 cases
completed. The full CPU owner took 2,137.663 seconds and reaped its child/container.
Mono gained case 52 and lost case 10 for downbeat and joint screens, with no beat
pass changes. Its private diagnostic recipe is not enabled in the player or
accepted as a production response.

The earlier [Final1 checkpoint comparison](beat-model-comparison-2026-10-26.md)
already regressed from 9/24 to 8/24 beat-screen passes under its ARTBeaT protocol.
That separate corpus and retained outer-observer failure must not be combined
with these BRID counts or presented as an untested checkpoint improvement.

Source pins are retained in the evidence:

| Component | Identity |
| --- | --- |
| Product source base | `03b589fbca528c611e335dc07206047bdbec2385` |
| Production analysis worker SHA-256 | `e76b8ab903aebc76a592ac262b1dd3c7602cdea0606610e866cca31156d753b5` |
| Final0 checkpoint SHA-256 | `8c328b45f59d8dd3dff219253ff6a8d6482be57d0133a29140e2febbf8eb8331` |
| Beat This source commit | `b95c8ab0c58c2d9fcfd40508ae8dffbc05ac4f5c` |
| Pinned postprocessor SHA-256 | `431d6aeef41a8e9e9e63ee2577e895a5e214e0dbbcd2d505458cdfec02a611ee` |
| Pinned upstream inference SHA-256 | `ccccff4665399b931ebf181b73d264a6ad64fc1fe8acb4d3b950df825c47ce2d` |
| Mono diagnostic worker SHA-256 | `7ba774aa27521ddcf9b0c11bfd7292d2ace6d6d823b28245d01e4f55b9fbcae8` |
| MADmom source commit | `27f032e8947204902c675e5e341a3faf5dc86dae` |

The existing private MADmom Python-compatibility package was used only for the
diagnostic, with SciPy 1.17.1. CPU inference used Python 3.12.14, Torch 2.11.0+cpu,
NumPy 2.5.4 and soxr 1.1.0. No production dependency was added.

The [manifest](data/brid-decoder-frontend-2026-11-06/verification.json) binds 677
members in a 1,723,741-byte archive, including raw predictions, raw logits/masks,
reference identities, scorers, plans, failed setup evidence and cleanup receipts.
The raw files passed Gitleaks before compression. No audio, model weights,
executables or PCM data are included. The offline verifier checks every digest,
re-scores all 372 retained predictions with the original scorer functions and
recomputes the descriptive logit windows; its
[result](data/brid-decoder-frontend-2026-11-06/reproduction.json) is retained.
Run it with an existing Python/NumPy installation from the repository root:

```sh
python3 -B docs/benchmarks/data/brid-decoder-frontend-2026-11-06/verify_evidence.py
```

This command starts no model, GPU, player or audio output and extracts no files.
Original capture commands and dependency pins remain in the archive; reproducing
inference also requires the separately retained dataset and model. BRID is
[CC BY 4.0, published by Maia et al.](https://zenodo.org/records/14051323); the
upstream code's MIT notice is included. Completed CPU containers, private
dependency overlays, temporary PCM and caches were removed after verification.
The compact baseline logits remain available for open analysis work.

No power, thermal-guard or OneDrive setting changed during these experiments.
They supply no cancellation, physical-audio, laptop p99 or sustained UI-load
qualification. Best quality and the strict 4.095-second live-stem target remain
unchanged. Automatic beat/downbeat/phrase/section/key/cue accuracy, confidence,
integrated fallback and current primary-laptop sustained qualification still
need evidence before completing M2/M3 or publishing `0.1.0`.

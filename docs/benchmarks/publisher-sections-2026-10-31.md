# Publisher-reference section assessment, 31 October 2026

The publisher's exact stem preparation did not resolve the weak section-boundary
results. All eight fixed SALAMI reference recordings completed all eight model
folds. Automatic section and cue qualification remains open; no production backend
or release gate changed. This assesses automatic section timing; the earlier
stem-sound and manual-mixing acceptances remain recorded.

F1 ranges from 0 to 1; higher is better. Each model cell reports annotator 1 /
annotator 2. Human agreement provides context for disagreement between the two
annotations; it is not a fitted target or an accuracy ceiling.

| Layer | Tolerance | Earlier native F32 | Publisher PCM16 | Human agreement |
| --- | --- | --- | --- | --- |
| coarse | 0.5 s | 0.444 / 0.471 | 0.421 / 0.454 | 0.711 |
| coarse | 3 s | 0.508 / 0.569 | 0.483 / 0.539 | 0.768 |
| fine | 0.5 s | 0.419 / 0.427 | 0.440 / 0.450 | 0.715 |
| fine | 3 s | 0.486 / 0.491 | 0.501 / 0.509 | 0.744 |

Coarse boundaries became slightly worse. Fine boundaries improved slightly.
The weak recordings stayed in the denominator: coarse F1 at 0.5 seconds against
annotator 1 was 0.143 for recording 1100 and 0.074 for 1290. This comparison gives
no evidence that using the original publisher frontend fixes section accuracy.
Confidence remains uncalibrated, and phrase/bar accuracy is unqualified.

The run used the original publisher Demucs preparation and unmodified PCM16
spectrogram function, with the original role order and source-relative frame
centers at i/100 seconds. Six completed stem sets were preserved from the
interrupted owner; a separate continuation generated only the remaining two.
The interrupted owner's failure record remains intact. All 32 WAVs and all eight
feature arrays passed receipt, format, shape, clock and hash verification.

The model graph, eight pinned fold checkpoints, postprocessing, fixed recordings,
both annotation layers, both annotators and the 0.5/3-second scorers were held
fixed. No track was replaced, no time offset was fitted, and the inference
container had no annotation policy. Stock and guarded section decoding agreed on
all eight recordings. Raw logits remain on Apollo for inspection without another
neural run.

The CPU-only inference took 1838.691 seconds; the largest
reported per-case RSS was 1.96 GiB.
The owned service, child groups and container are closed. No GPU, player, audio
output, laptop power change or physical-test readiness was consumed.

The [evidence manifest](data/publisher-sections-2026-10-31/manifest.json) binds
31 compressed text files with original and compressed hashes. It retains the
interrupted owner, separate continuation, input provenance, executed scripts,
cleanup receipts, full scores and the earlier baseline. The package contains
191,488 compressed bytes. Audio, feature/logit arrays,
model weights and runtime binaries remain external. Publisher license notices
are included with the unchanged spectrogram source. The annotation policy hash
is recorded; no confidence threshold or qualification gate was fitted to these
eight exposed recordings.

These measurements supply no new live-stem latency, cancellation or physical-audio
qualification. The laptop's strict service p99 target remains below 4.095 seconds.
M2, M3 and release 0.1.0 remain open under the existing plan.

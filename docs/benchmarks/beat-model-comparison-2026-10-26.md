# Beat-model comparison, 26 October 2026 UTC

The Final1 candidate met the fixed numerical screen on **8 of 24** ARTBeaT
fixtures, compared with **9 of 24** for the retained Final0 production-input
baseline. It gained case 16 and lost cases 2 and 3. Final1 is not promoted;
production stays on Final0. Automatic beat analysis and the remaining M2/M3
release gates are still unqualified.

| Check | Retained Final0 baseline | Final1 candidate |
|---|---:|---:|
| Fixed admitted fixtures | 24 | 24 |
| Fixtures meeting every numerical threshold | 9 | 8 |
| Passing IDs | 1, 2, 3, 4, 7, 8, 9, 10, 11 | 1, 4, 7, 8, 9, 10, 11, 16 |
| Current native-prepared input clock | Stereo 48 kHz | Same PCM frame counts and SHA-256 hashes |
| Original outer execution verdict | Failed RSS observation | Failed RSS observation |
| Candidate accepted by the production parser | Not applicable | No; all 24 private-recipe responses rejected |
| Automatic use or milestone qualification | No | No |

The comparison retained the same decoder, per-channel resampling, linear-mel
channel mean, source timestamps, minimal postprocessor and full denominator.
The screen still requires full F1 ≥ 0.90, median absolute matched timing error
≤ 35 ms, nearest-rank p95 ≤ 70 ms, median relative adjacent-BPM error ≤ 3%,
70 ms matching and nonempty known intervals. No shift, octave correction,
threshold fitting, selected window or additional exclusion was applied.

The published Final1 checkpoint has the same architecture and tensor geometry
as Final0; a safe CPU `weights_only` load found all tensors finite before
inference. The worker uses CPU float32 evaluation with two CPU threads and the
same pinned upstream source/dependencies. The publisher identifies these as
separately trained seeds and licenses the code and published weights under MIT;
see the pinned [upstream README](https://github.com/CPJKU/beat_this/blob/b95c8ab0c58c2d9fcfd40508ae8dffbc05ac4f5c/README.md).
The locally retained checkpoint hash is an acquisition integrity record, not
an independently signed publisher checksum.

The unchanged native preparation/analysis probe regenerated each prepared
source. Its recorded frame count and PCM digest matched the baseline for every
fixture. A private worker retained its exact candidate bytes before returning
them unchanged. The production parser then rejected the private Final1 recipe;
its acceptance checks were not bypassed or relabelled. This keeps an experimental
model separate from accepted production timing. All 24 raw responses were
retained before numerical scoring, including cases that failed the screen.
All 24 returned estimates; an estimate does not establish musical accuracy or
calibrated confidence.

The native corpus process emitted its 24-case completion record after 178.842
seconds. The outer owner subsequently failed with `Live owned RSS missing` and
sent TERM at 179.241 seconds. It recorded a normal native return code of 1,
complete bounded captures, unchanged input pins and absent known process groups,
but retained `complete=false` and `cleanup_proven=false`. Its command ended in
180.293 seconds of the original 600-second limit. The highest sampled owned RSS
was 477,114,368 bytes, below its 2 GiB sampled limit; this is not a hard peak-RSS
measurement.

An initial exit-observation correction passed a regression that failed against
the original observer, but it did not cover this real exit sequence. That
failure remains unresolved. A separate host check verified all 25 known owned
lifetimes and groups absent and reverified the frozen inputs at 450.284 seconds
from the original command start. It does not turn the failed execution into an
operational pass.

A separately authorized scorer used the already retained bytes and that
absence proof. It verifies canonical candidate output, source-frame order,
PCM identity and the expected native rejection, while retaining all 24 slots.
It preserves both the original owner failure and the earlier scorer that
required a fully successful execution. Twenty-four pure checks cover these
rules, the observer regression and existing scoring/ownership contracts. No
model rerun occurred to obtain the reported scores.

The [verification record](data/m2-beat-final1-2026-10-26/verification.json)
retains the plans, complete raw candidate predictions, failed receipts,
separate cleanup, numerical results and source/test evidence with original and
compressed hashes. Audio, checkpoints, executables and prepared PCM remain
private. Training-set independence and downbeat truth remain unknown; this
exposed-set diagnostic does not qualify phrase/key/cue analysis, latency,
physical audio/UI behavior or M2 completion.

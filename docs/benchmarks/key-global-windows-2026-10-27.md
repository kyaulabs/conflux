# Global-key window comparison

Using the publisher's global-window defaults **did not improve the exact-match count** on the retained
FMAK sample: **10/21 (47.62%)** inferred tracks,
compared with **10/21 (47.62%)** using local-keygram sampling.
Including the three retained decoder failures gives **10/24
(41.67%)**. This is the same previously exposed sample;
it does not establish fresh held-out accuracy or calibrated confidence.
The production key backend remains disabled.

| Setting | Retained baseline | Candidate |
| --- | --- | --- |
| Numeric model | DeepSpec k16 | Same pinned model and weights |
| Context length | 60 CQT columns | 60 CQT columns |
| Context hop | 8 columns | 30 columns |
| Boundary padding | 30 columns at each end | Right padding only for inputs shorter than 60 columns |
| Total classified contexts | 3,086 | 787 |
| Exact matches on inferred tracks | 10/21 | 10/21 |

The native prepared PCM, source identities, legacy CQT, whole-population
float64 whitening, float16 conversion, numeric forward path and arithmetic-mean
posterior rule are unchanged. Window selection is the sole experimental
change; its altered population also changes the normalization statistics.
Legacy float64 CQT values are retained without narrowing. No hop, padding,
class mapping or reference label was tuned after viewing candidate predictions.

All 21 inferable tracks completed. The original failures `003853`, `005153`
and `061480` were neither converted nor replaced. All 24 captures closed
before the reference mapping was read for scoring. The paired comparison
repaired **0** tracks and regressed **0**;
**10** were correct in both runs.

Fourteen generated CQT geometries matched the pinned publisher helpers and
an independent marker oracle. Sixteen functional checks cover complete score
denominators, capture and model identity, malformed probabilities, source
context bounds, legacy precision and stop/reap behavior. This global-only
packet rejects the local-keygram population and does not emit a local key map.
The earlier [numeric-model checks](deepspec-fmak-2026-10-27.md) remain the
operator reference; the original TensorFlow execution environment was not reproduced.

The [2019 paper](https://www.audiolabs-erlangen.de/content/05_fau/professor/00_mueller/03_publications/2019_SchreiberM_TempoKeyCNN_SMC.pdf)
evaluates whole spectrograms. This run tests the separate packaged API's
window defaults on CONFLUX-prepared PCM. Exact training membership of the released weights
remains unverified. Song-level FMAK references do not qualify local key changes.

The CPU-only run closed after **1125.906 seconds** within
its original 2,820-second bound. All **176 frozen inputs**
still matched. The owned unit/cgroup, both children per inferred case and
private preparation spools are closed. The existing thermal guard and 15 W
ceiling remained in force. The diagnostic made no profile, cap or guard writes. This was an accuracy
diagnostic without player, GPU inference or audio output, and it consumed no
physical-test readiness.

The [text-evidence manifest](data/m2-key-global-window-2026-10-27/manifest.json)
retains the frozen recipe, generated checks, every capture, source failures,
paired scores and closure proofs. It omits audio, PCM, CQT tensors, weights,
model containers and executables. M2/M3, live-stem cancellation, the actual-player
soak and the unchanged **4.095-second p99** target remain open.

# Local key-CNN diagnostic

The fixed key-CNN candidate reached **79.82% exact-key agreement over all
230.46 annotated seconds** of two previously exposed SC06 recordings. Both
cases completed with the exact prepared frames and PCM hashes used by the
earlier diagnostics. This is an encouraging private accuracy result, not a
held-out or general-genre qualification. The application harmony engine remains
unconfigured, confidence remains **Unknown**, and M2/M3 and release gates stay open.

The comparison uses the existing Decimal overlap scorer with `ann3` as the
primary reference. Unknown, Ambiguous and Failed spans retain their denominator;
unlabelled reference gaps remain unlabelled. No model, threshold, transposition,
context-duration or time-offset fit followed scoring. All candidate predictions
were saved before this run parsed the reference CSVs.

| Candidate | Exact top-one agreement | Estimated coverage |
| --- | ---: | ---: |
| Existing chroma/profile worker, four-second support | 11.95% | 37.54% |
| Existing worker, cropped sixteen-second support | 15.80% | 30.45% |
| KeyFinder, cropped sixteen-second support | 22.07% | 100.00% |
| Key-CNN, nominal sixty-frame context and eight-frame hop | 79.82% | 100.00% |

Every row uses the same primary known-reference duration and exact prepared
source PCM. The algorithms, contexts and output grids differ, so this does not
isolate one cause of the improvement. The earlier workers used fixed four-second
target cells. The key-CNN produces contextual estimates about every
**1.486077 seconds**; its nominal sixty-frame window spans **11.145578 seconds**.
CQT filters have additional frequency-dependent support. These are not exact
musical change boundaries or confidence intervals.

The primary per-recording results were **70.20%** for `D911-02` and **86.47%**
for `D911-03`. The candidate emitted 65 and 93 cells, covering all 11,175,810
prepared frames, or 232.829375 source seconds. The 2.369375 seconds outside the
primary annotations were retained separately and never assigned reference labels.

| Reference | Known duration | Exact top-one agreement |
| --- | ---: | ---: |
| `ann3`, primary | 230.46 s | 79.82% |
| `ann1`, sensitivity | 230.46 s | 71.94% |
| `ann2`, sensitivity | 167.34 s | 82.45% |

The sensitivity rows use their own annotated spans. They are not pooled into a
larger test set. Estimated coverage was 100% on those spans; this reflects the
unthresholded top-one diagnostic, not calibrated confidence or automatic-use approval.

The candidate uses the pinned
[Key-CNN source](https://github.com/hendriks73/key-cnn/tree/9715d6f443c0f01d9842e567a3f20c9b228c3c7f)
and the already audited `winterreise_n_fold16` model, SHA-256
`b425f2e8d5165415c8863b6666aa82d7573121ee675bd2fd8b59d7594f5f11e9`.
Its filename and the published split description are consistent with excluding
SC06 and songs 01–03; exact released-weight training membership remains unverified.
The network reads only hashed raw float tensors extracted earlier as data.
No TensorFlow/Keras module or serialized HDF5 model code was executed.

Prepared stereo float PCM is averaged to mono and resampled from 48 kHz to
22.05 kHz with the legacy `kaiser_best` path. The unmodified Librosa 0.6.2 CQT
and Resampy 0.2.2 sources retain the upstream E1 minimum, 168 bins, 24 bins per
octave and 4,096-sample hop. Thirty zero feature columns precede and follow the
source. Normalization uses the entire feature population in float64, converts
through float16, then feeds the numeric float32 classifier in batches of at most
four. The source definitions are in
[the feature function](https://github.com/hendriks73/key-cnn/blob/9715d6f443c0f01d9842e567a3f20c9b228c3c7f/keycnn/feature.py),
[normalizer](https://github.com/hendriks73/key-cnn/blob/9715d6f443c0f01d9842e567a3f20c9b228c3c7f/keycnn/classifier.py)
and [keygram tool](https://github.com/hendriks73/key-cnn/blob/9715d6f443c0f01d9842e567a3f20c9b228c3c7f/bin/keygram).

The isolated environment used Python 3.10.22, NumPy 1.23.5, SciPy 1.10.1 and
Numba 0.57.1. A removed `numba.decorators` import name aliases the same `jit`
callable. This reproduces the audited source path with compatible dependencies,
not the original training environment or verified legacy TensorFlow inference.
No production dependency or configuration was added.

The generated checks establish narrower properties:

- Six generated signals passed CQT/window construction in five configurations.
  Their window and padding values matched the upstream helpers exactly.
  Modern Librosa substitutions differed on every non-silent fixture and changed
  the top class in 3 of 18 generated classifier windows. The legacy feature
  recipe was retained rather than assuming interchangeability.
- Seven generated PCM16 WAV comparisons matched the upstream file path exactly
  after decoding, resampling and feature construction. Four impulse peaks stayed
  within half a resampled sample; two tone bursts peaked at the nearest CQT frame.
- Six clock tests check full source coverage, fractional-step rounding,
  unsupported tails, a padded prediction centered after EOF and malformed
  extents. Those unsupported cells remain Unknown. Very short sources preserve
  distinct resampler and CQT failures instead of producing a fabricated estimate.
- Whole-population normalization matched the original isolated source function
  bit for bit at 129, 471 and 512 windows. Routing checks used row markers to
  verify batching and order; seven edge windows used the real numeric classifier.
  This does not measure inference over an entire maximum-length track.
- Seven protocol tests reject forged source/recipe/clock fields, malformed class
  probabilities, Boolean frame/tonic values, wrong PCM hashes and trailing bytes.
  Four real entry checks confirmed dry defaults and rejection of an incorrect
  owner or source before model work.

The first two-source attempt failed operationally when repeated legacy
deprecation warnings exceeded the 4,096-byte stderr limit. Both slots remain
**Failed** in that attempt; its zero score is not a musical accuracy measurement.
The correction captured all warnings in a bounded sidecar and kept phase/error
summaries in stderr. Each completed case retained 25 warnings with two unique
messages. The algorithm and the original 45-second case, 110-second inner,
120-second unit and 150-second cleanup bounds were unchanged. Earlier generated
test-expectation failures are also retained.

The completed run took **63.202 seconds** including owned-unit cleanup.
All four case children were reaped, both preparation spools were empty and the
unit cgroup was absent or empty. All 147 input pins remained unchanged. The
thermal guard and existing power ceilings were preserved. This was a CPU-only
correctness diagnostic: it opened no audio device, used no GPU, selected no power
profile and consumed no pending laptop-test readiness.

The [evidence manifest](data/m2-keycnn-diagnostic-2026-10-27/manifest.json) retains
the numeric results, predictions, source hashes, scripts, limits, warnings,
failures and cleanup receipts as compressed UTF-8 files. Original recordings,
audio derivatives, reference CSV contents, model weights, generated arrays and
installed runtimes are omitted. The manifest records four reversible source-digest
label changes, retaining original hashes and unchanged digest values. The SC06 recordings remain private noncommercial
evaluation assets under their retained BY-NC-ND 3.0 notices.

Next work must test independent material, verify model provenance and source-time
behavior on real key changes, assess confidence and validate production ownership
before promotion. Beat accuracy remains 9/24 on the existing challenge.
The latest [GPU usage trace](gpu-client-usage-2026-10-26.md) still exceeded the
unchanged **4.095-second live-stem p99** target. This diagnostic supplies no new
audio-output, cancellation, sustained-player or release qualification.

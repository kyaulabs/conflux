# Key-CNN CQT compatibility

A source-derived filter-length correction and the legacy resampler brought
Librosa 0.11.0 within the existing CQT tolerance on all six generated fixtures.
All eighteen generated top-class predictions matched the legacy reference;
the largest posterior difference was **0.000009239**. Normalized inputs and
posteriors were still not bitwise identical. This is a generated compatibility
result, with no new musical accuracy or production qualification.

The [earlier diagnostic](key-cnn-diagnostic-2026-10-27.md) retained Librosa 0.6.2
and Resampy 0.2.2 after uncorrected modern substitutions changed three of eighteen
generated predictions. This follow-up held the six signals, CQT geometry,
reflection padding and `kaiser_fast` mode fixed. It changed filter scale,
requested output precision and resampler version separately. No musical labels
were read or parameters fitted to music scores.

| Configuration | Largest relative CQT L2 error | CQT tolerance passed | Changed top classes |
| --- | ---: | ---: | ---: |
| Retained modern control, Resampy 0.4.3 | 0.012884945 | 1/6 | 3/18 |
| Control with complex128 requested | 0.012884949 | 1/6 | 3/18 |
| Corrected filter scale, complex64 requested | 0.006610294 | 1/6 | 0/18 |
| Corrected filter scale, complex128 requested | 0.006610297 | 1/6 | 0/18 |
| Corrected filter scale, complex64, Resampy 0.2.2 | 0.0000001002 | 6/6 | 0/18 |

Each CQT comparison used the unchanged elementwise tolerance `rtol=1e-5`,
`atol=1e-7` over the full array. The eighteen windows are three padded windows
per generated signal, using the earlier helper's thirty-frame step. They are
not the eight-frame prediction grid used by the real-music diagnostic.

The difference follows from the filter definitions. At 24 bins per octave,
[the legacy implementation](https://github.com/librosa/librosa/blob/0.6.2/librosa/filters.py)
uses `Q = filter_scale / (2**(1/24) - 1)`.
[The newer implementation](https://librosa.org/doc/0.11.0/_modules/librosa/filters.html#wavelet_lengths)
uses `Q = filter_scale / alpha`, where the equal-tempered value is
`alpha = (2**(2/24) - 1) / (2**(2/24) + 1)`.
Setting the newer filter scale to **0.9853549899493643** restores the legacy Q.
The measured relative filter-length difference across all 168 bins was at most
`1.4322e-14`; the newer per-frequency bandwidth calculation retains small
floating-point differences.

Requesting complex128 output did not resolve the remaining feature differences.
It does not make every internal operation double precision: the newer CQT's
wavelet construction still defaults to a complex64 basis. Keeping the corrected
scale and requested complex64 output fixed, switching only Resampy 0.4.3 to the
already audited 0.2.2 sources reduced the largest relative CQT L2 error from
0.006610294 to `1.001664e-7`. This identifies the resampler version as a material
part of this recipe; it does not distinguish filter-table changes from resampling
algorithm changes.

The final configuration preserved every generated top class. Float16 conversion
after whole-population normalization still changed some values for the chord
and noise fixtures. The largest normalized-input difference was **0.0009765625**;
the largest class-probability difference was **0.00000923872**. Matching top
classes does not establish equivalent confidence or legacy TensorFlow behavior.

All four new feature configurations completed, followed by 72 generated
classifier windows. Their probability vectors were finite and normalized. The
four frozen plans retained 38 distinct source/input files, all unchanged at
completion. Workers used one CPU thread, a ninety-second CPU limit and a
hundred-second outer limit. The original reference features and posteriors were
reused. No GPU, player, audio device or physical laptop test started, and no
production dependency or configuration changed.

The [evidence manifest](data/m2-keycnn-cqt-compatibility-2026-10-27/manifest.json)
retains the plans, scripts, complete numeric results, warnings and file hashes
as compressed UTF-8 files. Generated arrays, installed dependencies and model
weights are omitted. The prior evidence retains their fixture definitions,
dependency/source provenance and legacy references.

The real-music baseline remains the earlier **79.82%** exposed-source diagnostic
using the legacy recipe. The corrected modern recipe needs independent musical
and source-time validation before promotion. Confidence and production harmony
remain unconfigured. Beat qualification, the actual-player soak and the unchanged
**4.095-second live-stem p99** target remain open; this work supplies no new
latency, cancellation or audio-output result.

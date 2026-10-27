# DeepSpec on the fixed FMAK sample

The publisher's default DeepSpec model matched **10/21 inferred tracks
(47.62%)**, compared with **10/21 (47.62%)** for the earlier Winterreise
candidate. Keeping the three source rejections gives **10/24 selected
tracks (41.67%)**. The paired comparison improved 3
tracks and regressed 3. The production key backend
remains disabled; confidence remains `Unknown`.

This uses the same exposed sample as the [earlier FMAK diagnostic](key-cnn-fmak-2026-10-27.md).
Its 24 tonic/mode classes, distinct artists, source files and native preparations
are unchanged. Earlier predictions and reference classes had already been
examined before this candidate was selected. Training membership is unverified.
These results therefore describe a candidate comparison, not fresh held-out or
general-genre accuracy. FMAK supplies song-level labels, not local key changes.

The [pinned upstream classifier](https://github.com/hendriks73/key-cnn/blob/9715d6f443c0f01d9842e567a3f20c9b228c3c7f/keycnn/classifier.py)
maps its default `deepspec` alias to `deepspec_k16`. Its directional spectral
filters differ from the Winterreise model's square filters. The downloaded
container's Git blob and SHA-256 were checked. Only its 74 numeric F32 tensors
(290,376 parameters) were extracted; serialized Keras objects were not executed.
The model and its numeric weights remain local. The publisher's source/model
license record is retained with the evidence.

The private NumPy implementation passed three directional-filter scalar checks
and **102 layer comparisons over four generated windows** against an independent
float64 SciPy graph interpreter. Maximum posterior difference was
**1.47848375e-07**.
Batch sizes 1, 2 and 4 produced identical posteriors for the mixed-window fixture;
11 invalid-input/geometry/variance cases were rejected. Original TensorFlow
execution equivalence remains unverified. The previously used CPU Torch runtime
was unavailable, so this check used the already-installed SciPy reference.

The music comparison changes only the model. Native PCM identities, the legacy
CQT recipe, padded 60-frame windows at hop eight, whole-population whitening and
the frozen arithmetic-mean posterior rule remain unchanged. This window recipe
was chosen for the earlier local-key diagnostic; it does not reproduce the
publisher's default global-window selection. Eight generated protocol/scoring
checks passed, including rejection of an old-model packet even after rehashing.

All 21 inferable sources completed. Sources 003853, 005153 and 061480 retain the
earlier decoder failures; none was converted, replaced, trimmed or relabelled.
All captures closed before reference parsing and scoring. The complete pipeline
retains `operational_pass=false` for those three source failures, while
`inference_pipeline_pass=true` records the successful inference portion.

The CPU-only run closed in **1386.674 seconds**. All 170
frozen inputs still matched, and the owned unit, process group and spool paths
were retired. It used one CPU core at low priority with the thermal guard and
15 W ceiling unchanged. No player, audio device or GPU worker started. Its
execution time is not a live-stem service-latency result.

| Source ID | Reference | Winterreise | DeepSpec | DeepSpec result |
| --- | --- | --- | --- | --- |
| 013088 | C major | F major | C major | Match |
| 040989 | Db major | Gb minor | E major | Mismatch |
| 003853 | D major | — | — | Source rejected |
| 005153 | Eb major | — | — | Source rejected |
| 010518 | E major | E major | E major | Match |
| 047070 | F major | F major | F major | Match |
| 013079 | Gb major | Gb major | Gb major | Match |
| 065671 | G major | G major | G major | Match |
| 014033 | Ab major | C major | G major | Mismatch |
| 071516 | A major | A minor | A minor | Mismatch |
| 105052 | Bb major | Bb major | Bb major | Match |
| 071506 | B major | Eb minor | B major | Match |
| 074906 | C minor | Eb major | C minor | Match |
| 086476 | Db minor | Db minor | E major | Mismatch |
| 112316 | D minor | G minor | Bb major | Mismatch |
| 059542 | Eb minor | D minor | F major | Mismatch |
| 045099 | E minor | E minor | G major | Mismatch |
| 065781 | F minor | A major | A major | Mismatch |
| 039588 | Gb minor | A major | A major | Mismatch |
| 107616 | G minor | G minor | G minor | Match |
| 075746 | Ab minor | Ab minor | B major | Mismatch |
| 050892 | A minor | A minor | A minor | Match |
| 061673 | Bb minor | Ab major | Ab major | Mismatch |
| 061480 | B minor | — | — | Source rejected |

The [evidence manifest](data/m2-deepspec-2026-10-27/manifest.json) retains source
provenance, numeric checks, frozen code/rules, raw posteriors, warnings, paired
scores and cleanup proofs. It excludes audio, prepared PCM, feature arrays,
weights, model containers and executables.

Local key-change accuracy, confidence calibration, production integration and
laptop performance remain unqualified. The actual-player latency baseline is
still **8.889-second p99** against **4.095 seconds**, with Best quality, the 15 W
ceiling and thermal protection unchanged. M2, M3 and release 0.1.0 remain open.

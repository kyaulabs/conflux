# Key-CNN on the fixed FMAK sample

The candidate matched **10 of 21 inferred tracks (47.62%)**. Retaining the three
source-preparation failures gives **10 of 24 selected tracks (41.67%)** for the
complete pipeline. This result does not support promoting the candidate to
CONFLUX. Confidence remains `Unknown`; the production key backend stays disabled.

The sample comes from the [FMAK release](https://zenodo.org/records/10719860),
which supplies song-level key/mode tags. Its labels do not identify local key
changes. Selection was frozen before media extraction or model inference:
one source for each of the 24 tonic/mode classes, with 24 distinct artists,
using the recorded seed and deterministic matching. The metadata filter kept
explicit CC-BY/CC0 entries, durations of 30–600 seconds and known artist/genre
identities. The 24 selections span twelve overlapping top-level genre labels;
they are not a representative random genre sample. Exact released-model
training membership remains unverified, so this is a diagnostic rather than a
claim of held-out performance.

The published FMAK label checksum and the original FMA metadata archive checksum
were verified. The selected audio members were fetched through bounded ZIP
range reads and checked against their directory extents and CRCs, then pinned
by SHA-256. Whole audio-archive MD5 values were **not** computed. Corpus audio,
prepared PCM, feature arrays, model weights and binaries are omitted from the
repository evidence.

The [MPEG EOF correction](mp3-length-2026-10-27.md) allowed the follow-up native
preparer to complete **21/24 sources**. Sources 003853 and 005153 were rejected
at opening by libsndfile, although FFprobe identified MP3 streams. Source 061480
produced a decoder/resynchronization error during reading. No source was
replaced, converted, trimmed or relabelled to remove those failures. The first
interrupted preparation remains recorded in the earlier EOF report.

All 21 successful preparations supplied their actual stereo 48 kHz frame count
and PCM digest. A second native preparation feeding each inference had to match
those identities exactly. The classifier and legacy feature recipe are the
same numeric `winterreise_n_fold16` candidate used in the
[earlier SC06 diagnostic](key-cnn-diagnostic-2026-10-27.md). Original TensorFlow
execution equivalence and training independence remain unverified.

The global decision rule was frozen before inference: take the arithmetic mean
of every validated raw posterior row with `math.fsum` in Python float64, choose
the largest class and break ties by the lowest C-relative index. An entirely
zero-feature population remains Unknown. There is no fitted confidence
threshold, temporal weighting or case replacement. Each output was closed and
checksum-bound before the scorer loaded the reference mapping. Seven generated
protocol/scoring checks passed beforehand, covering that denominator, quiet
rows, ties, altered captures, malformed probabilities and maximum packet size.

All **21 inference jobs completed** with no inference failures. The complete
24-case capture, including three source failures, was scored once. The root
receipt's `operational_pass=false` retains those input failures;
`inference_pipeline_pass=true` records the successful inference portion.
The bounded CPU-only run took **1,232.887 seconds**, including its observation
and cleanup. All 168 frozen inputs still matched. The owned unit/process group
is retired, and its 21 empty spool directories were removed after verification.
This runtime is an execution-budget record, not a live-stem latency result.

| Source ID | Reference | Candidate | Result |
| --- | --- | --- | --- |
| 013088 | C major | F major | Mismatch |
| 040989 | Db major | Gb minor | Mismatch |
| 003853 | D major | — | Source rejected |
| 005153 | Eb major | — | Source rejected |
| 010518 | E major | E major | Match |
| 047070 | F major | F major | Match |
| 013079 | Gb major | Gb major | Match |
| 065671 | G major | G major | Match |
| 014033 | Ab major | C major | Mismatch |
| 071516 | A major | A minor | Mismatch |
| 105052 | Bb major | Bb major | Match |
| 071506 | B major | Eb minor | Mismatch |
| 074906 | C minor | Eb major | Mismatch |
| 086476 | Db minor | Db minor | Match |
| 112316 | D minor | G minor | Mismatch |
| 059542 | Eb minor | D minor | Mismatch |
| 045099 | E minor | E minor | Match |
| 065781 | F minor | A major | Mismatch |
| 039588 | Gb minor | A major | Mismatch |
| 107616 | G minor | G minor | Match |
| 075746 | Ab minor | Ab minor | Match |
| 050892 | A minor | A minor | Match |
| 061673 | Bb minor | Ab major | Mismatch |
| 061480 | B minor | — | Source rejected |

The [evidence manifest](data/m2-fmak-keycnn-2026-10-27/manifest.json) retains the
selection/admission records, decoder failures, frozen rule and code, exact
per-case predictions, warning summaries, score and cleanup proofs. Enharmonic
spellings in the table are canonical display names for the compared pitch classes.

Song-level agreement cannot establish local harmonic-change accuracy. The
candidate also supplies no calibrated confidence. It therefore remains an
experimental comparison; the stronger SC06 result is not a general-genre
qualification. No audio device, player or GPU worker started for this diagnostic.

The last actual-player trace still has **8.889-second p99** against the unchanged
**4.095-second target**. Best quality, the 15 W ceiling and thermal guard remain
in place. M2, M3 and the first 0.1.0 release are still open.

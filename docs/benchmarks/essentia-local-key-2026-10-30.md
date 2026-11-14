# Essentia local-key assessment, 30 October 2026

The private Essentia candidate reached **29.97% exact local-key accuracy** over
the annotated duration of two complete Schubert Winterreise SC06 recordings.
It assigned a key to every window while reporting confidence as `Unknown`.
The result does not qualify the candidate for product use. M2, M3 and release
`0.1.0` remain open.

Both hosts processed the same fixed sources, D911-02 and D911-03. The source
preparer used `AudioFile.cpp` from signed commit
`b8339e186e6d7228855d169397196ba56e0574dc`. Fresh 48 kHz stereo PCM hashes and
frame counts matched the retained native preparation and each other. The two
complete prediction streams were byte-identical across hosts; all 59 local
window decisions agreed.

| Reference | Laptop exact accuracy | Apollo exact accuracy |
| --- | ---: | ---: |
| ann3, frozen primary | 29.9748% | 29.9748% |
| ann1, sensitivity | 29.7405% | 29.7405% |
| ann2, sensitivity | 35.4727% | 35.4727% |

The primary denominator is 230.46 seconds of known-key annotation, of which
69.08 seconds matched the predicted tonic and mode. D911-02 scored 32.68% and
D911-03 scored 28.10%. The primary annotation remained ann3 throughout; ann1
and ann2 did not replace it. Reference gaps stayed unlabelled.

The unchanged `private-essentia-bgate36-48k-v1` worker used the existing
Essentia `2.1b6.dev1438` runtime, the frozen BGATE/36-bin parameters and a fresh
extractor for each window. Four-second source cells used 16-second centered
support clipped to the source extent. No source-level normalization, pitch correction,
parameter sweep, fitted shift or key-class forgiveness was applied. Raw key
strength was never treated as confidence. The worker SHA-256 is
`2cfa210b482e839558e4aa9d1ec70cb659af5b88e5c6ab0e869c99e43726889f`.

Predictions, source identities and owner cleanup were checked before parsing
the annotations for this comparison. The scorer reused the retained Decimal
source-frame overlap calculation. It counted each emitted cell against every
reference segment it overlapped, including cells crossing key changes. It
kept the complete known-time denominator. A single-candidate estimator has no
top-two result.

The retained product-profile baseline scored 11.95% with 37.54% Estimated
coverage on the same primary reference. This candidate had 100% Estimated
coverage, so its conditional accuracy is also 29.97%. This compares different
frontends and support recipes; it does not isolate one cause or establish
generalization. These two exposed voice-and-piano recordings do not provide
independent genre validation or calibrated confidence. No backend was enabled.

The laptop capture took 26.09 seconds and Apollo took 30.21 seconds. The jobs
used one CPU each, with no GPU, audio output or power-policy changes. Both
owners exited and their owned process group/container were gone before scoring.
These are correctness timings, not realtime or latency qualification. Five
protocol checks covered source/clock mismatch, clipped support, missing/extra
windows, invalid numeric types and confidence/strength separation. They do not
replace musical accuracy evidence.

The [evidence manifest](data/essentia-local-key-2026-10-30/manifest.json) binds
56 compressed text files: exact scripts, helper source, plans, captures,
cleanup receipts, baseline scores and the new scores. It records both compressed
and original hashes. The package contains 74,027 bytes of compressed evidence;
recordings, PCM, models and binaries are excluded. Scripts retain the executed
private paths and one-use owner names; another run needs a fresh run directory.
The product source, model, live-stem target and hardware retry are unchanged.

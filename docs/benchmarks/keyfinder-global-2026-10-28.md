# Whole-track KeyFinder comparison

The private whole-track KeyFinder candidate matched 10 of 21 inferable FMAK
tracks on both hosts (47.62%), or 10 of the original 24 selections (41.67%)
with decoder failures retained. It did not improve on the earlier KeyCNN,
DeepSpec or fixed-model global-window counts. The production key backend stays
disabled, confidence stays Unknown, and M2/M3 and release gates remain open.

| Host | Inferred tracks | Exact global matches | Original denominator | Byte-identical emitted predictions across hosts |
| --- | ---: | ---: | ---: | ---: |
| Laptop | 21 | 10/21 | 10/24 | 21 |
| Apollo | 21 | 10/21 | 10/24 | 21 |

The candidate uses stock KeyFinder 2.2.8 profiles, tuning and classifier at
upstream commit `b33b5a88e04a5182dd19c38c57762925631118fd`.
Recipe `private-keyfinder-2.2.8-stereo-global-stream-v1` reads the complete
prepared stereo 48 kHz float stream in one-second blocks. Separate mono
workspaces retain each channel's chroma; combining their magnitudes avoids
cancelling antiphase stereo content. It makes one global decision after exact
EOF, complete PCM hashing and finite-sample validation. Inputs, output size and
process lifetimes are bounded. This is a private diagnostic, with no new product
dependency or integration.

The existing 24-track selection and preparation outcomes were fixed before
inference. The same 21 complete sources were decoded again with exact frame
counts and PCM hashes checked against the prior preparation record.
`003853`, `005153` and `061480` remained source-preparation failures; they were
neither retried nor replaced. Reference labels were first parsed by the scorer
after both prediction captures closed, all source and prediction hashes were
validated, and both owned units were retired. No label file was mounted into
Apollo's inference container.

Both independently built workers had the same binary hash. All 21 emitted
prediction files were byte-identical across hosts; this compares their keys and
metadata, not internal chromagram floating-point values. Both original captures
and every per-case failure remain in the evidence. The corpus was already
exposed by earlier experiments, so this result does not establish held-out
accuracy, training independence, local key changes or confidence calibration.

Generated DSP and protocol checks passed on both hosts. The first Apollo
protocol fixture tried to execute a Python fixture directly from a noexec
temporary mount. Its retained failure was corrected by invoking the existing
Python interpreter; the mount restriction stayed in place. A separate remote
quoting failure occurred before operations and is retained. The complete music
captures were scored once, without rerunning cases to improve the result.

The music runners used one CPU core and 1 GiB limits, with no GPU, audio device,
player or model inference. Apollo used only the isolated CONFLUX Docker daemon,
with no network or device access. Both units ended with empty cgroups; private
containers and PCM spools were removed. No power profile, thermal policy,
OneDrive state or desktop inhibition changed. These software checks provide no
laptop latency, cancellation, physical audio or sustained thermal qualification.

The [evidence manifest](data/keyfinder-global-2026-10-28/manifest.json) binds the complete per-case captures, source/build pins, generated and protocol checks, failed setup diagnostics, unit retirement and scoring order. Audio, binaries, third-party source and caches are excluded.

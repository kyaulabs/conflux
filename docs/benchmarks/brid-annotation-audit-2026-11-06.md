# BRID reference annotation audit

All 93 frozen evaluation references match the publisher's original files exactly: 4,342 beat timestamps and 2,205 position-one labels. Every reference alternates between 1 and 2. Twenty-five recordings start on position two; the frozen references preserve that leading partial bar. No correction to reference parsing or bar-label conventions is warranted by this audit. Automatic downbeat qualification remains open.

The publisher's [annotation archive](https://zenodo.org/records/14051323) matches its advertised MD5. Its second column is loaded as a bar index by the [dataset loader](https://mirdata.readthedocs.io/en/0.3.9/_modules/mirdata/datasets/brid.html#load_beats). A [published meter-tracking study](https://arxiv.org/html/2502.12972v1#S2.SS2) identifies the 93 mixtures as 2/4. The M2/M3/M4 filename prefix counts instruments, rather than beats per bar; every prefix agrees with the publisher's description table: 45 two-instrument, 29 three-instrument and 19 four-instrument recordings.

All cases retain their original timestamps, labels and denominator. This audit makes no shift, label inversion, filtering, octave correction or threshold change, and does not rescore predictions. It rules out a reference-copying or convention mismatch as the explanation for the recorded downbeat failures. It does not establish perceptual annotation correctness, model training independence, calibrated confidence or improved model accuracy.

The augmentation study reports limited unseen-BRID downbeat performance and accent-related errors. Its results do not establish that those models meet CONFLUX's unchanged numeric screens, so no new backend or dependency was selected from that study. The current decoder/frontend and Explore This failures remain recorded in their [comparison](brid-decoder-frontend-2026-11-06.md) [reports](brid-explore-this-2026-11-06.md).

Zenodo's current metadata explicitly states CC BY 4.0. The loader's versioned documentation lists a different license string. This audit retains that discrepancy and attributes the included, unmodified annotation files to the publisher's record. No audio is included or newly downloaded, and no user music was changed.

The [manifest](data/brid-annotation-audit-2026-11-06/verification.json) binds ten members with the original annotation ZIP, publisher metadata/description, fixed protocol, audit result, parser and four generated regression checks. The [offline reproducer](data/brid-annotation-audit-2026-11-06/verify_evidence.py) verifies every digest, runs those checks and compares all 93 references again without extraction, network, model inference, GPU use or audio output:

```sh
python3 -B docs/benchmarks/data/brid-annotation-audit-2026-11-06/verify_evidence.py
```

The checks preserve a leading second beat and reject duplicate/reversed times, unsupported positions and malformed/nonfinite inputs. The reproducer reuses the prior report's pinned bounded archive loader and its existing Python/NumPy environment; its [result](data/brid-annotation-audit-2026-11-06/reproduction.json) is retained. Confidence remains unknown; this audit does not close M2/M3 or authorize release `0.1.0`.

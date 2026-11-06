# Explore This beat/downbeat comparison, 6 November 2026

Reject the Explore This Final0 candidate. It returned valid results on all 93
BRID recordings but reduced beat-screen passes from 81 to 60, joint
beat/downbeat passes from 4 to 0, and downbeat micro F1 from 0.518031 to 0.267928.
The existing backend remains in place. M2/M3 and release `0.1.0` remain open.

| Unchanged numeric screens | Current Beat This Final0 | Explore This Final0 |
| --- | ---: | ---: |
| Completed / fixed denominator | 93 / 93 | 93 / 93 |
| Operational failures | 0 | 0 |
| Beat passes | 81 | 60 |
| Downbeat passes | 4 | 0 |
| Joint passes | 4 | 0 |
| Downbeat micro F1 | 0.518031 | 0.267928 |
| Matched / extra / missed downbeats | 984 / 610 / 1,221 | 439 / 633 / 1,766 |

The candidate gained beat passes on cases 49, 71, 72 and 86 and lost 25 others.
It lost all four baseline joint passes: 10, 30, 45 and 46. Per-case results remain
in the evidence. These counts use every original input and the unchanged scorer:
beat F1 at least 0.90 within 70 ms, matched median error at most 35 ms,
nearest-rank p95 error at most 70 ms, median relative adjacent BPM error at most
3%, and downbeat F1 at least 0.90 within 70 ms. No shift, octave repair, threshold
fitting or post-output exclusion was applied. This is an in-sample diagnostic;
confidence and recording-level training independence remain unknown.

This compares a distinct backend, including its matching frontend, rather than
attributing a change to one component. Source is pinned to
[Explore This revision 408db038](https://github.com/CPJKU/explore_this/tree/408db038b41bdc18104e70c5a8ac8daabd9cd496).
The selected published Final0 checkpoint is 78,039,686 bytes, SHA-256
`ca262c9848f0b8c232bc82eb22bf4f024011487c8155a0b716b98a1608438ed0`.
All 174 state tensors loaded strictly into the unmodified source, which reports
6,502,010 model parameters. The private loader uses `weights_only=True` with only
the inspected standard-library `PosixPath` metadata constructor added; it never
falls back to unrestricted pickle loading.

The candidate receives the same immutable native-prepared stereo 48 kHz PCM as
the baseline. It averages stereo samples, resamples to 22,050 Hz and applies the
upstream log-Mel frontend. Inference uses CPU float32, 1,500-frame chunks and
`keep_first` aggregation. The upstream threshold decoder stays at 0.5 with a
maximum downbeat meter of seven. Its pinned evaluation notebook reports that
default method and a separate optional Viterbi variant; this run implements the
default. The active protocol was not changed after predictions began.

Source eligibility is the exact inequality `k * 48000 < prepared_frames * 50`.
It is applied to returned masks; this decoder performs no peak pooling. Every
retained raw mask reconstructs the encoded source-frame events exactly, and all
prepared PCM identities match the fixed input plan. The private response recipe
`explore-this-final0-threshold-diagnostic-v1` was not enabled in the player or
accepted as production analysis.

Seven generated compatibility checks passed, covering meter-state paths for
2/3/4/7, state mapping, empty-grid threshold output and frontend padding. The
optional Viterbi path raised `IndexError` for an empty grid; that failure remains
recorded and no Viterbi output is included in these scores. The first container
attempt failed on source-directory permissions before the worker ran; the
corrected attempt used a new owned container and explicit read/traverse modes.
Neither setup result is musical-accuracy evidence.

The fixed first-case pilot completed on the native PCM identity without reading
accuracy annotations. The full run reused that unscored pilot and inferred the
remaining 92 cases. Its owner completed in 1,840.738 seconds, reaped its client
and retired the container. All predictions were saved before local reference
scoring began. The dedicated CONFLUX daemon was then empty. CPU limits were two
cores and 4 GiB; source, model, media and runtime assets were read-only, with no
container network or GPU access. No new Python inference package was installed.

The [manifest](data/brid-explore-this-2026-11-06/verification.json) binds 899 files
in a 558,632-byte archive: raw predictions and masks, native source identities,
producer code, protocols, original scorer functions, the baseline, complete
results, setup failures and cleanup records available at collection. The raw
files passed Gitleaks before compression. No audio, PCM, model weights or
executables are included. The MIT source notice and BRID CC BY 4.0 attribution
are retained. The
[offline reproduction](data/brid-explore-this-2026-11-06/reproduction.json)
re-scores all 93 candidate and baseline cases and checks each raw-mask protocol.
From the repository root, using an existing Python/NumPy installation:

```sh
python3 -B docs/benchmarks/data/brid-explore-this-2026-11-06/verify_evidence.py
```

The verifier reuses the bounded archive/scorer loader beside the earlier
[decoder/frontend comparison](brid-decoder-frontend-2026-11-06.md), pinned by
SHA-256. It extracts no files and starts no model, GPU, audio or network work.
The rejected model and completed Apollo staging are removed after archive
verification; the original corpus, shared runtime, user music and recordings are
preserved. The [cleanup receipt](data/brid-explore-this-2026-11-06/cleanup.json) records those removals.
Compact evidence remains available for review.

No laptop workload, power-profile change, thermal-policy change or OneDrive
pause was involved. This supplies no live-stem latency, physical-audio,
cancellation or sustained UI qualification. Best quality and the strict
4.095-second primary-laptop p99 target remain unchanged, as does the requirement
to complete full M2/M3 qualification before publishing `0.1.0`.

# Local-key screen on Beethoven performances

The unchanged dense Winterreise Key-CNN matched **66.81% of annotated local-key time** on both hosts. All eight selected performances decoded and completed inference. Individual results ranged from 51.11% to 82.85%; the overall result does not justify enabling automatic key detection. Confidence remains Unknown, the production key backend stays disabled, and M2/M3 and release gates remain open.

| Check | Laptop | Apollo |
| --- | ---: | ---: |
| Complete source and inference cases | 8/8 | 8/8 |
| Exact tonic/mode duration | 66.81% | 66.81% |
| Estimated coverage of annotated duration | 99.99% | 99.99% |
| Source preparation wall time | 182.590 s | 249.578 s |
| CPU capture wall time, including preparation | 586.189 s | 551.918 s |

| Composition | Performance | Exact annotated duration |
| --- | --- | ---: |
| Op049No2-01 | AS35 | 74.51% |
| Op049No2-01 | FG58 | 82.85% |
| Op109-01 | AS35 | 51.11% |
| Op109-01 | FG58 | 60.79% |
| Op014No1-01 | AS35 | 66.56% |
| Op014No1-01 | FG58 | 65.99% |
| Op010No2-01 | AS35 | 58.05% |
| Op010No2-01 | FG58 | 72.52% |

The [Beethoven Piano Sonata Dataset](https://zenodo.org/records/18662551) supplies time-resolved local-key annotations. Credit: Johannes Zeitler, Christof Weiß, Vlora Arifi-Müller and Meinard Müller; the selected annotation CSVs retain the publisher's CC-BY-3.0 attribution. The [dataset paper](https://doi.org/10.5334/tismir.196) describes the collection. This screen covers four compositions, each in versions AS35 and FG58. Eight performances are not eight independent compositions, and classical piano does not establish accuracy for the requested DJ genres.

Selection was fixed before new audio, labels or predictions were acquired. Composition filenames were ordered by SHA-256 with a recorded seed; the first four eligible works were retained. Eligibility required both fixed versions, matching local-key/active-interval files and an 18 MiB per-source bound. The previously inspected Op. 2 No. 1 pilot was excluded. Op. 109 was retained with the publisher's warning about partly incorrect harmonic/structure measure counts. It was not replaced or removed from the aggregate.

All eight original WAVs were acquired through bounded HTTPS ranges against the published ZIP directory. The admitted files retained their extent, ZIP CRC32 and SHA-256. The full archive MD5 was not computed. Both hosts then prepared each complete uncropped source as stereo 48 kHz float PCM; every frame count and complete PCM hash matched exactly. No source audio is redistributed here.

The checkpoint, legacy CQT, 60-frame centered contexts, boundary padding and hop-8 source partition are unchanged from the [earlier dense screen](independent-keycnn-2026-10-29.md). Each local cell uses its unthresholded top-one tonic/mode. The primary score measures Decimal overlap against all well-formed annotated intervals: 2361.660 seconds across eight cases. Unknown or failed predictions retain the known-duration denominator; gaps remain unlabelled. No time shift, boundary tolerance, transposition, selected excerpt, setting or confidence threshold was fitted. The scoring algorithm and failure rules were frozen before inference; report execution glue was written after capture launch. This does not establish per-sample musical boundary accuracy or confidence calibration.

Eight generated scoring checks passed per host, covering fractional boundaries, unknown/failed outcomes, unlabelled gaps, invalid references, truncated/overlapping source windows and exact mode/enharmonic classes. Native rejection checks and seven decoder protocol checks passed per host; the existing classifier pipeline passed twenty-three generated protocol/lifecycle checks on each host. Both numerical runtime manifests matched before and after inference. No package or model was installed or changed.

All 38,928 compared posterior elements stayed within the inherited `atol=2e-6, rtol=2e-6` descriptive bound; the maximum absolute difference was 1.847743988e-06. All projected local class decisions agree. This numerical agreement does not establish musical accuracy. Training membership remains unverified, and the earlier failed whole-track FMAK screens remain recorded separately.

Both CPU batches, their original cgroups and the private Apollo container were retired before scoring. Anonymous PCM spools were empty. These runs started no player, hardware sound output or GPU and changed no sync service, power policy, thermal rule or Best-quality stem setting. They do not qualify the primary laptop's strict p99 target below 4.095 seconds, hard cancellation or the current-player hour.

The [evidence manifest](data/bpsd-local-key-2026-10-29/manifest.json) binds 221 unique gzip text members and 100 byte-identical aliases, including complete predictions, raw selected annotations, frozen scoring code, acquisition/source/runtime pins, checks and owned cleanup. Audio, weights, binary arrays, installed runtimes and generated caches are excluded.

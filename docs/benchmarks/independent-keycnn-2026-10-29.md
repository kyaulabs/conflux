# Key-CNN on the independent artist sample

The unchanged dense Winterreise recipe matched **11/24 global references**
(45.83%) on both hosts, or 11/23 decoded tracks (47.83%). The rejected MP3 remains
in the original denominator. On the same sample, the earlier whole-track
KeyFinder recipe matched 12/24. This screen provides no reason to promote the
Key-CNN candidate. The production key backend stays disabled, confidence stays
Unknown, and M2/M3 and release gates remain open.

| Recipe | Decoded sources per host | Exact global matches | Original denominator |
| --- | ---: | ---: | ---: |
| Whole-track KeyFinder | 23 | 12/23 | 12/24 |
| Dense Winterreise Key-CNN | 23 | 11/23 | 11/24 |

The [fixed selection](independent-key-2026-10-29.md) has 24 artists absent from
the prior screen, 22 selected key classes and 21 classes among the decoded
sources. Its metadata exposed reference labels, and the KeyFinder results were
known before this comparison was chosen. No Key-CNN settings were fitted to this
sample. The original source `003956` remained rejected and was neither converted
nor replaced. Every successful capture decoded the original source again and
matched its complete stereo 48 kHz float32 frame count and PCM SHA-256.

This is the existing `winterreise_n_fold16` model, SHA-256
`b425f2e8d5165415c8863b6666aa82d7573121ee675bd2fd8b59d7594f5f11e9`,
with legacy CQT, dense hop-8 contexts and boundary zero padding. One global
decision uses the arithmetic mean of every raw posterior row with float64
`math.fsum`; quiet rows remain included, all-zero feature populations are
Unknown, and ties choose the lowest class index. The publisher's global defaults
use hop 30 without boundary padding. That alternate context recipe was not part
of this capture. Neither this result nor the separate DeepSpec experiment
qualifies every Key-CNN model or explains these errors.

The weight set's [earlier numerical reference](keycnn-winterreise-reference-2026-10-28.md)
passed 99/99 generated layer/output comparisons against TensorFlow 1.15.2.
For this new host pairing, the existing Python 3.10.22/CQT environment was copied
into CONFLUX's private Apollo directory. All 8,585 regular files retained their
hashes; 1,054 internal symlinks were rebound within that copied tree. No package
version was changed or newly installed, and no duplicate runtime archive was
stored on the laptop. Model loading reads only the audited raw float tensors;
the HDF5 file is hashed without deserializing model classes.

Both environment checks passed the frozen generated fixtures:
normalization was bit-exact, six CQT cases differed by at most
5.7731597281e-15, and three posterior batching checks differed from the retained
fixtures by at most 3.2782554626e-7. These checks use the original combined
posterior tolerance `atol=2e-6, rtol=2e-6`; they do not repeat the full 99-layer
reference or establish musical accuracy. The initial asset freeze rejected the
existing Python symlink as a regular file. Resolving its internal target and
checking the original binary hash fixed that setup check before inference.

Both full music captures completed all 23 usable sources, with no inference
failure or altered PCM. All 23 global decisions agreed across hosts. The largest
posterior difference was 2.0861625671e-6, with zero elements outside the frozen
combined absolute/relative bound. Emitted prediction files are not claimed
byte-identical. Both captures and owned resources closed before the scorer read
the reference mapping. No reference file was mounted into the classifier.

An incorrect global estimate had a maximum mean posterior of 0.776.
The recorded probability bins are descriptive; no confidence threshold was
fitted. These song-level labels supply no local key-change truth, confidence
calibration or proof of upstream training independence.

Twenty-three protocol and process-lifecycle regressions passed on each host
before music classification. The batches used one CPU and 3 GiB limits;
the retained execution durations were 1,206.336 seconds on the laptop and
1,231.565 seconds on Apollo. Those durations include preparation and are not
live-stem latency measurements. All source/runtime pins remained unchanged.
Both units have empty original cgroups, all owned containers are removed, and
no PCM spool remains. The tests made no player, sound-output, GPU, power-policy,
OneDrive or desktop-inhibitor changes. They consume no physical client-test
readiness and close no sustained audio/UI, cancellation or release gate.

The [evidence manifest](data/independent-keycnn-2026-10-29/manifest.json) binds
244 unique compressed text members and 91 byte-identical
aliases, including complete per-case predictions, source/PCM and runtime pins,
generated checks, failed setup records, frozen code and owned cleanup. Audio,
weights, numeric arrays, installed runtimes, native binaries and JIT caches are
excluded. The receiver with an embedded manifest is represented by its hash,
the complete manifest and its generating script.

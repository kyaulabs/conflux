# Independent-artist global key screen

The unchanged whole-track KeyFinder recipe matched 12 of 23 decoded tracks
on each host (52.17%), or 12 of the original 24 selections (50.00%)
with the rejected MP3 retained. All 23 emitted prediction files were byte-identical
across hosts. This new artist sample provides no calibrated confidence or
local key-change truth. The production key backend
stays disabled, confidence stays Unknown, and M2/M3 and release gates remain open.

| Host | Exact matches among decoded tracks | Original denominator | Source failures |
| --- | ---: | ---: | ---: |
| Laptop | 12/23 | 12/24 | 1 |
| Apollo | 12/23 | 12/24 | 1 |

The selection excludes every artist and track in the earlier 24-song FMAK screen.
Its seed, `conflux-fmak-independent-artists-20261029-v1`, was fixed before audio
acquisition. Selection used metadata, class coverage and artist identity without
model scores or listening. The original attribution-only license screen left
32 tracks from 23 unused artists covering fourteen global key classes. Including
the recorded CC BY-SA 3.0/4.0 and CC BY 3.0 US license entries gave 186 eligible
tracks from 46 unused artists. The selection retains each track's artist, title,
source URL and license; audio is excluded from this evidence and product artifacts.
The applicable license deeds are linked in the selection record.

Those candidates cover 23 classes, but the only A-flat-major and B-flat-major
candidates share one artist. Matching one artist per class therefore reached
22 classes; two seeded unused artists filled the fixed 24-track sample. The
construction error that initially assumed 23 distinct artists was caught and
retained before the selection froze. B-flat major and B-flat minor are absent.
The later rejected B-minor source leaves 21 classes among the decoded tracks.
This metadata-balanced sample cannot establish general genre accuracy or
independence from an upstream model's training data. Its selection metadata
exposed reference labels; no classifier settings were changed after selection.

All 24 original MP3s passed bounded source admission, ZIP CRC32 and SHA-256 checks.
Two incomplete HTTP range responses were rejected, with the original failures
retained; smaller bounded ranges completed the same sources without replacement.
The first private decoder build omitted a required header and failed on both
hosts. Adding the unchanged product include closure fixed the helper build.
The product audio decoder itself was not modified.

Both hosts decoded 23 sources to exactly the same complete stereo 48 kHz float32
PCM hashes and frame counts. Both rejected `003956`, Miss Derringer's
"Better Run Away From Me," as `Format not recognised`. A header-only FFprobe check
recognized a stereo 44.1 kHz MP3; it does not prove complete decoding or erase the
native failure. This source was neither converted nor substituted. Decoder
durations are CPU correctness observations, without latency qualification.

The [existing stock-profile recipe](keyfinder-global-2026-10-28.md) and pinned
worker binary were reused. Each successful key capture re-decoded the original
source and checked its complete PCM identity before one global decision.
Scoring waited until both captures closed and both owned units were retired.
No reference file was mounted into Apollo's inference container. Byte-identical
emitted predictions compare keys and metadata, not unrecorded internal chroma.

Twelve protocol and process-lifecycle regressions passed on each host before
inference, including timeout, preparation failure, malformed output, full-clock
binding and stopping after unproven child cleanup. The fixed batches retained
every outcome. Both ran with one CPU and 1 GiB limits and no GPU or sound output;
Apollo used only the isolated CONFLUX daemon with no network or devices. Both
units and the private container are retired, with empty cgroups and no retained
PCM. Root-owned service logs initially blocked collection of the earlier decode
batch; a bounded read recovered those exact logs without another decode run.
No power setting, thermal guard, OneDrive state or desktop inhibitor changed.

The [evidence manifest](data/independent-key-2026-10-29/manifest.json) binds
536 compressed text records: selection and attribution, complete source
and PCM pins, all per-case results, failed acquisition/build/collection attempts,
recipe and protocol code, scoring order and resource cleanup. These software
checks consume no physical client-test readiness and provide no laptop service,
cancellation, physical audio or sustained thermal qualification.

# Stem frame-count CPU check, 26 October 2026

The live-stem converter counted output frames by resampling discarded stereo zeros with the Best filter. The candidate uses the shorter sinc filter for this count only. Actual stem samples still pass through `SRC_SINC_BEST_QUALITY`; the fixed ratio, stereo counting shape, bounded buffers, cancellation checks and output budget are unchanged.

One generated 23.4-second fixture produced these offline timings on libsamplerate 0.2.2:

| Operation | Original CPU time | Candidate CPU time |
|---|---:|---:|
| Count 1,031,940 input frames | 3.176 s | 0.438 s |
| Convert the four actual stems with Best | 5.770 s | 6.106 s |
| Best conversion from a seek at output frame 280,800 | 4.439 s | 4.265 s |

The discarded counting pass used 86.2% less CPU in this single fixture. These serial prototype measurements used generated PCM without a player, audio device, model or GPU. They do not establish an overall speedup or a change in service p99. The actual Best conversion implementation did not change.

Exact frame geometry matters: this fixture produces 1,123,201 frames, one more than the rounded rate ratio. Ten longer boundary checks, including three minutes of input, matched the actual complete-file Best preparation. A mono counting prototype failed this comparison and was rejected. The retained stereo candidate follows the common sinc stereo phase/EOF path in the [pinned libsamplerate source](https://github.com/libsndfile/libsamplerate/blob/c96f5e3de9c4488f4e6c97f59f5245f22fda22f7/src/src_sinc.c); no upstream code or new dependency was added.

The product converter and live-bridge regression suites passed after the change. They cover exact PCM against complete-file Best preparation, partitions, gaps, seeks, EOF, invalid inputs, budgets and cancellation. The converter suite now includes the exact four-hop live source in its frame-count and budget checks. These checks establish the tested geometry and audio behavior, not a guarantee about every future library version.

This offline check did not include a client run. A [subsequent player check with fresh user readiness](frame-count-player-2026-10-26.md) completed 120.006 seconds but reported 9.767-second service p99 and steady starvation. Best quality, the 15 W ceiling, thermal protection, the 4.095-second target, the completed-hour requirement and all M2/release gates remain unchanged.

The small text-only evidence set and its hashes are listed in [verification.json](data/m2-stem-frame-count-2026-10-26/verification.json).

# Track-preparation CPU profile

The existing eight-minute quiet stereo 48 kHz WAV prepared correctly. A complete
prepared-store cache miss took **0.851 seconds wall / 0.781 seconds thread CPU**;
a cache hit took **0.351 / 0.348 seconds**. Direct decode plus waveform overview
took **0.195 / 0.194 seconds**. These are single observations on this fixture,
not processing percentiles or a live-stem service result.

The frozen baseline is commit `b39d26929fa7dc747f10537af7d36501758f20b4`. The standalone QCore
probe used the project decoder and prepared-store implementation on 23,040,000
frames. Prepared PCM and all 512 waveform bins matched the direct reference
exactly. The source digest remained unchanged, and the store missed before its
subsequent hit. This run does not measure the later waveform-cache or MPEG
pipeline-revision changes.

| Stage | Thread CPU (ms) | Wall time (ms) |
| --- | ---: | ---: |
| Source SHA-256 | 91.625 | 91.889 |
| Decode and overview | 193.923 | 194.734 |
| PCM finite/peak/hash scan | 166.416 | 166.645 |
| PCM copy/finite/peak/hash | 207.776 | 209.590 |
| Copy fsync | 15.550 | 73.476 |
| Copied PCM verification | 175.867 | 176.302 |
| Complete store: cache miss | 780.867 | 850.751 |
| Complete store: cache hit | 347.837 | 351.136 |
| New store instance: PCM validation | 165.949 | 166.242 |
| Final source verification | 87.449 | 87.618 |

Each row is a separate synchronous API call. Store spans include their own
internal hashing, conversion and validation, so the rows are not additive.
Earlier calls also warm filesystem caches; this is not a cold-disk comparison.
It bounds the measured operations for this quiet WAV, rather than attributing
the actual player's long GPU waits. BPM/harmony analysis, stem inference and
UI work were outside the probe.

The runner first required sixty continuous seconds with all sampled temperatures
at or below 55°C. Across its 43 observations, the maximum
sampled temperature was **73°C**, with no sample at or
above 85°C. The observed profile was balanced. The existing thermal guard and
15 W ceiling stayed in force; no profile, cap or guard writes occurred. This
short diagnostic does not establish sustained thermal behavior.

The native result closed 115.797 seconds after the fresh readiness receipt.
Cleanup was verified after **125.074 seconds**, within the original 300-second
window. All **106 frozen assets** still matched. The owned unit/process group
is retired, its sleep inhibitor is released, and private prepared files were
removed. The native child was reaped and the source stayed unchanged.

The [evidence manifest](data/m2-preparation-cpu-2026-10-27/manifest.json) retains
the frozen plan, runner/native source, offline checks, readiness/dispatch
records, complete temperature observations, stage measurements and cleanup
proofs. It excludes the source WAV, prepared PCM and executable.

Audio output, seeks and cancellation were not exercised. The actual-player
baseline remains **8.889-second p99** against the unchanged **4.095-second**
target. Best quality, the 15 W ceiling, M2/M3 and release gates remain unchanged.

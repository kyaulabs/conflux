# Flash-attention row-tiling diagnostic

The 25 October 2026 UTC four/eight-row and four/two-row comparisons missed the
unchanged service target of p99 < 4,095 ms with every runtime. Both candidates
also starved during steady playback and remain unpromoted. Best quality, the normal runtime,
M2's sustained-testing requirement and the `0.1.0` release gate remain unchanged.

The first pair used the same player built from
`119a579e4ca717a1d59d7a7a6f9752534d68dcaf`, default visible UI, source excerpt,
gains, two runtime CPU threads and sixteen automated control changes. The
intended runtime difference was four versus eight independent query rows per
Flash-attention tile for the four Intel 64-wide full-F32 attention shapes.
Shader modules, 32-column key tiles, workgroup, reduction, precision, graph and
coarse GPU timestamp instrumentation stayed the same. The runs used the public
service and measured the complete source-read/request/overlap path.

| Visible-player result | Four-row baseline | Eight-row candidate |
|---|---:|---:|
| Measured wall interval | 120.017 s | 120.008 s |
| Published service hops | 21 | 17 |
| Service median | 3,561.181 ms | 5,255.574 ms |
| Nearest-rank service p99 and maximum | 7,308.094 ms | 7,607.781 ms |
| Hops at or above 4,095 ms | 10 / 21 | 10 / 17 |
| Steady starvation at 48 kHz | 0 frames | 181,248 frames / 3.776 s |
| Seek/replay recovery starvation | 1,443,072 frames / 30.064 s | 1,317,888 frames / 27.456 s |
| Peer-deck starvation | 0 frames | 0 frames |
| Automated control acknowledgements | 16 / 16 | 16 / 16 |
| Healthy visible-window observations | 9,370 / 9,370 | 7,350 / 7,350 |
| Recording frames accepted = written | 6,192,640 | 6,074,624 |

All thirteen service fields covered every published hop, with no observation
loss and a final `ServiceClosed` record. At the slowest hop, graph compute took
7,029.978 ms of the baseline's 7,308.094 ms total and 7,318.268 ms of the
candidate's 7,607.781 ms total. This pair retained the standard service fields;
it does not provide separate GPU-only interval measurements.

The incomplete-generation seek recovered within a poll-observed issue-to-cursor
bound of 6.855 seconds for baseline and 7.881 seconds for candidate. Baseline's
four replay bounds were 4.359, 8.225, 7.107 and 3.797 seconds; candidate's three
were 5.015, 7.374 and 7.459 seconds. Both superseded an accepted generation
before its observed `RawComplete`. Native inference-start was not observed, so
executing GPU preemption remains unverified.

Recording overflow, callback execution deadline misses, invalid buffers,
rejected commands, source-read failures, driver recovery flags and driver
position discontinuities were zero in both runs. Independent WAV scans found
all 12,385,280 baseline and 12,149,248 candidate float samples finite, with frame
counts, nonzero counts and peaks matching the native reports. Recording
includes setup audio. These integrity checks do not remove starvation or
measure physical output latency, and no new listening verdict was requested.

Neither launcher selected a power profile or wrote power limits. Both waited
for natural balanced-mode admission with the unchanged thermal guard. All 82
baseline and 71 candidate read-only cap snapshots stayed within the retained
ceilings; the MMIO long-term configured limit ranged from 7.5 to 15 W. Runtime
profile observations show the guard subsequently reduced both runs to low-power
mode. The guard logged an 88°C baseline warning and a 97°C candidate warning
and critical entry. The candidate's two-second host observations included one
97°C sample; they do not establish continuous high-temperature duration.
Configured limits are not measured consumption or effective hardware limits.

The baseline wrapper retained its strict-target failure. Candidate retained
its native steady-starvation failure and subsequent wrapper control-validation
failure, despite all sixteen control acknowledgements. Complete measurement,
capture, final input hashes and guard identity were verified independently.
Earlier root cleanup checks misread an empty post-exit `ControlGroup` property
as `/sys/fs/cgroup` and falsely reported that the owned scope remained. No root
cgroup was altered. Separate host checks proved the original recorded unit
scopes, units, owned process lifetimes and scoped KDE idle blockers gone.
Baseline closure preceded candidate launch. The pair
finished within its original 20-minute human authorization without renewing
readiness. No original failed receipt was rewritten.

The silent prerequisite matched all sixteen candidate outputs exactly against
the normal Best-quality reference and confirmed eight-row full-F32 pipeline
activation. Its native-only median was 1,550.115 ms and p99/maximum 1,802.095 ms;
it did not exercise the player, audio, UI or service target. Twenty extracted
selector/shared-memory cases passed, as did five dependency-parser checks,
25 silent-launcher methods and fifty player-launcher methods for each arm.
Those offline checks used generated fixtures and add no laptop qualification.

The [verification record](data/m2-flash-row-tiling-2026-10-25/verification.json)
binds 75 compressed UTF-8 metadata, source, patch and log files to their original
hashes. It preserves build-audit and fixture corrections alongside the completed
measurements and cleanup. No audio, PCM, model, executable or driver cache is
published. Fixed order and different thermal histories limit causal comparisons;
these short diagnostics establish no sustained pass or runtime promotion.

The later two-row candidate changed the same four Intel shapes to two query
rows per tile, reducing the selector's shared-memory requirement from 4,304 to
3,504 bytes. Twenty extracted selector/shared-memory cases passed. All sixteen
complete Best-quality outputs matched the normal reference exactly, and the
silent GPU capture confirmed two-row full-F32 activation. Native-only median
was 1,593.025 ms and p99/maximum 1,717.472 ms. This prerequisite did not exercise
audio, UI, service delivery or cancellation and does not qualify latency.

The first two-row player attempt stopped before any model, audio or UI began.
Natural guard recovery arrived 1.682 seconds after its 120-second admission
cutoff. The candidate was not dispatched. A separate retry increased only that
initial allowance to 150 seconds, within the unchanged 270-second preparation
and 20-minute shared budgets. The full 60-second cooldown and all temperature,
power, cleanup and service rules stayed unchanged. A regression reproduced
122-second recovery followed by the full cooldown: it failed with the old
allowance and passed with the fix. Fifty-one offline launcher methods passed
for each arm, along with both frozen-input preflights. The stopped attempt and
original failed receipts remain in the evidence.

The corrected pair reused the same player binary and compiled-source bytes as
the first pair; the intervening reporting commit changed documentation only.
It completed both visible-player runs with the same workload, four-row baseline
first, then two-row candidate. Neither strict service verdict passed.

| Later visible-player result | Four-row baseline | Two-row candidate |
|---|---:|---:|
| Measured wall interval | 120.007 s | 120.011 s |
| Published service hops | 17 | 17 |
| Service median | 6,070.804 ms | 2,460.792 ms |
| Nearest-rank service p99 and maximum | 7,262.560 ms | 7,508.171 ms |
| Hops at or above 4,095 ms | 9 / 17 | 7 / 17 |
| Steady starvation at 48 kHz | 0 frames | 258,816 frames / 5.392 s |
| Seek/replay recovery starvation | 1,467,904 frames / 30.581 s | 1,071,360 frames / 22.320 s |
| Peer-deck starvation | 0 frames | 0 frames |
| Automated control acknowledgements | 16 / 16 | 16 / 16 |
| Healthy visible-window observations | 7,163 / 7,163 | 8,271 / 8,271 |
| Recording frames accepted = written | 6,062,848 | 6,065,408 |

All thirteen service fields and final closure were retained without observation
loss. The slowest baseline hop spent 6,898.689 ms in graph compute of 7,262.560 ms
total; candidate spent 7,126.223 ms of 7,508.171 ms there. The incomplete-generation
seek recovered within poll-observed bounds of 8.840 seconds and 6.091 seconds.
Baseline's three replay bounds were 4.159, 7.923 and 9.914 seconds; candidate's
were 2.695, 5.433 and 8.320 seconds. Generation supersession was observed;
executing GPU preemption remains unverified.

Both recordings passed independent finite-sample, count and peak checks with
zero overflow. Callback execution deadline misses, invalid buffers, rejected
commands, source-read failures, driver recovery flags and position
discontinuities were zero. These integrity results do not remove the measured
starvation or establish physical output latency. No fresh listening verdict
was requested. Both original wrapper failures and the candidate's native
steady-starvation failure remain recorded.

All seventy read-only cap snapshots per arm stayed within the retained ceilings;
the configured MMIO long-term limit ranged from 7.5 to 15 W. Neither launcher
selected a profile or changed the guard. Guard entries recorded 97°C for
baseline and 99°C for candidate, followed by natural recovery. Both runs changed
from balanced to low-power mode. Two-second host observations contained one
sample at or above 95°C per arm; these point samples establish no continuous
high-temperature duration. Both original owned cgroup paths were absent at
final verification, and all units, process lifetimes and scoped idle blockers
were gone within 608.153 seconds of the original shared readiness.

The [two-row verification record](data/m2-flash-two-row-2026-10-25/verification.json)
contains the silent gate, stopped admission, recovery regression, completed
pair, independent recording summaries and cleanup correction, with hashes for
the compressed UTF-8 evidence. Audio and binaries remain private. Fixed order
and different thermal histories preclude a causal speedup claim. Both tile
candidates remain unpromoted; M2 and the first release remain gated.

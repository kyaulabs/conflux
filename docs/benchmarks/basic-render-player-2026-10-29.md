# Basic render-loop player diagnostic

The laptop still failed the unchanged 4.095-second Best-quality service target.
Its retained completed-hop p99 was 7.912 seconds, with steady starvation and an
unfinished final replay recovery. The exact native process used Qt's basic
render loop. Apollo's unchanged private virtual-audio control reached 0.843
seconds without steady starvation. Neither short capture qualifies the laptop
hour, hard cancellation or any M2/M3/release gate; no UI setting was promoted.

| Observation | Laptop | Apollo control |
| --- | ---: | ---: |
| Closed measured interval | 120.014 s | 120.005 s |
| Retained completed service hops | 21 | 23 |
| Nearest-rank service p99 / maximum | **7.912 s** | 0.843 s |
| Hops at or above 4.095 s | **11** | 0 |
| Steady starvation | **4.192 s** | 0 s |
| Seek/replay starvation | 27.675 s | 14.272 s |
| Accessible controls acknowledged | 16/16 | 16/16 |
| Scheduled replays recovered | 3/4 | 4/4 |
| Callback execution deadline misses | 0 | 0 |
| Driver discontinuities / recovery flags | 0 / 0 | 0 / 0 |
| Recording overflow frames | 0 | 0 |
| Recorded stereo frames accepted and written | 6,026,240 | 6,269,440 |

The complete observations retain all late work: the laptop published three of
its 21 hops during the diagnostic tail after the measured interval closed.
These small populations make nearest-rank p99 equal the maximum. The laptop's
final scheduled replay had no recovered-cursor acknowledgement by the measured
boundary. Its later EOF/idle-close tail also failed audio health, adding 30,464
starved frames (0.635 seconds) outside the already finalized WAV. That failure
and every late hop remain recorded. The table is diagnostic evidence from a
failed player test, not an admissible sustained-performance qualification.

Both hosts proved a seek while the previous generation remained unfinished,
discarded one stale packet and retained sixteen control acknowledgements.
Hard worker termination was not exercised. Both recordings finalized as finite
stereo 48 kHz float32 WAVs with matching frame/header counts and no overflow.
Invalid buffers, rejected commands, source-read failures and scene errors were
zero. Recording integrity does not remove the retained starvation or failed
recovery. Apollo's offscreen software UI and private audio graph provide
secondary-machine evidence only.

The probe reused the frozen O2 player at source
`48b82030fcf6ab768c4f7b89d65021485cbf9b40`, unchanged Best model/runtime,
tracks, cache seed and 16 ms forced redraw. It requested `QSG_RENDER_LOOP=basic`
on the laptop while leaving graphics-backend selection at its existing default.
Startup `qt.scenegraph.general` logging was the diagnostic addition; Apollo's
control stayed unchanged. The retained earlier default-loop run reached 9.045
seconds, but did not directly log its actual loop. This single follow-up does
not establish a causal improvement over that run.

Qt routed its startup messages to journald, leaving both native stream files
empty. The original stream-only assessment therefore remained unconfirmed.
The original user-unit journal contains `basic render loop` from the exact
native PID 2089768, executable, UID, unit and invocation, within its owned clock
window. The derived proof checks all those identities and rejects nine altered
identity/category/message/clock cases. The original empty-stream assessment is
preserved; no workload was rerun to obtain this proof. Qt documents the loop
selection and logging category in its [scene-graph reference](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html).

The laptop had one sampled 97°C critical reading. The unchanged guard journal
records its immediate Power Saver response and later natural recovery.
Eleven of 75 host samples showed Balanced/15 W; 64 showed the guard's 7.5 W
allowance. The five hops completed against preceding Balanced samples ranged
from 1.964 to 2.996 seconds, with no target misses. Sixteen completed against
preceding Power Saver samples, including all eleven misses. This association
does not isolate the cause. The largest graph-compute interval was 7.598 seconds
inside the worst 7.912-second hop. Stage timings include waits and are nested;
they must not be added together.

The frozen packet passed 56 offline checks and the actual unarmed KDE/systemd
entry before fresh readiness. Both hosts launched concurrently under the
original twenty-minute budget. The approved OneDrive pause held through native
cleanup, then its original active state was restored. The same restored PID and
invocation remained active after retiring the laptop unit, without another
start. Both exact test units are gone with empty cgroups; private containers are
absent and KDE's temporary inhibitors are released. The guarded wrapper's
`passed=true` records capture/cleanup/restoration success, while native and
latency failures remain false. No power, thermal, quality or milestone policy
changed.

The [evidence manifest](data/basic-render-player-2026-10-29/manifest.json) binds the frozen launch packet, complete two-host observations, failed playback/tail results, source/build pins, original journals, derived render-loop proof, recording audits and resource restoration. Audio, binaries, source trees and derived caches are excluded.

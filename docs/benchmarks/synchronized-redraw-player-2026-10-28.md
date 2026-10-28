# Synchronized laptop/Apollo redraw comparison — 2026-10-28

Both two-minute laptop runs missed the strict 4.095-second service-hop p99 target. At 16 ms redraw, p99 reached 9.395 seconds with steady-audio starvation. At 50 ms, p99 reached 6.236 seconds with no steady starvation. Apollo passed both short service-target checks using private virtual audio. M2's laptop gate remains open; these runs do not qualify a release or sustained operation.

Both host owners started concurrently from one fresh human reply at 05:24:02.845 UTC. Each host ran 16 ms, then 50 ms, without retries. Both pairs completed and cleaned up within the original 20-minute window. The autonomous laptop controller dispatched and closed both variants without an assistant check-in between them. Future matching tests can run concurrently across hosts, with variants sequential on each host.

All four runs used source `cbeefc0a2834681085efaae335e965449d565eb0`, Best-quality stems and identical source, input-PCM and model hashes. Each variant received a separate copy of the pinned driver-cache seed. The laptop retained its normal 15 W ceiling and thermal guard, which reduced both workloads to power-saver mode. No profile or power cap was written by the test. Apollo used its NVIDIA runtime, private PipeWire audio and offscreen software UI. Its results cannot substitute for the visible-window, host-audio laptop gate.

| Host | Redraw | Hops | p99 | Hops ≥4.095 s | Steady starvation | Control/recovery starvation |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Laptop | 16 ms | 17 | 9.395 s | 5 | 8.757 s | 19.797 s |
| Laptop | 50 ms | 21 | 6.236 s | 5 | 0 s | 20.528 s |
| Apollo, virtual audio | 16 ms | 22 | 1.063 s | 0 | 0 s | 14.293 s |
| Apollo, virtual audio | 50 ms | 22 | 0.986 s | 0 | 0 s | 14.395 s |

Counts, target misses and nearest-rank p99 were recomputed from every raw `hop_service_present` event. For these sample counts, p99 is the maximum. Starvation was independently summed from closed native intervals at 48 kHz. All measured wall intervals completed 120 seconds. The laptop 50 ms capture retained one additional completed hop during its unrecorded tail; no completed hop was removed from the service distribution.

The 50 ms laptop result was better in this pair, but it still failed the target. One fixed-order pair, different thermal histories and different delivered-hop counts do not establish a causal redraw improvement. The 16 ms laptop GPU graph-compute maximum was 9.098 seconds. Graph execution accounted for most of that run's longest request; the capture does not isolate a kernel or prove a hardware limit.

All 16 automated controls were acknowledged in each run. Each exercised a seek while its earlier generation was incomplete and validated one discarded stale packet. This uses retrospective C++ acceptance/completion stamps and makes no native inference-start claim. Hard cancellation was not exercised; no hard-cancellation latency is claimed. The laptop completed three replays at 16 ms and four at 50 ms; Apollo completed four per arm.

All four runs reported zero source-read failures, invalid audio buffers, rejected controls and callback execution-deadline misses. Every WAV finalized with accepted frames equal to written frames and zero recorder overflow. Recording correctness does not remove the delivery gaps above. Native functional status failed for laptop 16 ms and passed for laptop 50 ms and both Apollo arms. Both laptop wrapper assessments failed their combined gates. The ordinary assessor also reported a wrapper functional-completion mismatch; raw native flags and failed wrapper flags remain separate in the evidence.

The maximum sampled laptop temperature was 95°C at 16 ms and 88°C at 50 ms. The first capture had four samples at or above 85°C, including three at or above 95°C; its longest consecutive high-sample span was about 2.006 seconds. The second had one sample at or above 85°C and none at or above 95°C. Two-second sampling neither establishes continuous heat exposure nor excludes shorter spikes. Thermal-guard reductions were preserved through cleanup.

Both owners exited, owned Apollo containers were removed and temporary KDE display/power inhibitors were released. This test window is closed. Sustained laptop evidence, hard-cancellation qualification and the other M2 preparation gates remain outstanding.

The [evidence manifest](data/synchronized-redraw-player-2026-10-28/manifest.json) records original-capture, decoded-text and gzip hashes for 20 members. It includes the independent assessment, native results, complete observation streams, pair and dispatch/closure receipts, failed laptop assessments and source binding. Laptop native results and summaries are explicit object projections omitting unrelated host/process inventory; original wrapper hashes remain recorded. No audio, weights, executables or driver caches are published.

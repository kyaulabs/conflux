# M2 packed attention views, 20 October 2026

The packed-view candidate preserved **exact Best-quality output**, but live
chunk delivery still missed the **4.095-second target**. Its two-minute hidden
player run delivered 21 validated hops: nearest-rank p99 and maximum were
**6.128 seconds**, with **2/21 hops at or above target**. M2 remains open, and
the candidate is not promoted to the product runtime.

| Check | Result |
|---|---|
| Actual Vulkan output comparison | All 16 full/seek cases and complete float outputs match the accepted reference exactly |
| Player measurement | 120.007 seconds; Best, Vulkan, two runtime CPU threads, unpaced |
| Native requests | All 22 attempts retained; 21 validated hops and one stale return |
| Seek/replay behavior | Stale result suppressed; four replay recoveries; all 1,330 accepted packets match their accepted epoch/generation |
| Control buffering | 18.336 seconds of live-deck holds during seek and replay-recovery intervals |
| Steady/peer starvation | Zero frames; control holds counted separately |
| Audio callbacks | 23,458; zero execution deadline misses, invalid buffers or source-read failures |
| Recording | 5,995,264 stereo frames at 48 kHz; accepted/written counts match; zero overflow |
| Recorded samples | All 11,990,528 float samples finite; no clipping; header, peaks and nonzero counts independently verified |
| Sampled CPU/ACPI temperature | Maximum 96°C; two isolated samples at or above 85°C across 62 samples |
| Cleanup | Known processes/groups and owned PipeWire objects gone; guard, files and immutable cache seed unchanged |

The change replaces the first packed Q/K/V slice copies with guarded strided
views. It retains the final contiguous Flash inputs, projections, weights,
tensor types, Flash precision, ten Flash signatures and other backend paths.
The graph removes
26 copy nodes with 107,347,968 bytes of static payload; that count is not a
measurement of memory traffic, peak memory or speed. CPU checks passed 361
layout/allocator assertions and 31,127,040 independent sample-address comparisons.
One pipeline object was rebuilt; all 186 other engine archive members and the
reviewed full-client DRM observer object stayed unchanged.

| Short-run observation | Previous observer build | Packed-view candidate |
|---|---:|---:|
| Validated hops | 22 | 21 |
| Median service time | 2.115 s | 1.748 s |
| Sample p99 / maximum | 5.627 s | 6.128 s |
| Hops at or above 4.095 s | 8 | 2 |
| Seek/replay holds | 26.160 s | 18.336 s |

The [previous run](owned-drm-clients-2026-10-20.md) and this run were not a
counterbalanced comparison. Their profile exposure differed: 59/64 previous
host samples and 46/62 candidate samples were in low-power mode. The transition
occurred roughly four seconds into the previous run and 33 seconds into this
one. Neither the median nor the late-hop count establishes a copy-change benefit.
These repeated windows also do not establish a population p99 or sustained
performance.

Requests 18 and 22 were late at 5.110 and 6.128 seconds. Their graph-compute spans
alone were 4.806 and 5.843 seconds. The worst hop read source audio in 6.243 ms,
spent 279.929 ms of native process CPU time and rebuilt no graph. Its separate
render-client counter grew by 5.079 seconds; the other client's counter did not
grow. Counters retain unknown freshness and are neither summed nor treated as
exclusive current-request execution time.

Contained host samples during both late hops showed actual GPU clocks of
300–400 MHz, requested clocks of 1,350 MHz, PL1 flags and a 7.5 W MMIO package
limit. Passing hops also occurred in that state. The late samples were cooler
than the observed temperature peak. These sparse observations do not establish
a causal clock, power or temperature explanation. The existing thermal guard
and host policy were unchanged. Two-second sampling showed no consecutive hot
samples, but cannot establish the spikes' continuous duration or qualify a soak.

Request 2 began in generation 1 and returned after the worker advanced to
generation 2. It has no validated chunk or published lease. This demonstrates
stale-result suppression; no new hard-cancel acknowledgement or deadline was
measured. Initial load to first observed cursor advance took 4.858 seconds,
separate from control holds. At shutdown, generation 6 had a complete raw
endpoint of 1,031,940 frames, but only 946,176 published output frames and no
converter-complete event. Final conversion completion is not claimed.

The first launch failed after 4.896 seconds because the private benchmark
directory was mode 0755 instead of the helper's required 0700. No inference or
audio started. Its failed result, including `cleanup_complete: false`, remains
preserved; root inspection confirmed no native/readiness child or cache
resources were created. Read-only metadata subprocesses could run during setup.
The directory was corrected, an actual-parent regression was added, and all
31 launcher tests and 421 strict file checks passed before a fresh conditions
record authorized the corrected attempt. That attempt and its cleanup finished
297.256 seconds after the new record, within the original 960-second ceiling.
The failed attempt's readiness was not reused, and no automatic retry ran.

The source HEAD was `14ec0334e43276b96e74891167daea2a70ffce37`; candidate runtime
SHA-256 was
`82e5bcf77e70353a82bec88c892eb420d6d565382cbf7fadfaf00cbad0d61a6c`.
The [evidence manifest](data/m2-packed-attention-views-2026-10-20/archive-manifest.json)
binds selected source, patch, review, build, failed-launch and completed-run
snapshots. [Archive verification](data/m2-packed-attention-views-2026-10-20/verification.json)
checks every stored and decompressed hash. Models, executables, music,
recordings and caches are excluded; the recording hash is retained. The native
window stayed hidden with zero presented frames, so this does not qualify
visible UI rendering or a thermal soak. The next useful performance step is a
runtime-only baseline/candidate comparison, with reversed order before an
effect claim. No further laptop run is queued; Best quality and the target stay
unchanged.

# M2 owned DRM client diagnostic, 20 October 2026

Best-quality playback passed the functional checks, but **live chunk delivery
still missed the 4.095-second target**. The two-minute hidden-player run produced
22 validated hops: nearest-rank p99 and maximum were **5.627 seconds**, with
**8/22 hops at or above target**. M2 remains open. These dependent, repeated
windows do not establish sustained performance or a population p99 bound.

| Check | Result |
|---|---|
| New candidate output comparison | All 16 full/seek inputs and complete float outputs match the accepted reference exactly |
| Player measurement | 120.007 seconds; Best, Vulkan, two runtime CPU threads, unpaced |
| Native request evidence | All 23 attempts retained, including one stale return; 22 validated hops |
| Seek/replay behavior | Incomplete-generation seek exercised; stale result suppressed; four replay starts/recoveries observed |
| Control buffering | 26.160 seconds of live-deck holds during the seek and replay-recovery intervals |
| Steady/peer starvation | Zero frames; control holds are counted separately |
| Audio callbacks | 24,311; zero execution deadline misses, invalid buffers or source-read failures |
| Recording | 6,213,888 stereo frames at 48 kHz; accepted/written counts match; zero overflow |
| Recorded samples | All 12,427,776 float samples finite; header, frame counts and peaks independently verified |
| Sampled CPU/ACPI temperatures | Maximum 82°C across 64 samples; none at or above 85°C |
| Cleanup | All known processes/groups and owned PipeWire objects gone; guard, files and immutable cache seed unchanged |

The native window stayed hidden and reported zero presented frames. This run
does not qualify visible UI rendering or a thermal soak. Temperatures were
sampled every two seconds, so shorter spikes and continuous exposure remain
unmeasured. The recording contains the control holds; passing callback and file
checks does not imply instant recovery. Initial load to first observed cursor
advance took 9.416 seconds, separate from that interval total. The seek command
to published playback reserve took 11.177 seconds.

Native request 3 began in generation 1 and returned after the worker advanced to
generation 2. It remains in the trace and has no validated chunk or published
lease. This demonstrates stale-result suppression, without a new hard-cancel
acknowledgement or deadline claim. Request 2 was validated after seek issue but
before generation 2 acceptance; it must not be described as unvalidated. At
normal shutdown, generation 6 had a complete raw endpoint but only 573,440
converted/published frames. Final conversion completion is not claimed.

The previous diagnostic rejected startup because it assumed one DRM client.
A separate 73.111-second startup probe captured two distinct client IDs on the
same Intel device, with zero inference requests. The new private observer binds
the entire client/alias/engine set and keeps a separate counter history for
each owner. It selects neither client by activity and does not sum their counters.
All raw values retain unknown freshness. Source, fixture, build and launcher
reviews passed before execution, followed by 187 C++ assertions, 28 launcher
tests, 16 normalized-input comparisons and 193 strict file-spec checks.

Every late hop had a graph-compute span above 4.095 seconds, ranging from 4.276
to 5.361 seconds. The slowest hop spent 5.361 seconds in graph computation,
284.926 ms of native process CPU time and 4.440 ms reading source audio. Its
observed render-counter growth was 5.204 seconds. These observations direct
further review toward graph computation rather than source reading; counter
freshness prevents exclusive current-request GPU attribution.

Samples fully contained within the Python timing brackets of late hops showed
PL1 asserted, requested GPU frequency 1,350 MHz and actual samples of 300–700
MHz. The MMIO package-limit sample was 7.5 W and MSR sample 64 W. Passing hops
also occurred with those settings or PL1 asserted. Sparse samples do not prove
an effective limit, continuous clock rate or causal classifier. The observer
changed no host policy. Guard-driven profile transitions remain recorded.

Source HEAD was `976a5eb414d4697219f6931c7405832f1bd66351`; candidate runtime
SHA-256 was
`ca781657d698119b298c402cb716329b10ce113f10c31633be4b8fade8fa0e71`.
The comparison completed in 104.195 seconds. The player stage, including
cooldown/setup/cleanup, took 217.595 seconds. Both stages and the interstage
audit finished 398.938 seconds after the original conditions record, within
its unchanged 960-second ceiling. No automatic retry ran. Neither this
instrumented trial nor comparison with earlier results establishes a speedup.

The [evidence manifest](data/m2-owned-drm-clients-2026-10-20/archive-manifest.json)
binds 54 compressed source/record snapshots and 1,457 JSONL records.
[Archive verification](data/m2-owned-drm-clients-2026-10-20/verification.json)
checks every stored and decompressed hash. Models, executables, music, recorded
audio and caches are excluded; the recording's hash is retained. Independent
audits cover exact PCM, all client arithmetic, seek lineage, the complete WAV
scan and cleanup. The next performance step is review of existing compute
profiles for a concrete optimization. No further laptop run is queued, and
Best quality and the strict target remain unchanged.

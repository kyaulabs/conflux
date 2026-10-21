# M2 attention operation profile, 21 October 2026

The silent probe captured one complete Best-quality request per runtime and
matched both full outputs to the accepted reference. It did not reproduce the
seconds-long player stalls. The final attention-input copies took 5.029 ms in
baseline and 7.884 ms in the packed-view candidate. These copies account for
0.334% and 0.524% of the measured query intervals in this probe. Keep the
candidate unpromoted and M2 open; the **4.095-second p99 target** is unchanged.

| Instrumented GPU query intervals | Baseline | Packed candidate |
|---|---:|---:|
| 30 final attention-input copies | 5.029 ms | 7.884 ms |
| 10 Flash attention operations | 268.092 ms | 265.518 ms |
| Remaining query intervals | 1,232.639 ms | 1,231.588 ms |
| Total | 1,505.760 ms | 1,504.991 ms |
| Queries / optimized graph nodes | 1,557 / 2,871 | 1,531 / 2,793 |

Each fresh runtime processed the same normalized 7.8-second stereo Club XL
window, `club_xl:515970`, at 44.1 kHz. Both returned exactly 11,007,360 bytes
with the accepted full-output SHA-256. The parser verified all 30 final copies
and 10 Flash operations, including their graph identities, tensor layouts,
source strides and view offsets. It retained the remaining query intervals as
an aggregate. No selected operation was omitted through a top-N cutoff.

The packed graph removed 26 queries and 78 graph nodes, but its retained final
head copies cost 2.855 ms more in this pair. The query totals differed by only
0.770 ms. This observation does not explain the earlier multi-second stalls or
establish a speed improvement. The existing grouped logger also recorded 225
versus 199 contiguous copies across the whole graph, taking approximately
109.209 and 112.653 ms respectively.

These are instrumented query intervals, not exclusive kernel costs or normal
player delivery times. The existing nonconcurrent Vulkan logger adds timestamp
commands, synchronization, query retrieval and formatting. The private patch
only adds bounded metadata after completed query retrieval; model arithmetic,
Best quality, full-F32 Flash, Vulkan and two runtime CPU threads are unchanged.
There was one request per arm, with no audio or UI workload, seek, cancellation
or sustained thermal test. These results cannot replace the failed
[four-run player comparison](live-runtime-comparison-2026-10-21.md).

Host state also differed. Pre-request admission recorded 46°C with the balanced
profile for baseline and 43°C with power-saver active for the candidate. The retained
guard had recorded an 86°C warning before the candidate's inference. These are
saved observations, not continuous temperature measurements or per-query GPU
clock readings. The guard identity and policy stayed unchanged. Both arms used
fresh copies of the same immutable cache payload, but this was not a controlled
power-state comparison.

Both runtimes exited normally with complete stdout, stderr and DRM trace
footers. Output, source, model, cache and guard checks passed, as did owned
process and temporary-file cleanup. Both arms finished within the original
600-second window, without a renewed clock. The earlier attempt is preserved:
AC was offline, so it stopped before readiness or native inference. It produced
no profiling result.

The CPU preparation checks passed: 30 selector cases with 5,366 assertions,
10 parser methods and 19 capture/consumer tests. Independent review checked the
actual build, all 467 current compiler dependencies, the replaced backend
archive member and 159 unchanged archive members. Historical system-header
equivalence with the older player binaries remains unproved. This single-case
output check does not replace the required 16-case check before a later player
experiment.

The next investigation needs operation data from an actual slow player request.
Further delivery-queue tuning or adopting the packed candidate is unsupported
by this probe. No additional laptop run has started.

The [evidence manifest](data/m2-attention-node-profile-2026-10-21/archive-manifest.json)
binds the source patch, build proof, recipes, readiness, failed attempt, completed
results, full logs, traces and independent audits.
[Archive verification](data/m2-attention-node-profile-2026-10-21/verification.json)
checks the stored and decompressed hashes. Music, models, executables and driver
caches remain outside source history.

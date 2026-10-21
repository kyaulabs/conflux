# M2 player attention profile, 21 October 2026

The instrumented hidden player reproduced a slow request. Its 22 validated hops
had a sampled nearest-rank p99 of **4,542.598 ms**, above the unchanged
**4,095 ms target**; three hops missed the target. Functional checks and complete
request capture passed. Keep M2 open and retain Best quality. These instrumented
results diagnose the delay; they do not qualify normal delivery latency.

| Check | Result |
|---|---|
| Silent prerequisite | All 16 full outputs exactly matched the accepted reference |
| Actual player | 120 seconds requested, hidden throughout |
| Complete native attempts / validated hops | 23 / 22 |
| Sample hop p99 / late hops | 4,542.598 ms / 3 of 22 |
| Steady playback | 97.904 seconds, zero starved frames |
| Planned control intervals | 22.091 seconds, including 21.925 seconds of deck A holds |
| Recorded mix | 130.208 seconds, zero overflow, nonfinite or clipped samples |
| Cleanup | All known test processes, groups and owned PipeWire objects absent |

The largest hop was request 7. It spent 4,378.989 ms in native inference and
4,234.959 ms in the graph phase. Completed GPU query intervals totaled
4,204.820 ms, split between 15.830 ms of final attention-input copies,
840.188 ms of Flash attention and 3,348.802 ms of remaining intervals. The
copy intervals account for about 0.38% of that total. Removing those copies
alone would not recover the 447.598 ms target miss.

Requests 7, 11, 15, 19 and 23 all used source-window start frame 773955 in
generations 2 through 6. Their query totals ranged from 3,447.781 to
4,204.820 ms. Only requests 7, 11 and 19 crossed the service target. The
ordinary neighboring requests took roughly 1,500 ms in query intervals.
Several operation groups grew together; the printed matrix-multiplication
group alone took 1,729.986 ms in request 7 versus 569.556 ms in request 2.

The silent prerequisite processed the same Club XL source window in
1,508.986 ms of query intervals, comparable to its first three windows.
This contrast does not establish an input-only cause: exact live-versus-silent
normalized input bytes were not captured, and request spacing, playback and
power state differ. All 39 graphs across both stages retained 2,871 optimized
nodes, 1,557 queries and all 40 selected attention operations. The native input
size stayed fixed. No padding, attention-mask or quality change is justified
by this run.

The existing logger records instrumented intervals, including the effects of
its synchronization and query retrieval. The partitions are not exclusive
kernel costs. The retained graph sequence binds each complete serial native
attempt to agreeing raw result, DRM and service-context records; graph
sequence is not itself a request ID. The earlier
[single-request probe](attention-node-profile-2026-10-21.md) had not reproduced
this player delay.

One seek interrupted an incomplete generation, and four later replays
recovered. Request 3 returned for generation 1 after the worker had advanced
to generation 2; it was discarded and never became a validated hop. This
establishes stale-result suppression, not interruption of an executing GPU
request. The observed seek-to-cursor recovery upper bound was 7.337 seconds;
replay recovery upper bounds ranged from 3.601 to 3.856 seconds. Deck A held
while fresh stems became available. The peer deck had no starved frames.

The audio callback reported zero execution deadline misses, invalid buffers,
driver recovery flags or driver position discontinuities. The float WAV
contained 6,249,984 stereo frames at 48 kHz, including setup before the
measurement. An independent offline scan checked all 12,499,968 samples:
none were nonfinite or at or above full scale. Its channel peaks were
0.053625 and 0.053402. These checks do not establish musical quality, absence
of clicks or uninterrupted live stems during the planned control holds.
The fixed recording boundary remained unchanged during the 13.931-second
diagnostic tail.

The player stayed hidden in all 12,722 window observations. No visible UI
performance claim follows. Seventy-one CPU/ACPI samples peaked at 76°C, with
none at or above 85°C. The profile changed from balanced to low power early
in the run. GPU clocks and package limits were sampled every two seconds;
their association with slow requests does not establish a thermal or power
cause. Shorter spikes remain unobserved, and this was not a thermal soak.
The guard, AC/open-lid checks, source, model and immutable cache seed stayed
unchanged. Best/full-F32 Flash, Vulkan, two CPU threads and unpaced delivery
were retained.

Preparation passed 44 CPU methods, independent review and the complete
570-source file/build check. The reused build proof still leaves historical
system-header equivalence unproved. All 16 exact output checks passed before
the independent interstage audit allowed the player to start. Both stages
finished within the original 16-minute window. Owned cleanup and final file
checks passed; that physical test window is closed.

The next investigation should compare the captured request cadence and
operation groups with the silent reference before selecting another controlled
run. Broad graph slowdown is now recorded; a final-copy optimization would
address too little of the observed delay. No runtime candidate is promoted,
and no additional laptop test has started.

The [evidence manifest](data/m2-player-attention-profile-2026-10-21/archive-manifest.json)
binds the frozen plan, preparation checks, sources, complete captures, results
and independent reviews.
[Archive verification](data/m2-player-attention-profile-2026-10-21/verification.json)
checks stored and decompressed hashes. Music, models, executables and driver
caches remain outside source history.

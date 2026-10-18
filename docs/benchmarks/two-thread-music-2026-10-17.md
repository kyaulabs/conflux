# M0 two-thread music validation, 17 October 2026

The 600-second validation with two native threads missed the **4.095-second
continuous-hop service p99 target**. Observed p99 was **5.172 seconds**,
maximum **5.398 seconds**, with **8/102 hops above target**. All **101 ordinary
virtual playback demands were ready on time**. Cancellation, exact seek output
and independent audio/UI coverage passed. The conditional 60-minute run was
withheld. M0 remains open; M1 has not started.

| Paced validation | Continuous hops | Service p50 / p95 / p99 (s) | Maximum (s) | Target misses |
|---|---:|---:|---:|---:|
| Previous four-thread run | 102 | 1.902 / 5.702 / 6.822 | 7.446 | 21 |
| New two-thread run | 102 | 1.897 / 4.531 / 5.172 | 5.398 | 8 |

The new result is lower than the [previous paced validation](paced-music-2026-10-17.md),
but both failed. These sequential trials do not establish a causal thread-count
or thermal improvement: machine history, startup and natural policy transitions
remain confounded. Both new complete five-minute continuous bins also missed
the target, at 5.398-second and 4.836-second p99. All 103 delivered hops,
including the seek-first hop, have the same overall p99, maximum and miss count.
Nearest-rank percentiles on dependent repeated windows provide no population
confidence bound.

Only the native thread setting changed from four to two. All eight producer
hashes, four prepared music excerpts, fixed playback clock, two-hop queue,
software Wayland UI at 33 ms, silent PipeWire probe and guard/readiness policy
match the previous run. No producer source or binary changed or needed a rebuild.
The setting controls FFT preparation/reconstruction as well as native backend
threads; it does not cap every driver thread. The saved commands and native
ready metadata independently confirm two. Signed source HEAD was
`8ed19872d880bec4b881397c9145d4f76654a8c5`, with identical producer bytes to
the preceding `2223d6a` revision. Its existing
[push CI](https://github.com/kyaulabs/conflux/actions/runs/39082276909) and
[PR CI](https://github.com/kyaulabs/conflux/actions/runs/39082279701) passed,
including all four CTest entries. Those checks cover eighteen worker cases
and eleven soak cases; this parameter-only trial added no production code.

All ordinary demands found ready output: 26 observations found one chunk and
75 found two. Queue high-water stayed at two, with one held playing chunk.
Ordinary deadlines advanced by actual frames across every source rotation;
none reset, no future ordinary PCM was polled early and no terminal obligation
was omitted. Ready lateness and completed-demand starvation union were zero.
Consumer wake p99 was 0.256 ms; delivery p99 was 6.053 ms, maximum 15.529 ms.
Ordinary queue residence p99 was 9.837 seconds, intentional read-ahead outside
hop service. Initial priming took 3.005 seconds after the first timed request;
priming and explicit recovery are separate epochs, excluded from ordinary
readiness. This establishes short-run virtual availability, without audible
stem playback or sustained qualification.

The active control began at workload second 307.385, after 174.171 ms of a
revalidated request. Logical acknowledgement took 0.146 ms; active work drained
in 1.699 seconds. Replacement readiness took 3.740 seconds after resume or
5.441 seconds after cancellation. Its raw poll was also 5.441 seconds after
cancellation; reported handling, including parity work, completed at
5.504 seconds. The held old chunk was rejected, one matching request was
discarded and the trimmed seek was sample-exact with zero maximum difference.
No stale output or telemetry loss occurred, and one backend stayed alive.
Hardware GPU preemption remains unproven.

All four sources completed and repeated. Full traversal counts were 7, 5, 6
and 6 for Club XL, When the Light Breaks, Under My Skin and STFU; one resumed
When the Light Breaks traversal completed. Every continuous source-grid hash
matched the prior Vulkan reference supporting the user's acceptance as a
starting basis. Twenty-six load admissions produced first output in
1.877–3.751 seconds; preparation took 19.3–96.8 ms. These times include
preparation and first inference, separate from hop service. The inputs remain
the same 23.4-second prepared excerpts, original time 30.0–53.4 seconds;
no new FLAC decoding or whole-track qualification is implied.

Native graph execution/wait occupied 91.2–96.4% of service on the eight misses;
graph duration alone exceeded 4.095 seconds on seven. Every miss had only
low-power/`power` EPP interior samples. Forty-one passing hops also had that
policy, and one passing hop crossed a transition. Misses had per-hop mean
sampled active GPU frequencies of 441–606 MHz. Of 1,177 snapshots, 563 were
low-power/`power` and 614 balanced/`balance_performance`. Maximum sample gap
was 0.633 seconds. Sparse observations localize the tail to native execution/
wait and describe associations, without proving a cause or effective clocks.

Normal readiness took 125.606 seconds outside measurement; fresh admission
was balanced at 42°C with AC connected and the lid open. The unchanged guard
then logged a 95°C warning 0.102 seconds before the first timed request,
followed by a critical message. Recovery was logged 121.663 seconds after
that warning. Another 95°C warning and critical message occurred around
workload second 433.852; no later recovery appears in the bounded capture.
Message spacing describes policy latching, not continuous hot duration or
which startup stage produced heat. The measured package peak was 95°C.
Three samples at or above 85°C spanned 1.012 seconds and were bracketed by
cool readings 2.028 seconds apart. TCPU peaked at 71.05°C. CPU thermal-throttle
counters advanced; the sampled GPU thermal reason stayed zero. These samples
do not establish continuous heat duration. Brief heat remains acceptable under
the user's criterion; the service target is the failure gate. The runner
changed no profiles or services. All seventeen AC/lid snapshots were ready.

The independent silent audio stream covered all 600.000179 measured seconds
at negotiated 48 kHz stereo/256 frames. Its 112,857 callbacks recorded all six
fault counters at zero, callback p99.9 histogram upper bound 32 µs and maximum
187.526 µs. These are callback and reported driver observations, without a
physical-loopback or stem-playback claim. The UI recorded 18,827 frame
notifications, ten complete minute bins at 31.25–31.50/s, maximum notification
gap 49.736 ms, thirteen motion observations and no sampled exposure loss.
Notifications are not physical presentation latency. Native VmHWM was
280,300 kB and supervisor VmHWM 215,684 kB, both with zero sampled swap.
Separate DRM resident-system maximum was 535,780 KiB; these peaks are not
added. Four denied RAPL fields leave energy unavailable.

The [evidence manifest](data/two-thread-music-2026-10-17/archive-manifest.json)
binds 44 files, 33 exact prior references and 1,839 JSONL records. Stored
evidence is 830,222 bytes plus the 27,333-byte manifest. Three published-copy
replays reproduce event/control/deadline/coverage, thermal and descriptive
analysis facts; a separate reduction passed twenty evidence checks. The archive
checks source CI HEAD, workflow identities and successful outcomes, all eight
unchanged producer hashes and five source snapshots against prior canonical
evidence. Models, binaries, music, PCM, arrays and HTML are excluded, so replay
cannot recompute excluded audio or listening. All owned test processes and
inhibitors were released. Benchmarking is stopped; the listening server remains
available. Two threads did not establish the required headroom or avoid guard
policy transitions. Further M0 work must address that service tail before a
conditional hour or M1.

# M0 playback-paced music validation, 17 October 2026

The 600-second playback-paced validation missed the **4.095-second
continuous-hop service p99 target**. Observed p99 was **6.822 seconds**,
maximum **7.446 seconds**, with **21/102 hops above target**. All **101
ordinary virtual playback demands were ready on time**. The active
cancellation/seek check, independent audio probe and UI coverage passed.
The conditional 60-minute run was withheld. M0 remains open; M1 has not started.

| Population | Hops | Service p50 / p95 / p99 (s) | Maximum (s) | Target misses |
|---|---:|---:|---:|---:|
| Continuous | 102 | 1.902 / 5.702 / 6.822 | 7.446 | 21 |
| All delivered, including seek-first | 103 | 1.902 / 5.702 / 6.822 | 7.446 | 21 |

Both complete five-minute continuous bins missed the target: p99 was
7.446 seconds in the first and 6.732 seconds in the second. Percentiles use
nearest rank on dependent repeated windows, without a population confidence
bound. The preceding [saturated music hour](music-load-2026-10-17.md) reached
4.266-second p99. Different durations, machine history and observed power
states prevent treating these sequential runs as a causal cadence comparison.

The new optional `--delivery-mode playback-paced` holds one playing chunk
and leaves at most two future chunks in the existing worker queue. Ordinary
deadlines advance by actual chunk frames divided by 44,100, including source
changes and late arrivals. Only initial priming and explicit cancellation/seek
recovery anchor new epochs. Initial priming took 2.004 seconds after the first
request started. No ordinary deadline was reset, no future PCM was polled early,
and no due terminal demand went unrecorded. Regression tests also cover short
tails, producer delays, delayed consumer wake and censored terminal obligations.

Every ordinary demand found one or two ready chunks: 43 observations found
one and 58 found two. Ready-queue high-water was two. Completed ordinary
demands had zero readiness lateness and zero deadline-to-ready starvation
union; no final demand remained unfulfilled. Consumer wake p99 was 0.214 ms;
raw delivery p99 was 13.249 ms, maximum 16.551 ms. Ordinary queue residence
p99 was 9.836 seconds, intentional read-ahead outside hop service. These
virtual readiness results exclude initial priming and the explicit recovery
interval. They establish neither audible stem playback nor hour-long stability.

The test retained the four prepared user-track excerpts, original time
30.0–53.4 seconds, without concatenation or new FLAC decoding. Every source
completed and repeated; every continuous grid hash matched the prior Vulkan
material supporting the user's acceptance as a starting basis. Full traversal
counts were 7, 5, 6 and 6 for Club XL, When the Light Breaks, Under My Skin
and STFU; one resumed When the Light Breaks traversal also completed.
Twenty-six load admissions produced first output in 1.863–3.602 seconds;
source preparation took 19.7–75.2 ms. Admission includes preparation and
first inference and remains separate from hop service.

The active control began at workload second 306.380, after 171.263 ms of a
revalidated request. Logical cancellation acknowledgement took 0.108 ms,
active work drained in 1.704 seconds, and replacement readiness took
3.727 seconds after resume or 5.434 seconds after cancellation. The raw
replacement poll occurred 5.435 seconds after cancellation; the control's
reported replacement handling time, including parity work, was 5.556 seconds.
The old held chunk was rejected, one matching request was discarded and
the trimmed seek output was sample-exact with zero maximum difference.
No stale chunk reached the consumer; one backend stayed alive and no
telemetry was lost. Hardware GPU preemption remains unproven.

Native graph execution/wait occupied 95.0–97.6% of service on the 21 misses;
graph duration alone exceeded 4.095 seconds on twenty. All misses had only
low-power/`power` EPP interior samples. Twenty passing hops also had that
sampled policy, and two passing hops crossed a policy transition. Misses had
per-hop mean sampled active GPU frequencies of 381–663 MHz. Across all
1,175 snapshots, 672 were balanced/`balance_performance` and 503 low-power/
`power`. These are sparse associations, with a maximum sample gap of
0.635 seconds, rather than effective clocks or proof of cause.

The unchanged thermal guard logged three warnings at 86°C, 91°C and 92°C
and two recoveries during the measurement. Its two complete warning/recovery
message pairs were 121.412 and 121.478 seconds apart. Those intervals describe
policy latching, not hot duration. Retained package samples peaked at 95°C;
the two samples at or above 85°C were isolated 91°C and 95°C observations,
bracketed by cooler readings across 1.021 and 1.012 seconds. TCPU peaked at
70.05°C. Sampled CPU thermal-throttle counters advanced; the sampled GPU
thermal reason stayed zero. These observations do not prove continuous heat
duration. Brief heat is acceptable under the user's stated criterion; the
failed service target is the qualification blocker. The runner changed no
power profiles or services. Normal readiness took 71.623 seconds outside the
measurement, and fresh admission was balanced at 41°C with AC connected
and the lid open. All sixteen monitored AC/lid snapshots were ready.

The independent silent PipeWire probe covered all 600.000198 measured seconds
at negotiated 48 kHz stereo/256 frames. Its 112,820 callbacks recorded all six
fault counters at zero, callback p99.9 histogram upper bound 30 µs and maximum
79.684 µs. This measures callback and reported driver timing, without a
physical-loopback or stem-playback claim. The software Wayland UI retained the
33 ms cadence: 18,818 frame notifications, ten complete minute bins at
31.25–31.483/s, maximum notification gap 50.310 ms, thirteen motion observations
and zero sampled exposure loss. Notifications are not physical presentation
latency. Native VmHWM was 281,168 kB and supervisor VmHWM 216,504 kB, both
with zero sampled swap. Separate DRM resident-system maximum was 535,780 KiB;
these peaks are not added. Four denied RAPL fields leave energy unavailable.

Signed producer HEAD was `2223d6ae5e27d0c9e4b357b4a5dc70d51daef086`.
Runtime, model, UI/audio binaries and shared sampler code retain the prior
pins. The worker change exposes copied active-request metadata; it does not
change inference. CMake configure/build and all four CTest entries passed,
including eighteen worker cases and eleven soak cases. Both producer
[push CI](https://github.com/kyaulabs/conflux/actions/runs/39081254307) and
[PR CI](https://github.com/kyaulabs/conflux/actions/runs/39081257897) passed.

The [evidence manifest](data/paced-music-2026-10-17/archive-manifest.json)
binds 48 files, 24 exact prior references and 1,830 JSONL records. Stored
evidence is 1,155,972 bytes plus the manifest. Three published-copy replays
reproduce event/control/deadline/coverage, thermal and descriptive-analysis
facts. A separate reduction passed seventeen evidence checks. The first
audit's delayed-control-telemetry ordering assumption was corrected; its
original helper, failed report and initial review are retained. Measurement
bytes were unchanged. Models, binaries, music, PCM, arrays and HTML are
excluded; saved-data replay cannot recompute excluded audio or listening.
All owned test processes and inhibitors were released. Benchmarking is
stopped and the listening server remains available. Further M0 work must
address the native service tail while preserving the demonstrated readiness,
cancellation and audio/UI behavior before another conditional hour or M1.

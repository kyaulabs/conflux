# M0 music latency profile — 17 October 2026

The unchanged Vulkan worker completed a short four-track rerun with
**2.161-second continuous-hop p99/maximum**, below the 4.095-second target,
with zero misses across sixteen hops. A fixed-input comparison also ran
without misses. The previously slowest music window ran quickly in the new
replay; the earlier misses coincided with low GPU clocks and a guard-triggered
policy transition. These short results do not establish a causal fix or qualify
sustained music delivery under audio/UI load. M0 remains open; M1 has not started.

| Measurement | Samples | p99 / maximum (s) | Above 4.095 s |
|---|---:|---:|---:|
| Earlier four-track worker, continuous hops | 16 | 5.258 | 3 |
| New four-track worker, continuous hops | 16 | 2.161 | 0 |
| Fixed music window, native round trips | 24 | 2.346 | 0 |
| Fixed synthetic window, native round trips | 24 | 2.082 | 0 |

With these small populations, nearest-rank p99 equals the observed maximum.
Native round trips and delivered worker hops measure different intervals;
no table row includes source decoding. The
[original assessment](user-tracks-2026-10-17.md) retains the earlier misses.
The tests changed no runtime, model, worker algorithm or thread count and did
not set a host power profile.
Source HEAD was `8a61059f585947c91fef4a5606ad11fb81523daf`; the five producer
hashes match the existing [runtime decision](../decisions/0002-m0-stem-runtime.md).

The three earlier misses spent 96.3–96.7% of hop service in native graph
execution/wait. Each graph duration alone exceeded 4.095 seconds. Ready-queue
delay was below 0.165 ms on those misses and below 0.575 ms across all sixteen
hops. Interior active-GPU frequency samples averaged 475–562.5 MHz on the
misses, versus a requested 1,350 MHz; all miss samples used low-power policy.
Nine passing hops also used low-power policy, so that state alone does not
explain every request. These are sparse sample associations, not effective
clocks or isolated GPU-kernel timings.

The same thermal-guard PID/invocation logged a 96°C warning at
20:46:51.340887 UTC during the last Club XL seek, before the later tracks.
Actual policy samples then changed to low-power. The inspected guard engages
power saver at 85°C and requires sixty consecutive readings at or below 70°C,
two seconds apart, before recovery. A brief trigger can therefore leave a
longer policy latch. The journal does not identify the triggering sensor or
hot duration; the native package trace peaked at 78°C. No sustained-hot
failure is inferred from the warning, and the guard was left enabled.

The new native comparison used twelve synthetic/music/music/synthetic blocks
in one persistent four-thread process, completing 48 requests in 89.980 seconds.
The music input was the previously slowest window: frame 773,955 of
`when_the_light_breaks`, with the original source hash and exact normalization
statistics. Its 257,985 valid frames precede 85,995 zero-padding frames.
Repeated inputs produced identical output hashes. Music-minus-synthetic
block-mean round-trip differences averaged +10.8 ms, with both positive and
negative blocks. All 179 policy snapshots were balanced; sampled package
temperature peaked at 79°C with no samples at or above 85°C. This short,
ordered experiment does not establish a causal content or policy effect.

The worker follow-up used the original driver, manifest, four prepared WAVs,
four threads and 120-second request timeout. It completed in 48.963 seconds,
retaining one backend across four sources and three completed-source seeks.
Cold first delivery took 2.522 seconds; later sources took 1.866–2.205 seconds.
Seek first-ready times were 1.714, 3.543 and 3.415 seconds, with sample-exact
suffixes and zero RMS/maximum error. All four complete output-file hashes match
the earlier Vulkan arrays supporting the user's initial listening acceptance.
All 95 native policy samples were balanced; package temperature peaked at
79°C, with no samples at or above 85°C. Native VmHWM was 281,060 kB with zero
sampled swap. The separate DRM resident-system maximum was 535,780 KiB;
these peaks are not summed into a total. Four RAPL energy fields remained
permission-denied, so energy and power were unavailable.

Setup and recovery remain separate from those timings. Two earlier replay
attempts were refused before native startup and supplied zero latency samples:
one observed power saver, the other a 57°C fresh reading after the cool period.
The final admission retains the preceding 60-second observation at or below
55°C and uses the guard's 70°C recovery threshold for the immediate recheck,
along with balanced policy, no holds, matching recovered guard identity, AC
power and an open lid. Both actual readings and limits are saved. Guard
transitions also occurred before worker inference, so the separator cannot be
assigned every trigger. Worker readiness took 290.660 seconds, outside its
48.963-second run; the final fresh package reading was 63°C. Both successful
runs closed their native processes
and released their owned inhibitors.

No benchmark audio/UI load or active-request cancellation ran in this follow-up.
The [software-UI hour](software-ui-2026-10-17.md) remains separate evidence:
eleven logical cancellation/seek controls passed and all six audio fault
counters were zero. The new completed-source seeks add no GPU-preemption,
physical-audio or production-playback claim. The next M0 check is sustained
music delivery under audio/UI load with natural guard transitions retained.

The [evidence manifest](data/music-profile-2026-10-17/archive-manifest.json)
binds the raw timing streams, executed helpers, admission refusals, guard
records and derived audits: 48 files, including eleven compressed JSONL streams
with 565 records. The published-copy audit reproduced saved timing, seek,
output-identity and thermal reductions. Unchanged producer and baseline data are exact
references to earlier archives. Audio, normalized PCM, arrays, models,
binaries, full source tags and the local listening page are excluded.
Saved-JSON audits cannot replay those excluded PCM calculations or verify
listening acceptance.

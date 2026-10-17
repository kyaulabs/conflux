# M0 audio and UI load isolation

The final audio/UI run completed 3,600.0004 seconds on the laptop. Continuous
hop p99 was **5.941 seconds against the 4.095-second target**, with a 7.476-second
maximum and 228 misses in 1,296 continuous hops. All eleven cancellation/seek
checks passed, and the independent silent audio probe recorded zero observed
faults. M0 remains open. At the user's request, the suite paused after this
test; the remaining three conditions under the final protocol have not run.
M1 has not started.

| Completed audio/UI hour | Result |
|---|---:|
| Continuous hops | 1,296 |
| Continuous p50 / p95 / p99 | 2.194 / 4.790 / 5.941 s |
| Continuous maximum / target misses | 7.476 s / 228 |
| All accepted hops, including eleven seek replacements | 1,307 |
| All-hop p99 / target misses | 5.775 s / 230 |
| Completed cancellation/seek controls | 11 |
| Audio callbacks / observed faults | 675,428 / zero in all six counters |
| Audio callback p99.9 upper bound / maximum | 22 / 338.621 µs |
| Final UI frame notifications / complete nonzero minute bins | 215,554 / 60 |
| Maximum UI notification gap | 79.706 ms |

Percentiles use nearest rank. Continuous and all-hop populations differ;
consecutive windows are not independent tail samples. All twelve complete
five-minute all-hop bins failed the target, including the first and last bins
at 5.293 and 4.574 seconds p99. The tiny final partial bin contains no hops.

Cancellation acknowledged in 0.025–0.072 ms. Active native work still drained
for 1.824–4.838 seconds, and replacement delivery took 6.068–16.021 seconds
after cancellation. Every held chunk was rejected after invalidation, every
trimmed replacement matched its continuous reference exactly, and no stale
chunk reached the consumer. The backend loaded once, ready-queue high water
was one against a two-hop limit, and no telemetry was dropped. These are
logical cancellation measurements; hardware submission time is not observed.

Audio negotiated 48 kHz stereo with 256-frame stream and graph quanta. Its
streaming interval covered the complete workload. The zero counters cover
callback deadline misses, missing/invalid buffers, truncated frames, observed
graph-position discontinuities and observed clock recovery flags. This is
silent independent output, without stem playback or a physical latency test.
UI coverage passed with no sampled exposure loss. Frame notifications are
GUI-thread observations, not physical presentation measurements.

The experiment retains the same ThinkPad X1 Carbon Gen 9, i7-1185G7/Iris Xe,
native Vulkan runtime, HTDemucs model, four threads, blocking fence waits,
child-only PASSIVE/zero-spin OpenMP policy, fixture, overlap grid and production
worker. The window is native Wayland/OpenGL. Native/system sampling runs every
0.5 seconds; control checks are scheduled every 300 seconds with a 30-second
request timeout. The runtime, model and application/probe binaries are unchanged
from the [balanced comparison](policy-2026-10-16.md).

Early sustained attempts exposed a host-policy confound and repeated output
failures. They remain separate diagnostic evidence:

| Earlier attempt | Workload seconds | Continuous p99 / maximum | Outcome |
|---|---:|---:|---|
| Worker only | 1,563.432 | 10.325 / 17.034 s | Trimmed seek failed tolerance; four controls complete, fifth incomplete |
| Worker-only rerun | 590.135 | 16.996 / 19.712 s | Repeated full-window SHA changed; one control complete |
| Audio only | 272.631 | 20.085 / 20.085 s | Repeated full-window SHA changed before the first control |

The rejected chunks are excluded from accepted-hop statistics. The last two
attempts returned the same changed hash for the same full window, with rejected
service times of 20.318 and 20.277 seconds. Their numerical difference was not
retained or tested against tolerance. The earlier trimmed failure also lacks
retained compared samples. No numerical magnitude is inferred from a hash.
The audio-only partial run covered its actual workload with 51,442 callbacks
and six zero fault counters, but did not complete the requested hour.

Kernel records contain native-process i915 fence-expiration messages during
the terminal approximately 20-second requests. The
[pinned upstream watchdog implementation](https://github.com/torvalds/linux/blob/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/gpu/drm/i915/gt/intel_gt_requests.c)
cancels incomplete requests on that path. The exact hardened-kernel source was
unavailable; this is a mechanism cross-check and temporal association, without
proof of a reset or the numerical cause. The final hour's bounded kernel query
returned no matching graphics/fence/reset messages. Non-recurrence in this
different policy history does not establish a remedy.

The existing laptop thermal guard switches to power-saver at its warning
threshold and waits for recovery. Its throttled state does not reassert that
profile after another client selects balanced. Earlier requested-balanced
runs therefore did not establish a common initial guard state. Two short
audio/UI attempts were interrupted at 33.200 and 5.795 seconds while this was
investigated; their empty audio/UI reports remain unknown health evidence.
No guard, kernel or driver configuration was changed.

The final protocol never sets or restores a power profile. It first waits at
least ten minutes without a running benchmark producer, with sampled package
temperature at most 55°C for a 60-second span. A separate finite gate then
requires 60 seconds of balanced/no-hold observations and a recovery message
from the same active thermal-guard service identity. This infers readiness;
it does not directly observe the guard's private state. Automatic profile
transitions remain included in the run. Package temperature alone does not
represent every sensor used by the guard. Background heat spikes occurred
even during cooldown, so fixed order, RAM/cache history and other desktop work
still limit causal attribution. The final trace contains 275 low-power/EPP
power samples followed by 6,777 balanced/EPP balance_performance samples; the
first balanced observation was 140.640 seconds after workload start. This was
not an all-balanced hour.

The planned final sequence was audio+UI → UI only → worker only → audio only,
toggling one selected probe per transition. Only its first hour is complete.
The preceding diagnostic attempts cannot substitute for the three paused
conditions. Failure-only capture now retains bounded actual/reference F32LE
samples for trimmed failures and changed full windows whose reference is
already held. Exact SHA rejection is preserved even if numerical tolerance
passes; successful delivery retains no additional PCM. The capture paths,
load modes, observer runner, admission gate and pause controls have CPU-only
regression checks. Both Build workflows passed on producer commit `b375522`.

The [evidence archive](data/isolation/manifest.json) preserves executed source
versions, plans, raw compressed events/traces, source/binary hashes, lifecycle
records and audits. Model weights, binaries and generated WAV/F32 files are
excluded. Successful seek parity is an audited runtime assertion with hashes,
not a fresh comparison of archived waveforms.

The final worker and probes exited, the fixture was removed, and owned window,
desktop-idle and system sleep inhibitors were released. The next condition
was blocked explicitly by the user pause; its scheduler exit is not a failure
of the completed hour. No further test is scheduled or running.

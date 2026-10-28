# Shared attention staging on the laptop and Apollo — 2026-10-28

The laptop still missed the strict service-hop p99 target of less than 4.095 seconds: the baseline reached 9.706 seconds and the candidate reached 9.833 seconds, with 11.947 and 13.771 seconds of steady-playback starvation respectively. The baseline's ownership failure stopped its pair; the candidate ran in a fresh follow-up window. Apollo completed all three short virtual-audio arms below the target. No runtime is promoted, and M2, the shared M3 endurance gate and release 0.1.0 remain open.

The candidate stages full-F32 attention K/V data in shared memory for the existing Intel 64-wide, four-row path. Row/column tiles, workgroup, precision, weights, conversion quality and shader archive are unchanged. The NVIDIA path already stages this data; the new Intel condition is inactive there. Its Apollo timings therefore provide controls, not evidence of an NVIDIA optimization.

Before player testing, all 16 candidate outputs matched their host's baseline byte for byte. The laptop baseline also matched the earlier accepted Intel output hashes. This was a silent, fixed-order native RPC comparison with no audio/UI load. Native RPC timing omits parts of the complete service hop and cannot qualify its latency target.

| Silent native RPC comparison | Requests per arm | Baseline median / p99 | Candidate median / p99 |
| --- | ---: | ---: | ---: |
| Laptop | 16 | 4.831 / 6.560 s | 4.518 / 4.910 s |
| Apollo | 16 | 0.681 / 16.587 s | 0.648 / 17.189 s |

Apollo's silent outliers were its first requests. Its numeric-test cache directories contained no files; that observation alone does not establish why the first requests were slow. The silent owners did not measure simultaneously: Apollo DNS failed before connection, and its first run completed over the user-supplied WireGuard route `10.4.20.101` within the same original 20-minute window. The existing `[apollo.kyaulabs]:4222` host-key identity was preserved.

The loaded comparison used current player source `1f8f0d5cff08ddd6426348b1665953e78347717a`, normal optimized builds, Best-quality stems and the same inputs and model. Both arms used 16 ms forced redraw. Each received an independent copy of its host's pinned retained shader-cache seed. Both host owners started concurrently from fresh conditions at 17:21:22.431 UTC; natural laptop settling delayed its measurement until Apollo's candidate was running. Variants were sequential per host, with no retries. The laptop used visible Wayland UI and host audio. Apollo used private PipeWire audio and offscreen software UI in the CONFLUX-owned Docker environment; no Kairos or Hexforged resource was changed.

The candidate-only follow-up started both host owners concurrently at 17:55:54.330 UTC under fresh user-confirmed conditions. It reused the same source, native players, public ownership runner, inputs, model and retained-cache seeds; it did not use the later ownership-diagnostic source change. There was no baseline repeat or retry. Its laptop cleanup proof passed, while latency and audio health failed.

| Player load | Hops | Service p99 / maximum | Hops ≥4.095 s | Steady starvation | Control/recovery starvation |
| --- | ---: | ---: | ---: | ---: | ---: |
| Laptop baseline | 17 | 9.706 s | 11 | 11.947 s | 29.803 s |
| Apollo baseline, virtual audio | 22 | 1.017 s | 0 | 0 s | 14.320 s |
| Apollo candidate, virtual audio | 22 | 0.968 s | 0 | 0 s | 14.688 s |
| Laptop candidate, follow-up | 17 | 9.833 s | 7 | 13.771 s | 24.032 s |
| Apollo candidate control, follow-up, virtual audio | 23 | 0.852 s | 0 | 0 s | 14.485 s |

All five measured intervals completed 120 seconds. Counts and nearest-rank p99 were independently recomputed from every captured validated hop; p99 equals the maximum at these sample counts. Starvation was independently summed from closed native intervals at 48 kHz. No completed hop or failed interval was removed. The fixed order, separate laptop test windows and different thermal histories do not establish a causal speedup.

The laptop's longest hop spent 9.375 seconds in GPU graph computation out of 9.706 seconds of service time. Its largest source-read duration was 29.389 ms and its largest overlap duration was 193.588 ms. The candidate's longest hop spent 9.377 seconds in GPU graph computation out of 9.833 seconds of service time. These are nested observations, not additive independent phases. They place most of the observed stall inside native graph execution; they do not isolate a kernel or prove the laptop cannot meet the target.

All five runs acknowledged all 16 automated controls, exercised a seek while an earlier generation was incomplete, validated one stale-packet discard and completed final recovery. The seek evidence uses retrospective C++ acceptance/completion stamps; it does not establish native inference-start timing. Hard cancellation was not exercised, so no hard-cancellation latency is claimed.

All five reported zero callback execution-deadline misses, invalid buffers, rejected commands and source-read failures. Every recording finalized with accepted frames equal to written frames and zero overflow. Those checks preserve the starvation above; a complete WAV is not a gap-free-audio verdict. Apollo's native functional checks passed; both laptop native checks failed. None of the short Apollo runs qualifies physical audio, visible laptop UI, sustained latency or a release.

The laptop candidate additionally failed its final diagnostic drain with `Finite diagnostic tail final health/close failed`. That 8.208-second tail contained 83,200 starved frames, or 1.733 seconds, after the measured interval had closed and its WAV had finalized. Recording frames remained fixed at 6,035,200 while output continued. This later audio is unrecorded and is reported separately from the table's 120-second steady/control totals. Apollo's follow-up tail had no starvation.

The laptop retained its normal 15 W ceiling and unchanged thermal guard. During playback, 61 of 69 temperature samples reported power-saver mode and eight reported Balanced; the long-term MMIO allowance was 7.5 or 15 W. The sampled maximum was 85°C, with one sample at or above 85°C and none at or above 95°C. Samples were about two seconds apart, with a maximum gap of 2.153 seconds. In the follow-up, 49 of 67 samples reported power-saver and 18 reported Balanced, again with 7.5 or 15 W long-term MMIO allowances. Its sampled maximum was 88°C, with two isolated samples at or above 85°C and none at or above 95°C; its maximum sample gap was 2.244 seconds. These samples neither establish continuous exposure nor exclude shorter spikes. No profile or power cap was selected or written by either test.

The laptop owner retained `cleanup_proven=false`: a previously observed `export.py --describe` child disappeared during a later ownership walk. The original diagnostic does not identify which process/parent read failed. The pair therefore stopped before its second arm. A later check found every recorded child identity absent, no live cache job or PipeWire object remaining, and the owned unit stopped. That later check does not upgrade the failed ownership proof or delete the preserved cache. The candidate follow-up independently proved ownership and cleanup; this does not repair the original baseline's failed proof. Across both windows, all three Apollo containers were removed, the owned test units stopped and the temporary KDE dimming/locking/sleep inhibitors released. Both test windows are closed.

The follow-up Python change records the failing walk phase and any partial child identity. It retains the original failure reason and strict ownership/cleanup rejection, including previously known children and PID/parent changes. Two focused checks failed on the original missing diagnostic; all 36 offline runner tests passed after the change. No player, DSP, model, power or thermal behavior changed, and no new hardware test has run with that diagnostic source.

The [evidence manifest](data/shared-staging-player-2026-10-28/manifest.json) binds 59 text members by original, decoded and gzip hashes. Its original 34 captures are unchanged; 25 follow-up captures add the complete candidate native/observation streams, host samples, independent arithmetic, dispatch and terminal checks. The archive also retains the numeric prerequisite, runtime patch and identities, and original failed laptop result. Audio, weights, executables and driver-cache payloads are excluded. The 60-minute current-player laptop soak, hard cancellation and other M2 gates remain outstanding.

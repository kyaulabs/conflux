# Shared attention staging on the laptop and Apollo — 2026-10-28

The laptop baseline missed the strict service-hop p99 target of less than 4.095 seconds: 17 completed hops reached 9.706 seconds, with 11.947 seconds of steady-playback starvation. Its controller stopped before the candidate because process ownership was not fully proven. Apollo completed both short virtual-audio arms below the target. The laptop candidate remains untested under player load; no runtime is promoted, and M2, the shared M3 endurance gate and release 0.1.0 remain open.

The candidate stages full-F32 attention K/V data in shared memory for the existing Intel 64-wide, four-row path. Row/column tiles, workgroup, precision, weights, conversion quality and shader archive are unchanged. The NVIDIA path already stages this data; the new Intel condition is inactive there. Its two Apollo timings therefore provide a control, not evidence of an NVIDIA optimization.

Before player testing, all 16 candidate outputs matched their host's baseline byte for byte. The laptop baseline also matched the earlier accepted Intel output hashes. This was a silent, fixed-order native RPC comparison with no audio/UI load. Native RPC timing omits parts of the complete service hop and cannot qualify its latency target.

| Silent native RPC comparison | Requests per arm | Baseline median / p99 | Candidate median / p99 |
| --- | ---: | ---: | ---: |
| Laptop | 16 | 4.831 / 6.560 s | 4.518 / 4.910 s |
| Apollo | 16 | 0.681 / 16.587 s | 0.648 / 17.189 s |

Apollo's silent outliers were its first requests. Its numeric-test cache directories contained no files; that observation alone does not establish why the first requests were slow. The silent owners did not measure simultaneously: Apollo DNS failed before connection, and its first run completed over the user-supplied WireGuard route `10.4.20.101` within the same original 20-minute window. The existing `[apollo.kyaulabs]:4222` host-key identity was preserved.

The loaded comparison used current player source `1f8f0d5cff08ddd6426348b1665953e78347717a`, normal optimized builds, Best-quality stems and the same inputs and model. Both arms used 16 ms forced redraw. Each received an independent copy of its host's pinned retained shader-cache seed. Both host owners started concurrently from fresh conditions at 17:21:22.431 UTC; natural laptop settling delayed its measurement until Apollo's candidate was running. Variants were sequential per host, with no retries. The laptop used visible Wayland UI and host audio. Apollo used private PipeWire audio and offscreen software UI in the CONFLUX-owned Docker environment; no Kairos or Hexforged resource was changed.

| Player load | Hops | Service p99 / maximum | Hops ≥4.095 s | Steady starvation | Control/recovery starvation |
| --- | ---: | ---: | ---: | ---: | ---: |
| Laptop baseline | 17 | 9.706 s | 11 | 11.947 s | 29.803 s |
| Laptop candidate | — | Not run | — | — | — |
| Apollo baseline, virtual audio | 22 | 1.017 s | 0 | 0 s | 14.320 s |
| Apollo candidate, virtual audio | 22 | 0.968 s | 0 | 0 s | 14.688 s |

The three measured intervals completed 120 seconds. Counts and nearest-rank p99 were independently recomputed from every captured validated hop; p99 equals the maximum at these sample counts. Starvation was independently summed from closed native intervals at 48 kHz. No completed hop or failed interval was removed. A fixed-order pair, different thermal histories and the missing laptop candidate do not establish a causal speedup.

The laptop's longest hop spent 9.375 seconds in GPU graph computation out of 9.706 seconds of service time. Its largest source-read duration was 29.389 ms and its largest overlap duration was 193.588 ms. These are nested observations, not additive independent phases. They place most of the observed stall inside native graph execution; they do not isolate a kernel or prove the laptop cannot meet the target.

All three runs acknowledged all 16 automated controls, exercised a seek while an earlier generation was incomplete, validated one stale-packet discard and completed final recovery. The seek evidence uses retrospective C++ acceptance/completion stamps; it does not establish native inference-start timing. Hard cancellation was not exercised, so no hard-cancellation latency is claimed.

All three reported zero callback execution-deadline misses, invalid buffers, rejected commands and source-read failures. Every recording finalized with accepted frames equal to written frames and zero overflow. Those checks preserve the starvation above; a complete WAV is not a gap-free-audio verdict. Apollo's native functional checks passed, while the laptop's native check failed steady-playback health. Neither short Apollo run qualifies physical audio, visible laptop UI, sustained latency or a release.

The laptop retained its normal 15 W ceiling and unchanged thermal guard. During playback, 61 of 69 temperature samples reported power-saver mode and eight reported Balanced; the long-term MMIO allowance was 7.5 or 15 W. The sampled maximum was 85°C, with one sample at or above 85°C and none at or above 95°C. Samples were about two seconds apart, with a maximum gap of 2.153 seconds. These samples neither establish continuous exposure nor exclude shorter spikes. No profile or power cap was selected or written by the test.

The laptop owner retained `cleanup_proven=false`: a previously observed `export.py --describe` child disappeared during a later ownership walk. The original diagnostic does not identify which process/parent read failed. The pair therefore stopped before its second arm. A later check found every recorded child identity absent, no live cache job or PipeWire object remaining, and the owned unit stopped. That later check does not upgrade the failed ownership proof or delete the preserved cache. Both Apollo containers were removed, both test units stopped and the temporary KDE dimming/locking/sleep inhibitors released. This test window is closed.

The follow-up Python change records the failing walk phase and any partial child identity. It retains the original failure reason and strict ownership/cleanup rejection, including previously known children and PID/parent changes. Two focused checks failed on the original missing diagnostic; all 36 offline runner tests passed after the change. No player, DSP, model, power or thermal behavior changed, and no new hardware test has run with that diagnostic source.

The [evidence manifest](data/shared-staging-player-2026-10-28/manifest.json) binds 34 text members by original, decoded and gzip hashes. It includes the numeric prerequisite, runtime patch and identities, original failed laptop result, complete native/observation streams, host samples, independent arithmetic, dispatch and terminal checks. Audio, weights, executables and driver-cache payloads are excluded. The remaining laptop candidate comparison requires a new test window; the 60-minute current-player soak and other M2 gates remain outstanding.

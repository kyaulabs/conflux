# Tempo-aware prepared continuation, 30 October 2026

A live-stem deck can now continue from an already prepared four-stem bank while
retaining its selected BPM and Master Tempo. The handoff preserves the exact
fractional nominal source cursor, current stem ramps, channel gain, EQ, CUE,
crossfader, metadata and the other deck. It does not launch another separator or
substitute the original stereo mix.

An active approved recovery range finishes at its audible OUT before the owner
changes. The callback clips processed prefixes to that boundary and revalidates
source generation, transport revision, range identity and selected mode before
every attempt. A stale request cannot cut the range or replace a newer owner.
Old source retirement, processor construction and priming stay off the callback.

The prepared processor starts at the containing PCM frame and retains the exact
nominal transport position. Its fresh Best-quality DSP uses the existing transport
envelope; old filter history and sub-frame audio phase are not transferred.
Recovery does not permit tempo edits while held: leave recovery first. Manual
transport still invalidates consent and pending continuation.

| Check | Result and scope |
| --- | --- |
| Complete local build and CTest | All 60 targets passed from the frozen product source. |
| Concurrent Apollo core checks | All ten selected audio suites passed without GPU or physical audio devices. |
| Direct callback takeover | All eight cases passed across both hosts: -6%/+6%, Master Tempo off/on, approved OUT, stale tickets, peer clocks, recording and owner retirement. Seven cases retained a fractional parked cursor. |
| Callback audio | Zero additional affected-deck underruns through the measured recovery/takeover spans. Initial tempo setup buffering is reported separately. Every recorded master sample matched the peer oracle; affected-deck DSP PCM and both buses have complementary core coverage. |
| ASan/UBSan and leaks | All seven selected suites passed on both hosts with the audio library instrumented. External DSP libraries were not instrumented. |
| ThreadSanitizer | All seven suites passed on the laptop. Apollo could not initialize its runtime under the container memory layout/ASLR policy; none of those seven test bodies ran there. The failure is retained and no security setting was changed. |
| Baseline listening | The user accepted Club XL live-stem BPM changes, Master Tempo on/off and Reset in source e86585b. The client closed cleanly. This verdict does not cover the new recovery/continuation code. |

The full controller fixture is CPU-limited and processes generated audio. Its
initial strict runs recorded underruns, including in the old recovery processor
before owner replacement. Increasing Apollo from one to two CPUs produced a pass,
but the unchanged laptop control failed before takeover, so that comparison does
not prove a single cause. Those results remain failures. They are not replaced by
the functional results below.

The new processed controller case in functional-only mode reports every affected-
deck underrun and verifies that recovery parks the cursor and prepared playback
advances only for consumed frames at the retained tempo. The peer and every
recorded master frame remain exact. Strict mode, the existing raw cases, direct
callback checks and actual-player audio/performance gates retain their continuity
requirements. This distinction does not qualify the CPU-limited processed fixture
for uninterrupted audio.

All six final controller cases passed across the two hosts: processed takeover,
unchanged raw takeover and raw cancellation. The processed functional run reported
23,774 total affected-deck starved frames on the laptop and 10,545 on Apollo under
the fixture CPU limits. During the checked post-takeover advance, the laptop had
zero starved frames and Apollo had 614; both nominal advances matched only the
consumed frames at 1.06 times speed. These are functional results, not audio
continuity passes. All owned units, cgroups, containers and private graphs retired.

The processed fixture also stopped once because its first-Play observer required
source frame 8,192 even though recovery could enter and park F earlier. The final
observer follows the actual Play/Arm acknowledgement and consumed progress. Raw
case requirements remain unchanged. A receipt-path mismatch stopped a separate
entry before any test process started; original per-host receipts were verified.

The fractional-cursor and processed-OUT regression cases failed before their
respective changes. Priming tests also cover cancellation, invalid positions,
mode mismatch, unused-proof invalidation and supersession. Original failures and
intermediate compile/setup errors remain in the evidence.

A loop observer previously counted render attempts while an asynchronous worker
had not supplied PCM. The corrected observer waits for committed source advances
within the original deadline. Delayed-read reproduction and the full loop suite
passed on both hosts; the original failure and intermediate compile/setup errors
are retained.

The [verification record](data/tempo-continuation-2026-10-30/verification.json)
pins the source snapshots and scopes. The [archive manifest](data/tempo-continuation-2026-10-30/manifest.json)
binds original text receipts, failures and CPU-only protocols. No music, model,
executable or cache is included.

Current-code recorded-music recovery acceptance, laptop/Apollo audio/UI checks
and the sustained workload remain open. No new physical audio or GPU test ran for
this report. The strict Best-quality service target remains p99 below 4.095
seconds, and the laptop remains the primary release target. The earlier fixed-mix
hour applies to its own source and conditions. M2, M3 and release remain open.

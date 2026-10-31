# Live-stem tempo with resident recovery, 30 October 2026

Configured live-stem decks can retain their selected BPM and Master Tempo when
entering an explicitly approved resident recovery range. The normal live tempo
processor stays parked at its consumed source position while a separate worker
processes the immutable four-stem range. Current stem gains still apply when the
audio callback consumes the output. Healthy playback can return at recovery OUT
without repeating separation or resetting the selected tempo.

The resident worker adds one bounded output queue, at most 256 KiB of four-stem
PCM. DSP and thread storage are additional. The existing raw resident range,
2 MiB live input history and linear output queue remain separate. No original
mix, model change or dependency is introduced. The callback publishes requests
and consumes queued PCM; processing, allocation and source reads stay off callback.

A terminal live-producer fault cannot invalidate already accepted resident PCM.
Automatic entry still requires the exact approved range and an armed current
source. An exit request remains pending while no healthy return is available.
Cancel retains the original fault and does not restart separation. Malformed bank
staging fails closed in both paths. A seek, replacement or output reconstruction
clears recovery consent; reconstruction joins both old processors before replacing
their input owner and restores the selected BPM and key lock.

An armed recovery request now accepts a pending live tempo configuration. It waits
for compatible output before becoming ready, rather than silently rejecting the
one-shot Arm command queued after Play. Once that configuration reaches the
consumer, an empty output ring no longer looks like an older configuration.

| Check | Result and scope |
| --- | --- |
| Complete local player build and CTest | All 60 targets passed, including 238 offscreen UI cases. The owned user unit and original cgroup were absent afterward. |
| Concurrent Apollo CPU check | All nine selected audio suites passed from the same frozen audio sources. Its private container had no GPU or audio devices and was absent afterward. |
| Nonzero recovery range | The final test-only change starts the Master Tempo cases at source frame 1,234. The changed live-tempo suite passed on both hosts concurrently; both product audio archives were unchanged. |
| Exact waveform oracle | At -6% and +6%, with Master Tempo off/on, every consumed recovery sample matched an independent immutable file-loop reference after the selected stem gain. Tests covered explicit entry, repeated wraps, healthy return and automatic entry after terminal producer failure. |
| Render and lifecycle | Actual render commands verified acknowledgements, a frozen fractional live cursor, cue/mix gains, peer/output clocks, reconstruction, held stop and rejection of unavailable held tempo edits. |
| ASan/UBSan and leaks | Four selected suites passed with the audio library instrumented. Only test executables use non-PIE linking to avoid the previously observed shadow-memory startup overlap. Production flags and host security settings are unchanged. |
| ThreadSanitizer | The same four selected suites passed with the audio library instrumented. |

The [verification record](data/live-tempo-recovery-2026-10-30/verification.json) pins the changed source files and exact check variants. The [archive manifest](data/live-tempo-recovery-2026-10-30/manifest.json) binds the original text receipts, failure logs and CPU-only protocols in three deterministic gzip bundles. No music, model, executable or cache is included.

The first behavior test failed because configured recovery sources had no tempo
hooks. The pending-Arm regression then failed before its fix. Both failures are
retained with subsequent results, as are two intermediate compile errors. The
full 60-target result preceded only the nonzero-range test edit; no product source
changed afterward. Third-party DSP libraries are outside sanitizer instrumentation.
These CPU/generated-audio checks do not establish musical quality, physical audio
continuity or GPU latency.

Tempo changes and reset remain unavailable while resident recovery is active.
Leave recovery first. That checkpoint retained unity/integer-cursor
continuation eligibility. The [tempo-aware continuation follow-up](tempo-continuation-2026-10-30.md)
now covers fractional cursors and the processed recovery OUT handoff. Manual loops and
scratching still require prepared audio. These restrictions are visible in the
controls and are not a waiver of the remaining M2/M3 integration work.

Recorded-music acceptance, current-code laptop/Apollo audio/UI checks and the
required sustained workload have not run. The strict service p99 target remains
below 4.095 seconds at Best quality. The earlier fixed-mix hour qualifies its own
recorded source and conditions, not this revision. M2, M3 and release remain open.

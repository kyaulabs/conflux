# Check peer feedback over the playback interval — 28 October 2026

The resident-takeover integration check could reject healthy playback because its peer-continuity predicate required a cumulative GUI snapshot-drop counter to be zero. The audio callback deliberately drops these bounded state reports when the GUI cannot drain its 32-entry queue; it continues rendering audio. Drops from paused setup therefore remained visible after playback started.

The predicate now requires the counter to remain unchanged between the two asserted playback snapshots. It still rejects any new drop or counter reset within that interval. Exact peer/output cursor advance, source generation, playing state, buffering, read errors, starvation, invalid buffers and rejected commands retain their existing checks. Failure reports now include both compared snapshots and the resident scenario's audio, recorder and actions before shutdown.

A private copy inserted a 350 ms GUI pause before Play; the original predicate then rejected two healthy snapshots whose drop counts were both 37. Peer and output clocks advanced by exactly the same amount, with no peer starvation or other health error. A second private copy used the corrected predicate.

| Generated check | Result |
|---|---|
| Original predicate, paused-setup delay | Rejected unchanged 37 → 37 snapshot drops; all other peer/clock checks healthy |
| Corrected predicate, paused-setup delay | Passed with 41 historical drops retained; every one of 286,464 recorded stereo frames checked, zero oracle error and no excluded frames |
| Corrected predicate, delay during playback | Rejected 0 → 39 snapshot drops; all other peer/clock checks healthy; both snapshots retained |

The pause hooks exist only in the private diagnostic copies. The tracked helper has no new delay, environment option or injection mode. Its normal build passed all ten existing virtual scenarios across nine commands: live stall/exit, prepared takeover/miss/cancel/stop/foreground and resident hold/takeover/cancel. Each private daemon was reaped and its graph closed.

The original [post-merge Linux failure](https://github.com/kyaulabs/conflux/actions/runs/40825912623/job/123288786957) remains retained. Its 57 core tests and preceding private cases passed before resident takeover failed at this predicate. That older report omitted the compared values, so the controlled reproduction demonstrates a real false-failure mode without proving the exact original scheduler interleaving. Future failures retain the missing context.

No production audio code, model, runtime, thermal policy, recording oracle or latency target changed. These generated virtual tests do not qualify GPU cancellation, physical output, sustained performance or the 4.095-second laptop gate. The failed player hour and current latency results remain failed; M2, M3 and release gates stay open.

The [evidence manifest](data/resident-feedback-interval-2026-10-28/manifest.json) binds the original CI log, exact build/source pins, private diagnostic copies, complete scenario results and independent interval arithmetic. Generated WAVs, binaries and caches are excluded.

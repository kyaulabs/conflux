# Selected-tempo hour, 5 November 2026

Apollo completed 3,600.013 measured seconds at 106 BPM with Master Tempo enabled,
but failed continuity. Its complete captured service population reached 1.008-second
p99 across 579 deliveries, below the unchanged 4.095-second target. The live-stem deck
reported 672 missing steady-output frames (14 ms) in one playback cycle. The primary
laptop did not reach playback in either admission attempt. Neither host qualifies
the current primary-laptop hour, M2/M3, or release `0.1.0`.

| Observation | Apollo result |
| --- | --- |
| Service p99 / maximum | 1,007.718 / 1,032.939 ms; no delivery at or above 4,095 ms |
| Steady missing frames | A: 672; B: 0 |
| Control/startup/replay missing time | 427.371 seconds, retained separately |
| Replays / peer rewinds | 143 / 8; final recovery completed |
| Callback deadline misses / driver discontinuities | 0 / 0 |
| Source-read errors / invalid buffers / rejected commands | 0 / 0 / 0 |
| Recording | 173,332,736 accepted and written frames; zero overflow; WAV finalized |
| GPU temperature | Sampled maximum 66°C |
| Cleanup | Owned container absent; private sink/daemon reaped; completed unit retired |

The recording includes preparation before the measured hour. Its frame count is
not a claim that every recorded frame belongs to the measured interval. The finite
post-measurement drain is separate and unrecorded. Hard worker cancellation was not
exercised by this hour; the earlier [full-player shutdown check](full-player-shutdown-2026-11-04.md)
retains its separate cancellation scope.

The failed steady interval spans output frames 71,624,448–72,685,312, in generation
61 after a replay. Every other steady interval reported zero missing frames. The
capture has raw publication timing but lacks the tempo converter's queue state at
the short read. It cannot establish whether the cause was queue geometry, producer
scheduling, or another converter-path delay. Healthy callback timing and fast model
deliveries do not establish continuous source delivery.

The signed source was `8b9669e92e764822b272984502b25624609d419d`, including the bounded
resident-reader repair and MP3 preparation fix. The runtime/model, Best quality,
16 ms forced redraw, 100→106 BPM selection, Master Tempo and existing hour validators
were pinned. Apollo used the dedicated CONFLUX Docker environment, private virtual
audio and software UI. It supplies secondary-machine evidence, without physical
speaker/interface or primary-laptop qualification. No GPU power policy changed.

The laptop's first attempt ended before playback when administrator authentication
completed while the thermal guard was still in Power Saver. No short-term cap was
applied. The second attempt waited for natural recovery before authentication but
required an extra stable minute. It timed out before authentication after brief
95–97°C readings; no native player, model workload or cap started. OneDrive was
restored after each attempt, and both owned units are retired. The user subsequently
confirmed other work was running and would be paused; that does not retrospectively
establish the cause of those temperature spikes.

The unarmed follow-up requests authentication when the guard naturally returns to
Balanced at or below 70°C, then retains the original full 60-second post-cap settling
check. It does not select a profile or change thermal rules. Its 89 offline checks,
nine startup checks and real unarmed KDE entry passed. A new physical test still
requires fresh readiness and the bounded sync-pause/cap approval.

The [evidence manifest](data/selected-tempo-hour-2026-11-05/manifest.json) binds the
native result, failed assessment, routing cleanup, all 579 completed service-hop
records, GPU samples and both laptop admission failures. Raw observations and bounded
failed/clean PCM excerpts remain under `/srv/conflux-tests/laptop/runs/h1105/`.
The large generated hour WAV and private caches were removed after checksum and
excerpt verification, freeing 1.29 GiB. The excerpts' mapping to global output frames
is approximate because recorder stop is asynchronous; their retained PCM bytes are
exact. No user music or recordings were removed.

The full M2/M3-before-release decision remains in force. The earlier successful
[fixed-mix laptop hour](current-player-hour-2026-10-29.md) remains valid in its stated
scope; it does not clear this selected-tempo continuity failure or the unfinished
automatic musical-analysis and integrated-fallback gates.

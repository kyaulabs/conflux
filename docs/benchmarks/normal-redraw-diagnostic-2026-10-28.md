# Explicit redraw-load diagnostic

The explicit 50 ms redraw diagnostic passed offline verification. Its first [physical comparison](redraw-player-2026-10-28.md) then completed the failed 16 ms baseline at 10.323-second p99; the 50 ms arm stopped during thermal admission before playback. No redraw performance effect is established. The diagnostic does not replace the failed [Balanced waveform comparison](balanced-waveform-player-2026-10-27.md) or change the laptop's 4.095-second target.

The normal controller polls state every 50 ms. The actual-model benchmark additionally calls `window.update()` every 16 ms, even when no new controller snapshot is due. Those are update-request intervals, not guaranteed presentation rates. Qt may coalesce requests or redraw for other reasons. The difference identifies a factor to isolate; it does not establish that forced redraws caused the earlier stalls.

The host runner's explicit `--normal-redraw` option requires short, visible `--live-stems --active-controls` and exactly 120 seconds. It selects native mode `live-stems-controls-normal-redraw`, which records `forced_redraw_interval_ms=50`. The default mode retains 16 ms. Private generated checks may use their existing one-to-ten-second bounds. Audio, model, runtime, control schedule, visibility checks and finite-close rules are unchanged.

Validation requires the expected mode and cadence. A 50 ms result cannot be accepted as the default stress result or an hour qualification, even if another result field claims success. Existing health, telemetry, phase and strict service-target checks remain active. The new diagnostic stays unqualified.

All 35 fast runner tests passed. Two new regressions reject the parent implementation, then pass with explicit admission and evidence checks. Eight actual-executable boundary checks passed without starting audio or a model. Both generated ten-second private-audio runs completed all sixteen Qt controls with the expected mode/cadence, functional-only classification, no physical audio and no qualification. These fixtures use fake stems and do not measure laptop service performance.

The [offline evidence manifest](data/normal-redraw-diagnostic-2026-10-28/manifest.json) binds source/build hashes, generated native results, boundary checks, intentional parent failures and cleanup. All 34 text files passed hash verification and secret scanning; audio, fake model/executable and cache payloads are omitted.

Any further physical comparison must use the same built player with 16 ms and 50 ms forced redraw requests, changing only that option. The first comparison window is closed; a 50 ms result remains pending. Further work needs a newly frozen packet and fresh AC/open-lid/minimized-Codex/visible-window conditions. Best quality, normal Balanced operation, the 15 W ceiling, thermal-guard reductions and the unchanged target remain required. No power-profile or guard change is proposed, and no default-stress or release gate is waived.

# Live-stem tempo checks — 30 October 2026

Live sources without a configured recovery hold can now use the existing
±6% SRC and pitch-preserving Master Tempo processors. Raw stem input moves to
the processing worker when tempo starts; the audio callback consumes its bounded
queue and applies the current stem gains. Missing input holds source time and
reports buffering while the peer deck and output clock continue. This uses the
existing Best-quality processors and adds no dependency or model change.

Each eligible owner retains 65,536 frames of unchanged four-stem history
(2 MiB). Mode changes and recent seeks can reuse that history; older seeks start
a new provider epoch. The output queue holds at most 256 KiB, alongside the
existing 9 MiB live-buffer payload. These figures exclude DSP, thread and other
owner storage. The history contains only separated stems. No original mix or
disk cache is introduced.

Speed changes within one processor retain its generation. Key-lock changes,
seeks and live TEMPO RESET create a new generation. Live reset uses exact-unity
processing and retains the selected key lock, keeping raw input under one worker
owner. Prepared reset still returns to its normal reader. Source position remains
a nominal DSP cursor, including tail draining. Quiesced output reconstruction
joins the old input worker before restoring the selected tempo on a new worker.

The [verification record](data/live-tempo-2026-10-30/verification.json) binds the
changed source files and completed checks to the develop parent
`18b3fde9aa81298fb4bcca8964203d933a82d881`.
The [archive manifest](data/live-tempo-2026-10-30/manifest.json) pins 43 original
text records in three gzip JSON bundles. They contain no music, model weights,
executables or caches.

| Check | Result and scope |
| --- | --- |
| Normal player build | Complete Release player and test targets built. |
| Local CTest | First full run passed 59 of 60 targets. The sole failure expected the old prepared-only controller message. After correcting that assertion, the complete controller suite passed all 60 Qt cases; a final invalid-deck message refinement passed its three focused cases. The original failure is retained. |
| Audio behavior | Gap and partial-prefix handling, malformed input, finite samples, queue bounds, EOF, seek generations, raw gain independence, history wrap/eviction, reconstruction and stop passed. Sixteen SRC/R3 comparisons matched the same complete generated PCM exactly. The added RenderCore case passed on both hosts, covering command acknowledgements, pause, missing input, peer clocks, cue and mix controls. |
| Apollo CPU checks | All seven selected audio suites passed. The later RenderCore case passed after recompiling only its test; product audio archives were unchanged. Both containers were absent after completion. No GPU or audio device was exposed. |
| ASan/UBSan and leak checks | Four selected suites passed. The first executables could not initialize ASan because their PIE mapping overlapped shadow memory. Relinking only the test executables without PIE resolved startup; the instrumented audio archives, production flags and host security settings were unchanged. Initial failures are retained. |
| ThreadSanitizer | All four selected suites passed with the product audio library instrumented. Third-party DSP libraries were not instrumented in either sanitizer run. |

The live-owner behavior test first failed because tempo hooks were unavailable.
A subsequent mode-change regression detected repeated separation of already
available stems; the bounded history resolved it. Both functional failures
remain in the record.

Configured recovery holds retain their existing raw playback and fallback
behavior, with tempo unavailable. Tempo and recovery-hold coexistence is still
unfinished. Manual loops and scratching remain available only on prepared audio.
Recorded-music listening, dynamic live-control acceptance and current-code
laptop audio/UI performance have not run. The prior fixed-mix hour qualifies its
recorded source and conditions; it does not qualify this new code. The strict
4.095-second p99 target is unchanged. M2, M3 and the first release remain open.

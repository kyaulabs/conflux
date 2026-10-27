# Accepted waveform revisions

Deck-state updates now reuse an accepted waveform without reading its peak
array again. The controller exposes a per-deck revision that advances when an
overview is installed or cleared. QML retains the controller, deck index and
revision alongside its copied peaks. A matching identity skips the payload
access and comparison loop.

The source digest alone cannot identify the overview: loading the original
mix and a prepared stem bank can retain that digest while displaying different
peaks. A new revision therefore refreshes the accepted view even for the same
source. Revisions use strings so their 64-bit values retain exact identity in
JavaScript. Position, gain, meter and BPM updates leave the accepted overview
revision unchanged.

Pending preparation previews keep the existing comparison path and invalidate
the accepted identity. Returning from a cancelled or failed preparation reads
the retained overview again. Unloading, switching decks or controllers, and
snapshots without a usable revision also refresh the appropriate data. Theme
and size changes continue to repaint the retained peaks.

The regression creates the actual `DeckSurface.qml` component with observable
waveform getters. It first failed on the previous code because state updates
read the accepted payload. After the change, sixty-four position/gain updates
made **zero accepted-waveform payload reads**. The same probe checks replacement
under an unchanged source ID, identical replacement data, advancing pending
previews, cancellation restoration, unload/reload, deck/controller identity and
missing-revision updates. These are access-count and state assertions, with no
wall-time performance threshold.

All **232 UI results** passed under offscreen/software Qt 6.12.0. All **59 native
controller results** also passed. The extended native snapshot test verifies
that the revision is present, stays stable during BPM changes, advances on a
same-source reload, leaves the peer unchanged and survives normal shutdown
with the retained overview. The earlier snapshot stays immutable.

The [evidence manifest](data/m3-waveform-revision-2026-10-27/manifest.json) retains
the intentional red regression, passing focused/full tests, build logs and
source-file hashes. Product and fixture sources are committed with the record;
compiled files, media and models are omitted.

This removes demonstrated redundant work, but supplies no new measured CPU or
service-latency benefit on the laptop. The last retained
[player GPU trace](gpu-client-usage-2026-10-26.md) reached **8.889-second p99**
against the unchanged **4.095-second target**. A laptop comparison and the
completed actual-player hour remain pending. Best quality, the 15 W ceiling,
thermal guard and M2/M3/first-release gates remain unchanged.

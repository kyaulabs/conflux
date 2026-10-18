# CDJ-3000 control inventory

The current redraw uses the original CDJ-3000 and the four-channel
DJM-900NXS2 arrangement in the [supplied photograph](handoff/references/pioneer-cdj3000-djm900nxs2-setup.png).
The [CDJ-3000 operating instructions, pages 14–16](https://downloads.support.alphatheta.com/manuals/dj-players/CDJ-3000/CDJ-3000_DRI1586A_manual.pdf#page=14)
identify the controls; pages 47–49 distinguish tempo, Master Tempo and jog
operations. This inventory covers the visual redesign and the existing manual
audio functions. It adds a bounded source-peak overview without changing audio
PCM or marking M1 complete.

Both deck faces have the same orientation. Put the display across the upper
third, eight Hot Cue buttons immediately beneath it, and the large jog in the
lower center. Keep CUE above PLAY/PAUSE at the lower left and the vertical
TEMPO rail at the lower right. The left column holds search, direction, beat
navigation and loop groups; browse and tempo-related controls sit to the right.
The original model has a nine-inch display and eight horizontal Hot Cue
buttons. [Original CDJ-3000 product reference](https://www.pioneerdj.com/en/product/dj-players-turntables/cdj-3000/).

For the laptop layout, use those relationships without copying physical
dimensions. A starting estimate from the perspective photograph puts the jog
center near 48% of deck width and 68% of deck height, with a diameter around
55% of deck width. Transport sits near the left edge and the tempo rail near
the right edge. Preserve a legible central mixer with four vertical strips,
stacked knobs above channel faders and a bottom crossfader. Decks A/B work;
the C/D strips must say they are unavailable.

| Hardware group and position | CONFLUX behavior in this redraw |
| --- | --- |
| Display and browse area, top | Show the loaded filename, actual elapsed/total time, playback state and load status. The Library drawer accepts absolute local paths; LOAD calls `loadDeck`. Indexed library browsing, playlists and media-source selection remain unavailable. |
| Position rail within the display | Seek to an actual source time with `seekSeconds` over the source-peak overview. The operation has no beat quantization, cue preview or scratch semantics. |
| PLAY/PAUSE, lower left | Toggle the loaded deck's requested playing state with `setPlaying`. Show the requested PAUSED/PLAYING state in text as well as color; source time clamps at EOF. |
| Channel faders, mixer | A/B CHANNEL LEVEL controls call `setDeckGain` with a linear value in [0,1]. This is the existing deck gain, not a hardware trim/EQ model. C/D remain disabled. |
| MASTER LEVEL, mixer | Call `setMasterGain` in [0,1]. The deck's hardware MASTER button selects a sync master and must remain disabled. |
| Jog, jog mode/adjust and brake | Disabled: scratch, temporary pitch bend/nudge, jog frame search and brake processing are not implemented. Do not animate rotation as a claim of jog interaction. |
| CUE, Hot Cues A–H and cue memory | Disabled: cue points and cue audition are not implemented. Do not turn CUE into an unlabeled seek-to-zero action or draw stored cue positions. |
| SEARCH/TRACK SEARCH and direction | Disabled: held fast search, track navigation, reverse and slip playback are not implemented. Use the explicit position rail for current seeking. |
| Loop IN/OUT/RELOOP, 4/8 BEAT LOOP and BEAT JUMP | Disabled: loops, beat analysis and quantized beat navigation are not implemented. |
| TEMPO rail/range/reset, MASTER TEMPO and KEY SYNC | Disabled: tempo/pitch processing and key matching are not implemented. Keep deck gain off the tempo rail. |
| BEAT SYNC, MASTER, QUANTIZE and SLIP | Disabled: beat synchronization, quantization and slip transport are not implemented. |
| Mixer EQ, filter, FX, crossfader, headphone cue and meters | Disabled: their processing, routing or measurement is not implemented. Do not illuminate fictitious level bars. |

BPM, key and beatgrid remain unavailable. The overview uses at most 512
buckets of `max(abs(left), abs(right))` from decoded PCM, normalized to the
source peak for display only. Its playhead shows the actual source position.
Analysis runs in the existing decode job, outside the GUI and audio callbacks. Keep preparation and output errors near the affected deck; do not
invent analysis progress, AI ownership or recorded audio states. Use approved
CONFLUX assets and theme tokens, without reproducing the hardware's logo.

Retain keyboard focus order through path, LOAD, PLAY/PAUSE, POSITION and
CHANNEL LEVEL for each deck, then MASTER LEVEL. Enter loads the path; focused
buttons accept Space/Enter; focused sliders accept arrow keys. Pointer and
touch use the same actions. Keep controls large enough to operate at the
laptop's logical resolution, with visible focus and accessible names that
include the deck. Disable transport until that deck is loaded and output is
ready, and disable loading while a decode is pending. Explain inactive groups
with visible text or focus-accessible help, rather than hover or color alone.

# CDJ-3000 control inventory

The current redraw uses the original CDJ-3000 and the four-channel
DJM-900NXS2 arrangement in the [supplied photograph](handoff/references/pioneer-cdj3000-djm900nxs2-setup.png).
The [CDJ-3000 operating instructions, pages 14–16](https://downloads.support.alphatheta.com/manuals/dj-players/CDJ-3000/CDJ-3000_DRI1586A_manual.pdf#page=14)
identify the controls; pages 47–49 distinguish tempo, Master Tempo and jog
operations. This inventory covers the visual redesign and the existing manual
audio functions, including independent headphone cue, metering and stereo
recording. Source-peak overviews leave audio PCM unchanged. Platter scratching,
5 ms live ramps and source handoffs use the same output session. M1's sustained
playback run and speaker listening acceptance are complete within the
[tested manual-audio scope](M1.md). Physical headphone isolation remains
unverified where the required hardware was unavailable.

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
| Display and browse area, top | Show embedded cover art, title, artist and album in each deck LCD, with a filename fallback and a NO ART placeholder. Elapsed/remaining source time and the source overview remain available; processed tempo uses a nominal source cursor, clamped at the track end while its actual tail drains. The Library drawer imports and lists local tracks; LOAD A/B prepares the selected track. Playlists and media-source selection remain unavailable. |
| Position rail within the display | Seek to an actual source time with `seekSeconds` over the source-peak overview. Seeking cancels preview or active scratch and preserves the stored cue. Beat quantization remains unavailable. |
| PLAY/PAUSE, lower left | Toggle the loaded deck's requested playing state on press with `setPlaying`. Show PAUSED/PLAYING separately from CUE PREVIEW; source time clamps at EOF. |
| Channel faders, mixer | A/B CHANNEL LEVEL controls call `setDeckGain` with a linear value in [0,1]. This is the existing deck gain, not a hardware trim/EQ model. C/D remain disabled. |
| HI/MID/LOW, mixer | A/B channel EQ cuts to -26 dB and boosts to +6 dB; the center is 0 dB. LOW is a 250 Hz shelf, MID a 1 kHz peak and HI a 4 kHz shelf. Changes ramp over 240 frames (5 ms). EQ follows the current source/stem mix and precedes transport fades, deck meters, pre-fader cue and channel/master gains. Recording includes EQ. Channel settings survive track replacement and output rebuild; filter history is cleared on replacement. C/D remain disabled. |
| Loop IN/OUT/RELOOP | Prepared normal audio and stem banks call `loopIn`, `loopOut` and `loopToggle`. IN prepares up to 64 seconds; OUT captures the render cursor and repeats from IN once cache is ready. EXIT continues past OUT; RELOOP returns to IN. Minimum span is 256 frames; pause retains it, while seek/jog/transport CUE, retry, replacement and output rebuild clear it. Live stems and loop-point editing remain unavailable. |
| Crossfader, bottom of mixer | Call `setCrossfader` in [0,1], with A at the left and B at the right. At center both sends stay at unity. Toward an endpoint the opposite deck fades linearly to zero; live changes ramp over 240 frames (5 ms). Channel levels, pre-fader cue, deck meters and source clocks remain independent. The master recording includes the crossfader. Assignment and curve selection remain unavailable. |
| MASTER LEVEL, mixer | Call `setMasterGain` in [0,1]. The deck's hardware MASTER button selects a sync master and must remain disabled. |
| Mixer CUE and CUE LEVEL | Toggle A/B `setCueSelected` for the separate pre-fader headphone bus; `setCueGain` controls its independent level in [0,1]. Selection follows the deck's transport and does not start a paused deck. This is separate from the temporary transport CUE button. C/D remain disabled. |
| Stereo meters beside channel faders and master/cue controls | Show actual pre-fader A/B and post-gain master/cue peak markers and RMS fills, from −60 to 0 dBFS. OVR reflects cumulative samples above ±1 in the current render session. Hover/accessibility text gives numeric values. Internal cue metering can be nonzero while CUE UNROUTED shows that no headphone audio is routed. |
| AUDIO / REC, top bar | Refresh playback ports and select explicit stereo master/cue pairs. Default selection applies only to master; cue requires four distinct destination ports across the two pairs. Record the post-master mix to a new absolute `.wav` path, with visible recording/finalization/error state and saved path. |
| Jog, jog mode/adjust and brake | Hold and drag to scratch the actual prepared stereo track or four-stem bank, following speed and direction at two source seconds per turn, limited to ±8×. Release resumes the previous play/pause request at the consumed cursor. Reset tempo and clear stored loops first; live stems and processed tempo do not support vinyl mode. Right/Up or Left/Down still search ±10 ms; the wheel searches 10 ms per notch, with Shift giving one-second steps. Search clamps to the source, preserves playing request/stored cue and cancels preview or scratch. Pitch bend, jog-mode adjustment and adjustable braking remain unavailable. |
| CUE, lower left | One temporary cue per deck, initially frame zero. Press while paused away from the cue to replace it; press during playback to return and pause. Hold at the paused cue to preview. Indicators use acknowledged cue/preview state. |
| Hot Cues A–H and cue memory | Disabled: Hot Cues, persistent cue memory and memory navigation are not implemented. |
| SEARCH/TRACK SEARCH and direction | Disabled: held fast search, track navigation, reverse and slip playback are not implemented. Use the explicit position rail for current seeking. |
| 4/8 BEAT LOOP and BEAT JUMP | Disabled: quantized loops and beat navigation are not implemented. |
| TEMPO rail/reset and MASTER TEMPO | Prepared stereo tracks and four-stem banks support ±6% speed. Move the rail down for positive tempo; MASTER TEMPO preserves pitch with Rubber Band R3. Changes and pending state follow engine acknowledgements. TEMPO RESET returns to the exact normal-read path, retaining the Master Tempo selection. Manual loops and processed tempo are mutually gated; live stems are unsupported. Range selection and KEY SYNC remain disabled. |
| BEAT SYNC, MASTER, QUANTIZE and SLIP | Disabled: beat synchronization, quantization and slip transport are not implemented. |
| Mixer TRIM, COLOR and FX | Disabled: input trim, channel filter and effects processing are not implemented. |

The [operating instructions, pages 51–52](https://downloads.support.alphatheta.com/manuals/dj-players/CDJ-3000/CDJ-3000_DRI1586A_manual.pdf#page=51)
describe paused cue setting, return-and-pause during playback, and preview while
CUE is held. CONFLUX returns a released preview to the stored cue and pauses;
the cited pages do not separately specify that release position. Pressing PLAY
while previewing keeps the current position and latches normal playback.
Release-return and the PLAY latch are explicit CONFLUX policies, not claims
proved by those pages.

CUE accepts pointer/touch holds and focused Space, Return or Enter holds;
auto-repeat adds no press. An accepted hold requests one release on physical
release, cancellation, focus loss, disable/hide, window deactivation or
destruction. Same-window focus/cancel cleanup waits one event turn so a PLAY
press can precede it; deferred cleanup cannot release a newer hold. Physical
release, disable/hide, window deactivation and destruction request cleanup
without that deferral. Queue pressure retains the release for retry before
later transport commands.

Audio settings can discover ports before loading a track through an inactive,
unlinked filter. The live session renders master and cue once through four
mono `pw_filter` outputs, with automatic linking disabled. Each stereo pair
uses two distinct ports of one sink. Cue may use another pair on a four-channel
interface or a separate stereo sink; physical clock drift and DAC alignment
remain unverified. The graph must run at 48 kHz or output is muted with an
error. Source-rate conversion does not change that graph requirement.

Default master resolves the default sink's FL/FR ports at session start. Cue
never uses a default fallback. Port selections use runtime IDs and serials;
after removal or identity change, refresh and select the ports again. Cue loss
mutes cue while a valid master continues. Master loss stops output and allows
rediscovery/reselection. Applying outputs briefly interrupts playback and
restores the stopped transport state. Stop recording before applying outputs.

Recording captures only the post-master bus as stereo 48 kHz float WAV,
preserving values above ±1. The target is created exclusively and existing
files are never overwritten. A bounded queue feeds a background disk writer;
stopping detaches after the callback's last use, drains and finalizes before
showing Complete or Failed. Overflow or disk errors stop recording with a
visible error while playback can continue. Source replacement keeps the same
filter, output clock and writer. Only the target fades and swaps; peer audio,
including held preview, and recording continue through the shared callbacks.
Physical continuity remains unverified. Applying output settings still rebuilds
the session and requires recording to stop. Closing joins pending
open/finalization work after audio stops.

The live adapter uses 240-frame (5 ms) ramps for gains, headphone selection
and transport transitions, including seek/jog, CUE, EOF and data loss/recovery.
Pre-fader deck meters include those transport transitions. The ramps do not
implement a limiter or normalize the audio. The user accepted the current
speaker test without reported clicks or playback issues.

The three-band EQ uses standard [biquad equalizer formulas](https://www.w3.org/TR/audio-eq-cookbook/).
Its response has not been matched to the photographed mixer. Shelf slope is 1;
the mid-band Q is 1/sqrt(2). Neutral settings preserve the original PCM. Meter
and recording headroom remains above +/-1; EQ does not add a limiter.

The [EQ verification record](benchmarks/data/m3-channel-eq-2026-10-22/verification.json)
contains the accepted combined native mixing trial, 53 passing test groups,
213 UI checks and private recording/cue checks. Measured line coverage is
88.58% for C++ and 86.47% for Python; QML and the native stem adapter remain
unmeasured. These checks do not qualify laptop live-stem latency or the
required sustained actual-player run.

Estimated BPM and manual reference overrides are available through the deck LCD;
see [BPM controls](M3.md#bpm-controls-and-window-chrome). Key and beatgrid remain
unavailable. The overview uses at most 512
buckets of `max(abs(left), abs(right))` from decoded PCM, normalized to the
source peak for display only. Its playhead shows the source position, using a nominal cursor during tempo processing.
Analysis runs in the existing decode job, outside the GUI and audio callbacks.
Keep preparation and output errors near the affected deck. Recording state
comes from the writer lifecycle; do not invent analysis progress or AI ownership. Use approved
CONFLUX assets and theme tokens, without reproducing the hardware's logo.

Retain keyboard focus order through path, LOAD, CUE, PLAY/PAUSE, POSITION, JOG, TEMPO, MASTER TEMPO, TEMPO RESET and
CHANNEL LEVEL for each deck, then mixer HI/MID/LOW, CUE, MASTER LEVEL, CUE LEVEL and
CROSSFADER. Audio
settings expose keyboard-operable port selectors and recording controls. Enter loads the path; focused
buttons accept Space/Enter; focused sliders accept arrow keys. Pointer and
touch use the same actions. Keep controls large enough to operate at the
laptop's logical resolution, with visible focus and accessible names that
include the deck. Disable transport until that deck is loaded and output is
ready. A source handoff gates only the target's controls until its new name,
duration, overview and source-generation snapshot agree; peer and global
controls remain available. Disable loading throughout preparation, priming and
off-GUI retirement of the old reader. Explain inactive groups
with visible text or focus-accessible help, rather than hover or color alone.

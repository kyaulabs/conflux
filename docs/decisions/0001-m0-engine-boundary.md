# M0 engine reuse decision

Provisional, measured 16 October 2026 on the primary laptop. Keep a small CONFLUX
audio core with the same processing entry point for live playback and offline
rendering. Reuse individual decoder, resampler and time-stretch libraries as
those operations enter the implementation. This decision adds no production
dependency beyond the initial Qt/PipeWire harness.

The bounded Mixxx inspection used main commit
[`0fffd3d0d07b755c34c1730f7b87afadd8f2be19`](https://github.com/mixxxdj/mixxx/commit/0fffd3d0d07b755c34c1730f7b87afadd8f2be19),
dated 11 October 2026. CMake identifies version 2.7.0; its license identifies
2.7-alpha. The current entry point is `EngineMixer`. The older `EngineMaster`
path is absent in this revision.

Mixxx already has useful stem processing. With `__STEM__`, `EngineDeck` reads a
multichannel buffer, applies per-stem gain/mute and effects, then mixes to
stereo. Its deck API exposes stem handles. These are useful implementation
references, though they are internal interfaces.
[Pinned stem implementation](https://github.com/mixxxdj/mixxx/blob/0fffd3d0d07b755c34c1730f7b87afadd8f2be19/src/engine/channels/enginedeck.cpp#L121-L221).

`EngineMixer::process` and its output buffers can be driven by a caller.
Upstream signal-path tests do this, so processing without a performance window
is plausible. However, the existing test setup also creates `EffectsManager`,
`Deck`, `PlayerInfo`, settings and global controls; its base test creates
`MixxxApplication`. The build's `mixxx-lib` is the application library, containing
analysis, library, sound I/O and UI code. Its required Qt modules include
Widgets, PrintSupport, Qml and Sql even when its optional QML interface is off.
PortAudio remains required alongside optional PipeWire support.
[Engine interface](https://github.com/mixxxdj/mixxx/blob/0fffd3d0d07b755c34c1730f7b87afadd8f2be19/src/engine/enginemixer.h#L10-L107),
[signal-path setup](https://github.com/mixxxdj/mixxx/blob/0fffd3d0d07b755c34c1730f7b87afadd8f2be19/src/test/signalpathtest.h#L80-L160),
[test application](https://github.com/mixxxdj/mixxx/blob/0fffd3d0d07b755c34c1730f7b87afadd8f2be19/src/test/mixxxtest.cpp#L27-L77),
[build definition](https://github.com/mixxxdj/mixxx/blob/0fffd3d0d07b755c34c1730f7b87afadd8f2be19/CMakeLists.txt#L1198).

The extraction experiment recursively copied quoted project headers reached
from `EngineMixer`, `EngineDeck` and `EngineBuffer`. It found 71 headers,
306,656 bytes, including controls, MIDI messages, settings, recording and sound
management. The only unresolved quoted include was bundled `pa_ringbuffer.h`.
The experiment limits extraction to 120 files and 2 MiB; it does not clone or
build the application.

| C++20 syntax probe, `__STEM__` enabled | Mixer result | Buffer result |
| --- | --- | --- |
| Installed Qt Core headers | Missing `gsl/pointers` | Missing `gsl/pointers` |
| Qt Core plus unmodified temporary Microsoft GSL v4.2.0 headers | Missing `QDomNode` through settings | Missing `QJSValue` through MIDI/control headers |
| Qt Core, Xml, Qml plus the same GSL headers | Missing `QKeySequence` through settings | Missing `QKeySequence` through settings |

All six syntax probes returned 1. GSL was pinned to
[`2828399820ef4928cc89b65605dca5dc68efca6e`](https://github.com/microsoft/GSL/tree/2828399820ef4928cc89b65605dca5dc68efca6e).
The missing packages are fixable. These failures show that the attempted small
boundary was not isolated; they do not prove that extraction is impossible.
No linked Mixxx adapter, live/offline equivalence test or CONFLUX ownership
adapter was completed in this experiment.

CONFLUX still needs one authoritative scheduler that validates manual deck
ownership, shared controls, human EQ/FX and arrangement revisions before
commands reach DSP. Mixxx's generic control setters do not express that whole
contract. Adding the contract while extracting this application graph is more
work than the current harness requires. Revisit an extraction only if measured
manual-playback requirements make its existing transport and synchronization
worth that work.
[Control interface](https://github.com/mixxxdj/mixxx/blob/0fffd3d0d07b755c34c1730f7b87afadd8f2be19/src/control/controlobject.h#L96-L169).

| Installed component | Version | Provisional use and license |
| --- | --- | --- |
| libsndfile | 1.2.2-4 | Initial file decode/recording candidate; LGPL-2.1-or-later. Installed format enumeration includes WAV, AIFF, FLAC, Ogg and MPEG audio. Real corpus coverage remains to be tested. |
| libsamplerate | 0.2.2-3 | Sample-rate conversion candidate; BSD-2-Clause. |
| Rubber Band | 4.0.0-2 | Tempo/pitch and formant-preserving processing candidate; GPL-2.0-or-later. Measure R2/R3 with musical material before selecting settings. |
| SoundTouch | 2.4.1-1 | Available comparison candidate; package declares LGPL-2.1-only. No integration or quality result claimed. |
| FFmpeg | 2:9.0.2-2 | Reserve for corpus formats/container needs that libsndfile does not cover. This installed build declares GPL-3.0-only and enables GPL components; FFmpeg licensing depends on build options. |

Package versions/licenses above came from `pacman -Qi`; API versions came from
`pkg-config`. Upstream references:
[libsndfile](https://libsndfile.github.io/libsndfile/),
[libsamplerate license](https://libsndfile.github.io/libsamplerate/license.html),
[Rubber Band license](https://breakfastquay.com/rubberband/license.html),
[SoundTouch license](https://www.surina.net/soundtouch/license.html),
[FFmpeg build licensing](https://ffmpeg.org/legal.html).
Mixxx itself declares
[GPL-2.0-or-later](https://github.com/mixxxdj/mixxx/blob/0fffd3d0d07b755c34c1730f7b87afadd8f2be19/LICENSE).
Preserve upstream notices and the CONFLUX AGPLv3 license. Selecting a dependency
also requires recording the actual distributed build and its enabled modules.

The standalone API probe compiled with C++20, `-O2`, `-Wall`, `-Wextra` and
`-Wpedantic` without diagnostics. A generated 10-second, 44.1 kHz stereo float
WAV round-tripped exactly. One subsequent run produced:

| Operation | Result |
| --- | --- |
| libsamplerate `SRC_SINC_FASTEST`, 44.1 to 48 kHz | 441,000 input frames consumed; 480,001 output frames; 161.356 ms |
| Rubber Band R2, 48 kHz, time ratio 1.06, pitch ratio 1 | 543.634 ms; preferred start pad/start delay 1,024 frames each |
| Rubber Band R3, same rates/ratios | 1,766.168 ms; preferred start pad/start delay 2,048 frames each |
| Complete probe process | 2.517 s wall time; 22,796 KiB peak RSS |

Rubber Band used real-time mode, no worker threading, channels together and
formant preservation, with 512-frame input blocks. Times include construction,
priming and draining. Output counts are deliberately reported before delay
trimming. This is an API smoke probe on a synthetic tone. The probe does not
launch playback, inference or UI workloads; other machine activity was not
controlled. It measures neither callback deadlines nor musical quality. The
production path must account for variable output availability,
start padding/delay, scheduled ratio changes and final length, following the
[upstream integration guidance](https://breakfastquay.com/rubberband/integration.html).

Reproduce from the repository root with installed development packages. All
downloaded upstream files and generated audio stay in a new temporary directory:

```sh
python tools/experiments/inspect_mixxx.py --output /tmp/conflux-mixxx-inspection
probe_dir=$(mktemp -d /tmp/conflux-reuse.XXXXXX)
c++ -O2 -std=c++20 -Wall -Wextra -Wpedantic \
  tools/experiments/reuse_probe.cpp -o "$probe_dir/reuse-probe" \
  $(pkg-config --cflags --libs sndfile samplerate rubberband)
python - "$probe_dir" <<'PY'
from pathlib import Path
import resource
import subprocess
import sys
import time
root = Path(sys.argv[1])
start = time.monotonic()
subprocess.run([str(root / "reuse-probe"), str(root / "probe.wav")], check=True)
print("wall_s", time.monotonic() - start)
print("peak_rss_kib", resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
PY
```

The inspection command writes `summary.json` and full compiler commands/logs.
A successful inspection command means the experiment completed; check each
`exit_code` for the extraction result. The next engine slice should decode a
local fixture, process it through the shared live/offline core and verify timing
and output before adding another DSP dependency.

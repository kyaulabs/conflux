# Current fixed-tempo player hour, 7 November 2026 UTC

The primary laptop qualified the fixed-mix hour on signed player revision
`00eebb558f7f99801100ff38ddcb620095e4c0cb`. It delivered all 561 stem hops
below the strict 4.095-second target. Apollo met the same timing bound in
private virtual audio and failed steady playback continuity.

| Measurement | Laptop, physical audio and visible UI | Apollo, private virtual audio |
| --- | ---: | ---: |
| Measured duration | 3,600.013 s | 3,600.008 s |
| Completed stem deliveries | 561 | 579 |
| Nearest-rank p99 | **2.744 s** | **1.048 s** |
| Maximum delivery time | 2.946 s | 1.064 s |
| Steady starvation at 48 kHz | 0 frames | 1,024 frames |
| Control-related starvation | 508.741 s | 431.851 s |
| Replays | 139 | 143 |
| Callback deadline misses | 0 | 0 |
| Recording overflow | 0 frames | 0 frames |

Both hosts used the same two-deck recipe with a fixed 106 BPM target, Master
Tempo, Best-quality Vulkan stems, repeated seeks and replays, and recording.
The laptop stayed in Balanced with a 15 W MMIO long-term limit and an approved
temporary 15 W short-term cap. Its 1,750 temperature samples peaked at 83°C. All 342,813
visible-window observations were healthy. These observations cover the tested
window and workload. Shorter temperature spikes and physical scanout timing
are outside their resolution.

The read-only i915 observer captured at least one sample inside each of the 561
worker hop intervals. The 695 interior samples had a median reported actual
frequency of 1,000 MHz. They measure intermittent frequency during whole
worker intervals, with no continuous GPU clock or effective package power
measurement. The earlier multi-second slowdown did not recur in this hour.

Apollo's two steady gaps affected only Deck A, adding 256 and 768 starved
frames in replay epochs 32 and 88. All eight delivered hops in those epochs
completed in under 0.9 seconds and raw packet ranges stayed contiguous. At
each sampled endpoint of the two gap brackets, the recorded raw frontier was
more than 300,000 frames beyond the sampled source cursor. The counter samples
were about ten seconds apart. They cannot identify which callbacks starved or
establish raw availability at those callbacks.

Both recordings finalized with equal accepted and written frame counts. The
laptop's short-term cap returned to 64 W and OneDrive was active after cleanup.
Both owners retired and Apollo's dedicated test container was absent. Temporary
recordings and cache copies were removed after retaining the evidence.

The [machine-readable summary](data/current-fixed-tempo-hour-2026-11-07/summary.json)
contains every measured service duration, the retained archive hashes, and the
sampled Apollo gap brackets. This qualifies the laptop's current fixed-mix
hour only. Control recovery remains lengthy, and these runs did not exercise
hard cancellation or induced fallback. Automatic musical analysis and the
remaining M2/M3 acceptance gates stay open before `0.1.0`.

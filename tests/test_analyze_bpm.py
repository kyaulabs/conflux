#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stdlib contracts only: no learned inference, third-party imports or audio."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("conflux_analyze_bpm", Path(__file__).resolve().parents[1] / "tools/timing/analyze_bpm.py")
bpm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bpm)


class AnalyzeBpmTest(unittest.TestCase):
    def test_pcm_geometry_and_exact_stream_extent(self):
        raw = struct.pack("<ffff", .25, -.5, .75, -1)
        self.assertEqual(bpm.read_pcm(io.BytesIO(raw), 2), raw)
        for invalid in (raw[:-1], raw + b"\0", b"", raw + raw):
            with self.subTest(size=len(invalid)), self.assertRaises(ValueError):
                bpm.read_pcm(io.BytesIO(invalid), 2)
        for frames in (True, 0, -1, 1.5, "1", bpm.MAX_FRAMES + 1):
            with self.subTest(frames=frames), self.assertRaises(ValueError):
                bpm.validate_frames(frames)
        self.assertEqual(bpm.validate_frames(bpm.MAX_FRAMES) * 8, 268435456)

    def test_nonfinite_pcm_in_either_channel_is_rejected(self):
        for value in (float("nan"), float("inf"), -float("inf")):
            for pair in ((value, .25), (.25, value)):
                with self.subTest(pair=pair), self.assertRaises(ValueError):
                    bpm.validate_pcm(struct.pack("<ff", *pair), 1)

    def test_nonfinite_pcm_at_end_of_long_input_is_rejected(self):
        frames = 600_000
        raw = bytearray(frames * 8)
        bpm.validate_pcm(raw, frames)
        struct.pack_into("<I", raw, len(raw) - 4, 0x7f800001)  # Signaling NaN in the final channel.
        with self.assertRaisesRegex(ValueError, "nonfinite input PCM"):
            bpm.validate_pcm(raw, frames)

    def test_short_reads_are_accumulated_without_accepting_truncation(self):
        class ShortReads(io.BytesIO):
            def read(self, count=-1):
                return super().read(min(count, 3))
        raw = struct.pack("<ffffff", 0, 1, 2, 3, 4, 5)
        self.assertEqual(bpm.read_pcm(ShortReads(raw), 3), raw)
        with self.assertRaises(ValueError):
            bpm.read_pcm(ShortReads(raw[:-1]), 3)

    def test_valid_input_too_short_for_centered_mel_is_unknown(self):
        with (patch.object(bpm, "verified_sources", return_value={}),
              patch("importlib.metadata.version", side_effect=bpm.DEPENDENCIES.__getitem__),
              patch.object(bpm, "load_upstream", side_effect=AssertionError("must not infer"))):
            for frames in (1, 1115):
                with self.subTest(frames=frames):
                    raw = struct.pack("<ff", .25, -.5) * frames
                    output = bpm.run("/unused", "/unused", frames, io.BytesIO(raw))
                    [header] = [json.loads(line) for line in output.splitlines()]
                    self.assertEqual(header["frames"], frames)
                    self.assertEqual(header["pcm_sha256"], hashlib.sha256(raw).hexdigest())
                    self.assertIsNone(header["bpm"])
                    self.assertEqual(header["beat_count"], 0)
            for invalid in (b"", struct.pack("<ff", float("nan"), 0)):
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    bpm.run("/unused", "/unused", 1, io.BytesIO(invalid))

    def test_digital_silence_is_unknown_without_inference(self):
        with (patch.object(bpm, "verified_sources", return_value={}),
              patch("importlib.metadata.version", side_effect=bpm.DEPENDENCIES.__getitem__),
              patch.object(bpm, "load_upstream", side_effect=AssertionError("must not infer"))):
            frames = 70_000
            raw = struct.pack("<ff", -0., 0.) * frames
            output = bpm.run("/unused", "/unused", frames, io.BytesIO(raw))
            [header] = [json.loads(line) for line in output.splitlines()]
            self.assertIsNone(header["bpm"])
            self.assertEqual(header["beat_count"], 0)
            self.assertEqual(header["pcm_sha256"], hashlib.sha256(raw).hexdigest())

    def test_activity_edges_preserve_channel_and_source_frame(self):
        import numpy as np

        stereo = np.zeros((140_000, 2), dtype="<f4")
        stereo[:, 0] = -0.
        # The left and right channels start and end at different offsets.
        # Even a quiet nonzero sample belongs to the source.
        stereo[65_545, 1] = 1e-12
        stereo[71_999, 0] = -0.01
        self.assertEqual(bpm.first_nonzero_frame(stereo), 65_545)
        self.assertEqual(len(stereo) - 1 - bpm.first_nonzero_frame(stereo[::-1]), 71_999)

    def test_long_stereo_break_suppresses_only_interior_events(self):
        import numpy as np

        left_end = 65_531
        minimum = bpm.MIN_INTERIOR_SILENCE
        margin = bpm.SILENCE_MARGIN_FRAMES
        frames = left_end + minimum + 70_000
        stereo = np.zeros((frames, 2), dtype="<f4")
        stereo[:left_end, 0] = .01
        stereo[left_end + minimum:, 1] = 1e-12
        begin, end = left_end + margin, left_end + minimum - margin
        gaps = bpm.interior_silence_cores(stereo, 0, frames - 1)
        self.assertEqual(gaps, [(begin, end)])
        times = [frame / bpm.RATE for frame in (begin - 1, begin, end - 1, end)]
        keep = bpm.filter_silent_events(times, frames, 0, frames - 1, gaps, (begin,))
        self.assertEqual(keep, [times[0], times[3]])
        self.assertEqual(bpm.summarize_beats(keep, frames, (begin,)), (None, 2))
        self.assertEqual(bpm.summarize_beats([0., .5, times[3]], frames, (begin,)), (120., 3))
        with self.assertRaisesRegex(ValueError, "distinct increasing"):
            bpm.filter_silent_events([times[1], times[1]], frames, 0, frames - 1, gaps, (begin,))

        # Even one quiet nonzero sample breaks exact digital silence; the
        # revised gap is now one frame shorter than the supported cutoff.
        stereo[left_end, 1] = 1e-12
        self.assertEqual(bpm.interior_silence_cores(stereo, 0, frames - 1), [])

    def test_dither_between_active_passages_leaves_quiet_music_supported(self):
        import numpy as np

        begin, end = bpm.RATE, 5 * bpm.RATE + 131
        stereo = np.zeros((end + bpm.RATE, 2), dtype="<f4")
        stereo[:begin, 0] = .03
        stereo[end:, 1] = -.03
        gap = stereo[begin:end]
        floor = 2 ** -15
        rng = np.random.default_rng(4619)
        gap[:] = floor * (rng.random(gap.shape) - rng.random(gap.shape))
        margin = bpm.SILENCE_MARGIN_FRAMES

        def cores():
            return bpm.interior_silence_cores(stereo, 0, len(stereo) - 1)

        self.assertEqual(cores(), [(begin + margin, end - margin)])

        # Quiet rhythmic attacks can have an uncorrelated waveform.
        stationary = gap.copy()
        pulse = np.arange(len(gap)) % (bpm.RATE // 2) < bpm.RATE // 20
        gap[~pulse, 1] *= .12
        self.assertEqual(cores(), [])
        gap[:] = stationary

        # Add a coherent, near-floor signal only in the last partial window
        # of the timing gap. The opposite channel continues to carry dither.
        count = 131
        tail = gap[len(gap) - margin - count:len(gap) - margin]
        tail[:, 1] = floor * .9 * (-1.) ** np.arange(count)
        tail[:, 0] = 0.
        self.assertEqual(cores(), [])

    def test_unknown_and_median_of_adjacent_bpms(self):
        self.assertEqual(bpm.summarize_beats([], 144000), (None, 0))
        self.assertEqual(bpm.summarize_beats([.5], 144000), (None, 1))
        self.assertEqual(bpm.summarize_beats([0, .5, 1, 1.5], 144000), (120.0, 4))
        # Median(120,240)=180, not 60 divided by the median time delta (160).
        self.assertEqual(bpm.summarize_beats([0, .5, .75], 144000), (180.0, 3))
        self.assertEqual(bpm.summarize_beats([0, 23990 / 48000], 144000), (120.1, 2))

    def test_summary_range_is_checked_before_rounding(self):
        self.assertEqual(bpm.summarize_beats([0, 3], 192000), (20.0, 2))
        self.assertEqual(bpm.summarize_beats([0, .15], 192000), (400.0, 2))
        self.assertEqual(bpm.summarize_beats([0, 3.01], 192000), (None, 2))
        self.assertEqual(bpm.summarize_beats([0, 7199 / 48000], 192000), (None, 2))

    def test_summary_excludes_unsupported_intervals_only(self):
        frames = 240000
        digest = hashlib.sha256(b"fixture").hexdigest()
        cases = (
            ([0, .12, 4.12], None),
            ([0, .5, 4.5], 120.0),
            ([0, .12, .62], 120.0),
        )
        for seconds, expected in cases:
            with self.subTest(seconds=seconds):
                estimate, count = bpm.summarize_beats(seconds, frames)
                self.assertEqual((estimate, count), (expected, len(seconds)))
                wire = bpm.encode_result(frames, digest, estimate, seconds, [seconds[-1]])
                header, *events = [json.loads(line) for line in wire.splitlines()]
                self.assertEqual(header["bpm"], expected)
                self.assertEqual(events, [
                    {"frame": round(time * 48000), "downbeat": index == len(seconds) - 1, "unmeasured_before": False}
                    for index, time in enumerate(seconds)
                ])

    def test_invalid_events_are_errors_not_filtered_beats(self):
        invalid = ([float("nan")], [float("inf")], [True], ["0.5"], [-.1],
                   [1], [.999999], [0, 0], [.5, .25], [0, 1 / 96000], [10**1000])
        for stream in invalid:
            with self.subTest(stream=stream), self.assertRaises(ValueError):
                bpm.summarize_beats(stream, 48000)
        # .999999 rounds to EOF and is rejected even though raw seconds <1.
        self.assertEqual(bpm.source_beat_frames([0, .99998], 48000), [0, 47999])

    def test_centered_eof_mask_keeps_source_indices(self):
        exact = bpm.source_mask(48000, [22050, 22050], 51)
        self.assertEqual(exact, (True,) * 50 + (False,))
        # At 1s+one source frame, model position 1s is actually inside source.
        for resampled in (22050, 22051):
            self.assertEqual(bpm.source_mask(48001, [resampled, resampled], 51), (True,) * 51)
        for lengths, count in (([22050, 22051], 51), ([22049, 22049], 51),
                               ([22050, 22050], 50), ([True, True], 1), ([22050], 51)):
            with self.subTest(lengths=lengths, count=count), self.assertRaises(ValueError):
                bpm.source_mask(48000, lengths, count)
        # Product geometry has no private 48-second fixture ceiling.
        frames = 120 * 48000
        self.assertEqual(len(bpm.source_mask(frames, [120 * 22050] * 2, 6001)), 6001)

    def test_schema3_keeps_summary_and_exact_source_frame_events(self):
        digest = hashlib.sha256(b"fixture").hexdigest()
        seconds = [0, .5, .75, 1.25]
        estimate, count = bpm.summarize_beats(seconds, 96000)
        raw = bpm.encode_result(96000, digest, estimate, seconds, [0, .75])
        self.assertTrue(raw.endswith(b"\n"))
        self.assertLessEqual(len(raw), bpm.MAX_OUTPUT_BYTES)
        lines = raw.splitlines()
        header, *events = [json.loads(line) for line in lines]
        self.assertEqual(header, {"schema": 3, "recipe": "beat-this-final0-linear-mel-v1",
                         "frames": 96000, "pcm_sha256": digest, "bpm": 120.0, "beat_count": count})
        self.assertEqual(set(header), {"schema", "recipe", "frames", "pcm_sha256", "bpm", "beat_count"})
        self.assertEqual(len(events), header["beat_count"])
        self.assertEqual(events, [{"frame": 0, "downbeat": True, "unmeasured_before": False},
                                 {"frame": 24000, "downbeat": False, "unmeasured_before": False},
                                 {"frame": 36000, "downbeat": True, "unmeasured_before": False},
                                 {"frame": 60000, "downbeat": False, "unmeasured_before": False}])
        self.assertTrue(all(type(event["frame"]) is int and type(event["downbeat"]) is bool for event in events))
        self.assertTrue(all(len(line) + 1 <= 64 for line in lines[1:]))

    def test_confirmed_short_silence_withholds_intervals_and_keeps_beats(self):
        import numpy as np

        rate = bpm.RATE
        stereo = np.zeros((10 * rate, 2), dtype="<f4")
        stereo[:3 * rate, 0] = .1
        stereo[11 * rate // 2:, 0] = .1
        beats = [i / 2 for i in range(19)]
        core = [(3 * rate + bpm.SILENCE_MARGIN_FRAMES,
                 11 * rate // 2 - bpm.SILENCE_MARGIN_FRAMES)]
        self.assertEqual(bpm.interior_silence_cores(stereo, 0, len(stereo) - 1), [])
        self.assertEqual(bpm.interior_silence_cores(stereo, 0, len(stereo) - 1,
                                                    bpm.MIN_LOCAL_SILENCE), core)
        raw = bpm.encode_result(len(stereo), "a" * 64, 120., beats, [0, 4, 8], core)
        header, *events = map(json.loads, raw.splitlines())
        self.assertEqual(header["bpm"], 120.)
        self.assertEqual([row["frame"] for row in events], [int(beat * rate) for beat in beats])
        self.assertEqual([row["frame"] for row in events if row["downbeat"]], [0, 4*rate, 8*rate])
        self.assertEqual([row["frame"] for row in events if row["unmeasured_before"]],
                         [i*rate//2 for i in range(7, 12)])
        # The same geometry is not a missing interval at 24 BPM.
        slow = [i*120000 for i in range(10)]
        self.assertFalse(any(bpm.unmeasured_intervals(slow, [(390000,508000)])))

    def test_unknown_summary_preserves_zero_one_and_outlier_timing(self):
        digest = hashlib.sha256(b"fixture").hexdigest()
        for seconds, downbeats in (([], []), ([.5], [.5]), ([0, 3.01], [0])):
            with self.subTest(seconds=seconds):
                estimate, count = bpm.summarize_beats(seconds, 192000)
                self.assertIsNone(estimate)
                header, *events = [json.loads(line) for line in
                    bpm.encode_result(192000, digest, estimate, seconds, downbeats).splitlines()]
                self.assertIsNone(header["bpm"])
                self.assertEqual(header["beat_count"], count)
                self.assertEqual([event["frame"] for event in events], bpm.source_beat_frames(seconds, 192000))
        # Local intervals remain actual adjacent intervals, not a constant grid
        # reconstructed from the whole-track median.
        seconds = [0, .5, .75]
        estimate, _count = bpm.summarize_beats(seconds, 96000)
        header, *events = [json.loads(line) for line in
            bpm.encode_result(96000, digest, estimate, seconds, []).splitlines()]
        self.assertEqual(header["bpm"], 180.0)
        frames = [event["frame"] for event in events]
        self.assertEqual([60 * bpm.RATE / (b - a) for a, b in zip(frames, frames[1:])], [120.0, 240.0])

    def test_output_rejects_invalid_summary_events_and_downbeat_membership(self):
        digest = hashlib.sha256(b"fixture").hexdigest()
        for value, seconds in ((float("nan"), [0, .5]), (float("inf"), [0, .5]),
                               (True, [0, .5]), (19.9, [0, .5]), (400.1, [0, .5]),
                               (120.11, [0, .5]), (120, [.5])):
            with self.subTest(value=value), self.assertRaises(ValueError):
                bpm.encode_result(48000, digest, value, seconds, [])
        for stream in ([float("nan")], [float("inf")], [True], ["0.5"], [-.1],
                       [1], [.999999], [0, 0], [.5, .25], [0, 1 / 96000], [10**1000]):
            with self.subTest(beats=stream), self.assertRaises(ValueError):
                bpm.encode_result(48000, digest, None, stream, [])
            with self.subTest(downbeats=stream), self.assertRaises(ValueError):
                bpm.encode_result(48000, digest, 120.0, [0, .5], stream)
        # A nonmember that rounds to an existing beat frame is still not an
        # original downbeat timestamp; preserve the exact subset requirement.
        for downbeats in ([.25], [1 / 480000], [.5, 0], [.5, .5]):
            with self.subTest(downbeats=downbeats), self.assertRaises(ValueError):
                bpm.encode_result(48000, digest, 120.0, [0, .5], downbeats)
        with self.assertRaises(ValueError):
            bpm.encode_result(48000, "x" * 64, None, [], [])

    def test_maximum_event_geometry_and_whole_output_cap(self):
        digest = hashlib.sha256(b"fixture").hexdigest()
        self.assertEqual(bpm.MAX_EVENTS, bpm.MAX_FRAMES * bpm.FPS // bpm.RATE + 1)
        seconds = [index / bpm.FPS for index in range(bpm.MAX_EVENTS)]
        raw = bpm.encode_result(bpm.MAX_FRAMES, digest, None, seconds, [seconds[0], seconds[-1]])
        header, *events = [json.loads(line) for line in raw.splitlines()]
        self.assertEqual(header["beat_count"], 34953)
        self.assertEqual(len(events), 34953)
        self.assertEqual(events[-1], {"frame": (bpm.MAX_EVENTS - 1) * 960, "downbeat": True, "unmeasured_before": False})
        self.assertLess(events[-1]["frame"], bpm.MAX_FRAMES)
        self.assertLessEqual(len(raw), 4 * 1024**2)
        self.assertTrue(all(len(line) + 1 <= 64 for line in raw.splitlines()[1:]))
        with self.assertRaises(ValueError):
            bpm.encode_result(bpm.MAX_FRAMES, digest, None, seconds + [seconds[-1] + .02], [])
        with self.assertRaises(ValueError):
            bpm.encode_result(48000, digest, None, seconds[:52], [])
        with patch.object(bpm, "MAX_EVENTS", 3), self.assertRaises(ValueError):
            bpm.encode_result(96000, digest, None, [0, .5, 1, 1.5], [])
        small = bpm.encode_result(48000, digest, 120.0, [0, .5], [0])
        with patch.object(bpm, "MAX_OUTPUT_BYTES", len(small) - 1), self.assertRaises(ValueError):
            bpm.encode_result(48000, digest, 120.0, [0, .5], [0])
        empty = bpm.encode_result(48000, digest, None, [], [])
        with patch.object(bpm, "MAX_OUTPUT_BYTES", len(empty) - 1), self.assertRaises(ValueError):
            bpm.encode_result(48000, digest, None, [], [])

    def test_pinned_regular_file_content_and_symlink_rejection(self):
        with tempfile.TemporaryDirectory(prefix="conflux-bpm-test-") as temporary:
            path = Path(temporary) / "small-source.py"
            raw = b"# verified fixture, never executed\n"
            path.write_bytes(raw)
            digest = hashlib.sha256(raw).hexdigest()
            stream, before = bpm.open_verified(path, len(raw), digest)
            with stream:
                self.assertEqual(stream.read(), raw)
                self.assertEqual(before.st_size, len(raw))
            path.write_bytes(raw.replace(b"verified", b"modified"))
            with self.assertRaises(ValueError):
                bpm.open_verified(path, len(raw), digest)
            alias = path.with_suffix(".alias")
            alias.symlink_to(path)
            with self.assertRaises(OSError):
                bpm.open_verified(alias, path.stat().st_size, hashlib.sha256(path.read_bytes()).hexdigest())
            with self.assertRaises(ValueError):
                bpm.open_verified(Path("relative-source.py"), len(raw), digest)

    def test_main_invalid_geometry_cannot_reach_inference(self):
        with patch.object(bpm, "run", side_effect=AssertionError("must not infer")) as infer:
            with patch("sys.argv", ["analyze_bpm.py", "--source-dir", "/unused", "--model", "/unused", "--frames", "0"]):
                with self.assertRaises(ValueError):
                    bpm.main()
            infer.assert_not_called()


if __name__ == "__main__":
    unittest.main()

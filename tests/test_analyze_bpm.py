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

    def test_short_reads_are_accumulated_without_accepting_truncation(self):
        class ShortReads(io.BytesIO):
            def read(self, count=-1):
                return super().read(min(count, 3))
        raw = struct.pack("<ffffff", 0, 1, 2, 3, 4, 5)
        self.assertEqual(bpm.read_pcm(ShortReads(raw), 3), raw)
        with self.assertRaises(ValueError):
            bpm.read_pcm(ShortReads(raw[:-1]), 3)

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

    def test_schema2_keeps_summary_and_exact_source_frame_events(self):
        digest = hashlib.sha256(b"fixture").hexdigest()
        seconds = [0, .5, .75, 1.25]
        estimate, count = bpm.summarize_beats(seconds, 96000)
        raw = bpm.encode_result(96000, digest, estimate, seconds, [0, .75])
        self.assertTrue(raw.endswith(b"\n"))
        self.assertLessEqual(len(raw), bpm.MAX_OUTPUT_BYTES)
        lines = raw.splitlines()
        header, *events = [json.loads(line) for line in lines]
        self.assertEqual(header, {"schema": 2, "recipe": "beat-this-final0-linear-mel-v1",
                         "frames": 96000, "pcm_sha256": digest, "bpm": 120.0, "beat_count": count})
        self.assertEqual(set(header), {"schema", "recipe", "frames", "pcm_sha256", "bpm", "beat_count"})
        self.assertEqual(len(events), header["beat_count"])
        self.assertEqual(events, [{"frame": 0, "downbeat": True},
                                 {"frame": 24000, "downbeat": False},
                                 {"frame": 36000, "downbeat": True},
                                 {"frame": 60000, "downbeat": False}])
        self.assertTrue(all(type(event["frame"]) is int and type(event["downbeat"]) is bool for event in events))
        self.assertTrue(all(len(line) + 1 <= 64 for line in lines[1:]))

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
        self.assertEqual(events[-1], {"frame": (bpm.MAX_EVENTS - 1) * 960, "downbeat": True})
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

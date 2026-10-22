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

    def test_output_is_small_versioned_summary_with_null_unknown(self):
        digest = hashlib.sha256(b"fixture").hexdigest()
        raw = bpm.encode_result(bpm.MAX_FRAMES, digest, 120.1, 42)
        self.assertLessEqual(len(raw), 4096)
        self.assertTrue(raw.endswith(b"\n"))
        self.assertEqual(json.loads(raw), {"schema": 1, "recipe": "beat-this-final0-linear-mel-v1",
                         "frames": 33554432, "pcm_sha256": digest, "bpm": 120.1, "beat_count": 42})
        self.assertIsNone(json.loads(bpm.encode_result(48000, digest, None, 1))["bpm"])
        for value, count in ((float("nan"), 2), (float("inf"), 2), (True, 2),
                             (19.9, 2), (400.1, 2), (120.11, 2), (120, 1), (None, True), (None, 52)):
            with self.subTest(value=value, count=count), self.assertRaises(ValueError):
                bpm.encode_result(48000, digest, value, count)
        with self.assertRaises(ValueError):
            bpm.encode_result(48000, "x" * 64, None, 0)

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

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Exercise soak source rotation and ownership without native/audio/UI load."""

from collections import deque
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import wave

import numpy as np

from stem_worker_test import fixture

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "benchmarks"))
import stem_soak
from tools.stems.worker import HOP, RATE


class Clock:
    def __init__(self):
        self.now = 100.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class ScriptedWorker:
    """Emit one bounded source traversal per load/seek, in worker event order."""

    backend_pid = None

    def __init__(self, soak, clock, corrupt_repeat=False):
        self.soak, self.clock = soak, clock
        self.corrupt_repeat = corrupt_repeat
        self.generation = 0
        self.loads, self.seeks = [], []
        self.events = deque()
        self.completed = True
        self.closed = False

    def load(self, path):
        if not self.completed:
            raise AssertionError("source rotated before complete")
        self.loads.append(path)
        self.source = next(source for source in self.soak.sources if source["path"] == path)
        return self.schedule()

    def seek(self, frame):
        if not self.completed or frame != 0:
            raise AssertionError("unexpected synthetic restart")
        self.seeks.append(frame)
        return self.schedule()

    def schedule(self):
        self.generation += 1
        self.completed = False
        if self.generation == 1:
            self.events.append({"event": "runtime_ready"})
        self.events.append({"event": "source_ready", "source_frames": self.source["frames"],
                            "mean": 0.0, "std": 1.0, "reused": bool(self.seeks)})
        for origin in range(0, self.source["frames"], HOP):
            self.events.append({"event": "started", "request_id": origin // HOP + 1})
            self.events.append({"event": "chunk", "window_start_frame": origin})
        self.events.append({"event": "complete"})
        return self.generation

    def poll(self, timeout):
        self.clock.sleep(timeout)
        event = self.events.popleft()
        event.update(generation=self.generation, source_id=self.source["sha256"],
                     timestamp=self.clock.monotonic())
        if event["event"] == "chunk":
            origin = event["window_start_frame"]
            frames = min(HOP, self.source["frames"] - origin)
            value = int(self.source["sha256"][:8], 16) / 2 ** 32
            if (self.corrupt_repeat and self.loads.count(self.source["path"]) > 1
                    and origin == HOP):
                value += 0.5
            event.update(frame_start=origin, frames=frames, request_id=origin // HOP + 1,
                         samples=np.full((4, frames, 2), value, dtype=np.float32),
                         ready_queue_depth=1, ready_at=self.clock.monotonic(),
                         request_ms=1.0, source_read_ms=0.0, overlap_ms=0.0,
                         hop_service_ms=1.0, control_latency_ms=1.0, queue_delay_ms=0.0,
                         metrics={}, model_id="fake-model")
        elif event["event"] == "complete":
            self.completed = True
        return event

    def is_current(self, event):
        return event["generation"] == self.generation

    def statistics(self):
        return {"ready_count": 0, "ready_high_water": 1, "backend_start_count": 1,
                "telemetry_dropped": 0, "inflight": None, "closed": self.closed}

    def close(self, timeout):
        self.closed = True


class NoLoad:
    MODES = stem_soak.Load.MODES

    def __init__(self, *_):
        pass

    def start(self):
        pass

    def check(self):
        pass

    def stop(self):
        return {"children": {}, "errors": []}


class NoSampler:
    def __init__(self, *_args, **_kwargs):
        pass

    def __enter__(self):
        return self

    def stop(self):
        return {}


class SoakTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="conflux-soak-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.runtime = self.root / "runtime"
        self.model = self.root / "model"
        self.runtime.write_bytes(b"fake runtime")
        self.model.write_bytes(b"fake model")
        self.sources = [self.root / "a.wav", self.root / "b.wav"]
        for phase, path in enumerate(self.sources):
            fixture(path, 2 * HOP + 138, phase=phase * 123)

    def run_soak(self, sources, *, corrupt_repeat=False):
        output = self.root / "output"
        output.mkdir()
        args = SimpleNamespace(runtime=self.runtime, model=self.model, source_wav=sources,
                               renderer="software", seconds=6, load_mode="none", threads=4,
                               request_timeout=1, control_interval=100, sample_interval=1,
                               load_seconds=70)
        soak = stem_soak.Soak(args, output)
        clock = Clock()
        worker = ScriptedWorker(soak, clock, corrupt_repeat)
        with (patch.object(stem_soak, "time", clock),
              patch.object(stem_soak, "conditions", return_value={}),
              patch.object(stem_soak, "Load", NoLoad),
              patch.object(stem_soak, "ProcessSampler", NoSampler),
              patch.object(stem_soak.subprocess, "run", return_value=SimpleNamespace(stdout="test-head\n")),
              patch.object(stem_soak, "StemWorker", return_value=worker) as constructor):
            result = soak.execute()
        return soak, worker, result, constructor

    def test_corpus_rotation_and_source_specific_seek_parity(self):
        before = [path.read_bytes() for path in self.sources]
        soak, worker, result, constructor = self.run_soak(self.sources)
        self.assertEqual(result["status"], "ok", result["failures"])
        constructor.assert_called_once()
        self.assertEqual(worker.loads[:3], list(map(str, [*self.sources, self.sources[0]])))
        self.assertEqual(worker.seeks, [])
        self.assertGreaterEqual(result["same_grid_hash_checks"], 3)
        self.assertEqual([path.read_bytes() for path in self.sources], before)
        self.assertTrue(worker.closed)
        self.assertEqual(len(result["reference_hashes_by_source"]), 2)
        self.assertEqual({row["source_id"] for row in soak.hops},
                         {source["sha256"] for source in soak.sources})
        self.assertEqual(result["source_completions"][soak.sources[0]["sha256"]]["full"], 2)
        # A trimmed seek into B must use B's continuous reference, despite A
        # having identical frame numbers and different sample values.
        soak.select_source(1)
        reference = soak.references[HOP]
        row = next(row for row in soak.hops if row["source_id"] == soak.fixture["sha256"]
                   and row["window_start_frame"] == HOP)
        event = {**row, "frame_start": HOP + 333, "frames": HOP - 333,
                 "samples": reference[:, 333:].copy(), "ready_queue_depth": 1}
        soak.expected, soak.first_after_seek, soak.log = HOP + 333, True, io.StringIO()
        soak.accept(event)
        self.assertTrue(soak.hops[-1]["parity"]["exact"])
        self.assertEqual(soak.hops[-1]["kind"], "seek_first")
        self.assertEqual(soak.trimmed_parity_checks, 1)

    def test_same_source_repeat_corruption_is_rejected(self):
        _, worker, result, _ = self.run_soak(self.sources, corrupt_repeat=True)
        self.assertEqual(result["status"], "error")
        self.assertTrue(any("repeated same-grid chunk hash changed" in failure["error"]
                            for failure in result["failures"]))
        self.assertTrue(worker.closed)

    def test_synthetic_default_restarts_and_cleans_only_owned_fixture(self):
        _, worker, result, _ = self.run_soak(None)
        self.assertEqual(result["status"], "ok", result["failures"])
        self.assertEqual(result["source_mode"], "synthetic")
        self.assertEqual(result["fixture"]["frames"], stem_soak.FIXTURE_FRAMES)
        self.assertEqual(len(worker.loads), 1)
        self.assertGreaterEqual(len(worker.seeks), 1)
        self.assertFalse((self.root / "output" / "source.wav").exists())
        self.assertTrue(all(path.exists() for path in self.sources))

    def test_invalid_sources_refused_before_worker_start_and_preserved(self):
        for invalid in ("mono", "short", "truncated"):
            with self.subTest(invalid=invalid):
                path = self.root / f"{invalid}.wav"
                if invalid == "mono":
                    with wave.open(str(path), "wb") as stream:
                        stream.setparams((1, 2, RATE, 1, "NONE", "not compressed"))
                        stream.writeframes(b"\0\0")
                elif invalid == "short":
                    fixture(path, 2 * HOP + 137)
                else:
                    path.write_bytes(self.sources[0].read_bytes()[:-4])
                before = path.read_bytes()
                _, _, result, constructor = self.run_soak([path])
                self.assertEqual(result["status"], "error")
                constructor.assert_not_called()
                self.assertEqual(path.read_bytes(), before)
                # Each execute owns a fresh evidence directory.
                for child in (self.root / "output").iterdir():
                    child.unlink()
                (self.root / "output").rmdir()


if __name__ == "__main__":
    unittest.main()

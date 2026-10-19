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


class DelayedWakeClock(Clock):
    def sleep(self, seconds):
        crossed = self.now < 106.8 <= self.now + seconds
        self.now += seconds + (0.4 if crossed else 0)


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


class PacedWorker:
    """A fake producer with real two-chunk backpressure and inference clocks."""

    backend_pid = None

    def __init__(self, soak, clock, *, slow_second_source=False):
        self.soak, self.clock = soak, clock
        self.slow_second_source = slow_second_source
        self.generation = 0
        self.request = 0
        self.events, self.chunks = deque(), deque()
        self.active = None
        self.high_water = 0
        self.closed = False
        self.loads, self.seeks = [], []
        self.cancelled = 0
        self.discarded = 0
        self.blocked_observations = 0

    def load(self, path):
        self.loads.append((path, self.clock.now))
        self.source = next(source for source in self.soak.sources if source["path"] == path)
        return self.schedule(0)

    def seek(self, frame):
        self.seeks.append(frame)
        return self.schedule(frame)

    def schedule(self, frame):
        self.generation += 1
        self.chunks.clear()
        self.next_frame = frame
        if self.generation == 1:
            self.events.append({"event": "runtime_ready", "timestamp": self.clock.now,
                                "generation": self.generation})
        self.events.append({"event": "source_ready", "source_frames": self.source["frames"],
            "mean": 0.0, "std": 1.0, "reused": False, "timestamp": self.clock.now,
            "source_id": self.source["sha256"], "generation": self.generation})
        self.begin(self.clock.now)
        return self.generation

    def begin(self, timestamp):
        if self.next_frame >= self.source["frames"] or len(self.chunks) == 2:
            self.active = None
            return
        self.request += 1
        duration = 1.0
        if (self.slow_second_source and len(self.loads) == 2
                and self.next_frame == 0):
            duration = 8.0
        self.active = {"request_id": self.request, "generation": self.generation,
                       "source_id": self.source["sha256"], "timestamp": timestamp,
                       "window_start_frame": self.next_frame // HOP * HOP,
                       "preroll": False, "frame_start": self.next_frame,
                       "ready": timestamp + duration, "duration": duration}
        self.events.append({"event": "started", **self.active})

    def sync(self):
        while self.active is not None and self.active["ready"] <= self.clock.now:
            active, self.active = self.active, None
            if active["generation"] != self.generation:
                self.discarded += 1
                self.events.append({"event": "discarded", **active,
                                    "timestamp": active["ready"]})
                break
            frames = min(HOP - active["frame_start"] % HOP,
                         self.source["frames"] - active["frame_start"])
            self.chunks.append({"event": "chunk", **active, "timestamp": active["ready"],
                "ready_at": active["ready"], "frames": frames, "ready_queue_depth": len(self.chunks) + 1,
                "request_ms": active["duration"] * 1000, "source_read_ms": 0.0, "overlap_ms": 0.0,
                "hop_service_ms": active["duration"] * 1000, "control_latency_ms": active["duration"] * 1000,
                "metrics": {}, "model_id": "fake-model"})
            self.high_water = max(self.high_water, len(self.chunks))
            self.next_frame += frames
            if self.next_frame == self.source["frames"]:
                self.events.append({"event": "complete", "timestamp": active["ready"] + 1e-9,
                    "source_id": self.source["sha256"], "generation": self.generation})
            else:
                self.begin(active["ready"])

    def poll(self, timeout):
        self.sync()
        if not self.events and not self.chunks:
            wait = timeout if self.active is None else min(timeout, self.active["ready"] - self.clock.now)
            self.clock.sleep(max(0, wait))
            self.sync()
        if self.events and (not self.chunks or self.events[0]["timestamp"] <= self.chunks[0]["timestamp"]):
            return self.events.popleft()
        if not self.chunks:
            return None
        # Reading ordinary PCM before its deadline would free the producer's
        # queue and accidentally turn the test back into saturated delivery.
        if (self.soak.playback_next_deadline is not None and self.soak.controls == []
                and self.soak.held is not None):
            if self.clock.now < self.soak.playback_next_deadline:
                raise AssertionError("future chunk polled before playback deadline")
        chunk = self.chunks.popleft()
        chunk["queue_delay_ms"] = (self.clock.now - chunk["ready_at"]) * 1000
        value = int(chunk["source_id"][:8], 16) / 2 ** 32
        chunk["samples"] = np.full((4, chunk["frames"], 2), value, dtype=np.float32)
        if self.active is None:
            self.begin(self.clock.now)
        return chunk

    def statistics(self):
        self.sync()
        self.blocked_observations += len(self.chunks) == 2 and self.active is None
        return {"ready_count": len(self.chunks), "ready_high_water": self.high_water,
                "backend_start_count": 1, "telemetry_dropped": 0,
                "inflight": dict(self.active) if self.active else None,
                "closed": self.closed, "cancel_count": self.cancelled,
                "discarded_count": self.discarded}

    def is_current(self, event):
        return event["generation"] == self.generation

    def cancel(self):
        self.sync()
        self.cancelled += 1
        self.generation += 1
        self.chunks.clear()
        return self.generation

    def close(self, timeout):
        self.closed = True


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

    def run_paced(self, *, seconds=55, control_interval=1000, slow_second_source=False,
                  clock=None):
        output = self.root / "output"
        output.mkdir()
        args = SimpleNamespace(runtime=self.runtime, model=self.model, source_wav=self.sources,
            renderer="software", seconds=seconds, load_mode="none", threads=4,
            request_timeout=1, control_interval=control_interval, sample_interval=1,
            load_seconds=120, delivery_mode="playback-paced")
        soak = stem_soak.Soak(args, output)
        clock = clock or Clock()
        worker = PacedWorker(soak, clock, slow_second_source=slow_second_source)
        with (patch.object(stem_soak, "time", clock),
              patch.object(stem_soak, "conditions", return_value={}),
              patch.object(stem_soak, "Load", NoLoad),
              patch.object(stem_soak, "ProcessSampler", NoSampler),
              patch.object(stem_soak.subprocess, "run", return_value=SimpleNamespace(stdout="test-head\n")),
              patch.object(stem_soak, "StemWorker", return_value=worker)):
            result = soak.execute()
        return soak, worker, result

    def test_paced_clock_backpressure_and_partial_source_boundary(self):
        soak, worker, result = self.run_paced()
        self.assertEqual(result["status"], "ok", result["failures"])
        self.assertEqual(worker.high_water, 2)
        self.assertGreater(worker.blocked_observations, 0)
        self.assertGreaterEqual(len(worker.loads), 3)
        rows = [row["playback"] for row in soak.hops]
        self.assertEqual(len(result["playback"]["epochs"]), 1)
        for previous, current in zip(rows, rows[1:]):
            self.assertAlmostEqual(current["deadline_monotonic"],
                previous["deadline_monotonic"] + previous["frames"] / RATE)
        # The 138-frame tail leaves almost no next-source preparation budget.
        # It causes a real ordinary deadline miss, not a new priming period.
        transition = next(index for index in range(1, len(rows))
                          if rows[index]["source_id"] != rows[index - 1]["source_id"])
        before, after = rows[transition - 1:transition + 1]
        self.assertEqual(before["frames"], 138)
        self.assertEqual(after["kind"], "ordinary")
        self.assertGreater(after["ready_late_ms"], 900)
        self.assertAlmostEqual(worker.loads[1][1], before["consumed_monotonic"])
        self.assertLess(worker.loads[1][1], after["deadline_monotonic"])
        self.assertFalse(result["acceptance"]["ordinary_playback_ready_deadlines_met"])
        self.assertGreater(result["playback"]["queue_residence_percentiles_ms"]["99"], 1000)
        self.assertTrue(all(row["hop_service_ms"] == 1000 for row in soak.hops))

    def test_late_source_output_does_not_reset_clock_and_final_demand_is_retained(self):
        soak, _, result = self.run_paced(seconds=15, slow_second_source=True)
        self.assertEqual(result["status"], "ok", result["failures"])
        rows = [row["playback"] for row in soak.hops]
        self.assertEqual(len(rows), 3)
        self.assertEqual(len(result["playback"]["epochs"]), 1)
        pending = result["playback"]["unfulfilled_final_demand"]
        self.assertIsNotNone(pending)
        self.assertAlmostEqual(pending["deadline_monotonic"],
            rows[-1]["deadline_monotonic"] + 138 / RATE)
        self.assertGreater(pending["observed_wait_seconds"], 2)
        self.assertFalse(result["acceptance"]["ordinary_playback_ready_deadlines_met"])

    def test_completed_late_source_preserves_following_deadline(self):
        soak, _, result = self.run_paced(seconds=35, slow_second_source=True)
        self.assertEqual(result["status"], "ok", result["failures"])
        rows = [row["playback"] for row in soak.hops]
        late_index = next(index for index, row in enumerate(rows)
                          if row.get("ready_late_ms", 0) > 7000)
        late, following = rows[late_index:late_index + 2]
        self.assertAlmostEqual(following["deadline_monotonic"],
                               late["deadline_monotonic"] + late["frames"] / RATE)
        self.assertLess(following["deadline_monotonic"], late["consumed_monotonic"])
        self.assertEqual(len(result["playback"]["epochs"]), 1)

    def test_consumer_scheduling_delay_is_not_a_producer_deadline_miss(self):
        soak, _, result = self.run_paced(seconds=10, clock=DelayedWakeClock())
        self.assertEqual(result["status"], "ok", result["failures"])
        self.assertEqual(len(soak.playback_demands), 1)
        demand = soak.playback_demands[0]
        self.assertGreater(demand["consumer_wake_late_ms"], 390)
        self.assertGreater(demand["delivery_late_ms"], 390)
        self.assertEqual(demand["ready_late_ms"], 0)
        self.assertTrue(result["acceptance"]["ordinary_playback_ready_deadlines_met"])

    def test_delayed_wake_past_stop_retains_due_unrequested_clock_obligation(self):
        soak, _, result = self.run_paced(seconds=6.9, clock=DelayedWakeClock())
        self.assertEqual(result["status"], "ok", result["failures"])
        self.assertEqual(soak.playback_demands, [])
        pending = result["playback"]["unfulfilled_final_demand"]
        self.assertTrue(pending["request_not_issued"])
        self.assertIsNone(pending["consumer_wake_monotonic"])
        self.assertIsNone(pending["ready_count_at_demand"])
        self.assertIsNone(pending["observed_wait_seconds"])
        self.assertLess(pending["deadline_monotonic"], pending["workload_planned_stop_monotonic"])
        self.assertGreater(pending["terminal_observed_monotonic"], pending["workload_planned_stop_monotonic"])
        self.assertEqual(result["playback"]["unfulfilled_due_demands"], 1)
        self.assertFalse(result["acceptance"]["ordinary_playback_ready_deadlines_met"])

    def test_delayed_wake_does_not_create_obligation_after_planned_stop(self):
        soak, _, result = self.run_paced(seconds=6.8, clock=DelayedWakeClock())
        self.assertEqual(result["status"], "ok", result["failures"])
        self.assertGreater(soak.finished, soak.playback_next_deadline)
        self.assertGreater(soak.playback_next_deadline, soak.deadline)
        self.assertIsNone(result["playback"]["unfulfilled_final_demand"])
        self.assertEqual(result["playback"]["unfulfilled_due_demands"], 0)
        self.assertTrue(result["acceptance"]["ordinary_playback_ready_deadlines_met"])

    def test_explicit_cancel_creates_labelled_recovery_epoch(self):
        soak, worker, result = self.run_paced(seconds=95, control_interval=30)
        self.assertEqual(result["status"], "ok", result["failures"])
        self.assertGreaterEqual(len(soak.controls), 1)
        self.assertEqual(worker.cancelled, len(soak.controls))
        self.assertEqual(worker.discarded, len(soak.controls))
        self.assertTrue(all(control["complete"] and control["held_chunk_rejected"]
                            and control["parity"]["exact"] for control in soak.controls))
        epochs = result["playback"]["epochs"]
        self.assertEqual(len(epochs), len(soak.controls) + 1)
        self.assertEqual(epochs[0]["reason"], "initial_priming")
        self.assertTrue(all(epoch["reason"] == "seek_recovery" for epoch in epochs[1:]))
        self.assertTrue(all(row["kind"] == "ordinary" for row in soak.playback_demands))
        self.assertTrue(all(control["started_to_cancel_ms"] >= 150 - 1e-6
                            for control in soak.controls))

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

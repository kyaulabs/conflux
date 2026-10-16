#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Behavior tests for the M0 supervisor, requiring neither model nor GPU."""

import json
from pathlib import Path
import stat
import sys
import tempfile
import threading
import time
import unittest
import wave

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.stems.worker import (CHANNELS, HOP, OVERLAP, RATE, STEMS, WINDOW,
                                NativeRuntime, StemWorker)


def fixture(path, frames, phase=0):
    index = np.arange(frames, dtype=np.int64)
    audio = np.column_stack((((index + phase) % 30001) - 15000,
                             ((index * 7 + phase) % 21001) - 9000)).astype("<i2")
    with wave.open(str(path), "wb") as stream:
        stream.setparams((CHANNELS, 2, RATE, frames, "NONE", "not compressed"))
        stream.writeframes(audio.tobytes())
    return audio.astype(np.float32) / 32768


def separated(samples):
    # Asymmetric stereo, source gains and position-dependent output expose
    # channel/source transposes and incorrect overlap weights or source grids.
    gains = np.arange(1, 5, dtype=np.float32)[:, None, None] / 4
    ramp = np.arange(WINDOW, dtype=np.float32)[None, :, None] / WINDOW
    return (samples[None] * gains + ramp * np.array([0.1, -0.2], dtype=np.float32)).astype(np.float32)


class FakeRuntime:
    def __init__(self, blocked=False, fail=False):
        self.blocked = blocked
        self.fail = fail
        self.condition = threading.Condition()
        self.calls = 0
        self.starts = 0
        self.gate = threading.Semaphore(0)
        self.aborted = False
        self.closed = False

    def start(self):
        self.starts += 1
        return {"event": "ready", "sample_rate": RATE, "channels": CHANNELS,
                "segment_frames": WINDOW, "stems": list(STEMS), "pid": 1}

    def infer(self, samples, request_id):
        with self.condition:
            self.calls += 1
            self.condition.notify_all()
        if self.blocked and not self.gate.acquire(timeout=4):
            raise TimeoutError("test did not release fake inference")
        if self.fail or self.aborted:
            raise RuntimeError("injected runtime fault")
        return separated(samples), {"elapsed_ms": 1.0, "id": request_id}

    def wait_call(self, count):
        with self.condition:
            return self.condition.wait_for(lambda: self.calls >= count, timeout=3)

    def release(self, count=1):
        for _ in range(count):
            self.gate.release()

    def abort(self):
        self.aborted = True
        self.release()

    def close(self):
        with self.condition:
            self.closed = True
            self.condition.notify_all()


class WorkerTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="conflux-stem-test-")
        self.root = Path(self.directory.name)
        self.model = self.root / "model.gguf"
        self.model.write_bytes(b"fake model identity")
        self.path = self.root / "source.wav"
        self.audio = fixture(self.path, 2 * HOP + 137)
        self.workers = []

    def tearDown(self):
        for worker in self.workers:
            worker.close(timeout=0.05)
        self.directory.cleanup()

    def worker(self, fake=None, **kwargs):
        fake = fake or FakeRuntime()
        worker = StemWorker("fake-runtime", self.model,
                            _backend_factory=lambda *_: fake, **kwargs)
        self.workers.append(worker)
        return worker, fake

    def event(self, worker, name, generation=None, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            event = worker.poll(0.1)
            if event is not None:
                if event["event"] == "error" and name != "error":
                    self.fail(f"worker error: {event}")
                if event["event"] == name and (generation is None or event["generation"] == generation):
                    return event
        self.fail(f"no {name} event for generation {generation}")

    def collect(self, worker, generation):
        chunks = []
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            item = worker.poll(0.1)
            if item is None or item["generation"] != generation:
                continue
            self.assertNotEqual(item["event"], "error", item)
            if item["event"] == "chunk":
                chunks.append(item)
            if item["event"] == "complete":
                return chunks
        self.fail("worker did not complete")

    def test_coverage_channels_normalization_overlap_and_tail(self):
        worker, fake = self.worker()
        generation = worker.load(self.path)
        source = self.event(worker, "source_ready", generation)
        chunks = self.collect(worker, generation)
        self.assertEqual([item["frame_start"] for item in chunks], [0, HOP, 2 * HOP])
        self.assertEqual([item["frames"] for item in chunks], [HOP, HOP, 137])
        self.assertEqual(fake.starts, 1)
        self.assertEqual(fake.calls, 3)
        audio = np.concatenate([item["samples"] for item in chunks], axis=1)
        self.assertEqual(audio.shape, (4, len(self.audio), 2))
        mean, std = np.float32(source["mean"]), np.float32(source["std"])
        mono = self.audio.astype(np.float64).mean(axis=1)
        self.assertAlmostEqual(float(mean), float(mono.mean()), places=7)
        self.assertAlmostEqual(float(std), float(mono.std(ddof=1)), places=7)
        # Independent scalar oracle at both sides of each overlap/window edge.
        positions = [0, 1, HOP - 1, HOP, HOP + 1, HOP + OVERLAP - 1,
                     HOP + OVERLAP, 2 * HOP - 1, 2 * HOP, len(self.audio) - 1]
        for position in positions:
            expected = np.zeros((4, 2), dtype=np.float64)
            denominator = 0.0
            for start in range(0, position + 1, HOP):
                local = position - start
                if local >= WINDOW:
                    continue
                weight = min(local + 1, WINDOW - local) / (WINDOW // 2)
                normalized = (self.audio[position] - mean) / std
                prediction = normalized[None] * np.arange(1, 5)[:, None] / 4
                prediction += local / WINDOW * np.array([0.1, -0.2])
                expected += prediction * weight
                denominator += weight
            expected = expected / denominator * std + mean
            np.testing.assert_allclose(audio[:, position], expected, atol=2e-7)
        self.assertTrue(all(worker.is_current(chunk) for chunk in chunks))

    def test_seek_matches_continuous_grid_and_reuses_model_and_statistics(self):
        worker, fake = self.worker()
        baseline = self.collect(worker, worker.load(self.path))
        continuous = np.concatenate([item["samples"] for item in baseline], axis=1)
        for frame in [HOP + 17, 21, 2 * HOP + 7, len(self.audio)]:
            generation = worker.seek(frame)
            source = self.event(worker, "source_ready", generation)
            self.assertTrue(source["reused"])
            chunks = self.collect(worker, generation)
            if frame == len(self.audio):
                self.assertEqual(chunks, [])
            else:
                self.assertEqual(chunks[0]["frame_start"], frame)
                output = np.concatenate([item["samples"] for item in chunks], axis=1)
                np.testing.assert_array_equal(output, continuous[:, frame:])
        self.assertEqual(fake.starts, 1)

    def test_single_frame_exact_hop_and_short_tail_coverage(self):
        for count in [1, HOP, WINDOW + 1]:
            with self.subTest(frames=count):
                path = self.root / f"short-{count}.wav"
                fixture(path, count)
                worker, _ = self.worker()
                chunks = self.collect(worker, worker.load(path))
                self.assertEqual(sum(chunk["frames"] for chunk in chunks), count)
                self.assertEqual(chunks[0]["frame_start"], 0)
                self.assertEqual(chunks[-1]["frame_start"] + chunks[-1]["frames"], count)
                self.assertTrue(all(np.isfinite(chunk["samples"]).all() for chunk in chunks))
                worker.close()

    def test_active_seek_burst_latest_wins_and_discarded_result(self):
        worker, fake = self.worker(FakeRuntime(blocked=True))
        old = worker.load(self.path)
        self.assertTrue(fake.wait_call(1))
        started = time.monotonic()
        worker.seek(HOP + 7)
        worker.seek(3)
        latest = worker.seek(2 * HOP + 11)
        self.assertLess(time.monotonic() - started, 0.1)
        fake.release()
        discarded = self.event(worker, "discarded", old)
        self.assertEqual(discarded["request_id"], 1)
        self.assertTrue(fake.wait_call(2))
        fake.release(2)
        chunk = self.event(worker, "chunk", latest)
        self.assertEqual(chunk["frame_start"], 2 * HOP + 11)
        self.assertEqual(fake.calls, 3)  # stale call + exact-grid prior/current.
        self.assertEqual(fake.starts, 1)

    def test_cancel_running_idle_and_pending_then_seek(self):
        worker, fake = self.worker(FakeRuntime(blocked=True))
        idle = worker.cancel()
        self.assertEqual(fake.calls, 0)
        old = worker.load(self.path)
        self.assertTrue(fake.wait_call(1))
        started = time.monotonic()
        cancelled = worker.cancel()
        self.assertGreater(cancelled, idle)
        self.assertLess(time.monotonic() - started, 0.1)
        fake.release()
        self.event(worker, "discarded", old)
        self.assertIsNone(worker.poll(0.05))
        latest = worker.seek(7)
        self.assertTrue(fake.wait_call(2))
        fake.release()
        chunk = self.event(worker, "chunk", latest)
        self.assertEqual(chunk["frame_start"], 7)
        worker.cancel()  # Also wakes a ready-queue or next-call wait.
        fake.release()
        self.assertFalse(worker.is_current(chunk))

    def test_cancel_pending_seek_does_not_launch_it(self):
        worker, fake = self.worker(FakeRuntime(blocked=True))
        old = worker.load(self.path)
        self.assertTrue(fake.wait_call(1))
        worker.seek(HOP + 9)
        cancelled = worker.cancel()
        fake.release()
        self.event(worker, "discarded", old)
        self.assertIsNone(worker.poll(0.05))
        self.assertEqual(fake.calls, 1)
        stats = worker.statistics()
        self.assertEqual(stats["generation"], cancelled)
        self.assertIsNone(stats["pending_latest_generation"])
        self.assertEqual(stats["discarded_count"], 1)

    def test_source_replacement_rejects_old_source_results(self):
        other = self.root / "other.wav"
        fixture(other, HOP + 19, phase=117)
        worker, fake = self.worker(FakeRuntime(blocked=True))
        old = worker.load(self.path)
        old_source = self.event(worker, "source_ready", old)
        self.assertTrue(fake.wait_call(1))
        latest = worker.load(other, start_frame=8)
        fake.release()
        self.event(worker, "discarded", old)
        source = self.event(worker, "source_ready", latest)
        self.assertNotEqual(source["source_id"], old_source["source_id"])
        self.assertTrue(fake.wait_call(2))
        fake.release()
        chunk = self.event(worker, "chunk", latest)
        self.assertEqual(chunk["source_id"], source["source_id"])
        self.assertEqual(chunk["frame_start"], 8)
        worker.cancel()
        fake.release()

    def test_ready_backpressure_and_close_wakes_producer(self):
        worker, fake = self.worker(max_ready=1)
        generation = worker.load(self.path)
        self.event(worker, "started", generation)
        # Wait on the producer's condition without consuming its queued chunk.
        with worker._condition:
            self.assertTrue(worker._condition.wait_for(lambda: len(worker._chunks) == 1, timeout=3))
        self.assertEqual(fake.calls, 1)
        chunk = self.event(worker, "chunk", generation)
        self.assertEqual(chunk["ready_queue_depth"], 1)
        with worker._condition:
            self.assertTrue(worker._condition.wait_for(lambda: len(worker._chunks) == 1, timeout=3))
        self.assertEqual(fake.calls, 2)
        worker.close(timeout=0.2)
        self.assertFalse(worker._thread.is_alive())
        self.assertTrue(fake.closed)

    def test_queued_and_already_polled_chunks_become_stale_on_cancel(self):
        worker, _ = self.worker()
        generation = worker.load(self.path)
        chunk = self.event(worker, "chunk", generation)
        self.assertTrue(worker.is_current(chunk))
        cancelled = worker.cancel()
        self.assertFalse(worker.is_current(chunk))
        self.event(worker, "control", cancelled)
        for _ in range(5):
            event = worker.poll(0.03)
            self.assertTrue(event is None or event["event"] != "chunk")

    def test_fault_is_reported_and_runtime_can_restart_on_next_control(self):
        failed = FakeRuntime(fail=True)
        working = FakeRuntime()
        choices = iter([failed, working])
        worker = StemWorker("fake-runtime", self.model, _backend_factory=lambda *_: next(choices))
        self.workers.append(worker)
        generation = worker.load(self.path)
        error = self.event(worker, "error", generation)
        self.assertIn("injected runtime fault", error["message"])
        with failed.condition:
            self.assertTrue(failed.condition.wait_for(lambda: failed.closed, timeout=3))
        chunks = self.collect(worker, worker.seek(0))
        self.assertEqual(len(chunks), 3)
        self.assertEqual(working.starts, 1)

    def test_close_aborts_stalled_inference(self):
        worker, fake = self.worker(FakeRuntime(blocked=True))
        worker.load(self.path)
        self.assertTrue(fake.wait_call(1))
        worker.close(timeout=0.01)
        self.assertTrue(fake.aborted)
        self.assertFalse(worker._thread.is_alive())

    def test_bad_source_fails_without_chunks(self):
        bad = self.root / "mono.wav"
        with wave.open(str(bad), "wb") as stream:
            stream.setparams((1, 2, RATE, 1, "NONE", "not compressed"))
            stream.writeframes(b"\0\0")
        worker, fake = self.worker()
        generation = worker.load(bad)
        self.assertIn("stereo", self.event(worker, "error", generation)["message"])
        self.assertEqual(fake.calls, 0)

    def test_source_mutation_invalidates_results_and_seek_reprepares(self):
        worker, _ = self.worker(max_ready=1)
        generation = worker.load(self.path)
        source = self.event(worker, "source_ready", generation)
        with worker._condition:
            self.assertTrue(worker._condition.wait_for(lambda: len(worker._chunks) == 1, timeout=3))
        fixture(self.path, len(self.audio), phase=717)
        held = self.event(worker, "chunk", generation)
        self.assertIn("changed", self.event(worker, "error", generation)["message"])
        self.assertFalse(worker.is_current(held))
        latest = worker.seek(0)
        updated = self.event(worker, "source_ready", latest)
        self.assertFalse(updated["reused"])
        self.assertNotEqual(source["source_id"], updated["source_id"])
        self.assertEqual(len(self.collect(worker, latest)), 3)

    def test_telemetry_and_ready_observations_are_bounded(self):
        worker, _ = self.worker()
        for _ in range(1000):
            worker.cancel()
        stats = worker.statistics()
        self.assertEqual(stats["ready_count"], 0)
        self.assertEqual(stats["telemetry_count"], 256)
        self.assertEqual(stats["telemetry_high_water"], 256)
        self.assertEqual(stats["telemetry_dropped"], 744)
        self.assertEqual(stats["cancel_count"], 1000)

    def test_native_protocol_rejects_faults_and_times_out(self):
        # Exercise the actual process transport without a native model/GPU.
        for mode in ["timeout", "invalid", "truncated", "shape", "eof"]:
            executable = self.root / f"runtime-{mode}"
            script = f'''#!{sys.executable}
import json, sys, time
print(json.dumps({{"event":"ready","sample_rate":44100,"channels":2,"segment_frames":343980,"stems":{list(STEMS)!r}}}), flush=True)
line = json.loads(sys.stdin.readline())
mode = {mode!r}
if mode == "timeout": time.sleep(5)
elif mode == "invalid": print("not json", flush=True)
elif mode == "eof": sys.exit(1)
else:
    open(line["output"], "wb").write(b"bad")
    print(json.dumps({{"event":"result","id":line["id"],"frames":1 if mode == "shape" else 343980,"stems":4}}), flush=True)
'''
            executable.write_text(script)
            executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
            native = NativeRuntime(executable, self.model, "cpu", 1, 0.2)
            try:
                native.start()
                with self.assertRaises((RuntimeError, TimeoutError, json.JSONDecodeError)):
                    native.infer(np.zeros((WINDOW, 2), dtype=np.float32), 1)
            finally:
                native.close()


if __name__ == "__main__":
    unittest.main()

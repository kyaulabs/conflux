#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Behavior tests for the M0 supervisor, requiring neither model nor GPU."""

import hashlib
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import wave
import warnings

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.stems.worker import (CHANNELS, HOP, OVERLAP, RATE, STEMS, WINDOW,
                                NativeRuntime, StemWorker)
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "benchmarks"))
from stem_worker import Load


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


class RecordingRuntime(FakeRuntime):
    def __init__(self):
        super().__init__()
        self.inputs = []

    def infer(self, samples, request_id):
        self.inputs.append(samples.copy())
        return super().infer(samples, request_id)


class NativeRuntimeConstructionTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="conflux-native-init-test-")
        self.root = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)

    def resources(self):
        # Keep strong references after the failed constructor, so GC/finalizers
        # cannot hide a missing explicit cleanup in NativeRuntime.__init__.
        real_directory = tempfile.TemporaryDirectory
        created = []
        def make_directory(*args, **kwargs):
            directory = real_directory(*args, dir=self.root, **kwargs)
            created.append(directory)
            self.addCleanup(directory.cleanup)
            return directory
        actions = []
        child = Mock()
        child.stdin, child.stdout = io.BytesIO(), io.BytesIO()
        self.addCleanup(child.stdin.close)
        self.addCleanup(child.stdout.close)
        child.poll.return_value = None
        child.kill.side_effect = lambda: actions.append("kill")
        def wait(*, timeout):
            actions.append("wait")
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, 2)
            return -9
        child.wait.side_effect = wait
        launch = Mock(return_value=child)
        return make_directory, created, child, actions, launch

    def assert_files_cleaned(self, created, launch):
        self.assertEqual(len(created), 1)
        self.assertFalse(Path(created[0].name).exists(), "failed startup retained its temporary directory")
        self.assertTrue(launch.call_args.kwargs["stderr"].closed, "failed startup retained its stderr file")

    def assert_child_cleaned(self, child, actions):
        child.kill.assert_called_once_with()
        child.wait.assert_called_once()
        self.assertEqual(actions, ["kill", "wait"], "startup cleanup must terminate and reap the owned child")
        self.assertTrue(child.stdin.closed, "failed startup retained child stdin")
        self.assertTrue(child.stdout.closed, "failed startup retained child stdout")

    def test_selector_creation_failure_reaps_spawned_child_and_closes_resources(self):
        factory, created, child, actions, launch = self.resources()
        original = OSError("injected selector allocation failure")
        with patch("tools.stems.worker.tempfile.TemporaryDirectory", side_effect=factory), \
             patch("tools.stems.worker.subprocess.Popen", launch), \
             patch("tools.stems.worker.selectors.DefaultSelector", side_effect=original) as selector:
            with self.assertRaises(OSError) as caught:
                NativeRuntime("unused-runtime", "unused-model", "cpu", 1, 2)
        self.assertIs(caught.exception, original)
        launch.assert_called_once()
        selector.assert_called_once_with()
        self.assert_child_cleaned(child, actions)
        self.assert_files_cleaned(created, launch)

    def test_selector_registration_failure_reaps_spawned_child_and_closes_selector(self):
        factory, created, child, actions, launch = self.resources()
        original = OSError("injected selector registration failure")
        selector = Mock()
        selector.register.side_effect = original
        with patch("tools.stems.worker.tempfile.TemporaryDirectory", side_effect=factory), \
             patch("tools.stems.worker.subprocess.Popen", launch), \
             patch("tools.stems.worker.selectors.DefaultSelector", return_value=selector):
            with self.assertRaises(OSError) as caught:
                NativeRuntime("unused-runtime", "unused-model", "cpu", 1, 2)
        self.assertIs(caught.exception, original)
        selector.register.assert_called_once()
        self.assertIs(selector.register.call_args.args[0], child.stdout)
        selector.close.assert_called_once_with()
        self.assert_child_cleaned(child, actions)
        self.assert_files_cleaned(created, launch)

    def test_popen_failure_closes_stderr_and_temporary_directory_without_selector(self):
        factory, created, child, actions, launch = self.resources()
        original = OSError("injected process creation failure")
        launch.side_effect = original
        with patch("tools.stems.worker.tempfile.TemporaryDirectory", side_effect=factory), \
             patch("tools.stems.worker.subprocess.Popen", launch), \
             patch("tools.stems.worker.selectors.DefaultSelector") as selector:
            with self.assertRaises(OSError) as caught:
                NativeRuntime("unused-runtime", "unused-model", "cpu", 1, 2)
        self.assertIs(caught.exception, original)
        launch.assert_called_once()
        selector.assert_not_called()
        child.kill.assert_not_called()
        child.wait.assert_not_called()
        self.assertEqual(actions, [])
        self.assert_files_cleaned(created, launch)

    def test_selector_cleanup_failure_preserves_startup_error_and_cleans_other_resources(self):
        factory, created, child, actions, launch = self.resources()
        original = OSError("injected selector registration failure")
        selector = Mock()
        selector.register.side_effect = original
        selector.close.side_effect = OSError("injected selector cleanup failure")
        with patch("tools.stems.worker.tempfile.TemporaryDirectory", side_effect=factory), \
             patch("tools.stems.worker.subprocess.Popen", launch), \
             patch("tools.stems.worker.selectors.DefaultSelector", return_value=selector):
            with self.assertRaises(OSError) as caught:
                NativeRuntime("unused-runtime", "unused-model", "cpu", 1, 2)
        self.assertIs(caught.exception, original)
        selector.register.assert_called_once()
        selector.close.assert_called_once_with()
        self.assert_child_cleaned(child, actions)
        self.assert_files_cleaned(created, launch)


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

    def test_f32_matches_pcm16_windows_reconstruction_and_tail(self):
        raw = self.root / "equivalent.f32"
        self.audio.astype("<f4").tofile(raw)
        fake = RecordingRuntime()
        worker, _ = self.worker(fake)
        legacy_generation = worker.load(self.path)
        legacy_source = self.event(worker, "source_ready", legacy_generation)
        legacy = self.collect(worker, legacy_generation)
        legacy_inputs = fake.inputs[:]
        fake.inputs.clear()
        generation = worker.load_f32(raw)
        source = self.event(worker, "source_ready", generation)
        chunks = self.collect(worker, generation)
        self.assertEqual(source["source_format"], "f32le")
        self.assertEqual(source["source_id"], hashlib.sha256(raw.read_bytes()).hexdigest())
        self.assertEqual(source["source_frames"], len(self.audio))
        self.assertEqual((source["mean"], source["std"]), (legacy_source["mean"], legacy_source["std"]))
        self.assertEqual([chunk["frame_start"] for chunk in chunks], [0, HOP, 2 * HOP])
        self.assertEqual([chunk["frames"] for chunk in chunks], [HOP, HOP, 137])
        self.assertEqual(len(fake.inputs), len(legacy_inputs))
        for actual, expected in zip(fake.inputs, legacy_inputs):
            np.testing.assert_array_equal(actual, expected)
        for actual, expected in zip(chunks, legacy):
            self.assertEqual(actual["source_format"], "f32le")
            np.testing.assert_array_equal(actual["samples"], expected["samples"])
        self.assertEqual(fake.starts, 1)

    def test_f32_preserves_headroom_and_single_frame_constant(self):
        values = (self.audio * np.float32(4) + np.float32(1.25)).astype("<f4")
        raw = self.root / "headroom.f32"
        values.tofile(raw)
        fake = RecordingRuntime()
        worker, _ = self.worker(fake)
        generation = worker.load_f32(raw)
        source = self.event(worker, "source_ready", generation)
        chunks = self.collect(worker, generation)
        mono = values.astype(np.float64).mean(axis=1)
        self.assertAlmostEqual(source["mean"], float(mono.mean()), delta=1e-6)
        self.assertAlmostEqual(source["std"], float(mono.std(ddof=1) + 1e-8), delta=1e-6)
        restored = fake.inputs[0] * np.float32(source["std"]) + np.float32(source["mean"])
        np.testing.assert_allclose(restored, values[:WINDOW], rtol=1e-6, atol=2e-6)
        self.assertGreater(float(restored.max()), 1)
        self.assertGreater(float(np.concatenate([chunk["samples"] for chunk in chunks], axis=1).max()), 1)
        raw = self.root / "constant.f32"
        np.full((1, CHANNELS), 3.25, dtype="<f4").tofile(raw)
        generation = worker.load_f32(raw)
        source = self.event(worker, "source_ready", generation)
        chunks = self.collect(worker, generation)
        self.assertEqual(source["mean"], 3.25)
        self.assertEqual(source["std"], float(np.float32(1e-8)))
        self.assertEqual(sum(chunk["frames"] for chunk in chunks), 1)
        np.testing.assert_array_equal(chunks[0]["samples"], np.full((4, 1, CHANNELS), 3.25, dtype=np.float32))
        self.assertTrue(all(np.isfinite(value).all() for value in fake.inputs))

    def test_f32_seeks_reuse_format_statistics_and_exact_grid(self):
        raw = self.root / "seeks.f32"
        self.audio.astype("<f4").tofile(raw)
        worker, fake = self.worker()
        baseline = self.collect(worker, worker.load_f32(raw))
        continuous = np.concatenate([item["samples"] for item in baseline], axis=1)
        for frame in [HOP + 17, 21, 2 * HOP + 7, len(self.audio)]:
            generation = worker.seek(frame)
            source = self.event(worker, "source_ready", generation)
            self.assertTrue(source["reused"])
            self.assertEqual(source["source_format"], "f32le")
            chunks = self.collect(worker, generation)
            if frame == len(self.audio):
                self.assertEqual(chunks, [])
            else:
                self.assertEqual(chunks[0]["frame_start"], frame)
                np.testing.assert_array_equal(np.concatenate([item["samples"] for item in chunks], axis=1),
                                              continuous[:, frame:])
        self.assertEqual(fake.starts, 1)

    def test_f32_cancel_discards_active_result_and_seek_preserves_format(self):
        raw = self.root / "cancel.f32"
        self.audio.astype("<f4").tofile(raw)
        worker, fake = self.worker(FakeRuntime(blocked=True))
        old = worker.load_f32(raw)
        self.assertTrue(fake.wait_call(1))
        cancelled = worker.cancel()
        self.assertGreater(cancelled, old)
        fake.release()
        self.event(worker, "discarded", old)
        latest = worker.seek(HOP + 7)
        source = self.event(worker, "source_ready", latest)
        self.assertTrue(source["reused"])
        self.assertEqual(source["source_format"], "f32le")
        self.assertTrue(fake.wait_call(2))
        fake.release(2)
        chunk = self.event(worker, "chunk", latest)
        self.assertEqual(chunk["frame_start"], HOP + 7)
        self.assertEqual(chunk["source_format"], "f32le")
        self.assertTrue(worker.is_current(chunk))
        worker.cancel()
        self.assertFalse(worker.is_current(chunk))
        fake.release()

    def test_f32_rejects_finite_backend_output_that_overflows_reconstruction(self):
        class HugeFiniteRuntime(FakeRuntime):
            def infer(self, samples, request_id):
                output, metrics = super().infer(samples, request_id)
                output.fill(np.finfo(np.float32).max)
                self.backend_output_finite = bool(np.isfinite(output).all())
                return output, metrics
        raw = self.root / "reconstruction-overflow.f32"
        (self.audio * np.float32(16)).astype("<f4").tofile(raw)
        worker, fake = self.worker(HugeFiniteRuntime())
        # Intentional finite arithmetic overflow must become an error, never a
        # queued chunk. Suppress only this fixture's expected NumPy warning.
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)
            generation = worker.load_f32(raw)
            source = self.event(worker, "source_ready", generation)
            self.assertGreater(source["std"], 1)
            error = self.event(worker, "error", generation)
        self.assertIn("reconstruction contains non-finite samples", error["message"])
        self.assertTrue(fake.backend_output_finite)
        self.assertEqual(fake.calls, 1)
        with worker._condition:
            self.assertEqual(len(worker._chunks), 0)

    def test_f32_rejects_bad_geometry_nonfinite_and_normalization_overflow(self):
        maximum = np.finfo(np.float32).max
        cases = [
            ("empty", b"", "complete stereo frames"),
            ("partial-frame", b"x" * 9, "complete stereo frames"),
            ("nan", np.array([[np.nan, 0]], dtype="<f4").tobytes(), "non-finite samples"),
            ("inf", np.array([[np.inf, 0]], dtype="<f4").tobytes(), "non-finite samples"),
            ("canonical-overflow", np.array([[maximum, maximum], [-maximum, -maximum]], dtype="<f4").tobytes(),
             "normalization"),
            ("window-overflow", np.array([[maximum, -maximum]], dtype="<f4").tobytes(), "normalization"),
        ]
        for name, data, reason in cases:
            with self.subTest(case=name):
                raw = self.root / f"invalid-{name}.f32"
                raw.write_bytes(data)
                worker, fake = self.worker()
                generation = worker.load_f32(raw)
                self.assertIn(reason, self.event(worker, "error", generation)["message"])
                self.assertEqual(fake.calls, 0)
                with worker._condition:
                    self.assertEqual(len(worker._chunks), 0)
                worker.close()

    def test_f32_rejects_nonregular_storage_without_inference(self):
        regular = self.root / "regular.f32"
        np.zeros((1, CHANNELS), dtype="<f4").tofile(regular)
        link = self.root / "linked.f32"
        link.symlink_to(regular)
        fifo = self.root / "fifo.f32"
        os.mkfifo(fifo)
        directory = self.root / "directory.f32"
        directory.mkdir()
        for path in [link, fifo, directory]:
            with self.subTest(path=path.name):
                worker, fake = self.worker()
                generation = worker.load_f32(path)
                self.event(worker, "error", generation)
                self.assertEqual(fake.calls, 0)
                worker.close()

    def test_f32_mutation_invalidates_results_and_seek_reprepares(self):
        raw = self.root / "mutable.f32"
        self.audio.astype("<f4").tofile(raw)
        worker, _ = self.worker(max_ready=1)
        generation = worker.load_f32(raw)
        source = self.event(worker, "source_ready", generation)
        with worker._condition:
            self.assertTrue(worker._condition.wait_for(lambda: len(worker._chunks) == 1, timeout=3))
        (self.audio + np.float32(.125)).astype("<f4").tofile(raw)
        held = self.event(worker, "chunk", generation)
        self.assertIn("changed", self.event(worker, "error", generation)["message"])
        self.assertFalse(worker.is_current(held))
        latest = worker.seek(0)
        updated = self.event(worker, "source_ready", latest)
        self.assertFalse(updated["reused"])
        self.assertEqual(updated["source_format"], "f32le")
        self.assertNotEqual(source["source_id"], updated["source_id"])
        self.assertEqual(sum(chunk["frames"] for chunk in self.collect(worker, latest)), len(self.audio))

    def test_same_path_formats_do_not_share_cached_geometry_or_statistics(self):
        # These unchanged bytes are both a three-frame PCM16 WAV and seven raw
        # float frames. A path/signature-only cache key would reuse wrong geometry.
        path = self.root / "same-path.wav"
        with wave.open(str(path), "wb") as stream:
            stream.setparams((CHANNELS, 2, RATE, 3, "NONE", "not compressed"))
            stream.writeframes(bytes(3 * CHANNELS * 2))
        worker, fake = self.worker()
        generation = worker.load(path)
        legacy = self.event(worker, "source_ready", generation)
        self.assertEqual(sum(chunk["frames"] for chunk in self.collect(worker, generation)), 3)
        generation = worker.load_f32(path)
        raw = self.event(worker, "source_ready", generation)
        self.assertFalse(raw["reused"])
        self.assertEqual(raw["source_id"], legacy["source_id"], "file identity should remain a byte digest")
        self.assertEqual(raw["source_format"], "f32le")
        self.assertEqual(raw["source_frames"], path.stat().st_size // 8)
        self.assertEqual(sum(chunk["frames"] for chunk in self.collect(worker, generation)), raw["source_frames"])
        generation = worker.load(path)
        repeated = self.event(worker, "source_ready", generation)
        self.assertFalse(repeated["reused"])
        self.assertEqual(repeated["source_format"], "wav_pcm16")
        self.assertEqual(sum(chunk["frames"] for chunk in self.collect(worker, generation)), 3)
        self.assertEqual(fake.starts, 1)

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
        observed = self.event(worker, "started", old)
        inflight = worker.statistics()["inflight"]
        self.assertEqual(inflight, {key: observed[key] for key in (
            "generation", "request_id", "window_start_frame", "source_id", "timestamp", "preroll")})
        # Observing an active request must not let the consumer change it.
        inflight["generation"] = -1
        self.assertEqual(worker.statistics()["inflight"]["generation"], old)
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
            native = NativeRuntime(executable, self.model, "cpu", 1, 2)
            try:
                native.start()
                native.timeout = 0.2
                with self.assertRaises((RuntimeError, TimeoutError, json.JSONDecodeError)):
                    native.infer(np.zeros((WINDOW, 2), dtype=np.float32), 1)
            finally:
                native.close()

    def test_native_rejects_oversized_lines_and_nonfinite_pcm_and_cleans_up(self):
        # Real pipes and files exercise protocol bounds and sample validation,
        # without loading a model. A late invalid sample follows a valid prefix.
        for mode in ("oversized", "nan", "inf"):
            with self.subTest(mode=mode):
                executable = self.root / f"runtime-{mode}"
                executable.write_text(f'''#!{sys.executable}
import json, struct, sys
print(json.dumps({{"event":"ready","sample_rate":44100,"channels":2,"segment_frames":343980,"stems":{list(STEMS)!r}}}), flush=True)
request = json.loads(sys.stdin.readline())
mode = {mode!r}
if mode == "oversized":
    print("x" * 65537, flush=True)
else:
    size = {len(STEMS) * WINDOW * CHANNELS * 4}
    with open(request["output"], "wb") as output:
        output.truncate(size)
        output.seek(size - 4)
        output.write(struct.pack("<f", float(mode)))
    print(json.dumps({{"event":"result","id":request["id"],"frames":343980,"stems":4}}), flush=True)
# Stay alive so NativeRuntime.close must terminate and reap the faulty child.
sys.stdin.readline()
''')
                executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
                native = NativeRuntime(executable, self.model, "cpu", 1, 2)
                runtime_directory = native.root
                try:
                    native.start()
                    reason = "protocol line exceeds limit" if mode == "oversized" else "non-finite samples"
                    with self.assertRaisesRegex(RuntimeError, reason):
                        native.infer(np.zeros((WINDOW, 2), dtype=np.float32), 1)
                finally:
                    native.close()
                self.assertIsNotNone(native.process.returncode)
                with self.assertRaises(ChildProcessError):
                    os.waitpid(native.process.pid, os.WNOHANG)
                self.assertTrue(native.process.stdin.closed)
                self.assertTrue(native.process.stdout.closed)
                self.assertTrue(native.stderr.closed)
                self.assertFalse(runtime_directory.exists())

    def test_passive_openmp_is_local_to_vulkan_child_and_can_be_disabled(self):
        executable = self.root / "runtime-environment"
        executable.write_text(f'''#!{sys.executable}
import json, os, sys
keys = ("OMP_WAIT_POLICY", "GOMP_SPINCOUNT")
print(json.dumps({{"event":"ready","sample_rate":44100,"channels":2,"segment_frames":343980,"stems":{list(STEMS)!r},"observed_environment":{{key:os.environ[key] for key in keys if key in os.environ}}}}), flush=True)
sys.stdin.readline()
''')
        executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
        inherited = {"OMP_WAIT_POLICY": "ACTIVE", "GOMP_SPINCOUNT": "123"}
        with patch.dict(os.environ, inherited):
            for backend, passive in [("vulkan", True), ("vulkan", False),
                                     ("cpu", True), ("cpu", False)]:
                with self.subTest(backend=backend, passive_openmp=passive):
                    native = NativeRuntime(executable, self.model, backend, 1, 2,
                                           passive_openmp=passive)
                    try:
                        ready = native.start()
                        expected = ({"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"}
                                    if backend == "vulkan" and passive else inherited)
                        self.assertEqual(ready["observed_environment"], expected)
                        self.assertEqual(ready["process_environment"], expected)
                        self.assertEqual({key: os.environ[key] for key in inherited}, inherited)
                    finally:
                        native.close()


class BenchmarkLoadTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="conflux-load-test-")
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def load(self, renderer, mode="both"):
        args = SimpleNamespace(renderer=renderer, load_mode=mode, ui=self.root / "ui",
                               audio_probe=self.root / "audio", quantum=256)
        return Load(args, self.root)

    def test_renderer_environment_is_scoped_to_load_children(self):
        inherited = {"QT_QUICK_BACKEND": "inherited", "QSG_RHI_BACKEND": "metal"}
        with patch.dict(os.environ, inherited):
            for renderer in ("opengl", "vulkan", "software"):
                with self.subTest(renderer=renderer):
                    load = self.load(renderer)
                    child = Mock()
                    child.poll.return_value = None
                    try:
                        with patch("stem_worker.subprocess.Popen", return_value=child) as launch, \
                             patch("stem_worker.time.sleep"):
                            load.start()
                        ui_env, audio_env = [call.kwargs["env"] for call in launch.call_args_list]
                        self.assertEqual(ui_env["QT_QPA_PLATFORM"], "wayland")
                        self.assertEqual(ui_env.get("QT_QUICK_BACKEND"),
                                         "software" if renderer == "software" else None)
                        self.assertNotIn("QT_QUICK_BACKEND", audio_env)
                        for environment in (ui_env, audio_env):
                            self.assertEqual(environment.get("QSG_RHI_BACKEND"),
                                             "vulkan" if renderer == "vulkan" else None)
                        self.assertEqual({key: os.environ[key] for key in inherited}, inherited)
                    finally:
                        for _, _, stdout, stderr, _ in load.children:
                            stdout.close()
                            stderr.close()

    def stopped_ui(self, renderer, report):
        load = self.load(renderer, "ui")
        stdout = (self.root / "ui.json").open("w")
        stdout.write(json.dumps(report))
        stderr = (self.root / "ui.stderr").open("w")
        child = Mock()
        child.wait.return_value = 0
        load.children.append(("ui", child, stdout, stderr, load.commands["ui"]))
        return load.stop()

    def test_ui_report_requires_requested_wayland_renderer_and_success(self):
        for renderer, api in (("opengl", 3), ("vulkan", 5), ("software", 1)):
            report = {"rendered": True, "capture_ok": True, "platform": "wayland",
                      "graphics_api": api}
            with self.subTest(renderer=renderer, report="valid"):
                self.assertEqual(self.stopped_ui(renderer, report)["errors"], [])
            for field, value in (("graphics_api", 5 if api != 5 else 3),
                                 ("platform", "offscreen"), ("rendered", False),
                                 ("capture_ok", False)):
                with self.subTest(renderer=renderer, invalid_field=field):
                    invalid = {**report, field: value}
                    self.assertIn("requested Wayland renderer did not render",
                                  self.stopped_ui(renderer, invalid)["errors"])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Behavior tests for the M0 supervisor, requiring neither model nor GPU."""

from contextlib import ExitStack
import hashlib
import io
import json
import os
import signal
from pathlib import Path
import stat
import subprocess
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
from tools.stems.export import _PinnedFile
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

    def test_verified_model_pin_is_reused_and_rejects_replacement(self):
        with ExitStack() as stack:
            model_pin = _PinnedFile(self.model, 1024, stack, None, time.monotonic() + 3)
            worker, backend = self.worker(_model_pin=model_pin)
            try:
                with patch("tools.stems.worker._open_model",
                           side_effect=AssertionError("verified pin must avoid a duplicate model scan")):
                    generation = worker.load(self.path)
                    ready = self.event(worker, "runtime_ready", generation)
                    self.assertEqual(ready["model_id"], hashlib.sha256(self.model.read_bytes()).hexdigest())
                    self.assertEqual(ready["model_id"], model_pin.digest)
            finally:
                worker.close()
            self.assertEqual(backend.starts, 1)
            self.assertTrue(backend.closed)

            replacement = self.root / "replacement.gguf"
            replacement.write_bytes(self.model.read_bytes())
            replacement.replace(self.model)
            worker, backend = self.worker(_model_pin=model_pin)
            try:
                generation = worker.load(self.path)
                failure = self.event(worker, "error", generation)
                self.assertIn("model changed", failure["message"])
                self.assertEqual(backend.starts, 0)
            finally:
                worker.close()

    def test_pinned_model_change_during_native_start_fails_and_closes_backend(self):
        with ExitStack() as stack:
            model_pin = _PinnedFile(self.model, 1024, stack, None, time.monotonic() + 3)

            class ChangingRuntime(FakeRuntime):
                def start(backend):
                    self.model.write_bytes(b"changed during native startup")
                    return super().start()

            worker, backend = self.worker(ChangingRuntime(), _model_pin=model_pin)
            try:
                generation = worker.load(self.path)
                failure = self.event(worker, "error", generation)
                self.assertIn("model changed during runtime loading", failure["message"])
            finally:
                worker.close()
            self.assertEqual(backend.starts, 1)
            self.assertTrue(backend.closed)

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
        latest = worker.seek(HOP + 7)
        fake.release()
        self.assertEqual(self.event(worker, "discarded", old)["request_id"], 1)
        source = self.event(worker, "source_ready", latest)
        self.assertTrue(source["reused"])
        self.assertEqual(source["source_format"], "f32le")
        self.assertTrue(fake.wait_call(2))
        fake.release(2)
        chunk = self.event(worker, "chunk", latest)
        self.assertEqual(chunk["frame_start"], HOP + 7)
        self.assertEqual(chunk["source_format"], "f32le")
        self.assertEqual(chunk["request_id"], 3)
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

    def test_seek_reuses_exact_window_tail_while_new_suffix_is_blocked(self):
        raw = self.root / "seek-tail.f32"
        self.audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([c["samples"] for c in self.collect(reference, reference.load_f32(raw))], axis=1)
        worker, fake = self.worker(FakeRuntime(blocked=True))
        generation = worker.load_f32(raw)
        self.assertTrue(fake.wait_call(1))
        fake.release()
        initial = self.event(worker, "chunk", generation)
        self.assertTrue(fake.wait_call(2))
        frame = HOP - 4704
        seek = worker.seek(frame)
        reused = self.event(worker, "chunk", seek, timeout=1)
        self.assertEqual(fake.calls, 2)
        fake.release()
        continued = self.event(worker, "chunk", seek, timeout=1)
        self.assertEqual(continued["request_id"], 2)
        self.assertEqual(continued["frame_start"], HOP)
        self.assertEqual(continued["metrics"]["id"], 2)
        self.assertNotIn("inference_reused", continued)
        self.assertEqual((reused["frame_start"], reused["frames"]), (frame, 4704))
        self.assertEqual(reused["request_id"], initial["request_id"])
        self.assertIs(reused["inference_reused"], True)
        self.assertNotIn("metrics", reused)
        np.testing.assert_array_equal(reused["samples"], expected[:, frame:HOP])
        self.assertTrue(fake.wait_call(3))
        fake.release()
        suffix = [continued, *self.collect(worker, seek)]
        np.testing.assert_array_equal(np.concatenate([c["samples"] for c in suffix], axis=1), expected[:, HOP:])
        self.assertEqual(fake.calls, 3)

    def test_backward_seek_reuses_three_published_windows_with_an_active_request(self):
        raw = self.root / "backward-seek.f32"
        audio = fixture(self.root / "four-windows.wav", 3 * HOP + 137)
        audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([c["samples"] for c in self.collect(reference, reference.load_f32(raw))], axis=1)

        worker, fake = self.worker(FakeRuntime(blocked=True), max_ready=1)
        generation = worker.load_f32(raw)
        published = []
        for call in range(1, 4):
            self.assertTrue(fake.wait_call(call))
            fake.release()
            published.append(self.event(worker, "chunk", generation))
        self.assertTrue(fake.wait_call(4))
        frame = HOP - 4704
        seek = worker.seek(frame)
        replayed = [self.event(worker, "chunk", seek, timeout=1) for _ in published]
        self.assertEqual(fake.calls, 4)
        self.assertEqual(worker.statistics()["inflight"]["request_id"], 4)
        fake.release()
        suffix, = self.collect(worker, seek)
        self.assertEqual(suffix["request_id"], 4)
        self.assertNotIn("inference_reused", suffix)
        self.assertEqual(replayed[0]["frame_start"], frame)
        self.assertEqual([chunk["request_id"] for chunk in replayed],
                         [chunk["request_id"] for chunk in published])
        self.assertTrue(all(chunk["inference_reused"] is True and "metrics" not in chunk for chunk in replayed))
        np.testing.assert_array_equal(np.concatenate([chunk["samples"] for chunk in replayed], axis=1),
                                      expected[:, frame:3 * HOP])

        self.assertEqual(fake.calls, 4)
        np.testing.assert_array_equal(suffix["samples"], expected[:, 3 * HOP:])

        # The first window has aged out of the three recent-window slots.
        # A replay at the converter anchor must still use the original opening.
        opening_anchor = 236082
        revisit = worker.seek(opening_anchor)
        opening = self.event(worker, "chunk", revisit, timeout=1)
        restored = [opening, *self.collect(worker, revisit)]
        self.assertEqual([chunk["request_id"] for chunk in restored], [1, 2, 3, 4])
        self.assertEqual(opening["frame_start"], opening_anchor)
        self.assertTrue(all(chunk["inference_reused"] is True for chunk in restored))
        self.assertEqual(fake.calls, 4)
        np.testing.assert_array_equal(np.concatenate([chunk["samples"] for chunk in restored], axis=1),
                                      expected[:, opening_anchor:])

        # The same retained opening supplies exact-grid preroll, without
        # evicting one of the three useful recent weighted windows.
        restored = self.collect(worker, worker.seek(HOP))
        self.assertEqual([chunk["request_id"] for chunk in restored], [2, 3, 4])
        self.assertTrue(all(chunk["inference_reused"] is True for chunk in restored))
        self.assertEqual(fake.calls, 4)
        np.testing.assert_array_equal(np.concatenate([chunk["samples"] for chunk in restored], axis=1),
                                      expected[:, HOP:])

    def test_cold_seek_only_infers_windows_that_contribute_to_output(self):
        raw = self.root / "cold-seek-overlap-boundary.f32"
        self.audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([chunk["samples"] for chunk in
                                   self.collect(reference, reference.load_f32(raw))], axis=1)

        for frame, first_window in ((HOP + OVERLAP - 1, 0), (HOP + OVERLAP, HOP)):
            with self.subTest(frame=frame):
                worker, fake = self.worker(FakeRuntime(blocked=True), max_ready=1)
                opened = worker.load_f32(raw, start_frame=len(self.audio))
                self.assertEqual(self.collect(worker, opened), [])

                seek = worker.seek(frame)
                started = self.event(worker, "started", seek)
                self.assertEqual(started["window_start_frame"], first_window)
                self.assertEqual(started["preroll"], first_window == 0)
                calls_to_target = 2 if first_window == 0 else 1
                fake.release(calls_to_target)
                first = self.event(worker, "chunk", seek)
                self.assertEqual((first["request_id"], first["frame_start"]), (calls_to_target, frame))
                np.testing.assert_array_equal(first["samples"], expected[:, frame:2 * HOP])
                self.assertTrue(fake.wait_call(calls_to_target + 1))

                repeat = worker.seek(frame)
                replayed = self.event(worker, "chunk", repeat, timeout=1)
                self.assertTrue(replayed["inference_reused"])
                self.assertEqual(replayed["request_id"], first["request_id"])
                np.testing.assert_array_equal(replayed["samples"], first["samples"])
                fake.release()
                final, = self.collect(worker, repeat)
                np.testing.assert_array_equal(final["samples"], expected[:, 2 * HOP:])
                self.assertEqual(fake.calls, calls_to_target + 1)

    def test_repeat_cold_seek_reuses_preroll_while_suffix_is_pending(self):
        raw = self.root / "cold-seek-preroll.f32"
        self.audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([chunk["samples"] for chunk in
                                   self.collect(reference, reference.load_f32(raw))], axis=1)
        worker, fake = self.worker(FakeRuntime(blocked=True), max_ready=1)
        start = HOP + 17
        original = worker.load_f32(raw, start_frame=start)
        self.assertTrue(fake.wait_call(1))
        fake.release(2)
        first = self.event(worker, "chunk", original)
        self.assertEqual((first["request_id"], first["frame_start"]), (2, start))
        np.testing.assert_array_equal(first["samples"], expected[:, start:2 * HOP])
        self.assertTrue(fake.wait_call(3))

        target = start + 19
        repeat = worker.seek(target)
        replayed = self.event(worker, "chunk", repeat, timeout=1)
        self.assertEqual((replayed["request_id"], replayed["frame_start"]), (2, target))
        self.assertTrue(replayed["inference_reused"])
        self.assertEqual(fake.calls, 3)
        self.assertEqual(worker.statistics()["inflight"]["request_id"], 3)
        np.testing.assert_array_equal(replayed["samples"], expected[:, target:2 * HOP])

        fake.release()
        remaining, = self.collect(worker, repeat)
        self.assertEqual(remaining["request_id"], 3)
        self.assertNotIn("inference_reused", remaining)
        np.testing.assert_array_equal(remaining["samples"], expected[:, 2 * HOP:])
        self.assertEqual(fake.calls, 3)

    def test_repeat_seek_keeps_opening_preroll_with_cached_forward_windows(self):
        raw = self.root / "opening-preroll.f32"
        audio = fixture(self.root / "opening-preroll.wav", 4 * HOP + 137)
        audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([chunk["samples"] for chunk in
                                   self.collect(reference, reference.load_f32(raw))], axis=1)
        worker, fake = self.worker(FakeRuntime(blocked=True), max_ready=1)

        # Starting after the overlap fills all three recent slots without the opening.
        original = worker.load_f32(raw, start_frame=HOP + OVERLAP)
        forward = []
        for call in range(1, 4):
            self.assertTrue(fake.wait_call(call))
            fake.release()
            forward.append(self.event(worker, "chunk", original))
        self.assertTrue(fake.wait_call(4))

        first = worker.seek(HOP + 17)
        fake.release()  # Finish the superseded suffix request.
        opening = self.event(worker, "started", first)
        self.assertEqual(opening["window_start_frame"], 0)
        self.assertTrue(opening["preroll"])
        self.assertTrue(fake.wait_call(5))
        fake.release()
        first_pass = [self.event(worker, "chunk", first) for _ in forward]
        self.assertEqual([chunk["request_id"] for chunk in first_pass],
                         [chunk["request_id"] for chunk in forward])
        np.testing.assert_array_equal(np.concatenate([c["samples"] for c in first_pass], axis=1),
                                      expected[:, HOP + 17:4 * HOP])
        self.assertTrue(fake.wait_call(6))

        repeat = worker.seek(HOP + 37)
        replay = [self.event(worker, "chunk", repeat, timeout=1) for _ in forward]
        self.assertEqual([chunk["request_id"] for chunk in replay],
                         [chunk["request_id"] for chunk in forward])
        self.assertTrue(all(chunk["inference_reused"] for chunk in replay))
        np.testing.assert_array_equal(np.concatenate([c["samples"] for c in replay], axis=1),
                                      expected[:, HOP + 37:4 * HOP])
        self.assertEqual(fake.calls, 6)

        fake.release()
        tail, = self.collect(worker, repeat)
        self.assertEqual(tail["request_id"], 6)
        np.testing.assert_array_equal(tail["samples"], expected[:, 4 * HOP:])
        self.assertEqual(fake.calls, 6)

    def test_seek_uses_active_target_window_with_cached_preroll(self):
        raw = self.root / "seek-active-target.f32"
        self.audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([c["samples"] for c in self.collect(reference, reference.load_f32(raw))], axis=1)
        worker, fake = self.worker(FakeRuntime(blocked=True), max_ready=1)
        original = worker.load_f32(raw)
        self.assertTrue(fake.wait_call(1))
        fake.release()
        self.event(worker, "chunk", original)
        self.assertTrue(fake.wait_call(2))

        frame = HOP + 17
        latest = worker.seek(frame)
        fake.release()
        chunk = self.event(worker, "chunk", latest, timeout=1)
        self.assertEqual((chunk["request_id"], chunk["frame_start"]), (2, frame))
        self.assertFalse(chunk.get("inference_reused", False))
        np.testing.assert_array_equal(chunk["samples"], expected[:, frame:2 * HOP])
        self.assertTrue(fake.wait_call(3))
        fake.release()
        suffix, = self.collect(worker, latest)
        self.assertEqual(suffix["request_id"], 3)
        np.testing.assert_array_equal(suffix["samples"], expected[:, 2 * HOP:])
        self.assertEqual(fake.calls, 3)

    def test_seek_reuses_reply_received_before_superseded_chunk_is_published(self):
        raw = self.root / "seek-returned-target.f32"
        self.audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([chunk["samples"] for chunk in
                                   self.collect(reference, reference.load_f32(raw))], axis=1)
        worker, fake = self.worker(max_ready=1)
        received = threading.Event()
        resume = threading.Event()
        infer = worker._inference_for_window

        def receive_window(command, source, start, preroll, **options):
            reply = infer(command, source, start, preroll, **options)
            if command.generation == 1 and start == HOP:
                received.set()
                if not resume.wait(timeout=3):
                    raise TimeoutError("test did not resume stem reconstruction")
            return reply

        with patch.object(worker, "_inference_for_window", side_effect=receive_window):
            original = worker.load_f32(raw)
            self.event(worker, "chunk", original)
            try:
                self.assertTrue(received.wait(timeout=3))
                frame = HOP + 17
                latest = worker.seek(frame)
            finally:
                resume.set()
            first, tail = self.collect(worker, latest)

        self.assertEqual((first["request_id"], first["frame_start"]), (2, frame))
        self.assertEqual(first["metrics"]["id"], 2)
        self.assertFalse(first.get("inference_reused", False))
        self.assertEqual((tail["request_id"], fake.calls), (3, 3))
        np.testing.assert_array_equal(np.concatenate([first["samples"], tail["samples"]], axis=1),
                                      expected[:, frame:])

    def test_return_seek_preserves_completed_window_during_superseded_cache_insertion(self):
        raw = self.root / "superseded-cache-insertion.f32"
        audio = fixture(self.root / "superseded-cache-insertion.wav", 3 * HOP + 137)
        audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        continuous = self.collect(reference, reference.load_f32(raw))
        expected = continuous[1]["samples"][:, OVERLAP:]

        for edge in ("before_validation", "during_validation"):
            with self.subTest(edge=edge):
                worker, backend = self.worker(FakeRuntime(blocked=True), max_ready=1)
                try:
                    opened = worker.load_f32(raw)
                    self.assertTrue(backend.wait_call(1))
                    backend.release()
                    self.event(worker, "chunk", opened)
                    self.assertTrue(backend.wait_call(2))

                    entered, resume = threading.Event(), threading.Event()
                    remember = worker._remember_superseded_seek_window
                    isfinite = np.isfinite

                    def wait_for_seek():
                        entered.set()
                        if not resume.wait(3):
                            raise TimeoutError("test did not release obsolete window validation")

                    def during_validation(samples):
                        wait_for_seek()
                        return isfinite(samples)

                    def handoff(command, pending, output):
                        if edge == "before_validation":
                            wait_for_seek()
                            return remember(command, pending, output)
                        with patch("tools.stems.worker.np.isfinite", during_validation):
                            return remember(command, pending, output)

                    with patch.object(worker, "_remember_superseded_seek_window", side_effect=handoff):
                        try:
                            worker.seek(2 * HOP + OVERLAP)
                            backend.release()
                            self.assertTrue(entered.wait(3), "obsolete request did not enter the cache")
                            frame = HOP + OVERLAP
                            generation = worker.seek(frame)
                        finally:
                            resume.set()
                        chunk = self.event(worker, "chunk", generation, timeout=1)

                    self.assertEqual((chunk["request_id"], chunk["frame_start"]), (2, frame))
                    self.assertIs(chunk["inference_reused"], True)
                    np.testing.assert_array_equal(chunk["samples"], expected)
                finally:
                    worker.close(timeout=0.05)

    def test_return_seek_can_reuse_completed_window_with_all_cache_slots_full(self):
        raw = self.root / "full-seek-cache.f32"
        audio = fixture(self.root / "full-seek-cache.wav", 6 * HOP + OVERLAP + 137)
        audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        continuous = self.collect(reference, reference.load_f32(raw))

        for action in ("return_before_validation", "return_during_validation", "continue_away"):
            with self.subTest(action=action):
                worker, backend = self.worker(FakeRuntime(blocked=True), max_ready=1)
                try:
                    initial = worker.load_f32(raw)
                    for call in range(1, 5):
                        self.assertTrue(backend.wait_call(call))
                        backend.release()
                        self.event(worker, "chunk", initial)
                    self.assertTrue(backend.wait_call(5))
                    with worker._condition:
                        self.assertEqual(set(worker._weighted_windows), {HOP, 2 * HOP, 3 * HOP})

                    entered, resume = threading.Event(), threading.Event()
                    remember = worker._remember_superseded_seek_window
                    finite = np.isfinite

                    def pause_validation():
                        entered.set()
                        if not resume.wait(3):
                            raise TimeoutError("test did not release the superseded reply")

                    def wait_during_validation(samples):
                        pause_validation()
                        return finite(samples)

                    def handoff(command, pending, output):
                        if action == "return_during_validation":
                            with patch("tools.stems.worker.np.isfinite", wait_during_validation):
                                return remember(command, pending, output)
                        pause_validation()
                        return remember(command, pending, output)

                    with patch.object(worker, "_remember_superseded_seek_window", side_effect=handoff):
                        try:
                            worker.seek(6 * HOP + OVERLAP)
                            backend.release()
                            self.assertTrue(entered.wait(2), "superseded reply did not reach validation")
                            index = 4
                            if action != "continue_away":
                                generation = worker.seek(index * HOP)
                        finally:
                            resume.set()
                        if action == "continue_away":
                            self.assertTrue(backend.wait_call(6))
                            with worker._condition:
                                self.assertEqual(set(worker._weighted_windows), {HOP, 2 * HOP, 3 * HOP})
                            index = 1
                            generation = worker.seek(index * HOP)
                        chunk = self.event(worker, "chunk", generation, timeout=1)

                    self.assertEqual((chunk["request_id"], chunk["frame_start"]), (index + 1, index * HOP))
                    self.assertIs(chunk["inference_reused"], True)
                    np.testing.assert_array_equal(chunk["samples"], continuous[index]["samples"])
                    if action != "continue_away":
                        with worker._condition:
                            self.assertEqual(set(worker._weighted_windows), {2 * HOP, 3 * HOP, 4 * HOP})
                finally:
                    worker.close(timeout=0.05)

    def test_seek_retains_active_target_while_filling_missing_preroll(self):
        raw = self.root / "seek-active-target-uncached-preroll.f32"
        self.audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([c["samples"] for c in self.collect(reference, reference.load_f32(raw))], axis=1)
        worker, fake = self.worker(FakeRuntime(blocked=True), max_ready=1)
        opened = worker.load_f32(raw, start_frame=len(self.audio))
        self.assertEqual(self.collect(worker, opened), [])

        original = worker.seek(HOP + OVERLAP)
        target = self.event(worker, "started", original)
        self.assertEqual(target["window_start_frame"], HOP)
        self.assertTrue(fake.wait_call(1))

        frame = HOP + 17
        latest = worker.seek(frame)
        fake.release(3)
        overlap = self.event(worker, "started", latest)
        self.assertEqual(overlap["window_start_frame"], 0)
        self.assertTrue(overlap["preroll"])
        first = self.event(worker, "chunk", latest)
        self.assertEqual(first["request_id"], target["request_id"])
        self.assertEqual(first["metrics"]["id"], target["request_id"])
        self.assertFalse(first.get("inference_reused", False))
        self.assertGreaterEqual(first["hop_service_ms"], (first["ready_at"] - overlap["timestamp"]) * 1000)
        tail, = self.collect(worker, latest)
        self.assertEqual([first["frame_start"], tail["frame_start"]], [frame, 2 * HOP])
        np.testing.assert_array_equal(np.concatenate([first["samples"], tail["samples"]], axis=1),
                                      expected[:, frame:])
        self.assertEqual(worker.statistics()["discarded_count"], 0)
        self.assertEqual(fake.calls, 3)

    def test_repeated_cold_seek_uses_parked_target_during_missing_preroll(self):
        raw = self.root / "repeated-cold-target.f32"
        self.audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([chunk["samples"] for chunk in
                                   self.collect(reference, reference.load_f32(raw))], axis=1)
        cases = (("needs overlap", HOP + 37, 1, 3),
                 ("past overlap", HOP + OVERLAP + 37, 1, 3),
                 ("after cancellation", HOP + 37, 4, 5))
        for description, frame, expected_request, expected_calls in cases:
            with self.subTest(description=description):
                worker, fake = self.worker(FakeRuntime(blocked=True), max_ready=1)
                opened = worker.load_f32(raw, start_frame=len(self.audio))
                self.assertEqual(self.collect(worker, opened), [])

                original = worker.seek(HOP + OVERLAP)
                target = self.event(worker, "started", original)
                self.assertEqual((target["request_id"], target["window_start_frame"]), (1, HOP))
                self.assertTrue(fake.wait_call(1))
                first_seek = worker.seek(HOP + 17)
                fake.release()
                overlap = self.event(worker, "started", first_seek)
                self.assertEqual(overlap["window_start_frame"], 0)
                self.assertTrue(overlap["preroll"])
                self.assertTrue(fake.wait_call(2))

                if description == "after cancellation":
                    worker.cancel()
                final_seek = worker.seek(frame)
                fake.release(4)
                first, tail = self.collect(worker, final_seek)

                self.assertEqual((first["request_id"], first["frame_start"]), (expected_request, frame))
                self.assertEqual(first["metrics"]["id"], expected_request)
                self.assertFalse(first.get("inference_reused", False))
                self.assertEqual((tail["request_id"], fake.calls), (expected_calls, expected_calls))
                np.testing.assert_array_equal(np.concatenate([first["samples"], tail["samples"]], axis=1),
                                              expected[:, frame:])
                worker.close()

    def test_seek_uses_active_preroll_before_first_target_chunk(self):
        raw = self.root / "seek-active-preroll.f32"
        self.audio.astype("<f4").tofile(raw)
        reference, _ = self.worker()
        expected = np.concatenate([c["samples"] for c in self.collect(reference, reference.load_f32(raw))], axis=1)
        worker, fake = self.worker(FakeRuntime(blocked=True), max_ready=1)
        worker.load_f32(raw)
        self.assertTrue(fake.wait_call(1))

        frame = HOP + 17
        latest = worker.seek(frame)
        fake.release()
        self.assertTrue(fake.wait_call(2))
        fake.release()
        chunk = self.event(worker, "chunk", latest, timeout=1)
        self.assertEqual((chunk["request_id"], chunk["frame_start"]), (2, frame))
        np.testing.assert_array_equal(chunk["samples"], expected[:, frame:2 * HOP])
        self.assertTrue(fake.wait_call(3))
        fake.release()
        suffix, = self.collect(worker, latest)
        self.assertEqual(suffix["request_id"], 3)
        np.testing.assert_array_equal(suffix["samples"], expected[:, 2 * HOP:])
        self.assertEqual(fake.calls, 3)

    def test_superseded_inference_failure_preserves_cold_seek(self):
        raw = self.root / "cold-seek-fault.f32"
        self.audio.astype("<f4").tofile(raw)
        broken = FakeRuntime(blocked=True, fail=True)
        replacement = FakeRuntime()
        backends = iter((broken, replacement))
        worker = StemWorker("fake-runtime", self.model, _backend_factory=lambda *_: next(backends))
        self.workers.append(worker)

        old = worker.load_f32(raw)
        self.assertTrue(broken.wait_call(1))
        frame = HOP + 17
        latest = worker.seek(frame)
        broken.release()

        failure = self.event(worker, "error")
        self.assertEqual(failure["generation"], old)
        self.assertIn("injected runtime fault", failure["message"])
        chunks = self.collect(worker, latest)
        self.assertEqual(chunks[0]["frame_start"], frame)
        self.assertEqual(chunks[0]["request_id"], 3)  # Prior-grid window then requested window.
        self.assertFalse(chunks[0].get("inference_reused", False))
        reference, _ = self.worker()
        continuous = self.collect(reference, reference.load_f32(raw))
        expected = np.concatenate([chunk["samples"] for chunk in continuous], axis=1)
        actual = np.concatenate([chunk["samples"] for chunk in chunks], axis=1)
        np.testing.assert_array_equal(actual, expected[:, frame:])
        self.assertTrue(broken.closed)
        self.assertEqual(replacement.starts, 1)


    def test_cached_seek_fault_marks_active_prefix_stale(self):
        raw = self.root / "cached-seek-fault.f32"
        self.audio.astype("<f4").tofile(raw)
        worker, fake = self.worker(FakeRuntime(blocked=True))
        old = worker.load_f32(raw)
        self.assertTrue(fake.wait_call(1))
        fake.release()
        initial = self.event(worker, "chunk", old)
        self.assertTrue(fake.wait_call(2))

        frame = HOP - 4704
        latest = worker.seek(frame)
        cached = self.event(worker, "chunk", latest, timeout=1)
        self.assertTrue(cached["inference_reused"])
        self.assertTrue(worker.is_current(cached))
        np.testing.assert_array_equal(cached["samples"], initial["samples"][:, frame:])
        fake.fail = True
        fake.release()

        failure = self.event(worker, "error")
        self.assertEqual(failure["generation"], latest)
        self.assertIn("injected runtime fault", failure["message"])
        self.assertFalse(worker.is_current(cached))
        self.assertEqual(fake.calls, 2)

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

    def test_seek_supersedes_window_after_read_before_native_submission(self):
        path = self.root / "direct-submit.f32"
        audio = fixture(self.root / "direct-submit.wav", 2 * HOP + OVERLAP + 137)
        audio.astype("<f4").tofile(path)
        reference, _ = self.worker()
        expected = self.collect(reference, reference.load_f32(path))[-1]["samples"]

        worker, fake = self.worker()
        read_window = worker._read_window
        check = worker._check
        prepared = threading.Event()
        resume = threading.Event()
        after_read = False

        def read(source, start):
            nonlocal after_read
            window = read_window(source, start)
            after_read = start == 0
            return window

        def stop_after_validation(command):
            nonlocal after_read
            check(command)
            if after_read:
                after_read = False
                prepared.set()
                if not resume.wait(3):
                    raise TimeoutError("test did not release prepared window")

        with (patch.object(worker, "_read_window", side_effect=read),
              patch.object(worker, "_check", side_effect=stop_after_validation)):
            worker.load_f32(path)
            try:
                self.assertTrue(prepared.wait(3), "first window never passed validation")
                latest = worker.seek(2 * HOP + OVERLAP)
            finally:
                resume.set()
            chunk, = self.collect(worker, latest)

        self.assertEqual(chunk["frame_start"], 2 * HOP + OVERLAP)
        np.testing.assert_array_equal(chunk["samples"], expected[:, OVERLAP:])
        self.assertEqual(chunk["request_id"], 1)
        self.assertEqual(fake.calls, 1)
        self.assertEqual(worker.statistics()["discarded_count"], 0)

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

    def test_model_fifo_replacement_before_hash_open_is_rejected_without_backend_start(self):
        # Isolate the real supervisor so even the old blocking Path.open cannot
        # strand this test runner. run(timeout=...) kills AND reaps its child.
        script = r"""
import os
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import Mock, patch
from tools.stems import worker as module

case = unittest.TestCase()
model, source = map(Path, sys.argv[1:])
real_signature = module._signature
replaced = False
def replace_after_capture(path):
    global replaced
    captured = real_signature(path)
    if path == model and not replaced:
        model.unlink()
        os.mkfifo(model)
        replaced = True
    return captured

factory = Mock(side_effect=AssertionError("backend must not start for a replaced model"))
with patch.object(module, "_signature", side_effect=replace_after_capture):
    owner = module.StemWorker("unused-runtime", model, _backend_factory=factory)
    try:
        generation = owner.load(source)
        failure = None
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            event = owner.poll(0.05)
            if event is not None and event["event"] == "error":
                failure = event
                break
        case.assertTrue(replaced)
        case.assertIsNotNone(failure, "model FIFO replacement blocked instead of emitting an error")
        case.assertEqual(failure["generation"], generation)
        case.assertIn("model changed", failure["message"])
        factory.assert_not_called()
        case.assertEqual(owner.statistics()["backend_start_count"], 0)
    finally:
        owner.close(timeout=0.1)
case.assertFalse(owner._thread.is_alive())
factory.assert_not_called()
"""
        try:
            result = subprocess.run(
                [sys.executable, "-B", "-c", script, str(self.model), str(self.path)],
                cwd=Path(__file__).resolve().parents[1], capture_output=True,
                text=True, timeout=10, check=False)
        except subprocess.TimeoutExpired:
            self.fail("model FIFO replacement blocked the supervisor; isolated child was killed and reaped")
        self.assertEqual(result.returncode, 0, result.stderr[-4096:])

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
                self.assertEqual(native.process.returncode, -signal.SIGKILL)
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

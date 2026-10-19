#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Offline export behavior/failure regressions; no model, GPU or audio device."""

from dataclasses import replace
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.stems import export as exporter
from tools.stems.worker import CHANNELS, HOP, RATE, STEMS, WINDOW, NativeRuntime, StemWorker


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ready(backend="vulkan", threads=2):
    return {"event": "ready", "sample_rate": RATE, "channels": CHANNELS,
            "segment_frames": WINDOW, "stems": list(STEMS), "backend": backend,
            "threads": threads, "revision": "a" * 40, "weight_storage": "f32",
            "fence_wait_mode": "blocking", "pid": 123, "load_ms": 0.1}


class ScriptedWorker:
    def __init__(self, runtime, model, *, backend, threads, **unused):
        self.model = model
        self.backend = backend
        self.threads = threads
        self.closed = False
        self.mutate = None
        self.on_poll = None
        self.on_close = None
        self.silent = False

    def load_f32(self, source):
        self.source = source
        audio = np.fromfile(source, dtype="<f4").reshape(-1, CHANNELS)
        frames = audio.shape[0]
        identity = sha(source)
        common = {"generation": 7}
        self.events = [{**common, "event": "control"},
                       {**common, "event": "runtime_ready", "model_id": sha(self.model),
                        "runtime": ready(self.backend, self.threads)},
                       {**common, "event": "source_ready", "source_id": identity,
                        "source_frames": frames, "source_format": "f32le", "mean": 0.1, "std": 2.0}]
        self.audio = np.stack([audio * (index + 1) for index in range(4)])
        for offset in range(0, frames, HOP):
            count = min(HOP, frames - offset)
            self.events.append({**common, "event": "chunk", "source_id": identity,
                                "model_id": sha(self.model), "source_frames": frames,
                                "source_format": "f32le", "stems": STEMS,
                                "sample_rate": RATE, "channels": CHANNELS,
                                "frame_start": offset, "window_start_frame": offset,
                                "frames": count, "samples": self.audio[:, offset:offset + count].copy()})
        self.events.append({**common, "event": "complete", "source_id": identity, "frame_start": frames})
        if self.mutate:
            self.mutate(self.events)
        return 7

    def poll(self, timeout):
        if self.on_poll:
            action, self.on_poll = self.on_poll, None
            action()
        if self.silent:
            time.sleep(timeout)
            return None
        return self.events.pop(0)

    def is_current(self, event):
        return event.get("generation") == 7

    def statistics(self):
        return {"telemetry_dropped": 0}

    def close(self):
        if self.on_close:
            self.on_close()
        self.closed = True


class ExportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "input.f32"
        self.input = np.array([[2.5, -3], [0.5, 0.1], [-2, 4], [0, -0.25], [1, 2]], dtype="<f4")
        self.input.tofile(self.source)
        self.model = self.root / "model.bin"
        self.model.write_bytes(b"bounded fake model")
        self.runtime = self.root / "runtime"
        self.runtime.write_bytes(b"bounded fake executable")
        self.runtime.chmod(0o700)
        self.destination = self.root / "raw"
        self.workers = []
        self.configure = lambda worker: None

    def factory(self, *args, **kwargs):
        worker = ScriptedWorker(*args, **kwargs)
        self.configure(worker)
        self.workers.append(worker)
        return worker

    def run_export(self, **kwargs):
        return exporter.export_f32(self.source, self.destination, self.runtime, self.model,
                                   _worker_factory=self.factory, **kwargs)

    def assert_unpublished(self):
        self.assertFalse(self.destination.exists())
        self.assertFalse((self.root / "raw.stage").exists())
        self.assertTrue(self.source.exists())
        if self.workers:
            self.assertTrue(self.workers[-1].closed)

    def test_atomic_aligned_headroom_and_verifiable_manifest(self):
        original = self.source.read_bytes()
        self.configure = lambda worker: setattr(worker, "on_close", lambda: self.assertFalse(self.destination.exists()))
        manifest = self.run_export()
        self.assertTrue(self.workers[0].closed)
        self.assertEqual(set(path.name for path in self.destination.iterdir()),
                         {"manifest.json", *(name + ".f32" for name in STEMS)})
        self.assertEqual(manifest, json.loads((self.destination / "manifest.json").read_text()))
        self.assertEqual(manifest["describe"], exporter.describe())
        self.assertEqual(manifest["source"]["sha256"], sha(self.source))
        self.assertNotIn("original_source_sha256", manifest)
        self.assertEqual(manifest["source"]["frames"], len(self.input))
        self.assertEqual(manifest["runtime"]["ready"]["weight_storage"], "f32")
        self.assertEqual(manifest["runtime"]["ready"]["fence_wait_mode"], "blocking")
        self.assertEqual(manifest["reservation_bytes"], exporter.reservation_bytes(self.source.stat().st_size))
        for index, (name, leaf) in enumerate(zip(STEMS, manifest["stems"])):
            path = self.destination / leaf["file"]
            self.assertEqual(leaf["name"], name)
            self.assertEqual(leaf["bytes"], len(original))
            self.assertEqual(leaf["sha256"], sha(path))
            np.testing.assert_array_equal(np.fromfile(path, dtype="<f4").reshape(-1, 2), self.input * (index + 1))
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertFalse((self.root / "raw.stage").exists())

    def test_chunks_cover_full_hops_and_exact_tail(self):
        audio = np.tile(self.input, (HOP // len(self.input) + 1, 1))[:HOP + 3]
        audio.tofile(self.source)
        manifest = self.run_export()
        self.assertEqual(manifest["source"]["frames"], HOP + 3)
        for index, leaf in enumerate(manifest["stems"]):
            actual = np.fromfile(self.destination / leaf["file"], dtype="<f4").reshape(-1, 2)
            np.testing.assert_array_equal(actual, audio * (index + 1))

    def test_bad_generation_identity_geometry_and_audio_never_publish(self):
        def change(kind, key, value):
            return lambda events: next(event for event in events if event["event"] == kind).__setitem__(key, value)
        faults = [change("chunk", key, value) for key, value in (
            ("generation", 8), ("source_id", "bad"), ("model_id", "bad"),
            ("stems", tuple(reversed(STEMS))), ("sample_rate", 48000), ("channels", True),
            ("frame_start", 1), ("window_start_frame", 1), ("frames", 4),
            ("source_frames", 6), ("source_format", "wav_pcm16"),
            ("samples", np.ones((4, 5, 2), dtype=np.float64)),
            ("samples", np.full((4, 5, 2), np.inf, dtype=np.float32)))]
        faults += [change("source_ready", "std", float("nan")), change("source_ready", "source_id", "bad"),
                   change("complete", "frame_start", 4), change("runtime_ready", "model_id", "bad"),
                   lambda events: events[1]["runtime"].__setitem__("weight_storage", "unknown"),
                   lambda events: events[1]["runtime"].__setitem__("revision", "oversized" * 100),
                   lambda events: events.__delitem__(3)]
        for mutate in faults:
            with self.subTest(mutate=mutate):
                self.configure = lambda worker: setattr(worker, "mutate", mutate)
                with self.assertRaises(RuntimeError):
                    self.run_export()
                self.assert_unpublished()

    def test_limits_and_nonregular_inputs_fail_before_worker(self):
        for limits in (replace(exporter.ExportLimits(), max_input_bytes=8),
                       replace(exporter.ExportLimits(), max_output_bytes=8),
                       replace(exporter.ExportLimits(), max_job_bytes=100),
                       replace(exporter.ExportLimits(), max_diagnostic_bytes=True),
                       replace(exporter.ExportLimits(), max_seconds=float("inf")),
                       replace(exporter.ExportLimits(), request_timeout=31)):
            with self.subTest(limits=limits), self.assertRaises(ValueError):
                self.run_export(limits=limits)
        self.assertEqual(self.workers, [])
        for path in (self.source, self.model, self.runtime):
            original = path.read_bytes()
            path.unlink()
            os.mkfifo(path)
            with self.subTest(path=path), self.assertRaises(ValueError):
                self.run_export()
            path.unlink()
            target = self.root / "target"
            target.write_bytes(original)
            path.symlink_to(target)
            with self.assertRaises(OSError):
                self.run_export()
            path.unlink()
            path.write_bytes(original)
        self.source.write_bytes(b"bad length")
        with self.assertRaises(ValueError):
            self.run_export()
        self.assertEqual(self.workers, [])

    def test_existing_destination_and_held_lock_are_preserved(self):
        self.destination.mkdir()
        sentinel = self.destination / "keep"
        sentinel.write_text("keep")
        with self.assertRaises(FileExistsError):
            self.run_export()
        self.assertEqual(sentinel.read_text(), "keep")
        sentinel.unlink()
        self.destination.rmdir()
        lock = os.open(self.root / ".raw.export.lock", os.O_RDWR)
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                self.run_export()
        finally:
            os.close(lock)
        self.assertEqual(self.workers, [])

    def test_existing_private_stage_is_never_deleted_or_reused(self):
        stage = self.root / "raw.stage"
        stage.mkdir()
        sentinel = stage / "prior-job"
        sentinel.write_text("preserve")
        with self.assertRaises(FileExistsError):
            self.run_export()
        self.assertEqual(sentinel.read_text(), "preserve")
        self.assertEqual(self.workers, [])
        self.assertFalse(self.destination.exists())

    def test_cancel_before_during_and_after_last_chunk_closes_worker(self):
        for at in ("before", "poll", "close"):
            with self.subTest(at=at):
                cancel = threading.Event()
                if at == "before":
                    cancel.set()
                elif at == "poll":
                    self.configure = lambda worker: setattr(worker, "on_poll", cancel.set)
                else:
                    self.configure = lambda worker: setattr(worker, "on_close", cancel.set)
                with self.assertRaises(exporter.ExportCancelled):
                    self.run_export(cancel=cancel)
                self.assert_unpublished()

    def test_deadline_and_changed_input_close_without_publication(self):
        self.configure = lambda worker: setattr(worker, "silent", True)
        with self.assertRaises(TimeoutError):
            self.run_export(limits=replace(exporter.ExportLimits(), max_seconds=0.03))
        self.assert_unpublished()
        self.configure = lambda worker: setattr(worker, "on_poll", lambda: self.source.write_bytes(b"x" * self.source.stat().st_size))
        with self.assertRaises(RuntimeError):
            self.run_export()
        self.assert_unpublished()

    def test_io_failure_and_tool_identity_change_never_publish(self):
        with patch.object(exporter.os, "fsync", side_effect=OSError("disk failure")):
            with self.assertRaisesRegex(OSError, "disk failure"):
                self.run_export()
        self.assert_unpublished()
        actual = exporter.describe()
        with patch.object(exporter, "describe", side_effect=[actual, {**actual, "numpy_version": "changed"}]):
            with self.assertRaisesRegex(RuntimeError, "identity changed"):
                self.run_export()
        self.assert_unpublished()

    def test_constructor_setup_and_wait_failure_preserve_original_and_native_spool(self):
        process = Mock()
        process.stdout, process.stdin, process.stderr = io.BytesIO(), io.BytesIO(), io.BytesIO()
        process.poll.return_value = None
        process.wait.side_effect = subprocess.TimeoutExpired("unreaped fake child", 2)
        selector = Mock()
        selector.register.side_effect = RuntimeError("original setup failure")
        def factory(runtime, model, **kwargs):
            return NativeRuntime(runtime, model, "cpu", 2, 1,
                                 spool_directory=self.root / "raw.stage" / "native", max_stderr_bytes=4096)
        with patch("tools.stems.worker.subprocess.Popen", return_value=process), patch("tools.stems.worker.selectors.DefaultSelector", return_value=selector):
            with self.assertRaisesRegex(RuntimeError, "original setup failure") as caught:
                exporter.export_f32(self.source, self.destination, self.runtime, self.model,
                                    _worker_factory=factory)
        self.assertFalse(self.destination.exists())
        self.assertTrue((self.root / "raw.stage" / "native" / "runtime.stderr").is_file())
        self.assertTrue(self.source.exists())
        self.assertTrue(any("cleanup unproven" in note.lower() for note in caught.exception.__notes__))
        process.wait.assert_called_once_with(timeout=2)
        self.assertTrue(process.stderr.closed)

    def test_cleanup_failure_preserves_primary_error_and_private_stage(self):
        def configure(worker):
            worker.mutate = lambda events: events[3].__setitem__("model_id", "bad")
            worker.on_close = lambda: (_ for _ in ()).throw(TimeoutError("cleanup failed"))
        self.configure = configure
        with self.assertRaisesRegex(RuntimeError, "invalid or noncontiguous") as caught:
            self.run_export()
        self.assertTrue((self.root / "raw.stage").is_dir())
        self.assertFalse(self.destination.exists())
        self.assertTrue(any("retain caller input" in note for note in caught.exception.__notes__))
        self.assertTrue(self.source.exists())


    def test_cancel_owns_and_stops_real_supervisor_with_blocked_backend(self):
        entered, release, cancel = threading.Event(), threading.Event(), threading.Event()
        class Backend:
            closed = False
            aborted = False
            def start(self):
                return ready()
            def infer(self, samples, request_id):
                entered.set()
                if not release.wait(8):
                    raise TimeoutError("test backend did not unblock")
                raise RuntimeError("test backend stopped")
            def abort(self):
                self.aborted = True
                release.set()
            def close(self):
                self.closed = True
        backend = Backend()
        workers, failures = [], []
        def factory(*args, **kwargs):
            kwargs["_backend_factory"] = lambda *unused: backend
            worker = StemWorker(*args, **kwargs)
            workers.append(worker)
            return worker
        def run():
            try:
                exporter.export_f32(self.source, self.destination, self.runtime, self.model,
                                    cancel=cancel, _worker_factory=factory)
            except BaseException as error:
                failures.append(error)
        thread = threading.Thread(target=run)
        thread.start()
        try:
            self.assertTrue(entered.wait(3))
            cancel.set()
            thread.join(6)
            self.assertFalse(thread.is_alive())
            self.assertEqual(len(failures), 1)
            self.assertIsInstance(failures[0], exporter.ExportCancelled)
            self.assertTrue(backend.aborted)
            self.assertTrue(backend.closed)
            self.assertFalse(workers[0]._thread.is_alive())
            self.assertTrue(workers[0].statistics()["closed"])
            self.assert_unpublished()
        finally:
            cancel.set()
            release.set()
            thread.join(6)
            for worker in workers:
                worker.close()

    def test_failed_cleanup_stops_supervisor_instead_of_reusing_native(self):
        entered = threading.Event()
        class Backend:
            calls = 0
            fail_close = True
            def start(self):
                return ready()
            def infer(self, samples, request_id):
                self.calls += 1
                entered.set()
                raise RuntimeError("injected inference failure")
            def abort(self):
                pass
            def close(self):
                if self.fail_close:
                    raise TimeoutError("injected cleanup failure")
        backend = Backend()
        worker = StemWorker(self.runtime, self.model, _backend_factory=lambda *unused: backend)
        try:
            worker.load_f32(self.source)
            self.assertTrue(entered.wait(3))
            worker._thread.join(3)
            self.assertFalse(worker._thread.is_alive())
            self.assertTrue(worker.statistics()["closed"])
            self.assertIs(worker._native, backend)
            with self.assertRaisesRegex(RuntimeError, "closed"):
                worker.seek(0)
            self.assertEqual(backend.calls, 1)
        finally:
            backend.fail_close = False
            try:
                worker.close()
            except TimeoutError:
                pass
            worker.close()

    def test_supervisor_close_failure_cannot_publish_and_retains_retry_owner(self):
        class Backend:
            fail_close = True
            def start(self):
                return ready()
            def infer(self, samples, request_id):
                return np.repeat(samples[None], 4, axis=0), {"id": request_id}
            def abort(self):
                pass
            def close(self):
                if self.fail_close:
                    raise TimeoutError("native cleanup wait failed")
        backend = Backend()
        workers = []
        def factory(*args, **kwargs):
            kwargs["_backend_factory"] = lambda *unused: backend
            worker = StemWorker(*args, **kwargs)
            workers.append(worker)
            return worker
        try:
            with self.assertRaisesRegex(TimeoutError, "native cleanup wait failed") as caught:
                exporter.export_f32(self.source, self.destination, self.runtime, self.model,
                                    _worker_factory=factory)
            self.assertFalse(self.destination.exists())
            self.assertTrue((self.root / "raw.stage").is_dir())
            self.assertTrue(self.source.exists())
            self.assertIs(workers[0]._native, backend)
            self.assertFalse(workers[0]._thread.is_alive())
            self.assertTrue(any("retain caller input" in note for note in caught.exception.__notes__))
        finally:
            backend.fail_close = False
            for worker in workers:
                try:
                    worker.close()
                except TimeoutError:
                    pass  # First retry reports the earlier failure after reaping.
                worker.close()
                self.assertIsNone(worker._native)

    def test_directory_sync_order_and_postrename_failure_keep_complete_bank(self):
        actual_sync = exporter._sync_directory
        seen = []
        def sync(path):
            seen.append(path)
            return actual_sync(path)
        with patch.object(exporter, "_sync_directory", side_effect=sync):
            self.run_export()
        self.assertEqual(seen, [self.root / "raw.stage", self.root])
        import shutil
        shutil.rmtree(self.destination)
        with patch.object(exporter, "_sync_directory", side_effect=[None, OSError("parent sync failed")]):
            with self.assertRaisesRegex(OSError, "parent sync failed"):
                self.run_export()
        self.assertTrue((self.destination / "manifest.json").is_file())
        self.assertEqual(len(list(self.destination.glob("*.f32"))), 4)
        self.assertFalse((self.root / "raw.stage").exists())

class NativeBoundedDiagnosticsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.model = self.root / "model"
        self.model.write_bytes(b"fake")

    def runtime(self, body, timeout=1):
        script = self.root / "child"
        script.write_text("#!" + sys.executable + "\n" + body)
        script.chmod(0o700)
        runtime = NativeRuntime(script, self.model, "cpu", 2, timeout,
                                spool_directory=self.root / "native", max_stderr_bytes=4096)
        self.addCleanup(runtime.close)
        return runtime

    def test_noisy_stderr_does_not_block_stdout_and_storage_is_capped(self):
        body = ("import os,json,time\n"
                "for unused in range(64): os.write(2,b'x'*4096)\n"
                "print(" + repr(json.dumps(ready("cpu"))) + ",flush=True)\n"
                "time.sleep(10)\n")
        runtime = self.runtime(body)
        self.assertEqual(runtime.start()["event"], "ready")
        self.assertLessEqual((runtime.root / "runtime.stderr").stat().st_size, 4096)
        self.assertLessEqual(len(runtime._stderr_tail), 4096)
        self.assertEqual(runtime.root, self.root / "native")
        pid = runtime.process.pid
        runtime.close()
        self.assertIsNotNone(runtime.process.returncode)
        self.assertTrue(runtime.process.stderr.closed)
        self.assertFalse(runtime.root.exists())
        with self.assertRaises(ChildProcessError):
            os.waitpid(pid, os.WNOHANG)

    def test_continuous_stderr_cannot_extend_absolute_request_deadline(self):
        runtime = self.runtime("import os\nwhile True: os.write(2,b'x'*4096)\n", timeout=0.05)
        failure = []
        def receive():
            try:
                runtime.start()
            except BaseException as error:
                failure.append(error)
        thread = threading.Thread(target=receive)
        thread.start()
        thread.join(2)
        if thread.is_alive():
            runtime.abort()
            thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(failure), 1)
        self.assertIsInstance(failure[0], TimeoutError)
        self.assertLessEqual((runtime.root / "runtime.stderr").stat().st_size, 4096)

    def test_failed_native_wait_preserves_fixed_spool_and_close_can_retry(self):
        process = Mock()
        process.stdout, process.stdin, process.stderr = io.BytesIO(), io.BytesIO(), io.BytesIO()
        process.poll.return_value = None
        process.wait.side_effect = subprocess.TimeoutExpired("injected native wait", 2)
        selector = Mock()
        with patch("tools.stems.worker.subprocess.Popen", return_value=process), patch("tools.stems.worker.selectors.DefaultSelector", return_value=selector):
            runtime = NativeRuntime("unused", self.model, "cpu", 2, 1,
                                    spool_directory=self.root / "native", max_stderr_bytes=4096)
        with self.assertRaises(subprocess.TimeoutExpired):
            runtime.close()
        self.assertTrue(runtime.root.is_dir())
        self.assertTrue((runtime.root / "runtime.stderr").is_file())
        self.assertFalse(runtime._process_reaped)
        process.wait.side_effect = None
        process.wait.return_value = -9
        runtime.close()
        runtime.close()
        self.assertTrue(runtime._process_reaped)
        self.assertFalse(runtime.root.exists())

    def test_invalid_cap_does_not_spawn_and_second_registration_failure_reaps(self):
        for cap in (True, 0, -1, 65537, 3.5):
            with self.subTest(cap=cap), patch("tools.stems.worker.subprocess.Popen") as spawn:
                with self.assertRaises(ValueError):
                    NativeRuntime("unused", self.model, "cpu", 2, 1, max_stderr_bytes=cap)
                spawn.assert_not_called()
        process = Mock()
        process.stdout = io.BytesIO()
        process.stdin = io.BytesIO()
        process.stderr = io.BytesIO()
        process.poll.return_value = None
        selector = Mock()
        selector.register.side_effect = [None, RuntimeError("second registration failed")]
        with patch("tools.stems.worker.subprocess.Popen", return_value=process), patch("tools.stems.worker.selectors.DefaultSelector", return_value=selector):
            with self.assertRaisesRegex(RuntimeError, "second registration failed"):
                NativeRuntime("unused", self.model, "cpu", 2, 1,
                              spool_directory=self.root / "native", max_stderr_bytes=4096)
        process.kill.assert_called_once()
        process.wait.assert_called_once_with(timeout=2)
        self.assertTrue(process.stderr.closed)
        self.assertTrue(process.stdout.closed)
        self.assertTrue(process.stdin.closed)
        self.assertFalse((self.root / "native").exists())


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Persistent service protocol/lease failures; fake workers and native process only."""

from collections import deque
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.stems import service
from tools.stems.worker import CHANNELS, HOP, RATE, STEMS, WINDOW


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_source(path, frames):
    numbers = np.arange(frames, dtype=np.int64)
    audio = np.empty((frames, 2), dtype="<f4")
    audio[:, 0] = 2.5 + ((numbers * 7) % 31 - 15) / 64
    audio[:, 1] = -1.75 + ((numbers * 11) % 37 - 18) / 32
    audio.tofile(path)
    return audio


def ready(backend, threads):
    return {"event": "ready", "sample_rate": RATE, "channels": CHANNELS,
            "segment_frames": WINDOW, "stems": list(STEMS), "backend": backend,
            "threads": threads, "revision": "a" * 40, "weight_storage": "f32",
            "fence_wait_mode": "blocking", "pid": 123, "load_ms": 0.1}


class FakeWorker:
    """Only one generated chunk is materialized by poll; controls stay immediate."""
    def __init__(self, runtime, model, *, backend, threads, **options):
        self.model = Path(model)
        self.backend, self.threads, self.options = backend, threads, options
        self.generation = 0
        self.request_id = 0
        self.position = 0
        self.events = deque()
        self.load_calls = self.poll_calls = self.close_calls = 0
        self.closed = self.active = self.started = False
        self.allow_data = threading.Event()
        self.allow_data.set()
        self.mutate = None
        self.on_close = None
        self.on_load = None
        self.controls = []

    def _begin(self, command, frame):
        pending_runtime = next((event for event in self.events if event["event"] == "runtime_ready"), None)
        self.generation += 1
        self.position = frame
        self.active = command != "cancel"
        self.controls.append((command, frame, self.generation))
        self.events.clear()
        if pending_runtime is not None:
            self.events.append(pending_runtime)
        elif not self.started and self.active:
            self.events.append({"event": "runtime_ready", "generation": self.generation,
                                "model_id": digest(self.model), "runtime": ready(self.backend, self.threads)})
            self.started = True
        if self.active:
            self.events.append({"event": "source_ready", "generation": self.generation,
                                "source_id": self.source_id, "source_frames": len(self.audio),
                                "source_format": "f32le", "mean": 0.1, "std": 2.0})
        self.completed = False
        self.chunk_pending = None
        return self.generation

    def load_f32(self, source, start_frame=0):
        self.load_calls += 1
        self.source = Path(source)
        self.source_id = digest(source)
        self.audio = np.fromfile(source, dtype="<f4").reshape(-1, 2)
        generation = self._begin("load", start_frame)
        if self.on_load:
            self.on_load(self)
        return generation

    def seek(self, frame):
        return self._begin("seek", frame)

    def cancel(self):
        return self._begin("cancel", 0)

    def _changed(self, event):
        if self.mutate:
            self.mutate(event)
        return event

    def poll(self, timeout=0):
        self.poll_calls += 1
        if self.events:
            return self._changed(self.events.popleft())
        if not self.active or self.closed or not self.allow_data.is_set():
            return None
        if self.chunk_pending is not None:
            result, self.chunk_pending = self.chunk_pending, None
            return self._changed(result)
        if self.position == len(self.audio):
            if self.completed:
                return None
            self.completed = True
            return self._changed({"event": "complete", "generation": self.generation,
                                  "source_id": self.source_id, "frame_start": self.position})
        first = self.position
        count = min(HOP - first % HOP, len(self.audio) - first)
        self.position += count
        self.request_id += 1
        self.chunk_pending = {"event": "chunk", "generation": self.generation,
            "source_id": self.source_id, "source_frames": len(self.audio), "model_id": digest(self.model),
            "source_format": "f32le", "sample_rate": RATE, "channels": CHANNELS, "stems": STEMS,
            "frame_start": first, "window_start_frame": first // HOP * HOP, "frames": count,
            "request_id": self.request_id, "timestamp": time.monotonic(), "ready_at": time.monotonic(),
            "samples": np.stack([self.audio[first:first + count] * np.float32(index + 1) for index in range(4)]),
            "request_ms": 0.1, "hop_service_ms": 0.2, "metrics": {"not_forwarded": "private native details"}}
        return self._changed({"event": "started", "generation": self.generation,
            "source_id": self.source_id, "model_id": digest(self.model), "request_id": self.request_id,
            "window_start_frame": first // HOP * HOP, "preroll": False, "timestamp": time.monotonic()})

    def is_current(self, event):
        return self.active and event.get("generation") == self.generation and event.get("source_id") == self.source_id

    def statistics(self):
        return {"telemetry_dropped": 0, "ready_count": 0, "max_ready": 1, "backend_pid": None}

    def close(self, timeout=2):
        self.close_calls += 1
        if self.on_close:
            self.on_close()
        self.closed = True


class Harness:
    def __init__(self, case, frames=17, configure=None, limits=None, prefill_output=False):
        self.case = case
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.directory = self.root / "service"
        self.source, self.runtime, self.model = (self.root / name for name in ("input.f32", "runtime", "model"))
        self.audio = write_source(self.source, frames)
        self.runtime.write_bytes(b"fake runtime identity\n")
        self.model.write_bytes(b"fake model identity\n")
        self.stop = threading.Event()
        self.factory_created = threading.Event()
        self.workers = []
        self.error = None
        self.result = object()
        self.buffer = bytearray()
        self.events = []
        self.next_id = 1
        self.input_read, self.input_write = os.pipe()
        self.output_read, self.output_write = os.pipe()
        os.set_blocking(self.input_write, False)
        os.set_blocking(self.output_read, False)
        if prefill_output:
            os.set_blocking(self.output_write, False)
            try:
                while True:
                    os.write(self.output_write, b"x" * 4096)
            except BlockingIOError:
                pass

        def factory(*args, **kwargs):
            worker = FakeWorker(*args, **kwargs)
            self.workers.append(worker)
            self.factory_created.set()
            if configure:
                configure(worker)
            return worker

        def run():
            try:
                self.result = service.serve_f32(self.source, self.directory, self.runtime, self.model,
                    limits=limits or replace(service.ServiceLimits(), max_seconds=15), stop=self.stop,
                    input_fd=self.input_read, output_fd=self.output_write, _worker_factory=factory)
            except BaseException as error:
                self.error = error
            finally:
                os.close(self.output_write)
                self.output_write = -1

        self.thread = threading.Thread(target=run, daemon=True)
        case.addCleanup(self.cleanup)

    def start(self):
        self.thread.start()
        return self

    def raw(self, wire):
        done = 0
        deadline = time.monotonic() + 5
        while done < len(wire):
            self.case.assertLess(time.monotonic(), deadline, "Service stopped reading bounded commands")
            _, writable, _ = select.select([], [self.input_write], [], 0.05)
            if writable:
                try:
                    done += os.write(self.input_write, wire[done:])
                except BlockingIOError:
                    pass

    def send(self, command, **fields):
        value = {"v": 1, "id": self.next_id, "command": command, **fields}
        self.next_id += 1
        self.raw((json.dumps(value, allow_nan=False) + "\n").encode())
        return value["id"]

    def event(self, kind, *, identifier=None, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            for index, event in enumerate(self.events):
                if event.get("event") == kind and (identifier is None or event.get("id") == identifier):
                    return self.events.pop(index)
            while b"\n" in self.buffer:
                line, _, rest = self.buffer.partition(b"\n")
                self.buffer = bytearray(rest)
                self.case.assertLessEqual(len(line) + 1, 16384)
                event = json.loads(line, parse_constant=lambda value: self.case.fail("Nonfinite JSON event: " + value))
                self.case.assertIsInstance(event, dict)
                self.case.assertEqual(event.get("v"), 1)
                self.events.append(event)
            if any(event.get("event") == kind and (identifier is None or event.get("id") == identifier) for event in self.events):
                continue
            self.case.assertLess(time.monotonic(), deadline, f"Missing {kind} event; service error={self.error!r}")
            readable, _, _ = select.select([self.output_read], [], [], min(0.05, max(0, deadline - time.monotonic())))
            if readable:
                data = os.read(self.output_read, 8192)
                if not data:
                    self.case.fail(f"Service closed before {kind}; error={self.error!r}; retained events={self.events!r}")
                self.buffer.extend(data)

    def ack(self, chunk):
        return self.send("ack", **{key: chunk[key] for key in ("slot", "sequence", "generation", "source_id", "model_id")})

    def finish(self, error=False, timeout=5):
        self.thread.join(timeout)
        self.case.assertFalse(self.thread.is_alive(), "Service did not finish its bounded shutdown")
        if error:
            self.case.assertIsNotNone(self.error)
        else:
            self.case.assertIsNone(self.error)
            self.case.assertIsNone(self.result)
        return self.error

    def remaining(self):
        self.case.assertFalse(self.thread.is_alive())
        while True:
            data = os.read(self.output_read, 8192)
            if not data:
                break
            self.buffer.extend(data)
        while b"\n" in self.buffer:
            line, _, rest = self.buffer.partition(b"\n")
            self.buffer = bytearray(rest)
            self.case.assertLessEqual(len(line) + 1, 16384)
            event = json.loads(line, parse_constant=lambda value: self.case.fail("Nonfinite terminal JSON: " + value))
            self.case.assertIsInstance(event, dict)
            self.case.assertEqual(event.get("v"), 1)
            self.events.append(event)
        self.case.assertFalse(self.buffer, "Service left a truncated terminal protocol event")
        return self.events

    def close_clean(self):
        identifier = self.send("close")
        self.event("accepted", identifier=identifier)
        closed = self.event("closed")
        self.case.assertTrue(closed["worker_closed"])
        self.finish()
        return closed

    def cleanup(self):
        self.stop.set()
        if self.input_write >= 0:
            os.close(self.input_write)
            self.input_write = -1
        if self.thread.ident:
            self.thread.join(6)
            self.case.assertFalse(self.thread.is_alive(), "A service test left its supervisor thread running")
        for name in ("input_read", "output_read", "output_write"):
            descriptor = getattr(self, name)
            if descriptor >= 0:
                os.close(descriptor)
                setattr(self, name, -1)
        self.temp.cleanup()


class ServiceTest(unittest.TestCase):
    def assert_chunk(self, harness, event, first, count, final):
        self.assertEqual(event["frame_start"], first)
        self.assertEqual(event["window_start_frame"], first // HOP * HOP)
        self.assertEqual(event["frames"], count)
        self.assertEqual(event["final"], final)
        self.assertEqual(event["source_id"], digest(harness.source))
        self.assertEqual(event["model_id"], digest(harness.model))
        self.assertEqual(event["source_frames"], len(harness.audio))
        self.assertEqual((event["source_format"], event["sample_rate"], event["channels"]), ("f32le", RATE, 2))
        self.assertEqual(event["stems"], list(STEMS))
        self.assertIn(event["slot"], (0, 1))
        self.assertEqual(event["file"], f"slot{event['slot']}.f32")
        path = harness.directory / event["file"]
        self.assertTrue(stat.S_ISREG(path.lstat().st_mode))
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        expected = np.stack([harness.audio[first:first + count] * np.float32(index + 1) for index in range(4)])
        self.assertEqual(path.read_bytes(), expected.astype("<f4", copy=False).tobytes())
        self.assertEqual(event["bytes"], count * 2 * 4 * 4)
        self.assertEqual(event["sha256"], digest(path))
        self.assertNotIn("metrics", event)
        self.assertNotIn("samples", event)
        self.assertLessEqual(path.stat().st_size, 8255520)
        return path

    def test_ready_pins_input_without_inference_and_close_is_explicit(self):
        harness = Harness(self).start()
        initial = harness.event("ready")
        self.assertEqual(initial["generation"], 0)
        self.assertEqual(initial["source"], {"format": "f32le", "sample_rate": RATE, "channels": 2,
            "frames": len(harness.audio), "bytes": harness.source.stat().st_size, "sha256": digest(harness.source)})
        self.assertEqual(initial["model"], {"sha256": digest(harness.model), "bytes": harness.model.stat().st_size})
        self.assertEqual(initial["runtime"], {"sha256": digest(harness.runtime), "bytes": harness.runtime.stat().st_size})
        self.assertEqual(initial["describe"]["service_sha256"], digest(service.__file__))
        self.assertEqual(harness.workers[0].load_calls, 0)
        self.assertEqual(harness.workers[0].options["max_ready"], 1)
        closed = harness.close_clean()
        self.assertEqual(closed["reason"], "close")
        self.assertEqual(closed["outstanding_slots"], 0)
        self.assertTrue(harness.workers[0].closed)
        self.assertFalse((harness.directory / "native").exists())

    def test_exact_trimmed_seek_tail_and_role_major_headroom(self):
        harness = Harness(self, HOP + 3).start()
        harness.event("ready")
        identifier = harness.send("load", frame=HOP - 2)
        accepted = harness.event("accepted", identifier=identifier)
        self.assertEqual(accepted["frame"], HOP - 2)
        runtime = harness.event("runtime")
        self.assertEqual(runtime["ready"]["weight_storage"], "f32")
        self.assertEqual(runtime["ready"]["fence_wait_mode"], "blocking")
        source = harness.event("source")
        self.assertEqual(source["generation"], accepted["generation"])
        chunks = [harness.event("chunk"), harness.event("chunk")]
        self.assertEqual([chunk["sequence"] for chunk in chunks], [1, 2])
        self.assert_chunk(harness, chunks[0], HOP - 2, 2, False)
        self.assert_chunk(harness, chunks[1], HOP, 3, True)
        for chunk in chunks:
            harness.event("accepted", identifier=harness.ack(chunk))
        complete = harness.event("complete")
        self.assertEqual(complete["frame_start"], HOP + 3)
        self.assertEqual(complete["generation"], accepted["generation"])
        self.assertEqual(complete["telemetry_dropped"], 0)
        harness.close_clean()

    def test_full_leases_survive_cancel_seek_and_old_ack_cannot_free_reuse(self):
        harness = Harness(self, 2 * HOP + 5).start()
        harness.event("ready")
        accepted = harness.event("accepted", identifier=harness.send("load", frame=0))
        first, second = harness.event("chunk"), harness.event("chunk")
        first_path = self.assert_chunk(harness, first, 0, HOP, False)
        second_path = self.assert_chunk(harness, second, HOP, HOP, False)
        old_bytes = {first_path: first_path.read_bytes(), second_path: second_path.read_bytes()}
        polled_at_full = harness.workers[0].poll_calls
        cancelled = harness.event("accepted", identifier=harness.send("cancel"))
        sought = harness.event("accepted", identifier=harness.send("seek", frame=11))
        self.assertGreater(sought["generation"], cancelled["generation"])
        self.assertGreater(cancelled["generation"], accepted["generation"])
        self.assertEqual(harness.workers[0].poll_calls, polled_at_full)
        for path, content in old_bytes.items():
            self.assertEqual(path.read_bytes(), content)
        released = harness.event("accepted", identifier=harness.ack(first))
        self.assertEqual(released["generation"], first["generation"])
        self.assertEqual(released["current_generation"], sought["generation"])
        reused = harness.event("chunk")
        self.assertEqual(reused["slot"], first["slot"])
        self.assertGreater(reused["sequence"], second["sequence"])
        self.assertEqual(reused["generation"], sought["generation"])
        new_path = self.assert_chunk(harness, reused, 11, HOP - 11, False)
        new_bytes = new_path.read_bytes()
        self.assertEqual(second_path.read_bytes(), old_bytes[second_path])
        harness.ack(first)  # Same old lease after slot reuse: terminal, no release.
        harness.finish(error=True)
        self.assertEqual(new_path.read_bytes(), new_bytes)
        self.assertEqual(second_path.read_bytes(), old_bytes[second_path])
        self.assertTrue(harness.workers[0].closed)

    def test_ack_bool_values_cannot_alias_live_integer_lease_ids(self):
        for key, value in (("slot", False), ("sequence", True), ("generation", True)):
            with self.subTest(key=key):
                harness = Harness(self).start()
                harness.event("ready")
                harness.event("accepted", identifier=harness.send("load", frame=0))
                chunk = harness.event("chunk")
                path = self.assert_chunk(harness, chunk, 0, 17, True)
                content = path.read_bytes()
                fields = {name: chunk[name] for name in ("slot", "sequence", "generation", "source_id", "model_id")}
                fields[key] = value
                harness.send("ack", **fields)
                harness.finish(error=True)
                self.assertEqual(path.read_bytes(), content)
                self.assertTrue(harness.workers[0].closed)

    def test_controls_continue_during_unavailable_inference_and_eof_has_no_empty_chunk(self):
        harness = Harness(self, 64, configure=lambda worker: worker.allow_data.clear()).start()
        harness.event("ready")
        harness.event("accepted", identifier=harness.send("load", frame=0))
        seek = harness.event("accepted", identifier=harness.send("seek", frame=17))
        cancel = harness.event("accepted", identifier=harness.send("cancel"))
        restart = harness.event("accepted", identifier=harness.send("seek", frame=64))
        self.assertEqual(restart["generation"], seek["generation"] + 2)
        self.assertEqual(cancel["generation"], seek["generation"] + 1)
        harness.workers[0].allow_data.set()
        complete = harness.event("complete")
        self.assertEqual(complete["frame_start"], 64)
        self.assertEqual(complete["generation"], restart["generation"])
        self.assertFalse(any(event.get("event") == "chunk" for event in harness.events))
        self.assertFalse(any(harness.directory.glob("slot*.f32")))
        harness.close_clean()

    def test_batched_latest_seek_preserves_lifetime_runtime_and_only_current_audio(self):
        harness = Harness(self, 64).start()
        harness.event("ready")
        commands = [{"v": 1, "id": 1, "command": "load", "frame": 0},
                    {"v": 1, "id": 2, "command": "seek", "frame": 7},
                    {"v": 1, "id": 3, "command": "seek", "frame": 17}]
        harness.raw(b"".join((json.dumps(command) + "\n").encode() for command in commands))
        for identifier in (1, 2, 3):
            accepted = harness.event("accepted", identifier=identifier)
            self.assertEqual(accepted["generation"], identifier)
        runtime = harness.event("runtime")
        self.assertEqual(runtime["worker_generation"], 1)
        chunk = harness.event("chunk")
        self.assertEqual(chunk["generation"], 3)
        self.assert_chunk(harness, chunk, 17, 47, True)
        harness.next_id = 4
        harness.event("accepted", identifier=harness.ack(chunk))
        harness.close_clean()

    def test_invalid_current_worker_payloads_never_publish(self):
        def change(kind, key, value):
            return lambda event: event.__setitem__(key, value) if event.get("event") == kind else None
        faults = [change("chunk", key, value) for key, value in (
            ("generation", 999),
            ("source_id", "0" * 64), ("model_id", "0" * 64), ("frame_start", 1),
            ("window_start_frame", 1), ("frames", 16), ("channels", True),
            ("stems", tuple(reversed(STEMS))), ("source_frames", 99),
            ("samples", np.ones((4, 17, 2), dtype=np.float64)),
            ("samples", np.full((4, 17, 2), np.inf, dtype=np.float32)))]
        faults += [change("source_ready", "std", float("nan")),
                   change("runtime_ready", "model_id", "0" * 64)]
        for fault in faults:
            with self.subTest(fault=fault):
                harness = Harness(self, configure=lambda worker: setattr(worker, "mutate", fault)).start()
                harness.event("ready")
                harness.event("accepted", identifier=harness.send("load", frame=0))
                harness.finish(error=True)
                self.assertFalse(any(event.get("event") == "chunk" for event in harness.remaining()))
                self.assertFalse(any(harness.directory.glob("slot*.f32")))
                self.assertTrue(harness.workers[0].closed)

    def test_malformed_json_and_command_values_fail_closed(self):
        commands = [b"[]\n", b"{\"v\":1,\"v\":1,\"id\":1,\"command\":\"close\"}\n",
            b'{"v":1,"id":1,"command":"load","frame":NaN}\n',
            b'{"v":1,"id":true,"command":"close"}\n',
            b'{"v":1,"id":1,"command":"load","frame":true}\n',
            b'{"v":1,"id":1,"command":"load","frame":1.5}\n',
            b'{"v":1,"id":1,"command":"load","frame":-1}\n',
            b'{"v":1,"id":1,"command":"load","frame":18}\n',
            b'{"v":2,"id":1,"command":"close"}\n',
            b'{"v":1,"id":9007199254740992,"command":"close"}\n',
            b'{"v":1,"id":1,"command":"load","frame":0,"path":"/forbidden"}\n',
            b'{"v":1,"id":1,"command":"seek","frame":0}\n',
            b"\xff\n", b"x" * 4097 + b"\n"]
        for wire in commands:
            with self.subTest(wire=wire[:100]):
                harness = Harness(self).start()
                harness.event("ready")
                harness.raw(wire)
                harness.finish(error=True)
                self.assertFalse(any(harness.directory.glob("slot*.f32")))
                self.assertTrue(harness.workers[0].closed)

    def test_partial_eof_duplicate_ids_and_command_flood_are_bounded_failures(self):
        for mode in ("partial", "duplicate", "flood"):
            with self.subTest(mode=mode):
                harness = Harness(self, configure=lambda worker: worker.allow_data.clear()).start()
                harness.event("ready")
                if mode == "partial":
                    harness.raw(b'{"v":1')
                    os.close(harness.input_write)
                    harness.input_write = -1
                elif mode == "duplicate":
                    harness.event("accepted", identifier=harness.send("load", frame=0))
                    harness.raw(b'{"v":1,"id":1,"command":"close"}\n')
                else:
                    harness.raw(b"".join((json.dumps({"v": 1, "id": index, "command": "cancel"}) + "\n").encode()
                                         for index in range(1, 20)))
                harness.finish(error=True)
                self.assertTrue(harness.workers[0].closed)
                self.assertFalse(any(harness.directory.glob("slot*.f32")))

    def test_input_eof_stop_event_and_broken_output_close_the_worker(self):
        for reason in ("eof", "signal", "pipe"):
            with self.subTest(reason=reason):
                harness = Harness(self, configure=lambda worker: worker.allow_data.clear()).start()
                harness.event("ready")
                if reason == "pipe":
                    os.close(harness.output_read)
                    harness.output_read = -1
                    harness.send("load", frame=0)
                    harness.finish(error=True)
                else:
                    if reason == "eof":
                        os.close(harness.input_write)
                        harness.input_write = -1
                    else:
                        harness.stop.set()
                    closed = harness.event("closed")
                    self.assertEqual(closed["reason"], reason)
                    self.assertTrue(closed["worker_closed"])
                    harness.finish()
                self.assertTrue(harness.workers[0].closed)

    def test_blocked_output_still_handles_stop_and_bounds_final_drain(self):
        harness = Harness(self, prefill_output=True).start()
        self.assertTrue(harness.factory_created.wait(5), "Blocked-output fixture did not complete preflight")
        harness.stop.set()
        self.assertIsInstance(harness.finish(error=True, timeout=5), TimeoutError)
        self.assertTrue(harness.workers[0].closed)
        self.assertFalse(any(harness.directory.glob("slot*.f32")))

    def test_slow_reader_control_pressure_hits_bounded_output_queue(self):
        harness = Harness(self).start()
        harness.event("ready")
        worker = harness.workers[0]
        sent = 0
        deadline = time.monotonic() + 8
        # At most four unprocessed commands are submitted at once. Thus any
        # overload comes from retained replies, not the input's eight-line cap.
        while harness.thread.is_alive() and sent < 2000:
            commands = [{"v": 1, "id": sent + index + 1, "command": "cancel"} for index in range(4)]
            harness.raw(b"".join((json.dumps(command) + "\n").encode() for command in commands))
            sent += 4
            while len(worker.controls) < sent and harness.thread.is_alive() and time.monotonic() < deadline:
                time.sleep(0.001)
            self.assertLess(time.monotonic(), deadline, "Output pressure did not produce bounded failure")
        error = harness.finish(error=True)
        self.assertIsInstance(error, RuntimeError)
        self.assertIn("output backpressure", str(error))
        self.assertLess(sent, 2000)
        self.assertTrue(worker.closed)
        self.assertEqual(worker.load_calls, 0)
        self.assertFalse(any(harness.directory.glob("slot*.f32")))

    def test_owned_paths_and_reservations_reject_before_worker(self):
        for mode in ("source-symlink", "source-fifo", "model-symlink", "runtime-symlink", "directory", "lock-symlink",
                     "budget", "input-limit"):
            with self.subTest(mode=mode):
                limits = replace(service.ServiceLimits(), **({"max_job_bytes": 1} if mode == "budget" else {"max_input_bytes": 8})) if mode in ("budget", "input-limit") else None
                harness = Harness(self, limits=limits)
                sentinel = harness.root / "sentinel"
                sentinel.write_bytes(b"preserve")
                if mode.endswith("symlink") and mode != "lock-symlink":
                    path = {"source-symlink": harness.source, "model-symlink": harness.model,
                            "runtime-symlink": harness.runtime}[mode]
                    path.unlink(); path.symlink_to(sentinel)
                elif mode == "source-fifo":
                    harness.source.unlink(); os.mkfifo(harness.source)
                elif mode == "directory":
                    harness.directory.mkdir(mode=0o700)
                    (harness.directory / "keep").write_bytes(b"keep")
                elif mode == "lock-symlink":
                    (harness.root / ".service.service.lock").symlink_to(sentinel)
                harness.start().finish(error=True)
                self.assertEqual(harness.workers, [])
                if sentinel.exists():
                    self.assertEqual(sentinel.read_bytes(), b"preserve")

    def test_fixed_slot_collision_and_source_change_never_publish(self):
        for mode in ("slot", "source", "model", "runtime"):
            with self.subTest(mode=mode):
                harness = Harness(self).start()
                harness.event("ready")
                if mode == "slot":
                    target = harness.root / "keep"
                    target.write_bytes(b"caller data")
                    (harness.directory / "slot0.f32").symlink_to(target)
                else:
                    path = {"source": harness.source, "model": harness.model, "runtime": harness.runtime}[mode]
                    with path.open("r+b") as stream:
                        stream.write(np.array([123.0], dtype="<f4").tobytes())
                harness.send("load", frame=0)
                harness.finish(error=True)
                if mode == "slot":
                    self.assertEqual(target.read_bytes(), b"caller data")
                    self.assertTrue((harness.directory / "slot0.f32").is_symlink())
                else:
                    self.assertFalse(any(harness.directory.glob("slot*.f32")))

    def test_invalid_limits_are_rejected_before_worker(self):
        for changes in ({"max_seconds": float("nan")}, {"max_seconds": 7201},
                        {"request_timeout": 31}, {"max_diagnostic_bytes": True},
                        {"max_input_bytes": 256 * 1024**2 + 1}, {"max_job_bytes": 0}):
            with self.subTest(changes=changes):
                harness = Harness(self, limits=replace(service.ServiceLimits(), **changes)).start()
                harness.finish(error=True)
                self.assertEqual(harness.workers, [])
                self.assertFalse(harness.directory.exists())

    def test_constructor_failure_with_native_entry_preserves_unproven_owner(self):
        holder = {}
        def construct(worker):
            directory = holder["harness"].directory / "native"
            directory.mkdir()
            (directory / "runtime.stderr").write_bytes(b"partial native owner")
            raise RuntimeError("fake constructor failed")
        harness = Harness(self, configure=construct)
        holder["harness"] = harness
        harness.start()
        error = harness.finish(error=True)
        self.assertEqual(str(error), "fake constructor failed")
        self.assertIn("retain", " ".join(getattr(error, "__notes__", ())).lower())
        self.assertTrue((harness.directory / "native" / "runtime.stderr").exists())
        self.assertTrue(harness.source.exists())
        self.assertEqual(harness.workers[0].close_calls, 0)
        self.assertFalse(any(event.get("event") == "closed" for event in harness.remaining()))


class ServiceNativeLifecycleTest(unittest.TestCase):
    def test_cli_signal_reaps_fake_native_in_inherited_process_group(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, model, runtime = (root / name for name in ("input.f32", "model", "runtime.py"))
            write_source(source, 17)
            model.write_bytes(b"bounded fake model")
            runtime.write_text("#!" + sys.executable + "\n" + r'''import argparse, json, os, pathlib, sys
import numpy as np
p = argparse.ArgumentParser()
p.add_argument('--model'); p.add_argument('--backend'); p.add_argument('--threads', type=int)
a = p.parse_args()
start_ticks = pathlib.Path('/proc/self/stat').read_text().split(') ',1)[1].split()[19]
pathlib.Path(__file__).with_suffix('.pid').write_text(json.dumps({'pid':os.getpid(),'start_ticks':start_ticks}))
print(json.dumps({'event':'ready','sample_rate':44100,'channels':2,'segment_frames':343980,
    'stems':['drums','bass','other','vocals'],'backend':a.backend,'threads':a.threads,
    'revision':'a'*40,'weight_storage':'f32','fence_wait_mode':'blocking','pid':os.getpid()}), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    samples = np.fromfile(request['input'], dtype='<f4').reshape(343980,2)
    np.stack([samples * np.float32(index + 1) for index in range(4)]).astype('<f4').tofile(request['output'])
    print(json.dumps({'event':'result','id':request['id'],'frames':343980,'stems':4}), flush=True)
''')
            runtime.chmod(0o700)
            environment = os.environ.copy()
            environment['PYTHONDONTWRITEBYTECODE'] = '1'
            child = subprocess.Popen([sys.executable, '-B', str(Path(service.__file__)), '--input', str(source),
                '--directory', str(root / 'service'), '--runtime', str(runtime), '--model', str(model),
                '--backend', 'cpu', '--threads', '2', '--max-seconds', '15'],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                start_new_session=True, bufsize=0, env=environment)
            native_pid = None
            buffer = bytearray()
            events = []
            def event(kind):
                deadline = time.monotonic() + 8
                while True:
                    for index, value in enumerate(events):
                        if value.get('event') == kind:
                            return events.pop(index)
                    while b'\n' in buffer:
                        line, _, rest = buffer.partition(b'\n')
                        buffer[:] = rest
                        self.assertLessEqual(len(line) + 1, 16384)
                        events.append(json.loads(line))
                    if any(value.get('event') == kind for value in events):
                        continue
                    self.assertLess(time.monotonic(), deadline, 'Fake-native service did not make bounded progress')
                    readable, _, _ = select.select([child.stdout], [], [], 0.05)
                    if readable:
                        data = os.read(child.stdout.fileno(), 8192)
                        self.assertTrue(data, f'Service exited before {kind}')
                        buffer.extend(data)
            try:
                event('ready')
                wire = b'{"v":1,"id":1,"command":"load","frame":0}\n'
                self.assertEqual(child.stdin.write(wire), len(wire))
                event('accepted')
                event('runtime')
                chunk = event('chunk')
                self.assertEqual(chunk['frames'], 17)
                native_pid = int(json.loads(runtime.with_suffix('.pid').read_text())['pid'])
                self.assertEqual(os.getpgid(native_pid), child.pid,
                                 'Service/native runtime escaped the caller-owned process group')
                os.kill(child.pid, signal.SIGTERM)
                closed = event('closed')
                self.assertEqual(closed['reason'], 'signal')
                self.assertTrue(closed['worker_closed'])
                self.assertEqual(closed['outstanding_slots'], 1)
                self.assertEqual(child.wait(timeout=5), 130)
                with self.assertRaises(ProcessLookupError):
                    os.kill(native_pid, 0)
                self.assertFalse((root / 'service' / 'native').exists())
                self.assertTrue((root / 'service' / chunk['file']).is_file(),
                                'Close must not silently ACK a caller-held immutable slot')
                self.assertLessEqual(len(child.stderr.read(65537)), 65536)
            finally:
                # Give the real supervisor its normal bounded close/reap path
                # even when an earlier protocol or process-group assertion fails.
                if child.poll() is None:
                    try:
                        child.terminate()
                    except ProcessLookupError:
                        pass
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pass
                # A regression may place the fake native outside that group.
                # Its own fixture marker binds the fallback to this PID's start
                # identity; never kill a later process which reused its number.
                try:
                    marker = json.loads(runtime.with_suffix('.pid').read_text())
                    pid = int(marker['pid'])
                    current = Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1].split()[19]
                    if pid > 1 and current == marker['start_ticks']:
                        os.kill(pid, signal.SIGKILL)
                except (OSError, ValueError, KeyError, IndexError):
                    pass
                # Test owns this new session, including after its leader exits.
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait(timeout=5)
                for stream in (child.stdin, child.stdout, child.stderr):
                    stream.close()

    def test_cleanup_failure_retains_native_and_original_protocol_error(self):
        def configure(worker):
            def fail_close():
                native = worker.service_directory / "native"
                native.mkdir(exist_ok=True)
                (native / "runtime.stderr").write_bytes(b"unproven fake native cleanup")
                raise TimeoutError("fake native wait failed")
            worker.on_close = fail_close
        harness = Harness(self, configure=configure).start()
        harness.event("ready")
        harness.workers[0].service_directory = harness.directory
        harness.raw(b'{"v":1,"id":1,"command":"forbidden"}\n')
        error = harness.finish(error=True)
        self.assertNotIsInstance(error, TimeoutError)
        self.assertIn("retain", " ".join(getattr(error, "__notes__", ())).lower())
        self.assertTrue((harness.directory / "native" / "runtime.stderr").exists())
        self.assertTrue(harness.source.exists())
        self.assertFalse(any(event.get("event") == "closed" for event in harness.remaining()))


if __name__ == "__main__":
    unittest.main()

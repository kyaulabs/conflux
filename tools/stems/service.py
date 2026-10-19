#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded local persistent stem chunks. Optional tool; never an audio callback.

The parent owns the process session and retains its input/job until exit and
independent group-reap proof. NativeRuntime inherits that session. Publication
and file-change checks have the exporter's cooperative same-user scope.
"""

from contextlib import ExitStack
from dataclasses import asdict, dataclass
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import selectors
import stat
import sys
import time

import numpy as np

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.stems import export as exporter
from tools.stems.worker import CHANNELS, HOP, RATE, STEMS, NativeRuntime, StemWorker

VERSION = 1
MAX_INTEGER = 2**53 - 1
COMMAND_BYTES = 4096
INPUT_BYTES = 8192
EVENT_BYTES = 16384
OUTPUT_BYTES = 65536
SLOT_BYTES = len(STEMS) * HOP * CHANNELS * 4
NATIVE_TIMINGS = ("elapsed_ms", "inference_wall_ms", "inference_cpu_ms", "frontend_ms", "graph_ms",
                  "graph_upload_ms", "graph_compute_ms", "graph_readback_ms", "postprocess_ms", "graph_rebuild_ms")


@dataclass(frozen=True)
class ServiceLimits:
    max_input_bytes: int = exporter.MAX_INPUT_BYTES
    max_job_bytes: int = exporter.MAX_JOB_BYTES
    max_seconds: float = 7200
    request_timeout: float = 30
    max_diagnostic_bytes: int = exporter.MAX_DIAGNOSTIC_BYTES

    def validate(self):
        for name, maximum in (("max_input_bytes", exporter.MAX_INPUT_BYTES),
                              ("max_job_bytes", exporter.MAX_JOB_BYTES),
                              ("max_diagnostic_bytes", exporter.MAX_DIAGNOSTIC_BYTES)):
            value = getattr(self, name)
            if type(value) is not int or not 0 < value <= maximum:
                raise ValueError("invalid service limit: " + name)
        for name, maximum in (("max_seconds", 7200), ("request_timeout", 30)):
            value = getattr(self, name)
            if (isinstance(value, bool) or not isinstance(value, (int, float))
                    or not math.isfinite(value) or not 0 < value <= maximum):
                raise ValueError("invalid service limit: " + name)


def reservation_bytes(input_bytes, max_diagnostic_bytes=exporter.MAX_DIAGNOSTIC_BYTES):
    if type(input_bytes) is not int or input_bytes <= 0 or input_bytes % (CHANNELS * 4):
        raise ValueError("input must contain complete nonempty stereo F32LE frames")
    if type(max_diagnostic_bytes) is not int or not 0 < max_diagnostic_bytes <= exporter.MAX_DIAGNOSTIC_BYTES:
        raise ValueError("invalid diagnostic byte limit")
    return input_bytes + 2 * SLOT_BYTES + exporter.NATIVE_SPOOL_BYTES + max_diagnostic_bytes


def describe():
    result = exporter.describe()
    with ExitStack() as stack:
        result["service_sha256"] = exporter._PinnedFile(
            Path(__file__), 1024 * 1024, stack, None, time.monotonic() + 5).digest
    return result


def _integer(value, minimum=0):
    return type(value) is int and minimum <= value <= MAX_INTEGER


def _finite(value):
    return (not isinstance(value, bool) and isinstance(value, (int, float))
            and math.isfinite(value) and value >= 0)


def _unique(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate service command field")
        value[key] = item
    return value


def _nonfinite(value):
    raise ValueError("non-finite service JSON number")


class _Wire:
    """This service's bounded, fair pipe exchange; no pending command queue."""

    def __init__(self, input_fd, output_fd, stack):
        if input_fd == output_fd:
            raise ValueError("service requires separate input/output pipes")
        self.input_fd, self.output_fd = input_fd, output_fd
        self.selector = selectors.DefaultSelector()
        stack.callback(self.selector.close)
        for fd in (input_fd, output_fd):
            blocking = os.get_blocking(fd)
            stack.callback(os.set_blocking, fd, blocking)
            os.set_blocking(fd, False)
        self.selector.register(input_fd, selectors.EVENT_READ)
        self.input = bytearray()
        self.output = bytearray()
        self.output_registered = False
        self.eof = False

    def emit(self, event):
        wire = (json.dumps({"v": VERSION, **event}, sort_keys=True,
                           separators=(",", ":"), allow_nan=False) + "\n").encode()
        if len(wire) > EVENT_BYTES or len(self.output) + len(wire) > OUTPUT_BYTES:
            raise RuntimeError("service output backpressure exceeds bounded queue")
        self.output.extend(wire)
        if not self.output_registered:
            self.selector.register(self.output_fd, selectors.EVENT_WRITE)
            self.output_registered = True

    def _write(self):
        try:
            count = os.write(self.output_fd, self.output[:4096])
        except BlockingIOError:
            return
        if count <= 0:
            raise BrokenPipeError("service output made no progress")
        del self.output[:count]
        if not self.output:
            self.selector.unregister(self.output_fd)
            self.output_registered = False

    def step(self, timeout):
        for key, _ in self.selector.select(timeout):
            if key.fd == self.output_fd:
                self._write()
            else:
                space = min(4096, INPUT_BYTES - len(self.input))
                if space <= 0:
                    raise ValueError("service input buffer exceeds limit")
                try:
                    data = os.read(self.input_fd, space)
                except BlockingIOError:
                    continue
                if data:
                    self.input.extend(data)
                else:
                    self.eof = True
                    self.selector.unregister(self.input_fd)

    def commands(self):
        if self.input.count(b"\n") > 8:
            raise ValueError("service pending commands exceed limit")
        for _ in range(8):
            end = self.input.find(b"\n")
            if end < 0:
                if len(self.input) >= COMMAND_BYTES:
                    raise ValueError("service command exceeds byte limit")
                return
            if end + 1 > COMMAND_BYTES:
                raise ValueError("service command exceeds byte limit")
            line = bytes(self.input[:end])
            del self.input[:end + 1]
            value = json.loads(line.decode("utf-8"), object_pairs_hook=_unique,
                               parse_constant=_nonfinite)
            if not isinstance(value, dict):
                raise ValueError("service command requires an object")
            yield value

    def drain(self):
        if not self.eof:
            self.selector.unregister(self.input_fd)
            self.eof = True
        deadline = time.monotonic() + 2
        while self.output:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("service final output drain timed out")
            for key, _ in self.selector.select(remaining):
                if key.fd == self.output_fd:
                    self._write()


class _Session:
    def __init__(self, source, model, runtime, directory, wire, worker, pins):
        self.source, self.model, self.runtime = source, model, runtime
        self.directory, self.wire, self.worker, self.pins = directory, wire, worker, pins
        self.frames = source.size // (CHANNELS * 4)
        self.generation = self.command_id = self.sequence = 0
        self.loaded = self.active = self.source_ready = self.completed = False
        self.runtime_ready = None
        self.next_frame = 0
        self.leases = [None, None]

    def verify_signatures(self):
        for pinned in self.pins:
            if (exporter._signature(pinned.path.lstat()) != pinned.signature
                    or exporter._signature(os.fstat(pinned.fd)) != pinned.signature):
                raise RuntimeError("service source, model or tool changed")

    def drops(self):
        value = self.worker.statistics().get("telemetry_dropped", 0)
        if not _integer(value):
            raise RuntimeError("invalid service worker telemetry count")
        return value

    def command(self, command):
        kind = command.get("command")
        required = {"v", "id", "command"}
        if kind in ("load", "seek"):
            required |= {"frame"}
        elif kind == "ack":
            required |= {"slot", "sequence", "generation", "source_id", "model_id"}
        elif kind not in ("cancel", "close"):
            raise ValueError("unknown service command")
        if (set(command) != required or type(command.get("v")) is not int or command["v"] != VERSION
                or not _integer(command.get("id"), 1) or command["id"] <= self.command_id):
            raise ValueError("invalid service command fields/version/id")
        self.verify_signatures()
        accepted = {"event": "accepted", "id": command["id"], "command": kind,
                    "generation": self.generation, "accepted_at": time.monotonic()}
        if kind in ("load", "seek"):
            frame = command["frame"]
            if not _integer(frame) or frame > self.frames or (kind == "seek" and not self.loaded):
                raise ValueError("service frame is outside the loaded source")
            generation = (self.worker.load_f32(self.source.path, start_frame=frame) if kind == "load"
                          else self.worker.seek(frame))
            self._new_generation(generation)
            self.loaded = self.active = True
            self.next_frame = frame
            self.source_ready = self.completed = False
            accepted.update(generation=generation, frame=frame)
        elif kind == "cancel":
            self._new_generation(self.worker.cancel())
            self.active = self.source_ready = self.completed = False
            accepted["generation"] = self.generation
        elif kind == "ack":
            slot = command["slot"]
            if not _integer(slot) or slot >= 2:
                raise ValueError("invalid service ACK slot")
            lease = self.leases[slot]
            keys = ("slot", "sequence", "generation", "source_id", "model_id")
            if (lease is None or not _integer(command["sequence"], 1)
                    or not _integer(command["generation"], 1)
                    or any(command[key] != lease[key] for key in keys)):
                raise ValueError("service ACK does not match outstanding lease")
            path = self.directory / lease["file"]
            if exporter._signature(path.lstat()) != lease["signature"]:
                raise RuntimeError("service leased file changed")
            path.unlink()
            self.leases[slot] = None
            accepted.update({key: lease[key] for key in keys})
            accepted["current_generation"] = self.generation
        self.command_id = command["id"]
        self.wire.emit(accepted)
        return kind == "close"

    def _new_generation(self, generation):
        if not _integer(generation, 1) or generation <= self.generation:
            raise RuntimeError("invalid service worker generation")
        self.generation = generation

    def poll(self):
        worker_closed = self.worker.statistics().get("closed", False)
        full = all(lease is not None for lease in self.leases)
        if full and not worker_closed:
            return  # Slots remain immutable; controls can still seek/cancel/ACK.
        event = self.worker.poll(0)
        if event is None:
            if self.worker.statistics().get("closed", False):
                raise RuntimeError("service worker stopped unexpectedly")
            return
        if (not isinstance(event, dict) or not _integer(event.get("generation"), 1)
                or event["generation"] > self.generation):
            raise RuntimeError("invalid service worker event generation")
        kind = event.get("event")
        if kind == "runtime_ready":
            if self.runtime_ready is not None:
                raise RuntimeError("service runtime restarted unexpectedly")
            self.runtime_ready = exporter._ready(event, self.model.digest,
                                                  self.worker.backend, self.worker.threads)
            self.wire.emit({"event": "runtime", "worker_generation": event["generation"],
                            "model_id": self.model.digest, "ready": self.runtime_ready})
            return
        if event["generation"] != self.generation or not self.active:
            return
        if kind == "source_ready":
            mean, std = event.get("mean"), event.get("std")
            if (self.runtime_ready is None or self.source_ready
                    or event.get("source_id") != self.source.digest
                    or event.get("source_format") != "f32le"
                    or not _integer(event.get("source_frames"), 1) or event["source_frames"] != self.frames
                    or isinstance(mean, bool) or not isinstance(mean, (int, float)) or not math.isfinite(mean)
                    or not _finite(std) or std <= 0):
                raise RuntimeError("invalid service source identity/normalization")
            self.source_ready = True
            self.wire.emit({"event": "source", "generation": self.generation,
                            "source_id": self.source.digest, "source_frames": self.frames,
                            "source_format": "f32le", "mean": mean, "std": std})
        elif kind == "chunk":
            if full:
                raise RuntimeError("closed service worker retained queued audio")
            self._chunk(event)
        elif kind == "complete":
            if (not self.source_ready or self.completed or not self.worker.is_current(event)
                    or event.get("source_id") != self.source.digest or not _integer(event.get("frame_start"))
                    or event["frame_start"] != self.frames or self.next_frame != self.frames):
                raise RuntimeError("service completed with missing stem audio")
            self.completed = True
            self.wire.emit({"event": "complete", "generation": self.generation,
                            "source_id": self.source.digest, "frame_start": self.frames,
                            "telemetry_dropped": self.drops(), "completed_at": time.monotonic()})
        elif kind == "started":
            window = event.get("window_start_frame")
            if (not self.source_ready or not _integer(event.get("request_id"), 1)
                    or not _integer(window) or window >= self.frames or window % HOP
                    or type(event.get("preroll")) is not bool or not _finite(event.get("timestamp"))
                    or event.get("source_id") != self.source.digest):
                raise RuntimeError("invalid service request telemetry")
            self.wire.emit({"event": "started", "generation": self.generation,
                            "source_id": self.source.digest, "model_id": self.model.digest,
                            **{key: event[key] for key in ("request_id", "window_start_frame", "preroll", "timestamp")}})
        elif kind == "error":
            message = event.get("message")
            raise RuntimeError("stem service worker failed: " + (message[:1024] if isinstance(message, str) else "invalid error"))
        elif kind not in ("control", "discarded"):
            raise RuntimeError("unexpected service worker telemetry")

    def _chunk(self, event):
        count = min(HOP - self.next_frame % HOP, self.frames - self.next_frame)
        samples = event.get("samples")
        if (not self.source_ready or self.completed or not self.worker.is_current(event)
                or event.get("source_id") != self.source.digest or event.get("model_id") != self.model.digest
                or event.get("source_format") != "f32le" or event.get("stems") != STEMS
                or event.get("source_frames") != self.frames or event.get("sample_rate") != RATE
                or event.get("channels") != CHANNELS or event.get("frame_start") != self.next_frame
                or event.get("window_start_frame") != self.next_frame // HOP * HOP
                or event.get("frames") != count or count <= 0
                or any(not _integer(event.get(key)) for key in
                       ("source_frames", "sample_rate", "channels", "frame_start", "window_start_frame", "frames"))
                or not _integer(event.get("request_id"), 1)
                or not isinstance(samples, np.ndarray) or samples.dtype != np.dtype("float32")
                or samples.shape != (len(STEMS), count, CHANNELS) or not np.isfinite(samples).all()
                or not _finite(event.get("ready_at")) or self.sequence == MAX_INTEGER):
            raise RuntimeError("invalid or noncontiguous service stem audio")
        timings = {}
        for key in ("request_ms", "source_read_ms", "overlap_ms", "hop_service_ms"):
            if key in event:
                if not _finite(event[key]):
                    raise RuntimeError("invalid service chunk timing")
                timings[key] = event[key]
        metrics = event.get("metrics", {})
        if not isinstance(metrics, dict):
            raise RuntimeError("invalid native chunk timing object")
        native_timings = {}
        for key in NATIVE_TIMINGS:
            if key in metrics:
                if not _finite(metrics[key]):
                    raise RuntimeError("invalid native chunk timing")
                native_timings["native_" + key] = metrics[key]
        if native_timings:
            # Keep the existing 32-member per-object wire cap unchanged.
            timings["native_timings"] = native_timings
        self.verify_signatures()
        slot = self.leases.index(None)
        name = "slot" + str(slot) + ".f32"
        data = np.asarray(samples, dtype="<f4").tobytes(order="C")
        path = self.directory / name
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as stream:
            if stream.write(data) != len(data):
                raise OSError("incomplete service chunk write")
            stream.flush()
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size != len(data) or len(data) > SLOT_BYTES:
                raise RuntimeError("service chunk size mismatch")
        self.sequence += 1
        lease = {"slot": slot, "sequence": self.sequence, "generation": self.generation,
                 "source_id": self.source.digest, "model_id": self.model.digest,
                 "file": name, "signature": exporter._signature(info)}
        self.leases[slot] = lease
        packet = {"event": "chunk", **{key: value for key, value in lease.items() if key != "signature"},
                  "source_frames": self.frames, "source_format": "f32le", "sample_rate": RATE,
                  "channels": CHANNELS, "stems": list(STEMS), "frame_start": self.next_frame,
                  "window_start_frame": event["window_start_frame"], "frames": count,
                  "final": self.next_frame + count == self.frames, "bytes": len(data),
                  "sha256": hashlib.sha256(data).hexdigest(), "request_id": event["request_id"],
                  "worker_ready_at": event["ready_at"], "published_at": time.monotonic(), **timings}
        self.wire.emit(packet)
        self.next_frame += count


def serve_f32(source_f32, directory, runtime, model, *, backend="vulkan", threads=2,
              limits=ServiceLimits(), stop=None, input_fd=0, output_fd=1, _worker_factory=None):
    """Serve one pinned source until close/EOF/signal; return None after cleanup.

    Exceptions preserve the primary operation failure. Native cleanup failure
    retains the whole caller job. Parent must still prove the owned group exited.
    No raw lease is released implicitly by cancellation, seeking or shutdown.
    """
    limits.validate()
    if backend not in ("cpu", "vulkan") or type(threads) is not int or not 1 <= threads <= 64:
        raise ValueError("invalid service backend/threads")
    deadline = time.monotonic() + limits.max_seconds
    directory = Path(directory).absolute()
    parent = directory.parent
    info = parent.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError("service directory requires an owned private parent")
    worker = session = wire = None
    primary = None
    reason = "error"
    with ExitStack() as stack:
        lock = os.open(parent / ("." + directory.name + ".service.lock"),
                       os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        stack.callback(os.close, lock)
        info = os.fstat(lock)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1
                or stat.S_IMODE(info.st_mode) != 0o600 or info.st_size != 0):
            raise ValueError("unsafe service ownership lock")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        source = exporter._PinnedFile(source_f32, limits.max_input_bytes, stack, stop, deadline)
        reserved = reservation_bytes(source.size, limits.max_diagnostic_bytes)
        if reserved > limits.max_job_bytes:
            raise ValueError("service exceeds whole-job byte reservation")
        model_pin = exporter._PinnedFile(model, exporter.MAX_ASSET_BYTES, stack, stop, deadline)
        runtime_pin = exporter._PinnedFile(runtime, exporter.MAX_ASSET_BYTES, stack, stop, deadline)
        description = describe()
        pins = [source, model_pin, runtime_pin]
        for path in (Path(__file__), Path(exporter.__file__), Path(exporter.worker_module.__file__)):
            pins.append(exporter._PinnedFile(path, 1024 * 1024, stack, stop, deadline))
        if [pinned.digest for pinned in pins[3:]] != [description[key] for key in
                ("service_sha256", "helper_sha256", "worker_sha256")]:
            raise RuntimeError("service helper/worker identity changed")
        directory.mkdir(mode=0o700)  # Never adopt or remove an existing directory.
        try:
            wire = _Wire(input_fd, output_fd, stack)
            native_factory = lambda *args: NativeRuntime(*args, spool_directory=directory / "native",
                                                         max_stderr_bytes=limits.max_diagnostic_bytes)
            worker = (_worker_factory or StemWorker)(runtime_pin.path, model_pin.path, backend=backend,
                threads=threads, max_ready=1, request_timeout=limits.request_timeout, _backend_factory=native_factory)
            session = _Session(source, model_pin, runtime_pin, directory, wire, worker, pins)
            wire.emit({"event": "ready", "generation": 0, "describe": description,
                       "source": {"format": "f32le", "sample_rate": RATE, "channels": CHANNELS,
                                  "frames": session.frames, "bytes": source.size, "sha256": source.digest},
                       "model": {"sha256": model_pin.digest, "bytes": model_pin.size},
                       "runtime": {"sha256": runtime_pin.digest, "bytes": runtime_pin.size},
                       "limits": asdict(limits), "reservation_bytes": reserved})
            while True:
                if stop is not None and stop.is_set():
                    reason = "signal"
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("stem service deadline exceeded")
                session.verify_signatures()
                wire.step(0 if b"\n" in wire.input else min(0.02, max(0, deadline - time.monotonic())))
                closing = False
                for command in wire.commands():
                    if session.command(command):
                        reason, closing = "close", True
                        break
                if closing:
                    break
                if wire.eof and not wire.input:
                    reason = "eof"
                    break
                if wire.eof and b"\n" not in wire.input:
                    raise ValueError("truncated service command at EOF")
                session.poll()
        except BaseException as error:
            primary = error
        finally:
            closed = False
            if worker is not None:
                try:
                    worker.close()
                    closed = True
                except BaseException as cleanup_error:
                    if primary is None:
                        primary = cleanup_error
                    primary.add_note("Worker close failed; retain caller input/job: " + type(cleanup_error).__name__)
            retained = os.path.lexists(directory / "native") or (worker is not None and not closed)
            if retained:
                if primary is None:
                    primary = RuntimeError("native cleanup unproven; retain caller input/job")
                else:
                    primary.add_note("Native cleanup unproven; retain caller input/job")
            if primary is None and reason != "signal":
                try:
                    for pinned in pins:
                        pinned.verify(stop, deadline)
                except BaseException as error:
                    primary = error
                    reason = "error"
            if wire is not None:
                try:
                    if primary is not None:
                        wire.emit({"event": "error", "generation": session.generation if session else 0,
                                   "code": type(primary).__name__, "message": str(primary)[:1024],
                                   "retained_job": retained})
                    if closed and not retained:
                        wire.emit({"event": "closed", "generation": session.generation if session else 0,
                                   "reason": "error" if primary is not None else reason, "worker_closed": True,
                                   "outstanding_slots": sum(lease is not None for lease in session.leases) if session else 0,
                                   "telemetry_dropped": session.drops() if session else 0, "closed_at": time.monotonic()})
                    wire.drain()
                except BaseException as drain_error:
                    if primary is None:
                        primary = drain_error
                    else:
                        primary.add_note("Service final output failed: " + type(drain_error).__name__)
        if primary is not None:
            raise primary


def main(argv=None):
    import argparse
    import signal
    import threading
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--describe", action="store_true")
    for name in ("input", "directory", "runtime", "model"):
        parser.add_argument("--" + name)
    parser.add_argument("--backend", choices=("cpu", "vulkan"), default="vulkan")
    parser.add_argument("--threads", type=int, default=2)
    for name in ("max_input_bytes", "max_job_bytes", "max_diagnostic_bytes"):
        parser.add_argument("--" + name.replace("_", "-"), type=int, default=getattr(ServiceLimits(), name))
    for name in ("max_seconds", "request_timeout"):
        parser.add_argument("--" + name.replace("_", "-"), type=float, default=getattr(ServiceLimits(), name))
    args = parser.parse_args(argv)
    supplied = (args.input, args.directory, args.runtime, args.model)
    if args.describe and any(value is not None for value in supplied):
        parser.error("--describe cannot be combined with service paths")
    if not args.describe and any(value is None for value in supplied):
        parser.error("--input, --directory, --runtime and --model are required")
    stop = threading.Event()
    handlers = {}
    try:
        if args.describe:
            wire = (json.dumps(describe(), sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode()
            if len(wire) > COMMAND_BYTES:
                raise RuntimeError("service description exceeds byte limit")
            sys.stdout.buffer.write(wire)
            sys.stdout.buffer.flush()
            return 0
        for number in (signal.SIGINT, signal.SIGTERM):
            handlers[number] = signal.signal(number, lambda signum, frame: stop.set())
        serve_f32(args.input, args.directory, args.runtime, args.model, backend=args.backend,
                  threads=args.threads, limits=ServiceLimits(**{key: getattr(args, key) for key in asdict(ServiceLimits())}),
                  stop=stop)
        return 130 if stop.is_set() else 0
    except exporter.ExportCancelled as error:
        sys.stderr.write("stem service stopped\n")
        if any("retain" in note for note in getattr(error, "__notes__", ())):
            sys.stderr.write("Cleanup unproven; retain input/job for parent verification\n")
        return 130
    except Exception as error:
        retained = "; cleanup unproven; retain input/job" if any(
            "retain" in note for note in getattr(error, "__notes__", ())) else ""
        detail = (type(error).__name__ + ": " + str(error)[:800] + retained).replace("\n", " ").encode(errors="replace")[:1024]
        sys.stderr.buffer.write(detail + b"\n")
        sys.stderr.buffer.flush()
        return 1
    finally:
        for number, previous in handlers.items():
            signal.signal(number, previous)


if __name__ == "__main__":
    raise SystemExit(main())

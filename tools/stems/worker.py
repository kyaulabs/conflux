#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Experimental M0 supervisor; never call from the audio callback.

One background thread streams stereo 44.1 kHz PCM16 WAV or raw F32LE input to one
persistent native separator. Cancellation invalidates work immediately; an in-flight GPU
request finishes and is discarded. Timeout/close can terminate the child. Output
is bounded, source-major float32 [stem, frame, channel]. NumPy is required only
for this isolated experiment, not the application build.
"""

from collections import deque
from contextlib import ExitStack, suppress
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import selectors
import stat
import subprocess
import tempfile
import threading
import time
import wave

import numpy as np


RATE = 44100
CHANNELS = 2
WINDOW = 343980
HOP = 257985
OVERLAP = WINDOW - HOP
STEMS = ("drums", "bass", "other", "vocals")
_WEIGHT = np.concatenate((np.arange(1, WINDOW // 2 + 1, dtype=np.float32),
                          np.arange(WINDOW - WINDOW // 2, 0, -1, dtype=np.float32)))
_WEIGHT /= _WEIGHT.max()


class NativeRuntime:
    """Bounded JSONL/raw-file bridge to tools/runtime/htdemucs_segment.cpp."""

    def __init__(self, runtime, model, backend, threads, timeout, *, passive_openmp=True,
                 spool_directory=None, max_stderr_bytes=None):
        if (max_stderr_bytes is not None and (type(max_stderr_bytes) is not int
                or not 0 < max_stderr_bytes <= 65536)):
            raise ValueError("invalid native diagnostic byte limit")
        self._fixed_spool = spool_directory is not None
        self._process_reaped = False
        self._closed = False
        self._close_failed = False
        self.max_stderr_bytes = max_stderr_bytes
        self._stderr_tail = bytearray()
        self.timeout = timeout
        self._cleanup = ExitStack()
        try:
            if spool_directory is None:
                self.directory = tempfile.TemporaryDirectory(prefix="conflux-stem-worker-")
                self._cleanup.callback(self.directory.cleanup)
                self.root = Path(self.directory.name)
            else:
                self.root = Path(spool_directory)
                self.root.mkdir(mode=0o700)
                self._cleanup.callback(self._remove_spool)
            self.stderr = self._cleanup.enter_context((self.root / "runtime.stderr").open("wb"))
            environment = os.environ.copy()
            if backend == "vulkan" and passive_openmp:
                # CPU workers should sleep while the integrated GPU runs. Keep this
                # policy local to the separator; CPU inference inherits the caller.
                environment.update(OMP_WAIT_POLICY="PASSIVE", GOMP_SPINCOUNT="0")
            self.process_environment = {key: environment[key] for key in
                                        ("OMP_WAIT_POLICY", "GOMP_SPINCOUNT") if key in environment}
            self.process = subprocess.Popen(
                [str(runtime), "--model", str(model), "--backend", backend,
                 "--threads", str(threads)], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=self.stderr if max_stderr_bytes is None else subprocess.PIPE,
                bufsize=0, env=environment)
            if max_stderr_bytes is not None:
                self._cleanup.callback(self.process.stderr.close)
            self._cleanup.callback(self.process.stdout.close)
            self._cleanup.callback(self.process.stdin.close)
            self._cleanup.callback(self._close_process)
            self.selector = selectors.DefaultSelector()
            self._cleanup.callback(self.selector.close)
            self.selector.register(self.process.stdout, selectors.EVENT_READ)
            if max_stderr_bytes is not None:
                self.selector.register(self.process.stderr, selectors.EVENT_READ)
            self.buffer = bytearray()
        except BaseException:
            # Cleanup failures must not replace the original startup error.
            with suppress(Exception):
                self._cleanup.close()
            raise

    def _remove_spool(self):
        if hasattr(self, "process") and not self._process_reaped:
            return  # A failed wait must leave caller-owned native files intact.
        for name in ("input.f32", "output.f32", "runtime.stderr"):
            (self.root / name).unlink(missing_ok=True)
        self.root.rmdir()

    def _flush_diagnostics(self):
        if self.max_stderr_bytes is not None and not self.stderr.closed:
            self.stderr.seek(0)
            self.stderr.write(self._stderr_tail)
            self.stderr.truncate()
            self.stderr.flush()

    def _receive_bounded(self):
        deadline = time.monotonic() + self.timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self.abort()
                self._flush_diagnostics()
                raise TimeoutError("native stem request timed out; child terminated")
            ready = self.selector.select(remaining)
            # One fixed read per pipe/event: stderr cannot starve stdout or reset
            # the absolute request deadline, even when the child floods stderr.
            for key, _ in ready:
                if time.monotonic() >= deadline:
                    break
                data = os.read(key.fileobj.fileno(), 4096)
                if key.fileobj is self.process.stderr:
                    if data:
                        self._stderr_tail.extend(data)
                        del self._stderr_tail[:-self.max_stderr_bytes]
                    else:
                        self.selector.unregister(key.fileobj)
                elif not data:
                    self._flush_diagnostics()
                    detail = self._stderr_tail[-4096:].decode(errors="replace")
                    raise RuntimeError(f"native stem worker closed its output: {detail}")
                else:
                    self.buffer.extend(data)
                    if len(self.buffer) > 65536:
                        raise RuntimeError("native stem protocol line exceeds limit")
        self._flush_diagnostics()
        line, _, remainder = self.buffer.partition(b"\n")
        self.buffer = bytearray(remainder)
        event = json.loads(line)
        if not isinstance(event, dict):
            raise RuntimeError("native stem protocol requires an object")
        return event

    def _receive(self):
        if self.max_stderr_bytes is not None:
            return self._receive_bounded()
        deadline = time.monotonic() + self.timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self.selector.select(max(0, remaining)):
                self.abort()
                raise TimeoutError("native stem request timed out; child terminated")
            data = os.read(self.process.stdout.fileno(), 4096)
            if not data:
                self.stderr.flush()
                with (self.root / "runtime.stderr").open("rb") as diagnostic:
                    diagnostic.seek(max(0, os.fstat(diagnostic.fileno()).st_size - 4096))
                    detail = diagnostic.read(4096).decode(errors="replace")
                raise RuntimeError(f"native stem worker closed its output: {detail}")
            self.buffer.extend(data)
            if len(self.buffer) > 65536:
                raise RuntimeError("native stem protocol line exceeds limit")
        line, _, remainder = self.buffer.partition(b"\n")
        self.buffer = bytearray(remainder)
        event = json.loads(line)
        if not isinstance(event, dict):
            raise RuntimeError("native stem protocol requires an object")
        return event

    def start(self):
        ready = self._receive()
        if (ready.get("event") != "ready" or ready.get("sample_rate") != RATE
                or ready.get("channels") != CHANNELS
                or ready.get("segment_frames") != WINDOW
                or ready.get("stems") != list(STEMS)):
            raise RuntimeError(f"unexpected native stem contract: {ready}")
        ready["process_environment"] = dict(self.process_environment)
        return ready

    def infer(self, samples, request_id):
        input_path, output_path = self.root / "input.f32", self.root / "output.f32"
        np.asarray(samples, dtype="<f4").tofile(input_path)
        output_path.unlink(missing_ok=True)
        request = {"id": request_id, "input": str(input_path), "output": str(output_path)}
        wire = (json.dumps(request) + "\n").encode()
        if self.process.stdin.write(wire) != len(wire):
            raise RuntimeError("incomplete native stem request write")
        result = self._receive()
        if (result.get("event") != "result" or result.get("id") != request_id
                or result.get("frames") != WINDOW or result.get("stems") != len(STEMS)):
            raise RuntimeError(f"unexpected native stem result: {result}")
        expected = len(STEMS) * WINDOW * CHANNELS
        if output_path.stat().st_size != expected * 4:
            raise RuntimeError("native stem output size mismatch")
        output = np.fromfile(output_path, dtype="<f4").reshape(len(STEMS), WINDOW, CHANNELS)
        if not np.isfinite(output).all():
            raise RuntimeError("native stem output contains non-finite samples")
        return output, result

    def abort(self):
        try:
            self.process.kill()
        except ProcessLookupError:
            pass

    def _close_process(self):
        if self.process.poll() is None:
            self.abort()
        self.process.wait(timeout=2)
        self._process_reaped = True

    def close(self):
        if self._closed:
            return
        if self._close_failed:
            # ExitStack is already drained. Retain ownership so a later close can
            # retry kill/wait rather than treating failed reaping as success.
            self._close_process()
            if self._fixed_spool and self.root.exists():
                self._remove_spool()
            self._closed = True
            return
        try:
            try:
                self._flush_diagnostics()
            finally:
                self._cleanup.close()
        except BaseException:
            self._close_failed = True
            raise
        self._closed = True


@dataclass(frozen=True)
class _Command:
    generation: int
    path: Path
    start: int
    issued: float
    source_format: str = "wav_pcm16"


@dataclass(frozen=True)
class _Source:
    path: Path
    signature: tuple
    identity: str
    frames: int
    mean: np.float32
    std: np.float32
    source_format: str = "wav_pcm16"


class _Superseded(Exception):
    pass


def _signature(path):
    info = path.stat()
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def _open_f32(path, expected_signature):
    """Open only the captured regular stereo F32LE source; never block on a FIFO."""
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_size <= 0 or info.st_size % (CHANNELS * 4):
            raise ValueError("F32LE source must be a nonempty regular file of complete stereo frames")
        observed = (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)
        if observed != expected_signature:
            raise RuntimeError("stem source changed while opening F32LE input")
        return os.fdopen(descriptor, "rb")
    except BaseException:
        with suppress(OSError):
            os.close(descriptor)
        raise


def _open_model(path, expected_signature):
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        info = os.fstat(descriptor)
        observed = (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)
        if not stat.S_ISREG(info.st_mode) or observed != expected_signature:
            raise RuntimeError("stem model changed while opening identity input")
        return os.fdopen(descriptor, "rb")
    except BaseException:
        with suppress(OSError):
            os.close(descriptor)
        raise


class StemWorker:
    """Asynchronous experiment with one active request and latest-control wins.

    load/seek/cancel return a generation without waiting for inference or source
    I/O. poll filters stale queued chunks. A consumer holding an earlier result
    must call is_current again before scheduling it after any control change.
    max_ready bounds unconsumed audio; telemetry is capped at 256 events and may
    drop its oldest entry. This is a benchmark tool, not a real-time queue.
    """

    def __init__(self, runtime, model, backend="vulkan", threads=4, max_ready=2,
                 request_timeout=30, *, _backend_factory=NativeRuntime):
        if (backend not in ("cpu", "vulkan") or isinstance(threads, bool)
                or not isinstance(threads, int) or threads < 1 or threads > 64
                or isinstance(max_ready, bool) or not isinstance(max_ready, int)
                or max_ready < 1 or not math.isfinite(request_timeout) or request_timeout <= 0):
            raise ValueError("invalid stem worker configuration")
        self.runtime = Path(runtime).absolute()
        self.model = Path(model).absolute()
        self.backend = backend
        self.threads = threads
        self.max_ready = max_ready
        self.request_timeout = request_timeout
        self._factory = _backend_factory
        self._condition = threading.Condition()
        self._generation = 0
        self._command = None
        self._path = None
        self._source_format = "wav_pcm16"
        self._source_id = None
        self._requested_start = 0
        self._next_consume = 0
        self._closed = False
        self._events = deque(maxlen=256)
        self._chunks = deque()
        self._ready_high_water = 0
        self._telemetry_high_water = 0
        self._telemetry_dropped = 0
        self._discarded = 0
        self._cancelled = 0
        self._backend_starts = 0
        self._backend_pid = None
        self._processing_generation = 0
        self._inflight = None
        self._native = None
        self._cleanup_error = None
        self._model_id = None
        self._model_signature = None
        self._cached_source = None
        self._request_id = 0
        self._thread = threading.Thread(target=self._run, name="conflux-stem-worker", daemon=True)
        self._thread.start()

    def _control(self, action, path=None, start=0, source_format=None):
        if isinstance(start, bool) or not isinstance(start, int) or start < 0:
            raise ValueError("frame must be a nonnegative integer")
        with self._condition:
            if self._closed:
                raise RuntimeError("stem worker is closed")
            if path is not None:
                self._path = Path(path).absolute()
                self._source_format = source_format or "wav_pcm16"
            if action != "cancel" and self._path is None:
                raise RuntimeError("load a source before seeking")
            self._generation += 1
            if action == "cancel":
                self._cancelled += 1
            now = time.monotonic()
            self._command = None if action == "cancel" else _Command(
                self._generation, self._path, start, now, self._source_format)
            self._source_id = None
            self._requested_start = start
            self._next_consume = start
            self._chunks.clear()
            self._emit_locked("control", self._generation, command=action,
                              frame_start=start, source_path=str(self._path), timestamp=now)
            self._condition.notify_all()
            return self._generation

    def load(self, path, start_frame=0):
        return self._control("load", path, start_frame, "wav_pcm16")

    def load_f32(self, path, start_frame=0):
        """Load local raw little-endian float32 stereo at 44100 Hz; do not clip headroom."""
        return self._control("load", path, start_frame, "f32le")

    def seek(self, frame):
        return self._control("seek", start=frame)

    def cancel(self):
        return self._control("cancel")

    @property
    def backend_pid(self):
        with self._condition:
            return self._backend_pid

    def statistics(self):
        """Serializable observation for benchmark evidence, never an audio API."""
        with self._condition:
            pending = (self._command.generation if self._command is not None
                       and self._command.generation != self._processing_generation else None)
            return {"generation": self._generation, "ready_count": len(self._chunks),
                    "max_ready": self.max_ready, "ready_high_water": self._ready_high_water,
                    "telemetry_count": len(self._events), "max_telemetry": self._events.maxlen,
                    "telemetry_high_water": self._telemetry_high_water,
                    "telemetry_dropped": self._telemetry_dropped,
                    "pending_latest_generation": pending,
                    "inflight": dict(self._inflight) if self._inflight is not None else None,
                    "discarded_count": self._discarded, "cancel_count": self._cancelled,
                    "backend_start_count": self._backend_starts,
                    "backend_pid": self._backend_pid, "closed": self._closed}

    def is_current(self, event):
        with self._condition:
            return self._is_current_locked(event)

    def _is_current_locked(self, event):
        return (not self._closed and self._command is not None
                and event.get("generation") == self._generation
                and event.get("source_id") == self._source_id
                and event.get("frame_start", -1) >= self._requested_start)

    def poll(self, timeout=0):
        deadline = time.monotonic() + max(0, timeout)
        with self._condition:
            while True:
                while self._chunks and not self._is_current_locked(self._chunks[0]):
                    self._chunks.popleft()
                    self._condition.notify_all()
                if self._events and (not self._chunks
                        or self._events[0]["timestamp"] <= self._chunks[0]["timestamp"]):
                    return self._events.popleft()
                if self._chunks:
                    event = self._chunks.popleft()
                    self._condition.notify_all()
                    if event["frame_start"] != self._next_consume:
                        raise RuntimeError("noncontiguous stem consumer result")
                    self._next_consume += event["frames"]
                    event["queue_delay_ms"] = (time.monotonic() - event["ready_at"]) * 1000
                    return event
                remaining = deadline - time.monotonic()
                if self._closed or remaining <= 0:
                    return None
                self._condition.wait(remaining)

    def close(self, timeout=2):
        if timeout < 0:
            raise ValueError("close timeout must be nonnegative")
        with self._condition:
            self._closed = True
            self._command = None
            self._chunks.clear()
            self._condition.notify_all()
        self._thread.join(timeout)
        if self._thread.is_alive():
            with self._condition:
                native = self._native
            if native is not None:
                native.abort()
            self._thread.join(2)
        if self._thread.is_alive():
            raise TimeoutError("stem supervisor did not stop after child termination")
        with self._condition:
            error = self._cleanup_error
        if error is not None:
            self._drop_runtime()  # Retry any retained native owner after join.
            with self._condition:
                if self._native is None:
                    self._cleanup_error = None
            raise error

    def _emit_locked(self, event, generation, **fields):
        item = {"event": event, "generation": generation, "timestamp": time.monotonic(), **fields}
        if len(self._events) == self._events.maxlen:
            self._telemetry_dropped += 1
        self._events.append(item)
        self._telemetry_high_water = max(self._telemetry_high_water, len(self._events))
        if event == "discarded":
            self._discarded += 1
        self._condition.notify_all()

    def _emit(self, event, generation, **fields):
        with self._condition:
            self._emit_locked(event, generation, **fields)

    def _active(self, command):
        with self._condition:
            return not self._closed and self._command == command

    def _check(self, command):
        if not self._active(command):
            raise _Superseded

    def _ensure_runtime(self, command):
        if self._native is not None:
            return
        signature = _signature(self.model)
        if self._model_id is None or signature != self._model_signature:
            with _open_model(self.model, signature) as stream:
                digest = hashlib.sha256()
                remaining = signature[2]
                while remaining:
                    self._check(command)
                    block = stream.read(min(1024 * 1024, remaining))
                    if not block:
                        raise RuntimeError("stem model was truncated during identity hashing")
                    digest.update(block)
                    remaining -= len(block)
                self._model_id = digest.hexdigest()
            if _signature(self.model) != signature:
                raise RuntimeError("stem model changed during identity hashing")
            self._model_signature = signature
        self._check(command)
        native = self._factory(self.runtime, self.model, self.backend, self.threads,
                               self.request_timeout)
        with self._condition:
            self._native = native
        ready = native.start()
        if _signature(self.model) != signature:
            raise RuntimeError("stem model changed during runtime loading")
        with self._condition:
            self._backend_starts += 1
            self._backend_pid = ready.get("pid")
        self._emit("runtime_ready", self._generation, model_id=self._model_id, runtime=ready)

    def _drop_runtime(self):
        with self._condition:
            native = self._native
        if native is not None:
            try:
                native.close()
            except Exception as error:
                with self._condition:
                    self._cleanup_error = error
                    self._closed = True
                    self._command = None
                    self._chunks.clear()
                    self._condition.notify_all()
                return False
            with self._condition:
                if self._native is native:
                    self._native = None
                    self._backend_pid = None
        return True

    def _source(self, command):
        signature = _signature(command.path)
        cached = self._cached_source
        started = time.monotonic()
        if (cached is not None and cached.path == command.path and cached.signature == signature
                and cached.source_format == command.source_format):
            source = cached
            reused = True
        else:
            # Both passes are bounded; never retain an entire track or its stems.
            raw_float = command.source_format == "f32le"
            digest = hashlib.sha256()
            opened = _open_f32(command.path, signature) if raw_float else command.path.open("rb")
            with opened as stream:
                while block := stream.read(1024 * 1024):
                    self._check(command)
                    digest.update(block)
            count, mean, m2 = 0, 0.0, 0.0
            opened = _open_f32(command.path, signature) if raw_float else wave.open(str(command.path), "rb")
            with opened as stream:
                if raw_float:
                    frames = os.fstat(stream.fileno()).st_size // (CHANNELS * 4)
                else:
                    if (stream.getparams()[:3] != (CHANNELS, 2, RATE)
                            or stream.getcomptype() != "NONE" or stream.getnframes() < 1):
                        raise ValueError("M0 source must be nonempty stereo 44.1 kHz PCM16 WAV")
                    frames = stream.getnframes()
                while data := (stream.read(65536 * CHANNELS * 4) if raw_float else stream.readframes(65536)):
                    self._check(command)
                    if raw_float:
                        if len(data) % (CHANNELS * 4):
                            raise RuntimeError("stem source changed or was truncated during preparation")
                        pcm = np.frombuffer(data, dtype="<f4").reshape(-1, CHANNELS)
                        if not np.isfinite(pcm).all():
                            raise ValueError("F32LE source contains non-finite samples")
                        mono = pcm.astype(np.float64).mean(axis=1)
                    else:
                        pcm = np.frombuffer(data, dtype="<i2").reshape(-1, CHANNELS)
                        mono = pcm.astype(np.float64).mean(axis=1) / 32768.0
                    size = mono.size
                    average = float(mono.mean())
                    delta = average - mean
                    m2 += float(np.square(mono - average).sum()) + delta * delta * count * size / (count + size)
                    mean += delta * size / (count + size)
                    count += size
            if count != frames or _signature(command.path) != signature:
                raise RuntimeError("stem source changed or was truncated during preparation")
            if raw_float:
                with np.errstate(over="ignore", invalid="ignore"):
                    canonical_mean = np.float32(mean)
                    canonical_std = np.float32(math.sqrt(m2 / max(1, count - 1)) + 1e-8)
                if not np.isfinite(canonical_mean) or not np.isfinite(canonical_std) or canonical_std <= 0:
                    raise ValueError("F32LE source normalization is non-finite")
            else:
                canonical_mean = np.float32(mean)
                canonical_std = np.float32(math.sqrt(m2 / max(1, count - 1)) + 1e-8)
            source = _Source(command.path, signature, digest.hexdigest(), frames,
                             canonical_mean, canonical_std, command.source_format)
            self._cached_source = source
            reused = False
        self._check(command)
        with self._condition:
            if self._command != command or self._closed:
                raise _Superseded
            self._source_id = source.identity
            self._emit_locked("source_ready", command.generation, source_id=source.identity,
                              source_frames=source.frames, source_path=str(source.path),
                              source_format=source.source_format,
                              mean=float(source.mean), std=float(source.std), reused=reused,
                              preparation_ms=(time.monotonic() - started) * 1000)
        return source

    def _read_window(self, source, start):
        if _signature(source.path) != source.signature:
            raise RuntimeError("stem source changed after preparation")
        samples = np.zeros((WINDOW, CHANNELS), dtype=np.float32)
        raw_float = source.source_format == "f32le"
        if raw_float:
            with _open_f32(source.path, source.signature) as stream:
                stream.seek(start * CHANNELS * 4)
                data = stream.read(min(WINDOW, source.frames - start) * CHANNELS * 4)
            count = len(data) // (4 * CHANNELS)
        else:
            with wave.open(str(source.path), "rb") as stream:
                stream.setpos(start)
                data = stream.readframes(min(WINDOW, source.frames - start))
            count = len(data) // (2 * CHANNELS)
        if count != min(WINDOW, source.frames - start) or _signature(source.path) != source.signature:
            raise RuntimeError("stem source changed or was truncated while reading")
        # Normalize valid audio before zero-padding, matching the native session.
        if raw_float:
            samples[:count] = np.frombuffer(data, dtype="<f4").reshape(-1, CHANNELS)
            if not np.isfinite(samples[:count]).all():
                raise ValueError("F32LE source contains non-finite samples")
            try:
                with np.errstate(over="raise", invalid="raise", divide="raise"):
                    samples[:count] -= source.mean
                    samples[:count] /= source.std
            except FloatingPointError as error:
                raise ValueError("F32LE source normalization produced non-finite samples") from error
            if not np.isfinite(samples[:count]).all():
                raise ValueError("F32LE source normalization produced non-finite samples")
        else:
            samples[:count] = np.frombuffer(data, dtype="<i2").reshape(-1, CHANNELS).astype(np.float32) / 32768
            samples[:count] -= source.mean
            samples[:count] /= source.std
        return samples

    def _produce(self, command, source):
        if command.start >= source.frames:
            self._emit("complete", command.generation, source_id=source.identity,
                       frame_start=command.start)
            return
        target_window = command.start // HOP
        first_window = max(0, target_window - 1)
        previous = None
        for index in range(first_window, (source.frames + HOP - 1) // HOP):
            self._check(command)
            preroll = index < target_window
            with self._condition:
                while not preroll and len(self._chunks) >= self.max_ready:
                    if self._closed or self._command != command:
                        raise _Superseded
                    self._condition.wait()
            self._check(command)
            start = index * HOP
            service_started = time.monotonic()
            samples = self._read_window(source, start)
            source_read_ms = (time.monotonic() - service_started) * 1000
            self._check(command)
            self._request_id += 1
            request_id = self._request_id
            started = time.monotonic()
            self._emit("started", command.generation, request_id=request_id,
                       source_id=source.identity, window_start_frame=start, preroll=preroll,
                       timestamp=started)
            with self._condition:
                self._inflight = {"generation": command.generation, "request_id": request_id,
                                  "window_start_frame": start, "source_id": source.identity,
                                  "timestamp": started, "preroll": preroll}
            try:
                output, metrics = self._native.infer(samples, request_id)
            finally:
                with self._condition:
                    self._inflight = None
            elapsed = (time.monotonic() - started) * 1000
            if not self._active(command):
                self._emit("discarded", command.generation, request_id=request_id,
                           source_id=source.identity, window_start_frame=start, request_ms=elapsed)
                raise _Superseded
            if output.shape != (len(STEMS), WINDOW, CHANNELS) or not np.isfinite(output).all():
                raise RuntimeError("stem backend returned invalid samples")
            overlap_started = time.monotonic()
            # Fixed source grid, including a zero-padded final window. This avoids
            # the upstream offline session's moving, centered final-window grid.
            weighted = output * _WEIGHT[None, :, None]
            valid = min(HOP, source.frames - start)
            hop = weighted[:, :valid].copy()
            denominator = _WEIGHT[:valid].copy()
            if previous is not None:
                overlap = min(valid, OVERLAP)
                hop[:, :overlap] += previous[:, :overlap]
                denominator[:overlap] += _WEIGHT[HOP:HOP + overlap]
            previous = weighted[:, HOP:].copy()
            if preroll:
                continue
            hop /= denominator[None, :, None]
            # audio.cpp restores the track-wide mono mean to EACH source.
            hop *= source.std
            hop += source.mean
            if not np.isfinite(hop).all():
                raise RuntimeError("stem reconstruction contains non-finite samples")
            trim = max(0, command.start - start)
            hop = hop[:, trim:].copy()
            ready = time.monotonic()
            event = {"event": "chunk", "generation": command.generation,
                     "timestamp": ready, "ready_at": ready, "request_id": request_id,
                     "source_id": source.identity, "source_frames": source.frames,
                     "model_id": self._model_id, "frame_start": start + trim,
                     "source_format": source.source_format,
                     "window_start_frame": start, "frames": hop.shape[1],
                     "sample_rate": RATE, "channels": CHANNELS, "stems": STEMS,
                     "samples": hop, "request_ms": elapsed, "metrics": metrics,
                     "source_read_ms": source_read_ms,
                     "overlap_ms": (ready - overlap_started) * 1000,
                     "hop_service_ms": (ready - service_started) * 1000,
                     "control_latency_ms": (ready - command.issued) * 1000}
            with self._condition:
                if self._closed or self._command != command:
                    raise _Superseded
                self._chunks.append(event)
                self._ready_high_water = max(self._ready_high_water, len(self._chunks))
                event["ready_queue_depth"] = len(self._chunks)
                self._condition.notify_all()
        self._check(command)
        self._emit("complete", command.generation, source_id=source.identity,
                   frame_start=source.frames)

    def _run(self):
        processed = 0
        try:
            while True:
                with self._condition:
                    while not self._closed and (self._command is None or self._command.generation == processed):
                        self._condition.wait()
                    if self._closed:
                        return
                    command = self._command
                    processed = command.generation
                    self._processing_generation = processed
                try:
                    self._ensure_runtime(command)
                    self._check(command)
                    source = self._source(command)
                    self._produce(command, source)
                except _Superseded:
                    continue
                except Exception as error:
                    with self._condition:
                        if self._command == command:
                            self._source_id = None
                            self._chunks.clear()
                        self._emit_locked("error", command.generation, message=str(error),
                                          source_path=str(command.path))
                    if not self._drop_runtime():
                        return
        finally:
            self._drop_runtime()

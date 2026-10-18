#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Experimental M0 supervisor; never call from the audio callback.

One background thread streams stereo 44.1 kHz PCM16 WAV input to one persistent
native separator. Cancellation invalidates work immediately; an in-flight GPU
request finishes and is discarded. Timeout/close can terminate the child. Output
is bounded, source-major float32 [stem, frame, channel]. NumPy is required only
for this isolated experiment, not the application build.
"""

from collections import deque
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import selectors
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

    def __init__(self, runtime, model, backend, threads, timeout, *, passive_openmp=True):
        self.timeout = timeout
        self.directory = tempfile.TemporaryDirectory(prefix="conflux-stem-worker-")
        self.root = Path(self.directory.name)
        self.stderr = (self.root / "runtime.stderr").open("wb")
        environment = os.environ.copy()
        if backend == "vulkan" and passive_openmp:
            # CPU workers should sleep while the integrated GPU runs. Keep this
            # policy local to the separator; CPU inference inherits the caller.
            environment.update(OMP_WAIT_POLICY="PASSIVE", GOMP_SPINCOUNT="0")
        self.process_environment = {key: environment[key] for key in
                                    ("OMP_WAIT_POLICY", "GOMP_SPINCOUNT") if key in environment}
        try:
            self.process = subprocess.Popen(
                [str(runtime), "--model", str(model), "--backend", backend,
                 "--threads", str(threads)], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=self.stderr, bufsize=0, env=environment)
        except Exception:
            self.stderr.close()
            self.directory.cleanup()
            raise
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer = bytearray()

    def _receive(self):
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

    def close(self):
        if self.process.poll() is None:
            self.abort()
        self.process.wait(timeout=2)
        self.selector.close()
        self.process.stdin.close()
        self.process.stdout.close()
        self.stderr.close()
        self.directory.cleanup()


@dataclass(frozen=True)
class _Command:
    generation: int
    path: Path
    start: int
    issued: float


@dataclass(frozen=True)
class _Source:
    path: Path
    signature: tuple
    identity: str
    frames: int
    mean: np.float32
    std: np.float32


class _Superseded(Exception):
    pass


def _signature(path):
    info = path.stat()
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


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
        self._model_id = None
        self._model_signature = None
        self._cached_source = None
        self._request_id = 0
        self._thread = threading.Thread(target=self._run, name="conflux-stem-worker", daemon=True)
        self._thread.start()

    def _control(self, action, path=None, start=0):
        if isinstance(start, bool) or not isinstance(start, int) or start < 0:
            raise ValueError("frame must be a nonnegative integer")
        with self._condition:
            if self._closed:
                raise RuntimeError("stem worker is closed")
            if path is not None:
                self._path = Path(path).absolute()
            if action != "cancel" and self._path is None:
                raise RuntimeError("load a source before seeking")
            self._generation += 1
            if action == "cancel":
                self._cancelled += 1
            now = time.monotonic()
            self._command = None if action == "cancel" else _Command(
                self._generation, self._path, start, now)
            self._source_id = None
            self._requested_start = start
            self._next_consume = start
            self._chunks.clear()
            self._emit_locked("control", self._generation, command=action,
                              frame_start=start, source_path=str(self._path), timestamp=now)
            self._condition.notify_all()
            return self._generation

    def load(self, path, start_frame=0):
        return self._control("load", path, start_frame)

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

    def _ensure_runtime(self):
        if self._native is not None:
            return
        signature = _signature(self.model)
        if self._model_id is None or signature != self._model_signature:
            with self.model.open("rb") as stream:
                self._model_id = hashlib.file_digest(stream, "sha256").hexdigest()
            if _signature(self.model) != signature:
                raise RuntimeError("stem model changed during identity hashing")
            self._model_signature = signature
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
            native, self._native = self._native, None
            self._backend_pid = None
        if native is not None:
            native.close()

    def _source(self, command):
        signature = _signature(command.path)
        cached = self._cached_source
        started = time.monotonic()
        if cached is not None and cached.path == command.path and cached.signature == signature:
            source = cached
            reused = True
        else:
            # Both passes are bounded; never retain an entire track or its stems.
            digest = hashlib.sha256()
            with command.path.open("rb") as stream:
                while block := stream.read(1024 * 1024):
                    self._check(command)
                    digest.update(block)
            count, mean, m2 = 0, 0.0, 0.0
            with wave.open(str(command.path), "rb") as stream:
                if (stream.getparams()[:3] != (CHANNELS, 2, RATE)
                        or stream.getcomptype() != "NONE" or stream.getnframes() < 1):
                    raise ValueError("M0 source must be nonempty stereo 44.1 kHz PCM16 WAV")
                frames = stream.getnframes()
                while data := stream.readframes(65536):
                    self._check(command)
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
            source = _Source(command.path, signature, digest.hexdigest(), frames,
                             np.float32(mean), np.float32(math.sqrt(m2 / max(1, count - 1)) + 1e-8))
            self._cached_source = source
            reused = False
        self._check(command)
        with self._condition:
            if self._command != command or self._closed:
                raise _Superseded
            self._source_id = source.identity
            self._emit_locked("source_ready", command.generation, source_id=source.identity,
                              source_frames=source.frames, source_path=str(source.path),
                              mean=float(source.mean), std=float(source.std), reused=reused,
                              preparation_ms=(time.monotonic() - started) * 1000)
        return source

    def _read_window(self, source, start):
        if _signature(source.path) != source.signature:
            raise RuntimeError("stem source changed after preparation")
        samples = np.zeros((WINDOW, CHANNELS), dtype=np.float32)
        with wave.open(str(source.path), "rb") as stream:
            stream.setpos(start)
            data = stream.readframes(min(WINDOW, source.frames - start))
        count = len(data) // (2 * CHANNELS)
        if count != min(WINDOW, source.frames - start) or _signature(source.path) != source.signature:
            raise RuntimeError("stem source changed or was truncated while reading")
        # Normalize valid audio before zero-padding, matching the native session.
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
            trim = max(0, command.start - start)
            hop = hop[:, trim:].copy()
            ready = time.monotonic()
            event = {"event": "chunk", "generation": command.generation,
                     "timestamp": ready, "ready_at": ready, "request_id": request_id,
                     "source_id": source.identity, "source_frames": source.frames,
                     "model_id": self._model_id, "frame_start": start + trim,
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
                    self._ensure_runtime()
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
                    self._drop_runtime()
        finally:
            self._drop_runtime()

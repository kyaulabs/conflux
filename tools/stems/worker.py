#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Experimental M0 supervisor; never call from the audio callback.

One background thread streams stereo 44.1 kHz PCM16 WAV or raw F32LE input to one
persistent native separator. Cancellation invalidates work immediately. A same-source
seek can use an active request for the exact input window. Timeout/close can terminate the child. Output
is bounded, source-major float32 [stem, frame, channel]. NumPy is required only
for this isolated experiment, not the application build.
"""

from collections import OrderedDict, deque
from concurrent.futures import Future, ThreadPoolExecutor
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
_SEEK_CACHE_WINDOWS = 3
_WEIGHT = np.concatenate((np.arange(1, WINDOW // 2 + 1, dtype=np.float32),
                          np.arange(WINDOW - WINDOW // 2, 0, -1, dtype=np.float32)))
_WEIGHT /= _WEIGHT.max()


def _first_contributing_window(frame):
    window, offset = divmod(frame, HOP)
    # The previous window contributes to the first OVERLAP frames of this hop.
    return window - 1 if window and offset < OVERLAP else window


def _window_contributes_to_seek(window, frame):
    return _first_contributing_window(frame) <= window <= frame // HOP


class NativeRuntime:
    """Bounded JSONL/raw-file bridge to tools/runtime/htdemucs_segment.cpp."""

    def __init__(self, runtime, model, backend, threads, timeout, *, passive_openmp=True,
                 spool_directory=None, max_stderr_bytes=None):
        if (max_stderr_bytes is not None and (type(max_stderr_bytes) is not int
                or not 0 < max_stderr_bytes <= 65536)):
            raise ValueError("invalid native diagnostic byte limit")
        self._fixed_spool = spool_directory is not None
        self._process_reaped = False
        self._kill_requested = False
        self._closed = False
        self._close_failed = False
        self._idle = False
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
        self._idle = True
        return ready

    def infer(self, samples, request_id):
        self._idle = False
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
        self._idle = True
        return output, result

    @property
    def shutdown_result(self):
        if not self._process_reaped:
            return None
        status = self.process.returncode
        result = {"kill_requested": self._kill_requested}
        if status < 0:
            result["signal"] = -status
        else:
            result["exit_code"] = status
        return result

    def abort(self):
        self._idle = False
        self._kill_requested = True
        try:
            self.process.kill()
        except ProcessLookupError:
            pass

    def _close_process(self):
        if self.process.poll() is None:
            if self._idle:
                # Only a validated handshake or reply leaves an idle worker.
                # Give native teardown a bounded chance to complete.
                self._idle = False
                quit_request = b'{"command":"quit"}\n'
                with suppress(OSError, subprocess.TimeoutExpired):
                    if self.process.stdin.write(quit_request) == len(quit_request):
                        self.process.wait(timeout=0.5)
                        self._process_reaped = True
                        return
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
    source_generation: int
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


@dataclass(frozen=True)
class _PendingInference:
    command: _Command
    source: _Source
    event: dict
    future: Future
    source_read_ms: float


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
                 request_timeout=30, *, _backend_factory=NativeRuntime, _model_pin=None):
        if (backend not in ("cpu", "vulkan") or isinstance(threads, bool)
                or not isinstance(threads, int) or threads < 1 or threads > 64
                or isinstance(max_ready, bool) or not isinstance(max_ready, int)
                or max_ready < 1 or not math.isfinite(request_timeout) or request_timeout <= 0):
            raise ValueError("invalid stem worker configuration")
        self.runtime = Path(runtime).absolute()
        self.model = Path(model).absolute()
        if _model_pin is not None and _model_pin.path != self.model:
            raise ValueError("verified stem model pin must match the runtime model path")
        self._model_pin = _model_pin
        self.backend = backend
        self.threads = threads
        self.max_ready = max_ready
        self.request_timeout = request_timeout
        self._factory = _backend_factory
        self._condition = threading.Condition()
        self._generation = 0
        self._source_generation = 0
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
        self._native_shutdown = None
        self._backend_pid = None
        self._processing_generation = 0
        self._inflight = None
        self._pending_inference = None
        self._retained_target = None
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix='conflux-native-stems')
        self._native = None
        self._cleanup_error = None
        self._model_id = None
        self._model_signature = None
        self._cached_source = None
        self._opening_weighted_window = None
        self._weighted_windows = OrderedDict()
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
            if action != "seek":
                self._source_generation = self._generation
                self._opening_weighted_window = None
                self._weighted_windows.clear()
                self._retained_target = None
            elif self._retained_target is not None:
                retained_window = self._retained_target.event["window_start_frame"] // HOP
                if not _window_contributes_to_seek(retained_window, start):
                    self._retained_target = None
            if action == "cancel":
                self._cancelled += 1
            now = time.monotonic()
            self._command = None if action == "cancel" else _Command(
                self._generation, self._path, start, now, self._source_generation, self._source_format)
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
                    "backend_pid": self._backend_pid, "closed": self._closed,
                    "native_shutdown": None if self._native_shutdown is None else dict(self._native_shutdown)}

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
        model_pin = self._model_pin
        signature = _signature(self.model)
        if model_pin is not None:
            # The service/exporter has hashed this descriptor and owns it
            # until close. Check both the path and descriptor before reusing it.
            if signature != model_pin.signature or not model_pin.matches_signature():
                raise RuntimeError("stem model changed after verified pinning")
            self._model_id = model_pin.digest
        elif self._model_id is None or signature != self._model_signature:
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
            self._native_shutdown = None
        ready = native.start()
        if (_signature(self.model) != signature
                or (model_pin is not None and not model_pin.matches_signature())):
            raise RuntimeError("stem model changed during runtime loading")
        with self._condition:
            self._backend_starts += 1
            self._backend_pid = ready.get("pid")
        self._emit("runtime_ready", self._generation, model_id=self._model_id, runtime=ready)

    def _drop_runtime(self):
        # The inference owner may still be consuming a reply after supersession.
        # Close the native transport only after that exchange is finished.
        with suppress(Exception):
            self._wait_inference()
        with self._condition:
            self._opening_weighted_window = None
            self._weighted_windows.clear()
            self._retained_target = None
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
                    self._native_shutdown = getattr(native, "shutdown_result", None)
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
            raw_float = command.source_format == "f32le"
            digest = hashlib.sha256()
            if not raw_float:
                # A WAV's byte identity also covers headers and non-PCM chunks.
                with command.path.open("rb") as stream:
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
                        digest.update(data)
                        pcm = np.frombuffer(data, dtype="<f4").reshape(-1, CHANNELS)
                        if not np.isfinite(pcm).all():
                            raise ValueError("F32LE source contains non-finite samples")
                        mono = np.add(pcm[:, 0], pcm[:, 1], dtype=np.float64)
                        mono *= 0.5
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

    def _notify_inference(self, _):
        with self._condition:
            self._condition.notify_all()

    def _exchange(self, native, samples, request_id, started):
        output, metrics = native.infer(samples, request_id)
        return output, metrics, (time.monotonic() - started) * 1000

    def _retire_inference_locked(self, request_id):
        pending = self._pending_inference
        if pending is not None and pending.event["request_id"] == request_id:
            self._pending_inference = None
            self._inflight = None
        if self._retained_target is not None and self._retained_target.event["request_id"] == request_id:
            self._retained_target = None

    def _wait_inference(self, command=None, *, adopt=False, keep_pending=False):
        pending = self._pending_inference
        if pending is None:
            return None
        owner, event, future = pending.command, pending.event, pending.future
        with self._condition:
            while not future.done():
                if command is not None and (self._closed or self._command != command):
                    raise _Superseded
                self._condition.wait()
            if command is not None and (self._closed or self._command != command):
                raise _Superseded
            # The future is complete. Consume the reply without losing its
            # ownership if an exact-window seek interrupts publication.
            try:
                output, metrics, elapsed = future.result()
            except BaseException:
                self._retire_inference_locked(event["request_id"])
                raise
            if not keep_pending:
                self._retire_inference_locked(event["request_id"])
        if owner != command and not adopt:
            self._remember_superseded_seek_window(command, pending, output)
            self._emit("discarded", owner.generation, request_id=event["request_id"],
                       source_id=event["source_id"], window_start_frame=event["window_start_frame"],
                       request_ms=elapsed)
            return None
        return output, metrics, elapsed

    @staticmethod
    def _matches_window(reply, command, source, start):
        return (reply is not None and source.source_format == "f32le" and reply.source is source
                and reply.command.source_generation == command.source_generation
                and reply.event["window_start_frame"] == start)

    def _inference_for_window(self, command, source, start, preroll):
        retained = self._retained_target
        if self._matches_window(retained, command, source, start):
            self._check(command)
            if _signature(source.path) != source.signature:
                raise RuntimeError("stem source changed during seek preroll")
            output, metrics, elapsed = retained.future.result()
            return output, metrics, elapsed, retained.event["request_id"], retained.source_read_ms

        keep_pending = source.source_format == "f32le"
        pending = self._pending_inference
        if self._matches_window(pending, command, source, start):
            # The pending request used this exact grid window and normalization.
            # Keep its real request timings and charge the new hop for the wait.
            output, metrics, elapsed = self._wait_inference(command, adopt=True, keep_pending=keep_pending)
            if _signature(source.path) != source.signature:
                raise RuntimeError("stem source changed during pending inference")
            return output, metrics, elapsed, pending.event["request_id"], pending.source_read_ms

        # The persistent transport permits only one request at a time.
        self._wait_inference(command)
        self._check(command)
        read_started = time.monotonic()
        samples = self._read_window(source, start)
        source_read_ms = (time.monotonic() - read_started) * 1000
        self._check(command)
        with self._condition:
            # A control can arrive after the input has been checked. Validate
            # and enqueue under the same lock used to publish that control.
            if self._closed or self._command != command:
                raise _Superseded
            self._request_id += 1
            request_id = self._request_id
            started = time.monotonic()
            event = {"generation": command.generation, "request_id": request_id,
                     "window_start_frame": start, "source_id": source.identity,
                     "timestamp": started, "preroll": preroll}
            future = self._executor.submit(self._exchange, self._native, samples, request_id, started)
            self._inflight = event
            self._pending_inference = _PendingInference(command, source, event, future, source_read_ms)
            self._emit_locked("started", **event)
            future.add_done_callback(self._notify_inference)
        output, metrics, elapsed = self._wait_inference(command, keep_pending=keep_pending)
        return output, metrics, elapsed, request_id, source_read_ms

    def _seek_prefix_status(self, command):
        source = self._cached_source
        if (source is None or command.path != source.path or command.source_format != source.source_format
                or command.start > source.frames or _signature(source.path) != source.signature):
            return False, False
        if command.start == source.frames:
            # No native response can contribute audio to this empty suffix.
            return True, False
        target = command.start // HOP
        cached_prefix = True
        with self._condition:
            for index in range(_first_contributing_window(command.start), target + 1):
                start = index * HOP
                window = self._weighted_window_locked(start)
                # A completed target may be parked while a now obsolete
                # preroll is still in flight. It can supply this seek directly.
                if ((window is None or window[0] is not source)
                        and not self._matches_window(self._retained_target, command, source, start)):
                    cached_prefix = False
                    if self._matches_window(self._pending_inference, command, source, start):
                        return False, True
        return cached_prefix, False

    def _weighted_window_locked(self, start):
        if start == 0:
            return self._opening_weighted_window
        return self._weighted_windows.get(start)

    def _remember_window_locked(self, start, source, request_id, weighted):
        entry = (source, request_id, weighted)
        if start == 0:
            # Cached playback resumes within this first model window. Keep
            # one opening window outside the three recent non-opening slots.
            self._opening_weighted_window = entry
            return
        self._weighted_windows[start] = entry
        self._weighted_windows.move_to_end(start)
        if len(self._weighted_windows) > _SEEK_CACHE_WINDOWS:
            self._weighted_windows.popitem(last=False)

    def _remember_superseded_seek_window(self, command, pending, output):
        source = pending.source
        start = pending.event["window_start_frame"]
        if (command is None or source.source_format != "f32le" or source is not self._cached_source
                or command.source_generation != pending.command.source_generation):
            return
        with self._condition:
            # The next seek can share this window while validation runs
            # outside the condition lock.
            if (self._closed or self._source_generation != pending.command.source_generation
                    or self._weighted_window_locked(start) is not None):
                return
        if output.shape != (len(STEMS), WINDOW, CHANNELS) or not np.isfinite(output).all():
            return
        weighted = output * _WEIGHT[None, :, None]
        with self._condition:
            if self._closed or self._source_generation != pending.command.source_generation:
                return
            if start and len(self._weighted_windows) >= _SEEK_CACHE_WINDOWS:
                latest = self._command
                if latest is None or latest.start >= source.frames:
                    return
                first = _first_contributing_window(latest.start) * HOP
                target = latest.start // HOP * HOP
                if start == target + HOP:
                    # A clipped first hop can be too short to resume playback.
                    # Keep its completed successor if the first hop is ready.
                    prefix = (self._weighted_window_locked(frame)
                              for frame in range(first, target + HOP, HOP))
                    if any(window is None or window[0] is not source for window in prefix):
                        return
                elif not first <= start <= target:
                    return
                # The successor cannot evict any first-hop contributor.
                evict = next(window for window in self._weighted_windows if not first <= window <= target)
                del self._weighted_windows[evict]
            self._remember_window_locked(start, source, pending.event["request_id"], weighted)
            # Give unconsumed work lower priority than normally published windows.
            if start:
                self._weighted_windows.move_to_end(start, last=False)

    def _produce(self, command, source, retained_service=None):
        if command.start >= source.frames:
            self._emit("complete", command.generation, source_id=source.identity,
                       frame_start=command.start)
            return
        target_window = command.start // HOP
        first_window = _first_contributing_window(command.start)
        previous = None
        for index in range(first_window, (source.frames + HOP - 1) // HOP):
            self._check(command)
            preroll = index < target_window
            start = index * HOP
            with self._condition:
                while not preroll and len(self._chunks) >= self.max_ready:
                    if self._closed or self._command != command:
                        raise _Superseded
                    self._condition.wait()
                cached = self._weighted_window_locked(start)
            self._check(command)
            service_started = time.monotonic()
            if retained_service is not None and (retained_service[0] == start or
                    self._matches_window(self._retained_target, command, source, start)):
                service_started = retained_service[1]
            if (cached is not None and cached[0] is source
                    and _signature(source.path) == source.signature):
                _, request_id, weighted = cached
                overlap_started = time.monotonic()
                timings = {"inference_reused": True}
            else:
                pending = self._pending_inference
                if preroll and self._matches_window(pending, command, source, target_window * HOP):
                    # Release the native exchange for preroll. A newer seek
                    # can still use this reply as its target or overlap.
                    self._wait_inference(command, adopt=True)
                    with self._condition:
                        latest = self._command
                        if (self._closed or latest is None or latest.source_generation != command.source_generation
                                or latest.start >= source.frames
                                or not _window_contributes_to_seek(target_window, latest.start)):
                            raise _Superseded
                        self._retained_target = pending
                output, metrics, elapsed, request_id, source_read_ms = self._inference_for_window(
                    command, source, start, preroll)
                if output.shape != (len(STEMS), WINDOW, CHANNELS) or not np.isfinite(output).all():
                    raise RuntimeError("stem backend returned invalid samples")
                overlap_started = time.monotonic()
                # Fixed source grid, including a zero-padded final window. This avoids
                # the upstream offline session's moving, centered final-window grid.
                weighted = output * _WEIGHT[None, :, None]
                timings = dict(request_ms=elapsed, metrics=metrics, source_read_ms=source_read_ms)
            valid = min(HOP, source.frames - start)
            hop = weighted[:, :valid].copy()
            denominator = _WEIGHT[:valid].copy()
            if previous is not None:
                overlap = min(valid, OVERLAP)
                hop[:, :overlap] += previous[:, :overlap]
                denominator[:overlap] += _WEIGHT[HOP:HOP + overlap]
            previous = weighted[:, HOP:].copy()
            if preroll:
                if source.source_format == "f32le" and not timings.get("inference_reused", False):
                    with self._condition:
                        if self._closed or self._command != command:
                            raise _Superseded
                        target = self._weighted_window_locked(start + HOP)
                        # The opening slot cannot evict cached forward windows.
                        # For other preroll, preserve the existing cached suffix.
                        if start == 0 or target is None or target[0] is not source:
                            self._remember_window_locked(start, source, request_id, weighted)
                        self._retire_inference_locked(request_id)
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
                     "samples": hop, **timings,
                     "overlap_ms": (ready - overlap_started) * 1000,
                     "hop_service_ms": (ready - service_started) * 1000,
                     "control_latency_ms": (ready - command.issued) * 1000}
            with self._condition:
                if self._closed or self._command != command:
                    raise _Superseded
                if source.source_format == "f32le":
                    self._remember_window_locked(start, source, request_id, weighted)
                    self._retire_inference_locked(request_id)
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
                        if self._pending_inference is not None and self._pending_inference.future.done():
                            break
                        self._condition.wait()
                    if self._closed:
                        return
                    command = self._command
                    producing = command is not None and command.generation != processed
                    if producing:
                        processed = command.generation
                        self._processing_generation = processed
                    else:
                        command = self._pending_inference.command
                try:
                    if not producing:
                        self._wait_inference()
                        continue
                    retained_service = None
                    if self._pending_inference is not None:
                        cached_prefix, retain = self._seek_prefix_status(command)
                        if not cached_prefix:
                            pending = self._pending_inference
                            service_started = time.monotonic()
                            try:
                                # Before a cold seek publishes, a failed old request
                                # still belongs to its original generation. Keep a
                                # successful reply for a contributing missing window.
                                self._wait_inference(command, adopt=retain, keep_pending=retain)
                                if retain:
                                    retained_service = pending.event["window_start_frame"], service_started
                            except _Superseded:
                                raise
                            except Exception as error:
                                previous = pending.command
                                self._emit("error", previous.generation, message=str(error),
                                           source_path=str(previous.path))
                                if not self._drop_runtime():
                                    return
                    self._ensure_runtime(command)
                    self._check(command)
                    source = self._source(command)
                    self._produce(command, source, retained_service)
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
            self._executor.shutdown(wait=True)

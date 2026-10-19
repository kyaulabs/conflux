#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded local offline export. Explicit optional tool; never an audio callback.

Input/job paths belong to the caller through successful worker close. Publication
serializes cooperating same-user writers; it is not an adversarial path sandbox.
No original compressed-file identity is inferred from the raw input digest.
"""

from contextlib import ExitStack
from dataclasses import asdict, dataclass
import hashlib
import fcntl
import json
import math
import os
from pathlib import Path
import shutil
import stat
import sys
import time

import numpy as np

# Direct script invocation must resolve the same production package at any cwd.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import tools.stems.worker as worker_module
from tools.stems.worker import CHANNELS, HOP, OVERLAP, RATE, STEMS, WINDOW, NativeRuntime, StemWorker

MAX_INPUT_BYTES = 256 * 1024**2
MAX_OUTPUT_BYTES = 4 * MAX_INPUT_BYTES
MAX_JOB_BYTES = 2 * 1024**3
MAX_SECONDS = 7200
MAX_DIAGNOSTIC_BYTES = 65536
MAX_ASSET_BYTES = 512 * 1024**2
NATIVE_SPOOL_BYTES = WINDOW * CHANNELS * 4 * (1 + len(STEMS))
MANIFEST_LIMIT_BYTES = 16384
NORMALIZATION = "track-mono-sample-variance-f32-v1"
OVERLAP_ADD = "fixed-grid-triangle-f32-v1"


class ExportCancelled(Exception):
    """Handled cancellation; no completed bank was published."""


@dataclass(frozen=True)
class ExportLimits:
    max_input_bytes: int = MAX_INPUT_BYTES
    max_output_bytes: int = MAX_OUTPUT_BYTES
    max_job_bytes: int = MAX_JOB_BYTES
    max_seconds: float = 1800
    request_timeout: float = 30
    max_diagnostic_bytes: int = MAX_DIAGNOSTIC_BYTES

    def validate(self):
        for name, maximum in (("max_input_bytes", MAX_INPUT_BYTES),
                              ("max_output_bytes", MAX_OUTPUT_BYTES),
                              ("max_job_bytes", MAX_JOB_BYTES),
                              ("max_diagnostic_bytes", MAX_DIAGNOSTIC_BYTES)):
            value = getattr(self, name)
            if type(value) is not int or not 0 < value <= maximum:
                raise ValueError(f"invalid export limit: {name}")
        for name, maximum in (("max_seconds", MAX_SECONDS), ("request_timeout", 30)):
            value = getattr(self, name)
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 < value <= maximum:
                raise ValueError(f"invalid export limit: {name}")


def reservation_bytes(input_bytes, max_diagnostic_bytes=MAX_DIAGNOSTIC_BYTES):
    if type(input_bytes) is not int or input_bytes <= 0 or input_bytes % (CHANNELS * 4):
        raise ValueError("input size must contain complete nonempty stereo F32LE frames")
    if type(max_diagnostic_bytes) is not int or not 0 < max_diagnostic_bytes <= MAX_DIAGNOSTIC_BYTES:
        raise ValueError("invalid diagnostic byte limit")
    return 5 * input_bytes + NATIVE_SPOOL_BYTES + max_diagnostic_bytes + MANIFEST_LIMIT_BYTES


def _check(cancel, deadline):
    if cancel is not None and cancel.is_set():
        raise ExportCancelled("stem export cancelled")
    if time.monotonic() >= deadline:
        raise TimeoutError("stem export deadline exceeded")


def _signature(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


class _PinnedFile:
    def __init__(self, path, maximum, stack, cancel, deadline):
        self.path = Path(path).absolute()
        fd = os.open(self.path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
        stack.callback(os.close, fd)
        self.fd = fd
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= maximum:
            raise ValueError("export input/assets must be bounded nonempty regular files")
        self.signature = _signature(info)
        self.size = info.st_size
        self.digest = self.hash(cancel, deadline)

    def hash(self, cancel, deadline):
        if _signature(self.path.lstat()) != self.signature or _signature(os.fstat(self.fd)) != self.signature:
            raise RuntimeError("export input or tool changed")
        digest = hashlib.sha256()
        os.lseek(self.fd, 0, os.SEEK_SET)
        remaining = self.size
        while remaining:
            _check(cancel, deadline)
            block = os.read(self.fd, min(1024 * 1024, remaining))
            if not block:
                raise RuntimeError("export input or tool was truncated")
            digest.update(block)
            remaining -= len(block)
        if _signature(self.path.lstat()) != self.signature or _signature(os.fstat(self.fd)) != self.signature:
            raise RuntimeError("export input or tool changed")
        return digest.hexdigest()

    def verify(self, cancel, deadline):
        if self.hash(cancel, deadline) != self.digest:
            raise RuntimeError("export input or tool contents changed")


def describe():
    """Bounded identity probe: no model, native runtime or supervisor is started."""
    result = {"schema": 1, "numpy_version": np.__version__,
              "input": {"format": "f32le", "sample_rate": RATE, "channels": CHANNELS},
              "window_frames": WINDOW, "hop_frames": HOP, "overlap_frames": OVERLAP,
              "normalization": NORMALIZATION, "overlap_add": OVERLAP_ADD}
    # Product source leaves are small; read captured sizes rather than chasing EOF.
    with ExitStack() as stack:
        deadline = time.monotonic() + 5
        for key, path in (("helper_sha256", Path(__file__)),
                          ("worker_sha256", Path(worker_module.__file__))):
            result[key] = _PinnedFile(path, 1024 * 1024, stack, None, deadline).digest
    return result


def _sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _integer(value):
    return type(value) is int


def _ready(event, model_id, backend, threads):
    runtime = event.get("runtime", {})
    expected = {"sample_rate": RATE, "channels": CHANNELS, "segment_frames": WINDOW,
                "stems": list(STEMS), "backend": backend, "threads": threads}
    if event.get("model_id") != model_id or not isinstance(runtime, dict):
        raise RuntimeError("export model identity mismatch")
    if any(runtime.get(key) != value for key, value in expected.items()):
        raise RuntimeError("export runtime contract mismatch")
    if any(not _integer(runtime.get(key)) for key in ("sample_rate", "channels", "segment_frames", "threads")):
        raise RuntimeError("export runtime geometry must be integral")
    revision = runtime.get("revision")
    if (not isinstance(revision, str) or len(revision) != 40
            or any(character not in "0123456789abcdef" for character in revision)
            or runtime.get("weight_storage") not in ("native", "f32")
            or runtime.get("fence_wait_mode") not in ("blocking", "upstream-hybrid")):
        raise RuntimeError("export runtime precision/revision contract mismatch")
    return {**expected, **{key: runtime[key] for key in ("revision", "weight_storage", "fence_wait_mode")}}


def export_f32(source_f32, destination, runtime, model, *, backend="vulkan", threads=2,
               limits=ExportLimits(), cancel=None, _worker_factory=None):
    """Export exactly four aligned 44100 Hz leaves, then publish one directory.

    max_job_bytes reserves named input + outputs + native spool + diagnostics +
    manifest. The caller separately reserves any converted/cache leaves. Memory
    and preexisting model/runtime files are not copied into that disk reservation.
    Close/retry has a twelve-second cleanup allowance beyond max_seconds. If close
    fails, the private stage is retained; the caller must retain its input/job too.
    """
    limits.validate()
    if backend not in ("cpu", "vulkan") or type(threads) is not int or not 1 <= threads <= 64:
        raise ValueError("invalid export backend/threads")
    deadline = time.monotonic() + limits.max_seconds
    _check(cancel, deadline)
    destination = Path(destination).absolute()
    parent = destination.parent
    info = parent.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError("export destination requires an owned private parent directory")
    lock_path = parent / ("." + destination.name + ".export.lock")
    stage = None
    worker = None
    closed = False
    primary = None
    with ExitStack() as stack:
        lock = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        stack.callback(os.close, lock)
        lock_info = os.fstat(lock)
        if (not stat.S_ISREG(lock_info.st_mode) or lock_info.st_uid != os.getuid()
                or lock_info.st_nlink != 1 or stat.S_IMODE(lock_info.st_mode) != 0o600 or lock_info.st_size != 0):
            raise ValueError("unsafe export ownership lock")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Keep this fixed empty leaf: stale-job cleanup acquires the same flock.
        source = _PinnedFile(source_f32, limits.max_input_bytes, stack, cancel, deadline)
        if source.size % (CHANNELS * 4):
            raise ValueError("F32LE source has an incomplete stereo frame")
        reserved = reservation_bytes(source.size, limits.max_diagnostic_bytes)
        if source.size * len(STEMS) > limits.max_output_bytes or reserved > limits.max_job_bytes:
            raise ValueError("export exceeds whole-job byte reservation")
        model_pin = _PinnedFile(model, MAX_ASSET_BYTES, stack, cancel, deadline)
        runtime_pin = _PinnedFile(runtime, MAX_ASSET_BYTES, stack, cancel, deadline)
        description = describe()
        _check(cancel, deadline)
        try:
            if os.path.lexists(destination):
                raise FileExistsError("export destination already exists")
            candidate = parent / (destination.name + ".stage")
            candidate.mkdir(mode=0o700)
            stage = candidate
            streams = []
            for name in STEMS:
                fd = os.open(stage / (name + ".f32"), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
                streams.append(stack.enter_context(os.fdopen(fd, "wb")))
            native_factory = lambda *args: NativeRuntime(*args, spool_directory=stage / "native",
                                                         max_stderr_bytes=limits.max_diagnostic_bytes)
            factory = _worker_factory or StemWorker
            worker = factory(runtime_pin.path, model_pin.path, backend=backend, threads=threads,
                             max_ready=2, request_timeout=limits.request_timeout,
                             _backend_factory=native_factory)
            generation = worker.load_f32(source.path)
            frames = source.size // (CHANNELS * 4)
            offset = 0
            digests = [hashlib.sha256() for _ in STEMS]
            runtime_ready = None
            source_ready = None
            while True:
                _check(cancel, deadline)
                event = worker.poll(min(0.1, max(0, deadline - time.monotonic())))
                if event is None:
                    continue
                if event.get("generation") != generation or not _integer(event.get("generation")):
                    raise RuntimeError("export received a different generation")
                kind = event.get("event")
                if kind == "runtime_ready":
                    if runtime_ready is not None:
                        raise RuntimeError("export runtime restarted")
                    runtime_ready = _ready(event, model_pin.digest, backend, threads)
                elif kind == "source_ready":
                    if source_ready is not None or runtime_ready is None:
                        raise RuntimeError("export source readiness ordering mismatch")
                    if (event.get("source_id") != source.digest or event.get("source_format") != "f32le"
                            or event.get("source_frames") != frames or not _integer(event.get("source_frames"))):
                        raise RuntimeError("export source identity/geometry mismatch")
                    mean, std = event.get("mean"), event.get("std")
                    if (isinstance(mean, bool) or isinstance(std, bool)
                            or not isinstance(mean, (int, float)) or not isinstance(std, (int, float))
                            or not math.isfinite(mean) or not math.isfinite(std) or std <= 0):
                        raise RuntimeError("export source normalization is invalid")
                    source_ready = {"mean": mean, "std": std}
                elif kind == "chunk":
                    count = min(HOP, frames - offset)
                    samples = event.get("samples")
                    if (source_ready is None or not worker.is_current(event)
                            or event.get("source_id") != source.digest or event.get("model_id") != model_pin.digest
                            or event.get("source_format") != "f32le" or event.get("source_frames") != frames
                            or event.get("stems") != STEMS or event.get("sample_rate") != RATE
                            or event.get("channels") != CHANNELS or event.get("frame_start") != offset
                            or event.get("window_start_frame") != offset or event.get("frames") != count or count <= 0
                            or any(not _integer(event.get(key)) for key in ("source_frames", "sample_rate", "channels", "frame_start", "window_start_frame", "frames"))
                            or not isinstance(samples, np.ndarray) or samples.dtype != np.dtype("float32")
                            or samples.shape != (len(STEMS), count, CHANNELS) or not np.isfinite(samples).all()):
                        raise RuntimeError("export received invalid or noncontiguous stem audio")
                    for index, stream in enumerate(streams):
                        data = np.asarray(samples[index], dtype="<f4").tobytes()
                        if stream.write(data) != len(data):
                            raise OSError("incomplete stem output write")
                        digests[index].update(data)
                    offset += count
                elif kind == "complete":
                    if (source_ready is None or event.get("source_id") != source.digest
                            or event.get("frame_start") != frames or not _integer(event.get("frame_start"))
                            or offset != frames):
                        raise RuntimeError("export completed with missing stem audio")
                    break
                elif kind == "error":
                    message = event.get("message")
                    detail = message[:1024] if isinstance(message, str) else "invalid error event"
                    raise RuntimeError("stem export worker failed: " + detail)
                elif kind not in ("control", "started"):
                    raise RuntimeError("stem export worker emitted unexpected telemetry")
            if worker.statistics().get("telemetry_dropped") != 0:
                raise RuntimeError("export worker lost telemetry")
            worker.close()
            closed = True
            if os.path.lexists(stage / "native"):
                raise RuntimeError("native cleanup unproven; retain caller input and private stage")
            for pinned in (source, model_pin, runtime_pin):
                pinned.verify(cancel, deadline)
            if describe() != description:
                raise RuntimeError("export helper/worker identity changed")
            manifest = {"schema": 1, "describe": description,
                        "source": {"format": "f32le", "sample_rate": RATE, "channels": CHANNELS,
                                   "frames": frames, "bytes": source.size, "sha256": source.digest},
                        "model": {"sha256": model_pin.digest, "bytes": model_pin.size},
                        "runtime": {"sha256": runtime_pin.digest, "bytes": runtime_pin.size, "ready": runtime_ready},
                        "separation": {"worker_sha256": description["worker_sha256"],
                                       "numpy_version": description["numpy_version"], **source_ready,
                                       "window_frames": WINDOW, "hop_frames": HOP, "overlap_frames": OVERLAP,
                                       "normalization": NORMALIZATION, "overlap_add": OVERLAP_ADD},
                        "stems": [{"name": name, "file": name + ".f32", "frames": frames,
                                   "bytes": source.size, "sha256": digest.hexdigest()}
                                  for name, digest in zip(STEMS, digests)],
                        "reservation_bytes": reserved, "limits": asdict(limits)}
            wire = (json.dumps(manifest, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode()
            if len(wire) > MANIFEST_LIMIT_BYTES:
                raise RuntimeError("export manifest exceeds byte limit")
            for stream in streams:
                stream.flush()
                os.fsync(stream.fileno())
                if os.fstat(stream.fileno()).st_size != source.size:
                    raise RuntimeError("export output byte count mismatch")
                stream.close()
            fd = os.open(stage / "manifest.json", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
            with os.fdopen(fd, "wb") as stream:
                if stream.write(wire) != len(wire):
                    raise OSError("incomplete export manifest write")
                stream.flush()
                os.fsync(stream.fileno())
            _sync_directory(stage)
            _check(cancel, deadline)
            if os.path.lexists(destination):
                raise FileExistsError("export destination appeared before publication")
            os.rename(stage, destination)
            stage = None
            # A failure here preserves the complete published directory.
            _sync_directory(parent)
            return manifest
        except BaseException as error:
            primary = error
            raise
        finally:
            try:
                if worker is not None and not closed:
                    try:
                        worker.close()
                        closed = True
                    except BaseException as cleanup_error:
                        if primary is None:
                            raise
                        primary.add_note("Worker close failed; retain caller input and private stage: "
                                         + type(cleanup_error).__name__)
                # A live worker may still use native spool files after close failure.
                native_remains = stage is not None and os.path.lexists(stage / "native")
                if native_remains:
                    if primary is not None:
                        primary.add_note("Native cleanup unproven; retain caller input and private stage")
                elif stage is not None and (worker is None or closed):
                    shutil.rmtree(stage)
            except BaseException as cleanup_error:
                if primary is None:
                    raise
                primary.add_note("Export cleanup failed: " + type(cleanup_error).__name__)

def main(argv=None):
    """Thin local-process interface; parent independently validates manifest.json."""
    import argparse
    import signal
    import threading
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--describe", action="store_true")
    for name in ("input", "destination", "runtime", "model"):
        parser.add_argument("--" + name)
    parser.add_argument("--backend", choices=("cpu", "vulkan"), default="vulkan")
    parser.add_argument("--threads", type=int, default=2)
    for name in ("max_input_bytes", "max_output_bytes", "max_job_bytes", "max_diagnostic_bytes"):
        parser.add_argument("--" + name.replace("_", "-"), type=int, default=getattr(ExportLimits(), name))
    for name in ("max_seconds", "request_timeout"):
        parser.add_argument("--" + name.replace("_", "-"), type=float, default=getattr(ExportLimits(), name))
    args = parser.parse_args(argv)
    supplied = (args.input, args.destination, args.runtime, args.model)
    if args.describe and any(value is not None for value in supplied):
        parser.error("--describe cannot be combined with export paths")
    if not args.describe and any(value is None for value in supplied):
        parser.error("--input, --destination, --runtime and --model are required")
    cancel = threading.Event()
    old_handlers = {}
    try:
        if args.describe:
            wire = (json.dumps(describe(), sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode()
            if len(wire) > 4096:
                raise RuntimeError("description exceeds byte limit")
            sys.stdout.buffer.write(wire)
            sys.stdout.buffer.flush()
            return 0
        for number in (signal.SIGTERM, signal.SIGINT):
            old_handlers[number] = signal.signal(number, lambda signum, frame: cancel.set())
        limits = ExportLimits(**{key: getattr(args, key) for key in asdict(ExportLimits())})
        export_f32(args.input, args.destination, args.runtime, args.model,
                   backend=args.backend, threads=args.threads, limits=limits, cancel=cancel)
        sys.stdout.write('{"event":"complete","manifest":"manifest.json"}\n')
        sys.stdout.flush()
        return 0
    except ExportCancelled as error:
        sys.stderr.write("stem export cancelled\n")
        if any("retain" in note for note in getattr(error, "__notes__", ())):
            sys.stderr.write("Cleanup unproven; retain input/job for parent verification\n")
        return 130
    except Exception as error:
        # Diagnostic text is not a protocol or success receipt; no unbounded dump.
        retained = "; cleanup unproven; retain input/job" if any(
            "retain" in note for note in getattr(error, "__notes__", ())) else ""
        detail = (type(error).__name__ + ": " + str(error)[:800] + retained).replace("\n", " ").encode(errors="replace")[:1024]
        sys.stderr.buffer.write(detail + b"\n")
        sys.stderr.buffer.flush()
        return 1
    finally:
        for number, previous in old_handlers.items():
            signal.signal(number, previous)


if __name__ == "__main__":
    raise SystemExit(main())

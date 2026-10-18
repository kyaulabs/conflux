#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Replay one normalized soak window in one persistent native Vulkan process.

No StemWorker, Load, UI/audio child, seeks, cancellation, or policy changes.
Prepare/run only after other GPU benchmarks have stopped. Source preparation
mirrors tools/stems/worker.py _source/_read_window without starting a supervisor.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import sys
import time
import wave

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))
sys.path.insert(0, str(REPOSITORY / "tools/benchmarks"))

import numpy as np
from tools.stems.worker import NativeRuntime, RATE, WINDOW, HOP, CHANNELS, _signature
from tools.benchmarks.worker_support import ProcessSampler, create_fixture
from tools.benchmarks.stem_worker import conditions, percentiles

FIXTURE_FRAMES = 8 * HOP + 12345
EXPECTED = {
    "runtime": "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6",
    "model": "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6",
    "fixture": "840561b2f6c6cd26ab70b0e2adef629f3eee09878333b1e41f901972504546c4",
}
DEBUG_FLAGS = ("MESA_GPU_TRACES", "MESA_GPU_TRACEFILE", "INTEL_DEBUG",
               "GGML_VK_PERF_LOGGER", "GGML_VK_PERF_LOGGER_CONCURRENT",
               "GGML_VK_DEBUG_MARKERS")


def digest_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def atomic_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def normalized_window(path, start):
    """Same blocks, float64 accumulation and float32 operations as StemWorker."""
    signature = _signature(path)
    count, mean, m2 = 0, 0.0, 0.0
    with wave.open(str(path), "rb") as stream:
        if (stream.getparams()[:3] != (CHANNELS, 2, RATE)
                or stream.getcomptype() != "NONE"
                or stream.getnframes() != FIXTURE_FRAMES):
            raise ValueError("fixture does not match the soak source contract")
        frames = stream.getnframes()
        while data := stream.readframes(65536):
            pcm = np.frombuffer(data, dtype="<i2").reshape(-1, CHANNELS)
            mono = pcm.astype(np.float64).mean(axis=1) / 32768.0
            size = mono.size
            average = float(mono.mean())
            delta = average - mean
            m2 += float(np.square(mono - average).sum()) + delta * delta * count * size / (count + size)
            mean += delta * size / (count + size)
            count += size
    if count != frames or _signature(path) != signature:
        raise RuntimeError("fixture changed during source statistics")
    mean = np.float32(mean)
    std = np.float32(math.sqrt(m2 / max(1, count - 1)) + 1e-8)
    samples = np.zeros((WINDOW, CHANNELS), dtype=np.float32)
    with wave.open(str(path), "rb") as stream:
        stream.setpos(start)
        data = stream.readframes(min(WINDOW, frames - start))
    valid = len(data) // (2 * CHANNELS)
    if valid != min(WINDOW, frames - start) or _signature(path) != signature:
        raise RuntimeError("fixture changed during window extraction")
    samples[:valid] = np.frombuffer(data, dtype="<i2").reshape(-1, CHANNELS).astype(np.float32) / 32768
    samples[:valid] -= mean
    samples[:valid] /= std
    if not np.isfinite(samples).all():
        raise RuntimeError("normalized input is not finite")
    samples = np.asarray(samples, dtype="<f4")
    samples.flags.writeable = False
    return samples, {"mean": float(mean), "std": float(std), "valid_frames": valid,
                     "scope": "Whole-source mono sample statistics; normalize valid frames before padding."}


def interrupted(_signum, _frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "model", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--requests", type=int, default=120)
    parser.add_argument("--max-seconds", type=float, default=600,
                        help="Post-ready request admission/response budget; actual elapsed is recorded.")
    parser.add_argument("--request-timeout", type=float, default=30)
    parser.add_argument("--window-start-frame", type=int, default=4 * HOP)
    args = parser.parse_args()
    if (not 1 <= args.requests <= 120 or not math.isfinite(args.max_seconds)
            or not 0 < args.max_seconds <= 600 or not math.isfinite(args.request_timeout)
            or not 0 < args.request_timeout <= 60 or args.window_start_frame < 0
            or args.window_start_frame >= FIXTURE_FRAMES or args.window_start_frame % HOP):
        parser.error("requests 1..120, positive budget <=600s, positive timeout <=60s, source-grid window required")
    inherited_debug = {key: os.environ[key] for key in DEBUG_FLAGS if os.environ.get(key)}
    if inherited_debug:
        parser.error("unset diagnostic GPU flags in this invocation: " + ", ".join(inherited_debug))
    root = args.output.resolve()
    if root.exists() and any(root.iterdir()):
        parser.error("output must be new or empty")
    root.mkdir(parents=True, exist_ok=True)
    args.runtime, args.model = args.runtime.resolve(), args.model.resolve()
    fixture_path = root / "source.wav"
    native = sampler = None
    sampler_started = False
    loop_started = loop_stopped = None
    active_attempt = None
    rows, failures = [], []
    result = {"schema": 1, "status": "starting", "command": [sys.executable, *sys.argv],
              "started_at_utc": datetime.now(timezone.utc).isoformat(), "pins": {},
              "requested_requests": args.requests, "max_replay_seconds": args.max_seconds,
              "request_timeout_seconds": args.request_timeout, "threads": 4,
              "budget_scope": "Starts after native readiness; limits admission and response waiting, not setup/file IO or cleanup. Elapsed overruns are marked budget_exhausted.",
              "backend": "vulkan", "window_start_frame": args.window_start_frame,
              "fixture_frames": FIXTURE_FRAMES, "window_frames": WINDOW,
              "sample_interval_seconds": 0.5, "numpy_version": np.__version__,
              "conditions_at_start": conditions(),
              "inherited_GPU_environment": {key: value for key, value in os.environ.items()
                  if key.startswith(("MESA_", "INTEL_", "GGML_VK_", "OMP_", "GOMP_"))},
              "scope": "Native-only fixed-window replay; no audio/UI load or worker controls. No machine policy is changed."}
    previous_int = signal.getsignal(signal.SIGINT)
    previous_term = signal.signal(signal.SIGTERM, interrupted)
    try:
        for name, path in (("runtime", args.runtime), ("model", args.model),
                           ("driver", Path(__file__)),
                           ("worker_source", REPOSITORY / "tools/stems/worker.py"),
                           ("sampler_source", REPOSITORY / "tools/benchmarks/worker_support.py"),
                           ("shared_benchmark_source", REPOSITORY / "tools/benchmarks/stem_worker.py")):
            actual = digest_file(path)
            result["pins"][name] = {"path": str(path), "sha256": actual, "bytes": path.stat().st_size}
            if name in EXPECTED and actual != EXPECTED[name]:
                raise RuntimeError(f"{name} SHA256 differs from the frozen soak artifact")
        result["fixture"] = create_fixture(fixture_path, FIXTURE_FRAMES / RATE)
        if result["fixture"]["sha256"] != EXPECTED["fixture"]:
            raise RuntimeError("fixture SHA256 differs from the soak source")
        samples, statistics = normalized_window(fixture_path, args.window_start_frame)
        result["normalization"] = statistics
        samples.tofile(root / "input.f32")
        input_sha = digest_file(root / "input.f32")
        result["input"] = {"path": str(root / "input.f32"), "sha256": input_sha,
                           "bytes": (root / "input.f32").stat().st_size,
                           "format": "F32LE interleaved stereo", "immutable_in_memory": True}
        fixture_path.unlink()
        native = NativeRuntime(args.runtime, args.model, "vulkan", 4, args.request_timeout,
                               passive_openmp=True)
        ready = result["runtime_ready"] = native.start()
        if (ready.get("fence_wait_mode") != "blocking" or ready.get("backend") != "vulkan"
                or ready.get("threads") != 4 or ready.get("weight_storage") != "native"
                or ready.get("process_environment") != {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"}):
            raise RuntimeError("native ready metadata differs from the frozen execution policy")
        sampler = ProcessSampler(lambda: native.process.pid, 0.5,
                                 trace_path=root / "native-trace.jsonl", include_system=True)
        sampler.__enter__()
        sampler_started = True
        loop_started = time.monotonic()
        deadline = loop_started + args.max_seconds
        reference_hash = None
        with (root / "requests.jsonl").open("x") as log:
            for request_id in range(1, args.requests + 1):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    result.update(status="budget_exhausted", termination_reason="replay wall budget")
                    break
                native.timeout = min(args.request_timeout, remaining)
                began = time.monotonic()
                active_attempt = {"request_id": request_id, "started_at": began,
                                  "effective_response_timeout_seconds": native.timeout}
                output, metrics = native.infer(samples, request_id)
                finished = time.monotonic()
                output_hash = hashlib.sha256(output.tobytes()).hexdigest()
                if reference_hash is None:
                    reference_hash = output_hash
                row = {"request_id": request_id, "started_at": began, "finished_at": finished,
                       "round_trip_ms": (finished - began) * 1000, "metrics": metrics,
                       "source_sha256": result["fixture"]["sha256"], "input_sha256": input_sha,
                       "output_sha256": output_hash, "matches_first_output": output_hash == reference_hash}
                rows.append(row)
                log.write(json.dumps(row, allow_nan=False) + "\n")
                log.flush()
                if output_hash != reference_hash:
                    raise RuntimeError("identical native input produced different output bytes")
                active_attempt = None
            else:
                result["status"] = "ok"
        loop_stopped = time.monotonic()
        if loop_stopped > deadline:
            result.update(status="budget_exhausted", termination_reason="actual replay elapsed exceeded budget")
    except KeyboardInterrupt:
        result["status"] = "interrupted"
        if active_attempt is not None:
            result["failed_attempt_context"] = {**active_attempt, "failure_observed_at": time.monotonic()}
    except Exception as error:
        result["status"] = "error"
        failures.append({"phase": "replay", "error": f"{type(error).__name__}: {error}"})
        if active_attempt is not None:
            result["failed_attempt_context"] = {**active_attempt, "failure_observed_at": time.monotonic()}
    finally:
        # Repeated stops must not interrupt native termination or durable reporting.
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        if loop_started is not None and loop_stopped is None:
            loop_stopped = time.monotonic()
        if sampler_started:
            try:
                result["resources"] = sampler.stop()
                if result["resources"]["trace"]["errors"]:
                    raise RuntimeError("requested host trace is incomplete")
            except Exception as error:
                failures.append({"phase": "sampler", "error": str(error)})
        if native is not None:
            try:
                native.stderr.flush()
                stderr_path = native.root / "runtime.stderr"
                size = stderr_path.stat().st_size
                with stderr_path.open("rb") as stream:
                    stream.seek(max(0, size - 1024 * 1024))
                    (root / "runtime.stderr").write_bytes(stream.read(1024 * 1024))
                result["stderr_capture"] = {"original_bytes": size, "retained_tail_limit_bytes": 1024 * 1024}
            except Exception as error:
                failures.append({"phase": "stderr_capture", "error": str(error)})
            try:
                native.close()
            except Exception as error:
                failures.append({"phase": "native_cleanup", "error": str(error)})
        try:
            fixture_path.unlink(missing_ok=True)
        except OSError as error:
            failures.append({"phase": "fixture_cleanup", "error": str(error)})
        result.update(requests=rows, completed_requests=len(rows), failures=failures,
                      replay_started_monotonic=loop_started, replay_stopped_monotonic=loop_stopped,
                      replay_seconds=(loop_stopped - loop_started) if loop_started is not None else None,
                      round_trip_percentiles_ms=percentiles([row["round_trip_ms"] for row in rows]),
                      graph_percentiles_ms=percentiles([row["metrics"]["graph_ms"] for row in rows]),
                      conditions_at_finish=conditions(), finished_at_utc=datetime.now(timezone.utc).isoformat())
        if failures and result["status"] != "interrupted":
            result["status"] = "error"
        try:
            atomic_json(root / "result.json", result)
        finally:
            signal.signal(signal.SIGINT, previous_int)
            signal.signal(signal.SIGTERM, previous_term)
    print(json.dumps({key: result[key] for key in
        ("status", "completed_requests", "replay_seconds", "round_trip_percentiles_ms", "graph_percentiles_ms", "failures")}, indent=2))
    return 0 if result["status"] == "ok" else 130 if result["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

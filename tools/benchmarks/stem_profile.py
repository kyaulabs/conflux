#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Repeat one identical native window under audio/UI load to profile graph stalls."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import time
import wave

import numpy as np

from stem_worker import Load, conditions, percentiles
from tools.stems.worker import NativeRuntime, RATE, WINDOW
from worker_support import ProcessSampler, create_fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "model", "ui", "audio-probe", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--renderer", choices=("opengl", "vulkan"), default="opengl")
    parser.add_argument("--requests", type=int, default=30)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--quantum", type=int, choices=(256, 512), default=256)
    parser.add_argument("--passive-openmp", action="store_true",
                        help="Park idle OpenMP workers in the native Vulkan child.")
    args = parser.parse_args()
    if not 1 <= args.requests <= 120 or not 1 <= args.threads <= 64:
        parser.error("requests must be 1..120 and threads 1..64")
    root = args.output.resolve()
    if root.exists() and any(root.iterdir()):
        parser.error("output must be new or empty")
    root.mkdir(parents=True, exist_ok=True)
    fixture = create_fixture(root / "source.wav", WINDOW / RATE)
    with wave.open(fixture["path"], "rb") as stream:
        pcm = np.frombuffer(stream.readframes(WINDOW), dtype="<i2").reshape(-1, 2).astype(np.float32) / 32768
    mono = pcm.astype(np.float64).mean(axis=1)
    samples = (pcm - np.float32(mono.mean())) / np.float32(mono.std(ddof=1) + 1e-8)
    result = {"schema": 1, "fixture": fixture, "renderer": args.renderer,
              "passive_openmp_requested": args.passive_openmp,
              "caller_openmp_environment": {key: os.environ.get(key) for key in
                                     ("OMP_WAIT_POLICY", "GOMP_SPINCOUNT", "OMP_NUM_THREADS")},
              "input_sha256": hashlib.sha256(samples.tobytes()).hexdigest(),
              "conditions_at_start": conditions(), "requests": []}
    for name, path in (("runtime", args.runtime), ("model", args.model)):
        with path.open("rb") as stream:
            result[name + "_sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
    load = Load(args, root)
    native = None
    sampler = None
    try:
        load.start()
        native = NativeRuntime(args.runtime.resolve(), args.model.resolve(), "vulkan", args.threads, 30,
                               passive_openmp=args.passive_openmp)
        result["runtime_ready"] = native.start()
        sampler = ProcessSampler(lambda: native.process.pid,
                                 trace_path=root / "native-trace.jsonl", include_system=True)
        with sampler, (root / "requests.jsonl").open("w") as log:
            reference = None
            for request in range(args.requests):
                load.check()
                began = time.monotonic()
                output, metrics = native.infer(samples, request)
                finished = time.monotonic()
                if reference is None:
                    reference = output.copy()
                delta = float(np.max(np.abs(output - reference)))
                if delta != 0:
                    raise RuntimeError("identical native input produced different output")
                record = {"request": request, "started_at": began, "finished_at": finished,
                          "round_trip_ms": (finished - began) * 1000, "metrics": metrics,
                          "max_abs_difference": delta,
                          "output_sha256": hashlib.sha256(output.tobytes()).hexdigest()}
                result["requests"].append(record)
                log.write(json.dumps(record) + "\n")
                log.flush()
        result["round_trip_percentiles_ms"] = percentiles([r["round_trip_ms"] for r in result["requests"]])
        result["graph_percentiles_ms"] = percentiles([r["metrics"]["graph_ms"] for r in result["requests"]])
        result["status"] = "ok"
    except KeyboardInterrupt:
        result.update(status="interrupted")
    except Exception as error:
        result.update(status="error", error=str(error))
    finally:
        if native:
            try:
                native.close()
            except Exception as error:
                result.update(status="error", close_error=str(error))
        if sampler and sampler._finished is not None:
            try:
                result["resources"] = sampler.report()
                if result["resources"]["trace"]["errors"]:
                    result.update(status="error", sampling_error="requested profile trace was incomplete")
            except Exception as error:
                result.update(status="error", sampling_error=str(error))
        try:
            result["load"] = load.stop()
            if result["load"]["errors"]:
                result["status"] = "error"
        except Exception as error:
            result.update(status="error", load_error=str(error))
        result["conditions_at_finish"] = conditions()
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in ("status", "error", "round_trip_percentiles_ms", "graph_percentiles_ms") if key in result}, indent=2))
    return 0 if result["status"] == "ok" else 130 if result["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

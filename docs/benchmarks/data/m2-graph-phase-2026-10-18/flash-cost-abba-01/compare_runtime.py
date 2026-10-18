#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Pinned, sequential baseline/diagnostic PCM comparison; dry by default.

This is an ignored experiment driver, not a product test or latency gate. Only
--run can import NumPy/the experimental worker or create a native process.
The caller owns hardware admission and guard monitoring. No playback, host
queries, power/profile changes, downloads, or source modifications occur here.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import signal
import stat
import sys
import time

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve()
ROOT = HERE.parents[2]
UPSTREAM = Path("/tmp/conflux-stem-m0/audio.cpp-0097d365a00b52a6991ad7618f0a04e8d2935e47")
BASELINE = Path("/tmp/conflux-stem-m0/conflux-stem-runtime-blocking")
DIAGNOSTIC = Path("/tmp/conflux-stem-phase-timing-2026-10-18/build/conflux-stem-runtime")
MODEL = Path("/tmp/conflux-stem-m0/models/htdemucs-f16.gguf")
BASELINE_SHA = "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6"
MODEL_SHA = "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"
REVISION = "0097d365a00b52a6991ad7618f0a04e8d2935e47"
MANIFEST = ROOT / "docs/benchmarks/data/user-tracks-2026-10-17/series/manifest.json"
NORMALIZER = ROOT / "benchmarks-local/music-profile-2026-10-17/interleaved_replay.py"
ASSETS = ROOT / "benchmarks-local/user-tracks-2026-10-17/assets"
TRACKS = (
    ("club_xl", "1b959a945c5169b3bbfcfb8b1c1c7a3b8ca5aa885bfda36a56d967d74d700023"),
    ("when_the_light_breaks", "3448e91b9af700e250a73c752ce9ccff2d413add50ad46c928cb9728ea3ff1cf"),
    ("under_my_skin", "afce9859f42deaf1a94346c41a9501a244ba8a5be513920b3b9da799f3abb879"),
    ("stfu", "30a3b35ca65368d698a8b7ae0f24761cf5491d9174243d03759b2ed5103c1fda"),
)
STARTS = (0, 257985, 515970, 773955)
FRAMES, RATE, WINDOW, CHANNELS, THREADS = 1031940, 44100, 343980, 2, 2
ASSET_BYTES = 4127804
REQUIRED_SOURCES = (
    HERE, NORMALIZER, MANIFEST,
    ROOT / "tools/stems/worker.py",
    ROOT / "tools/benchmarks/native_replay.py",
    ROOT / "tools/benchmarks/stem_worker.py",
    ROOT / "tools/benchmarks/worker_support.py",
    ROOT / "tools/runtime/htdemucs_segment.cpp",
    ROOT / "tools/runtime/CMakeLists.txt",
    ROOT / "tools/runtime/BlockingVulkanWait.cmake",
    ROOT / "tools/runtime/GraphPhaseTiming.cmake",
    UPSTREAM / "CMakeLists.txt",
    UPSTREAM / "include/engine/models/demucs/pipeline.h",
    UPSTREAM / "src/models/demucs/pipeline.cpp",
    UPSTREAM / "external/ggml/src/ggml-vulkan/ggml-vulkan.cpp",
)
COMMON_TIMINGS = ("elapsed_ms", "frontend_ms", "graph_ms", "postprocess_ms", "graph_rebuild_ms")
PHASE_TIMINGS = ("inference_wall_ms", "inference_cpu_ms", "graph_upload_ms", "graph_compute_ms", "graph_readback_ms")
SWITCH_COUNTS = ("voluntary_context_switches", "involuntary_context_switches")
DEBUG_FLAGS = ("MESA_GPU_TRACES", "MESA_GPU_TRACEFILE", "INTEL_DEBUG",
               "GGML_VK_PERF_LOGGER", "GGML_VK_PERF_LOGGER_CONCURRENT", "GGML_VK_DEBUG_MARKERS")
HEX = re.compile(r"[0-9a-f]{64}\Z")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def utc():
    return datetime.now(timezone.utc).isoformat()


def check_deadline(deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("comparison total time budget exhausted")
    return remaining


def signature(value):
    return (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)


def inspect_file(path, maximum, deadline, *, expected=None, keep=False):
    """Bounded regular-file read/hash of one retained nonblocking descriptor."""
    path = Path(path)
    require(path.is_absolute(), f"pin path must be absolute: {path}")
    check_deadline(deadline)
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode), f"pin must be a regular file: {path}")
        require(0 < before.st_size <= maximum, f"pin exceeds size bound or is empty: {path}")
        digest, contents, offset = hashlib.sha256(), bytearray(), 0
        while offset < before.st_size:
            check_deadline(deadline)
            block = os.read(fd, min(1024 * 1024, before.st_size - offset))
            require(bool(block), f"pin shortened while reading: {path}")
            offset += len(block)
            digest.update(block)
            if keep:
                contents.extend(block)
        after = os.fstat(fd)
        require(signature(before) == signature(after), f"pin changed while reading: {path}")
        # Also reject ordinary pathname replacement during the retained-fd read.
        require(signature(after) == signature(os.stat(path, follow_symlinks=False)),
                f"pin pathname changed while reading: {path}")
        actual = digest.hexdigest()
        require(expected is None or actual == expected, f"SHA256 mismatch: {path}")
        record = {"path": str(path), "bytes": before.st_size, "sha256": actual,
                  "device": before.st_dev, "inode": before.st_ino,
                  "mtime_ns": before.st_mtime_ns, "ctime_ns": before.st_ctime_ns}
        return record, bytes(contents) if keep else None
    finally:
        os.close(fd)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def decode_json(contents):
    def invalid_constant(value):
        raise ValueError(f"invalid JSON number: {value}")
    return json.loads(contents, object_pairs_hook=unique_object,
                      parse_constant=invalid_constant)


def source_specs(path, deadline):
    pin, data = inspect_file(path, 64 * 1024, deadline, keep=True)
    mapping = decode_json(data)
    require(isinstance(mapping, dict) and len(REQUIRED_SOURCES) <= len(mapping) <= 64,
            "source-pins must map 15..64 absolute file paths to SHA256 strings")
    for name, digest in mapping.items():
        require(isinstance(name, str) and Path(name).is_absolute()
                and str(Path(name)) == name and isinstance(digest, str) and HEX.fullmatch(digest),
                "invalid source-pins path or SHA256")
    require(all(str(source) in mapping for source in REQUIRED_SOURCES), "required source pin missing")
    # The pin configuration must not redefine the fixed executable/model/assets.
    forbidden = {str(BASELINE), str(DIAGNOSTIC), str(MODEL)}
    forbidden.update(str(ASSETS / f"{label}-30s-23p4s.wav") for label, _ in TRACKS)
    require(not forbidden.intersection(mapping), "source-pins cannot override fixed artifact pins")
    return pin, mapping


def capture_pins(specs, deadline, *, verify=True):
    return {label: inspect_file(path, limit, deadline, expected=expected if verify else None)[0]
            for label, (path, expected, limit) in specs.items()}


def load_track_plan(deadline):
    _, data = inspect_file(MANIFEST, 1024 * 1024, deadline, keep=True)
    manifest = decode_json(data)
    rows = manifest.get("tracks")
    require(isinstance(rows, list) and len(rows) == 4, "authoritative manifest requires four tracks")
    plan = manifest.get("excerpt_plan", {})
    require(plan.get("frames") == FRAMES and plan.get("sample_rate") == RATE
            and plan.get("channels") == CHANNELS and plan.get("sample_bytes") == 2
            and plan.get("looping") is False, "authoritative excerpt geometry changed")
    result = []
    for row, (label, digest) in zip(rows, TRACKS):
        path = ASSETS / f"{label}-30s-23p4s.wav"
        pin = row.get("input_pin", {})
        require(row.get("id") == label and row.get("input_wav") == str(path)
                and pin.get("path") == str(path) and pin.get("sha256") == digest
                and pin.get("bytes") == ASSET_BYTES and pin.get("frames") == FRAMES
                and pin.get("sample_rate") == RATE and pin.get("channels") == CHANNELS
                and pin.get("sample_bytes") == 2, "authoritative track order/pin differs")
        result.append((label, path))
    return result


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    # The fresh owned output directory has one writer. Reuse its temporary file
    # if a signal interrupted an earlier checkpoint before the atomic replace.
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")
    temporary.replace(path)


def checkpoint(output, report):
    atomic_json(output / "result.json", report)


def validate_number(record, name, *, integral=False):
    value = record.get(name)
    require(type(value) in (int, float) and math.isfinite(value) and value >= 0
            and (not integral or int(value) == value), f"invalid native metric: {name}")


def prepare_cases(track_plan, deadline):
    # Import only after executable/model/source/asset verification and explicit run.
    sys.path.insert(0, str(ROOT))
    sys.path.insert(0, str(ROOT / "tools/benchmarks"))
    import numpy as np
    from tools.stems.worker import NativeRuntime
    spec = importlib.util.spec_from_file_location("conflux_phase_comparison_normalizer", NORMALIZER)
    normalizer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(normalizer)
    require((normalizer.RATE, normalizer.WINDOW, normalizer.HOP, normalizer.CHANNELS)
            == (RATE, WINDOW, STARTS[1], CHANNELS), "normalizer constants differ")
    cases = []
    for label, path in track_plan:
        for start in STARTS:
            check_deadline(deadline)
            samples, statistics = normalizer.normalized_window(path, start, FRAMES)
            valid = min(WINDOW, FRAMES - start)
            require(samples.shape == (WINDOW, CHANNELS) and samples.dtype.str == "<f4"
                    and samples.flags.c_contiguous and not samples.flags.writeable
                    and np.isfinite(samples).all(), "invalid normalized input buffer")
            require(statistics["valid_frames"] == valid and np.count_nonzero(samples[valid:]) == 0,
                    "normalized tail differs from valid-before-padding contract")
            for metric in ("mean", "std"):
                validate_number({metric: abs(statistics[metric])}, metric)
            require(statistics["std"] > 0, "normalization standard deviation is not positive")
            cases.append({"id": f"{label}:{start}", "track": label, "start_frame": start,
                          "valid_frames": valid, "shape": [WINDOW, CHANNELS], "dtype": "<f4",
                          "input_sha256": hashlib.sha256(memoryview(samples)).hexdigest(),
                          "normalization": statistics, "samples": samples})
    require(len(cases) == 16, "comparison case count differs")
    return np, NativeRuntime, cases


def save_stderr(native, output, label):
    native.stderr.flush()
    with (native.root / "runtime.stderr").open("rb") as stream:
        total = os.fstat(stream.fileno()).st_size
        stream.seek(max(0, total - 128 * 1024))
        data = stream.read(128 * 1024)
    destination = output / f"{label}.stderr"
    destination.write_bytes(data)
    return {"path": str(destination), "bytes": len(data), "original_bytes": total,
            "truncated": total > len(data), "sha256": hashlib.sha256(data).hexdigest()}


def run_runtime(label, executable, cases, baseline_hashes, np, NativeRuntime, args, output, report, deadline):
    record = {"label": label, "executable": str(executable), "requests": [], "cleanup": {"complete": False}}
    report["runtimes"].append(record)
    native = None
    report["native_started"] = True  # Conservative even if construction fails.
    try:
        native = NativeRuntime(executable, MODEL, "vulkan", THREADS,
                               min(args.request_timeout, check_deadline(deadline)), passive_openmp=True)
        record["pid"] = native.process.pid
        record["temporary_directory"] = str(native.root)
        started = time.monotonic()
        ready = native.start()
        record["startup_wall_ms"] = (time.monotonic() - started) * 1000
        record["ready"] = ready
        require(ready.get("backend") == "vulkan" and ready.get("threads") == THREADS
                and ready.get("revision") == REVISION and ready.get("model") == str(MODEL)
                and ready.get("pid") == native.process.pid and ready.get("weight_storage") == "native",
                "native ready backend/thread/model/revision/PID differs")
        require(ready.get("fence_wait_mode") == "blocking", "native fence wait mode is not blocking")
        require(ready.get("process_environment") == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"},
                "native child passive OpenMP settings differ")
        validate_number(ready, "load_ms")
        checkpoint(output, report)
        for request_id, case in enumerate(cases, 1):
            native.timeout = min(args.request_timeout, check_deadline(deadline))
            samples = case["samples"]
            row = {key: value for key, value in case.items() if key != "samples"}
            row.update(request_id=request_id, input_sha_match=False, output_matches_baseline=False,
                       phase_fields_valid=False)
            record["requests"].append(row)
            produced = None
            try:
                started = time.monotonic()
                produced, metrics = native.infer(samples, request_id)
                row["request_wall_ms"] = (time.monotonic() - started) * 1000
                row["native_metrics"] = metrics
                for name in COMMON_TIMINGS:
                    validate_number(metrics, name)
                if label == "diagnostic":
                    for name in PHASE_TIMINGS:
                        validate_number(metrics, name)
                    for name in SWITCH_COUNTS:
                        validate_number(metrics, name, integral=True)
                    row["phase_fields_valid"] = True
                require(produced.shape == (4, WINDOW, CHANNELS) and produced.dtype.str == "<f4"
                        and produced.flags.c_contiguous and np.isfinite(produced).all(),
                        "native output geometry/dtype/finiteness differs")
                written, _ = inspect_file(native.root / "input.f32", WINDOW * CHANNELS * 4,
                                          deadline, expected=case["input_sha256"])
                require(written["bytes"] == WINDOW * CHANNELS * 4
                        and hashlib.sha256(memoryview(samples)).hexdigest() == case["input_sha256"],
                        "normalized input changed or native input file differs")
                row["input_sha_match"] = True
                row["output_shape"], row["output_dtype"] = list(produced.shape), "<f4"
                row["output_bytes"] = produced.nbytes
                row["output_sha256"] = hashlib.sha256(memoryview(produced)).hexdigest()
                if label == "baseline":
                    baseline_hashes[case["id"]] = row["output_sha256"]
                else:
                    row["output_matches_baseline"] = row["output_sha256"] == baseline_hashes[case["id"]]
                    require(row["output_matches_baseline"], f"PCM SHA256 differs for {case['id']}")
                row["completed"] = True
                report["completed_cases_per_runtime"][label] += 1
                report["total_completed_requests"] += 1
            finally:
                produced = None
                # No separated music/raw buffers are retained in evidence.
                for name in ("input.f32", "output.f32"):
                    (native.root / name).unlink(missing_ok=True)
                checkpoint(output, report)
    finally:
        # Repeated SIGINT/SIGTERM must not interrupt bounded child reaping.
        handlers = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            if native is not None:
                closed = False
                try:
                    try:
                        record["stderr"] = save_stderr(native, output, label)
                    finally:
                        native.close()
                        closed = True
                finally:
                    record["cleanup"] = {"complete": closed and native.process.poll() is not None and not native.root.exists(),
                                         "close_returned": closed, "reaped": native.process.poll() is not None,
                                         "returncode": native.process.returncode,
                                         "temporary_directory_removed": not native.root.exists()}
                require(record["cleanup"]["complete"], "native cleanup incomplete")
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            checkpoint(output, report)


def dry_plan():
    return {"schema": 1, "status": "dry_run", "native_started": False,
            "backend": "vulkan", "threads": THREADS, "run_order": ["baseline", "diagnostic"],
            "planned_cases_per_runtime": 16, "planned_total_requests": 32,
            "required_source_pin_paths": [str(path) for path in REQUIRED_SOURCES],
            "baseline": {"path": str(BASELINE), "sha256": BASELINE_SHA},
            "diagnostic": {"path": str(DIAGNOSTIC), "sha256": "required via --diagnostic-sha256"},
            "model": {"path": str(MODEL), "sha256": MODEL_SHA},
            "track_order": [label for label, _ in TRACKS], "window_start_frames": list(STARTS),
            "required_run_arguments": ["--run", "--diagnostic-sha256 SHA256", "--source-pins FILE", "--output FRESH_DIRECTORY"],
            "scope": "Metadata plan only; no imports of NumPy/worker, asset hashing, host queries or native process."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--diagnostic-sha256")
    parser.add_argument("--source-pins", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--request-timeout", type=float, default=60)
    parser.add_argument("--max-seconds", type=float, default=1200)
    args = parser.parse_args()
    if not args.run:
        print(json.dumps(dry_plan(), indent=2, allow_nan=False))
        return 0
    require(args.diagnostic_sha256 is not None and HEX.fullmatch(args.diagnostic_sha256),
            "--run requires exact lowercase --diagnostic-sha256")
    require(args.source_pins is not None and args.source_pins.is_absolute(), "--run requires absolute --source-pins")
    require(args.output is not None and args.output.is_absolute(), "--run requires absolute fresh --output")
    require(math.isfinite(args.request_timeout) and 5 <= args.request_timeout <= 120, "request timeout must be 5..120 seconds")
    require(math.isfinite(args.max_seconds) and 60 <= args.max_seconds <= 1800, "total budget must be 60..1800 seconds")
    require(not any(os.environ.get(key) for key in DEBUG_FLAGS), "inherited GPU diagnostic flags must be unset")
    args.output.mkdir(mode=0o700, parents=False, exist_ok=False)
    output = args.output
    started = time.monotonic()
    deadline = started + args.max_seconds
    report = {"schema": 1, "status": "running", "started_at_utc": utc(), "native_started": False,
              "backend": "vulkan", "threads": THREADS, "run_order": ["baseline", "diagnostic"],
              "planned_cases_per_runtime": 16, "planned_total_requests": 32,
              "completed_cases_per_runtime": {"baseline": 0, "diagnostic": 0}, "total_completed_requests": 0,
              "exact_pcm_match": False, "input_sha_match": False, "phase_fields_valid": False,
              "pins_unchanged": False, "cleanup_complete": False, "pins_before": {}, "pins_after": {},
              "runtimes": [], "errors": [], "request_timeout_seconds": args.request_timeout,
              "pins_before_each_runtime": {}, "post_cleanup_pin_budget_seconds": 60,
              "max_seconds": args.max_seconds,
              "scope": "16 requests per persistent runtime, baseline then diagnostic, one child at a time. Exact PCM instrumentation check; no live-stem p99, audio/UI-load, GPU-timestamp or perceptual qualification. Startup, runtime order and thermal confounding are not removed. Phase units are CPU wall milliseconds; compute includes explicit backend wait. No playback or host policy/device changes."}
    specs = {}
    handlers = {}
    def interrupted(signum, _frame):
        raise KeyboardInterrupt(f"signal {signum}")
    try:
        handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
        config_pin, sources = source_specs(args.source_pins, deadline)
        specs = {"baseline": (BASELINE, BASELINE_SHA, 256 * 1024 * 1024),
                 "diagnostic": (DIAGNOSTIC, args.diagnostic_sha256, 256 * 1024 * 1024),
                 "model": (MODEL, MODEL_SHA, 2 * 1024 * 1024 * 1024),
                 "source_pin_configuration": (args.source_pins, config_pin["sha256"], 64 * 1024)}
        for path, digest in sources.items():
            specs[f"source:{path}"] = (Path(path), digest, 32 * 1024 * 1024)
        for label, digest in TRACKS:
            specs[f"asset:{label}"] = (ASSETS / f"{label}-30s-23p4s.wav", digest, ASSET_BYTES)
        report["pins_before"] = capture_pins(specs, deadline)
        for label, _ in TRACKS:
            require(report["pins_before"][f"asset:{label}"]["bytes"] == ASSET_BYTES, "prepared asset size differs")
        track_plan = load_track_plan(deadline)
        np, NativeRuntime, cases = prepare_cases(track_plan, deadline)
        report["numpy_version"] = np.__version__
        report["python_version"] = sys.version
        report["cases"] = [{key: value for key, value in case.items() if key != "samples"} for case in cases]
        checkpoint(output, report)
        baseline_hashes = {}
        for label, executable in (("baseline", BASELINE), ("diagnostic", DIAGNOSTIC)):
            report["pins_before_each_runtime"][label] = capture_pins(specs, deadline)
            require(report["pins_before_each_runtime"][label] == report["pins_before"],
                    f"pins changed before {label} launch")
            run_runtime(label, executable, cases, baseline_hashes, np, NativeRuntime,
                        args, output, report, deadline)
        report["exact_pcm_match"] = all(row["output_matches_baseline"] for row in report["runtimes"][1]["requests"])
        report["input_sha_match"] = all(row["input_sha_match"] for runtime in report["runtimes"] for row in runtime["requests"])
        report["phase_fields_valid"] = all(row["phase_fields_valid"] for row in report["runtimes"][1]["requests"])
        report["cleanup_complete"] = len(report["runtimes"]) == 2 and all(runtime["cleanup"]["complete"] for runtime in report["runtimes"])
        require(report["completed_cases_per_runtime"] == {"baseline": 16, "diagnostic": 16}
                and report["total_completed_requests"] == 32 and report["exact_pcm_match"]
                and report["input_sha_match"] and report["phase_fields_valid"] and report["cleanup_complete"],
                "comparison completeness checks failed")
        report["status"] = "passed"
    except KeyboardInterrupt as error:
        report["status"] = "cancelled"
        report["errors"].append({"type": type(error).__name__, "message": str(error)[:8192]})
    except Exception as error:
        report["status"] = "failed"
        report["errors"].append({"type": type(error).__name__, "message": str(error)[:8192]})
    finally:
        # Always retain after-pins, including failures/cancellation, without a native child.
        ignored = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            if specs:
                try:
                    report["pins_after"] = capture_pins(specs, time.monotonic() + 60, verify=False)
                    report["pins_unchanged"] = report["pins_before"] == report["pins_after"] and bool(report["pins_before"])
                except Exception as error:
                    report["errors"].append({"type": type(error).__name__, "message": f"after-pins: {str(error)[:8000]}"})
            if report["status"] == "passed" and not report["pins_unchanged"]:
                report["status"] = "failed"
                report["errors"].append({"type": "ValueError", "message": "pins changed or after-pin capture failed"})
            report["cleanup_complete"] = bool(report["runtimes"]) and all(runtime["cleanup"]["complete"] for runtime in report["runtimes"])
            report["finished_at_utc"] = utc()
            report["elapsed_seconds"] = time.monotonic() - started
            checkpoint(output, report)
        finally:
            for sig, handler in (handlers or ignored).items():
                signal.signal(sig, handler)
    print(json.dumps({"status": report["status"], "result": str(output / "result.json"),
                      "total_completed_requests": report["total_completed_requests"]}, allow_nan=False))
    return 0 if report["status"] == "passed" else 2 if report["status"] == "cancelled" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"comparison admission failed: {error}", file=sys.stderr)
        raise SystemExit(1)

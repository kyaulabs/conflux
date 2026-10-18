#!/usr/bin/env python3
"""Bounded two-input native Vulkan diagnostic; no host policy changes.

--prepare-only validates normalization without a native process or host queries.
The real replay is 12 ABBA blocks/48 requests in one persistent native process.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import wave

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools/benchmarks"))
import numpy as np
from tools.benchmarks import native_replay as replay
from tools.benchmarks.stem_worker import conditions, percentiles
from tools.benchmarks.worker_support import ProcessSampler, create_fixture
from tools.stems.worker import NativeRuntime, StemWorker, RATE, WINDOW, HOP, CHANNELS, _signature

MUSIC = ROOT / "benchmarks-local/user-tracks-2026-10-17/assets/when_the_light_breaks-30s-23p4s.wav"
READY_HELPER = ROOT / "benchmarks-local/user-tracks-2026-10-17/normal_ready.py"
MUSIC_FRAMES, MUSIC_START, SYNTHETIC_START = 1031940, 773955, 4 * HOP
MAX_SECONDS, REQUEST_TIMEOUT, BLOCKS = 300, 30, 12
ORDER = [label for _ in range(BLOCKS) for label in ("synthetic", "music", "music", "synthetic")]
EXPECTED = {
    "runtime": "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6",
    "model": "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6",
    "worker_source": "308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8",
    "sampler_source": "8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b",
    "shared_benchmark_source": "87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7",
    "replay_source": "6ee7408415fdf0366cd3282fe57f94409d8f9bc34eb2967eb0e622bf2cf1757e",
    "readiness_source": "4e94000d5966357c653a6ef64729f9b513df723ce0badb17e496d4b85c07135f",
    "music_source": "3448e91b9af700e250a73c752ce9ccff2d413add50ad46c928cb9728ea3ff1cf",
}


def require(test, message):
    if not test:
        raise ValueError(message)


def pin(path):
    path = Path(path).resolve()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": replay.digest_file(path)}


def utc():
    return datetime.now(timezone.utc).isoformat()


def normalized_window(path, start, expected_frames):
    """Copy of pinned native_replay.normalized_window with explicit source length."""
    require(isinstance(start, int) and 0 <= start < expected_frames and start % HOP == 0, "Invalid source-grid window")
    signature = _signature(path)
    count, mean, m2 = 0, 0.0, 0.0
    with wave.open(str(path), "rb") as stream:
        if (stream.getparams()[:3] != (CHANNELS, 2, RATE)
                or stream.getcomptype() != "NONE" or stream.getnframes() != expected_frames):
            raise ValueError("source does not match the explicit frame contract")
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
        raise RuntimeError("source changed during source statistics")
    mean = np.float32(mean)
    std = np.float32(math.sqrt(m2 / max(1, count - 1)) + 1e-8)
    samples = np.zeros((WINDOW, CHANNELS), dtype=np.float32)
    with wave.open(str(path), "rb") as stream:
        stream.setpos(start)
        data = stream.readframes(min(WINDOW, frames - start))
    valid = len(data) // (2 * CHANNELS)
    if valid != min(WINDOW, frames - start) or _signature(path) != signature:
        raise RuntimeError("source changed during window extraction")
    samples[:valid] = np.frombuffer(data, dtype="<i2").reshape(-1, CHANNELS).astype(np.float32) / 32768
    samples[:valid] -= mean
    samples[:valid] /= std
    if not np.isfinite(samples).all():
        raise RuntimeError("normalized input is not finite")
    samples = np.asarray(samples, dtype="<f4")
    samples.flags.writeable = False
    return samples, {"mean": float(mean), "std": float(std), "valid_frames": valid,
                     "scope": "Whole-source mono sample statistics; normalize valid frames before padding."}


def prepare(output):
    with tempfile.TemporaryDirectory(prefix="own-fixture-", dir=output) as temporary:
        fixture_path = Path(temporary) / "synthetic.wav"
        fixture = create_fixture(fixture_path, replay.FIXTURE_FRAMES / RATE)
        require(fixture["sha256"] == replay.EXPECTED["fixture"], "Synthetic fixture differs from previous native replay")
        synthetic, stats = replay.normalized_window(fixture_path, SYNTHETIC_START)
        copied, copied_stats = normalized_window(fixture_path, SYNTHETIC_START, replay.FIXTURE_FRAMES)
        require(np.array_equal(synthetic, copied) and stats == copied_stats, "Explicit-frame normalization differs from proven replay")
        source_identity = {**pin(fixture_path), "frames": replay.FIXTURE_FRAMES, "temporary_source_removed_after_preparation": True}
    music, music_stats = normalized_window(MUSIC, MUSIC_START, MUSIC_FRAMES)
    source = SimpleNamespace(path=MUSIC, signature=_signature(MUSIC), frames=MUSIC_FRAMES,
                             mean=np.float32(music_stats["mean"]), std=np.float32(music_stats["std"]))
    require(np.array_equal(music, StemWorker._read_window(None, source, MUSIC_START)), "Music extraction differs from worker _read_window")
    require(music_stats["valid_frames"] == HOP and np.count_nonzero(music[HOP:]) == 0, "Music tail padding differs")
    require(len(ORDER) == 48 and ORDER.count("synthetic") == ORDER.count("music") == 24
            and all(ORDER[i:i+4] == ["synthetic", "music", "music", "synthetic"] for i in range(0, 48, 4)), "ABBA plan differs")
    inputs, metadata = {"synthetic": synthetic, "music": music}, {}
    for label, samples in inputs.items():
        require(samples.shape == (WINDOW, CHANNELS) and samples.dtype == np.dtype("<f4")
                and not samples.flags.writeable and np.isfinite(samples).all(), "Invalid immutable normalized buffer")
        metadata[label] = {"normalized_sha256": hashlib.sha256(samples.tobytes()).hexdigest(), "bytes": samples.nbytes,
                           "shape": list(samples.shape), "dtype": "<f4", "immutable_in_memory": True,
                           "source": source_identity if label == "synthetic" else {**pin(MUSIC), "frames": MUSIC_FRAMES},
                           "window_start_frame": SYNTHETIC_START if label == "synthetic" else MUSIC_START,
                           "normalization": stats if label == "synthetic" else music_stats}
    return inputs, metadata, ["Synthetic fixture SHA matches prior replay", "Explicit-frame function matches proven synthetic bytes/statistics",
        "Music bytes match worker _read_window using copied whole-source statistics", "Music valid257985 frames followed by85995 exact-zero padding frames",
        "Two immutable finite little-endian normalized buffers", "12 exact ABBA blocks,24 requests per input"]


def ac_lid():
    started = time.monotonic()
    record = {"AC": None, "lid": None, "started_monotonic": started, "errors": []}
    try:
        record["AC"] = Path("/sys/class/power_supply/AC/online").read_text().strip()
        record["lid"] = subprocess.run(["busctl", "--system", "get-property", "org.freedesktop.login1", "/org/freedesktop/login1",
                              "org.freedesktop.login1.Manager", "LidClosed"], capture_output=True, text=True, check=True, timeout=2).stdout.strip()
    except Exception as error:
        record["errors"].append({"type": type(error).__name__, "error": str(error)})
    record.update(ready=not record["errors"] and record["AC"] == "1" and record["lid"] == "b false",
                  finished_monotonic=time.monotonic(), at_utc=utc())
    return record


def readiness_module():
    spec = importlib.util.spec_from_file_location("music_profile_readiness", READY_HELPER)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def fresh_admission(output):
    proof_path = output / "admission-normal-ready.json"
    record = {"started_monotonic": time.monotonic(), "ready": False}
    try:
        proof = json.loads(proof_path.read_text())
        record["proof"] = pin(proof_path)
        require(proof["status"] == "matched" and proof["profile_mutation"] is False and proof["service_mutation"] is False,
                "Post-preparation normal-readiness proof did not match")
        module = readiness_module()
        row = record["live_normal_readiness"] = module.capture(Path(proof["package_zone"]), time.monotonic() + 12)
        expected = record["same_guard_service_identity"] = proof["guard_service_identity"]
        matched = module.ready(row, expected)
        record["AC_lid"] = ac_lid()
        record["ready"] = module.identity(row) == expected and matched and record["AC_lid"]["ready"]
        require(record["ready"], "Fresh <=55C/balanced/no-holds/same recovered guard/AC/lid admission failed")
        return record
    except Exception as error:
        record["error"] = {"type": type(error).__name__, "message": str(error)}
        raise
    finally:
        record["finished_monotonic"] = time.monotonic()
        replay.atomic_json(output / "fresh-admission.json", record)


def interrupted(_signum, _frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "model", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    require(not {key: os.environ[key] for key in replay.DEBUG_FLAGS if os.environ.get(key)}, "Unset inherited diagnostic GPU flags; frozen replay requires their absence")
    output = args.output.resolve(); require(not output.exists(), "Use a fresh output directory")
    output.mkdir(parents=True)
    native = sampler = None
    loop_start = loop_stop = active = None
    rows, failures, reference_hashes = [], [], {}
    result = {"schema": 1, "status": "starting", "started_at_utc": utc(), "pins": {}, "backend": "vulkan", "threads": 4,
              "planned_requests": 48, "planned_per_input": 24, "order": ORDER, "post_ready_budget_seconds": MAX_SECONDS,
              "request_timeout_seconds": REQUEST_TIMEOUT, "numpy_version": np.__version__, "prepare_only": args.prepare_only,
              "native_started": False, "scope": "Fixed normalized-input diagnostic; one persistent backend, ABBA order. No worker controls/audio/UI load, host policy mutation or live p99 qualification.24 observations/input do not remove thermal/clock/order confounding.",
              "budget_scope": "300s starts immediately after native readiness; includes requests, hashing and logging. Setup/readiness/startup/cleanup are outside this budget and separately recorded."}
    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    phase_log = (output / "phases.jsonl").open("x")
    def phase(name, **fields):
        record = {"phase": name, "timestamp_monotonic": time.monotonic(), "at_utc": utc(), **fields}
        phase_log.write(json.dumps(record, allow_nan=False) + "\n"); phase_log.flush()
    try:
        paths = {"runtime": args.runtime, "model": args.model, "driver": Path(__file__), "worker_source": ROOT / "tools/stems/worker.py",
                 "sampler_source": ROOT / "tools/benchmarks/worker_support.py", "shared_benchmark_source": ROOT / "tools/benchmarks/stem_worker.py",
                 "replay_source": ROOT / "tools/benchmarks/native_replay.py", "readiness_source": READY_HELPER, "music_source": MUSIC}
        for name, path in paths.items():
            result["pins"][name] = pin(path)
            require(name not in EXPECTED or result["pins"][name]["sha256"] == EXPECTED[name], "Frozen artifact differs: " + name)
        phase("preparation_started")
        inputs, result["inputs"], result["preparation_checks"] = prepare(output)
        phase("preparation_finished")
        if args.prepare_only:
            result["status"] = "prepared"
        else:
            phase("normal_readiness_started", maximum_seconds=600, after_preparation=True)
            ready_code = readiness_module().run("admission", output)
            proof_path = output / "admission-normal-ready.json"
            result["normal_readiness"] = {"returncode": ready_code, "proof": pin(proof_path), "after_preparation": True}
            phase("normal_readiness_finished", **result["normal_readiness"])
            require(ready_code == 0, "Post-preparation normal readiness did not match")
            result["conditions_before_admission"] = conditions()
            result["admission"] = fresh_admission(output)
            phase("native_spawn", admission=result["admission"])
            native = NativeRuntime(args.runtime.resolve(), args.model.resolve(), "vulkan", 4, REQUEST_TIMEOUT, passive_openmp=True)
            result.update(native_started=True, native_pid=native.process.pid, native_temporary_directory=str(native.root))
            sampler = ProcessSampler(lambda: native.process.pid, .5, trace_path=output / "native-trace.jsonl", include_system=True)
            sampler.__enter__()
            ready = result["runtime_ready"] = native.start()
            loop_start = time.monotonic(); deadline = loop_start + MAX_SECONDS
            phase("native_ready", runtime=ready)
            require(ready.get("backend") == "vulkan" and ready.get("threads") == 4 and ready.get("fence_wait_mode") == "blocking"
                    and ready.get("weight_storage") == "native" and ready.get("process_environment") == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"}, "Native ready policy differs")
            result["AC_lid_before_first_request"] = ac_lid()
            require(result["AC_lid_before_first_request"]["ready"], "Fresh AC/open-lid conditions absent before first request")
            with (output / "requests.jsonl").open("x") as log:
                for request_id, label in enumerate(ORDER, 1):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        result["status"] = "budget_exhausted"; break
                    native.timeout = min(REQUEST_TIMEOUT, remaining)
                    phase("request_started", request_id=request_id, input_label=label, block=(request_id-1)//4, position_in_block=(request_id-1)%4)
                    began = time.monotonic()
                    active = {"request_id": request_id, "input_label": label, "started_monotonic": began, "effective_timeout_seconds": native.timeout}
                    values, metrics = native.infer(inputs[label], request_id)
                    finished = time.monotonic()
                    phase("request_finished", request_id=request_id, input_label=label)
                    hash_start = time.monotonic(); output_sha = hashlib.sha256(values.tobytes()).hexdigest(); hash_stop = time.monotonic()
                    reference_hashes.setdefault(label, output_sha)
                    row = {**active, "finished_monotonic": finished, "round_trip_ms": (finished-began)*1000, "metrics": metrics,
                           "input_sha256": result["inputs"][label]["normalized_sha256"], "output_sha256": output_sha,
                           "output_hash_ms": (hash_stop-hash_start)*1000, "matches_first_output_for_input": output_sha == reference_hashes[label]}
                    rows.append(row); log.write(json.dumps(row, allow_nan=False)+"\n"); log.flush()
                    require(row["matches_first_output_for_input"], "Repeated fixed input produced different output bytes: " + label)
                    active = None
                else: result["status"] = "ok"
            loop_stop = time.monotonic()
            if loop_stop > deadline: result["status"] = "budget_exhausted"
    except KeyboardInterrupt:
        result["status"] = "interrupted"
    except Exception as error:
        result["status"] = "error"; failures.append({"type": type(error).__name__, "error": str(error)})
    finally:
        for sig in previous: signal.signal(sig, signal.SIG_IGN)
        if loop_start is not None and loop_stop is None: loop_stop = time.monotonic()
        if active is not None: result["failed_attempt_context"] = {**active, "observed_monotonic": time.monotonic()}
        if sampler is not None:
            try:
                result["resources"] = sampler.stop()
                require(not result["resources"]["trace"]["errors"], "Native trace incomplete")
            except Exception as error: failures.append({"phase": "sampler_stop", "error": str(error)})
        if native is not None:
            try:
                native.stderr.flush(); stderr_path = native.root / "runtime.stderr"; size = stderr_path.stat().st_size
                with stderr_path.open("rb") as stream:
                    stream.seek(max(0,size-1048576)); (output/"runtime.stderr").write_bytes(stream.read(1048576))
                result["stderr_original_bytes"] = size
            except Exception as error: failures.append({"phase": "stderr_capture", "error": str(error)})
            try:
                native.close()
                result["cleanup"] = {"native_pid": native.process.pid, "process_returncode": native.process.returncode,
                    "temporary_directory_gone": not native.root.exists(), "PID_exists_after_close": Path("/proc",str(native.process.pid)).exists()}
                require(native.process.returncode is not None and result["cleanup"]["temporary_directory_gone"]
                        and not result["cleanup"]["PID_exists_after_close"], "Owned native process/directory cleanup incomplete")
            except Exception as error: failures.append({"phase": "native_cleanup", "error": str(error)})
        result.update(requests=rows, completed_requests=len(rows), failures=failures, reference_output_sha256=reference_hashes,
                      replay_started_monotonic=loop_start, replay_stopped_monotonic=loop_stop,
                      replay_seconds=loop_stop-loop_start if loop_start is not None else None, finished_at_utc=utc())
        if (output / "fresh-admission.json").is_file(): result["fresh_admission_capture"] = pin(output / "fresh-admission.json")
        result["per_input"] = {label: {"requests": sum(row["input_label"] == label for row in rows),
            "round_trip_percentiles_ms": percentiles([row["round_trip_ms"] for row in rows if row["input_label"] == label]),
            "graph_percentiles_ms": percentiles([row["metrics"]["graph_ms"] for row in rows if row["input_label"] == label])} for label in ("synthetic","music")}
        if not args.prepare_only: result["conditions_at_finish"] = conditions()
        if failures and result["status"] != "interrupted": result["status"] = "error"
        phase("driver_finished", status=result["status"]); phase_log.close()
        replay.atomic_json(output / "result.json", result)
        for sig, handler in previous.items(): signal.signal(sig, handler)
    print(json.dumps({key: result[key] for key in ("status","completed_requests","replay_seconds","per_input","failures")},indent=2))
    return 0 if result["status"] in ("ok","prepared") else 130 if result["status"] == "interrupted" else 1


if __name__ == "__main__": raise SystemExit(main())

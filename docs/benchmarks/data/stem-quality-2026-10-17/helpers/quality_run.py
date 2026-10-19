#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded M0 music experiment; local float outputs are not a quality verdict."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import sys
import time
import wave

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools/benchmarks"))
import numpy as np
from tools.stems.worker import StemWorker, RATE, WINDOW, HOP, OVERLAP, STEMS
from tools.benchmarks.stem_worker import conditions, json_event, percentiles
from tools.benchmarks.worker_support import ProcessSampler

MAX_SECONDS = 1800


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest}


def wav_info(path):
    info = pin(path)
    with wave.open(str(path), "rb") as audio:
        if audio.getparams()[:3] != (2, 2, RATE) or audio.getcomptype() != "NONE" or audio.getnframes() < 1:
            raise ValueError("Sources/references must be nonempty stereo 44.1 kHz PCM16 WAV: " + str(path))
        info.update(frames=audio.getnframes(), sample_rate=RATE, channels=2, sample_bytes=2)
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("manifest", "runtime", "model", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--backend", choices=("cpu", "vulkan"), required=True)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--request-timeout", type=float, default=120)
    args = parser.parse_args()
    if args.threads != 4 or not math.isfinite(args.request_timeout) or not 0 < args.request_timeout <= 120:
        parser.error("This experiment fixes four threads and requires request timeout in (0,120] seconds")
    began = time.monotonic()
    deadline = began + MAX_SECONDS - 5  # Leave time for bounded child/sampler cleanup.
    output = args.output.resolve()
    if output.exists():
        parser.error("--output must be fresh")
    output.mkdir(parents=True)
    manifest_path = args.manifest.resolve()
    result = {"schema": 1, "status": "starting", "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "backend": args.backend, "threads": args.threads, "request_timeout_seconds": args.request_timeout,
              "maximum_total_seconds": MAX_SECONDS, "work_admission_deadline_seconds": MAX_SECONDS - 5,
              "window_frames": WINDOW, "hop_frames": HOP, "overlap_frames": OVERLAP,
              "normalization": "Whole-source mono sample mean/std; valid samples normalized before tail zero padding; global mean restored per stem",
              "tracks": [], "seeks": [], "runtime_ready": [], "failures": [],
              "musical_quality_verified": False,
              "scope": "Sequential local music preparation; no playback/UI load or sustained-performance qualification. Same-grid tolerance is a numerical regression check, not a quality threshold."}
    worker = native = supervisor = log = None
    active_track = scenario = None
    old_signals = {}

    def check_deadline():
        if time.monotonic() >= deadline:
            raise TimeoutError("Overall quality experiment work budget exhausted")

    def stop(_signum, _frame):
        raise KeyboardInterrupt

    def collect(generation, track, start, *, destination=None, reference=None):
        expected, services, first = start, [], None
        actual_hash, reference_hash = hashlib.sha256(), hashlib.sha256()
        max_difference, square_difference, compared, exact, numeric = 0.0, 0.0, 0, True, True
        while True:
            check_deadline()
            event = worker.poll(min(0.25, max(0, deadline - time.monotonic())))
            if event is None:
                continue
            record = json_event(event)
            record.update(track_id=track["id"], scenario=scenario, consumed_at=time.monotonic())
            log.write(json.dumps(record, allow_nan=False) + "\n")
            log.flush()
            if event["event"] == "error":
                raise RuntimeError(event["message"])
            if event["event"] == "runtime_ready":
                ready = event["runtime"]
                if ready.get("backend") != args.backend or ready.get("threads") != 4:
                    raise RuntimeError("Native execution settings differ from requested backend/threads")
                result["runtime_ready"].append(record)
            if event["event"] == "source_ready" and event["generation"] == generation:
                if event["source_id"] != track["input"]["sha256"] or event["source_frames"] != track["input"]["frames"]:
                    raise RuntimeError("Prepared source identity/length differs from pinned input")
                track.setdefault("source_ready", []).append(record)
            if event["event"] not in ("chunk", "complete"):
                continue
            if event["generation"] != generation:
                raise RuntimeError("Unexpected generation reached experiment consumer")
            if event["source_id"] != track["input"]["sha256"]:
                raise RuntimeError("Source identity changed during collection")
            if event["event"] == "complete":
                if expected != track["input"]["frames"] or event["frame_start"] != expected:
                    raise RuntimeError("Incomplete source/suffix coverage")
                break
            if not worker.is_current(event) or event["model_id"] != result["pins"]["model"]["sha256"]:
                raise RuntimeError("Stale or unexpected-model chunk reached experiment consumer")
            samples, frames = event["samples"], event["frames"]
            if (event["frame_start"] != expected or not 0 < frames <= HOP
                    or samples.dtype != np.dtype("float32") or samples.shape != (4, frames, 2)
                    or expected + frames > track["input"]["frames"] or not np.isfinite(samples).all()):
                raise RuntimeError("Invalid/noncontiguous/nonfinite stem chunk")
            if first is None:
                first = {"ready_monotonic": event["ready_at"], "delivered_monotonic": time.monotonic(),
                         "control_to_ready_ms": event["control_latency_ms"]}
            services.append(event["hop_service_ms"])
            actual_hash.update(samples.tobytes())
            if destination is not None:
                destination[:, expected:expected + frames] = samples
            if reference is not None:
                baseline = reference[:, expected:expected + frames]
                if baseline.shape != samples.shape or not np.isfinite(baseline).all():
                    raise RuntimeError("Invalid continuous comparison suffix")
                reference_hash.update(baseline.tobytes())
                delta = samples.astype(np.float64) - baseline
                max_difference = max(max_difference, float(np.max(np.abs(delta))))
                square_difference += float(np.square(delta).sum())
                compared += delta.size
                exact = exact and np.array_equal(samples, baseline)
                numeric = numeric and bool(np.allclose(samples, baseline, rtol=1e-4, atol=1e-5))
            expected += frames
        summary = {"generation": generation, "start_frame": start, "frames": expected - start,
                   "chunks": len(services), "first_chunk": first,
                   "hop_service_percentiles_ms": percentiles(services), "hop_service_max_ms": max(services, default=None),
                   "chunk_stream_sha256": actual_hash.hexdigest(),
                   "hash_layout": "Successive source-major [4,chunk_frames,2] float32 chunk bytes in delivery order"}
        if reference is not None:
            summary.update(reference_chunk_stream_sha256=reference_hash.hexdigest(), exact=exact,
                           numeric_parity=numeric, max_abs_difference=max_difference,
                           rms_difference=math.sqrt(square_difference / compared) if compared else None,
                           parity_tolerance={"rtol": 1e-4, "atol": 1e-5},
                           parity_scope="Existing same-grid worker regression tolerance; not a musical-quality judgment")
        return summary

    try:
        check_deadline()
        manifest = json.loads(manifest_path.read_text())
        raw_tracks = manifest["tracks"]
        if not isinstance(raw_tracks, list) or not raw_tracks:
            raise ValueError("Manifest tracks must be a nonempty list")
        result["pins"] = {"manifest": pin(manifest_path), "runtime": pin(args.runtime.resolve()),
                          "model": pin(args.model.resolve()), "driver": pin(Path(__file__).resolve()),
                          "worker_source": pin(ROOT / "tools/stems/worker.py"),
                          "shared_benchmark_source": pin(ROOT / "tools/benchmarks/stem_worker.py"),
                          "sampler_source": pin(ROOT / "tools/benchmarks/worker_support.py")}
        tracks, identifiers = [], set()
        for raw in raw_tracks:
            check_deadline()
            identifier = raw["id"]
            if not isinstance(identifier, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", identifier) or identifier in identifiers:
                raise ValueError("Track IDs must be unique safe file-name components")
            identifiers.add(identifier)
            path = Path(raw["input_wav"])
            source = wav_info((manifest_path.parent / path).resolve() if not path.is_absolute() else path.resolve())
            track = {"id": identifier, "input": source, "annotations": raw["annotations"], "references": {}}
            for stem, value in raw.get("reference_wavs", {}).items():
                if stem not in STEMS:
                    raise ValueError("Unknown reference stem: " + stem)
                path = Path(value)
                reference = wav_info((manifest_path.parent / path).resolve() if not path.is_absolute() else path.resolve())
                if reference["frames"] != source["frames"]:
                    raise ValueError("Reference length differs from source: " + identifier)
                track["references"][stem] = reference
            tracks.append(track)
        if args.backend == "vulkan" and tracks[0]["input"]["frames"] <= WINDOW + 1:
            raise ValueError("First Vulkan clip must extend beyond the requested window-edge seek")
        result["conditions_at_start"] = conditions()
        log = (output / "events.jsonl").open("x")
        for signum in (signal.SIGINT, signal.SIGTERM):
            old_signals[signum] = signal.signal(signum, stop)
        worker = StemWorker(args.runtime.resolve(), args.model.resolve(), backend=args.backend,
                            threads=4, request_timeout=args.request_timeout)
        native = ProcessSampler(lambda: worker.backend_pid, 0.5,
                                trace_path=output / "native-trace.jsonl", include_system=True)
        supervisor = ProcessSampler(os.getpid, 0.5)
        native.__enter__()
        supervisor.__enter__()
        for index, track in enumerate(tracks):
            check_deadline()
            active_track, scenario = track["id"], "continuous"
            result["tracks"].append(track)
            destination_path = output / (track["id"] + ".npy")
            destination = np.lib.format.open_memmap(destination_path, mode="w+", dtype="<f4",
                                                    shape=(4, track["input"]["frames"], 2))
            track["output"] = {"path": str(destination_path), "shape": list(destination.shape),
                               "dtype": "<f4", "complete": False}
            issued = time.monotonic()
            generation = worker.load(track["input"]["path"])
            track["continuous"] = collect(generation, track, 0, destination=destination)
            track["continuous"]["wall_seconds"] = time.monotonic() - issued
            track["continuous"]["load_issued_monotonic"] = issued
            track["continuous"]["first_chunk"]["load_to_delivered_ms"] = (
                track["continuous"]["first_chunk"]["delivered_monotonic"] - issued) * 1000
            destination.flush()
            track["output"].update(pin(destination_path), complete=True)
            if args.backend == "vulkan" and index == 0:
                for target in (HOP - 1, HOP + OVERLAP + 1, track["input"]["frames"] - 4096):
                    if not 0 <= target < track["input"]["frames"]:
                        raise ValueError("First Vulkan clip is too short for requested seek boundaries")
                    scenario = "seek"
                    issued = time.monotonic()
                    seek = collect(worker.seek(target), track, target, reference=destination)
                    seek.update(track_id=track["id"], wall_seconds=time.monotonic() - issued)
                    result["seeks"].append(seek)
                    if not seek["numeric_parity"]:
                        raise RuntimeError("Same-grid seek differs beyond the existing worker numerical tolerance")
            del destination
        stats = worker.statistics()
        if len(result["runtime_ready"]) != 1 or stats["telemetry_dropped"] != 0:
            raise RuntimeError("Persistent backend identity or bounded event evidence was lost")
        result["status"] = "ok"
    except KeyboardInterrupt:
        result["status"] = "interrupted"
        result["failures"].append({"track_id": active_track, "scenario": scenario, "error": "Interrupted"})
    except Exception as error:
        result["status"] = "error"
        result["failures"].append({"track_id": active_track, "scenario": scenario,
                                   "error": type(error).__name__ + ": " + str(error)})
    finally:
        for signum in old_signals:
            signal.signal(signum, signal.SIG_IGN)
        if worker is not None:
            try:
                worker.close(timeout=0.01)
                result["worker_statistics"] = worker.statistics()
                if not result["worker_statistics"]["closed"] or worker.backend_pid is not None:
                    raise RuntimeError("Worker/native child did not close")
            except Exception as error:
                result["status"] = "error"
                result["failures"].append({"phase": "close", "error": str(error)})
        for label, sampler in (("native_memory_cpu", native), ("supervisor_memory_cpu", supervisor)):
            if sampler is not None:
                try:
                    result[label] = sampler.stop()
                    if result[label].get("trace", {}).get("errors"):
                        raise RuntimeError("Requested native trace is incomplete")
                except Exception as error:
                    result["status"] = "error"
                    result["failures"].append({"phase": label, "error": str(error)})
        if log is not None:
            log.close()
        result["elapsed_seconds"] = time.monotonic() - began
        if result["elapsed_seconds"] > MAX_SECONDS:
            result["status"] = "error"
            result["failures"].append({"phase": "budget", "error": "Actual experiment elapsed exceeded 1800 seconds"})
        result["conditions_at_finish"] = conditions()
        result["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        (output / "result.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
        for signum, handler in old_signals.items():
            signal.signal(signum, handler)
    print(json.dumps({"output": str(output), "status": result["status"], "tracks": len(result["tracks"]),
                      "seeks": len(result["seeks"]), "elapsed_seconds": result["elapsed_seconds"],
                      "failures": result["failures"]}))
    return 0 if result["status"] == "ok" else 130 if result["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

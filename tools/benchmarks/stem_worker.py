#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Measure the native persistent separator and generation-safe chunk delivery."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
import wave

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.stems.worker import StemWorker, RATE, WINDOW, HOP, OVERLAP
from worker_support import create_fixture, ProcessSampler


def percentiles(values):
    ordered = sorted(values)
    return {str(p): ordered[math.ceil(len(ordered) * p / 100) - 1]
            for p in (50, 95, 99)} if ordered else {}


def conditions():
    result = {}
    for name in ("/sys/class/power_supply/AC/online",
                 "/sys/firmware/acpi/platform_profile",
                 "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference",
                 "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                 "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"):
        try:
            result[name] = Path(name).read_text().strip()
        except OSError:
            pass
    for zone in Path("/sys/class/thermal").glob("thermal_zone*"):
        try:
            result[str(zone)] = {"type": (zone / "type").read_text().strip(),
                                 "millidegrees_C": int((zone / "temp").read_text())}
        except (OSError, ValueError):
            pass
    return result


def json_event(event):
    result = {k: v for k, v in event.items() if k != "samples"}
    if "samples" in event:
        result["samples_sha256"] = hashlib.sha256(event["samples"].tobytes()).hexdigest()
    return result


class Load:
    MODES = {"none": (), "audio": ("audio",), "both": ("ui", "audio"), "ui": ("ui",)}

    def __init__(self, args, root):
        self.root = root
        self.children = []
        self.started = None
        self.mode = getattr(args, "load_mode", "both")
        self.selected = self.MODES[self.mode]
        self.renderer = args.renderer
        self.environment = dict(os.environ, QT_QPA_PLATFORM="wayland")
        self.environment.pop("QT_QUICK_BACKEND", None)
        self.environment.pop("QSG_RHI_BACKEND", None)
        if args.renderer == "vulkan":
            self.environment["QSG_RHI_BACKEND"] = "vulkan"
        load_seconds = str(getattr(args, "load_seconds", 3600))
        self.commands = {}
        if "ui" in self.selected:
            self.commands["ui"] = [str(args.ui.resolve()), "--smoke-test", "--smoke-seconds", load_seconds,
                                   "--smoke-stop-file", str(root / "stop")]
        if "audio" in self.selected:
            self.commands["audio"] = [str(args.audio_probe.resolve()), "--seconds", load_seconds, "--quantum",
                                      str(args.quantum), "--stop-file", str(root / "stop")]
        if "ui" in self.selected and getattr(args, "ui_progress_file", None) is not None:
            self.commands["ui"] += ["--smoke-progress-file", str(args.ui_progress_file)]
    def start(self):
        self.started = datetime.now(timezone.utc).isoformat()
        for name, command in self.commands.items():
            stdout = (self.root / f"{name}.json").open("wb")
            stderr = (self.root / f"{name}.stderr").open("wb")
            environment = self.environment
            if name == "ui" and self.renderer == "software":
                environment = dict(environment, QT_QUICK_BACKEND="software")
            try:
                child = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=environment)
            except Exception:
                stdout.close()
                stderr.close()
                raise
            self.children.append((name, child, stdout, stderr, command))
        time.sleep(1)
        self.check()

    def check(self):
        for name, child, *_ in self.children:
            if child.poll() is not None:
                raise RuntimeError(f"{name} load exited early: {child.returncode}")

    def stop(self):
        if self.children:
            (self.root / "stop").touch()
        result = {"started_at_utc": self.started, "load_mode": self.mode,
                  "selected_loads": list(self.selected),
                  "launched_loads": [name for name, *_ in self.children],
                  "not_run": [name for name in ("ui", "audio") if name not in self.selected],
                  "children": {}, "errors": []}
        for name, child, stdout, stderr, command in self.children:
            try:
                code = child.wait(timeout=15)
            except subprocess.TimeoutExpired:
                child.kill()
                code = child.wait(timeout=5)
            stdout.close()
            stderr.close()
            try:
                report = json.loads((self.root / f"{name}.json").read_text())
            except (ValueError, OSError) as error:
                report = {}
                result["errors"].append(f"{name} report: {error}")
            result["children"][name] = {"command": command, "returncode": code, "report": report}
            if code != 0:
                result["errors"].append(f"{name} load failed: {code}")
            if name == "ui" and (not report.get("rendered") or not report.get("capture_ok")
                                  or report.get("platform") != "wayland"
                                  or report.get("graphics_api") != {"opengl": 3, "vulkan": 5, "software": 1}[self.renderer]):
                result["errors"].append("requested Wayland renderer did not render")
            if name == "audio" and report.get("status") != "ok":
                result["errors"].append("audio probe did not complete successfully")
            if name == "audio":
                faults = {key: report.get(key) for key in (
                    "callback_execution_deadline_misses", "missing_buffers", "invalid_buffers",
                    "truncated_frames", "observed_graph_position_discontinuities",
                    "observed_position_clock_xrun_recovery_flags")}
                result["audio_fault_counters"] = faults
                if any(value for value in faults.values() if value is not None):
                    result["errors"].append("audio probe observed deadline/buffer/position faults")
        result["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        return result


class Run:
    def __init__(self, args, root, fixture, alternate):
        self.args, self.root = args, root
        self.fixture, self.alternate = fixture, alternate
        self.load = None
        self.worker = StemWorker(args.runtime, args.model, threads=args.threads,
                                 request_timeout=args.request_timeout)
        self.log = (root / "events.jsonl").open("w")
        self.records = []
        self.controls = []
        self.runtime = []
        self.references = {}
        self.stale_chunks = 0
        self.source_statistics_reused = 0

    def event(self, timeout=1):
        if self.load:
            self.load.check()
        event = self.worker.poll(timeout)
        if event is None:
            return None
        record = json_event(event)
        record["consumed_at"] = time.monotonic()
        self.log.write(json.dumps(record) + "\n")
        self.log.flush()
        if event["event"] == "runtime_ready":
            self.runtime.append(record)
        if event["event"] == "source_ready" and event["reused"]:
            self.source_statistics_reused += 1
        if event["event"] == "error":
            raise RuntimeError(event["message"])
        if event["event"] == "chunk" and not self.worker.is_current(event):
            self.stale_chunks += 1
            raise RuntimeError("stale chunk reached the benchmark consumer")
        return event

    def until(self, predicate, timeout=None):
        deadline = time.monotonic() + (timeout or self.args.request_timeout * 4)
        while time.monotonic() < deadline:
            event = self.event()
            if event is not None and predicate(event):
                return event
        raise TimeoutError("worker scenario did not finish")

    def first_chunk(self, generation):
        return self.until(lambda e: e["event"] == "chunk" and e["generation"] == generation)

    def started(self, generation):
        return self.until(lambda e: e["event"] == "started" and e["generation"] == generation)

    def throughput(self):
        began = time.monotonic()
        generation = self.worker.load(self.fixture["path"])
        expected, square_error, count, peak_join = 0, 0.0, 0, 0.0
        last = None
        first = None
        with wave.open(self.fixture["path"], "rb") as source:
            while True:
                event = self.until(lambda e: e["generation"] == generation
                                   and e["event"] in ("chunk", "complete"))
                if event["event"] == "complete":
                    break
                if event["frame_start"] != expected:
                    raise RuntimeError("noncontiguous normal output")
                if first is None:
                    first = {"ready_ms": (event["ready_at"] - began) * 1000,
                             "delivered_ms": (time.monotonic() - began) * 1000}
                samples = event["samples"]
                frames = event["frames"]
                if expected // HOP in (0, 1, 2, 3):
                    self.references[expected] = samples.copy()
                if last is not None:
                    peak_join = max(peak_join, float(np.max(np.abs(samples[:, 0] - last))))
                last = samples[:, -1].copy()
                original = np.frombuffer(source.readframes(frames), dtype="<i2").reshape(-1, 2) / 32768
                error = samples.sum(axis=0, dtype=np.float64) - original
                square_error += float(np.square(error).sum())
                count += error.size
                self.records.append(json_event(event))
                expected += frames
        if expected != self.fixture["frames"]:
            raise RuntimeError("incomplete source coverage")
        elapsed = time.monotonic() - began
        services = [e["hop_service_ms"] for e in self.records]
        return {"generation": generation, "frames": expected, "chunks": len(self.records),
                "wall_seconds": elapsed, "first_chunk": first,
                "process_rtf": elapsed / (expected / RATE),
                "hop_service_percentiles_ms": percentiles(services),
                "hop_service_max_ms": max(services),
                "headroom_target_ms": HOP / RATE * 700,
                "observed_p99_within_target": percentiles(services)["99"] < HOP / RATE * 700,
                "recombination_rms_error": math.sqrt(square_error / count),
                "maximum_stem_join_step": peak_join,
                "musical_quality_verified": False}

    def paced(self):
        generation = self.worker.seek(0)
        first = self.first_chunk(generation)
        began = time.monotonic()
        waits = []
        expected = first["frames"]
        # One currently playing hop, at most two ready hops in the producer.
        # This consumes real delivered chunks on a wall clock, without audio routing.
        for index in range(1, 6):
            deadline = began + index * HOP / RATE
            while time.monotonic() < deadline:
                self.load.check()
                time.sleep(max(0, min(0.05, deadline - time.monotonic())))
            requested = time.monotonic()
            chunk = self.first_chunk(generation)
            obtained = time.monotonic()
            if chunk["frame_start"] != expected:
                raise RuntimeError("paced consumer skipped source frames")
            waits.append({"frame_start": expected,
                          "ready_lateness_ms": max(0.0, (chunk["ready_at"] - deadline) * 1000),
                          "poll_wait_ms": (obtained - requested) * 1000,
                          "ready_at": chunk["ready_at"], "deadline": deadline})
            expected += chunk["frames"]
        self.worker.cancel()
        return {"chunks": 6, "delivered_audio_seconds": 6 * HOP / RATE,
                "paced_interval_seconds": 5 * HOP / RATE,
                "wall_seconds": time.monotonic() - began,
                "late_chunks": sum(e["ready_lateness_ms"] > 0 for e in waits),
                "scheduled_consumption": waits,
                "scope": "paced delivery consumer; stems are not routed to PipeWire"}

    def seek_case(self, name, target, start_generation):
        active = self.started(start_generation)
        time.sleep(0.15)
        began = time.monotonic()
        generation = self.worker.seek(target)
        ack_ms = (time.monotonic() - began) * 1000
        chunk = self.first_chunk(generation)
        if chunk["frame_start"] != target:
            raise RuntimeError("seek output starts at the wrong source frame")
        origin = target // HOP * HOP
        reference = self.references[origin][:, target - origin:]
        if chunk["samples"].shape != reference.shape:
            raise RuntimeError("seek output shape differs from continuous reference")
        delta = chunk["samples"] - reference
        parity = bool(np.allclose(chunk["samples"], reference, rtol=1e-4, atol=1e-5))
        if not parity:
            raise RuntimeError("same-grid seek differs from continuous reference beyond numeric tolerance")
        result = {"scenario": name, "generation": generation,
                  "superseded_request_id": active["request_id"], "frame_start": target,
                  "invalidation_ack_ms": ack_ms,
                  "replacement_delivered_ms": (time.monotonic() - began) * 1000,
                  "same_grid_max_abs_difference": float(np.max(np.abs(delta))),
                  "same_grid_rms_difference": float(np.sqrt(np.mean(np.square(delta)))),
                  "shape_matches_reference": True, "numeric_parity": parity,
                  "parity_tolerance": {"rtol": 1e-4, "atol": 1e-5}}
        self.controls.append(result)
        return generation, chunk

    def scenarios(self):
        generation = self.worker.seek(0)
        generation, held = self.seek_case("seek_forward_in_flight", 2 * HOP + 137, generation)
        generation, held = self.seek_case("seek_backward_in_flight", HOP + 333, generation)
        self.started(generation)
        time.sleep(0.15)
        began = time.monotonic()
        self.worker.cancel()
        acknowledgement = (time.monotonic() - began) * 1000
        if self.worker.is_current(held):
            raise RuntimeError("a held chunk stayed eligible after cancellation")
        discarded = self.until(lambda e: e["event"] == "discarded" and e["generation"] == generation)
        quiet_end = time.monotonic() + 0.25
        while time.monotonic() < quiet_end:
            event = self.event(0.05)
            if event and event["event"] == "chunk":
                raise RuntimeError("cancelled generation delivered a chunk")
        self.controls.append({"scenario": "cancel_in_flight", "invalidation_ack_ms": acknowledgement,
                              "residual_request_finish_ms": (discarded["timestamp"] - began) * 1000,
                              "held_chunk_rejected": True, "post_cancel_chunks": 0})
        # Only the latest pending seek is eligible after one already active request.
        generation = self.worker.seek(0)
        self.started(generation)
        for target in (HOP + 1, 2 * HOP + 7, 3 * HOP + 999):
            generation = self.worker.seek(target)
        latest = self.first_chunk(generation)
        if latest["frame_start"] != 3 * HOP + 999:
            raise RuntimeError("burst seek did not honor latest control")
        self.controls.append({"scenario": "seek_burst", "generation": generation,
                              "latest_frame_start": latest["frame_start"]})
        self.started(generation)
        generation = self.worker.load(self.alternate["path"], HOP + 43)
        replacement = self.first_chunk(generation)
        if replacement["source_id"] != self.alternate["sha256"]:
            raise RuntimeError("source replacement retained old source identity")
        self.controls.append({"scenario": "replace_source_in_flight", "source_id": replacement["source_id"],
                              "frame_start": replacement["frame_start"],
                              "replacement_delivered_ms": replacement["control_latency_ms"]})
        # Leave a full two-chunk queue untouched, then verify cancellation wakes it.
        deadline = time.monotonic() + self.args.request_timeout * 3
        while self.worker.statistics()["ready_count"] < 2 and time.monotonic() < deadline:
            self.load.check()
            time.sleep(0.05)
        full = self.worker.statistics()
        began = time.monotonic()
        self.worker.cancel()
        self.controls.append({"scenario": "cancel_backpressured", "statistics_before": full,
                              "invalidation_ack_ms": (time.monotonic() - began) * 1000})
        if full["ready_count"] != 2:
            raise RuntimeError("backpressure scenario did not fill the bounded output queue")
        generation = self.worker.seek(0)
        self.started(generation)
        time.sleep(0.15)
        began = time.monotonic()
        pid = self.worker.backend_pid
        self.worker.close(timeout=0.01)
        self.controls.append({"scenario": "hard_stop_active_request", "pid": pid,
                              "close_ms": (time.monotonic() - began) * 1000,
                              "supervisor_closed": self.worker.statistics()["closed"]})

    def execute(self):
        result = {"schema": 1, "started_at_utc": datetime.now(timezone.utc).isoformat(),
                  "host": platform.platform(), "renderer": self.args.renderer,
                  "fixture": self.fixture, "replacement_fixture": self.alternate,
                  "window_frames": WINDOW, "hop_frames": HOP,
                  "first_hop_input_horizon_frames": WINDOW,
                  "lookahead_beyond_hop_end_frames": OVERLAP,
                  "normalization": "whole-source mono sample mean/std; reused on seek",
                  "numpy_version": np.__version__, "conditions_at_start": conditions()}
        native = ProcessSampler(lambda: self.worker.backend_pid,
                                trace_path=self.root / "native-trace.jsonl" if self.args.trace_system else None,
                                include_system=self.args.trace_system)
        supervisor = ProcessSampler(os.getpid)
        try:
            self.load = Load(self.args, self.root)
            self.load.start()
            with native, supervisor:
                result["throughput"] = self.throughput()
                result["paced_delivery"] = self.paced()
                self.scenarios()
            result.update({"status": "ok", "controls": self.controls,
                           "runtime_ready": self.runtime,
                           "worker_statistics": self.worker.statistics(),
                           "stale_chunks_delivered": self.stale_chunks,
                           "source_statistics_reused": self.source_statistics_reused})
            if len(self.runtime) != 1:
                raise RuntimeError("ordinary controls reloaded the native model")
        except KeyboardInterrupt:
            result.update({"status": "interrupted", "controls": self.controls,
                           "runtime_ready": self.runtime,
                           "partial_throughput_chunks": len(self.records)})
        except Exception as error:
            result.update({"status": "error", "error": str(error), "controls": self.controls,
                           "runtime_ready": self.runtime, "worker_statistics": self.worker.statistics()})
        finally:
            try:
                self.worker.close(timeout=0.01)
            except Exception as error:
                result.update({"status": "error", "close_error": str(error)})
            self.log.close()
            if native._finished is not None:
                try:
                    result["native_memory_cpu"] = native.report()
                    result["supervisor_memory_cpu"] = supervisor.report()
                    if self.args.trace_system and result["native_memory_cpu"]["trace"]["errors"]:
                        result.update(status="error", sampling_error="requested native trace was incomplete")
                except Exception as error:
                    result.update({"status": "error", "sampling_error": str(error)})
            if self.load:
                try:
                    result["load"] = self.load.stop()
                    if result["load"]["errors"]:
                        result["status"] = "error"
                except Exception as error:
                    result.update({"status": "error", "load_error": str(error)})
            result["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
            result["conditions_at_finish"] = conditions()
            (self.root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "model", "ui", "audio-probe", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--renderer", choices=("opengl", "vulkan", "software"), default="opengl")
    parser.add_argument("--hops", type=int, default=60)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--quantum", type=int, choices=(256, 512), default=256)
    parser.add_argument("--request-timeout", type=float, default=30)
    parser.add_argument("--trace-system", action="store_true",
                        help="Stream native scheduling, GPU clock and throttle observations.")
    args = parser.parse_args()
    if not 6 <= args.hops <= 600 or not 1 <= args.threads <= 64 or not math.isfinite(args.request_timeout) or args.request_timeout <= 0:
        parser.error("hops must be 6..600, threads 1..64 and timeout positive/finite")
    root = args.output.resolve()
    if root.exists() and any(root.iterdir()):
        parser.error("output must be a new or empty directory")
    root.mkdir(parents=True, exist_ok=True)
    try:
        fixture = create_fixture(root / "source.wav", (args.hops * HOP + 12345) / RATE)
        alternate = create_fixture(root / "replacement.wav", (6 * HOP + 77) / RATE, seed=11)
        result = Run(args, root, fixture, alternate).execute()
    finally:
        for name in ("source.wav", "replacement.wav"):
            (root / name).unlink(missing_ok=True)
    print(json.dumps({k: result[k] for k in ("status", "throughput", "controls") if k in result}, indent=2))
    return 0 if result["status"] == "ok" else 130 if result["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

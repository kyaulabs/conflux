#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Saturate one persistent stem worker under continuous audio/UI load."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time

import numpy as np

from stem_worker import Load, conditions, json_event, percentiles
from tools.stems.worker import StemWorker, RATE, WINDOW, HOP, OVERLAP
from worker_support import ProcessSampler, create_fixture


TARGET_MS = HOP / RATE * 700
FIXTURE_FRAMES = 8 * HOP + 12345


def atomic_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def interrupted(_signum, _frame):
    raise KeyboardInterrupt


def timing_summary(rows):
    values = [row["hop_service_ms"] for row in rows]
    return {
        "chunks": len(values),
        "frames": sum(row["frames"] for row in rows),
        "hop_service_percentiles_ms": percentiles(values),
        "hop_service_max_ms": max(values, default=None),
        "hops_above_target": sum(value > TARGET_MS for value in values),
        "observed_p99_within_target": bool(values and percentiles(values)["99"] < TARGET_MS),
    }


class Soak:
    def __init__(self, args, root):
        self.args, self.root = args, root
        self.worker = None
        self.load = None
        self.log = None
        self.fixture = None
        self.native_sampler = None
        self.supervisor_sampler = None
        self.started = None
        self.finished = None
        self.deadline = None
        self.generation = None
        self.expected = 0
        self.source_start = 0
        self.first_after_seek = False
        self.next_control = None
        self.progress_due = 0
        self.held = None
        self.last_ready = None
        self.last_generation = None
        self.hashes = {}
        self.references = {}
        self.hops = []
        self.controls = []
        self.runtime = []
        self.failures = []
        self.source_loops = 0
        self.resumed_completions = 0
        self.parity_checks = 0
        self.trimmed_parity_checks = 0
        self.stale_chunks = 0
        self.alive_checks = 0
        self.last_alive_check = None
        self.max_alive_check_gap = 0
        self.statistics_reused = 0
        self.finished_control_attempts = 0

    def failure(self, phase, error):
        self.failures.append({"phase": phase, "error": str(error),
                              "timestamp_monotonic": time.monotonic()})

    def require(self, condition, message):
        if not condition:
            raise RuntimeError(message)

    def elapsed(self):
        return max(0.0, (self.finished or time.monotonic()) - self.started) if self.started else 0.0

    def progress(self, *, force=False, status="running"):
        now = time.monotonic()
        if not force and now < self.progress_due:
            return
        self.progress_due = now + 30
        atomic_json(self.root / "progress.json", {
            "status": status, "updated_at_utc": datetime.now(timezone.utc).isoformat(),
            "elapsed_workload_seconds": self.elapsed(), "requested_seconds": self.args.seconds,
            "timing": timing_summary(self.hops), "completed_controls": len([
                control for control in self.controls if control.get("complete")]),
            "full_source_loops": self.source_loops, "failures": self.failures,
            "worker_statistics": self.worker.statistics() if self.worker else None,
            "conditions": conditions(),
        })

    def check_load(self):
        self.load.check()
        now = time.monotonic()
        if self.last_alive_check is not None:
            self.max_alive_check_gap = max(self.max_alive_check_gap, now - self.last_alive_check)
        self.last_alive_check = now
        self.alive_checks += 1

    def event(self, timeout=0.25):
        self.check_load()
        event = self.worker.poll(timeout)
        self.progress()
        if event is None:
            return None
        record = json_event(event)
        record["consumed_at"] = time.monotonic()
        self.log.write(json.dumps(record) + "\n")
        self.log.flush()
        if event["event"] == "runtime_ready":
            self.runtime.append(record)
            self.require(len(self.runtime) == 1, "native backend reloaded during soak")
        elif event["event"] == "source_ready":
            self.statistics_reused += bool(event["reused"])
        elif event["event"] == "error":
            raise RuntimeError(event["message"])
        elif event["event"] == "chunk":
            if not self.worker.is_current(event) or event["generation"] != self.generation:
                self.stale_chunks += 1
                raise RuntimeError("stale chunk reached soak consumer")
            self.require(event["source_id"] == self.fixture["sha256"], "source identity changed")
        statistics = self.worker.statistics()
        self.require(statistics["ready_count"] <= 2 and statistics["ready_high_water"] <= 2,
                     "ready output exceeded its two-hop bound")
        return event

    def wait(self, predicate, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            event = self.event(min(0.25, max(0, deadline - time.monotonic())))
            if event is not None and predicate(event):
                return event
        raise TimeoutError("soak control did not finish before its deadline")

    def accept(self, event):
        self.require(event["frame_start"] == self.expected, "noncontiguous source output")
        samples = event["samples"]
        self.require(samples.shape == (4, event["frames"], 2), "unexpected chunk shape")
        self.require(event["ready_queue_depth"] <= 2, "chunk reported an unbounded ready queue")
        origin = event["window_start_frame"]
        trimmed = event["frame_start"] - origin
        sample_hash = hashlib.sha256(samples.tobytes()).hexdigest()
        parity = {"kind": "reference_created", "exact": True}
        if trimmed:
            reference = self.references.get(origin)
            self.require(reference is not None, "seek has no continuous reference")
            reference = reference[:, trimmed:]
            self.require(samples.shape == reference.shape, "seek shape differs from reference")
            difference = samples - reference
            equivalent = bool(np.allclose(samples, reference, rtol=1e-4, atol=1e-5))
            parity = {"kind": "trimmed_seek", "exact": bool(np.array_equal(samples, reference)),
                      "numeric_parity": equivalent,
                      "max_abs_difference": float(np.max(np.abs(difference))),
                      "rtol": 1e-4, "atol": 1e-5}
            self.require(equivalent, "seek differs from continuous reference beyond tolerance")
            self.trimmed_parity_checks += 1
        elif origin in self.hashes:
            self.require(sample_hash == self.hashes[origin], "repeated same-grid chunk hash changed")
            parity = {"kind": "same_grid_sha256", "exact": True}
            self.parity_checks += 1
        else:
            self.hashes[origin] = sample_hash
            if origin in (HOP, 2 * HOP):
                self.references[origin] = samples.copy()
        row = {key: event[key] for key in (
            "generation", "request_id", "frame_start", "frames", "window_start_frame",
            "ready_at", "request_ms", "source_read_ms", "overlap_ms", "hop_service_ms",
            "control_latency_ms", "queue_delay_ms", "metrics")}
        row.update(elapsed_seconds=event["ready_at"] - self.started,
                   kind="seek_first" if self.first_after_seek else "continuous",
                   samples_sha256=sample_hash, parity=parity,
                   above_target=event["hop_service_ms"] > TARGET_MS)
        if self.last_ready is not None:
            row["ready_gap_ms"] = (event["ready_at"] - self.last_ready) * 1000
            row["same_generation_as_previous"] = event["generation"] == self.last_generation
        self.hops.append(row)
        self.log.write(json.dumps({"event": "soak_hop", **row}) + "\n")
        self.log.flush()
        self.last_ready, self.last_generation = event["ready_at"], event["generation"]
        self.expected += event["frames"]
        self.first_after_seek = False
        self.held = event

    def active_control(self, active):
        settle_until = time.monotonic() + 0.15
        while time.monotonic() < settle_until:
            self.check_load()
            time.sleep(max(0, min(0.05, settle_until - time.monotonic())))
        self.check_load()
        inflight = self.worker.statistics()["inflight"]
        if (not inflight or inflight["request_id"] != active["request_id"]
                or inflight["generation"] != self.generation
                or active["generation"] != self.generation):
            self.finished_control_attempts += 1
            return
        began = time.monotonic()
        control = {"at_workload_seconds": began - self.started,
                   "superseded_generation": self.generation,
                   "superseded_request_id": active["request_id"],
                   "started_to_cancel_ms": (began - active["timestamp"]) * 1000,
                   "hardware_submission_confirmed": False, "complete": False}
        self.controls.append(control)
        self.generation = self.worker.cancel()
        control["cancel_ack_ms"] = (time.monotonic() - began) * 1000
        control["held_chunk_rejected"] = self.held is None or not self.worker.is_current(self.held)
        self.require(control["held_chunk_rejected"], "held result survived cancellation")
        discarded = self.wait(lambda event: event["event"] == "discarded"
                              and event.get("request_id") == active["request_id"],
                              self.args.request_timeout + 2)
        control["residual_finish_ms"] = (discarded["timestamp"] - began) * 1000
        target = (HOP + 333) if len(self.controls) % 2 else (2 * HOP + 137)
        resumed = time.monotonic()
        self.generation = self.worker.seek(target)
        control.update(resume_ack_ms=(time.monotonic() - resumed) * 1000,
                       generation=self.generation, target_frame=target)
        self.expected, self.source_start, self.first_after_seek = target, target, True
        chunk = self.wait(lambda event: event["event"] == "chunk"
                          and event["generation"] == self.generation,
                          self.args.request_timeout * 2 + 5)
        self.accept(chunk)
        control.update(ready_after_resume_ms=(chunk["ready_at"] - resumed) * 1000,
                       ready_after_cancel_ms=(chunk["ready_at"] - began) * 1000,
                       delivered_after_cancel_ms=(time.monotonic() - began) * 1000,
                       post_cancel_chunks=0, parity=self.hops[-1]["parity"], complete=True)
        while self.next_control <= time.monotonic():
            self.next_control += self.args.control_interval
        self.progress(force=True)

    def workload(self):
        self.generation = self.worker.load(self.fixture["path"])
        first = self.wait(lambda event: event["event"] == "started",
                          self.args.request_timeout * 2 + 5)
        self.started = first["timestamp"]
        self.deadline = self.started + self.args.seconds
        self.next_control = self.started + self.args.control_interval
        self.progress(force=True)
        while time.monotonic() < self.deadline:
            event = self.event(min(0.25, max(0, self.deadline - time.monotonic())))
            if event is None:
                continue
            if event["event"] == "chunk":
                self.accept(event)
            elif event["event"] == "complete" and event["generation"] == self.generation:
                self.require(self.expected == self.fixture["frames"], "source ended before full coverage")
                if self.source_start == 0:
                    self.source_loops += 1
                else:
                    self.resumed_completions += 1
                self.generation = self.worker.seek(0)
                self.expected, self.source_start = 0, 0
            elif (event["event"] == "started" and time.monotonic() >= self.next_control
                  and self.deadline - time.monotonic() > min(
                      self.args.request_timeout * 3 + 10, self.args.seconds / 2)
                  and HOP in self.references and 2 * HOP in self.references):
                inflight = self.worker.statistics()["inflight"]
                if inflight and inflight["request_id"] == event["request_id"]:
                    self.active_control(event)
        self.check_load()
        self.finished = time.monotonic()
        self.require(self.elapsed() >= self.args.seconds, "workload ended before requested duration")

    def summaries(self):
        window = min(300.0, self.args.seconds / 3)
        elapsed = self.elapsed()
        bins = []
        for start in range(0, math.ceil(max(elapsed, 1)), 300):
            rows = [row for row in self.hops if start <= row["elapsed_seconds"] < start + 300]
            bins.append({"start_seconds": start, "end_seconds": min(start + 300, elapsed),
                         **timing_summary(rows)})
        return {"all_delivered": timing_summary(self.hops),
                "continuous": timing_summary([row for row in self.hops if row["kind"] == "continuous"]),
                "first_window": {"seconds": window, **timing_summary([
                    row for row in self.hops if row["elapsed_seconds"] < window])},
                "last_window": {"seconds": window, **timing_summary([
                    row for row in self.hops if row["elapsed_seconds"] >= max(0, elapsed - window)])},
                "bins_300_seconds": bins, "headroom_target_ms": TARGET_MS,
                "percentile_method": "nearest rank; consecutive windows are not independent tail samples"}

    def execute(self):
        result = {"schema": 1, "status": "starting", "command": [sys.executable, *sys.argv],
                  "started_at_utc": datetime.now(timezone.utc).isoformat(),
                  "host": platform.platform(), "numpy_version": np.__version__,
                  "renderer": self.args.renderer, "requested_seconds": self.args.seconds,
                  "control_interval_seconds": self.args.control_interval,
                  "sample_interval_seconds": self.args.sample_interval,
                  "load_seconds_fallback": self.args.load_seconds,
                  "window_frames": WINDOW, "hop_frames": HOP, "overlap_frames": OVERLAP,
                  "conditions_at_start": conditions(), "pins": {}}
        interrupted = False
        try:
            for name, path in (("runtime", self.args.runtime), ("model", self.args.model),
                               ("ui", self.args.ui), ("audio_probe", self.args.audio_probe),
                               ("soak_source", Path(__file__)),
                               ("shared_benchmark_source", Path(__file__).with_name("stem_worker.py")),
                               ("worker_source", Path(__file__).parents[1] / "stems/worker.py"),
                               ("sampler_source", Path(__file__).with_name("worker_support.py"))):
                with path.open("rb") as stream:
                    result["pins"][name] = {"path": str(path.resolve()),
                                            "bytes": path.stat().st_size,
                                            "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}
            revision = subprocess.run(["git", "rev-parse", "HEAD"], text=True,
                                      capture_output=True, check=True)
            result["repository_head"] = revision.stdout.strip()
            self.log = (self.root / "events.jsonl").open("w")
            self.fixture = create_fixture(self.root / "source.wav", FIXTURE_FRAMES / RATE)
            result["fixture"] = self.fixture
            self.worker = StemWorker(self.args.runtime, self.args.model, threads=self.args.threads,
                                     max_ready=2, request_timeout=self.args.request_timeout)
            self.load = Load(self.args, self.root)
            self.load.start()
            result["load_started_monotonic"] = time.monotonic()
            self.native_sampler = ProcessSampler(lambda: self.worker.backend_pid,
                                                 self.args.sample_interval,
                                                 trace_path=self.root / "native-trace.jsonl", include_system=True)
            self.supervisor_sampler = ProcessSampler(os.getpid, self.args.sample_interval)
            self.native_sampler.__enter__()
            self.supervisor_sampler.__enter__()
            self.workload()
            result["status"] = "ok"
        except KeyboardInterrupt:
            interrupted = True
            result["status"] = "interrupted"
        except Exception as error:
            self.failure("workload", f"{type(error).__name__}: {error}")
            result["status"] = "error"
        finally:
            if self.started is not None and self.finished is None:
                self.finished = time.monotonic()
            if self.worker:
                result["worker_statistics_before_shutdown"] = self.worker.statistics()
                began = time.monotonic()
                try:
                    self.worker.close(timeout=0.1)
                except Exception as error:
                    self.failure("worker_shutdown", error)
                result["worker_shutdown_ms"] = (time.monotonic() - began) * 1000
                result["worker_statistics"] = self.worker.statistics()
            for name, sampler in (("native_memory_cpu", self.native_sampler),
                                  ("supervisor_memory_cpu", self.supervisor_sampler)):
                if sampler is not None:
                    try:
                        result[name] = sampler.stop()
                        if result[name].get("trace", {}).get("errors"):
                            self.failure(name, "requested trace was incomplete")
                    except Exception as error:
                        self.failure(name, error)
            if self.load:
                try:
                    result["load"] = self.load.stop()
                    for error in result["load"]["errors"]:
                        self.failure("audio_ui_load", error)
                except Exception as error:
                    self.failure("audio_ui_shutdown", error)
            if self.log:
                try:
                    self.log.close()
                except OSError as error:
                    self.failure("event_log_close", error)
            try:
                (self.root / "source.wav").unlink(missing_ok=True)
            except OSError as error:
                self.failure("fixture_cleanup", error)
            result.update(workload_started_monotonic=self.started, workload_stopped_monotonic=self.finished,
                          workload_seconds=self.elapsed(), timing=self.summaries(), controls=self.controls,
                          runtime_ready=self.runtime, source_statistics_reused=self.statistics_reused,
                          full_source_loops=self.source_loops, resumed_source_completions=self.resumed_completions,
                          control_attempts_finished_before_cancel=self.finished_control_attempts,
                          reference_hashes={str(frame): digest for frame, digest in self.hashes.items()},
                          same_grid_hash_checks=self.parity_checks, trimmed_seek_checks=self.trimmed_parity_checks,
                          stale_chunks_delivered=self.stale_chunks,
                          load_alive_checks=self.alive_checks, maximum_load_check_gap_seconds=self.max_alive_check_gap,
                          load_coverage_scope="UI child alive checks plus final rendered/frame count; no per-frame timing",
                          conditions_at_finish=conditions(),
                          finished_at_utc=datetime.now(timezone.utc).isoformat())
            audio = result.get("load", {}).get("children", {}).get("audio", {}).get("report", {})
            audio_start, audio_stop = audio.get("streaming_started_monotonic_ns"), audio.get("stopped_monotonic_ns")
            overlap = 0.0
            if self.started is not None and self.finished is not None and audio_start and audio_stop:
                overlap = max(0.0, min(self.finished, audio_stop / 1e9) - max(self.started, audio_start / 1e9))
            result["concurrent_audio_workload_seconds"] = overlap
            if result["status"] == "ok":
                if overlap < self.args.seconds:
                    self.failure("audio_coverage", "actual streaming interval did not cover requested workload")
                statistics = result["worker_statistics"]
                if statistics["backend_start_count"] != 1 or len(self.runtime) != 1:
                    self.failure("backend_identity", "soak did not retain exactly one backend")
                if statistics["telemetry_dropped"]:
                    self.failure("telemetry", "worker telemetry events were dropped")
                if self.args.seconds >= 60 and not any(control.get("complete") for control in self.controls):
                    self.failure("control_coverage", "no active cancellation/seek/resume batch completed")
            result["native_trace_bytes"] = (self.root / "native-trace.jsonl").stat().st_size if (self.root / "native-trace.jsonl").exists() else 0
            result["failures"] = self.failures
            if self.failures and not interrupted:
                result["status"] = "error"
            result["acceptance"] = {
                "requested_wall_duration_completed": self.elapsed() >= self.args.seconds,
                "sixty_minute_workload_completed": self.elapsed() >= 3600 and self.args.seconds >= 3600,
                "audio_covers_requested_duration": overlap >= self.args.seconds,
                "controls_completed": sum(bool(control.get("complete")) for control in self.controls),
                "observed_continuous_p99_within_target": result["timing"]["continuous"]["observed_p99_within_target"],
                "parity_scope": "Full same-grid chunks require identical SHA-256; trimmed seeks use allclose(rtol=1e-4, atol=1e-5) and record exact equality/max difference.",
                "active_cancel_scope": "Wait 150 ms after the supervisor started event, then revalidate generation/request. No GPU submission timestamp is observed.",
                "scope": "synthetic saturated delivery, animated UI and independent silent audio; no stem playback/full deck or musical-quality claim",
            }
            try:
                self.progress(force=True, status=result["status"])
            except OSError as error:
                self.failure("progress_report", error)
                if not interrupted:
                    result["status"] = "error"
            atomic_json(self.root / "result.json", result)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "model", "ui", "audio-probe", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--renderer", choices=("opengl", "vulkan"), default="opengl")
    parser.add_argument("--seconds", type=float, default=3600)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--quantum", type=int, choices=(256, 512), default=256)
    parser.add_argument("--request-timeout", type=float, default=30)
    parser.add_argument("--control-interval", type=float)
    parser.add_argument("--sample-interval", type=float, default=1.0)
    args = parser.parse_args()
    if (not math.isfinite(args.seconds) or args.seconds <= 0 or not 1 <= args.threads <= 64
            or not math.isfinite(args.request_timeout) or args.request_timeout <= 0
            or not math.isfinite(args.sample_interval) or args.sample_interval <= 0):
        parser.error("duration, timeout and sample interval must be finite/positive; threads must be 1..64")
    args.control_interval = args.control_interval if args.control_interval is not None else min(300, args.seconds / 3)
    if not math.isfinite(args.control_interval) or args.control_interval <= 0:
        parser.error("control interval must be finite and positive")
    args.load_seconds = math.ceil(args.seconds + args.request_timeout * 2 + 60)
    if args.load_seconds > 7200:
        parser.error("duration plus startup/shutdown margin must fit native 7200-second limit")
    args.runtime, args.model = args.runtime.resolve(), args.model.resolve()
    root = args.output.resolve()
    if root.exists() and any(root.iterdir()):
        parser.error("output must be new or empty")
    root.mkdir(parents=True, exist_ok=True)
    previous = signal.signal(signal.SIGTERM, interrupted)
    try:
        result = Soak(args, root).execute()
    finally:
        signal.signal(signal.SIGTERM, previous)
    print(json.dumps({key: result[key] for key in ("status", "workload_seconds", "acceptance", "failures")}, indent=2))
    return 0 if result["status"] == "ok" else 130 if result["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

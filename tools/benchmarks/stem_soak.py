#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Exercise one persistent stem worker with selected continuous audio/UI load."""

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
import wave

import numpy as np

from stem_worker import Load, conditions, json_event, percentiles
from tools.stems.worker import StemWorker, RATE, WINDOW, HOP, OVERLAP
from worker_support import ProcessSampler, create_fixture


TARGET_MS = HOP / RATE * 700
FIXTURE_FRAMES = 8 * HOP + 12345
UI_PROGRESS_INTERVAL = 1.0
UI_STARTUP_GRACE = 5.0
UI_MAX_GAP = 30.0


def atomic_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def interrupted(_signum, _frame):
    raise KeyboardInterrupt


def wav_source(path):
    path = path.resolve()
    before = path.stat()
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    with wave.open(str(path), "rb") as stream:
        if stream.getparams()[:3] != (2, 2, RATE) or stream.getcomptype() != "NONE":
            raise ValueError("source must be stereo 44.1 kHz PCM16 WAV: " + str(path))
        frames = stream.getnframes()
        if frames <= 2 * HOP + 137:
            raise ValueError("source must extend beyond the second trimmed seek: " + str(path))
        count = 0
        while data := stream.readframes(65536):
            count += len(data)
        if count != frames * 4:
            raise RuntimeError("source WAV is truncated: " + str(path))
    after = path.stat()
    if ((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
            != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)):
        raise RuntimeError("source changed during identity/geometry checks: " + str(path))
    return {"path": str(path), "bytes": after.st_size, "sha256": digest,
            "frames": frames, "seconds": frames / RATE, "sample_rate": RATE,
            "channels": 2, "sample_bytes": 2}


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
        self.source_paths = getattr(args, "source_wav", None) or []
        self.source_mode = "prepared_wav_corpus" if self.source_paths else "synthetic"
        self.sources = []
        self.source_index = 0
        self.generated_fixture = None
        self.hashes_by_source = {}
        self.references_by_source = {}
        self.source_normalization = {}
        self.source_completions = {}
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
        self.load_mode = getattr(args, "load_mode", "both")
        self.selected_loads = Load.MODES[self.load_mode]
        self.args.ui_progress_file = self.root / "ui-progress.json" if "ui" in self.selected_loads else None
        self.ui_watch_started = None
        self.ui_next_check = 0
        self.ui_last_check = None
        self.ui_last_advance = None
        self.ui_last_progress = None
        self.ui_checks = 0
        self.ui_progress_errors = {}
        self.ui_gate_failure = None
        self.ui_max_progress_age = 0.0
        self.ui_max_frame_age = 0.0
        self.ui_max_check_gap = 0.0
        self.ui_unexposed_observations = 0
        self.ui_unexposed_since = None
        self.ui_max_unexposed_span = 0.0
        self.ui_last_unexposed_span = None
        self.parity_failure = None
        self.delivery_mode = getattr(args, "delivery_mode", "saturated")
        self.playback_epochs = []
        self.playback_demands = []
        self.playback_next_deadline = None
        self.playback_pending_demand = None
        self.source_admissions = {}

    def playback_summary(self):
        if self.delivery_mode != "playback-paced":
            return {"requested": False, "status": "not_run"}
        demands = self.playback_demands
        late = [row for row in demands if row["ready_late_ms"] > 0]
        # A late demand can overlap another late demand. Merge the intervals
        # rather than counting the same virtual missing-audio time twice.
        intervals = []
        for row in late:
            start, end = row["deadline_monotonic"], row["ready_at"]
            if intervals and start <= intervals[-1][1]:
                intervals[-1][1] = max(intervals[-1][1], end)
            else:
                intervals.append([start, end])
        pending = self.playback_pending_demand
        unfulfilled = bool(pending and self.deadline is not None
                          and pending["deadline_monotonic"] < self.deadline)
        return {"requested": True, "mode": self.delivery_mode,
                "scope": "Virtual frame-clock consumption with one held playing chunk and at most two worker-ready chunks; independent silent audio probe, no physical stem playback",
                "clock": "First initial/recovery delivery anchors an epoch; every ordinary deadline advances by actual delivered frames / sample rate, including source rotations and late output",
                "ordinary_demands": len(demands),
                "ready_deadline_misses": len(late),
                "unfulfilled_due_demands": int(unfulfilled),
                "ordinary_deadline_failures": len(late) + int(unfulfilled),
                "ready_lateness_percentiles_ms": percentiles([row["ready_late_ms"] for row in demands]),
                "maximum_ready_lateness_ms": max((row["ready_late_ms"] for row in demands), default=None),
                "consumer_wake_lateness_percentiles_ms": percentiles([row["consumer_wake_late_ms"] for row in demands]),
                "delivery_lateness_percentiles_ms": percentiles([row["delivery_late_ms"] for row in demands]),
                "queue_residence_percentiles_ms": percentiles([row["queue_residence_ms"] for row in demands]),
                "ready_count_at_demand": {str(count): sum(row["ready_count_at_demand"] == count for row in demands)
                                          for count in range(3)},
                "virtual_starvation_union_seconds": sum(end - start for start, end in intervals),
                "virtual_starvation_intervals": intervals,
                "virtual_starvation_interval_scope": "Union of completed ordinary demands' deadline-to-ready intervals; any unfulfilled final demand is reported separately with readiness censored at workload stop",
                "maximum_held_chunks": 1 if self.playback_epochs else 0,
                "unfulfilled_final_demand": pending,
                "epochs": self.playback_epochs,
                "source_admissions": list(self.source_admissions.values())}

    def playback_deliver(self, event, *, reason=None, wake=None, statistics=None):
        consumed = event["consumed_at"]
        if reason is not None:
            self.playback_epochs.append({"epoch": len(self.playback_epochs) + 1,
                "reason": reason, "started_monotonic": consumed,
                "first_ready_at": event["ready_at"], "generation": event["generation"],
                "source_id": event["source_id"], "first_frame": event["frame_start"]})
            if reason == "initial_priming":
                self.playback_epochs[-1]["priming_after_workload_start_ms"] = (consumed - self.started) * 1000
            self.playback_next_deadline = consumed
        self.require(self.playback_next_deadline is not None, "playback epoch has no anchor")
        deadline = self.playback_next_deadline
        row = {"epoch": self.playback_epochs[-1]["epoch"], "kind": reason or "ordinary",
               "generation": event["generation"], "request_id": event["request_id"],
               "source_id": event["source_id"], "frame_start": event["frame_start"],
               "frames": event["frames"], "ready_at": event["ready_at"],
               "consumed_monotonic": consumed, "deadline_monotonic": deadline,
               "queue_residence_ms": max(0, (consumed - event["ready_at"]) * 1000)}
        if reason is None:
            self.require(wake is not None and statistics is not None, "ordinary demand has no observation")
            row.update(consumer_wake_monotonic=wake,
                       ready_late_ms=max(0, (event["ready_at"] - deadline) * 1000),
                       consumer_wake_late_ms=max(0, (wake - deadline) * 1000),
                       delivery_late_ms=max(0, (consumed - deadline) * 1000),
                       ready_count_at_demand=statistics["ready_count"])
            self.playback_demands.append(row)
            self.playback_pending_demand = None
        self.playback_next_deadline = deadline + event["frames"] / RATE
        row["next_deadline_monotonic"] = self.playback_next_deadline
        self.hops[-1]["playback"] = row
        self.log.write(json.dumps({"event": "playback_delivery", **row}) + "\n")
        admission = self.source_admissions.get(event["generation"])
        if admission is not None and "first_ready_at" not in admission:
            admission.update(first_ready_at=event["ready_at"], first_delivered_at=consumed,
                             first_ready_after_load_ms=(event["ready_at"] - admission["load_issued_monotonic"]) * 1000,
                             first_delivered_after_load_ms=(consumed - admission["load_issued_monotonic"]) * 1000,
                             playback_epoch=row["epoch"], first_kind=row["kind"])
            self.log.write(json.dumps({"event": "playback_source_first_output", **admission}) + "\n")
        self.log.flush()

    def playback_load(self):
        issued = time.monotonic()
        self.generation = self.worker.load(self.fixture["path"])
        self.source_admissions[self.generation] = {"generation": self.generation,
            "source_id": self.fixture["sha256"], "load_issued_monotonic": issued}

    def source_complete(self, event):
        self.require(event["source_id"] == self.fixture["sha256"], "completed source identity changed")
        self.require(self.expected == self.fixture["frames"], "source ended before full coverage")
        if self.source_start == 0:
            self.source_loops += 1
            self.source_completions[self.fixture["sha256"]]["full"] += 1
        else:
            self.resumed_completions += 1
            self.source_completions[self.fixture["sha256"]]["resumed"] += 1
        if self.source_paths:
            self.select_source((self.source_index + 1) % len(self.sources))
            if self.delivery_mode == "playback-paced":
                self.playback_load()
            else:
                self.generation = self.worker.load(self.fixture["path"])
        else:
            self.generation = self.worker.seek(0)
        self.expected, self.source_start = 0, 0

    def select_source(self, index):
        self.source_index = index
        self.fixture = self.sources[index]
        source_id = self.fixture["sha256"]
        self.hashes = self.hashes_by_source.setdefault(source_id, {})
        self.references = self.references_by_source.setdefault(source_id, {})
        self.source_completions.setdefault(source_id, {"full": 0, "resumed": 0})

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
            "load_mode": self.load_mode, "selected_loads": list(self.selected_loads),
            "timing": timing_summary(self.hops), "completed_controls": len([
                control for control in self.controls if control.get("complete")]),
            "full_source_loops": self.source_loops, "failures": self.failures,
            "delivery_mode": self.delivery_mode,
            "playback": {"ordinary_demands": len(self.playback_demands),
                         "ready_deadline_misses": sum(row["ready_late_ms"] > 0 for row in self.playback_demands),
                         "epochs": len(self.playback_epochs),
                         "source_admissions": len(self.source_admissions)},
            "worker_statistics": self.worker.statistics() if self.worker else None,
            "ui_progress": self.ui_progress_summary(),
            "conditions": conditions(),
        })

    def check_load(self):
        self.load.check()
        now = time.monotonic()
        if self.selected_loads:
            if self.last_alive_check is not None:
                self.max_alive_check_gap = max(self.max_alive_check_gap, now - self.last_alive_check)
            self.last_alive_check = now
            self.alive_checks += 1
        self.check_ui_progress(now)

    def ui_progress_summary(self):
        if "ui" not in self.selected_loads:
            return {"requested": False, "status": "not_run"}
        unexposed_span = (max(0.0, (self.finished or time.monotonic()) - self.ui_unexposed_since)
                          if self.ui_unexposed_since is not None else 0.0)
        last_span = self.ui_last_unexposed_span
        if self.ui_unexposed_since is not None:
            last_span = {"started_monotonic": self.ui_unexposed_since,
                         "ended_monotonic": self.finished, "seconds": unexposed_span}
        return {"requested": True, "path": str(self.args.ui_progress_file), "checks": self.ui_checks,
                "check_interval_seconds": UI_PROGRESS_INTERVAL,
                "startup_grace_seconds": UI_STARTUP_GRACE,
                "maximum_allowed_gap_seconds": UI_MAX_GAP,
                "maximum_progress_age_seconds": self.ui_max_progress_age,
                "maximum_frame_age_seconds": self.ui_max_frame_age,
                "maximum_check_gap_seconds": self.ui_max_check_gap,
                "unexposed_observations": self.ui_unexposed_observations,
                "maximum_observed_unexposed_span_seconds": max(self.ui_max_unexposed_span, unexposed_span),
                "last_unexposed_span": last_span,
                "exposure_scope": "Sampled heartbeat exposure; spans end at recovery or workload stop, or remain open during the run",
                "read_errors": dict(self.ui_progress_errors),
                "last_progress": self.ui_last_progress,
                "gate_failure": self.ui_gate_failure}

    def check_ui_progress(self, now):
        if "ui" not in self.selected_loads:
            return
        if self.ui_watch_started is None:
            self.ui_watch_started = now
        if now < self.ui_next_check:
            return
        self.ui_next_check = now + UI_PROGRESS_INTERVAL
        if self.ui_last_check is not None:
            self.ui_max_check_gap = max(self.ui_max_check_gap, now - self.ui_last_check)
        self.ui_last_check = now
        self.ui_checks += 1
        startup = now - self.ui_watch_started <= UI_STARTUP_GRACE
        pulse = None
        try:
            pulse = json.loads(self.args.ui_progress_file.read_text())
            for field in ("timestamp_monotonic_ns", "frames_swapped", "last_frame_monotonic_ns"):
                if (isinstance(pulse.get(field), bool) or not isinstance(pulse.get(field), int)
                        or pulse[field] < 0):
                    raise ValueError(f"invalid UI progress field: {field}")
            if not all(isinstance(pulse.get(field), bool) for field in
                       ("exposed", "scene_graph_initialized")):
                raise ValueError("invalid UI visibility/scene graph fields")
            expected_pid = next((child.pid for name, child, *_ in self.load.children
                                 if name == "ui"), None)
            if expected_pid is not None and pulse.get("pid") != expected_pid:
                raise ValueError("UI progress PID does not match the child")
            previous = self.ui_last_progress
            if previous is not None and (pulse["frames_swapped"] < previous["frames_swapped"]
                    or pulse["timestamp_monotonic_ns"] < previous["timestamp_monotonic_ns"]):
                raise ValueError("UI progress counters or timestamp moved backward")
            progress_age = now - pulse["timestamp_monotonic_ns"] / 1e9
            frame_age = (now - pulse["last_frame_monotonic_ns"] / 1e9
                         if pulse["last_frame_monotonic_ns"] else now - self.ui_watch_started)
            if min(progress_age, frame_age) < -0.5:
                raise ValueError("UI progress uses an incompatible future clock")
            if previous is None or pulse["frames_swapped"] > previous["frames_swapped"]:
                self.ui_last_advance = (pulse["last_frame_monotonic_ns"] / 1e9
                                        if pulse["frames_swapped"] else self.ui_watch_started)
            advance_age = now - self.ui_last_advance
            self.ui_last_progress = pulse
            self.ui_max_progress_age = max(self.ui_max_progress_age, progress_age)
            self.ui_max_frame_age = max(self.ui_max_frame_age, frame_age, advance_age)
            observed_at = min(now, pulse["timestamp_monotonic_ns"] / 1e9)
            exposure_event = None
            if not pulse["exposed"]:
                self.ui_unexposed_observations += 1
                if self.ui_unexposed_since is None:
                    self.ui_unexposed_since = observed_at
                    exposure_event = {"exposed": False, "unexposed_since_monotonic": observed_at}
                unexposed_span = max(0.0, now - self.ui_unexposed_since)
                self.ui_max_unexposed_span = max(self.ui_max_unexposed_span, unexposed_span)
            else:
                unexposed_span = 0.0
                if self.ui_unexposed_since is not None:
                    span = {"started_monotonic": self.ui_unexposed_since,
                            "ended_monotonic": observed_at,
                            "seconds": max(0.0, observed_at - self.ui_unexposed_since)}
                    self.ui_last_unexposed_span = span
                    self.ui_max_unexposed_span = max(self.ui_max_unexposed_span, span["seconds"])
                    self.ui_unexposed_since = None
                    unexposed_span = span["seconds"]
                    exposure_event = {"exposed": True, "unexposed_span": span}
            if exposure_event is not None and self.log is not None:
                self.log.write(json.dumps({"event": "ui_exposure_changed", "timestamp_monotonic": now,
                                           "progress": dict(pulse), **exposure_event}) + "\n")
                self.log.flush()
            if not startup:
                self.require(progress_age <= UI_MAX_GAP, "UI progress heartbeat became stale")
                self.require(max(frame_age, advance_age) <= UI_MAX_GAP,
                             "UI frame notifications stopped advancing for over 30 seconds")
                self.require(pulse["scene_graph_initialized"], "UI scene graph is not initialized")
                self.require(unexposed_span <= UI_MAX_GAP,
                             "UI window remained unexposed for over 30 seconds")
        except (OSError, ValueError, TypeError, AttributeError, RuntimeError) as error:
            label = type(error).__name__
            self.ui_progress_errors[label] = self.ui_progress_errors.get(label, 0) + 1
            if startup:
                return
            if self.ui_gate_failure is None:
                self.ui_gate_failure = {"timestamp_monotonic": now, "error": str(error),
                                        "progress": dict(pulse) if isinstance(pulse, dict) else pulse}
                if self.log is not None:
                    self.log.write(json.dumps({"event": "ui_gate_failure", **self.ui_gate_failure}) + "\n")
                    self.log.flush()
            raise RuntimeError(f"required UI progress failed: {error}") from error

    def ui_coverage(self, report):
        if "ui" not in self.selected_loads:
            return {"requested": False, "status": "not_run", "passed": None}
        observation = report.get("frame_observation", {})
        if not isinstance(observation, dict):
            observation = {}
        counts = observation.get("frames_per_60_seconds", [])
        valid_counts = isinstance(counts, list) and bool(counts) and all(
            isinstance(count, int) and not isinstance(count, bool) and count >= 0 for count in counts)
        if not valid_counts:
            counts = []
        boundaries = [observation.get(field) for field in
                      ("started_monotonic_ns", "stopped_monotonic_ns")]
        valid_boundaries = all(isinstance(value, int) and not isinstance(value, bool)
                               and value > 0 for value in boundaries)
        start, stop = (value / 1e9 for value in boundaries) if valid_boundaries else (0, 0)
        complete, zero = [], []
        if self.started is not None and self.finished is not None:
            for index, count in enumerate(counts):
                begin, end = start + index * 60, start + (index + 1) * 60
                if end <= stop and end > self.started and begin < self.finished:
                    complete.append(index)
                    if count <= 0:
                        zero.append(index)
        gap = observation.get("maximum_notification_gap_ms")
        progress = self.ui_progress_summary()
        checks = {"progress_observed": self.ui_last_progress is not None,
                  "runtime_gate_passed": self.ui_gate_failure is None,
                  "observed_unexposed_spans_within_limit": progress["maximum_observed_unexposed_span_seconds"] <= UI_MAX_GAP,
                  "final_observation_valid": valid_counts and valid_boundaries,
                  "observation_covers_workload": (self.started is not None and self.finished is not None
                                                   and 0 < start <= self.started and stop >= self.finished),
                  "minute_counts_match_total": bool(counts) and sum(counts) == report.get("frames_swapped"),
                  "complete_minute_bins_nonzero": not zero and (bool(complete) or self.args.seconds < 60),
                  "notification_gap_within_limit": (not isinstance(gap, bool)
                                                     and isinstance(gap, (int, float))
                                                     and 0 <= gap <= UI_MAX_GAP * 1000)}
        passed = all(checks.values())
        return {"requested": True, "status": "passed" if passed else "failed",
                "passed": passed, "checks": checks,
                "complete_minute_bins_intersecting_workload": complete,
                "zero_complete_minute_bins_intersecting_workload": zero,
                "minute_bin_scope": "Complete 60-second UI-observation bins overlapping the workload; partial final UI bins are excluded",
                "maximum_notification_gap_ms": gap,
                "progress": progress,
                "scope": "GUI-thread frameSwapped notifications and initialized heartbeat with a 30-second unexposed budget; not physical presentation latency"}

    def audio_coverage(self, report):
        if "audio" not in self.selected_loads:
            return {"requested": False, "status": "not_run", "passed": None,
                    "concurrent_workload_seconds": None}
        boundaries = [report.get(field) for field in
                      ("streaming_started_monotonic_ns", "stopped_monotonic_ns")]
        valid = (all(isinstance(value, int) and not isinstance(value, bool) and value > 0
                     for value in boundaries) and boundaries[1] >= boundaries[0])
        overlap = None
        if valid and self.started is not None and self.finished is not None:
            start, stop = (value / 1e9 for value in boundaries)
            overlap = max(0.0, min(self.finished, stop) - max(self.started, start))
        passed = report.get("status") == "ok" and overlap is not None and overlap >= self.args.seconds
        return {"requested": True, "status": "passed" if passed else "failed", "passed": passed,
                "streaming_interval_valid": valid, "concurrent_workload_seconds": overlap,
                "scope": "Independent silent audio probe streaming interval; no stem playback claim"}

    def event(self, timeout=0.25):
        self.check_load()
        event = self.worker.poll(timeout)
        consumed = time.monotonic() if self.delivery_mode == "playback-paced" else None
        self.progress()
        if event is None:
            return None
        record = json_event(event)
        record["consumed_at"] = consumed if consumed is not None else time.monotonic()
        if self.delivery_mode == "playback-paced":
            event["consumed_at"] = record["consumed_at"]
        self.log.write(json.dumps(record) + "\n")
        self.log.flush()
        if event["event"] == "runtime_ready":
            self.runtime.append(record)
            self.require(len(self.runtime) == 1, "native backend reloaded during soak")
        elif event["event"] == "source_ready":
            self.statistics_reused += bool(event["reused"])
            if event["generation"] == self.generation:
                self.require(event["source_id"] == self.fixture["sha256"]
                             and event["source_frames"] == self.fixture["frames"],
                             "source preparation identity/geometry changed")
                normalization = {key: event[key] for key in ("mean", "std", "source_frames")}
                previous = self.source_normalization.setdefault(event["source_id"], normalization)
                self.require(previous == normalization, "unchanged source normalization changed")
                admission = self.source_admissions.get(event["generation"])
                if admission is not None:
                    admission.update(source_ready_at=event["timestamp"],
                                     source_preparation_ms=event.get("preparation_ms"))
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

    def capture_parity_failure(self, event, samples, reference, difference, parity):
        record = {"event": "parity_failure", "timestamp_monotonic": time.monotonic(),
                  "identity": {key: event[key] for key in (
                      "generation", "request_id", "source_id", "model_id", "frame_start",
                      "window_start_frame", "frames")},
                  "parity": dict(parity), "format": "F32LE", "layout": "stem, frame, channel",
                  "maximum_artifact_bytes": 4 * HOP * 2 * 4,
                  "artifacts": {}, "capture_errors": []}
        if not math.isfinite(record["parity"]["max_abs_difference"]):
            record["parity"]["max_abs_difference"] = None
        self.parity_failure = record
        try:
            failed = ~np.isclose(samples, reference, rtol=parity["rtol"], atol=parity["atol"])
            record["failed_elements"] = int(np.count_nonzero(failed))
            record["failed_elements_per_stem"] = np.count_nonzero(failed, axis=(1, 2)).tolist()
            record["difference_element_counts"] = {
                "finite": int(np.count_nonzero(np.isfinite(difference))),
                "nan": int(np.count_nonzero(np.isnan(difference))),
                "infinite": int(np.count_nonzero(np.isinf(difference)))}
            record["difference_dtype"] = str(difference.dtype)
            maxima = np.max(np.abs(difference), axis=(1, 2)).tolist()
            record["max_abs_difference_per_stem"] = [value if math.isfinite(value) else None
                                                     for value in maxima]
        except Exception as error:
            record["capture_errors"].append({"phase": "comparison", "error": str(error)})
        # The compared slices contain at most one four-stem hop each. Only the
        # first failure reaches this path; no successful PCM is retained.
        for name, values in (("actual", samples), ("reference", reference)):
            try:
                path = self.root / f"parity-failure-{name}.f32"
                artifact = {"path": str(path), "shape": list(values.shape),
                            "bytes": values.size * 4, "written": False}
                record["artifacts"][name] = artifact
                if (values.ndim != 3 or values.shape[0] != 4 or values.shape[2] != 2
                        or not 0 < values.shape[1] <= HOP):
                    raise ValueError("parity capture exceeds its one-hop artifact bound")
                values = np.asarray(values, dtype="<f4")
                artifact["sha256"] = hashlib.sha256(values.tobytes()).hexdigest()
                values.tofile(path)
                artifact["written"] = True
            except Exception as error:
                record["capture_errors"].append({"phase": name, "error": str(error)})
        try:
            self.log.write(json.dumps(record, allow_nan=False) + "\n")
            self.log.flush()
        except Exception as error:
            record["capture_errors"].append({"phase": "event_log", "error": str(error)})

    def accept(self, event):
        self.require(event["source_id"] == self.fixture["sha256"], "chunk reference source changed")
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
            if not equivalent:
                try:
                    self.capture_parity_failure(event, samples, reference, difference, parity)
                except Exception as error:
                    self.failure("parity_failure_capture", error)
            self.require(equivalent, "seek differs from continuous reference beyond tolerance")
            self.trimmed_parity_checks += 1
        elif origin in self.hashes:
            if sample_hash != self.hashes[origin] and origin in self.references:
                try:
                    reference = self.references[origin]
                    difference = samples - reference
                    mismatch = {"kind": "same_grid_sha256", "exact": False,
                                "numeric_parity": bool(np.allclose(samples, reference, rtol=1e-4, atol=1e-5)),
                                "max_abs_difference": float(np.max(np.abs(difference))),
                                "rtol": 1e-4, "atol": 1e-5,
                                "expected_sha256": self.hashes[origin], "actual_sha256": sample_hash}
                    self.capture_parity_failure(event, samples, reference, difference, mismatch)
                except Exception as error:
                    self.failure("parity_failure_capture", error)
            self.require(sample_hash == self.hashes[origin], "repeated same-grid chunk hash changed")
            parity = {"kind": "same_grid_sha256", "exact": True}
            self.parity_checks += 1
        else:
            self.hashes[origin] = sample_hash
            if origin in (HOP, 2 * HOP):
                self.references[origin] = samples.copy()
        row = {key: event[key] for key in (
            "generation", "request_id", "source_id", "frame_start", "frames", "window_start_frame",
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
                or active["generation"] != self.generation
                or (self.delivery_mode == "playback-paced"
                    and (time.monotonic() >= self.playback_next_deadline
                         or self.held is None or not self.worker.is_current(self.held)))):
            self.finished_control_attempts += 1
            return
        began = time.monotonic()
        self.require(active["source_id"] == self.fixture["sha256"], "active control source changed")
        control = {"at_workload_seconds": began - self.started,
                   "source_id": self.fixture["sha256"],
                   "superseded_generation": self.generation,
                   "superseded_request_id": active["request_id"],
                   "started_to_cancel_ms": (began - active["timestamp"]) * 1000,
                   "hardware_submission_confirmed": False, "complete": False}
        self.controls.append(control)
        if self.delivery_mode == "playback-paced":
            self.require(self.held is not None and self.worker.is_current(self.held),
                         "paced cancellation has no current held playing chunk")
            self.playback_epochs[-1].update(ended_monotonic=began, end_reason="explicit_cancel",
                                            interrupted_next_deadline=self.playback_next_deadline)
            self.log.write(json.dumps({"event": "playback_interruption",
                "epoch": self.playback_epochs[-1]["epoch"], "timestamp_monotonic": began,
                "next_deadline_monotonic": self.playback_next_deadline,
                "superseded_generation": self.generation}) + "\n")
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
        if self.delivery_mode == "playback-paced":
            self.playback_deliver(chunk, reason="seek_recovery")
        control.update(ready_after_resume_ms=(chunk["ready_at"] - resumed) * 1000,
                       ready_after_cancel_ms=(chunk["ready_at"] - began) * 1000,
                       delivered_after_cancel_ms=(time.monotonic() - began) * 1000,
                       post_cancel_chunks=0, parity=self.hops[-1]["parity"], complete=True)
        while self.next_control <= time.monotonic():
            self.next_control += self.args.control_interval
        self.progress(force=True)

    def paced_workload(self):
        self.playback_load()
        first = self.wait(lambda event: event["event"] == "started",
                          self.args.request_timeout * 2 + 5)
        self.started = first["timestamp"]
        self.deadline = self.started + self.args.seconds
        self.next_control = self.started + self.args.control_interval
        self.progress(force=True)
        first_chunk = self.wait(lambda event: event["event"] == "chunk",
                                self.args.request_timeout * 2 + 5)
        self.accept(first_chunk)
        self.playback_deliver(first_chunk, reason="initial_priming")
        while time.monotonic() < self.deadline:
            # After consuming the final source chunk, process only its trailing
            # complete event. The following source can prepare while that held
            # chunk plays; the ordinary playback clock is never re-anchored.
            if self.expected == self.fixture["frames"]:
                complete = self.wait(lambda event: event["event"] == "complete"
                                     and event["generation"] == self.generation,
                                     self.args.request_timeout + 2)
                self.source_complete(complete)
            now = time.monotonic()
            if now >= self.deadline:
                break
            if now < self.playback_next_deadline:
                self.check_load()
                self.progress()
                if (now >= self.next_control
                        and self.playback_next_deadline - now >= 0.20
                        and self.deadline - now > min(self.args.request_timeout * 3 + 10,
                                                     self.args.seconds / 2)
                        and HOP in self.references and 2 * HOP in self.references
                        and self.held is not None and self.worker.is_current(self.held)):
                    active = self.worker.statistics()["inflight"]
                    if (active and active["generation"] == self.generation
                            and active["source_id"] == self.fixture["sha256"]):
                        self.active_control(active)
                        continue
                time.sleep(max(0, min(0.25, self.playback_next_deadline - time.monotonic(),
                                     self.deadline - time.monotonic())))
                continue
            wake = now
            statistics = self.worker.statistics()
            self.require(statistics["ready_count"] <= 2 and statistics["ready_high_water"] <= 2,
                         "ready output exceeded its two-hop bound")
            self.playback_pending_demand = {"epoch": self.playback_epochs[-1]["epoch"],
                "deadline_monotonic": self.playback_next_deadline,
                "consumer_wake_monotonic": wake, "generation": self.generation,
                "source_id": self.fixture["sha256"], "frame_start": self.expected,
                "ready_count_at_demand": statistics["ready_count"]}
            self.log.write(json.dumps({"event": "playback_demand", **self.playback_pending_demand}) + "\n")
            self.log.flush()
            while time.monotonic() < self.deadline:
                event = self.event(min(0.25, max(0, self.deadline - time.monotonic())))
                if event is not None and event["event"] == "chunk":
                    self.accept(event)
                    self.playback_deliver(event, wake=wake, statistics=statistics)
                    break
        self.check_load()
        self.finished = time.monotonic()
        if (self.playback_pending_demand is None and self.playback_next_deadline is not None
                and self.playback_next_deadline < self.deadline):
            rotation_pending = self.expected == self.fixture["frames"]
            next_source = (self.sources[(self.source_index + 1) % len(self.sources)]
                           if rotation_pending and self.source_paths else self.fixture)
            self.playback_pending_demand = {"epoch": self.playback_epochs[-1]["epoch"],
                "deadline_monotonic": self.playback_next_deadline,
                "consumer_wake_monotonic": None,
                "generation": None if rotation_pending else self.generation,
                "source_id": next_source["sha256"],
                "frame_start": 0 if rotation_pending else self.expected,
                "ready_count_at_demand": None, "request_not_issued": True,
                "next_source_not_admitted": rotation_pending,
                "loaded_generation_at_terminal": self.generation,
                "loaded_source_id_at_terminal": self.fixture["sha256"],
                "loaded_expected_frame_at_terminal": self.expected}
        if self.playback_pending_demand is not None:
            pending = self.playback_pending_demand
            wake = pending["consumer_wake_monotonic"]
            pending.update(workload_stopped_monotonic=self.finished,
                workload_planned_stop_monotonic=self.deadline,
                terminal_observed_monotonic=self.finished,
                request_not_issued=pending.get("request_not_issued", False),
                next_source_not_admitted=pending.get("next_source_not_admitted", False),
                observed_wait_seconds=max(0, self.finished - wake) if wake is not None else None,
                scope="Unfulfilled final virtual clock obligation due before planned stop; readiness censored. If request_not_issued, wake/queue/wait are unknown. If next_source_not_admitted, source/frame describe the planned next traversal and generation is unknown; loaded state is recorded separately")
            self.log.write(json.dumps({"event": "playback_unfulfilled_demand", **self.playback_pending_demand}) + "\n")
            self.log.flush()
        if self.playback_epochs:
            self.playback_epochs[-1].update(ended_monotonic=self.finished,
                                            end_reason="workload_stop")
        self.require(self.elapsed() >= self.args.seconds, "workload ended before requested duration")

    def workload(self):
        if self.delivery_mode == "playback-paced":
            return self.paced_workload()
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
                self.source_complete(event)
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
                  "delivery_mode": self.delivery_mode,
                  "load_mode": self.load_mode, "selected_loads": list(self.selected_loads),
                  "control_interval_seconds": self.args.control_interval,
                  "sample_interval_seconds": self.args.sample_interval,
                  "load_seconds_fallback": self.args.load_seconds,
                  "window_frames": WINDOW, "hop_frames": HOP, "overlap_frames": OVERLAP,
                  "conditions_at_start": conditions(), "pins": {}}
        result["ui_progress_file"] = str(self.args.ui_progress_file) if self.args.ui_progress_file else None
        interrupted = False
        try:
            pins = [("runtime", self.args.runtime), ("model", self.args.model),
                    ("soak_source", Path(__file__)),
                    ("shared_benchmark_source", Path(__file__).with_name("stem_worker.py")),
                    ("worker_source", Path(__file__).parents[1] / "stems/worker.py"),
                    ("sampler_source", Path(__file__).with_name("worker_support.py"))]
            if "ui" in self.selected_loads:
                pins.append(("ui", self.args.ui))
            if "audio" in self.selected_loads:
                pins.append(("audio_probe", self.args.audio_probe))
            for name, path in pins:
                with path.open("rb") as stream:
                    result["pins"][name] = {"path": str(path.resolve()),
                                            "bytes": path.stat().st_size,
                                            "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}
            revision = subprocess.run(["git", "rev-parse", "HEAD"], text=True,
                                      capture_output=True, check=True)
            result["repository_head"] = revision.stdout.strip()
            self.log = (self.root / "events.jsonl").open("w")
            if self.source_paths:
                self.sources = [wav_source(path) for path in self.source_paths]
            else:
                self.generated_fixture = self.root / "source.wav"
                self.sources = [create_fixture(self.generated_fixture, FIXTURE_FRAMES / RATE)]
            self.source_completions = {source["sha256"]: {"full": 0, "resumed": 0}
                                       for source in self.sources}
            self.select_source(0)
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
                if self.generated_fixture is not None:
                    self.generated_fixture.unlink(missing_ok=True)
            except OSError as error:
                self.failure("fixture_cleanup", error)
            result.update(workload_started_monotonic=self.started, workload_stopped_monotonic=self.finished,
                          workload_seconds=self.elapsed(), timing=self.summaries(), controls=self.controls,
                          runtime_ready=self.runtime, source_statistics_reused=self.statistics_reused,
                          full_source_loops=self.source_loops, resumed_source_completions=self.resumed_completions,
                          control_attempts_finished_before_cancel=self.finished_control_attempts,
                          reference_hashes={str(frame): digest for frame, digest in self.hashes.items()},
                          source_mode=self.source_mode, sources=self.sources,
                          current_source_id=self.fixture["sha256"] if self.fixture else None,
                          reference_hashes_by_source={source_id: {str(frame): digest for frame, digest in hashes.items()}
                                                      for source_id, hashes in self.hashes_by_source.items()},
                          source_normalization_by_source=self.source_normalization,
                          source_completions=self.source_completions,
                          same_grid_hash_checks=self.parity_checks, trimmed_seek_checks=self.trimmed_parity_checks,
                          stale_chunks_delivered=self.stale_chunks,
                          load_alive_checks=self.alive_checks if self.selected_loads else None,
                          maximum_load_check_gap_seconds=self.max_alive_check_gap if self.selected_loads else None,
                          load_coverage_scope="Selected child process liveness; requested UI also requires atomic GUI heartbeat, per-minute frameSwapped counts and notification gaps; not physical presentation timing",
                          conditions_at_finish=conditions(),
                          finished_at_utc=datetime.now(timezone.utc).isoformat())
            result["playback"] = self.playback_summary()
            if self.parity_failure is not None:
                result["parity_failure"] = self.parity_failure
            audio = result.get("load", {}).get("children", {}).get("audio", {}).get("report", {})
            ui = result.get("load", {}).get("children", {}).get("ui", {}).get("report", {})
            result["ui_coverage"] = self.ui_coverage(ui)
            result["audio_coverage"] = self.audio_coverage(audio)
            result["concurrent_audio_workload_seconds"] = result["audio_coverage"]["concurrent_workload_seconds"]
            if result["status"] == "ok":
                if "audio" in self.selected_loads and not result["audio_coverage"]["passed"]:
                    self.failure("audio_coverage", "actual streaming interval did not cover requested workload")
                statistics = result["worker_statistics"]
                if statistics["backend_start_count"] != 1 or len(self.runtime) != 1:
                    self.failure("backend_identity", "soak did not retain exactly one backend")
                if statistics["telemetry_dropped"]:
                    self.failure("telemetry", "worker telemetry events were dropped")
                if self.args.seconds >= 60 and not any(control.get("complete") for control in self.controls):
                    self.failure("control_coverage", "no active cancellation/seek/resume batch completed")
                if "ui" in self.selected_loads and not result["ui_coverage"]["passed"]:
                    self.failure("ui_coverage", "required UI heartbeat/frame notification coverage was not maintained")
            result["native_trace_bytes"] = (self.root / "native-trace.jsonl").stat().st_size if (self.root / "native-trace.jsonl").exists() else 0
            result["failures"] = self.failures
            if self.failures and not interrupted:
                result["status"] = "error"
            result["acceptance"] = {
                "requested_wall_duration_completed": self.elapsed() >= self.args.seconds,
                "sixty_minute_workload_completed": self.elapsed() >= 3600 and self.args.seconds >= 3600,
                "audio_covers_requested_duration": result["audio_coverage"]["passed"],
                "required_UI_coverage_completed": result["ui_coverage"]["passed"],
                "selected_load_coverage_completed": all(result[f"{name}_coverage"]["passed"] is True
                                                        for name in self.selected_loads),
                "controls_completed": sum(bool(control.get("complete")) for control in self.controls),
                "observed_continuous_p99_within_target": result["timing"]["continuous"]["observed_p99_within_target"],
                "ordinary_playback_ready_deadlines_met": (not self.playback_pending_demand and
                    all(row["ready_late_ms"] == 0 for row in self.playback_demands))
                    if self.delivery_mode == "playback-paced" else None,
                "parity_scope": "Full same-grid chunks require identical SHA-256; trimmed seeks use allclose(rtol=1e-4, atol=1e-5) and record exact equality/max difference.",
                "active_cancel_scope": "Wait 150 ms after the supervisor started event, then revalidate generation/request. No GPU submission timestamp is observed.",
                "scope": f"{'prepared WAV corpus' if self.source_paths else 'synthetic'} {self.delivery_mode} delivery with requested load={self.load_mode}; no stem playback/full deck or musical-quality claim",
            }
            try:
                self.progress(force=True, status=result["status"])
            except OSError as error:
                self.failure("progress_report", error)
                if not interrupted:
                    result["status"] = "error"
            acceptance = result["acceptance"]
            acceptance["full_audio_UI_soak_qualified"] = (
                self.load_mode == "both" and result["status"] == "ok"
                and acceptance["sixty_minute_workload_completed"]
                and acceptance["selected_load_coverage_completed"]
                and acceptance["controls_completed"] > 0
                and acceptance["observed_continuous_p99_within_target"]
                and (self.delivery_mode != "playback-paced" or
                     acceptance["ordinary_playback_ready_deadlines_met"]))
            atomic_json(self.root / "result.json", result)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "model", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--ui", type=Path)
    parser.add_argument("--audio-probe", type=Path)
    parser.add_argument("--source-wav", type=Path, action="append",
                        help="Prepared stereo 44.1 kHz PCM16 source; repeat to rotate files after completion")
    parser.add_argument("--delivery-mode", choices=("saturated", "playback-paced"), default="saturated",
                        help="Consume immediately, or hold each chunk for its actual audio duration")
    parser.add_argument("--load", dest="load_mode", choices=tuple(Load.MODES), default="both")
    parser.add_argument("--renderer", choices=("opengl", "vulkan", "software"), default="opengl")
    parser.add_argument("--seconds", type=float, default=3600)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--quantum", type=int, choices=(256, 512), default=256)
    parser.add_argument("--request-timeout", type=float, default=30)
    parser.add_argument("--control-interval", type=float)
    parser.add_argument("--sample-interval", type=float, default=1.0)
    args = parser.parse_args()
    for name, path in (("ui", args.ui), ("audio", args.audio_probe)):
        if name in Load.MODES[args.load_mode] and path is None:
            parser.error(f"--{'audio-probe' if name == 'audio' else name} is required for --load {args.load_mode}")
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

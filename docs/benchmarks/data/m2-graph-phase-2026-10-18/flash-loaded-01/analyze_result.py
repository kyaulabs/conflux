#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Derive a bounded post-run graph-phase report; never launch/query a workload.

Usage: analyze_result.py COMPLETED_RUN_DIRECTORY [--output FRESH_JSON_PATH]
Only this run's retained result/events/trace and optional sibling lifecycle JSON
are read. Comparison timings, media, model/binary files and live host state are
never read. The producer's continuous-hop p99 gate remains authoritative.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import sys

TARGET_MS = 4095.0
PHASES = ("graph_upload_ms", "graph_compute_ms", "graph_readback_ms")
NATIVE_FIELDS = ("elapsed_ms", "inference_wall_ms", "inference_cpu_ms", "frontend_ms",
                 "graph_ms", "postprocess_ms", "graph_rebuild_ms", *PHASES,
                 "voluntary_context_switches", "involuntary_context_switches")
FAULTS = ("callback_execution_deadline_misses", "missing_buffers", "invalid_buffers",
          "truncated_frames", "observed_graph_position_discontinuities",
          "observed_position_clock_xrun_recovery_flags")
MAX_LINE = 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def nonnegative(value):
    return number(value) and value >= 0


def finite_nonnegative(value, field):
    require(nonnegative(value), f"invalid {field}")
    return value


def decode(data):
    def invalid(value):
        raise ValueError(f"invalid JSON constant: {value}")
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(data, parse_constant=invalid, object_pairs_hook=object_pairs)


def identity(value):
    return (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)


def artifact(path, maximum, pins, label, *, lines=False):
    """Yield bounded file content/JSONL from one retained regular descriptor."""
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode) and 0 < before.st_size <= maximum,
                f"artifact is not a bounded nonempty regular file: {path}")
        digest = hashlib.sha256()
        if lines:
            consumed = 0
            while consumed < before.st_size:
                data = stream.readline(min(MAX_LINE + 1, before.st_size - consumed))
                require(data and len(data) <= MAX_LINE and data.endswith(b"\n"),
                        f"truncated/oversized JSONL record: {path}")
                consumed += len(data)
                digest.update(data)
                yield decode(data)
        else:
            data = stream.read(before.st_size)
            require(len(data) == before.st_size, f"artifact shortened: {path}")
            digest.update(data)
            yield decode(data)
        require(identity(before) == identity(os.fstat(stream.fileno()))
                == identity(os.stat(path, follow_symlinks=False)), f"artifact changed during analysis: {path}")
        pins[label] = {"path": str(path), "bytes": before.st_size, "sha256": digest.hexdigest()}


def read_json(path, pins, label, *, optional=False):
    if optional and not path.exists():
        return None
    values = list(artifact(path, 8 * 1024 * 1024, pins, label))
    require(len(values) == 1 and isinstance(values[0], dict), f"expected JSON object: {path}")
    return values[0]


def distribution(values):
    require(all(nonnegative(value) for value in values), "invalid distribution sample")
    ordered = sorted(values)
    return {"samples": len(ordered),
            "percentiles": {str(p): ordered[math.ceil(len(ordered) * p / 100) - 1]
                            for p in (50, 95, 99)} if ordered else None,
            "min": ordered[0] if ordered else None, "max": ordered[-1] if ordered else None}


def service_summary(rows):
    values = [row["hop_service_ms"] for row in rows]
    stats = distribution(values)
    return {"chunks": len(rows), "frames": sum(row["frames"] for row in rows),
            "hop_service_ms": stats,
            "pass_lt_4095ms": sum(value < TARGET_MS for value in values),
            "miss_ge_4095ms": sum(value >= TARGET_MS for value in values),
            "equal_4095ms": sum(value == TARGET_MS for value in values),
            "producer_hops_above_target_gt_4095ms": sum(value > TARGET_MS for value in values),
            "observed_p99_within_target": stats["percentiles"]["99"] < TARGET_MS if values else None}


def phase_summary(rows):
    grouped = {"overall": rows,
               "pass": [row for row in rows if row["hop_service_ms"] < TARGET_MS],
               "miss": [row for row in rows if row["hop_service_ms"] >= TARGET_MS]}
    result = {}
    for label, selected in grouped.items():
        fields = {}
        for field in PHASES:
            values = [row["metrics"][field] for row in selected if field in row["metrics"]]
            fields[field] = distribution(values)
            fields[field]["missing_rows"] = len(selected) - len(values)
        result[label] = {"rows": len(selected), "fields_ms": fields,
                         "all_phase_fields_observed": bool(selected) and all(
                             all(field in row["metrics"] for field in PHASES) for row in selected)}
    return result


def request_key(row):
    key = (row.get("generation"), row.get("request_id"))
    require(all(type(value) is int and value > 0 for value in key), "invalid request identity")
    return key


def read_events(path, pins):
    hops, started, chunks, discarded = {}, {}, {}, {}
    count = 0
    for row in artifact(path, 64 * 1024 * 1024, pins, "events", lines=True):
        count += 1
        require(count <= 100000 and isinstance(row, dict), "event count/shape exceeded bound")
        event = row.get("event")
        if event not in ("soak_hop", "started", "chunk", "discarded"):
            continue
        key = request_key(row)
        destination = {"soak_hop": hops, "started": started, "chunk": chunks, "discarded": discarded}[event]
        require(key not in destination, f"duplicate {event} request record")
        destination[key] = row
        if event != "soak_hop":
            continue
        for field in ("hop_service_ms", "request_ms", "source_read_ms", "overlap_ms", "ready_at", "elapsed_seconds"):
            finite_nonnegative(row.get(field), field)
        require(type(row.get("frames")) is int and 0 < row["frames"] <= 257985,
                "invalid delivered frame count")
        require(row.get("kind") in ("continuous", "seek_first") and isinstance(row.get("metrics"), dict),
                "invalid delivery kind/native metrics")
        for field in NATIVE_FIELDS:
            if field in row["metrics"]:
                finite_nonnegative(row["metrics"][field], f"native {field}")
        require("graph_ms" in row["metrics"], "native graph_ms missing")
    require(hops, "no accepted soak_hop records; empty groups cannot qualify")
    for key, row in hops.items():
        require(key in chunks and key in started, "accepted hop lacks original chunk/started event")
        for field in ("source_id", "frame_start", "frames", "hop_service_ms", "metrics"):
            require(chunks[key].get(field) == row.get(field), f"chunk/soak_hop {field} differs")
        finite_nonnegative(started[key].get("timestamp"), "request start timestamp")
    return list(hops.values()), started, {"event_rows": count, "started_requests_observed": len(started),
        "preroll_starts_observed": sum(row.get("preroll") is True for row in started.values()),
        "chunk_records_observed": len(chunks), "accepted_delivered_hops": len(hops),
        "chunk_records_not_accepted": len(set(chunks) - set(hops)),
        "discarded_requests_observed": len(discarded),
        "deduplication": "Analyze soak_hop only; matching original chunk records are cross-checked, not counted again.",
        "native_coverage_scope": "Metrics describe accepted delivered hops. Preroll, discarded/cancelled and unresolved final requests lack complete retained phase metrics and are excluded."}


def stall(row, started):
    metrics = row["metrics"]
    result = {field: row.get(field) for field in ("generation", "request_id", "source_id", "kind",
              "frame_start", "window_start_frame", "elapsed_seconds", "hop_service_ms", "request_ms",
              "source_read_ms", "overlap_ms", "queue_delay_ms", "ready_gap_ms")}
    result["native_metrics"] = {field: metrics.get(field) for field in NATIVE_FIELDS}
    result["miss_ge_4095ms"] = row["hop_service_ms"] >= TARGET_MS
    begin = started[request_key(row)]["timestamp"]
    result["request_interval_monotonic"] = [begin, begin + row["request_ms"] / 1000]
    complete = all(field in metrics for field in PHASES)
    result["graph_phase_attribution"] = {
        "status": "observed" if complete else "unavailable",
        "largest_observed_phase": max(PHASES, key=lambda field: metrics[field]) if complete else None,
        "phase_sum_ms": sum(metrics[field] for field in PHASES) if complete else None,
        "scope": "Largest measured CPU-wall graph phase, not a causal diagnosis or GPU timestamp. Compute includes explicit backend wait; phases need not sum exactly to graph_ms."}
    result["service_outside_measured_request_read_overlap_ms"] = (
        row["hop_service_ms"] - row["request_ms"] - row["source_read_ms"] - row["overlap_ms"])
    result["request_outside_native_elapsed_ms"] = (
        row["request_ms"] - metrics["elapsed_ms"]) if "elapsed_ms" in metrics else None
    result["sampled_context"] = {"samples": 0, "profiles": {}, "sensor_peak_C": {}, "gpu_clock_MHz": {}}
    return result


def thermal_trace(path, result, top_stalls, pins):
    if not path.exists():
        return {"status": "unavailable", "scope": "No retained trace; no temperature values inferred."}
    units = result.get("native_memory_cpu", {}).get("system_sampling", {}).get("fields", {})
    sensor_paths = [key for key, value in units.items() if value == "millidegrees_C"]
    clocks = [key for key, value in units.items() if value == "MHz"]
    sensors = {key: {"values": [], "spans": [], "active": None, "previous_low": None,
                     "previous_low_capture_started": None,
                     "missing": 0} for key in sensor_paths}
    profiles, intervals, first, previous, count, workload_samples = {}, [], None, None, 0, 0
    workload_begin = result.get("workload_started_monotonic")
    workload_end = result.get("workload_stopped_monotonic")
    valid_workload = (nonnegative(workload_begin) and nonnegative(workload_end)
                      and workload_end >= workload_begin)
    profile_path = "/sys/firmware/acpi/platform_profile"
    def end_span(sensor, following_low=None):
        active = sensor["active"]
        if active is not None:
            active["observed_span_seconds"] = active["last_high"] - active["first_high"]
            active["following_below_at"] = following_low
            active["row_timestamp_bracket_seconds"] = (following_low - active["preceding_below_at"]
                if following_low is not None and active["preceding_below_at"] is not None else None)
            active["capture_conservative_bracket_seconds"] = (
                following_low - active["preceding_below_capture_started_at"]
                if following_low is not None and active["preceding_below_capture_started_at"] is not None else None)
            sensor["spans"].append(active)
            sensor["active"] = None
    for row in artifact(path, 512 * 1024 * 1024, pins, "native_trace", lines=True):
        count += 1
        require(count <= 20000 and isinstance(row, dict), "trace count/shape exceeded bound")
        when = finite_nonnegative(row.get("timestamp_monotonic"), "trace timestamp")
        capture_start = row.get("capture_started_monotonic")
        if not nonnegative(capture_start) or capture_start > when:
            capture_start = None
        require(previous is None or when >= previous, "trace timestamps move backwards")
        if first is None:
            first = when
        workload_samples += bool(valid_workload and workload_begin <= when <= workload_end)
        if previous is not None:
            intervals.append(when - previous)
        previous = when
        values = row.get("system_values", {})
        require(isinstance(values, dict), "invalid trace system_values")
        profile = values.get(profile_path)
        if isinstance(profile, str):
            profiles[profile] = profiles.get(profile, 0) + 1
        for key, sensor in sensors.items():
            value = values.get(key)
            if not number(value):
                sensor["missing"] += 1
                end_span(sensor)
                sensor["previous_low"] = None
                sensor["previous_low_capture_started"] = None
                continue
            temperature = value / 1000
            sensor["values"].append(temperature)
            if temperature >= 85:
                if sensor["active"] is None:
                    sensor["active"] = {"first_high": when, "last_high": when, "samples": 0,
                                         "preceding_below_at": sensor["previous_low"],
                                         "preceding_below_capture_started_at": sensor["previous_low_capture_started"]}
                sensor["active"]["last_high"] = when
                sensor["active"]["samples"] += 1
            else:
                end_span(sensor, when)
                sensor["previous_low"] = when
                sensor["previous_low_capture_started"] = capture_start
        for top in top_stalls:
            if not top["request_interval_monotonic"][0] <= when <= top["request_interval_monotonic"][1]:
                continue
            context = top["sampled_context"]
            context["samples"] += 1
            if isinstance(profile, str):
                context["profiles"][profile] = context["profiles"].get(profile, 0) + 1
            for key in sensor_paths:
                if number(values.get(key)):
                    context["sensor_peak_C"][key] = max(context["sensor_peak_C"].get(key, -math.inf), values[key] / 1000)
            for key in clocks:
                if nonnegative(values.get(key)):
                    context["gpu_clock_MHz"].setdefault(key, []).append(values[key])
    observations = {}
    for key, sensor in sensors.items():
        end_span(sensor)
        values, spans = sensor["values"], sensor["spans"]
        bracketed = [span["row_timestamp_bracket_seconds"] for span in spans if span["row_timestamp_bracket_seconds"] is not None]
        conservative = [span["capture_conservative_bracket_seconds"] for span in spans
                        if span["capture_conservative_bracket_seconds"] is not None]
        observations[key] = {"samples": len(values), "missing_samples": sensor["missing"],
            "min_C": min(values) if values else None, "peak_C": max(values) if values else None,
            "samples_ge_85C": sum(value >= 85 for value in values),
            "sampled_high_runs": len(spans),
            "maximum_consecutive_high_sample_span_seconds": max((span["observed_span_seconds"] for span in spans), default=None),
            "maximum_fully_bracketed_row_timestamp_span_seconds": max(bracketed, default=None),
            "maximum_capture_conservative_bracket_seconds": max(conservative, default=None),
            "unbracketed_high_runs": len(spans) - len(bracketed),
            "longest_five_sampled_high_runs": sorted(spans, key=lambda span: span["observed_span_seconds"], reverse=True)[:5]}
    for top in top_stalls:
        context = top["sampled_context"]
        context["gpu_clock_MHz"] = {key: distribution(values) for key, values in context["gpu_clock_MHz"].items()}
        context["scope"] = "Snapshots inside this measured external request interval. Correlation only; native CPU time is not total host CPU load. Missing sensors/clocks remain unavailable."
    observed_sensors = sum(sensor["samples"] > 0 for sensor in observations.values())
    status = ("unavailable" if observed_sensors == 0 else "partial"
              if observed_sensors < len(sensor_paths) or any(sensor["missing_samples"] for sensor in observations.values())
              else "observed")
    return {"status": status, "trace_samples": count,
            "temperature_fields_declared": len(sensor_paths), "temperature_fields_observed": observed_sensors,
            "trace_span_monotonic": [first, previous],
            "workload_span_monotonic": [workload_begin, workload_end] if valid_workload else None,
            "trace_samples_within_workload": workload_samples if valid_workload else None,
            "sensors": observations, "firmware_profile_sample_counts": profiles,
            "maximum_trace_sample_gap_seconds": max(intervals, default=None),
            "scope": "All retained native-trace samples, including any startup/shutdown outside the stated workload span. 85C counts describe observations, not an added thermal pass gate. Consecutive spans and row brackets use timestamps recorded after capture, not precise sensor-read times. Capture-conservative brackets run from preceding below-threshold capture start to following below-threshold row timestamp; missing capture starts remain unavailable. Brackets describe sampled excursions, not continuous temperature or all unseen peaks; gaps are not filled or asserted safe."}


def cancellation(result):
    controls = result.get("controls", [])
    require(isinstance(controls, list) and all(isinstance(row, dict) for row in controls), "invalid controls")
    complete = [row for row in controls if row.get("complete") is True]
    fields = ("cancel_ack_ms", "residual_finish_ms", "resume_ack_ms", "ready_after_resume_ms",
              "ready_after_cancel_ms", "delivered_after_cancel_ms", "started_to_cancel_ms")
    return {"attempts_recorded": len(controls), "completed": len(complete),
            "timings_ms": {field: distribution([row[field] for row in controls if field in row]) for field in fields},
            "held_chunks_rejected_all_completed": all(row.get("held_chunk_rejected") is True for row in complete) if complete else None,
            "post_cancel_chunks_all_completed": sum(row["post_cancel_chunks"] for row in complete)
                if complete and all(type(row.get("post_cancel_chunks")) is int for row in complete) else None,
            "stale_chunks_delivered": result.get("stale_chunks_delivered"),
            "controls": controls, "scope": result.get("acceptance", {}).get("active_cancel_scope",
                "Supervisor invalidation acknowledgement; no confirmed hardware GPU submission or preemption.")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.run_directory.resolve()
    require(root.is_dir(), "supply a completed run directory")
    pins = {}
    result = read_json(root / "result.json", pins, "result")
    require(result.get("schema") == 1 and result.get("status") in ("ok", "error", "interrupted")
            and isinstance(result.get("finished_at_utc"), str), "result is not a closed soak report")
    require(result.get("delivery_mode") == "playback-paced", "expected playback-paced soak result, not comparison/replay")
    require(number(result.get("timing", {}).get("headroom_target_ms")) and math.isclose(
            result["timing"]["headroom_target_ms"], TARGET_MS, rel_tol=0, abs_tol=1e-6),
            "producer headroom target differs from 4095 ms")
    rows, started, coverage = read_events(root / "events.jsonl", pins)
    populations = {"all_delivered": rows,
                   "continuous": [row for row in rows if row["kind"] == "continuous"],
                   "seek_first": [row for row in rows if row["kind"] == "seek_first"]}
    summaries = {key: service_summary(group) for key, group in populations.items()}
    consistency = {}
    for name in ("all_delivered", "continuous"):
        recorded = result.get("timing", {}).get(name, {})
        actual = summaries[name]
        matches = recorded.get("chunks") == actual["chunks"] and recorded.get("frames") == actual["frames"]
        matches = matches and recorded.get("hops_above_target") == actual["producer_hops_above_target_gt_4095ms"]
        for p in ("50", "95", "99"):
            expected = recorded.get("hop_service_percentiles_ms", {}).get(p)
            observed = actual["hop_service_ms"]["percentiles"]
            matches = matches and ((expected is None and observed is None) or
                (number(expected) and observed is not None and math.isclose(expected, observed[p], rel_tol=0, abs_tol=1e-6)))
        consistency[name] = matches
    require(all(consistency.values()), "event-derived service count/frames/percentiles differ from closed result")
    top = [stall(row, started) for row in sorted(populations["continuous"],
           key=lambda row: row["hop_service_ms"], reverse=True)[:5]]
    load = result.get("load", {})
    audio = load.get("children", {}).get("audio", {}).get("report", {})
    ui = load.get("children", {}).get("ui", {}).get("report", {})
    faults = {key: audio.get(key) for key in FAULTS}
    report = {"schema": 1, "status": "derived", "analyzed_at_utc": datetime.now(timezone.utc).isoformat(),
        "run_directory": str(root), "source_result_status": result["status"],
        "repository_head_reported": result.get("repository_head"), "producer_pins_reported": result.get("pins"),
        "requested_seconds": result.get("requested_seconds"), "workload_seconds": result.get("workload_seconds"),
        "target_ms": TARGET_MS, "percentile_method": "nearest rank ceil(n*p/100)-1, matching stem_worker.percentiles; consecutive requests are not independent tail samples",
        "target_scope": "Strict continuous delivered-hop service p99 <4095ms. Individual pass<4095/miss>=4095; producer historical above-target count uses>4095, so equality is listed separately. No empty population passes.",
        "event_coverage": coverage, "service": summaries,
        "event_result_consistency": consistency,
        "graph_phases": {name: phase_summary(populations[name]) for name in ("all_delivered", "continuous")},
        "graph_phase_scope": "CPU-wall upload/compute/readback intervals of accepted delivered requests; compute includes explicit backend wait. Empty/missing fields are null/unavailable, never zero-filled. Comparison timings are excluded.",
        "top_five_continuous_stalls": top,
        "top_five_scope": "Worst five continuous delivered-hop service times; each reports whether it actually misses 4095 ms, even when every request passes.",
        "cancellation": cancellation(result), "playback": result.get("playback"),
        "audio": {"status": audio.get("status"), "signal": audio.get("signal"),
            "callbacks": audio.get("callbacks"), "streaming_seconds": audio.get("streaming_seconds"),
            "frames_rendered": audio.get("frames_rendered"), "negotiated": audio.get("negotiated"),
            "callback_elapsed_us": audio.get("callback_elapsed_us"), "fault_counters": faults,
            "all_six_fault_counters_observed_zero": all(type(value) is int and value == 0 for value in faults.values())
                if all(type(value) is int for value in faults.values()) else None,
            "telemetry_capabilities": audio.get("telemetry_capabilities"), "coverage": result.get("audio_coverage"),
            "scope": audio.get("measurement_scope"),
            "signal_scope": "Independent silent audio probe. No physical latency, complete graph-xrun, stem playback or musical-quality inference."},
        "ui": {key: ui.get(key) for key in ("platform", "graphics_api", "rendered", "capture_ok", "frames_swapped", "frame_observation")},
        "worker_statistics": result.get("worker_statistics"), "source_acceptance": result.get("acceptance"),
        "source_failures": result.get("failures"), "load_errors": load.get("errors"),
        "scope": "Post-run derivation only; no host queries, media reads, producer changes or new qualification gates. A 600-second diagnostic does not become a 60-minute qualification. Failed latency targets remain failed."}
    report["ui"]["coverage"] = result.get("ui_coverage")
    report["ui"]["scope"] = "GUI heartbeat/frameSwapped notifications and sampled exposure; not physical display presentation latency."
    report["thermal"] = thermal_trace(root / "native-trace.jsonl", result, top, pins)
    for label in ("launch", "policy", "guard"):
        path = root.parent / f"{root.name}-{label}.json"
        value = read_json(path, pins, label, optional=True)
        if value is None:
            report["window_guard" if label == "guard" else label] = {"status": "unavailable"}
        elif label == "launch":
            report[label] = {key: value.get(key) for key in ("status", "returncode", "failures", "cleanup", "guard_cleanup", "profile_mutation", "service_mutation")}
        elif label == "policy":
            report[label] = {key: value.get(key) for key in ("mode", "profile_mutation", "restored_by_runner", "before", "during", "before_restore", "after", "runner_returncode", "error")}
        else:
            report["window_guard"] = {key: value.get(key) for key in ("status", "errors", "idle_inhibit_released", "script_unloaded", "restored")}
    report["artifacts_read"] = pins
    encoded = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.output is None:
        print(encoded, end="")
    else:
        destination = args.output.resolve()
        require(str(destination) not in {row["path"] for row in pins.values()}, "refusing to overwrite input evidence")
        with destination.open("x", encoding="utf-8") as stream:
            stream.write(encoded)
        print(json.dumps({"status": "derived", "output": str(destination),
                          "continuous_p99_within_target": summaries["continuous"]["observed_p99_within_target"]}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(f"analysis refused: {error}", file=sys.stderr)
        raise SystemExit(1)

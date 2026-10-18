#!/usr/bin/env python3
"""Closed ABBA request/startup/point-telemetry reduction; never run a workload."""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import statistics

BASE = Path(__file__).resolve().parent
LEGS = ("A1", "B1", "B2", "A2")
CASES = tuple(track + ":257985" for track in ("club_xl", "when_the_light_breaks", "under_my_skin", "stfu"))
FIELDS = ("elapsed_ms", "inference_wall_ms", "inference_cpu_ms", "frontend_ms", "postprocess_ms",
          "graph_ms", "graph_upload_ms", "graph_compute_ms", "graph_readback_ms", "graph_rebuild_ms",
          "voluntary_context_switches", "involuntary_context_switches")
GPU = "/sys/class/drm/card1/gt_act_freq_mhz"
PACKAGE = "/sys/class/thermal/thermal_zone7/temp"
PROFILE = "/sys/firmware/acpi/platform_profile"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path, maximum):
    raw = path.read_bytes()
    require(0 < len(raw) <= maximum, "Artifact size bound: " + str(path))
    return raw, {"path": str(path.resolve()), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def stats(values):
    require(bool(values) and all(type(value) in (int, float) and math.isfinite(value) for value in values), "Invalid statistic")
    return {"count": len(values), "mean": math.fsum(values) / len(values), "median": statistics.median(values),
            "minimum": min(values), "maximum": max(values)}


def effect(a, b):
    return {"A": a, "B": b, "B_minus_A": b - a, "B_relative_change_percent": 100 * (b / a - 1) if a else None}


def context(rows):
    clocks, profiles, temperatures = [], [], []
    raw_errors = []
    fd_values, task_errors = [], []
    for row in rows:
        values = row.get("system_values", {})
        if GPU in values:
            clocks.append(values[GPU])
        if PROFILE in values:
            profiles.append(values[PROFILE])
        if PACKAGE in values:
            temperatures.append(values[PACKAGE])
        if "fd_read_errors" in row:
            fd_values.append(row["fd_read_errors"])
        if "read_errors" in row.get("task_schedstat_ns", {}):
            task_errors.append(row["task_schedstat_ns"]["read_errors"])
        for key in ("error", "system_read_errors", "system_errors"):
            if row.get(key):
                raw_errors.append({"timestamp_monotonic": row["timestamp_monotonic"], key: row[key]})
    return {"rows": len(rows), "actual_GPU_MHz_point_counts": dict(Counter(clocks)),
            "GPU_points": len(clocks), "missing_GPU_rows": len(rows) - len(clocks),
            "profile_point_counts": dict(Counter(profiles)), "profile_points": len(profiles),
            "package_millidegrees_C": stats(temperatures) if temperatures else None,
            "package_points": len(temperatures), "missing_package_rows": len(rows) - len(temperatures),
            "fd_read_error_observations": len(fd_values), "fd_read_error_sum": sum(fd_values) if fd_values else None,
            "task_read_error_observations": len(task_errors), "task_read_error_sum": sum(task_errors) if task_errors else None,
            "raw_error_records": raw_errors}


def analyze(directory):
    raw, session_pin = read(directory / "session.json", 8 * 1024 * 1024)
    session = json.loads(raw)
    require(session.get("status") == "completed" and session.get("completed_requests") == 16,
            "Session is not closed and complete")
    summaries, all_rows, pins = {}, [], {"session": session_pin}
    for leg in LEGS:
        raw, pins[leg + ":result"] = read(directory / leg / "result.json", 4 * 1024 * 1024)
        value = json.loads(raw)
        require(value.get("status") == "passed" and value.get("leg") == leg and value.get("arm") == leg[0]
                and value.get("completed_requests") == 4 and value.get("arm_pcm_proof_match") is True
                and value.get("phase_fields_valid") is True, "Leg is not complete: " + leg)
        requests = value["runtimes"][0]["requests"]
        intervals = value["request_intervals"]
        require(tuple(row["id"] for row in requests) == CASES and len(intervals) == 4, "Case coverage differs")
        raw, pins[leg + ":trace"] = read(directory / leg / "native-trace.jsonl", 16 * 1024 * 1024)
        trace = [json.loads(line) for line in raw.splitlines() if line.strip()]
        observation = value["observation_summary"]
        native_pid, supervisor_pid = observation["native_pid"], observation["supervisor_pid"]
        native = [row for row in trace if row["pid"] == native_pid]
        supervisor = [row for row in trace if row["pid"] == supervisor_pid]
        require(len(native) == observation["native_samples"] and len(supervisor) == observation["supervisor_samples"]
                and len(native) + len(supervisor) == len(trace), "Trace identity/count differs")
        require({row["start_ticks"] for row in native} == set(observation["native_start_ticks"])
                and len(observation["native_start_ticks"]) == 1, "Native identity changed")
        integrity = value["trace_integrity"]
        require(len(trace) == integrity["parsed_rows"] == integrity["samples_written"] == integrity["sampler_samples"],
                "Trace counts disagree")
        require(integrity["trace_errors"] == value["sampler"]["trace"]["errors"]
                and integrity["sample_errors"] == value["sampler"]["sample_errors"], "Trace error summaries disagree")
        for row in trace:
            require(row["capture_started_monotonic"] <= row["timestamp_monotonic"], "Invalid capture interval")
        constructor, ready = value["constructor_begin_monotonic"], value["ready_receive_end_monotonic"]
        observed_startup = (ready - constructor) * 1000
        require(math.isclose(observed_startup, value["startup_total_observed_ms"], abs_tol=1e-7), "Startup arithmetic differs")
        rows = []
        for request, interval in zip(requests, intervals, strict=True):
            begin, end = interval["started_monotonic"], interval["finished_monotonic"]
            require(interval["returned"] is True and interval["request_id"] == request["request_id"]
                    and begin < end and request["output_matches_arm_proof"] is True
                    and request["input_sha_match"] is True, "Unvalidated request")
            contained = [row for row in native if begin <= row["capture_started_monotonic"] and row["timestamp_monotonic"] <= end]
            overlapping = [row for row in native if row["capture_started_monotonic"] < end and row["timestamp_monotonic"] > begin]
            points = [row for row in native if begin <= row["timestamp_monotonic"] <= end]
            row = {"leg": leg, "arm": leg[0], "case": request["id"], "request_id": request["request_id"],
                   "input_sha256": request["input_sha256"], "output_sha256": request["output_sha256"],
                   "request_wall_ms": request["request_wall_ms"],
                   **{field: request["native_metrics"][field] for field in FIELDS},
                   "interval_wall_ms": (end - begin) * 1000,
                   "started_monotonic": begin, "finished_monotonic": end,
                   "native_samples_contained": len(contained), "native_samples_overlapping": len(overlapping),
                   "native_timestamp_points": len(points), "point_context": context(points),
                   "first_point_after_request_start_ms": (points[0]["timestamp_monotonic"] - begin) * 1000 if points else None,
                   "request_end_after_last_point_ms": (end - points[-1]["timestamp_monotonic"]) * 1000 if points else None}
            row["graph_bucket_residual_ms"] = row["graph_ms"] - row["graph_upload_ms"] - row["graph_compute_ms"] - row["graph_readback_ms"]
            row["request_minus_native_wall_ms"] = row["request_wall_ms"] - row["inference_wall_ms"]
            rows.append(row)
        times = [row["timestamp_monotonic"] for row in trace]
        require(times == sorted(times), "Trace timestamps are not ordered")
        summaries[leg] = {"arm": leg[0], "startup_total_observed_ms": observed_startup,
            "constructor_ms": (value["constructor_end_monotonic"] - constructor) * 1000,
            "ready_receive_ms": (ready - value["ready_receive_begin_monotonic"]) * 1000,
            "ready_load_ms": value["runtimes"][0]["ready"]["load_ms"],
            "native_memory_before_close": value["native_memory_before_close"],
            "native_point_context": context(native), "supervisor_point_context": context(supervisor),
            "startup_native_points": sum(constructor <= row["timestamp_monotonic"] <= ready for row in native),
            "first_native_point_after_constructor_ms": (native[0]["timestamp_monotonic"] - constructor) * 1000,
            "last_native_point_after_final_request_ms": (native[-1]["timestamp_monotonic"] - rows[-1]["finished_monotonic"]) * 1000,
            "trace_rows": len(trace), "trace_integrity": integrity,
            "sampler_sample_errors": value["sampler"]["sample_errors"],
            "sampler_trace_errors": value["sampler"]["trace"]["errors"],
            "system_sampling_read_errors": value["sampler"]["system_sampling"]["read_errors"],
            "capture_elapsed_ms": stats([row["capture_elapsed_ms"] for row in trace]),
            "capture_elapsed_sum_ms": math.fsum(row["capture_elapsed_ms"] for row in trace),
            "sample_gap_ms": stats([(b - a) * 1000 for a, b in zip(times, times[1:])]),
            "request_descriptive": {field: stats([row[field] for row in rows]) for field in ("request_wall_ms", *FIELDS)},
            "requests": rows}
        all_rows.extend(rows)
    arms = {arm: [row for row in all_rows if row["arm"] == arm] for arm in ("A", "B")}
    comparison = {}
    for field in ("request_wall_ms", *FIELDS):
        a, b = stats([row[field] for row in arms["A"]]), stats([row[field] for row in arms["B"]])
        comparison[field] = {"A": a, "B": b, "mean_effect": effect(a["mean"], b["mean"]),
                             "median_effect": effect(a["median"], b["median"])}
    pairs = {}
    for case in CASES:
        rows = [row for row in all_rows if row["case"] == case]
        require([row["leg"] for row in rows] == list(LEGS) and len({row["input_sha256"] for row in rows}) == 1,
                "Paired input/leg coverage differs")
        hashes = {arm: {row["output_sha256"] for row in rows if row["arm"] == arm} for arm in ("A", "B")}
        require(all(len(values) == 1 for values in hashes.values()), "Within-arm repeat PCM changed")
        pairs[case] = {"input_sha256": rows[0]["input_sha256"],
            "output_sha256_by_arm": {arm: next(iter(values)) for arm, values in hashes.items()},
            "cross_arm_exact_pcm": hashes["A"] == hashes["B"],
            "mean_effects": {field: effect(statistics.mean(row[field] for row in rows if row["arm"] == "A"),
                                           statistics.mean(row[field] for row in rows if row["arm"] == "B"))
                             for field in ("request_wall_ms", "graph_compute_ms")}}
    return {"schema": 1, "status": "descriptive_analysis_complete", "source_artifacts": pins,
            "session_elapsed_seconds": session.get("elapsed_seconds"), "all_observations_included": True,
            "legs": summaries, "arm_comparison": comparison, "paired_cases": pairs,
            "startup_by_arm": {arm: stats([summaries[leg]["startup_total_observed_ms"] for leg in LEGS if leg[0] == arm])
                               for arm in ("A", "B")},
            "scope": "Sixteen direct native requests, four short legs, same-start windows. All first requests/startups retained. No p99, loaded worker service, audio, cancellation, sustained thermal, musical-equivalence or causal qualification.",
            "telemetry_scope": "GPU/profile/temperature values are sparse read-only point observations. Capture intervals can cross request boundaries; contained/overlapping/timestamp counts are separate. Capture elapsed is observer wall time, not CPU cost or a measured overhead effect. Native startup may be entirely unsampled. Package readings are not the guard's complete sensor set."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--directory", type=Path, default=BASE / "flash-cost-abba-laptop-01")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_plan", "scope": "Read closed JSON/JSONL only; no host/native/model/media"}))
        return 0
    require(args.output is not None, "Fresh --output JSON is required")
    value = analyze(args.directory)
    with args.output.open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"status": value["status"], "analysis": str(args.output),
                      "wall_mean_effect": value["arm_comparison"]["request_wall_ms"]["mean_effect"],
                      "compute_mean_effect": value["arm_comparison"]["graph_compute_ms"]["mean_effect"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

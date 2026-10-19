#!/usr/bin/env python3
"""Describe a CLOSED promoted Flash loaded run from retained JSON only; dry by default."""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
from pathlib import Path

BASE = Path(__file__).resolve().parent
HELPER = BASE / "analyze_result.py"
HELPER_SHA = "44188eb171a5309403380c7a02c3ca96c31e6532923afaf9d5fce870607c9092"
TARGET = 4095.0
FLASH_SHA = "6f4dca329ee158ad4c1abe963cd9d0940d63ba8a309afdc8b4cd1b7259edbc53"
MODEL_SHA = "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"


def require(value, message):
    if not value:
        raise ValueError(message)


def helper():
    raw = HELPER.read_bytes()
    require(len(raw) <= 128 * 1024 and hashlib.sha256(raw).hexdigest() == HELPER_SHA,
            "Frozen closed-result analyzer changed")
    spec = importlib.util.spec_from_file_location("conflux_closed_phase_analysis", HELPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def describe(h, values):
    return {**h.distribution(values), "mean": math.fsum(values) / len(values) if values else None}


def service(h, rows, target):
    values = [row["hop_service_ms"] for row in rows]
    observed = describe(h, values)
    p99 = observed["percentiles"]["99"] if values else None
    return {"chunks": len(rows), "frames": sum(row["frames"] for row in rows), "hop_service_ms": observed,
            "nominal_target_ms": TARGET, "producer_target_ms": target,
            "pass_lt_nominal": sum(value < TARGET for value in values),
            "miss_ge_nominal": sum(value >= TARGET for value in values),
            "equal_nominal": sum(value == TARGET for value in values),
            "producer_hops_above_target_gt": sum(value > target for value in values),
            "producer_equal_target": sum(value == target for value in values),
            "strict_p99_below_nominal": p99 < TARGET if p99 is not None else None,
            "strict_p99_below_producer": p99 < target if p99 is not None else None}


def trace_details(h, root, result, pins):
    rows = list(h.artifact(root / "native-trace.jsonl", 512 * 1024 * 1024, pins, "native_trace_coverage", lines=True))
    require(len(rows) <= 20000, "Trace sample bound exceeded")
    native = result.get("native_memory_cpu", {})
    trace = native.get("trace", {})
    identities = Counter((row["pid"], row["start_ticks"]) for row in rows)
    begin, end = result["workload_started_monotonic"], result["workload_stopped_monotonic"]
    points = [row for row in rows if begin <= row["timestamp_monotonic"] <= end]
    captures = [row["capture_elapsed_ms"] for row in rows]
    clocks = [key for key, unit in native.get("system_sampling", {}).get("fields", {}).items() if unit == "MHz"]
    count_match = len(rows) == native.get("samples") == trace.get("samples_written")
    return {"raw_rows": len(rows), "rows_within_workload": len(points),
            "identities": [{"pid": pid, "start_ticks": start, "samples": count}
                           for (pid, start), count in identities.items()],
            "reported_samples": native.get("samples"), "reported_rows_written": trace.get("samples_written"),
            "counts_match": count_match,
            "sampler_sample_errors": native.get("sample_errors"), "trace_errors": trace.get("errors"),
            "system_sampling_read_errors": native.get("system_sampling", {}).get("read_errors"),
            "fd_read_errors": sum(row.get("fd_read_errors", 0) for row in rows),
            "task_read_errors": sum(row.get("task_schedstat_ns", {}).get("read_errors", 0) for row in rows),
            "fd_error_field_observations": sum("fd_read_errors" in row for row in rows),
            "task_error_field_observations": sum("read_errors" in row.get("task_schedstat_ns", {}) for row in rows),
            "capture_elapsed_ms": describe(h, captures), "capture_elapsed_sum_ms": math.fsum(captures),
            "actual_trace_start_after_workload_start_seconds": rows[0]["timestamp_monotonic"] - begin if rows else None,
            "workload_end_after_last_trace_point_seconds": end - rows[-1]["timestamp_monotonic"] if rows else None,
            "clock_point_counts": {key: dict(Counter(str(row["system_values"][key]) for row in rows
                                                    if key in row.get("system_values", {}))) for key in clocks},
            "clock_missing_rows": {key: sum(key not in row.get("system_values", {}) for row in rows) for key in clocks},
            "startup_scope": "Native PID publication follows ready; this trace cannot observe native pre-ready startup. Relative trace/workload endpoints retain any setup or teardown samples.",
            "observer_scope": "Capture wall time covers sampler reads, not trace writing or a measured overhead effect. Missing optional fields remain unavailable; read failures are retained separately from trace/sample errors."}


def analyze(h, root):
    pins = {}
    launch = h.read_json(root.parent / (root.name + "-launch.json"), pins, "launch")
    require(launch.get("status") in ("completed", "error", "interrupted") and launch.get("finished_at_utc"),
            "Launcher is not closed; do not analyze a live run")
    result = h.read_json(root / "result.json", pins, "result")
    require(result.get("status") in ("ok", "error", "interrupted") and result.get("finished_at_utc")
            and result.get("delivery_mode") == "playback-paced", "Expected closed playback-paced result")
    target = result["timing"]["headroom_target_ms"]
    require(h.number(target) and target == 257985 / 44100 * 700,
            "Producer target formula differs")
    rows, starts, event_coverage = h.read_events(root / "events.jsonl", pins)
    groups = {"all_delivered": rows, "continuous": [row for row in rows if row["kind"] == "continuous"],
              "seek_first": [row for row in rows if row["kind"] == "seek_first"]}
    services = {name: service(h, selected, target) for name, selected in groups.items()}
    consistency = {}
    for name in ("all_delivered", "continuous"):
        recorded, actual = result["timing"][name], services[name]
        consistency[name] = (recorded["chunks"] == actual["chunks"] and recorded["frames"] == actual["frames"]
            and recorded["hop_service_max_ms"] == actual["hop_service_ms"]["max"]
            and recorded["hops_above_target"] == actual["producer_hops_above_target_gt"]
            and recorded["observed_p99_within_target"] == actual["strict_p99_below_producer"]
            and all(recorded["hop_service_percentiles_ms"].get(p) == actual["hop_service_ms"]["percentiles"][p]
                    for p in ("50", "95", "99")))
    require(all(consistency.values()), "Event-derived service disagrees with producer report")
    top = [h.stall(row, starts) for row in sorted(groups["continuous"], key=lambda row: row["hop_service_ms"], reverse=True)[:5]]
    thermal = h.thermal_trace(root / "native-trace.jsonl", result, top, pins)
    load = result.get("load", {})
    audio = load.get("children", {}).get("audio", {}).get("report", {})
    ui = load.get("children", {}).get("ui", {}).get("report", {})
    faults = {key: audio.get(key) for key in h.FAULTS}
    ready = result.get("runtime_ready", [])
    policy_checks = {"exactly_one_native_ready": len(ready) == 1,
                     "runtime_pin_is_Flash": result.get("pins", {}).get("runtime", {}).get("sha256") == FLASH_SHA,
                     "model_pin_matches": result.get("pins", {}).get("model", {}).get("sha256") == MODEL_SHA}
    if len(ready) == 1:
        native = ready[0].get("runtime", {})
        policy_checks.update(vulkan=native.get("backend") == "vulkan", two_threads=native.get("threads") == 2,
            blocking=native.get("fence_wait_mode") == "blocking", native_weight_storage=native.get("weight_storage") == "native",
            passive_process_environment=native.get("process_environment") == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"},
            model_id_matches=ready[0].get("model_id") == MODEL_SHA)
    ancillary = {name: h.read_json(root.parent / (root.name + "-" + name + ".json"), pins, name, optional=True)
                 for name in ("guard", "policy")}
    monitor_path = root.parent / (root.name + "-AC-lid-monitor.jsonl")
    monitor = list(h.artifact(monitor_path, 8 * 1024 * 1024, pins, "AC_lid_monitor", lines=True)) if monitor_path.exists() else []
    phase_fields = (*h.NATIVE_FIELDS,)
    native_metrics = {name: {field: {**describe(h, [row["metrics"][field] for row in selected if field in row["metrics"]]),
                                    "missing_rows": sum(field not in row["metrics"] for row in selected)}
                            for field in phase_fields} for name, selected in groups.items()}
    acceptance = result.get("acceptance", {})
    return {"schema": 1, "status": "closed_descriptive_analysis", "run_directory": str(root),
        "source_result_status": result["status"], "launcher_status": launch["status"],
        "requested_seconds": result.get("requested_seconds"), "workload_seconds": result.get("workload_seconds"),
        "repository_head_reported": result.get("repository_head"), "producer_pins_reported": result.get("pins"),
        "target": {"nominal_ms": TARGET, "producer_formula_ms": target, "producer_float_hex": float(target).hex(),
                   "operator": "continuous service p99 strictly below target", "all_observations_retained": True,
                   "percentile_method": "nearest rank ceil(n*p/100)-1; consecutive windows are not independent tail samples"},
        "event_coverage": event_coverage, "event_result_consistency": consistency,
        "service": services, "native_metric_distributions": native_metrics,
        "graph_phases": {name: h.phase_summary(selected) for name, selected in groups.items()},
        "phase_scope": "Accepted delivered native request CPU-wall buckets; compute includes backend wait. Preroll/cancelled/final unresolved requests lack full retained metrics and are counted separately, never filled or treated as zero. No GPU timestamps or causal diagnosis.",
        "accepted_hops": rows, "top_five_continuous_stalls": top,
        "cancellation": h.cancellation(result),
        "parity": {"same_grid_hash_checks": result.get("same_grid_hash_checks"),
                   "trimmed_seek_checks": result.get("trimmed_seek_checks"),
                   "seek_first_rows": [{key: row.get(key) for key in ("generation", "request_id", "source_id", "frame_start", "frames", "parity")} for row in groups["seek_first"]],
                   "stale_chunks_delivered": result.get("stale_chunks_delivered"),
                   "failure": result.get("parity_failure"), "scope": acceptance.get("parity_scope")},
        "playback": result.get("playback"),
        "worker_statistics_before_shutdown": result.get("worker_statistics_before_shutdown"),
        "worker_statistics": result.get("worker_statistics"), "worker_shutdown_ms": result.get("worker_shutdown_ms"),
        "runtime_ready": ready, "runtime_policy_checks": policy_checks, "all_runtime_policy_checks": all(policy_checks.values()),
        "audio": {"fault_counters": faults,
                  "all_six_fault_counters_observed_zero": all(type(value) is int and value == 0 for value in faults.values()) if all(type(value) is int for value in faults.values()) else None,
                  "report": audio, "coverage": result.get("audio_coverage"),
                  "scope": "Independent silent48k/stereo/256-frame probe. No actual stem playback, physical latency, DAC quality or complete graph-xrun inference."},
        "ui": {"report": ui, "coverage": result.get("ui_coverage"),
               "scope": "Software Harness update/heartbeat, sampled exposure and frameSwapped gaps; not physical display timing or integrated stem-player qualification."},
        "thermal": thermal, "native_trace_coverage": trace_details(h, root, result, pins),
        "source_acceptance": acceptance, "source_failures": result.get("failures"), "load_errors": load.get("errors"),
        "launch": {key: launch.get(key) for key in ("status", "returncode", "failures", "latency_gate", "cleanup", "guard_cleanup", "owned_group_cleanup", "comparison", "execution_provenance", "proof_source_pins_before", "proof_source_pins_after", "human_readiness_before_workload")},
        "ancillary": ancillary, "AC_lid_monitor": monitor, "artifacts_read": pins,
        "integrity_scope": "Retained metadata and its hashes only; separate independent integrity audit owns source/proof/readiness/cleanup revalidation. No live host or executable/model/audio reads.",
        "scope": f"Closed {result.get('requested_seconds')}-second independent-load diagnostic, all accepted rows and startup/control observations retained. Duration completion, strict continuous-hop target outcome and producer audio/UI soak flag are separate recorded facts. Independent silent audio/software Harness do not establish integrated stem playback, physical audio or complete M2 qualification. No new musical verdict or hardware-cancellation claim."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_directory", type=Path)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_plan", "scope": "Closed JSON/events/trace only; no live run, host, model/media or binary access"}))
        return 0
    require(args.output is not None, "Fresh output path required")
    value = analyze(helper(), args.run_directory.resolve())
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"status": value["status"], "output": str(args.output),
                      "continuous": value["service"]["continuous"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Describe closed music-soak misses and sparse host samples; no host queries."""
import argparse
import bisect
from collections import Counter
from datetime import datetime, timezone
import gzip
import hashlib
import importlib.util
import json
import math
from pathlib import Path

BASE = Path(__file__).resolve().parent
PROFILE_HELPER = BASE.parent / "music-profile-2026-10-17/profile_music.py"
SPAN_HELPER = BASE.parent / "user-tracks-2026-10-17/summarize_runs.py"
PROFILE_SHA = "9e2d3c5eedd6c7f00714619d45de51aee69713a4a4971d8779452979af74b6b6"
SPAN_SHA = "f704971b6a70862126de8f1d91498630dd7592d9d4ba63139ad8c49ef0e18fe5"
TARGET = 4095.0


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def strict_json(data):
    def reject(value):
        raise ValueError("Nonfinite JSON value: " + value)
    return json.loads(data, parse_constant=reject)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", help="Completed trial label")
    parser.add_argument("--base", type=Path, default=BASE)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--profile-helper", type=Path, default=PROFILE_HELPER)
    parser.add_argument("--span-helper", type=Path, default=SPAN_HELPER)
    args = parser.parse_args()
    require(Path(args.name).name == args.name and args.name not in (".", ".."), "Use a plain run label")
    base, run = args.base.resolve(), args.base.resolve() / args.name
    output = args.output or base / (args.name + "-analysis.json")
    require(not output.exists(), "Analysis output must be fresh")
    inputs = {}

    def read(path, lines=False):
        if lines and not path.exists():
            path = path.with_name(path.name + ".gz")
        data = path.read_bytes()
        value = {"bytes": len(data), "sha256": digest(data)}
        raw = gzip.decompress(data) if path.suffix == ".gz" else data
        if lines:
            value.update(decoded_bytes=len(raw), decoded_sha256=digest(raw))
        inputs[str(path)] = value
        return [strict_json(line) for line in raw.splitlines() if line.strip()] if lines else strict_json(raw)

    def dependency(path, wanted, name):
        data = path.read_bytes()
        require(digest(data) == wanted, "Frozen reducer source changed: " + name)
        inputs[str(path)] = {"bytes": len(data), "sha256": wanted}
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    # Only small final lifecycle records are read until closure is established.
    result = read(run / "result.json")
    launch = read(base / (args.name + "-launch.json"))
    guard = read(base / (args.name + "-guard.json"))
    policy = read(base / (args.name + "-policy.json"))
    worker = result.get("worker_statistics", {})
    require(result.get("finished_at_utc") and result.get("status") == "ok" and not result.get("failures")
            and launch.get("finished_at_utc") and launch.get("status") == "completed"
            and launch.get("returncode") == 0 and not launch.get("failures")
            and not launch["cleanup"]["remaining_owned_processes"] and launch["cleanup"]["last_child_reaped"]
            and worker.get("closed") and worker.get("backend_pid") is None and worker.get("backend_start_count") == 1
            and guard.get("status") == "ok" and not guard.get("errors") and guard.get("idle_inhibit_released")
            and guard.get("script_unloaded") and guard.get("restored")
            and policy.get("mode") == "observe" and policy.get("profile_mutation") is False
            and policy.get("benchmark_returncode") == policy.get("runner_returncode") == 0,
            "Refusing analysis before successful workload, native, observer and owned guard closure")
    audit = read(base / (args.name + "-audit.json"))
    require(audit.get("status") == "valid" and not audit.get("failed_checks"), "Saved structural audit is not valid")
    require(any(value.get("sha256") == inputs[str(run / "result.json")]["sha256"]
                for value in audit["inputs"].values()), "Structural audit is not bound to this result")
    pm = dependency(args.profile_helper, PROFILE_SHA, "closed_music_profile_reducers")
    spans = dependency(args.span_helper, SPAN_SHA, "closed_music_span_reducers").spans
    corpus = read(base / "corpus-pins.json")
    require(any(Path(path).name == "corpus-pins.json" and value.get("sha256")
                == inputs[str(base / "corpus-pins.json")]["sha256"]
                for path, value in audit["inputs"].items()), "Structural audit is not bound to this corpus metadata")
    tracks = {track["input"]["sha256"]: track for track in corpus["tracks"]}
    events = read(run / "events.jsonl", lines=True)
    event_input = next(value for path, value in inputs.items() if Path(path).name in ("events.jsonl", "events.jsonl.gz"))
    require(any(value.get("decoded_sha256", value.get("sha256")) == event_input["decoded_sha256"]
                for path, value in audit["inputs"].items() if Path(path).name in ("events.jsonl", "events.jsonl.gz")),
            "Structural audit is not bound to these raw events")
    trace = read(run / "native-trace.jsonl", lines=True)
    sampling = result["native_memory_cpu"]
    require(trace and len(trace) == sampling["samples"] == sampling["trace"]["samples_written"]
            and not sampling["sample_errors"] and not sampling["trace"]["errors"], "Native trace count/errors differ")
    times = [row["timestamp_monotonic"] for row in trace]
    captures = [row["capture_started_monotonic"] for row in trace]
    require(all(a < b for a, b in zip(times, times[1:])) and captures == sorted(captures), "Native clock order differs")
    require(all(start <= end and math.isclose((end - start) * 1000, row["capture_elapsed_ms"], abs_tol=1e-6)
                for start, end, row in zip(captures, times, trace)), "Native capture brackets differ")
    require(len({(row["pid"], row["start_ticks"]) for row in trace}) == len(sampling["processes"]) == 1,
            "Native process identity changed")
    native = sampling["processes"][0]
    require((trace[0]["pid"], trace[0]["start_ticks"]) == (native["pid"], native["start_ticks"])
            and len(result["runtime_ready"]) == 1 and native["pid"] == result["runtime_ready"][0]["runtime"]["pid"]
            and {key: max(row["process_status_kB"][key] for row in trace) for key in ("VmHWM", "VmRSS", "VmSwap")}
                == native["max_process_status_kB"], "Saved native memory maxima differ from raw trace")
    begin, end = result["workload_started_monotonic"], result["workload_stopped_monotonic"]
    fields_saved = sampling["system_sampling"]["fields"]

    def unique_field(suffix):
        found = [path for path in fields_saved if path.endswith(suffix)]
        require(len(found) == 1, "Missing/ambiguous saved field: " + suffix)
        return {"path": found[0], "unit": fields_saved[found[0]]}

    fields = {"gpu_active_frequency": unique_field("/gt_act_freq_mhz"),
              "gpu_requested_frequency": unique_field("/gt_cur_freq_mhz"),
              "cpu0_current_frequency": unique_field("/cpu0/cpufreq/scaling_cur_freq")}
    for path, unit in fields_saved.items():
        if unit == "millidegrees_C" and path.endswith("/temp"):
            zone = str(Path(path).parent)
            labels = {result[key][zone]["type"] for key in ("conditions_at_start", "conditions_at_finish")}
            require(len(labels) == 1, "Saved thermal type mapping changed")
            label = labels.pop()
            require(label not in fields, "Ambiguous saved sensor label")
            fields[label] = {"path": path, "unit": unit}
    starts = {(event["generation"], event["request_id"]): event for event in events if event["event"] == "started"}
    chunks = {(event["generation"], event["request_id"]): event for event in events if event["event"] == "chunk"}
    raw_hops = [event for event in events if event["event"] == "soak_hop"]
    controls = {event["generation"]: event for event in events if event["event"] == "control"}
    prepared = {event["generation"]: event for event in events if event["event"] == "source_ready"}
    require(len(chunks) == len(raw_hops), "Delivered chunk/hop count differs")
    reason_paths = [path for path in fields_saved if "throttle_reason_" in path]
    hops, first_by_generation = [], {}
    for index, hop in enumerate(raw_hops):
        key = hop["generation"], hop["request_id"]
        chunk, request = chunks[key], starts[key]
        require(all(hop[name] == chunk[name] for name in ("source_id", "frame_start", "frames", "window_start_frame",
                    "ready_at", "hop_service_ms", "queue_delay_ms", "metrics")), "Hop/raw chunk identity differs")
        finish, service, graph = hop["ready_at"], hop["hop_service_ms"], hop["metrics"]["graph_ms"]
        start = finish - service / 1000
        command = controls[hop["generation"]]
        require(command["timestamp"] <= request["timestamp"] <= finish <= chunk["consumed_at"]
                and start <= request["timestamp"] and not request["preroll"]
                and request["source_id"] == hop["source_id"] and hop["metrics"]["id"] == hop["request_id"],
                "Delivered request/control identity or timestamp order differs")
        assigned = list(range(bisect.bisect_left(times, start), bisect.bisect_right(times, finish)))
        interior_ids = [i for i in assigned if captures[i] >= start]
        interior = [trace[i] for i in interior_ids]
        previous = bisect.bisect_left(times, start) - 1
        following = bisect.bisect_right(captures, finish)
        sid = hop["source_id"]
        row = {"index_all_delivered": index, "track_id": tracks[sid]["id"], "source_id": sid,
               **{name: hop[name] for name in ("kind", "generation", "request_id", "frame_start", "frames", "window_start_frame", "elapsed_seconds")},
               "window_source_seconds": hop["window_start_frame"] / 44100,
               "hop_service_ms": service, "native_graph_ms": graph, "graph_fraction_of_hop": graph / service,
               "hop_minus_graph_ms": service - graph, "queue_delay_ms": hop["queue_delay_ms"],
               "ready_to_raw_consumer_ms": (chunk["consumed_at"] - finish) * 1000,
               "request_ms": hop["request_ms"], "source_read_ms": hop["source_read_ms"], "overlap_ms": hop["overlap_ms"],
               "native_metrics": hop["metrics"], "target_miss": service > TARGET,
               "service_interval": {"start_monotonic": start, "end_monotonic": finish},
               "alignment": {"fully_contained_trace_row_indices": interior_ids, "timestamp_assigned_trace_row_indices": assigned,
                   "previous_complete_capture_index": previous if previous >= 0 else None,
                   "next_capture_index": following if following < len(trace) else None},
               "policy_samples": dict(Counter(pm.policy(sample) for sample in interior)),
               "fully_contained_sample_statistics": pm.field_stats(interior, fields),
               "timestamp_assigned_sample_statistics": pm.field_stats([trace[i] for i in assigned], fields),
               "throttle_reason_samples": {path: dict(Counter(str(sample["system_values"][path]) for sample in interior
                    if path in sample["system_values"])) for path in reason_paths}}
        hops.append(row)
        first_by_generation.setdefault(hop["generation"], (hop, chunk, raw_hops[index - 1] if index else None))

    def population(selected):
        ids = [i for hop in selected for i in hop["alignment"]["fully_contained_trace_row_indices"]]
        return {"hops": len(selected), "misses_gt_4095ms": sum(hop["target_miss"] for hop in selected),
                "timing_ms": {name: pm.stats([hop[name] for hop in selected]) for name in
                    ("hop_service_ms", "native_graph_ms", "hop_minus_graph_ms", "queue_delay_ms", "ready_to_raw_consumer_ms")},
                "graph_fraction_of_hop": pm.stats([hop["graph_fraction_of_hop"] for hop in selected]),
                "hops_without_fully_contained_samples": sum(not hop["alignment"]["fully_contained_trace_row_indices"] for hop in selected),
                "per_hop_mean_sample_statistics": {label: pm.stats([hop["fully_contained_sample_statistics"][label]["mean"]
                    for hop in selected if hop["fully_contained_sample_statistics"][label]["count"]]) for label in fields},
                "pooled_interior_sample_statistics": pm.field_stats([trace[i] for i in ids], fields),
                "interior_snapshots": len(ids), "unique_interior_snapshots": len(set(ids)),
                "policy_classes_per_hop": dict(Counter(json.dumps(sorted(hop["policy_samples"])) for hop in selected))}

    continuous = [hop for hop in hops if hop["kind"] == "continuous"]
    def clusters(selected):
        groups, current = [], []
        for hop in selected + [None]:
            if hop is not None and hop["target_miss"]:
                current.append(hop)
            elif current:
                groups.append({"count": len(current), "first_elapsed_seconds": current[0]["elapsed_seconds"],
                    "last_elapsed_seconds": current[-1]["elapsed_seconds"], "sources": sorted({row["track_id"] for row in current}),
                    "request_ids": [row["request_id"] for row in current],
                    "worst": max(current, key=lambda row: row["hop_service_ms"]), "summary": population(current)})
                current = []
        return groups

    first_output = []
    tracks_by_path = {track["input"]["path"]: track for track in tracks.values()}
    for generation, command in controls.items():
        if command["command"] == "cancel":
            continue
        first = first_by_generation.get(generation)
        ready = prepared.get(generation)
        track = tracks_by_path[command["source_path"]]
        first_output.append({"generation": generation, "command": command["command"], "source_path": command["source_path"],
            "track_id": track["id"], "source_id": track["input"]["sha256"],
            "target_frame": command["frame_start"], "control_monotonic": command["timestamp"],
            "source_ready_monotonic": ready["timestamp"] if ready else None, "preparation_ms": ready["preparation_ms"] if ready else None,
            "first_hop_service_ms": first[0]["hop_service_ms"] if first else None,
            "control_to_first_ready_ms": (first[0]["ready_at"] - command["timestamp"]) * 1000 if first else None,
            "control_to_raw_consumed_ms": (first[1]["consumed_at"] - command["timestamp"]) * 1000 if first else None,
            "ready_gap_from_previous_delivered_ms": (first[0]["ready_at"] - first[2]["ready_at"]) * 1000 if first and first[2] else None,
            "previous_delivered_source_id": first[2]["source_id"] if first and first[2] else None,
            "complete_first_output_observed": first is not None})
    phases = []
    for index, sample in enumerate(trace):
        value = json.loads(pm.policy(sample))
        if not phases or phases[-1]["policy"] != value:
            phases.append({"policy": value, "first_trace_row": index, "first_workload_offset_seconds": times[index] - begin, "samples": 0})
        phases[-1].update(last_trace_row=index, last_workload_offset_seconds=times[index] - begin)
        phases[-1]["samples"] += 1
    thermal = {}
    for label, field in fields.items():
        if field["unit"] == "millidegrees_C":
            points = [(times[i] - begin, sample["system_values"][field["path"]] / 1000)
                      for i, sample in enumerate(trace) if field["path"] in sample["system_values"]]
            thermal[label] = {"path": field["path"], "samples": len(points), "statistics_C": pm.stats([point[1] for point in points]),
                "thresholds_descriptive_only": {str(threshold): {"hot_samples": sum(point[1] >= threshold for point in points),
                    "episodes": spans(points, lambda point, t=threshold: point[1] >= t)} for threshold in (85, 95)}}
    bins = [{"start_seconds": interval["start_seconds"], "end_seconds": interval["end_seconds"],
             "continuous": population([row for row in continuous if interval["start_seconds"] <= row["elapsed_seconds"] < interval["end_seconds"]]),
             "all_delivered": population([row for row in hops if interval["start_seconds"] <= row["elapsed_seconds"] < interval["end_seconds"]])}
            for interval in result["timing"]["bins_300_seconds"]]
    for name, selected in (("continuous", continuous), ("all_delivered", hops)):
        actual = pm.stats([hop["hop_service_ms"] for hop in selected])
        saved = result["timing"][name]
        require(actual["count"] == saved["chunks"] and actual["max"] == saved["hop_service_max_ms"]
                and all(actual["p" + q] == saved["hop_service_percentiles_ms"][q] for q in ("50", "95", "99")),
                "Saved timing reduction differs")
    report = {"schema": 1, "status": "ok", "run": args.name, "created_at_utc": datetime.now(timezone.utc).isoformat(),
        "repository_head": result["repository_head"], "captured_producer_pins": result["pins"], "target_ms": TARGET,
        "requested_seconds": result["requested_seconds"], "workload_seconds": result["workload_seconds"], "field_paths": fields,
        "populations": {"continuous": population(continuous), "all_delivered": population(hops),
            "continuous_misses": population([row for row in continuous if row["target_miss"]]),
            "continuous_nonmisses": population([row for row in continuous if not row["target_miss"]])},
        "per_track": [{"track_id": track["id"], "source_id": sid, "input": track["input"],
            "continuous": population([row for row in continuous if row["source_id"] == sid]),
            "all_delivered": population([row for row in hops if row["source_id"] == sid])} for sid, track in tracks.items()],
        "bins_300_seconds": bins, "continuous_miss_clusters": clusters(continuous), "all_delivered_miss_clusters": clusters(hops),
        "worst_ten_continuous_hops": sorted(continuous, key=lambda row: row["hop_service_ms"], reverse=True)[:10],
        "target_miss_hops": [row for row in hops if row["target_miss"]], "hops": hops,
        "source_admission_first_output": first_output, "prelaunch_admission": launch["admission"],
        "launch_phase_offsets_seconds": {key: launch[key] - launch["started_monotonic"] for key in
            ("checks_and_imports_finished_monotonic", "child_spawned_monotonic") if key in launch},
        "native_sampling": {"all_retained_samples": len(trace), "before_workload": sum(t < begin for t in times),
            "inside_workload": sum(begin <= t <= end for t in times), "after_workload": sum(t > end for t in times),
            "maximum_sample_gap_seconds": max((b - a for a, b in zip(times, times[1:])), default=None),
            "capture_elapsed_ms": pm.stats([row["capture_elapsed_ms"] for row in trace]),
            "policy_counts": dict(Counter(pm.policy(row) for row in trace)), "policy_runs": phases,
            "all_retained_field_statistics": pm.field_stats(trace, fields), "thermal": thermal,
            "process": native, "supervisor": result["supervisor_memory_cpu"],
            "system_read_errors": sampling["system_sampling"]["read_errors"],
            "energy_fields": {path: {"unit": unit, "statistics": pm.stats([row["system_values"][path] for row in trace
                if path in row["system_values"]])} for path, unit in fields_saved.items() if "energy_uj" in path},
            "sampled_throttle_fields": {path: {"unit": unit, "observed_values": dict(Counter(str(row["system_values"][path])
                for row in trace if path in row["system_values"]))} for path, unit in fields_saved.items()
                if "throttle_reason_" in path or "/thermal_throttle/" in path}},
        "inputs": inputs, "analysis_source_sha256": digest(Path(__file__).read_bytes()),
        "limits": ["Saved completed evidence only; no host command, journal query, inference, new workload or PCM replay.",
            "Graph duration includes native execution/wait. Hop service includes source read/request/overlap but excludes source hashing/statistics preparation and later ready-queue delay. Hop-minus-graph is a duration difference, not isolated CPU or transport time.",
            "Primary samples require their entire capture bracket inside the hop; timestamp-only sensitivity assignments are also retained. Native graph absolute endpoints and individual field-read times are unavailable.",
            "Sparse roughly0.5s samples give unweighted sample statistics, not continuous/effective clocks, time-in-state or causal explanation. Longer hops contribute more pooled samples; per-hop means are separate.",
            "Clusters mean adjacent misses in the selected delivered-hop population. They do not prove continuous lateness or uninterrupted host states between observations; continuous clusters omit seek-first replacement rows.",
            "All first hops, tails, startup and control samples remain. Per-track and bin p99 are nearest-rank statistics from dependent repeated excerpts, with coarse small-population tails.",
            "85/95C summaries are descriptive thresholds, not added acceptance gates. Sampled hot spans and neighboring cool brackets cannot establish continuous hot duration or identify an unobserved triggering sensor; other sensors are start/finish snapshots only.",
            "Native and supervisor /proc memory values are separate process peaks. DRM client maxima retain exact names/units and are not added to RSS or one another. Denied energy fields are unavailable, not zero energy/power.",
            "Per-generation control-to-first-ready/preparation and prelaunch admission are reported separately from hop-service latency. Natural policy/thermal/cache/order/background changes confound causal conclusions.",
            "The separate structural audit retains cancellation/audio/UI evidence. This descriptive helper adds no musical, physical-audio, hardware-preemption or production-playback acceptance claim."]}
    with output.open("x") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"output": str(output), "continuous": report["populations"]["continuous"],
                      "miss_clusters": len(report["continuous_miss_clusters"])}))


if __name__ == "__main__":
    main()

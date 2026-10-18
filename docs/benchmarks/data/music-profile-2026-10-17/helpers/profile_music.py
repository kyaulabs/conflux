#!/usr/bin/env python3
"""Describe completed Vulkan music hop/sample associations; never infer causes."""
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import statistics

BASE = Path(__file__).resolve().parent
SOURCE = BASE.parent / "user-tracks-2026-10-17"
PROFILE = "/sys/firmware/acpi/platform_profile"
EPP = "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"
TARGET = 4095.0


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest}


def load(path):
    def reject(value):
        raise ValueError("Nonfinite JSON: " + value)
    return json.loads(path.read_text(), parse_constant=reject)


def lines(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def stats(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    assert all(isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) for v in values)
    return {"count": len(values), "min": values[0], "mean": statistics.mean(values), "median": statistics.median(values),
            **{f"p{p}": values[math.ceil(len(values) * p / 100) - 1] for p in (50, 95, 99)}, "max": values[-1]}


def policy(row):
    v = row["system_values"]
    return json.dumps({"profile": v.get(PROFILE), "epp": v.get(EPP)}, sort_keys=True)


def field_stats(rows, fields):
    result = {}
    for label, description in fields.items():
        path, unit = description["path"], description["unit"]
        scale = 0.001 if unit == "millidegrees_C" else 1.0
        values = [r["system_values"][path] * scale for r in rows if path in r["system_values"]]
        result[label] = {**stats(values), "unit": "C" if unit == "millidegrees_C" else unit,
                         "missing_samples": len(rows) - len(values)}
    return result


def main():
    output = BASE / "vulkan-hop-profile.json"
    assert not output.exists(), "Output must be fresh"
    paths = {name: SOURCE / relative for name, relative in {
        "result": "vulkan/result.json", "events": "vulkan/events.jsonl", "trace": "vulkan/native-trace.jsonl",
        "sequence": "sequence.json", "guard": "series-guard.json", "run_facts": "run-facts.json"}.items()}
    result, sequence, guard, facts = (load(paths[k]) for k in ("result", "sequence", "guard", "run_facts"))
    assert result["status"] == "ok" and not result["failures"] and result["finished_at_utc"]
    assert result["backend"] == "vulkan" and result["worker_statistics"]["closed"]
    assert result["worker_statistics"]["backend_start_count"] == 1 and result["worker_statistics"]["backend_pid"] is None
    assert sequence["status"] == "completed" and sequence["completed_backends"] == ["cpu", "vulkan"]
    assert guard["status"] == "ok" and guard["idle_inhibit_released"] and guard["returncode"] == guard["child_returncode"] == 0
    assert facts["backends"]["vulkan"]["inputs"] == [pin(paths[k]) for k in ("result", "events", "trace")]
    events, trace = lines(paths["events"]), lines(paths["trace"])
    assert len(trace) == result["native_memory_cpu"]["trace"]["samples_written"] == 144
    assert all(a["timestamp_monotonic"] < b["timestamp_monotonic"] for a, b in zip(trace, trace[1:]))
    for row in trace:
        assert row["capture_started_monotonic"] <= row["timestamp_monotonic"]
        assert abs((row["timestamp_monotonic"] - row["capture_started_monotonic"]) * 1000 - row["capture_elapsed_ms"]) < 1e-6
    origins = [r["timestamp_monotonic"] - r["elapsed_seconds"] for r in trace]
    assert max(origins) - min(origins) < 1e-6
    starts = {e["request_id"]: e for e in events if e["event"] == "started"}
    assert len(starts) == sum(e["event"] == "started" for e in events)
    continuous = [e for e in events if e["event"] == "chunk" and e["scenario"] == "continuous"]
    assert len(continuous) == 16
    sampled_fields = result["native_memory_cpu"]["system_sampling"]["fields"]
    def unique_field(suffix):
        found = [p for p in sampled_fields if p.endswith(suffix)]
        assert len(found) == 1
        return {"path": found[0], "unit": sampled_fields[found[0]]}
    fields = {"gpu_active_frequency": unique_field("/gt_act_freq_mhz"),
              "gpu_requested_frequency": unique_field("/gt_cur_freq_mhz"),
              "cpu0_current_frequency": unique_field("/cpu0/cpufreq/scaling_cur_freq")}
    for path, unit in sampled_fields.items():
        if unit == "millidegrees_C" and path.endswith("/temp"):
            zone = str(Path(path).parent)
            labels = {result[k][zone]["type"] for k in ("conditions_at_start", "conditions_at_finish")}
            assert len(labels) == 1
            label = labels.pop()
            assert label in ("x86_pkg_temp", "TCPU")
            fields[label] = {"path": path, "unit": unit}
    assert "x86_pkg_temp" in fields and "TCPU" in fields
    reason_paths = [p for p in sampled_fields if "/gt/" in p and "/throttle_reason_" in p]
    first_load = min(e["timestamp"] for e in events if e["event"] == "control")
    hops = []
    for event in continuous:
        end = event["ready_at"]
        start = end - event["hop_service_ms"] / 1000
        request = starts[event["request_id"]]
        assert request["generation"] == event["generation"] and request["track_id"] == event["track_id"]
        assert not request["preroll"] and start <= request["timestamp"] <= end
        assert event["metrics"]["id"] == event["request_id"]
        # Fully contained capture intervals avoid assigning a field read from
        # outside the hop solely by its later aggregate trace timestamp.
        interior_ids = [i for i, r in enumerate(trace) if start <= r["capture_started_monotonic"] and r["timestamp_monotonic"] <= end]
        assigned_ids = [i for i, r in enumerate(trace) if start <= r["timestamp_monotonic"] <= end]
        overlaps = [i for i, r in enumerate(trace) if r["capture_started_monotonic"] <= end and r["timestamp_monotonic"] >= start]
        interior = [trace[i] for i in interior_ids]
        assert interior
        previous = [i for i, r in enumerate(trace) if r["timestamp_monotonic"] < start]
        following = [i for i, r in enumerate(trace) if r["capture_started_monotonic"] > end]
        policies = Counter(policy(r) for r in interior)
        graph = event["metrics"]["graph_ms"]
        hops.append({"track_id": event["track_id"], "generation": event["generation"], "request_id": event["request_id"],
            "frame_start": event["frame_start"], "source_seconds": event["frame_start"] / 44100,
            "target_miss": event["hop_service_ms"] > TARGET, "hop_service_ms": event["hop_service_ms"],
            "native_graph_ms": graph, "native_graph_exceeds_target": graph > TARGET,
            "hop_minus_graph_ms": event["hop_service_ms"] - graph, "graph_fraction_of_hop": graph / event["hop_service_ms"],
            "request_ms": event["request_ms"], "source_read_ms": event["source_read_ms"], "overlap_ms": event["overlap_ms"],
            "queue_delay_ms": event["queue_delay_ms"], "ready_to_raw_consumer_ms": (event["consumed_at"] - end) * 1000,
            "native_metrics": event["metrics"],
            "service_interval": {"start_monotonic": start, "end_monotonic": end,
                "start_seconds_after_first_load": start - first_load, "end_seconds_after_first_load": end - first_load,
                "request_started_event_monotonic": request["timestamp"]},
            "alignment": {"fully_contained_trace_row_indices": interior_ids,
                "timestamp_assigned_trace_row_indices": assigned_ids,
                "capture_intervals_crossing_boundary_indices": sorted(set(overlaps) - set(interior_ids)),
                "previous_complete_capture_index": previous[-1] if previous else None,
                "next_capture_index": following[0] if following else None,
                "previous_capture_end_to_hop_start_seconds": start - trace[previous[-1]]["timestamp_monotonic"] if previous else None,
                "hop_end_to_next_capture_start_seconds": trace[following[0]]["capture_started_monotonic"] - end if following else None,
                "start_to_first_interior_capture_seconds": interior[0]["capture_started_monotonic"] - start,
                "last_interior_capture_to_end_seconds": end - interior[-1]["timestamp_monotonic"],
                "interior_max_sample_gap_seconds": max((b["timestamp_monotonic"] - a["timestamp_monotonic"] for a, b in zip(interior, interior[1:])), default=None)},
            "policy_samples": dict(policies), "fully_contained_sample_statistics": field_stats(interior, fields),
            "timestamp_assigned_sample_statistics": field_stats([trace[i] for i in assigned_ids], fields),
            "gpu_throttle_reason_observation_counts": {p: dict(Counter(str(r["system_values"][p]) for r in interior if p in r["system_values"])) for p in reason_paths}})
    def population(selected):
        ids = [i for h in selected for i in h["alignment"]["fully_contained_trace_row_indices"]]
        return {"hops": len(selected), "requests": [h["request_id"] for h in selected],
            "timing_ms": {key: stats([h[key] for h in selected]) for key in ("hop_service_ms", "native_graph_ms", "hop_minus_graph_ms", "queue_delay_ms", "ready_to_raw_consumer_ms")},
            "per_hop_mean_sample_statistics": {label: {**stats([h["fully_contained_sample_statistics"][label]["mean"] for h in selected]),
                "unit": selected[0]["fully_contained_sample_statistics"][label]["unit"]} for label in fields} if selected else {},
            "interior_snapshots": len(ids), "unique_interior_snapshots": len(set(ids)),
            "pooled_interior_sample_statistics": field_stats([trace[i] for i in ids], fields),
            "policy_classes_per_hop": dict(Counter(json.dumps(sorted(h["policy_samples"])) for h in selected))}
    phase_runs = []
    for index, row in enumerate(trace):
        key = policy(row)
        if not phase_runs or phase_runs[-1]["policy"] != json.loads(key):
            phase_runs.append({"policy": json.loads(key), "first_trace_row": index, "last_trace_row": index,
                "first_seconds_after_load": row["timestamp_monotonic"] - first_load,
                "last_seconds_after_load": row["timestamp_monotonic"] - first_load, "samples": 0})
        phase_runs[-1].update(last_trace_row=index, last_seconds_after_load=row["timestamp_monotonic"] - first_load)
        phase_runs[-1]["samples"] += 1
    profile = {"schema": 1, "created_at_utc": datetime.now(timezone.utc).isoformat(), "status": "ok",
        "analysis_source": pin(Path(__file__).resolve()), "inputs": {k: pin(p) for k, p in paths.items()},
        "source_head": sequence["source_head"], "captured_producer_pins": result["pins"], "target_ms": TARGET,
        "field_paths": fields, "clock_and_alignment": {
            "event_clock": "Worker time.monotonic in same host/session as sampler time.monotonic; no wall-clock alignment or offset fitted.",
            "service_interval": "[ready_at-hop_service_ms/1000, ready_at], from pinned worker service_started/ready. Includes source read, request and overlap; excludes subsequent ready-queue waiting and cold load/preparation.",
            "sample_clock": result["native_memory_cpu"]["trace"]["timestamp_clock"],
            "capture_interval": "[capture_started_monotonic,timestamp_monotonic] brackets all process/system reads. Individual field read times are not recorded.",
            "primary_interior_rule": "Require full capture interval inside inclusive hop interval. Timestamp-only assignment retained separately as sensitivity data. No interpolation or inferred time-in-state.",
            "native_graph_boundary": "graph_ms is a native duration, including execution/wait; graph-specific absolute endpoints are not recorded, so snapshots align to the broader worker hop interval.",
            "trace_origin_monotonic_min": min(origins), "trace_origin_monotonic_max": max(origins),
            "first_load_monotonic": first_load, "first_trace_seconds_after_load": trace[0]["timestamp_monotonic"] - first_load,
            "last_trace_seconds_after_load": trace[-1]["timestamp_monotonic"] - first_load,
            "max_trace_gap_seconds": max(b["timestamp_monotonic"] - a["timestamp_monotonic"] for a, b in zip(trace, trace[1:])),
            "capture_elapsed_ms": stats([r["capture_elapsed_ms"] for r in trace])},
        "all_trace_policy_samples": dict(Counter(policy(r) for r in trace)), "all_trace_policy_runs": phase_runs,
        "populations": {"continuous_all": population(hops), "continuous_target_misses": population([h for h in hops if h["target_miss"]]),
                        "continuous_at_or_below_target": population([h for h in hops if not h["target_miss"]])}, "hops": hops,
        "limits": ["Descriptive correlation only; no inference, policy changes or new host workload. Serial single-run, fixed-order clips, model content, caches and background activity confound causal conclusions.",
            "Each hop uses only a few approximately0.5s samples. The mean/median are sample statistics, not effective clocks or duration-weighted exposure. Maxima and state transitions between samples are unknown.",
            "Miss versus nonmiss populations have3 and13 serial observations. P95/p99 of16continuous hops equal the maximum, not a sustained-tail estimate; no new gate is introduced.",
            "Longer hops contribute more pooled snapshots. Per-hop means are also provided to avoid silently treating snapshot count as independent hop count.",
            "All16continuous hops, including first delivery and tails, remain. Seek snapshots stay in full-trace policy context but are excluded from the continuous-hop populations.",
            "gpu_active_frequency is the sampled kernel gt_act_freq_mhz field; gpu_requested_frequency retains gt_cur_freq_mhz without treating it as measured execution frequency. CPU0 frequency does not describe every CPU core.",
            "Queue delay is the worker poll delay after ready_at; raw-consumer delay also includes harness hashing/work. hop-minus-graph is a difference of recorded durations, not a separately isolated CPU or transport interval.",
            "No audio/UI-load, active-cancellation, physical playback or sustained-music qualification is established by this profile."]}
    with output.open("x") as stream:
        json.dump(profile, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"output": pin(output), "populations": profile["populations"]}, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""CPU-only replay of completed renderer diagnostics and baseline hashes."""
import bisect
import collections
import hashlib
import json
import math
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
OLD = ROOT / "benchmarks-local/isolation-2026-10-16"
PROFILE = "/sys/firmware/acpi/platform_profile"
FREQUENCY = "/sys/class/drm/card1/gt_act_freq_mhz"
TEMPERATURE = "/sys/class/thermal/thermal_zone7/temp"


def pin(path):
    data = path.read_bytes()
    return {"path": str(path.relative_to(ROOT)), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def dist(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return {"count": len(values), "minimum": values[0], "p50": values[math.ceil(len(values)*.5)-1],
            "p95": values[math.ceil(len(values)*.95)-1], "p99": values[math.ceil(len(values)*.99)-1], "maximum": values[-1]}


def summary(records):
    return {"hops": len(records), "misses_above_4095_ms": sum(h["hop_service_ms"] > 4095 for h, _, _ in records),
            "hop_service_ms": dist([h["hop_service_ms"] for h, _, _ in records]),
            "graph_ms": dist([h["metrics"]["graph_ms"] for h, _, _ in records]),
            "hop_minus_graph_ms": dist([h["hop_service_ms"]-h["metrics"]["graph_ms"] for h, _, _ in records]),
            "mean_sampled_gpu_mhz": dist([frequency for _, frequency, _ in records]),
            "maximum_sampled_package_C": dist([temperature for _, _, temperature in records])}


def audit(renderer, historical):
    label = "diagnostic-" + renderer
    directory = BASE / label
    paths = [directory / name for name in ("result.json", "events.jsonl", "native-trace.jsonl")]
    side = {name: BASE / (label + "-" + name + ".json") for name in
            ("policy", "guard", "cleanup", "thermal", "verification")}
    r = json.loads(paths[0].read_text())
    events = [json.loads(line) for line in paths[1].open()]
    hops = [h for h in events if h.get("event") == "soak_hop" and h.get("kind") == "continuous"]
    trace = [json.loads(line) for line in paths[2].open()]
    times = [t["timestamp_monotonic"] for t in trace]
    groups = collections.defaultdict(list)
    for h in hops:
        rows = trace[bisect.bisect_left(times, h["ready_at"]-h["hop_service_ms"]/1000):bisect.bisect_right(times, h["ready_at"])]
        if rows:
            profiles = sorted({t["system_values"][PROFILE] for t in rows})
            groups["+".join(profiles)].append((h, statistics.mean(t["system_values"][FREQUENCY] for t in rows),
                                               max(t["system_values"][TEMPERATURE] for t in rows)/1000))
    episodes = []
    for t in trace:
        profile = t["system_values"][PROFILE]
        offset = t["timestamp_monotonic"] - r["workload_started_monotonic"]
        if not episodes or episodes[-1]["profile"] != profile:
            episodes.append({"profile": profile, "first_snapshot_offset_seconds": offset, "last_snapshot_offset_seconds": offset, "snapshots": 1})
        else:
            episodes[-1]["last_snapshot_offset_seconds"] = offset
            episodes[-1]["snapshots"] += 1
    controls = r["controls"]
    discarded = {(e["generation"], e["request_id"]) for e in events if e.get("event") == "discarded"}
    expected = {(c["superseded_generation"], c["superseded_request_id"]) for c in controls}
    policy = json.loads(side["policy"].read_text())
    guard = json.loads(side["guard"].read_text())
    thermal = json.loads(side["thermal"].read_text())
    return {"inputs": [pin(p) for p in paths + list(side.values())],
            "repository_head": r["repository_head"], "workload_seconds": r["workload_seconds"],
            "reported_continuous": r["timing"]["continuous"], "reported_all_delivered": r["timing"]["all_delivered"],
            "interior_snapshot_profile_groups": {name: summary(value) for name, value in groups.items()},
            "profile_snapshot_episodes": episodes,
            "controls": {"count": len(controls), "all_complete": all(c["complete"] for c in controls),
                         "all_held_chunks_rejected": all(c["held_chunk_rejected"] for c in controls),
                         "post_cancel_chunks": sum(c["post_cancel_chunks"] for c in controls),
                         "discarded_pairs_exact": expected == discarded,
                         "all_exact_numeric_parity": all(c["parity"]["exact"] and c["parity"]["numeric_parity"] and c["parity"]["max_abs_difference"] == 0 for c in controls),
                         "cancel_ack_ms": dist([c["cancel_ack_ms"] for c in controls]),
                         "residual_finish_ms": dist([c["residual_finish_ms"] for c in controls]),
                         "delivered_after_cancel_ms": dist([c["delivered_after_cancel_ms"] for c in controls])},
            "source_and_output_equality_vs_history": {name: {"fixture_sha256_equal": r["fixture"]["sha256"] == old["fixture"]["sha256"],
                         "nine_reference_hashes_equal": len(r["reference_hashes"]) == 9 and r["reference_hashes"] == old["reference_hashes"],
                         "common_pin_sha256_equal": {key: r["pins"][key]["sha256"] == value["sha256"] for key, value in old["pins"].items() if key in r["pins"]}} for name, old in historical.items()},
            "policy_mutation": policy["profile_mutation"], "policy_restored_by_runner": policy["restored_by_runner"],
            "guard_journal_entries": thermal["in_range_guard_entries"], "thermal_guard_service_identity": thermal["service_identity"],
            "window_guard": {key: guard[key] for key in ("status", "errors", "restored", "script_unloaded", "idle_inhibit_released")},
            "cleanup": json.loads(side["cleanup"].read_text()), "worker_statistics": r["worker_statistics"],
            "audio_faults": json.loads(side["verification"].read_text())["audio_faults"], "failures": r["failures"]}


historical_paths = {mode: OLD / f"observed-{mode}-hour/result.json" for mode in ("both", "ui", "none", "audio")}
initial = OLD / "ui-profile-initial-normal-ready.json"
report = {"schema": 1, "script": pin(Path(__file__).resolve()),
          "historical_inputs": {mode: pin(path) for mode, path in historical_paths.items()},
          "initial_opengl_overlapping_readiness_observer": {"input": pin(initial), "record": json.loads(initial.read_text())},
          "scope": ["Completed 300-second diagnostics only; neither qualifies a full-hour target nor establishes renderer improvement.",
                    "Each diagnostic includes native/audio/UI and extra Qt/process profiling; final hour is to run without those extra profilers.",
                    "OpenGL also overlapped a redundant initial readiness observer, saved separately; this limits paired causal comparison.",
                    "Balanced-only and low-power classifications require all available interior snapshots in that state; continuous state between snapshots is unproven.",
                    "Profile episode bounds are first/last observed snapshots, not exact transition instants or continuous durations.",
                    "Within-hop arithmetic mean GPU frequency includes observed zero values; package temperature is maximum observed snapshot.",
                    "A native graph stage includes execution/wait; no hardware GPU submission or preemption proof.",
                    "Cancellation is logical; discarded native work drains. Successful PCM is not retained for independent waveform recomputation.",
                    "Fixed renderer order, policy history and overlapping profiling prevent causal speedup claims."],
          "diagnostics": {renderer: audit(renderer, {name: json.loads(path.read_text()) for name, path in historical_paths.items()}) for renderer in ("opengl", "vulkan")}}
output = BASE / "diagnostic-comparison.json"
output.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"output": str(output), "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                  "groups": {renderer: value["interior_snapshot_profile_groups"] for renderer, value in report["diagnostics"].items()}}))

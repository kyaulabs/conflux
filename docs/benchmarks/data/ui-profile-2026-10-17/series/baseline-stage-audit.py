#!/usr/bin/env python3
"""Bounded, CPU-only audit of the four completed isolation hours."""
import bisect
import hashlib
import json
import math
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "benchmarks-local/isolation-2026-10-16"
PROFILE = "/sys/firmware/acpi/platform_profile"
FREQUENCY = "/sys/class/drm/card1/gt_act_freq_mhz"
TEMPERATURE = "/sys/class/thermal/thermal_zone7/temp"
PL1 = "/sys/class/drm/card1/gt/gt0/throttle_reason_pl1"
THERMAL = "/sys/class/drm/card1/gt/gt0/throttle_reason_thermal"


def pin(path):
    data = path.read_bytes()
    return {"path": str(path.relative_to(ROOT)), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def percentile(values, fraction):
    return sorted(values)[math.ceil(len(values) * fraction) - 1]


def distribution(values):
    if not values:
        return {"count": 0}
    return {"count": len(values), "p50": percentile(values, .5),
            "p95": percentile(values, .95), "p99": percentile(values, .99),
            "maximum": max(values)}


def correlation(left, right):
    x, y = statistics.mean(left), statistics.mean(right)
    return sum((a-x)*(b-y) for a, b in zip(left, right)) / math.sqrt(
        sum((a-x)**2 for a in left) * sum((b-y)**2 for b in right))


def summarize(hops):
    return {"hops": len(hops), "misses_above_4095_ms": sum(
        h["hop_service_ms"] > 4095 for h in hops),
        "hop_service_ms": distribution([h["hop_service_ms"] for h in hops])}


def audit(mode):
    directory = BASE / f"observed-{mode}-hour"
    paths = [directory / name for name in ("result.json", "events.jsonl", "native-trace.jsonl")]
    result = json.loads(paths[0].read_text())
    hops = [event for line in paths[1].open() if
            (event := json.loads(line)).get("event") == "soak_hop"
            and event.get("kind") == "continuous"]
    trace = [json.loads(line) for line in paths[2].open()]
    times = [row["timestamp_monotonic"] for row in trace]
    sampled = []
    for hop in hops:
        start = hop["ready_at"] - hop["hop_service_ms"] / 1000
        rows = trace[bisect.bisect_left(times, start):bisect.bisect_right(times, hop["ready_at"])]
        if rows:
            sampled.append((hop, {row["system_values"].get(PROFILE) for row in rows},
                            statistics.mean(row["system_values"][FREQUENCY] for row in rows),
                            max(row["system_values"][TEMPERATURE] for row in rows) / 1000))
    threshold = percentile([h["hop_service_ms"] for h in hops], .99)
    tail = [h for h in hops if h["hop_service_ms"] >= threshold]
    shares = [h["metrics"]["graph_ms"] / h["hop_service_ms"] * 100 for h in tail]
    groups = {}
    for name, selected in (("misses", [v for v in sampled if v[0]["hop_service_ms"] > 4095]),
                           ("under_3000_ms", [v for v in sampled if v[0]["hop_service_ms"] < 3000])):
        groups[name] = {"hops": len(selected), "mean_sampled_gpu_mhz": distribution([v[2] for v in selected]),
                        "maximum_sampled_package_C": distribution([v[3] for v in selected])}
    return {"inputs": [pin(path) for path in paths], "repository_head": result["repository_head"],
            "continuous": summarize(hops), "sampled_hops": len(sampled),
            "all_interior_samples_balanced": summarize([h for h, profiles, _, _ in sampled if profiles == {"balanced"}]),
            "last_55_minutes": summarize([h for h in hops if h["ready_at"] - result["workload_started_monotonic"] >= 300]),
            "graph_ms": distribution([h["metrics"]["graph_ms"] for h in hops]),
            "queue_delay_ms": distribution([h["queue_delay_ms"] for h in hops]),
            "hop_minus_graph_ms": distribution([h["hop_service_ms"] - h["metrics"]["graph_ms"] for h in hops]),
            "top_one_percent_graph_share_percent": {"hops": len(tail), "minimum": min(shares), "median": statistics.median(shares), "maximum": max(shares)},
            "sampled_frequency_groups": groups,
            "mean_gpu_at_most_650_mhz": summarize([h for h, _, frequency, _ in sampled if frequency <= 650]),
            "graph_vs_mean_sampled_gpu_pearson_r": correlation([h["metrics"]["graph_ms"] for h, _, _, _ in sampled], [frequency for _, _, frequency, _ in sampled]),
            "trace_samples": len(trace),
            "pl1_nonzero_samples": sum(bool(row["system_values"].get(PL1)) for row in trace),
            "gpu_thermal_nonzero_samples": sum(bool(row["system_values"].get(THERMAL)) for row in trace),
            "gpu_thermal_present_samples": sum(THERMAL in row["system_values"] for row in trace)}


report = {"schema": 1, "script": pin(Path(__file__).resolve()),
          "scope": ["CPU-only replay of completed saved evidence; no host probes or benchmarks.",
                    "Nearest-rank percentiles; consecutive hops are not independent tail samples.",
                    "Per-hop interior snapshots span ready_at minus hop_service_ms through ready_at, inclusive; no interpolation.",
                    "Balanced-only means every available interior snapshot reads balanced, not proof of continuous policy between snapshots.",
                    "Within-hop GPU frequency is the arithmetic mean of sampled gt_act_freq_mhz, including sampled zero values.",
                    "Trace covers native worker and host values, not GUI/KWin processes. No causal attribution to GUI updates versus rendering.",
                    "Fixed run order, background activity, power history and roughly 0.5-second sampling limit causal conclusions.",
                    "Missing thermal flags are counted separately; zero observations are not continuous thermal proof.",
                    "GPU utilization is intentionally not derived from fdinfo counters; prior combined trace contains an implausible delta."],
          "runs": {mode: audit(mode) for mode in ("both", "ui", "none", "audio")}}
destination = Path(__file__).with_name("baseline-stage-audit.json")
destination.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"output": str(destination), "runs": {mode: value["continuous"] for mode, value in report["runs"].items()}}))

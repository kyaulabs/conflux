#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Describe native timing and saved GPU/policy snapshots after the series closes.

Separate from acceptance/audit replay: this is descriptive, with no host commands,
new workloads, waveform recomputation, warmup deletion, or causal attribution.
"""
import argparse
import bisect
import collections
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics

BASE = Path(__file__).resolve().parent
PROFILE = "/sys/firmware/acpi/platform_profile"
EPP = "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0, "minimum": None, "p50": None, "p95": None, "p99": None, "maximum": None}
    return {"count": len(values), "minimum": values[0],
            **{f"p{q}": values[math.ceil(len(values) * q / 100) - 1] for q in (50, 95, 99)},
            "maximum": values[-1]}


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--gpu-frequency-path", default="/sys/class/drm/card1/gt_act_freq_mhz")
    args = parser.parse_args()
    run = BASE / args.name
    inputs = {}

    def fingerprint(path):
        with path.open("rb") as stream:
            inputs[str(path)] = {"bytes": path.stat().st_size,
                                 "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}

    def read(path):
        fingerprint(path)
        return json.loads(path.read_text())

    result = read(run / "result.json")
    launch = read(BASE / (args.name + "-launch.json"))
    cleanup = read(BASE / (args.name + "-cleanup.json"))
    series = read(BASE / "software-series-guard.json")
    if not (result.get("finished_at_utc") and result.get("status") == "ok"
            and not result.get("failures") and launch.get("finished_at_utc")
            and launch.get("returncode") == 0 and cleanup.get("worker_closed")
            and cleanup.get("PID_exists") and not any(cleanup["PID_exists"].values())
            and cleanup.get("fixture_exists") is False and cleanup.get("idle_inhibit_released")
            and not cleanup.get("guard_errors") and series.get("idle_inhibit_released")
            and series.get("finished_at_utc")):
        raise RuntimeError("Refusing descriptive replay while the workload or owned lifecycle is unfinished")

    def records(path):
        if not path.exists():
            path = path.with_name(path.name + ".gz")
        fingerprint(path)
        with (gzip.open if path.suffix == ".gz" else open)(path, "rt") as stream:
            return [json.loads(line) for line in stream if line.strip()]

    hops = [row for row in records(run / "events.jsonl") if row.get("event") == "soak_hop"]
    trace = records(run / "native-trace.jsonl")
    times = [row["timestamp_monotonic"] for row in trace]
    if not times or not all(finite(value) for value in times) or times != sorted(times):
        raise RuntimeError("Saved native trace is empty, nonfinite or unordered")
    start, stop = result["workload_started_monotonic"], result["workload_stopped_monotonic"]
    classified = []
    for hop in hops:
        service = hop["hop_service_ms"]
        graph = hop["metrics"]["graph_ms"]
        queue = hop["queue_delay_ms"]
        if not all(finite(value) for value in (service, graph, queue)):
            raise RuntimeError("Missing/nonfinite saved hop timing")
        rows = trace[bisect.bisect_left(times, hop["ready_at"] - service / 1000):
                     bisect.bisect_right(times, hop["ready_at"])]
        frequency = [row.get("system_values", {}).get(args.gpu_frequency_path) for row in rows]
        valid = [value for value in frequency if finite(value)]
        policies = sorted({(str(row.get("system_values", {}).get(PROFILE, "MISSING")),
                            str(row.get("system_values", {}).get(EPP, "MISSING"))) for row in rows})
        classified.append({"kind": hop["kind"], "generation": hop["generation"],
            "request_id": hop["request_id"], "elapsed_seconds": hop["elapsed_seconds"],
            "hop_service_ms": service, "graph_ms": graph, "queue_delay_ms": queue,
            "hop_minus_graph_ms": service - graph, "above_target": service > 4095,
            "policy_class": json.dumps(policies, separators=(",", ":")),
            "native_snapshots": len(rows), "frequency_missing_or_invalid": len(frequency) - len(valid),
            "sampled_gpu_mhz": valid, "mean_sampled_gpu_mhz": statistics.mean(valid) if valid else None})

    def summary(rows):
        return {"hops": len(rows), "timing_ms": {name: distribution(row[name] for row in rows)
                    for name in ("hop_service_ms", "graph_ms", "queue_delay_ms", "hop_minus_graph_ms")},
                "mean_interior_gpu_mhz_per_hop": distribution(row["mean_sampled_gpu_mhz"] for row in rows
                    if row["mean_sampled_gpu_mhz"] is not None),
                "all_interior_snapshot_gpu_mhz": distribution(value for row in rows for value in row["sampled_gpu_mhz"]),
                "hops_without_native_snapshots": sum(row["native_snapshots"] == 0 for row in rows),
                "frequency_missing_or_invalid": sum(row["frequency_missing_or_invalid"] for row in rows),
                "policy_classes_hop_counts": dict(collections.Counter(row["policy_class"] for row in rows))}

    populations = {}
    for name, selected in (("continuous", [row for row in classified if row["kind"] == "continuous"]),
                           ("all_delivered", classified)):
        populations[name] = {"all": summary(selected),
            "target_miss_gt_4095_ms": summary([row for row in selected if row["above_target"]]),
            "at_or_below_4095_ms": summary([row for row in selected if not row["above_target"]])}
    phases, previous = [], None
    counts = collections.Counter()
    for row in trace:
        values = row.get("system_values", {})
        phase = (str(values.get(PROFILE, "MISSING")), str(values.get(EPP, "MISSING")))
        counts[phase] += 1
        if phase != previous:
            phases.append({"timestamp_monotonic": row["timestamp_monotonic"],
                           "workload_offset_seconds": row["timestamp_monotonic"] - start,
                           "firmware": phase[0], "cpu0_EPP": phase[1]})
            previous = phase
    bins = []
    for interval in result["timing"]["bins_300_seconds"]:
        selected = [row for row in classified if interval["start_seconds"] <= row["elapsed_seconds"] < interval["end_seconds"]]
        bins.append({"start_seconds": interval["start_seconds"], "end_seconds": interval["end_seconds"],
                     "all_delivered": summary(selected),
                     "continuous": summary([row for row in selected if row["kind"] == "continuous"])})
    report = {"schema": 1, "run": args.name, "repository_head": result["repository_head"],
        "renderer": result["renderer"], "requested_seconds": result["requested_seconds"],
        "workload_seconds": result["workload_seconds"], "requested_repaint_interval_ms": launch["repaint_ms"],
        "target_ms": 4095, "populations": populations, "bins_300_seconds": bins,
        "native_snapshots": {"all_retained": len(trace), "before_workload": sum(value < start for value in times),
            "inside_workload": sum(start <= value <= stop for value in times), "after_workload": sum(value > stop for value in times),
            "policy_phase_counts": [{"firmware": key[0], "cpu0_EPP": key[1], "samples": value} for key, value in counts.items()],
            "policy_transitions_including_startup": phases,
            "gpu_frequency_path": args.gpu_frequency_path,
            "all_retained_valid_gpu_mhz": distribution(row.get("system_values", {}).get(args.gpu_frequency_path)
                for row in trace if finite(row.get("system_values", {}).get(args.gpu_frequency_path)))},
        "target_miss_hops": [{key: value for key, value in row.items() if key != "sampled_gpu_mhz"}
                             for row in classified if row["above_target"]],
        "limitations": [
            "Saved completed evidence only; descriptive associations do not establish GPU frequency, policy or repaint cadence as the cause of a miss.",
            "No startup/first-bin or phase removal. Continuous hops define the existing M0 p99 target; all-delivered populations also include seek replacements.",
            "Nearest-rank distributions use serial dependent hops. Small per-bin or miss populations have coarse tails; no new acceptance gate is added.",
            "graph_ms includes native execution/wait. hop-minus-graph is a difference between saved intervals, not an isolated CPU/transport duration; queue_delay_ms retains its original harness definition.",
            "Each hop uses snapshots inside [ready_at - hop_service, ready_at]. Mean frequency is an unweighted mean of valid interior samples, not continuous/effective frequency or proof of time in a state. Missing or sparse samples can miss transitions.",
            "Interior snapshot distributions can count one snapshot for multiple overlapping hop intervals. Full-trace phase counts/transitions include observations before/after the workload and do not measure the thermal guard's private latch.",
        ], "inputs": inputs,
        "analysis_source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    output = args.output or BASE / (args.name + "-native-tail-analysis.json")
    with output.open("x") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"output": str(output), "continuous": populations["continuous"]}))


if __name__ == "__main__":
    main()

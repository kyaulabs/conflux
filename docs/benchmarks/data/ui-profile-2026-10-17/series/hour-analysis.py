#!/usr/bin/env python3
"""Replay the completed combined Vulkan hour; refuse unfinished lifecycle records."""
import bisect
import collections
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path
import statistics

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
NAME = "combined-vulkan-hour"
RUN = BASE / NAME
OLD = ROOT / "benchmarks-local/isolation-2026-10-16"
PROFILE = "/sys/firmware/acpi/platform_profile"
GPU = "/sys/class/drm/card1/gt_act_freq_mhz"
FAULTS = ("callback_execution_deadline_misses", "missing_buffers", "invalid_buffers", "truncated_frames",
          "observed_graph_position_discontinuities", "observed_position_clock_xrun_recovery_flags")
inputs = []


def read(path, lines=False):
    data = path.read_bytes()
    inputs.append({"path": str(path.relative_to(ROOT)), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    return [json.loads(line) for line in data.splitlines()] if lines else json.loads(data)


def sibling(suffix):
    return BASE / (NAME + "-" + suffix + ".json")


def dist(values):
    values = sorted(values)
    return ({"count": len(values), "minimum": values[0], "p50": values[math.ceil(len(values)*.5)-1],
             "p95": values[math.ceil(len(values)*.95)-1], "p99": values[math.ceil(len(values)*.99)-1], "maximum": values[-1]}
            if values else {"count": 0})


def summary(hops):
    return {"hops": len(hops), "hop_service_ms": dist([h["hop_service_ms"] for h in hops]),
            "above_4095_ms": sum(h["hop_service_ms"] > 4095 for h in hops)}


def ac_summary(rows):
    times = [row["monotonic"] for row in rows]
    return {"samples": len(rows), "online_values": dict(collections.Counter(row["mains_online"] for row in rows)),
            "maximum_adjacent_snapshot_gap_seconds": max((b-a for a, b in zip(times, times[1:])), default=None),
            "first_monotonic": times[0] if times else None, "last_monotonic": times[-1] if times else None}


sequence = read(BASE / "hour-sequence.json")
launch = read(sibling("launch"))
r = read(RUN / "result.json")
cleanup = read(sibling("cleanup"))
guard = read(sibling("guard"))
series_guard = read(BASE / "profile-series-guard.json")
if not (sequence.get("status") == "completed" and launch.get("finished_at_utc") and launch.get("returncode") == 0
        and r.get("finished_at_utc") and r.get("status") == "ok" and cleanup.get("worker_closed")
        and not any(cleanup.get("PID_exists", {"unknown": True}).values()) and not cleanup.get("fixture_exists", True)
        and guard.get("idle_inhibit_released") and guard.get("script_unloaded")
        and series_guard.get("idle_inhibit_released") and series_guard.get("finished_at_utc")):
    raise SystemExit("Refusing audit: result, launch, sequence or owned cleanup is unfinished.")

events = read(RUN / "events.jsonl", True)
trace = read(RUN / "native-trace.jsonl", True)
audio = read(RUN / "audio.json")
ui = read(RUN / "ui.json")
policy = read(sibling("policy"))
kernel = read(sibling("kernel"))
thermal = read(sibling("thermal-snapshot-audit"))
ac = read(BASE / (NAME + "-AC-monitor.jsonl"), True)
all_hops = [e for e in events if e.get("event") == "soak_hop"]
hops = [h for h in all_hops if h.get("kind") == "continuous"]
times = [t["timestamp_monotonic"] for t in trace]
groups = collections.defaultdict(list)
sampled = []
for hop in hops:
    rows = trace[bisect.bisect_left(times, hop["ready_at"]-hop["hop_service_ms"]/1000):bisect.bisect_right(times, hop["ready_at"])]
    if rows:
        group = "+".join(sorted({t["system_values"][PROFILE] for t in rows}))
        frequency = statistics.mean(t["system_values"][GPU] for t in rows)
        groups[group].append(hop)
        sampled.append((hop["metrics"]["graph_ms"], frequency))
xmean = statistics.mean(x for x, _ in sampled)
ymean = statistics.mean(y for _, y in sampled)
denominator = math.sqrt(sum((x-xmean)**2 for x, _ in sampled) * sum((y-ymean)**2 for _, y in sampled))
correlation = sum((x-xmean)*(y-ymean) for x, y in sampled) / denominator if denominator else None
controls = r["controls"]
discarded = {(e["generation"], e["request_id"]) for e in events if e.get("event") == "discarded"}
expected = {(c["superseded_generation"], c["superseded_request_id"]) for c in controls}
historical = {}
for mode in ("both", "ui", "none", "audio"):
    old = read(OLD / f"observed-{mode}-hour/result.json")
    common = ("runtime", "model", "soak_source", "shared_benchmark_source", "worker_source", "sampler_source")
    historical[mode] = {"timing_continuous": old["timing"]["continuous"], "workload_seconds": old["workload_seconds"],
        "common_six_pin_sha256_equal": {key: r["pins"][key]["sha256"] == old["pins"][key]["sha256"] for key in common},
        "all_available_common_pin_sha256_equal": {key: r["pins"][key]["sha256"] == value["sha256"] for key, value in old["pins"].items() if key in r["pins"]},
        "fixture_sha256_equal": r["fixture"]["sha256"] == old["fixture"]["sha256"],
        "nine_reference_hashes_equal": len(r["reference_hashes"]) == 9 and r["reference_hashes"] == old["reference_hashes"]}
start, stop = r["workload_started_monotonic"], r["workload_stopped_monotonic"]
launch_ac = [row for row in ac if datetime.fromisoformat(launch["started_at_utc"]) <= datetime.fromisoformat(row["at_utc"]) <= datetime.fromisoformat(launch["finished_at_utc"])]
workload_ac = [row for row in ac if start <= row["monotonic"] <= stop]
report = {"schema": 1, "repository_head": r["repository_head"], "renderer": r["renderer"],
    "workload_seconds": r["workload_seconds"], "continuous_replayed": summary(hops), "all_hops_replayed": summary(all_hops),
    "reported_timing_including_bins": r["timing"], "existing_acceptance": r["acceptance"], "failures": r["failures"],
    "historical_comparison": historical,
    "native_stages": {"graph_ms": dist([h["metrics"]["graph_ms"] for h in hops]),
        "hop_minus_graph_ms": dist([h["hop_service_ms"]-h["metrics"]["graph_ms"] for h in hops]),
        "queue_delay_ms": dist([h["queue_delay_ms"] for h in hops]), "profile_classified_hops": {key: summary(value) for key, value in groups.items()},
        "graph_vs_mean_sampled_gpu_frequency_pearson_r": correlation, "profile_snapshot_counts": dict(collections.Counter(t["system_values"][PROFILE] for t in trace)),
        "gpu_frequency_mhz": dist([t["system_values"][GPU] for t in trace])},
    "cancellation": {"count": len(controls), "all_complete": all(c["complete"] for c in controls),
        "discarded_pairs_exact": discarded == expected, "all_held_rejected": all(c["held_chunk_rejected"] for c in controls),
        "post_cancel_chunks": sum(c["post_cancel_chunks"] for c in controls), "stale_chunks_delivered": r["stale_chunks_delivered"],
        "all_exact_and_numeric_parity": all(c["parity"]["exact"] and c["parity"]["numeric_parity"] and c["parity"]["max_abs_difference"] == 0 for c in controls),
        "cancel_ack_ms": dist([c["cancel_ack_ms"] for c in controls]), "native_drain_ms": dist([c["residual_finish_ms"] for c in controls]),
        "delivered_after_cancel_ms": dist([c["delivered_after_cancel_ms"] for c in controls]), "worker_statistics": r["worker_statistics"]},
    "audio": {"faults": {name: audio[name] for name in FAULTS}, "requested": audio["requested"], "negotiated": audio["negotiated"],
        "callbacks": audio["callbacks"], "frames_rendered": audio["frames_rendered"], "callback_elapsed_us": audio["callback_elapsed_us"],
        "streaming_seconds": audio["streaming_seconds"], "coverage": r["audio_coverage"], "scope": audio["measurement_scope"]},
    "ui": {"frames_swapped": ui["frames_swapped"], "graphics_api": ui["graphics_api"], "platform": ui["platform"],
        "frame_observation": ui["frame_observation"], "first_60_nonzero_bins": sum(bool(n) for n in ui["frame_observation"]["frames_per_60_seconds"][:60]), "coverage": r["ui_coverage"]},
    "AC": {"within_launch": ac_summary(launch_ac), "within_workload": ac_summary(workload_ac),
        "workload_start_to_first_sample_seconds": workload_ac[0]["monotonic"]-start if workload_ac else None,
        "last_sample_to_workload_stop_seconds": stop-workload_ac[-1]["monotonic"] if workload_ac else None},
    "thermal_snapshot_audit": thermal, "kernel": {"returncode": kernel["returncode"], "parse_errors": kernel["parse_errors"], "matched_entries": kernel["matched_entries"]},
    "policy_mutation": policy["profile_mutation"], "policy_restored_by_runner": policy["restored_by_runner"],
    "cleanup": cleanup, "sequence_status": sequence["status"], "series_idle_inhibit_released": series_guard["idle_inhibit_released"],
    "limitations": ["Fixed order and differing day/time, background activity and policy history prevent causal renderer speedup claims.",
        "Short diagnostics used extra Qt/process profiling and the first also overlapped a redundant readiness observer; this full hour omits those extra profilers.",
        "Nearest-rank hop percentiles use serial, dependent samples. Full-hour continuous p99 target is4095 ms; no per-bin acceptance gate is added.",
        "Native graph_ms includes execution/wait. Interior snapshots cannot establish exact policy transitions or continuous GPU frequency/temperature.",
        "AC observations establish sampled online values, not uninterrupted mains between samples.",
        "Cancellation is logical and validates current discarded generation/request pairs and rejection; hardware submission/preemption is unobserved.",
        "Same-grid output SHA and trimmed-seek exact/numeric results are recorded; successful PCM is not retained for independent waveform recomputation.",
        "Audio is independent silence on available laptop output. Physical latency, musical quality, actual stem playback and a complete graph xrun count are unmeasured.",
        "Brief heat spikes are acceptable to the user; sampled thermal peaks alone are not failures and no sustained-temperature threshold is invented."]}
script = Path(__file__).resolve()
data = script.read_bytes()
inputs.append({"path": str(script.relative_to(ROOT)), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
report["inputs"] = inputs
destination = BASE / "hour-analysis.json"
destination.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"output": str(destination), "continuous": report["continuous_replayed"], "audio": report["audio"]["faults"]}))

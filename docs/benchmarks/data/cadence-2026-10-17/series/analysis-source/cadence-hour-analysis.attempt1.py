#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Summarize completed cadence-hour audit JSON; no host queries or trace replay.

Run only after the workload and owned guards finish. The independent event and
thermal audits must already exist; their source-result hashes are checked here.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path

BASE = Path(__file__).resolve().parent
FROZEN = ("runtime", "model", "soak_source", "shared_benchmark_source",
          "worker_source", "sampler_source", "audio_probe")
FAULTS = ("callback_execution_deadline_misses", "missing_buffers", "invalid_buffers",
          "truncated_frames", "observed_graph_position_discontinuities",
          "observed_position_clock_xrun_recovery_flags")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", help="Completed 3600-second trial label in this directory")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--baseline-result", type=Path, default=BASE.parent /
                        "ui-profile-2026-10-17/combined-vulkan-hour/result.json")
    args = parser.parse_args()
    run = BASE / args.name
    inputs = {}

    def read(path, lines=False):
        data = path.read_bytes()
        inputs[str(path)] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        return [json.loads(line) for line in data.splitlines() if line.strip()] if lines else json.loads(data)

    def sibling(suffix):
        return BASE / (args.name + "-" + suffix + ".json")

    result = read(run / "result.json")
    launch = read(sibling("launch"))
    cleanup = read(sibling("cleanup"))
    guard = read(sibling("guard"))
    series = read(BASE / "cadence-series-guard.json")
    sequence = read(BASE / "hour-sequence.json")
    if not (result.get("finished_at_utc") and result.get("status") == "ok"
            and not result.get("failures") and result.get("requested_seconds") == 3600
            and result.get("workload_seconds", 0) >= 3600
            and launch.get("finished_at_utc") and launch.get("returncode") == 0
            and cleanup.get("worker_closed") and cleanup.get("PID_exists")
            and not any(cleanup["PID_exists"].values()) and cleanup.get("fixture_exists") is False
            and guard.get("idle_inhibit_released") and guard.get("script_unloaded")
            and not guard.get("errors") and sequence.get("status") == "completed"
            and series.get("idle_inhibit_released") and series.get("finished_at_utc")):
        raise RuntimeError("Refusing analysis: the hour or its owned lifecycle is unfinished/failed")

    audit = read(sibling("independent-audit"))
    thermal = read(sibling("thermal-snapshot-audit"))
    cadence = read(sibling("cadence-audit"))
    audio = read(run / "audio.json")
    ui = read(run / "ui.json")
    kernel = read(sibling("kernel"))
    policy = read(sibling("policy"))
    baseline = read(args.baseline_result)
    result_sha = inputs[str(run / "result.json")]["sha256"]
    if not (audit["source_evidence"]["result_sha256"] == result_sha
            and thermal["source_evidence"]["result_sha256"] == result_sha
            and audit["repository_head"] == result["repository_head"]
            and audit["workload_seconds"] == result["workload_seconds"]
            and audit["timing"]["continuous"] == result["timing"]["continuous"]
            and audit["timing"]["all_delivered"] == result["timing"]["all_delivered"]
            and thermal["workload_seconds"] == result["workload_seconds"]):
        raise RuntimeError("Saved audits do not describe this completed result")

    def timing(summary):
        return {**summary, "p99_seconds": summary["hop_service_percentiles_ms"].get("99", 0) / 1000,
                "maximum_seconds": summary["hop_service_max_ms"] / 1000
                if summary["hop_service_max_ms"] is not None else None}

    bins = audit["timing"]["bins_300_seconds"]
    complete = [row for row in bins if row["end_seconds"] <= 3600 and row["chunks"]]
    pins = {name: {"sha256": result["pins"][name]["sha256"],
                   "baseline_sha256": baseline["pins"][name]["sha256"],
                   "equal": result["pins"][name]["sha256"] == baseline["pins"][name]["sha256"]}
            for name in FROZEN}
    controls = audit["controls"]
    ac_rows = read(BASE / (args.name + "-AC-lid-monitor.jsonl"), lines=True)
    start, stop = result["workload_started_monotonic"], result["workload_stopped_monotonic"]
    ac = [row for row in ac_rows if row.get("event") == "snapshot"
          and start <= row["monotonic"] <= stop]
    ac_times = [row["monotonic"] for row in ac]
    p99 = audit["timing"]["continuous"]["hop_service_percentiles_ms"]["99"]
    report = {
        "schema": 1, "run": args.name, "repository_head": result["repository_head"],
        "workload_seconds": result["workload_seconds"], "requested_repaint_interval_ms": launch["repaint_ms"],
        "continuous": timing(audit["timing"]["continuous"]),
        "all_delivered": timing(audit["timing"]["all_delivered"]),
        "target": {"continuous_hop_p99_seconds": 4.095, "passed": p99 <= 4095,
                   "margin_seconds": (4095 - p99) / 1000,
                   "scope": "Existing full-hour continuous-hop nearest-rank target; no per-bin gate added"},
        "all_hop_bins_300_seconds": bins,
        "complete_bin_summary": {"nonempty_complete_bins": len(complete),
            "p99_at_or_below_4095_ms": sum(row["hop_service_percentiles_ms"]["99"] <= 4095 for row in complete),
            "first_bin_retained": bool(bins) and bins[0]["start_seconds"] == 0,
            "scope": "All delivered hops, including seek replacements. Startup and every saved partial/empty bin retained above"},
        "cancellation": {**controls, "stale_chunks_delivered": result["stale_chunks_delivered"],
            "post_cancel_chunks": sum(row["post_cancel_chunks"] for row in result["controls"]),
            "worker_statistics": audit["worker"],
            "scope": "Independent audit validates discarded generation/request pairs and held-chunk rejection; logical cancellation only. Hardware submission/preemption unobserved"},
        "audio": {**audit["audio"], "faults": {name: audio[name] for name in FAULTS},
            "requested": audio["requested"], "negotiated": audio["negotiated"],
            "streaming_seconds": audio["streaming_seconds"], "coverage": result["audio_coverage"],
            "scope": audio["measurement_scope"]},
        "UI": {"frames_swapped": ui["frames_swapped"], "graphics_api": ui["graphics_api"],
            "platform": ui["platform"], "frame_observation": ui["frame_observation"],
            "cadence_status": cadence["status"], "cadence_failed_checks": cadence["failed_checks"],
            "frame_rate": cadence["frame_rate"],
            "motion_summary": {key: value for key, value in cadence["motion"].items() if key != "positions"},
            "coverage": result["ui_coverage"]},
        "frozen_pins_against_prior_vulkan_hour": pins,
        "all_frozen_pins_equal": all(row["equal"] for row in pins.values()),
        "ui_pin_intentionally_changed": result["pins"]["ui"],
        "fixture_sha256_equal": result["fixture"]["sha256"] == baseline["fixture"]["sha256"],
        "reference_hashes_equal": result["reference_hashes"] == baseline["reference_hashes"],
        "prior_vulkan_hour_context": {"repository_head": baseline["repository_head"],
            "workload_seconds": baseline["workload_seconds"], "continuous": baseline["timing"]["continuous"]},
        "native_policy": audit["native_policy"],
        "policy_trace_scope": "All retained native snapshots, preserving startup and every recorded phase/transition, including offsets outside the workload; no interpolation or private-latch proof",
        "thermal_snapshot_audit": thermal,
        "AC_lid_within_workload": {"samples": len(ac),
            "mains_online_values": dict(collections.Counter(row.get("mains_online") for row in ac)),
            "lid_closed_values": dict(collections.Counter(str(row.get("lid_closed")) for row in ac)),
            "maximum_adjacent_snapshot_gap_seconds": max((b-a for a, b in zip(ac_times, ac_times[1:])), default=None),
            "start_to_first_snapshot_seconds": ac_times[0]-start if ac_times else None,
            "last_snapshot_to_stop_seconds": stop-ac_times[-1] if ac_times else None,
            "observation_errors": [error for row in ac for error in row.get("errors", [])]},
        "kernel": {key: kernel[key] for key in ("returncode", "parse_errors", "matched_entries")},
        "profile_mutation": policy["profile_mutation"], "restored_by_runner": policy["restored_by_runner"],
        "existing_acceptance": result["acceptance"], "cleanup": cleanup,
        "limitations": [
            "Fixed order, differing times/background load and thermal/policy/cache history prevent a causal cadence speedup claim. Prior hour also used the original NumberAnimation rather than QTimeLine; the short 16/33 ms comparison is a separate experiment.",
            "No warmup or first-bin deletion. Serial dependent hops use nearest-rank percentiles; per-bin excursions do not add an acceptance gate.",
            "Cancellation validates logical request/generation behavior; active native GPU work drains. Hardware submission or preemption is not measured.",
            "Successful PCM is not retained/recomputed; recorded same-grid hashes and exact/numeric seek assertions are audited.",
            "Audio is independent silence on available laptop output. Physical latency, actual stem playback, musical quality and a complete graph xrun count are unmeasured; no external audio interface is required for this assessment.",
            "Temperature and AC/lid values are sampled. Missing sensors/observations and intervening changes remain unobserved. Brief heat spikes are acceptable; peak alone is not a failure and no sustained-temperature threshold is invented.",
            "UI observations describe GUI frameSwapped notifications and saved low-frequency position samples, not physical presentation or a global frame-rate limit.",
        ],
        "inputs": inputs,
        "analysis_source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    output = args.output or BASE / (args.name + "-analysis.json")
    with output.open("x") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"output": str(output), "continuous_p99_seconds": p99 / 1000,
                      "target_passed": report["target"]["passed"],
                      "controls": controls["complete"], "audio_faults": report["audio"]["faults"]}))


if __name__ == "__main__":
    main()

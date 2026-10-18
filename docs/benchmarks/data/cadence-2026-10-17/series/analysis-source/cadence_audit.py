#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Audit saved completed cadence trials; never query the host or launch work."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import statistics

BASE = Path(__file__).resolve().parent
FROZEN = ("runtime", "model", "soak_source", "shared_benchmark_source",
          "worker_source", "sampler_source", "audio_probe")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compare-control", help="Completed, matched-duration 16 ms trial label")
    args = parser.parse_args()
    inputs = {}

    def read(path, lines=False):
        data = path.read_bytes()
        inputs[str(path)] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        return [json.loads(line) for line in data.splitlines() if line.strip()] if lines else json.loads(data)

    def completed(name):
        run = BASE / name
        result = read(run / "result.json")
        launch = read(BASE / (name + "-launch.json"))
        if (not launch.get("finished_at_utc") or launch.get("returncode") != 0
                or result.get("status") != "ok" or result.get("failures")
                or result.get("workload_seconds", 0) < result.get("requested_seconds", math.inf)):
            raise RuntimeError("Refusing cadence audit of unfinished or failed trial: " + name)
        return result, launch

    result, launch = completed(args.name)
    ui = read(BASE / args.name / "ui.json")
    coverage = result["ui_coverage"]
    interval = launch["repaint_ms"]
    observation = ui["frame_observation"]
    begin, end = result["workload_started_monotonic"], result["workload_stopped_monotonic"]
    ui_start = observation["started_monotonic_ns"] / 1e9
    ui_stop = observation["stopped_monotonic_ns"] / 1e9
    if not ui_start < ui_stop or interval not in (16, 33):
        raise RuntimeError("Invalid saved UI interval or requested repaint parameter")
    counts = observation["frames_per_60_seconds"]
    complete = [index for index in range(len(counts))
                if ui_start + (index + 1) * 60 <= ui_stop
                and ui_start + index * 60 < end and ui_start + (index + 1) * 60 > begin]
    fps = [counts[index] / 60 for index in complete]
    lower, upper = (50, 70) if interval == 16 else (25, 40)
    rows = read(BASE / "monitor.jsonl", lines=True)
    pulses = {}
    for row in rows:
        pulse = row.get("ui_pulse")
        if row.get("run") != args.name or not isinstance(pulse, dict):
            continue
        timestamp = pulse.get("timestamp_monotonic_ns")
        if not isinstance(timestamp, int) or isinstance(timestamp, bool):
            continue
        if begin <= timestamp / 1e9 <= end:
            pulses[(pulse.get("pid"), timestamp)] = pulse
    positions = []
    for pulse in sorted(pulses.values(), key=lambda row: row["timestamp_monotonic_ns"]):
        position = pulse.get("indicator_x")
        if isinstance(position, (int, float)) and not isinstance(position, bool) and math.isfinite(position):
            positions.append({"workload_offset_seconds": pulse["timestamp_monotonic_ns"] / 1e9 - begin,
                              "indicator_x": position})
    values = [row["indicator_x"] for row in positions]
    motion_range = max(values) - min(values) if values else None
    baseline = read(BASE.parent / "ui-profile-2026-10-17" / "combined-vulkan-hour" / "result.json")
    frozen = {name: {"sha256": result["pins"][name]["sha256"],
                     "baseline_sha256": baseline["pins"][name]["sha256"],
                     "equal": result["pins"][name]["sha256"] == baseline["pins"][name]["sha256"]}
              for name in FROZEN}
    controls = result["controls"]
    checks = {
        "both_loads_selected": result["load_mode"] == "both" and set(result["selected_loads"]) == {"audio", "ui"},
        "native_wayland_vulkan": ui["platform"] == "wayland" and ui["graphics_api"] == 5 and result["renderer"] == "vulkan",
        "saved_ui_report_matches_result": ui == result["load"]["children"]["ui"]["report"],
        "requested_interval_matches_ui": ui.get("repaint_interval_ms") == interval,
        "requested_interval_matches_environment": launch["UI_diagnostic_environment"].get("CONFLUX_M0_REPAINT_MS") == str(interval),
        "qtimeline_driver": ui.get("animation_driver") == "qtimeline",
        "reduced_motion_disabled": ui.get("reduced_motion") is False and bool(pulses)
                                   and all(pulse.get("reduced_motion") is False for pulse in pulses.values()),
        "pulse_driver_interval_match": bool(pulses) and all(pulse.get("animation_driver") == "qtimeline"
                                             and pulse.get("repaint_interval_ms") == interval for pulse in pulses.values()),
        "observed_indicator_motion": len(positions) >= 2 and motion_range > 0,
        "existing_ui_coverage_passed": coverage.get("passed") is True and all(coverage["checks"].values()),
        "existing_audio_coverage_passed": result["audio_coverage"].get("passed") is True,
        "zero_sampled_exposure_loss": coverage["progress"]["unexposed_observations"] == 0
                                      and ui["exposed"] is True and all(pulse.get("exposed") is True for pulse in pulses.values()),
        "complete_bins_match_existing_coverage": complete == coverage["complete_minute_bins_intersecting_workload"],
        "sufficient_complete_bins": len(complete) >= math.floor(result["requested_seconds"] / 60),
        "minute_counts_match_total": sum(counts) == ui["frames_swapped"],
        "complete_minute_fps_in_experiment_band": bool(fps) and all(lower <= value <= upper for value in fps),
        "common_producer_audio_native_model_pins_frozen": all(row["equal"] for row in frozen.values()),
        "fixture_pin_frozen": result["fixture"]["sha256"] == baseline["fixture"]["sha256"],
        "nine_reference_window_hashes_frozen": len(result["reference_hashes"]) == 9 and result["reference_hashes"] == baseline["reference_hashes"],
        "control_count_valid": len(controls) == 11 if result["requested_seconds"] == 3600 else len(controls) >= 1,
        "recorded_controls_passed": bool(controls) and all(control["complete"] and control["held_chunk_rejected"]
                             and control["post_cancel_chunks"] == 0 and control["parity"]["exact"]
                             and control["parity"]["numeric_parity"] and control["parity"]["max_abs_difference"] == 0 for control in controls),
        "no_stale_delivery": result["stale_chunks_delivered"] == 0,
        "diagnostic_timing_disabled": launch.get("diagnostic_ui_logging") is False
            and launch["UI_diagnostic_environment"].get("QT_LOGGING_RULES") == "qt.scenegraph.time.*=false",
    }
    comparison = None
    if args.compare_control:
        control, control_launch = completed(args.compare_control)
        common_pin_matches = {name: result["pins"][name]["sha256"] == control["pins"][name]["sha256"]
                              for name in result["pins"] if name in control["pins"]}
        keys = ("renderer", "load_mode", "seconds", "diagnostic_ui_logging", "policy_mode", "profile_mutation")
        configuration = {key: launch.get(key) == control_launch.get(key) for key in keys}
        candidate_env = {key: value for key, value in launch["UI_diagnostic_environment"].items()
                         if key != "CONFLUX_M0_REPAINT_MS"}
        control_env = {key: value for key, value in control_launch["UI_diagnostic_environment"].items()
                       if key != "CONFLUX_M0_REPAINT_MS"}
        command_options = ("--load", "--renderer", "--seconds", "--control-interval", "--request-timeout", "--sample-interval")
        def option(command, name):
            return command[command.index(name) + 1]
        command_matches = {name: option(launch["command"], name) == option(control_launch["command"], name)
                           for name in command_options}
        comparison = {"control": args.compare_control, "control_interval_ms": control_launch["repaint_ms"],
                      "all_common_pin_matches_including_ui": common_pin_matches,
                      "configuration_matches": configuration, "command_option_matches": command_matches,
                      "environment_equal_except_repaint_parameter": candidate_env == control_env,
                      "fixture_equal": result["fixture"]["sha256"] == control["fixture"]["sha256"],
                      "reference_hashes_equal": result["reference_hashes"] == control["reference_hashes"],
                      "scope": "Recorded matched-duration configuration and artifact checks only; run labels, output paths, PIDs and times differ. Fixed order, thermal/policy/cache history and background load prevent a causal guarantee."}
        checks["comparison_uses_16ms_control_and_33ms_candidate"] = control_launch["repaint_ms"] == 16 and interval == 33
        checks["comparison_ui_and_common_pins_match"] = set(result["pins"]) == set(control["pins"]) and all(common_pin_matches.values())
        checks["comparison_only_repaint_parameter_differs_in_recorded_configuration"] = (
            all(configuration.values()) and all(command_matches.values()) and candidate_env == control_env
            and comparison["fixture_equal"] and comparison["reference_hashes_equal"])
    report = {
        "schema": 1, "audited_at_utc": datetime.now(timezone.utc).isoformat(), "run": args.name,
        "status": "valid" if all(checks.values()) else "invalid", "checks": checks,
        "failed_checks": [key for key, value in checks.items() if not value],
        "requested_repaint_interval_ms": interval, "animation_driver": ui.get("animation_driver"),
        "cadence_scope": "QTimeLine update interval, not a global frame-rate limit; observed GUI frameSwapped notifications do not measure physical presentation",
        "experiment_fps_band": {"minimum": lower, "maximum": upper,
            "scope": "Experiment efficacy/validity band only, not a new M0 latency acceptance gate"},
        "frame_rate": {"complete_minute_indices": complete,
            "complete_minute_frames": [counts[index] for index in complete], "complete_minute_fps": fps,
            "complete_minute_fps_minimum": min(fps) if fps else None,
            "complete_minute_fps_maximum": max(fps) if fps else None,
            "complete_minute_fps_mean": statistics.mean(fps) if fps else None,
            "all_ui_frames": ui["frames_swapped"], "ui_observation_interval_seconds": ui_stop - ui_start,
            "whole_ui_interval_average_fps": ui["frames_swapped"] / (ui_stop - ui_start),
            "maximum_notification_gap_ms": observation["maximum_notification_gap_ms"]},
        "motion": {"unique_workload_pulses": len(pulses), "finite_indicator_position_samples": len(positions),
            "indicator_x_minimum": min(values) if values else None, "indicator_x_maximum": max(values) if values else None,
            "indicator_x_range": motion_range, "positions": positions,
            "final_indicator_x": ui.get("indicator_x"), "final_reduced_motion": ui.get("reduced_motion"),
            "scope": "Saved low-frequency GUI property observations inside the workload; not per-frame visual or physical presentation validation"},
        "existing_ui_coverage": coverage, "existing_audio_coverage": result["audio_coverage"],
        "existing_M0_acceptance": result["acceptance"], "continuous_timing": result["timing"]["continuous"],
        "controls": {"recorded_count": len(controls), "expected_for_hour": 11,
                     "scope": "Recorded outcomes; full request/generation and event audits remain separate"},
        "frozen_pins_against_prior_vulkan_hour": frozen, "ui_pin_intentionally_new": result["pins"]["ui"],
        "fixture_sha256": result["fixture"]["sha256"], "comparison": comparison,
        "scope": "Saved completed evidence only. No host commands, new workload, PCM recomputation, profile mutation or new latency gate.",
        "inputs": inputs,
    }
    source = Path(__file__).resolve().read_bytes()
    report["audit_source_sha256"] = hashlib.sha256(source).hexdigest()
    output = args.output or BASE / (args.name + "-cadence-audit.json")
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"output": str(output), "status": report["status"],
                      "complete_minute_fps_range": [min(fps), max(fps)] if fps else None,
                      "indicator_x_range": motion_range, "failed_checks": report["failed_checks"]}))
    return 0 if report["status"] == "valid" else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Audit closed saved music/audio/UI trials; no host queries or PCM replay."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import gzip
import hashlib
import importlib.util
import json
import math
from pathlib import Path

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
OLD = ROOT / "benchmarks-local/software-ui-2026-10-17/completed-hour-audit.py"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", help="Closed run label beneath this helper")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--source-snapshot", type=Path, default=ROOT / "tools/benchmarks/stem_soak.py")
    parser.add_argument("--baseline-events", type=Path,
                        help="Optional retained original Vulkan events (plain or gzip)")
    args = parser.parse_args()
    if Path(args.name).name != args.name or args.name in (".", ".."):
        parser.error("name must be one plain run label")
    inputs, checks = {}, {}

    def read(path, lines=False):
        if not path.exists() and lines:
            path = path.with_name(path.name + ".gz")
        data = path.read_bytes()
        inputs[str(path)] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        raw = gzip.decompress(data) if path.suffix == ".gz" else data
        if lines:
            inputs[str(path)].update(decoded_bytes=len(raw), decoded_sha256=hashlib.sha256(raw).hexdigest())
        return [json.loads(line) for line in raw.splitlines() if line.strip()] if lines else json.loads(raw)

    def check(name, condition):
        checks[name] = checks.get(name, True) and bool(condition)

    def near(left, right):
        return math.isclose(left, right, rel_tol=1e-10, abs_tol=1e-6)

    expected = read(BASE / "expected-pins.json")
    corpus = read(BASE / "corpus-pins.json")
    run = BASE / args.name
    launch = read(BASE / (args.name + "-launch.json"))
    result = read(run / "result.json")
    if not launch.get("finished_at_utc") or not result.get("finished_at_utc"):
        raise RuntimeError("Refusing to read traces from an unfinished trial")
    if expected.get("status") != "ready" or not expected.get("repository_head"):
        raise RuntimeError("Expected signed source pins are not armed")
    source = args.source_snapshot.read_bytes()
    inputs[str(args.source_snapshot)] = {"bytes": len(source), "sha256": hashlib.sha256(source).hexdigest()}
    check("closed_successful_launch", launch.get("status") == "completed" and launch.get("returncode") == 0
          and not launch.get("failures"))
    check("successful_workload", result["status"] == "ok" and not result["failures"])
    successful = result["status"] == "ok"
    check("signed_source_pins", launch["expected_producer"] == expected
          and result["repository_head"] == expected["repository_head"]
          and set(result["pins"]) == set(expected["pins"])
          and all(result["pins"][key]["sha256"] == value for key, value in expected["pins"].items())
          and inputs[str(args.source_snapshot)]["sha256"] == expected["pins"]["soak_source"]
          and launch["pins"]["expected_pins_file"]["sha256"] == inputs[str(BASE / "expected-pins.json")]["sha256"])
    spec = importlib.util.spec_from_file_location("music_saved_audit", OLD)
    saved = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(saved)
    inputs[str(OLD)] = {"bytes": OLD.stat().st_size, "sha256": saved.sha(OLD)}
    events = read(run / "events.jsonl", lines=True)
    hops = [event for event in events if event["event"] == "soak_hop"]
    chunks = [event for event in events if event["event"] == "chunk"]
    controls = result["controls"]
    begin, end = result["workload_started_monotonic"], result["workload_stopped_monotonic"]
    if begin is None or end is None:
        raise RuntimeError("No saved workload interval; this is an admission failure, not a completed audit")
    requested = result["requested_seconds"]
    check("requested_configuration", requested in (90, 3600) and launch["seconds"] == requested
          and result["control_interval_seconds"] == launch["control_interval_seconds"] == (20 if requested == 90 else 300)
          and result["source_mode"] == "prepared_wav_corpus"
          and result["load_mode"] == "both" and set(result["selected_loads"]) == {"audio", "ui"})
    check("wall_duration", end - begin == result["workload_seconds"] >= requested)
    tracks = {track["input"]["sha256"]: track for track in corpus["tracks"]}
    ordered = list(tracks)
    by_path = {track["input"]["path"]: source_id for source_id, track in tracks.items()}
    check("four_pinned_sources_in_order", len(tracks) == 4
          and [item["sha256"] for item in result["sources"]] == ordered
          and all(all(source[key] == tracks[source["sha256"]]["input"][key]
                      for key in tracks[source["sha256"]]["input"])
                  for source in result["sources"])
          and launch["inputs"] == {track["id"]: track["input"] for track in corpus["tracks"]})
    references = {sid: {row["window_start_frame"]: row for row in track["continuous_hop_references"]}
                  for sid, track in tracks.items()}
    check("sixteen_baseline_references", sum(map(len, references.values())) == 16
          and all(set(rows) == {0, 257985, 515970, 773955} for rows in references.values()))
    baseline_path = args.baseline_events or Path(corpus["original_Vulkan_events"]["path"])
    baseline_events = read(baseline_path, lines=True)
    if not baseline_path.exists():
        baseline_path = baseline_path.with_name(baseline_path.name + ".gz")
    baseline_chunks = [row for row in baseline_events if row["event"] == "chunk" and row.get("scenario") == "continuous"]
    check("baseline_reference_provenance", inputs[str(baseline_path)]["decoded_sha256"] == corpus["original_Vulkan_events"]["sha256"]
          and len(baseline_chunks) == 16 and all(
              all(row[field] == references[row["source_id"]][row["window_start_frame"]][field]
                  for field in ("frame_start", "frames", "window_start_frame", "samples_sha256", "source_id", "model_id"))
              for row in baseline_chunks)
          and all({key: row[key] for key in ("mean", "std", "source_frames")} == tracks[row["source_id"]]["normalization"]
                  for row in baseline_events if row["event"] == "source_ready"))
    model = expected["pins"]["model"]
    active = current = None
    next_frame = start_frame = None
    completed = False
    load_index = -1
    raw_controls, started, discarded, prepared = {}, {}, {}, {}
    chunk_by_key, hop_by_key = {}, {}
    counts = {sid: Counter() for sid in tracks}
    completions = {sid: {"full": 0, "resumed": 0} for sid in tracks}
    normalization, grid_hashes, first_output = {}, {sid: {} for sid in tracks}, []
    trimmed_parity = []
    previous_generation = 0
    for event in events:
        kind = event["event"]
        if kind == "control":
            generation = event["generation"]
            check("generation_controls_monotonic", generation == previous_generation + 1 and generation not in raw_controls)
            previous_generation = generation
            raw_controls[generation] = event
            sid = by_path.get(event["source_path"])
            check("control_source_pinned", sid is not None)
            if event["command"] == "load":
                check("rotate_only_after_complete", load_index < 0 or completed)
                load_index += 1
                check("round_robin_load_order", sid == ordered[load_index % len(ordered)] and event["frame_start"] == 0)
            else:
                check("cancel_seek_preserves_source", sid == current and event["command"] in ("cancel", "seek"))
            current = sid
            active = generation if event["command"] != "cancel" else None
            next_frame = start_frame = event["frame_start"]
            completed = False
        elif kind == "source_ready":
            sid = event["source_id"]
            check("source_ready_current_identity", sid == current and event["generation"] == active
                  and event["source_path"] == tracks[sid]["input"]["path"])
            values = {key: event[key] for key in ("mean", "std", "source_frames")}
            check("normalization_matches_prior_music", values == tracks[sid]["normalization"])
            normalization[sid] = values
            prepared[event["generation"]] = event
            counts[sid]["source_ready"] += 1
        elif kind == "started":
            key = (event["generation"], event["request_id"])
            check("started_current_and_unique", key not in started and event["generation"] == active
                  and event["source_id"] == current)
            started[key] = event
        elif kind == "chunk":
            sid, key = event["source_id"], (event["generation"], event["request_id"])
            check("chunk_currentness_and_coverage", key not in chunk_by_key and key in started
                  and event["generation"] == active and sid == current and event["frame_start"] == next_frame
                  and event["model_id"] == model and event["source_frames"] == tracks[sid]["input"]["frames"]
                  and event["sample_rate"] == 44100 and event["channels"] == 2
                  and 0 < event["frames"] <= 257985 and event["ready_queue_depth"] <= 2)
            check("chunk_geometry", event["window_start_frame"] % 257985 == 0
                  and event["frames"] == min(event["window_start_frame"] + 257985,
                                               tracks[sid]["input"]["frames"]) - event["frame_start"])
            next_frame = event["frame_start"] + event["frames"]
            chunk_by_key[key] = event
            counts[sid]["chunks"] += 1
            if not any(previous["generation"] == event["generation"] for previous in first_output):
                command = raw_controls[event["generation"]]
                first_output.append({"generation": event["generation"], "source_id": sid,
                                     "command": command["command"], "target_frame": command["frame_start"],
                                     "control_to_first_ready_ms": (event["ready_at"] - command["timestamp"]) * 1000,
                                     "preparation_ms": prepared[event["generation"]]["preparation_ms"],
                                     "first_hop_service_ms": event["hop_service_ms"]})
        elif kind == "soak_hop":
            sid, key = event["source_id"], (event["generation"], event["request_id"])
            chunk = chunk_by_key.get(key, {})
            check("hop_matches_raw_chunk", key not in hop_by_key and bool(chunk)
                  and all(event[field] == chunk[field] for field in
                          ("source_id", "frame_start", "frames", "window_start_frame", "ready_at", "request_ms",
                           "source_read_ms", "overlap_ms", "hop_service_ms", "control_latency_ms", "queue_delay_ms", "metrics", "samples_sha256"))
                  and event["elapsed_seconds"] == event["ready_at"] - begin
                  and event["above_target"] == (event["hop_service_ms"] > 4095))
            hop_by_key[key] = event
            trimmed = event["frame_start"] != event["window_start_frame"]
            parity = event["parity"]
            if trimmed:
                check("trimmed_seek_parity", event["kind"] == "seek_first" and parity["kind"] == "trimmed_seek"
                      and parity["numeric_parity"] is True
                      and isinstance(parity["exact"], bool) and math.isfinite(parity["max_abs_difference"])
                      and parity["max_abs_difference"] >= 0
                      and parity["rtol"] == 1e-4 and parity["atol"] == 1e-5)
                trimmed_parity.append({"generation": event["generation"], "source_id": sid, **parity})
                counts[sid]["trimmed_seek"] += 1
            else:
                check("recorded_same_grid_parity_exact", parity["exact"] is True)
                ref = references[sid][event["window_start_frame"]]
                check("same_grid_matches_prior_music", event["samples_sha256"] == ref["samples_sha256"]
                      and event["frames"] == ref["frames"] and event["kind"] == "continuous")
                grid_hashes[sid][str(event["window_start_frame"])] = event["samples_sha256"]
                counts[sid]["same_grid_repeats"] += parity["kind"] == "same_grid_sha256"
        elif kind == "complete":
            sid = event["source_id"]
            check("complete_has_full_current_coverage", event["generation"] == active and sid == current
                  and event["frame_start"] == next_frame == tracks[sid]["input"]["frames"])
            completions[sid]["full" if start_frame == 0 else "resumed"] += 1
            completed = True
        elif kind == "discarded":
            key = (event["generation"], event["request_id"])
            check("discarded_request_started_once", key not in discarded and key in started
                  and event["source_id"] == started[key]["source_id"])
            discarded[key] = event
        elif kind in ("error", "parity_failure"):
            check("no_raw_errors_or_parity_failures", False)
    check("raw_chunks_match_hops", set(chunk_by_key) == set(hop_by_key) and len(chunks) == len(hops))
    check("saved_source_maps_match_raw", set(result["reference_hashes_by_source"]) <= set(tracks)
          and all(rows == grid_hashes[sid] for sid, rows in result["reference_hashes_by_source"].items())
          and all(not rows or sid in result["reference_hashes_by_source"] for sid, rows in grid_hashes.items())
          and result["source_normalization_by_source"] == normalization and result["source_completions"] == completions
          and result["full_source_loops"] == sum(value["full"] for value in completions.values())
          and result["resumed_source_completions"] == sum(value["resumed"] for value in completions.values()))
    if successful and requested == 3600:
        check("all_four_sources_delivered_completed_repeated", all(counts[sid]["chunks"] > 0
              and completions[sid]["full"] > 0 and counts[sid]["same_grid_repeats"] > 0 for sid in tracks))
    elif successful:
        check("smoke_has_overall_repeat_parity", sum(value["same_grid_repeats"] for value in counts.values()) > 0)
    check("control_count", len(controls) == 11 if requested == 3600 else len(controls) >= 1)
    check("exact_discarded_request_set", set(discarded) == {(row["superseded_generation"], row["superseded_request_id"]) for row in controls})
    check("exact_cancel_and_seek_generation_sets", {gen for gen, row in raw_controls.items() if row["command"] == "cancel"}
          == {row["superseded_generation"] + 1 for row in controls}
          and {gen for gen, row in raw_controls.items() if row["command"] == "seek"} == {row["generation"] for row in controls})
    for index, control in enumerate(controls):
        key = (control["superseded_generation"], control["superseded_request_id"])
        active_request, dropped = started[key], discarded[key]
        cancel, seek = raw_controls[key[0] + 1], raw_controls[control["generation"]]
        first = next(hop for hop in hops if hop["generation"] == control["generation"])
        began = begin + control["at_workload_seconds"]
        resumed = first["ready_at"] - control["ready_after_resume_ms"] / 1000
        check("active_cancel_identity_and_timing", control["complete"] and control["held_chunk_rejected"]
              and control["post_cancel_chunks"] == 0 and control["hardware_submission_confirmed"] is False
              and control["source_id"] == active_request["source_id"] == first["source_id"]
              and by_path[cancel["source_path"]] == by_path[seek["source_path"]] == control["source_id"]
              and control["generation"] == key[0] + 2
              and control["target_frame"] == seek["frame_start"] == (257985 + 333 if index % 2 == 0 else 515970 + 137)
              and control["started_to_cancel_ms"] >= 150
              and near(control["started_to_cancel_ms"], (began - active_request["timestamp"]) * 1000)
              and began <= cancel["timestamp"] <= began + control["cancel_ack_ms"] / 1000 + 1e-6
              and near(control["residual_finish_ms"], (dropped["timestamp"] - began) * 1000)
              and resumed <= seek["timestamp"] + 1e-6 <= resumed + control["resume_ack_ms"] / 1000 + 2e-6
              and near(control["ready_after_cancel_ms"], (first["ready_at"] - began) * 1000)
              and control["delivered_after_cancel_ms"] >= control["ready_after_cancel_ms"]
              and control["parity"] == first["parity"])
    stats = result["worker_statistics"]
    runtime = [event for event in events if event["event"] == "runtime_ready"]
    check("one_persistent_bounded_closed_worker", stats["closed"] and stats["backend_start_count"] == len(runtime) == 1
          and runtime[0]["runtime"]["threads"] == 4 and runtime[0]["runtime"]["backend"] == "vulkan"
          and runtime[0]["runtime"]["fence_wait_mode"] == "blocking"
          and stats["cancel_count"] == stats["discarded_count"] == len(controls)
          and stats["telemetry_dropped"] == result["stale_chunks_delivered"] == 0
          and stats["ready_high_water"] <= stats["max_ready"] == 2)
    check("parity_counter_reductions", result["same_grid_hash_checks"] == sum(value["same_grid_repeats"] for value in counts.values())
          and result["trimmed_seek_checks"] == sum(value["trimmed_seek"] for value in counts.values()) == len(controls))
    continuous = [row for row in hops if row["kind"] == "continuous"]
    timing = {"all_delivered": saved.timing(hops), "continuous": saved.timing(continuous), "bins_300_seconds": []}
    for key in ("all_delivered", "continuous"):
        check("timing_recomputed", all(result["timing"][key][field] == value for field, value in timing[key].items()))
    for interval in result["timing"]["bins_300_seconds"]:
        subset = saved.timing([row for row in hops if interval["start_seconds"] <= row["elapsed_seconds"] < interval["end_seconds"]])
        check("timing_bins_recomputed", all(interval[field] == value for field, value in subset.items()))
        timing["bins_300_seconds"].append({"start_seconds": interval["start_seconds"], "end_seconds": interval["end_seconds"], **subset})
    p99 = timing["continuous"]["hop_service_percentiles_ms"].get("99")
    latency_pass = p99 is not None and p99 < 4095
    check("reported_latency_boolean_matches", result["acceptance"]["observed_continuous_p99_within_target"] == latency_pass)
    audio, ui = read(run / "audio.json"), read(run / "ui.json")
    for name, report in (("audio", audio), ("ui", ui)):
        check("saved_probe_reports_and_coverage", result["load"]["children"][name]["returncode"] == 0
              and result["load"]["children"][name]["report"] == report and result[name + "_coverage"]["passed"])
    audio_start, audio_stop = audio["streaming_started_monotonic_ns"] / 1e9, audio["stopped_monotonic_ns"] / 1e9
    overlap = max(0, min(end, audio_stop) - max(begin, audio_start))
    check("silent_audio_format_and_full_overlap", audio["status"] == "ok" and audio["signal"] == "silence"
          and audio["requested"]["sample_rate"] == audio["negotiated"]["sample_rate"] == 48000
          and audio["requested"]["channels"] == audio["negotiated"]["channels"] == 2
          and audio["requested"]["quantum_frames"] == 256 and audio["negotiated"]["format_valid"] is True
          and audio["callbacks"] * 256 == audio["frames_rendered"]
          and all(audio["negotiated"][key] == 256 for key in ("stream_frames_min", "stream_frames_max", "graph_quantum_min", "graph_quantum_max"))
          and overlap == result["workload_seconds"] == result["audio_coverage"]["concurrent_workload_seconds"]
          and all(audio[key] == 0 for key in saved.FAULTS))
    monitor = read(BASE / (args.name + "-AC-lid-monitor.jsonl"), lines=True)
    snapshots = [row for row in monitor if row.get("event") == "snapshot"]
    pulses = {row["ui_pulse"]["timestamp_monotonic_ns"]: row["ui_pulse"] for row in snapshots
              if isinstance(row.get("ui_pulse"), dict) and begin <= row["ui_pulse"]["timestamp_monotonic_ns"] / 1e9 <= end}
    positions = [pulse["indicator_x"] for pulse in pulses.values() if isinstance(pulse.get("indicator_x"), (int, float))
                 and not isinstance(pulse["indicator_x"], bool) and math.isfinite(pulse["indicator_x"])]
    observation = ui["frame_observation"]
    ui_start, ui_stop = observation["started_monotonic_ns"] / 1e9, observation["stopped_monotonic_ns"] / 1e9
    bins = observation["frames_per_60_seconds"]
    complete = [index for index in range(len(bins)) if ui_start + (index + 1) * 60 <= ui_stop
                and ui_start + index * 60 < end and ui_start + (index + 1) * 60 > begin]
    fps = [bins[index] / 60 for index in complete]
    coverage = result["ui_coverage"]
    check("software_wayland_cadence_geometry", launch["renderer"] == result["renderer"] == "software"
          and ui["platform"] == "wayland" and ui["graphics_api"] == 1 and ui["rendered"] and ui["capture_ok"]
          and ui["repaint_interval_ms"] == launch["repaint_ms"] == 33 and ui["animation_driver"] == "qtimeline"
          and (ui["width"], ui["height"], ui["device_pixel_ratio"]) == (640, 400, 2)
          and launch["UI_diagnostic_environment"]["CONFLUX_M0_REPAINT_MS"] == "33"
          and launch["diagnostic_ui_logging"] is False
          and launch["UI_diagnostic_environment"]["QT_LOGGING_RULES"] == "qt.scenegraph.time.*=false")
    check("sampled_motion_and_zero_exposure_loss", len(positions) >= 2 and max(positions) > min(positions)
          and ui["reduced_motion"] is False and ui["exposed"] is True
          and coverage["progress"]["unexposed_observations"] == 0
          and all(pulse.get("exposed") is True and pulse.get("reduced_motion") is False
                  and pulse.get("animation_driver") == "qtimeline" and pulse.get("repaint_interval_ms") == 33 for pulse in pulses.values()))
    check("ui_full_observation_and_measured_cadence", ui_start <= begin and ui_stop >= end
          and sum(bins) == ui["frames_swapped"] and complete == coverage["complete_minute_bins_intersecting_workload"]
          and len(complete) >= math.floor(requested / 60) and bool(fps) and all(25 <= value <= 40 for value in fps)
          and coverage["passed"] and all(coverage["checks"].values()))
    check("sampled_ac_lid_ready", bool(snapshots) and all(row["AC_lid"]["ready"] for row in snapshots)
          and launch["host_conditions_before_admission"]["ready"] and launch["admission"]["ready"])
    guard, policy = read(BASE / (args.name + "-guard.json")), read(BASE / (args.name + "-policy.json"))
    check("guard_observer_cleanup", not launch["cleanup"]["remaining_owned_processes"] and launch["cleanup"]["last_child_reaped"]
          and guard["status"] == "ok" and not guard["errors"] and guard["idle_inhibit_released"] and guard["script_unloaded"] and guard["restored"]
          and policy["mode"] == "observe" and policy["profile_mutation"] is False and policy["restored_by_runner"] is False
          and policy["runner_returncode"] == policy["benchmark_returncode"] == 0)
    ranges = {key: {"min": min(row[key] for row in controls), "max": max(row[key] for row in controls)} for key in
              ("started_to_cancel_ms", "cancel_ack_ms", "residual_finish_ms", "resume_ack_ms", "ready_after_resume_ms", "ready_after_cancel_ms", "delivered_after_cancel_ms")} if controls else {}
    report = {"schema": 1, "run": args.name, "audited_at_utc": datetime.now(timezone.utc).isoformat(),
              "status": "valid" if all(checks.values()) else "invalid", "checks": checks,
              "failed_checks": [key for key, value in checks.items() if not value], "timing": timing,
              "latency_qualification": {"target_ms": 4095, "continuous_p99_ms": p99, "observed_within_target": latency_pass,
                                        "scope": "Separate from functional validity; strict-below comparison matches the existing harness. Consecutive repeated windows are not independent tail samples."},
              "controls": {"count": len(controls), "nominal_smoke": 2, "required_smoke_minimum": 1, "required_hour": 11,
                           "ranges_ms": ranges, "discarded_events": len(discarded), "hardware_submission_confirmed": False,
                           "trimmed_parity": {"count": len(trimmed_parity),
                                              "nonexact_count": sum(not row["exact"] for row in trimmed_parity),
                                              "max_abs_difference": max((row["max_abs_difference"] for row in trimmed_parity), default=None),
                                              "records": trimmed_parity,
                                              "scope": "Recorded allclose(rtol=1e-4, atol=1e-5) assertions; exact equality and maximum difference are descriptive, without retained-PCM recomputation."},
                           "scope": "Logical cancellation only; supervisor start is not a GPU submission timestamp. Recorded return instants are not separately reconstructible."},
              "sources": [{"id": track["id"], "source_id": sid, "counts": dict(counts[sid]), "completions": completions[sid],
                           "timing": saved.timing([row for row in continuous if row["source_id"] == sid])} for sid, track in tracks.items()],
              "source_admission_first_output": first_output,
              "admission_timing_scope": "Control-to-first-ready includes preparation and first inference; hop service excludes whole-source identity/statistics preparation. Loads rotate complete files without concatenating PCM.",
              "event_counts": dict(Counter(event["event"] for event in events)), "worker": stats,
              "audio": {"callbacks": audio["callbacks"], "faults": {key: audio[key] for key in saved.FAULTS},
                        "callback_elapsed_us": audio["callback_elapsed_us"], "workload_overlap_seconds": overlap, "scope": audio["measurement_scope"]},
              "UI": {"frames": ui["frames_swapped"], "complete_minute_indices": complete, "complete_minute_fps": fps,
                     "maximum_notification_gap_ms": observation["maximum_notification_gap_ms"], "motion_samples": len(positions),
                     "indicator_x_range": [min(positions), max(positions)] if positions else None,
                     "scope": "GUI-thread notifications and sampled properties, not physical presentation."},
              "AC_lid": {"samples": len(snapshots), "scope": "Sampled readiness only, no unsampled continuity claim."},
              "inputs": inputs, "audit_source_sha256": saved.sha(Path(__file__)),
              "scope": "Saved completed/closed trial evidence only; baseline hashes and recorded exact parity are checked without PCM recomputation. Four repeated23.4s excerpts, not whole-track performance or stem playback. No new thermal pass gate; retained thermal snapshots are audited separately."}
    output = args.output or BASE / (args.name + "-audit.json")
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"output": str(output), "status": report["status"], "failed_checks": report["failed_checks"],
                      "continuous": timing["continuous"], "controls": len(controls)}))
    return 0 if report["status"] == "valid" else 1


if __name__ == "__main__":
    raise SystemExit(main())

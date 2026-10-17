#!/usr/bin/env python3
"""Audit completed ABBA native replay records; no inference or host queries."""
from collections import Counter
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import re
import statistics

import profile_music as pm

BASE = Path(__file__).resolve().parent
RUN = BASE / "abba-48-attempt3"
TARGET = 4095.0


def main():
    output = BASE / "replay-facts.json"
    assert not output.exists(), "Use a fresh facts output"
    paths = {n: RUN / n for n in ("result.json", "requests.jsonl", "phases.jsonl", "native-trace.jsonl",
        "fresh-admission.json", "admission-normal-ready.json", "admission-normal-ready.jsonl")}
    paths["guard"] = BASE / "replay-guard-attempt3.json"
    paths["original_vulkan_events"] = BASE.parent / "user-tracks-2026-10-17/vulkan/events.jsonl"
    r, admission, proof, guard = [pm.load(paths[n]) for n in ("result.json", "fresh-admission.json", "admission-normal-ready.json", "guard")]
    requests, phases, trace, readiness_rows = [pm.lines(paths[n]) for n in ("requests.jsonl", "phases.jsonl", "native-trace.jsonl", "admission-normal-ready.jsonl")]
    assert r["status"] == "ok" and not r["failures"] and r["native_started"] and not r["prepare_only"]
    assert guard["status"] == "ok" and guard["returncode"] == guard["child_returncode"] == 0 and not guard["errors"] and guard["idle_inhibit_released"]
    assert r["backend"] == "vulkan" and r["threads"] == 4 and r["planned_requests"] == r["completed_requests"] == len(requests) == 48
    assert r["requests"] == requests and r["order"] == [x for _ in range(12) for x in ("synthetic", "music", "music", "synthetic")]
    expected = {"runtime": "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6",
        "model": "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6",
        "driver": "72d202f04fd787ab8f0871854c8c6feeb8dc6aa4f00c2cbb84359389779441ee",
        "worker_source": "308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8",
        "sampler_source": "8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b",
        "shared_benchmark_source": "87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7",
        "replay_source": "6ee7408415fdf0366cd3282fe57f94409d8f9bc34eb2967eb0e622bf2cf1757e",
        "readiness_source": "4e94000d5966357c653a6ef64729f9b513df723ce0badb17e496d4b85c07135f",
        "music_source": "3448e91b9af700e250a73c752ce9ccff2d413add50ad46c928cb9728ea3ff1cf"}
    assert {k: p["sha256"] for k, p in r["pins"].items()} == expected
    assert r["pins"]["driver"] == pm.pin(BASE / "interleaved_replay.py")
    original_events = pm.lines(paths["original_vulkan_events"])
    original_source = [e for e in original_events if e["event"] == "source_ready" and e["scenario"] == "continuous" and e["track_id"] == "when_the_light_breaks"]
    assert len(original_source) == 1
    original_source = original_source[0]
    original_tail = [e for e in original_events if e["event"] == "chunk" and e["scenario"] == "continuous" and e["track_id"] == "when_the_light_breaks" and e["window_start_frame"] == r["inputs"]["music"]["window_start_frame"]]
    assert len(original_tail) == 1
    original_tail = original_tail[0]
    assert original_source["mean"] == r["inputs"]["music"]["normalization"]["mean"] and original_source["std"] == r["inputs"]["music"]["normalization"]["std"]
    assert original_source["source_id"] == original_tail["source_id"] == r["inputs"]["music"]["source"]["sha256"]
    assert original_source["source_frames"] == r["inputs"]["music"]["source"]["frames"]
    assert original_tail["source_frames"] - original_tail["window_start_frame"] == r["inputs"]["music"]["normalization"]["valid_frames"] == 257985
    assert r["admission"] == admission and r["fresh_admission_capture"] == pm.pin(paths["fresh-admission.json"])
    assert proof["status"] == "matched" and proof["rows"] == readiness_rows and proof["required_consecutive_sample_span_seconds"] == 60
    assert proof["package_threshold_millidegrees_C"] == 55000 and not proof["profile_mutation"] and not proof["service_mutation"]
    assert proof["rows"][-1]["consecutive_ready_sample_span_seconds"] >= 60
    assert r["normal_readiness"]["returncode"] == 0 and r["normal_readiness"]["after_preparation"]
    assert r["normal_readiness"]["proof"] == admission["proof"] == pm.pin(paths["admission-normal-ready.json"])
    live = admission["live_normal_readiness"]
    assert admission["ready"] and not live["errors"] and live["profile"] == "balanced" and live["holds"] == ""
    assert live["latest_message_claims_recovered"] and live["journal_identity_matches_service"]
    assert admission["same_guard_service_identity"] == proof["guard_service_identity"]
    assert {k: live["service_identity"][k] for k in ("MainPID", "InvocationID")} == proof["guard_service_identity"]
    assert 0 <= live["package_millidegrees_C"] <= admission["fresh_package_limit_millidegrees_C"] == 70000
    assert admission["completed_readiness_package_limit_millidegrees_C"] == 55000
    for snapshot in (admission["AC_lid"], r["AC_lid_before_first_request"]):
        assert snapshot["ready"] and not snapshot["errors"] and snapshot["AC"] == "1" and snapshot["lid"] == "b false"
    ready = r["runtime_ready"]
    assert ready["pid"] == r["native_pid"] == r["cleanup"]["native_pid"]
    assert ready["backend"] == "vulkan" and ready["threads"] == 4 and ready["fence_wait_mode"] == "blocking" and ready["weight_storage"] == "native"
    assert ready["process_environment"] == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"}
    assert r["cleanup"]["process_returncode"] is not None and r["cleanup"]["temporary_directory_gone"] and not r["cleanup"]["PID_exists_after_close"]
    assert r["replay_seconds"] == r["replay_stopped_monotonic"] - r["replay_started_monotonic"] <= r["post_ready_budget_seconds"] == 300
    assert all(a["timestamp_monotonic"] <= b["timestamp_monotonic"] for a, b in zip(phases, phases[1:]))
    phases_by_name = {n: [p for p in phases if p["phase"] == n] for n in {p["phase"] for p in phases}}
    assert [phases_by_name[n][0]["timestamp_monotonic"] for n in ("preparation_started", "preparation_finished", "normal_readiness_started", "normal_readiness_finished", "native_spawn", "native_ready", "driver_finished")] == sorted(phases_by_name[n][0]["timestamp_monotonic"] for n in ("preparation_started", "preparation_finished", "normal_readiness_started", "normal_readiness_finished", "native_spawn", "native_ready", "driver_finished"))
    assert admission["finished_monotonic"] <= phases_by_name["native_spawn"][0]["timestamp_monotonic"]
    assert len(phases_by_name["request_started"]) == len(phases_by_name["request_finished"]) == 48
    resources = r["resources"]
    assert len(trace) == resources["samples"] == resources["trace"]["samples_written"]
    assert not resources["sample_errors"] and not resources["trace"]["errors"]
    assert len({(x["pid"], x["start_ticks"]) for x in trace}) == 1 and trace[0]["pid"] == ready["pid"]
    assert all(a["timestamp_monotonic"] < b["timestamp_monotonic"] for a, b in zip(trace, trace[1:]))
    sample_fields = resources["system_sampling"]["fields"]
    fields = {}
    for label, suffix in (("gpu_act_MHz", "/gt_act_freq_mhz"), ("gpu_cur_MHz", "/gt_cur_freq_mhz"), ("cpu0_kHz", "/cpu0/cpufreq/scaling_cur_freq")):
        candidates = [p for p in sample_fields if p.endswith(suffix)]
        assert len(candidates) == 1
        fields[label] = {"path": candidates[0], "unit": sample_fields[candidates[0]]}
    for p, unit in sample_fields.items():
        if unit == "millidegrees_C":
            labels = {r[k][str(Path(p).parent)]["type"] for k in ("conditions_before_admission", "conditions_at_finish")}
            assert len(labels) == 1
            fields[labels.pop()] = {"path": p, "unit": unit}
    profiled = []
    for index, row in enumerate(requests):
        label = r["order"][index]
        assert row["request_id"] == row["metrics"]["id"] == index + 1 and row["input_label"] == label
        assert row["input_sha256"] == r["inputs"][label]["normalized_sha256"]
        assert row["matches_first_output_for_input"] and row["output_sha256"] == r["reference_output_sha256"][label] and re.fullmatch("[a-f0-9]{64}", row["output_sha256"])
        assert row["metrics"]["frames"] == 343980 and row["metrics"]["stems"] == 4 and row["metrics"]["event"] == "result"
        assert row["round_trip_ms"] == (row["finished_monotonic"] - row["started_monotonic"]) * 1000
        assert 0 < row["effective_timeout_seconds"] <= r["request_timeout_seconds"] == 30
        a, z = phases_by_name["request_started"][index], phases_by_name["request_finished"][index]
        assert a["request_id"] == z["request_id"] == index + 1 and a["input_label"] == z["input_label"] == label
        assert a["timestamp_monotonic"] <= row["started_monotonic"] <= row["finished_monotonic"] <= z["timestamp_monotonic"]
        assert r["replay_started_monotonic"] <= row["started_monotonic"] <= row["finished_monotonic"] <= r["replay_stopped_monotonic"]
        assert not index or requests[index-1]["finished_monotonic"] <= row["started_monotonic"]
        ids = [i for i, x in enumerate(trace) if row["started_monotonic"] <= x["capture_started_monotonic"] and x["timestamp_monotonic"] <= row["finished_monotonic"]]
        assigned = [i for i, x in enumerate(trace) if row["started_monotonic"] <= x["timestamp_monotonic"] <= row["finished_monotonic"]]
        interior = [trace[i] for i in ids]
        assert interior
        sampled = pm.field_stats(interior, fields)
        profiled.append({"request_id": index + 1, "input_label": label, "block": index // 4, "position_in_block": index % 4,
            "round_trip_ms": row["round_trip_ms"], "graph_ms": row["metrics"]["graph_ms"],
            "round_trip_minus_graph_ms": row["round_trip_ms"] - row["metrics"]["graph_ms"],
            "started_monotonic": row["started_monotonic"], "finished_monotonic": row["finished_monotonic"],
            "interior_trace_rows": ids, "timestamp_assigned_trace_rows": assigned,
            "policy_counts": dict(Counter(pm.policy(x) for x in interior)),
            "sampled": {k: {n: v for n, v in s.items() if n in ("count", "min", "mean", "max", "unit", "missing_samples")} for k, s in sampled.items()}})
    summaries = {}
    for label in ("synthetic", "music"):
        selected = [x for x in requests if x["input_label"] == label]
        assert len(selected) == 24 and len({x["output_sha256"] for x in selected}) == 1
        summaries[label] = {"requests": 24, "output_sha256": selected[0]["output_sha256"]}
        for key, source in (("round_trip_ms", lambda x: x["round_trip_ms"]), ("graph_ms", lambda x: x["metrics"]["graph_ms"])):
            summary = pm.stats([source(x) for x in selected])
            summaries[label][key] = {**summary, "above_4095ms": sum(source(x) > TARGET for x in selected)}
            assert r["per_input"][label]["round_trip_percentiles_ms" if key == "round_trip_ms" else "graph_percentiles_ms"] == {str(p): summary[f"p{p}"] for p in (50, 95, 99)}
    blocks = []
    adjacent = {"round_trip_ms": [], "graph_ms": []}
    for index in range(12):
        group = profiled[index*4:index*4+4]
        block = {"block": index, "request_ids": [x["request_id"] for x in group]}
        for key in adjacent:
            delta = (group[1][key] + group[2][key] - group[0][key] - group[3][key]) / 2
            block[key] = {"music_mean_minus_synthetic_mean_ms": delta,
                "forward_adjacent_music_minus_synthetic_ms": group[1][key] - group[0][key],
                "reverse_adjacent_music_minus_synthetic_ms": group[2][key] - group[3][key]}
            adjacent[key].extend((group[1][key] - group[0][key], group[2][key] - group[3][key]))
        blocks.append(block)
    module_path = BASE.parent / "user-tracks-2026-10-17/summarize_runs.py"
    spec = importlib.util.spec_from_file_location("saved_thermal_spans", module_path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    thermal = {}
    for label, field in fields.items():
        if field["unit"] == "millidegrees_C":
            points = [(x["timestamp_monotonic"] - r["replay_started_monotonic"], x["system_values"][field["path"]] / 1000) for x in trace if field["path"] in x["system_values"]]
            thermal[label] = {"path": field["path"], "samples": len(points), "min_C": min(x[1] for x in points), "peak_C": max(x[1] for x in points),
                "thresholds": {str(t): {"hot_samples": sum(x[1] >= t for x in points), "episodes": module.spans(points, lambda x,t=t: x[1] >= t)} for t in (85,95)}}
    facts = {"schema": 1, "status": "ok", "analyzed_at_utc": datetime.now(timezone.utc).isoformat(), "analysis_source": pm.pin(Path(__file__).resolve()),
        "dependencies": [pm.pin(BASE / "profile_music.py"), pm.pin(module_path)], "inputs": {k: pm.pin(p) for k,p in paths.items()},
        "captured_producer_pins": r["pins"], "normalized_inputs": r["inputs"], "native_settings": ready,
        "original_worker_music_normalization_binding": {"source_ready": original_source,
            "same_source_hash_mean_std_and_window_start": True, "original_request_id": original_tail["request_id"],
            "window_start_frame": original_tail["window_start_frame"], "valid_frames": 257985, "zero_pad_frames": 85995,
            "original_hop_service_ms": original_tail["hop_service_ms"], "original_native_graph_ms": original_tail["metrics"]["graph_ms"],
            "scope": "Exact saved source-hash/mean/std/window/valid-frame equivalence; the pinned driver also checked prepared music bytes against worker _read_window. No original native input PCM was retained or recomputed by this facts helper."},
        "timing_scope": "Native request round-trip and native graph duration; these are not StemWorker hop-service p99, and4095ms is a diagnostic comparison only.",
        "order_verified": r["order"], "per_input": summaries, "replay_seconds": r["replay_seconds"], "post_ready_budget_seconds": 300,
        "admission": admission, "completed_readiness": {k: proof[k] for k in ("status", "started_monotonic", "finished_monotonic", "required_consecutive_sample_span_seconds", "package_threshold_millidegrees_C", "guard_service_identity")},
        "completed_readiness_last_observed_span_seconds": proof["rows"][-1]["consecutive_ready_sample_span_seconds"],
        "cleanup": r["cleanup"], "guard": guard, "request_profiles": profiled,
        "abba_blocks": blocks, "paired_summaries": {k: {"block_mean_differences_ms": pm.stats([x[k]["music_mean_minus_synthetic_mean_ms"] for x in blocks]),
            "adjacent_direction_balanced_differences_ms": pm.stats(v), "music_slower_blocks": sum(x[k]["music_mean_minus_synthetic_mean_ms"] > 0 for x in blocks)} for k,v in adjacent.items()},
        "sampling": {"count": len(trace), "first_seconds_after_native_ready": trace[0]["timestamp_monotonic"] - r["replay_started_monotonic"],
            "last_seconds_after_native_ready": trace[-1]["timestamp_monotonic"] - r["replay_started_monotonic"],
            "max_gap_seconds": max(b["timestamp_monotonic"] - a["timestamp_monotonic"] for a,b in zip(trace,trace[1:])),
            "capture_elapsed_ms": pm.stats([x["capture_elapsed_ms"] for x in trace]), "fields": fields,
            "policy_counts_all_trace": dict(Counter(pm.policy(x) for x in trace)), "field_statistics_all_trace": pm.field_stats(trace, fields), "thermal": thermal,
            "processes": resources["processes"], "sample_errors": resources["sample_errors"], "trace_errors": resources["trace"]["errors"], "system_read_errors": resources["system_sampling"]["read_errors"]},
        "limits": ["All48serial requests retained including first request;24 observations per input and12ABBA blocks are a bounded diagnostic, not sustained qualification.",
            "ABBA adjacent and block differences balance local order; they do not establish a causal content effect or remove changing clock/policy/thermal/cache/background conditions.",
            "Synthetic has343980valid frames while selected music tail has257985valid+85995zero-pad frames; both normalized input arrays have343980stereo frames. Native requests omit worker source reads/overlap/queues/controls.",
            "Same-host monotonic request and sampler clocks. Primary samples require entire capture interval inside request; timestamp-only row selections are retained. Individual field read times and absolute graph endpoints are unavailable.",
            "Thermal threshold episodes are sampled hot spans with neighboring cool brackets, not proven continuous temperature. Other mapped sensors are admission/finish snapshots only.",
            "Recorded identical per-input output hashes and pinned normalization checks are audited; normalized/output PCM is not retained and not numerically replayed by this helper.",
            "Kernel/DRM values retain exact paths/units. Separate process/DRM peaks are not summed; RAPL read denials mean energy/power unavailable.",
            "Native close terminates its persistent child; returncode-9 here records owned cleanup, not an inference failure. No audio/UI, active cancellation, physicalaudio or worker-hop target pass follows."]}
    with output.open("x") as stream:
        json.dump(facts, stream, separators=(",", ":"), allow_nan=False); stream.write("\n")
    print(json.dumps({"facts": pm.pin(output), "per_input": summaries, "paired": facts["paired_summaries"],
        "policy": facts["sampling"]["policy_counts_all_trace"], "thermal": thermal}, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Saved-evidence-only timing/thermal facts and allowlisted corpus provenance."""
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path

BASE = Path(__file__).resolve().parent
TARGET, N, HOP, WINDOW = 4095.0, 1031940, 257985, 343980
PROFILE = "/sys/firmware/acpi/platform_profile"
EPP = "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest}


def read(name):
    return json.loads((BASE / name).read_text())


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def write(name, value):
    with (BASE / name).open("x") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")


def distribution(values):
    ordered = sorted(values)
    assert ordered and all(math.isfinite(v) for v in ordered)
    return {"count": len(ordered), "min": ordered[0],
            **{f"p{p}": ordered[math.ceil(len(ordered) * p / 100) - 1] for p in (50, 95, 99)},
            "max": ordered[-1]}


def timing(chunks):
    values = [c["hop_service_ms"] for c in chunks]
    return {**distribution(values), "unit": "ms", "target_ms": TARGET,
            "strictly_above_target_count": sum(v > TARGET for v in values),
            "hops": [{k: c[k] for k in ("generation", "track_id", "scenario", "frame_start", "frames", "hop_service_ms")}
                     for c in chunks]}


def equal_timing(summary, chunks):
    d = distribution([c["hop_service_ms"] for c in chunks])
    assert summary["chunks"] == len(chunks)
    assert summary["hop_service_max_ms"] == d["max"]
    assert summary["hop_service_percentiles_ms"] == {str(p): d[f"p{p}"] for p in (50, 95, 99)}


def spans(points, predicate):
    episodes, start = [], None
    for index in range(len(points) + 1):
        hot = index < len(points) and predicate(points[index])
        if hot and start is None:
            start = index
        if not hot and start is not None:
            end = index - 1
            first, last = points[start], points[end]
            previous = points[start - 1] if start else None
            following = points[index] if index < len(points) else None
            episodes.append({"samples": end - start + 1, "first_elapsed_seconds": first[0],
                             "last_elapsed_seconds": last[0], "observed_hot_sample_span_seconds": last[0] - first[0],
                             "previous_cool_elapsed_seconds": previous[0] if previous else None,
                             "following_cool_elapsed_seconds": following[0] if following else None,
                             "neighbor_cool_bracket_seconds": following[0] - previous[0] if previous and following else None,
                             "peak_C": max(p[1] for p in points[start:index])})
            start = None
    return episodes


def native_facts(result, trace, controls, complete):
    sampling = result["native_memory_cpu"]
    assert len(trace) == sampling["samples"] == sampling["trace"]["samples_written"]
    assert not sampling["sample_errors"] and not sampling["trace"]["errors"]
    assert all(a["timestamp_monotonic"] < b["timestamp_monotonic"] for a, b in zip(trace, trace[1:]))
    assert len({(r["pid"], r["start_ticks"]) for r in trace}) == 1
    assert len(sampling["processes"]) == 1
    native = sampling["processes"][0]
    assert (trace[0]["pid"], trace[0]["start_ticks"]) == (native["pid"], native["start_ticks"])
    assert {k: max(r["process_status_kB"][k] for r in trace) for k in ("VmHWM", "VmRSS", "VmSwap")} == native["max_process_status_kB"]
    assert trace[0]["process_counters"] == native["first_process_counters"]
    assert trace[-1]["process_counters"] == native["last_process_counters"]

    def phase(row):
        timestamp = row["timestamp_monotonic"]
        for control in controls:
            if control["timestamp"] <= timestamp <= complete[control["generation"]]["timestamp"]:
                return control["scenario"] + ":" + control["track_id"] + ":g" + str(control["generation"])
        if timestamp < controls[0]["timestamp"]:
            return "startup_before_first_control"
        return "between_controls_or_cleanup"

    phase_counts = Counter(phase(r) for r in trace)
    policy = Counter()
    policy_phases = defaultdict(Counter)
    policy_runs = []
    for row in trace:
        key = json.dumps({"profile": row["system_values"].get(PROFILE), "epp": row["system_values"].get(EPP)}, sort_keys=True)
        policy[key] += 1
        policy_phases[phase(row)][key] += 1
        if not policy_runs or policy_runs[-1]["policy"] != json.loads(key):
            policy_runs.append({"policy": json.loads(key), "samples": 0,
                                "first_elapsed_seconds": row["elapsed_seconds"], "last_elapsed_seconds": row["elapsed_seconds"]})
        policy_runs[-1]["samples"] += 1
        policy_runs[-1]["last_elapsed_seconds"] = row["elapsed_seconds"]
    for run in policy_runs:
        run["observed_sample_span_seconds"] = run["last_elapsed_seconds"] - run["first_elapsed_seconds"]
    sensors = {}
    for path, unit in sampling["system_sampling"]["fields"].items():
        if unit != "millidegrees_C" or not path.startswith("/sys/class/thermal/") or not path.endswith("/temp"):
            continue
        zone = str(Path(path).parent)
        labels = {result[k][zone]["type"] for k in ("conditions_at_start", "conditions_at_finish")}
        assert len(labels) == 1
        points = [(r["elapsed_seconds"], r["system_values"][path] / 1000) for r in trace if path in r["system_values"]]
        sensors[path] = {"type_from_saved_condition_snapshots": labels.pop(), "unit": "C", "samples": len(points),
                         "first_elapsed_seconds": points[0][0], "last_elapsed_seconds": points[-1][0],
                         "max_sample_gap_seconds": max((b[0] - a[0] for a, b in zip(points, points[1:])), default=None),
                         "min_C": min(p[1] for p in points), "peak_C": max(p[1] for p in points),
                         "thresholds": {str(threshold): {"inclusive_threshold_C": threshold,
                             "hot_samples": sum(p[1] >= threshold for p in points),
                             "episodes": spans(points, lambda p, t=threshold: p[1] >= t)} for threshold in (85, 95)}}
    sampled_zones = {str(Path(p).parent) for p in sensors}
    unsampled = {k: v["type"] for k, v in result["conditions_at_start"].items()
                 if isinstance(v, dict) and "type" in v and k not in sampled_zones}
    fields = sampling["system_sampling"]["fields"]
    frequencies = {path: {**distribution([r["system_values"][path] for r in trace if path in r["system_values"]]), "unit": unit}
                   for path, unit in fields.items() if path.endswith("scaling_cur_freq") or path.endswith("gt_act_freq_mhz") or path.endswith("gt_cur_freq_mhz")}
    throttles = {path: {"unit": fields[path], "first": trace[0]["system_values"].get(path),
                        "last": trace[-1]["system_values"].get(path),
                        "observed_values": dict(Counter(str(r["system_values"][path]) for r in trace if path in r["system_values"]))}
                 for path in fields if "throttle_reason_" in path or "/thermal_throttle/" in path}
    return {"samples": len(trace), "first_elapsed_seconds": trace[0]["elapsed_seconds"],
            "last_elapsed_seconds": trace[-1]["elapsed_seconds"], "sampler_elapsed_seconds": sampling["elapsed_seconds"],
            "coverage_note": "All saved native samples retained, including any startup/control/seek periods. Native process sampling begins after process discovery and ends before/at cleanup; it is not continuous sensor coverage.",
            "max_sample_gap_seconds": max(b["timestamp_monotonic"] - a["timestamp_monotonic"] for a, b in zip(trace, trace[1:])),
            "capture_elapsed_ms": distribution([r["capture_elapsed_ms"] for r in trace]),
            "phase_counts": dict(phase_counts), "policy_counts": dict(policy),
            "policy_phase_counts": {k: dict(v) for k, v in policy_phases.items()}, "policy_sample_runs": policy_runs,
            "thermal": sensors, "snapshot_only_unsampled_sensor_types": unsampled, "frequencies": frequencies,
            "sampled_throttle_fields": throttles, "process": native,
            "supervisor": result["supervisor_memory_cpu"], "sample_errors": sampling["sample_errors"],
            "trace_errors": sampling["trace"]["errors"], "system_read_errors": sampling["system_sampling"]["read_errors"],
            "memory_scope": "Native and supervisor /proc status kB are separate process peaks. DRM client maxima retain exact names/units and are not summed with RSS or with each other. Observed CPU seconds exclude earlier work before the first successful process sample.",
            "thermal_scope": "85/95 C are descriptive inclusive reporting thresholds, not new acceptance gates. Consecutive hot-sample spans and neighbor-cool brackets do not prove continuous temperature between samples or establish sustained-hot duration. Other mapped sensors are start/end snapshots only.",
            "power_scope": "Denied RAPL energy fields are unavailable, not zero energy or power. Sampled policy/frequency changes and kernel throttle values are descriptive; no causal attribution of latency."}


def backend_facts(backend, manifest):
    directory = BASE / backend
    result = read(backend + "/result.json")
    events = rows(directory / "events.jsonl")
    trace = rows(directory / "native-trace.jsonl")
    assert result["status"] == "ok" and not result["failures"] and result["backend"] == backend
    assert result["pins"]["manifest"] == pin(BASE / "manifest.json")
    assert result["threads"] == 4 and result["elapsed_seconds"] <= 1800
    assert [t["id"] for t in result["tracks"]] == [t["id"] for t in manifest["tracks"]]
    by_generation = defaultdict(list)
    for event in events:
        by_generation[event["generation"]].append(event)
    controls = [e for e in events if e["event"] == "control"]
    assert len(controls) == (4 if backend == "cpu" else 7)
    assert len({e["generation"] for e in controls}) == len(controls)
    complete = {}
    scenario_facts, tracks = [], []
    summaries = {t["continuous"]["generation"]: t["continuous"] for t in result["tracks"]}
    summaries.update({s["generation"]: s for s in result["seeks"]})
    inputs = {t["id"]: t["input"] for t in result["tracks"]}
    for control in controls:
        generation, track_id = control["generation"], control["track_id"]
        group = by_generation[generation]
        assert sum(e["event"] == "control" for e in group) == 1
        chunks = [e for e in group if e["event"] == "chunk"]
        ends = [e for e in group if e["event"] == "complete"]
        assert len(ends) == 1
        complete[generation] = ends[0]
        starts = {e["request_id"]: e for e in group if e["event"] == "started"}
        assert all(e["track_id"] == track_id and e["scenario"] == control["scenario"] for e in group)
        source = inputs[track_id]
        summary = summaries[generation]
        assert control["command"] == ("load" if control["scenario"] == "continuous" else "seek")
        assert control["frame_start"] == summary["start_frame"]
        assert control["source_path"] == source["path"]
        frame = control["frame_start"]
        for chunk in chunks:
            started = starts[chunk["request_id"]]
            assert not started["preroll"] and started["source_id"] == source["sha256"]
            assert started["window_start_frame"] == chunk["window_start_frame"]
            assert control["timestamp"] <= started["timestamp"] <= chunk["ready_at"] <= chunk["consumed_at"]
            assert chunk["source_id"] == source["sha256"] and chunk["model_id"] == result["pins"]["model"]["sha256"]
            assert chunk["frame_start"] == frame and 0 < chunk["frames"] <= HOP
            assert chunk["source_frames"] == N and chunk["sample_rate"] == 44100 and chunk["channels"] == 2
            frame += chunk["frames"]
        assert frame == N == ends[0]["frame_start"] and ends[0]["source_id"] == source["sha256"]
        assert summary["frames"] == N - control["frame_start"]
        first_latency = (chunks[0]["ready_at"] - control["timestamp"]) * 1000
        assert abs(first_latency - chunks[0]["control_latency_ms"]) <= 1e-6
        assert abs(first_latency - summary["first_chunk"]["control_to_ready_ms"]) <= 1e-6
        assert chunks[0]["ready_at"] == summary["first_chunk"]["ready_monotonic"]
        equal_timing(summary, chunks)
        scenario_facts.append({"generation": generation, "track_id": track_id, "scenario": control["scenario"],
                               "start_frame": control["frame_start"], "frames": frame - control["frame_start"],
                               "chunks": len(chunks), "first_control_to_ready_ms": first_latency,
                               "first_raw_ready_to_consumed_ms": (chunks[0]["consumed_at"] - chunks[0]["ready_at"]) * 1000,
                               "first_summary_delivery_to_ready_ms": (summary["first_chunk"]["delivered_monotonic"] - chunks[0]["ready_at"]) * 1000,
                               "control_monotonic": control["timestamp"], "complete_monotonic": ends[0]["timestamp"],
                               "raw_control_to_complete_ms": (ends[0]["timestamp"] - control["timestamp"]) * 1000,
                               "wall_seconds": summary["wall_seconds"], "timing": timing(chunks),
                               "preroll_requests": sum(e.get("preroll", False) for e in starts.values()),
                               "source_ready": [e for e in group if e["event"] == "source_ready"]})
    assert all(complete[a["generation"]]["timestamp"] <= b["timestamp"] for a, b in zip(controls, controls[1:]))
    for track, raw in zip(result["tracks"], manifest["tracks"]):
        assert track["input"] == raw["input_pin"] and not track["references"]
        summary = track["continuous"]
        assert summary["start_frame"] == 0 and summary["frames"] == N and summary["chunks"] == 4
        source_ready = [e for e in track["source_ready"] if e["generation"] == summary["generation"] and e["scenario"] == "continuous"]
        assert len(source_ready) == 1 and not source_ready[0]["reused"]
        output = track["output"]
        assert output["complete"] and output["shape"] == [4, N, 2] and output["dtype"] == "<f4"
        assert summary["first_chunk"]["load_to_delivered_ms"] == (summary["first_chunk"]["delivered_monotonic"] - summary["load_issued_monotonic"]) * 1000
        tracks.append({"id": track["id"], "input": track["input"], "output": output,
                       "first_load": "cold native backend" if not tracks else "warm persistent native backend, new source preparation",
                       "first_chunk": summary["first_chunk"], "source_preparation_ms": source_ready[0]["preparation_ms"],
                       "source_preparation_scope": "WAV read, statistics and worker preparation; original FLAC decoding occurred before the benchmark",
                       "source_ready_all_generations": track["source_ready"],
                       "wall_seconds": summary["wall_seconds"], "hop_service_percentiles_ms": summary["hop_service_percentiles_ms"],
                       "hop_service_max_ms": summary["hop_service_max_ms"]})
    stats = result["worker_statistics"]
    assert stats["backend_start_count"] == 1 and stats["closed"] and stats["backend_pid"] is None
    assert stats["inflight"] is None and stats["pending_latest_generation"] is None
    assert stats["ready_count"] == stats["telemetry_count"] == stats["telemetry_dropped"] == stats["discarded_count"] == stats["cancel_count"] == 0
    assert len(result["runtime_ready"]) == 1
    assert set(e["event"] for e in events) <= {"control", "runtime_ready", "source_ready", "started", "chunk", "complete"}
    if backend == "vulkan":
        assert [s["start_frame"] for s in result["seeks"]] == [HOP - 1, WINDOW + 1, N - 4096]
        assert all(s["exact"] and s["numeric_parity"] and s["max_abs_difference"] == s["rms_difference"] == 0
                   and s["chunk_stream_sha256"] == s["reference_chunk_stream_sha256"] for s in result["seeks"])
    else:
        assert not result["seeks"]
    chunks = [e for e in events if e["event"] == "chunk"]
    continuous = [c for c in chunks if c["scenario"] == "continuous"]
    assert len(continuous) == 16
    return {"status": "ok", "inputs": [pin(directory / n) for n in ("result.json", "events.jsonl", "native-trace.jsonl")],
            "pins": result["pins"], "elapsed_seconds": result["elapsed_seconds"], "runtime_ready": result["runtime_ready"],
            "tracks": tracks, "continuous_timing": timing(continuous), "all_delivered_timing": timing(chunks),
            "scenarios": scenario_facts, "seeks_recorded": result["seeks"], "worker_statistics": stats,
            "event_counts": dict(Counter(e["event"] for e in events)),
            "native_sampling": native_facts(result, trace, controls, complete),
            "conditions_at_start": result["conditions_at_start"], "conditions_at_finish": result["conditions_at_finish"],
            "stale_and_cancellation_scope": "All new loads/seeks followed the previous complete event. Zero discarded/cancelled events and zero cancel_count do not exercise active-request cancellation, stale-result rejection or hardware preemption. The three seeks compare recorded full suffixes to their same-backend continuous baseline; this facts helper checks their recorded parity/hashes and does not replay excluded PCM.",
            "audio_ui_scope": "No audio playback or UI load was launched. No callback/xrun/physical-audio or UI-cadence result is supplied by these runs."}


def corpus_facts():
    provenance, plan, revision = read("corpus-provenance.json"), read("excerpt-plan.json"), read("id-revision.json")
    assert provenance["status"] == "ready" and provenance["originals_unchanged"] and len(provenance["tracks"]) == 4
    assert provenance["manifest"] == pin(BASE / "manifest.json")
    assert provenance["excerpt_plan"] == pin(BASE / "excerpt-plan.json")
    assert provenance["id_revision"] == pin(BASE / "id-revision.json")
    allowed_decode = ("command", "elapsed_seconds", "float32_bytes", "decoded_excerpt_float32_sha256",
                      "decoded_excerpt_peak", "common_stereo_gain", "gain_required", "scaled_excerpt_peak",
                      "pcm16_min", "pcm16_max", "pcm16_sha256", "source_frame_offset", "source_frame_end_exclusive",
                      "frames", "source_offset_seconds", "duration_seconds", "sample_rate", "channels", "sample_bytes", "quantization")
    return {"schema": 1, "status": "ready", "sanitization": "Allowlisted facts only. Full ffprobe format/streams/tags/artwork are retained locally and omitted here. Source filenames are user-provided labels; artists/titles/license/provenance are not independently verified. No ground-truth stems supplied.",
            "inputs": [pin(BASE / name) for name in ("corpus-provenance.json", "excerpt-plan.json", "id-revision.json", "manifest.json")],
            "preparation_sources": {k: provenance[k] for k in ("preparation_source", "initial_decoder_source_preserved_as", "corrected_preparation_source")},
            "id_revision": {k: revision[k] for k in ("completed_at_utc", "reason", "scope", "revision_source", "initial_decoder_source_preserved_as", "corrected_preparation_source", "initial_documents", "tracks")},
            "excerpt_plan": {k: plan[k] for k in ("declared_at_utc", "status", "source_offset_seconds", "source_offset_frames",
                "duration_seconds", "excerpt_frames", "frames", "sample_rate", "rate", "channels", "sample_bytes", "looping", "crop_timeline", "normalization", "quantization", "decode_timeout_seconds_per_track")},
            "originals_unchanged": provenance["originals_unchanged"], "audio_download_bytes": provenance["audio_download_bytes"],
            "full_track_copies": provenance["full_track_copies"], "numpy_version": provenance["numpy_version"],
            "tracks": [{"id": t["id"], "source_filename": t["source_filename"], "source": t["source"],
                        "source_stat": t["source_stat"], "audio_metadata": t["audio_metadata"],
                        "decode": {k: t["decode"][k] for k in allowed_decode}, "prepared_wav": t["prepared_wav"],
                        "original_unchanged": t["original_unchanged"]} for t in provenance["tracks"]]}


def main():
    for name in ("run-facts.json", "corpus-facts.json"):
        assert not (BASE / name).exists(), "Facts outputs must be fresh"
    sequence, guard, manifest = read("sequence.json"), read("series-guard.json"), read("manifest.json")
    assert sequence["status"] == "completed" and sequence["completed_backends"] == ["cpu", "vulkan"]
    assert not sequence["profile_mutation"] and all(l["returncode"] == 0 for l in sequence["launches"])
    assert guard["status"] == "ok" and guard["idle_inhibit_released"] and guard["child_returncode"] == guard["returncode"] == 0
    facts = {"schema": 1, "created_at_utc": datetime.now(timezone.utc).isoformat(), "status": "ok",
             "analysis_source": pin(Path(__file__).resolve()), "source_head": sequence["source_head"],
             "percentile_method": "Nearest rank: sorted[ceil(N*p/100)-1], matching tools/benchmarks/stem_worker.py; no interpolation or startup omission. With16 continuous samples p95=p99=max.",
             "inputs": [pin(BASE / n) for n in ("manifest.json", "sequence.json", "series-guard.json")],
             "lifecycle": {"sequence_status": sequence["status"], "elapsed_seconds": sequence["elapsed_seconds"],
                "guard": guard, "launches": sequence["launches"], "producer_pins": sequence["producer_pins"],
                "admitted_producer_pins": sequence["admitted_producer_pins"], "profile_mutation": sequence["profile_mutation"]},
             "backends": {backend: backend_facts(backend, manifest) for backend in ("cpu", "vulkan")},
             "limits": ["Four23.4-second mixtures,16 continuous hops/backend; this is not a sustained music, audio/UI-load or thermal-soak qualification.",
                        "CPU precedes Vulkan and controls are sequential; sampled policy/frequency/thermal differences confound causal/backend timing comparisons.",
                        "No isolated reference stems or accepted human-listening verdict; timing/seek parity is not musical-quality acceptance.",
                        "No active cancellation, audio callback/physical audio, UI behavior or hardware preemption claim from these new runs.",
                        "Native trace sensor observations retain startup and seeks where sampled; between-sample temperatures and unmonitored source periods are unknown."]}
    corpus = corpus_facts()
    write("corpus-facts.json", corpus)
    facts["corpus_facts"] = pin(BASE / "corpus-facts.json")
    write("run-facts.json", facts)
    print(json.dumps({"status": "ok", "run_facts": pin(BASE / "run-facts.json"),
                      "corpus_facts": facts["corpus_facts"], "backends": {k: {"continuous": v["continuous_timing"],
                      "elapsed_seconds": v["elapsed_seconds"], "policy_counts": v["native_sampling"]["policy_counts"],
                      "thermal": v["native_sampling"]["thermal"]} for k, v in facts["backends"].items()}}, indent=2))


if __name__ == "__main__":
    main()

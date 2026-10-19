#!/usr/bin/env python3
"""Completed unchanged worker follow-up audit; no inference or host queries."""
from collections import Counter
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path

BASE = Path(__file__).resolve().parent
SOURCE = BASE.parent / "user-tracks-2026-10-17"
HELPER = SOURCE / "summarize_runs.py"
spec = importlib.util.spec_from_file_location("saved_worker_facts", HELPER)
saved = importlib.util.module_from_spec(spec); spec.loader.exec_module(saved)
N, HOP = 1031940, 257985
IDS = ["club_xl", "when_the_light_breaks", "under_my_skin", "stfu"]


def audit_events(result, events, inputs):
    """Pure raw-control/coverage/currentness/timing audit for later saved replay."""
    controls = [e for e in events if e["event"] == "control"]
    starts = {e["request_id"]: e for e in events if e["event"] == "started"}
    assert len(controls) == 7 and [e["generation"] for e in controls] == list(range(1,8))
    assert len(starts) == sum(e["event"] == "started" for e in events) == 26
    assert len([e for e in events if e["event"] == "runtime_ready"]) == 1
    assert set(e["event"] for e in events) == {"control", "runtime_ready", "source_ready", "started", "chunk", "complete"}
    assert {e["generation"] for e in events} == set(range(1,8))
    scenarios = [(t, "continuous", t["continuous"]) for t in result["tracks"]]
    scenarios += [(result["tracks"][0], "seek", s) for s in result["seeks"]]
    assert {s["generation"] for _,_,s in scenarios} == set(range(1,8))
    complete, facts = {}, []
    for track, scenario, summary in scenarios:
        group = [e for e in events if e["generation"] == summary["generation"]]
        cs, ss, ends, chunks = [[e for e in group if e["event"] == kind] for kind in ("control", "source_ready", "complete", "chunk")]
        assert len(cs) == len(ss) == len(ends) == 1 and chunks
        control, source, end = cs[0], ss[0], ends[0]
        assert all(e["track_id"] == track["id"] and e["scenario"] == scenario for e in group)
        assert control["command"] == ("load" if scenario == "continuous" else "seek")
        assert control["frame_start"] == summary["start_frame"] and control["source_path"] == source["source_path"] == track["input"]["path"]
        assert source["source_id"] == end["source_id"] == track["input"]["sha256"] and source["source_frames"] == N
        frame = summary["start_frame"]
        for chunk in chunks:
            request = starts[chunk["request_id"]]
            assert request["generation"] == chunk["generation"] == summary["generation"] and not request["preroll"]
            assert request["window_start_frame"] == chunk["window_start_frame"] == frame // HOP * HOP
            assert request["source_id"] == chunk["source_id"] == track["input"]["sha256"]
            assert chunk["model_id"] == result["pins"]["model"]["sha256"] and chunk["source_frames"] == N
            assert chunk["stems"] == ["drums","bass","other","vocals"] and chunk["sample_rate"] == 44100 and chunk["channels"] == 2
            assert chunk["frame_start"] == frame and chunk["frames"] == min(HOP - frame % HOP, N - frame)
            assert control["timestamp"] <= request["timestamp"] <= chunk["ready_at"] <= chunk["consumed_at"]
            latest = max((c for c in controls if c["timestamp"] <= chunk["consumed_at"]), key=lambda c:c["timestamp"])
            assert latest["generation"] == chunk["generation"]
            frame += chunk["frames"]
        assert frame == end["frame_start"] == N and summary["frames"] == N - summary["start_frame"]
        first = summary["first_chunk"]
        assert first["ready_monotonic"] == chunks[0]["ready_at"] and first["delivered_monotonic"] >= chunks[0]["consumed_at"]
        assert abs((chunks[0]["ready_at"] - control["timestamp"]) * 1000 - first["control_to_ready_ms"]) < 1e-6
        assert first["control_to_ready_ms"] == chunks[0]["control_latency_ms"]
        saved.equal_timing(summary, chunks)
        complete[summary["generation"]] = end
        facts.append({"track_id":track["id"], "scenario":scenario, "generation":summary["generation"],
            "start_frame":summary["start_frame"], "frames":summary["frames"], "first_chunk":first,
            "wall_seconds":summary["wall_seconds"], "source_preparation_ms":source["preparation_ms"],
            "source_reused":source["reused"], "timing":saved.timing(chunks)})
    assert all(complete[a["generation"]]["consumed_at"] <= b["timestamp"] for a,b in zip(controls,controls[1:]))
    assert [s["start_frame"] for s in result["seeks"]] == [257984,343981,1027844]
    assert all(s["exact"] and s["numeric_parity"] and s["max_abs_difference"] == s["rms_difference"] == 0
        and s["chunk_stream_sha256"] == s["reference_chunk_stream_sha256"] for s in result["seeks"])
    continuous = [e for e in events if e["event"] == "chunk" and e["scenario"] == "continuous"]
    all_chunks = [e for e in events if e["event"] == "chunk"]
    assert len(continuous) == 16 and len(all_chunks) == 24
    for track in result["tracks"]:
        assert track["input"] == inputs[track["id"]] and track["continuous"]["start_frame"] == 0 and track["continuous"]["chunks"] == 4
        c = track["continuous"]
        assert c["first_chunk"]["load_to_delivered_ms"] == (c["first_chunk"]["delivered_monotonic"] - c["load_issued_monotonic"]) * 1000
        assert track["output"]["complete"] and track["output"]["shape"] == [4,N,2] and track["output"]["dtype"] == "<f4"
    return controls, complete, {"event_counts":dict(Counter(e["event"] for e in events)), "continuous_timing":saved.timing(continuous),
        "all_delivered_timing":saved.timing(all_chunks), "scenarios":sorted(facts,key=lambda x:x["generation"]), "seeks":result["seeks"]}


def main():
    output = BASE / "worker-facts.json"
    assert not output.exists(), "Facts output must be fresh"
    run = BASE / "worker-four-tracks"
    paths = {"result":run/"result.json", "events":run/"events.jsonl", "trace":run/"native-trace.jsonl",
        "launch":BASE/"worker-followup-launch.json", "guard":BASE/"worker-guard.json", "manifest":SOURCE/"manifest.json",
        "previous_vulkan_result":SOURCE/"vulkan/result.json", "readiness":BASE/"worker-admission/admission-normal-ready.json",
        "readiness_rows":BASE/"worker-admission/admission-normal-ready.jsonl", "admission":BASE/"worker-admission/fresh-admission.json"}
    result, launch, guard, manifest, old, proof, admission = [saved_read(paths[k]) for k in ("result","launch","guard","manifest","previous_vulkan_result","readiness","admission")]
    events, trace = saved.rows(paths["events"]), saved.rows(paths["trace"])
    assert result["status"] == "ok" and not result["failures"] and result["backend"] == "vulkan" and result["threads"] == 4
    assert result["elapsed_seconds"] <= 360 and result["request_timeout_seconds"] == 120
    assert launch["status"] == "completed" and not launch["failures"] and launch["child_returncode"] == 0
    assert launch["source_head"] == launch["expected_head"] == "8a61059f585947c91fef4a5606ad11fb81523daf"
    assert launch["child_wall_limit_seconds"] == 360 and launch["outer_bound_seconds"] == 1020
    assert launch["cleanup"] == {"child_reaped":True,"owned_process_group_exists":False}
    assert launch["worker_result"] == saved.pin(paths["result"])
    assert launch["pins"]["launcher"] == saved.pin(BASE/"worker_followup.py") and launch["pins"]["launcher"]["sha256"] == "8cf1c7258fc60b206a651543f84b1caf86e5af0a0fbac5e9a1e6f14154b56dc4"
    assert guard["status"] == "ok" and guard["idle_inhibit_released"] and not guard["errors"] and guard["returncode"] == guard["child_returncode"] == 0
    assert [t["id"] for t in result["tracks"]] == [t["id"] for t in old["tracks"]] == IDS
    assert result["pins"] == old["pins"]
    assert all(result["pins"][k] == launch["pins"][k] for k in result["pins"])
    assert result["pins"]["manifest"] == saved.pin(paths["manifest"])
    assert {t["id"]:t["input_pin"] for t in manifest["tracks"]} == launch["inputs"]
    stats = result["worker_statistics"]
    assert stats["closed"] and stats["backend_pid"] is None and stats["backend_start_count"] == 1 and stats["generation"] == 7
    assert stats["pending_latest_generation"] is None and stats["inflight"] is None
    assert all(stats[k] == 0 for k in ("ready_count","telemetry_count","telemetry_dropped","discarded_count","cancel_count"))
    assert len(result["runtime_ready"]) == 1 and result["runtime_ready"][0]["runtime"]["pid"] == trace[0]["pid"]
    assert proof["status"] == "matched" and proof["rows"] == saved.rows(paths["readiness_rows"])
    assert proof["package_threshold_millidegrees_C"] == 55000 and proof["required_consecutive_sample_span_seconds"] == 60
    assert proof["rows"][-1]["consecutive_ready_sample_span_seconds"] >= 60 and not proof["profile_mutation"] and not proof["service_mutation"]
    assert launch["readiness_returncode"] == 0 and launch["readiness_proof"] == admission["proof"] == saved.pin(paths["readiness"])
    assert launch["admission"] == admission and launch["fresh_admission_capture"] == saved.pin(paths["admission"])
    live = admission["live_normal_readiness"]
    assert admission["ready"] and not live["errors"] and live["profile"] == "balanced" and live["holds"] == ""
    assert live["latest_message_claims_recovered"] and live["journal_identity_matches_service"] and admission["same_guard_service_identity"] == proof["guard_service_identity"]
    assert 0 <= live["package_millidegrees_C"] <= admission["fresh_package_limit_millidegrees_C"] == 70000
    assert admission["completed_readiness_package_limit_millidegrees_C"] == 55000
    assert admission["AC_lid"]["ready"] and not admission["AC_lid"]["errors"] and admission["AC_lid"]["AC"] == "1" and admission["AC_lid"]["lid"] == "b false"
    assert admission["finished_monotonic"] <= launch["child_spawn_started_monotonic"] <= launch["child_started_monotonic"]
    controls, complete, facts = audit_events(result,events,launch["inputs"])
    arrays = []
    for track, previous in zip(result["tracks"],old["tracks"]):
        current_pin, previous_pin = saved.pin(Path(track["output"]["path"])), saved.pin(Path(previous["output"]["path"]))
        assert all(current_pin[k] == track["output"][k] == previous_pin[k] == previous["output"][k] for k in ("bytes","sha256"))
        assert track["continuous"]["chunk_stream_sha256"] == previous["continuous"]["chunk_stream_sha256"]
        arrays.append({"id":track["id"], "current":current_pin, "previous":previous_pin, "file_bytes_and_sha256_match":True,
            "chunk_stream_sha256":track["continuous"]["chunk_stream_sha256"]})
    sampling = saved.native_facts(result,trace,controls,complete)
    facts.update(schema=1,status="ok",analyzed_at_utc=datetime.now(timezone.utc).isoformat(),analysis_source=saved.pin(Path(__file__).resolve()),
        dependencies=[saved.pin(HELPER)],inputs={k:saved.pin(p) for k,p in paths.items()},producer_pins=result["pins"],
        launcher_source=launch["pins"]["launcher"],worker_elapsed_seconds=result["elapsed_seconds"],worker_statistics=stats,
        output_identity=arrays,native_sampling=sampling,admission=admission,
        readiness_wait={"elapsed_seconds":proof["finished_monotonic"]-proof["started_monotonic"],
            "maximum_seconds":proof["maximum_seconds"],"last_ready_sample_span_seconds":proof["rows"][-1]["consecutive_ready_sample_span_seconds"],
            "sample_rows":len(proof["rows"]),"peak_package_C_all_wait_rows":max(x["package_millidegrees_C"] for x in proof["rows"] if x["package_millidegrees_C"] is not None)/1000,
            "scope":"Readiness waiting is outside the48.963s worker run; full wait snapshots are not pooled into worker thermal/latency results."},
        lifecycle={"launch_status":launch["status"],"child_returncode":launch["child_returncode"],"cleanup":launch["cleanup"],
            "child_wall_seconds":launch["child_finished_monotonic"]-launch["child_started_monotonic"],"launcher_elapsed_seconds":launch["elapsed_seconds"],"guard":guard},
        limits=["16continuous/8seek delivered hops in a short unchanged Vulkan worker run. Nearest-rank p95/p99 of16 equal max; this is no sustained music/audioUI/thermal-soak qualification.",
            "All7controls follow previous consumed completion. Exact same-grid seeks and zero cancel/discard counters do not test active cancellation or hardware preemption.",
            "All4complete NPY files were actually rehashed against previous Vulkan files. Identical file hashes bind identical saved samples; no new subjective audio quality verdict is inferred.",
            "Policy/frequency/temperature are sampled; any association with earlier slower runs is confounded by history/order/background state and cannot establish a causal fix.",
            "Initial static launcher review bound8ecf8b002c4fc5d8d1ca29a212da6e652712b1405e05b5c9c44d5e8a1a253ffe; root reports the final8cf1c725 launcher edit changed only scope prose. Final executed bytes are pinned here.",
            "The55C/60s readiness wait and final70C admission are distinct recorded conditions, not changed4.095s latency or musical acceptance gates."])
    with output.open("x") as stream:json.dump(facts,stream,separators=(",",":"),allow_nan=False);stream.write("\n")
    print(json.dumps({"facts":saved.pin(output),"continuous":facts["continuous_timing"],"all":facts["all_delivered_timing"],"policy":sampling["policy_counts"],"thermal":sampling["thermal"],"readiness_wait":facts["readiness_wait"]},indent=2))


def saved_read(path):
    return json.loads(path.read_text())


if __name__ == "__main__":main()

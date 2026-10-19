#!/usr/bin/env python3
"""Audit a CLOSED Flash loaded diagnostic using metadata only; dry by default."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

BASE = Path(__file__).resolve().parent
FLASH_SHA = "6f311e461e7af5f12ff38f90ddb8720179c2c3dce23d593437d2b34524dffd9d"
MODEL_SHA = "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"
CLOSED_SHA = "917104a1cc8ca2522eef38bae6d620c972fdb2be826876b2dee88e67dd7a497a"
WRAPPER_SHA = "883d537f8f2fe91dc2e5fc5a735ca20067a007cbe8e308e9f1673825bf39e684"
FAULTS = ("callback_execution_deadline_misses", "missing_buffers", "invalid_buffers", "truncated_frames",
          "observed_graph_position_discontinuities", "observed_position_clock_xrun_recovery_flags")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("--run", action="store_true", help="Read only completed, closed evidence")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", args.name):
        parser.error("Use one plain closed-run label")
    if not args.run:
        print(json.dumps({"status": "dry_run", "files_read": False, "native_started": False,
                          "scope": "Closed metadata integrity only; no latency or full-soak pass inferred."}))
        return 0
    run = BASE / args.name
    output = args.output or BASE / (args.name + "-independent-audit.json")
    report = {"schema": 1, "status": "incomplete", "name": args.name, "checks": [], "files": {}, "errors": []}

    def check(name, value):
        if not value:
            raise ValueError(name)
        report["checks"].append(name)

    def decode(raw):
        def invalid(value):
            raise ValueError("Nonfinite JSON constant: " + value)
        return json.loads(raw, parse_constant=invalid)

    def read(path, maximum=4 * 1024 * 1024, pin=None):
        path = Path(path)
        with path.open("rb") as stream:
            raw = stream.read(maximum + 1)
        check("Bounded metadata read: " + str(path), len(raw) <= maximum)
        actual = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        if pin:
            check("Raw retained pin binding: " + str(path),
                  all(actual[key] == pin[key] for key in actual))
        report["files"][str(path)] = actual
        return raw

    def load(path, maximum=4 * 1024 * 1024, pin=None):
        return decode(read(path, maximum, pin))

    def option(command, flag):
        return command[command.index(flag) + 1]

    try:
        launch = load(BASE / (args.name + "-launch.json"))
        # Completion is a precondition: this helper must never inspect a running workload.
        check("Closed structural launcher completion", launch["status"] == "completed"
              and launch["failures"] == [] and launch["returncode"] == 0 and launch["child_started"] is True)
        result = load(run / "result.json", pin=launch["result"])
        check("Completed600-second workload, independent of the latency gate",
              result["status"] == "ok" and result["failures"] == [] and result["requested_seconds"] == 600
              and result["workload_seconds"] >= 600
              and math.isclose(result["workload_stopped_monotonic"] - result["workload_started_monotonic"],
                               result["workload_seconds"], rel_tol=0, abs_tol=1e-6))
        expected = launch["expected_producer"]
        before, after = launch["proof_source_pins_before"], launch["proof_source_pins_after"]
        check("All68 proof/source identities remain unchanged", len(before) == 68 and before == after)
        sources = load(before["source_configuration"]["path"], 64 * 1024, before["source_configuration"])
        check("All64 source-manifest entries bind retained before/after identities",
              len(sources) == 64 and expected["source_pins_sha256"] == before["source_configuration"]["sha256"]
              and all(before["source:" + path]["path"] == path
                      and before["source:" + path]["sha256"] == digest for path, digest in sources.items())
              and {key[7:] for key in before if key.startswith("source:")} == set(sources))
        check("Recorded source HEAD is the exact armed checkpoint",
              result["repository_head"] == expected["repository_head"])
        config = load(launch["pins"]["expected_pins_file"]["path"], 64 * 1024,
                      launch["pins"]["expected_pins_file"])
        check("Raw armed configuration equals launch record", config == expected and config["status"] == "ready")
        producer = launch["pins"]
        check("All eight workload producers bind launcher and soak",
              set(expected["pins"]) == set(result["pins"])
              and all(result["pins"][key]["sha256"] == producer[key]["sha256"] == digest
                      and result["pins"][key]["path"] == producer[key]["path"]
                      for key, digest in expected["pins"].items())
              and producer["runtime"]["sha256"] == FLASH_SHA and producer["model"]["sha256"] == MODEL_SHA)
        proof = load(before["closed_quality"]["path"], 8 * 1024 * 1024, before["closed_quality"])
        admission_proof = launch["comparison"]
        check("Closed917104 own-arm B admission, without a cross-arm identity claim",
              before["closed_quality"]["sha256"] == CLOSED_SHA and proof["status"] == "comparison_complete"
              and proof["completed_requests"] == 32 and proof["cleanup_complete"] is True
              and proof["pins_before"]["candidate"]["sha256"] == FLASH_SHA
              and proof["pins_after"]["candidate"]["sha256"] == FLASH_SHA
              and admission_proof["sha256"] == CLOSED_SHA and admission_proof["selected_arm"] == "B"
              and admission_proof["candidate_sha256"] == FLASH_SHA
              and admission_proof["selected_reference_label"] == "flash"
              and admission_proof["cross_arm_exact_pcm"] is False and launch["cross_arm_exact_pcm"] is False)
        provenance = launch["execution_provenance"]
        check("Reviewed wrapper and selected/original nested-helper provenance is recorded",
              provenance["adapter_sha256"] == WRAPPER_SHA
              and producer["launcher"]["sha256"] == WRAPPER_SHA
              and sources[provenance["adapter"]] == WRAPPER_SHA
              and all(producer[key]["path"] == value["selected_path"]
                      and producer[key]["sha256"] == value["selected_sha256"]
                      and sources[value["selected_path"]] == value["selected_sha256"]
                      and sources[value["original_path"]] == value["original_sha256"]
                      for key, value in provenance["nested_helpers"].items()))
        inputs = launch["inputs"]
        check("All four prepared-source identities and geometry are unchanged",
              len(inputs) == len(result["sources"]) == 4 and result["source_mode"] == "prepared_wav_corpus"
              and [source["sha256"] for source in result["sources"]] == [pin["sha256"] for pin in inputs.values()]
              and all(all(source[key] == pin[key] for key in ("path", "bytes", "sha256", "frames",
                                                              "sample_rate", "channels", "sample_bytes"))
                      and source["frames"] == 1031940 and source["sample_rate"] == 44100
                      and source["channels"] == 2 and source["sample_bytes"] == 2
                      for source, pin in zip(result["sources"], inputs.values())))
        human = load(producer["human_readiness_file"]["path"], 16 * 1024, producer["human_readiness_file"])
        first, fresh = launch["human_readiness_before_setup"], launch["human_readiness_before_workload"]
        check("Same original fresh physical readiness at setup and admission",
              first["record"] == fresh["record"] == human and first["pin"] == fresh["pin"]
              and first["pin"]["sha256"] == producer["human_readiness_file"]["sha256"]
              and human["decision"] == "ready" and human["ready_for_run"] == args.name
              and all(human[key] is True for key in ("AC_power", "lid_open", "codex_minimized", "visible_window"))
              and 0 <= first["age_seconds"] <= fresh["age_seconds"] <= 900)
        admission = load(BASE / (args.name + "-admission") / "fresh-admission.json",
                         pin=launch["fresh_admission_capture"])
        normal = load(BASE / (args.name + "-admission") / "admission-normal-ready.json",
                      pin=launch["readiness_proof"])
        check("Original normal/fresh admission and same recovered active guard",
              admission == launch["admission"] and admission["ready"] is True
              and normal["status"] == "matched" and normal["maximum_seconds"] == 600
              and normal["required_consecutive_sample_span_seconds"] == 60
              and normal["package_threshold_millidegrees_C"] == 55000
              and normal["rows"][-1]["consecutive_ready_sample_span_seconds"] >= 60
              and normal["guard_service_identity"] == admission["same_guard_service_identity"]
              and admission["live_normal_readiness"]["profile"] == "balanced"
              and admission["live_normal_readiness"]["holds"] == ""
              and admission["live_normal_readiness"]["latest_message_claims_recovered"] is True
              and admission["live_normal_readiness"]["journal_identity_matches_service"] is True
              and admission["AC_lid"]["ready"] is True)
        monitor = [decode(line) for line in read(BASE / (args.name + "-AC-lid-monitor.jsonl"),
                                                pin=launch["AC_lid_monitor"]).splitlines() if line.strip()]
        observations = [row["AC_lid"] for row in monitor if "AC_lid" in row]
        check("All retained discrete AC/lid observations remain ready",
              observations and all(row["ready"] is True and row["errors"] == []
                                   and row["AC"] == "1" and row["lid"] == "b false" for row in observations))
        ready = result["runtime_ready"]
        check("One Vulkan/two-thread/blocking/passive runtime with exact model",
              len(ready) == 1 and ready[0]["model_id"] == MODEL_SHA
              and ready[0]["runtime"]["backend"] == "vulkan" and ready[0]["runtime"]["threads"] == 2
              and ready[0]["runtime"]["fence_wait_mode"] == "blocking"
              and ready[0]["runtime"]["weight_storage"] == "native"
              and ready[0]["runtime"]["model"] == producer["model"]["path"]
              and ready[0]["runtime"]["process_environment"] == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"})
        stats = result["worker_statistics"]
        check("Bounded ready queue and closed persistent worker",
              stats["max_ready"] == 2 and stats["ready_high_water"] <= 2
              and stats["closed"] is True and stats["backend_pid"] is None
              and stats["backend_start_count"] == 1 and stats["telemetry_dropped"] == 0
              and result["playback"]["maximum_held_chunks"] <= 1 and result["stale_chunks_delivered"] == 0)
        command = launch["command"]
        check("Unchanged requested600/300 paced both-load/software/33ms/silent256 workload",
              all(option(command, flag) == value for flag, value in
                  (("--seconds", "600"), ("--control-interval", "300"), ("--threads", "2"),
                   ("--quantum", "256"), ("--request-timeout", "30"), ("--sample-interval", "0.5"),
                   ("--load", "both"), ("--delivery-mode", "playback-paced"), ("--renderer", "software")))
              and launch["repaint_ms"] == 33 and launch["ready_queue_bound"] == 2
              and launch["effective_UI_QT_QUICK_BACKEND"] == "software"
              and result["renderer"] == "software" and result["delivery_mode"] == "playback-paced"
              and result["load_mode"] == "both" and set(result["selected_loads"]) == {"audio", "ui"})
        ui = load(run / "ui.json")
        audio = load(run / "audio.json")
        check("Raw UI/audio reports equal load records and cover requested duration",
              ui == result["load"]["children"]["ui"]["report"]
              and audio == result["load"]["children"]["audio"]["report"]
              and result["load"]["errors"] == []
              and all(child["returncode"] == 0 for child in result["load"]["children"].values())
              and result["ui_coverage"]["passed"] is True and result["audio_coverage"]["passed"] is True
              and result["audio_coverage"]["concurrent_workload_seconds"] >= 600)
        check("Observed software33ms initialized/exposed UI and silent48k/stereo256 probe",
              ui["animation_driver"] == "qtimeline" and ui["repaint_interval_ms"] == 33
              and ui["graphics_api"] == 1 and ui["rendered"] is True and ui["capture_ok"] is True
              and ui["exposed"] is True and ui["frames_swapped"] > 0
              and audio["status"] == "ok" and audio["signal"] == "silence" and audio["error"] == ""
              and audio["requested"]["sample_rate"] == audio["negotiated"]["sample_rate"] == 48000
              and audio["requested"]["channels"] == audio["negotiated"]["channels"] == 2
              and audio["requested"]["quantum_frames"] == 256 and audio["negotiated"]["format_valid"] is True
              and all(audio["negotiated"][key] == 256 for key in
                      ("stream_frames_min", "stream_frames_max", "graph_quantum_min", "graph_quantum_max"))
              and all(audio[key] == result["load"]["audio_fault_counters"][key] for key in FAULTS))
        events = [decode(line) for line in read(run / "events.jsonl", 32 * 1024 * 1024).splitlines() if line.strip()]
        chunks = [row for row in events if row.get("event") == "chunk"]
        check("Raw delivered chunk identities/queue remain bounded", chunks
              and all(row["ready_queue_depth"] <= 2 and row["model_id"] == MODEL_SHA
                      and row["source_id"] in {pin["sha256"] for pin in inputs.values()} for row in chunks))
        controls = result["controls"]
        check("Required active cancellation/seek batch completed without a hardware-preemption claim",
              len(controls) >= 1 and result["acceptance"]["controls_completed"] == len(controls)
              and all(control["complete"] is True and control["held_chunk_rejected"] is True
                      and control["post_cancel_chunks"] == 0 and control["hardware_submission_confirmed"] is False
                      and control["parity"]["numeric_parity"] is True for control in controls))
        for index, control in enumerate(controls):
            old, new = control["superseded_generation"], control["generation"]
            starts = [row for row in events if row.get("event") == "started" and row["generation"] == old
                      and row.get("request_id") == control["superseded_request_id"] and row.get("source_id") == control["source_id"]]
            discarded = [row for row in events if row.get("event") == "discarded" and row["generation"] == old
                         and row.get("request_id") == control["superseded_request_id"] and row.get("source_id") == control["source_id"]]
            cancel = [row for row in events if row.get("event") == "control" and row.get("command") == "cancel"
                      and row["generation"] == old + 1]
            seek = [row for row in events if row.get("event") == "control" and row.get("command") == "seek"
                    and row["generation"] == new and row["frame_start"] == control["target_frame"]]
            returned = [row for row in chunks if row["generation"] == new
                        and row["source_id"] == control["source_id"] and row["frame_start"] == control["target_frame"]]
            check("Raw cancellation/seek identity batch" + str(index), len(starts) == len(discarded) == len(cancel) == len(seek) == len(returned) == 1
                  and new == old + 2 and not any(row["generation"] == old
                      and row.get("consumed_at", row["timestamp"]) > cancel[0]["timestamp"] for row in chunks))
        cleanup = launch["cleanup"]
        guard = load(BASE / (args.name + "-guard.json"), pin=launch["guard"])
        policy = load(BASE / (args.name + "-policy.json"), pin=launch["policy"])
        check("Bounded owned outer/nested groups and native worker were cleaned",
              cleanup["remaining_known_groups"] == {} and cleanup["remaining_owned_processes"] == {}
              and cleanup["last_child_reaped"] is True
              and len(launch["owned_group_cleanup"]) >= 2
              and all(row["disappearance_observed"] is True for row in launch["owned_group_cleanup"])
              and guard["child_group_cleanup"]["disappearance_observed"] is True
              and policy["child_group_cleanup"]["disappearance_observed"] is True)
        check("Owned KWin placement and idle inhibitor restored/unloaded",
              guard["status"] == "ok" and guard["errors"] == [] and guard["returncode"] == 0
              and guard["idle_inhibit_released"] is True and guard["script_unloaded"] is True
              and bool(guard["restored"]) and guard["window_guard"]["status"] == "restored"
              and guard["ui"]["pid"] == result["ui_coverage"]["progress"]["last_progress"]["pid"])
        check("Runner policy remained observe-only; automatic guard changes remain observable",
              launch["profile_mutation"] is False and launch["service_mutation"] is False
              and policy["mode"] == "observe" and policy["profile_mutation"] is False
              and policy["restored_by_runner"] is False
              and policy["benchmark_returncode"] == policy["runner_returncode"] == 0
              and policy["before"]["profile"] == policy["during"]["profile"] == "balanced"
              and policy["before"]["holds"] == policy["during"]["holds"] == "")
        p99 = result["timing"]["continuous"]["hop_service_percentiles_ms"]["99"]
        target = result["timing"]["headroom_target_ms"]
        gate = launch["latency_gate"]
        check("Strict4095 target outcome is independent of structural completion",
              type(p99) in (int, float) and math.isfinite(p99) and p99 >= 0
              and math.isclose(target, 4095.0, rel_tol=0, abs_tol=1e-9)
              and gate["operator"] == "strict_less_than" and gate["target_ms"] == 4095.0
              and gate["observed_continuous_p99_ms"] == p99 and gate["passed"] == (p99 < target and p99 < 4095.0)
              and result["acceptance"]["observed_continuous_p99_within_target"] == (p99 < target)
              and result["acceptance"]["sixty_minute_workload_completed"] is False
              and result["acceptance"]["full_audio_UI_soak_qualified"] is False
              and gate["full_60minute_qualification"] is False)
        report.update(status="evidence_consistent", structural_completion=True, source_head=expected["repository_head"],
                      workload_seconds=result["workload_seconds"], source_entries=len(sources), proof_pin_entries=len(before),
                      continuous_p99_ms=p99, strict4095_gate_passed=gate["passed"], full_60minute_qualified=False,
                      AC_lid_observations=len(observations), cancellation_batches=len(controls),
                      audio_fault_counters={key: audio[key] for key in FAULTS},
                      policy_values={key: policy[key] for key in ("before", "during", "before_restore", "after")},
                      execution_provenance=provenance,
                      limitations=["Closed metadata only; no model, executable, PCM or live host state was read.",
                                   "64 source identities were checked against recorded68 before/after pins, not rehashed from mutable current source paths.",
                                   "The admission uses closed exact own-arm B proof. This soak does not retain fresh per-request native PCM hashes or shader-selection stderr.",
                                   "Cancellation identities and parity are logical worker observations, not GPU submission/preemption proof.",
                                   "Independent silent audio and software UI notifications do not demonstrate actual stem playback, physical latency or musical acceptance.",
                                   "Policy values may change automatically under the same thermal guard; runner mutation remains forbidden.",
                                   "Detailed independent percentile/phase/control-duration/audio/UI/thermal reductions are separate."])
    except Exception as error:
        report["status"] = "inconsistent_or_incomplete"
        report["errors"].append({"type": type(error).__name__, "message": str(error)})
    report["check_count"] = len(report["checks"])
    report["auditor_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    with output.open("x") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"status": report["status"], "checks": report["check_count"], "receipt": str(output)}))
    return 0 if report["status"] == "evidence_consistent" else 1


if __name__ == "__main__":
    raise SystemExit(main())

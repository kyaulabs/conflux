#!/usr/bin/env python3
"""Append closed hour evidence to the existing master; dry by default."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PARENT = ROOT / "docs/benchmarks/data/m2-graph-phase-2026-10-18"
ARCHIVE = PARENT / "flash-60minute-02"
MASTER = PARENT / "verification.json"


def require(value, message):
    if not value:
        raise ValueError(message)


def read(path):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= 64 * 1024 * 1024,
            "Expected bounded regular metadata: " + str(path))
    before = path.stat()
    raw = path.read_bytes()
    after = path.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
            "Metadata changed while reading: " + str(path))
    return raw


def pin(raw):
    return {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def load(name):
    return json.loads(read(ARCHIVE / name))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_plan", "master": str(MASTER), "archive": str(ARCHIVE)}))
        return 0
    original = read(MASTER)
    master = json.loads(original)
    require(len(master["files"]) == 268 and master["latest_retained_completed_live_validation_p99_ms"]
            == 2776.3940359582193, "Master no longer matches previous checkpoint")
    verification = load("verification.json")
    analysis = load("flash-60minute-laptop-02-analysis.json")
    audit = load("flash-60minute-laptop-02-independent-audit.json")
    result = load("run/result.json")
    continuous = analysis["service"]["continuous"]
    require(analysis["status"] == "closed_descriptive_analysis"
            and analysis["event_result_consistency"] == {"all_delivered": True, "continuous": True}
            and continuous["chunks"] == 600 and continuous["miss_ge_nominal"] == 5
            and continuous["hop_service_ms"]["percentiles"]["99"] == 4024.9258929979987
            and continuous["strict_p99_below_nominal"] is True
            and continuous["strict_p99_below_producer"] is True
            and audit["status"] == "evidence_consistent" and audit["errors"] == []
            and audit["check_count"] == 66 and audit["structural_completion"] is True
            and audit["producer_full_audio_UI_soak_qualified"] is True
            and audit["integrated_stem_playback_qualified"] is False
            and verification["source_head"] == audit["source_head"] == result["repository_head"]
            and verification["workload_seconds"] == result["workload_seconds"] >= 3600
            and verification["latency_gate"]["passed"] is True, "Closed summaries disagree")
    files = {}
    for name, expected in verification["files"].items():
        path = ARCHIVE / name
        require(path.resolve().is_relative_to(ARCHIVE.resolve()), "Archive path escapes directory")
        actual = pin(read(path))
        require(actual == expected, "Archive hash differs: " + name)
        files["flash-60minute-02/" + name] = actual
    files["flash-60minute-02/verification.json"] = pin(read(ARCHIVE / "verification.json"))
    screen = {
        "status": "sixty_minute_independent_load_service_target_passed_integration_pending",
        "repository_head": result["repository_head"], "workload_seconds": result["workload_seconds"],
        "service": analysis["service"], "target": analysis["target"],
        "latency_gate": verification["latency_gate"],
        "producer_full_audio_UI_soak_qualified": True,
        "integrated_stem_playback_qualified": False, "M2_qualified": False,
        "physical_audio_quality_tested": False,
        "cancellation": analysis["cancellation"], "parity": analysis["parity"],
        "playback": {key: analysis["playback"][key] for key in (
            "ordinary_demands", "ready_deadline_misses", "unfulfilled_due_demands",
            "ordinary_deadline_failures", "maximum_ready_lateness_ms", "ready_count_at_demand", "scope")},
        "audio": {"callbacks": analysis["audio"]["report"]["callbacks"],
                  "fault_counters": analysis["audio"]["fault_counters"],
                  "coverage": analysis["audio"]["coverage"], "scope": analysis["audio"]["scope"]},
        "ui": {"frames_swapped": analysis["ui"]["report"]["frames_swapped"],
               "coverage": analysis["ui"]["coverage"], "scope": analysis["ui"]["scope"]},
        "thermal": analysis["thermal"], "native_trace_coverage": analysis["native_trace_coverage"],
        "supervisor_memory_cpu_reported": result["supervisor_memory_cpu"],
        "supervisor_scope": "Retained producer summary; no supervisor per-row trace for independent reconciliation.",
        "runtime_policy_checks": analysis["runtime_policy_checks"],
        "source_entries": audit["source_entries"], "stable_proof_pin_entries": audit["proof_pin_entries"],
        "independent_audit_checks": audit["check_count"], "cleanup_complete": True,
        "verification": "flash-60minute-02/verification.json",
        "result": "flash-60minute-02/run/result.json",
        "analysis": "flash-60minute-02/flash-60minute-laptop-02-analysis.json",
        "independent_audit": "flash-60minute-02/flash-60minute-laptop-02-independent-audit.json",
        "scope": "All 611 accepted deliveries retained. Nearest-rank continuous service p99 passes by70.074107ms, with5/600 continuous target exceedances and a7705.374736ms maximum. Independent silent audio/software Harness and a virtual playback clock do not establish integrated stem playback, GPU preemption, physical latency or full M2 acceptance. Control recovery remains separate. No causal comparison to previous runs."
    }
    require("sustained_validation" not in master["flash_attention_candidate"], "Hour already recorded")
    master["flash_attention_candidate"]["sustained_validation"] = screen
    master["flash_attention_candidate"]["status"] = "tested_excerpts_human_accepted_sixty_minute_service_target_passed_integration_pending"
    master["status"] = "flash_sixty_minute_service_target_passed_integration_pending"
    master["live_release_gate"] = "pending integrated stem playback/fallback and remaining M2 work; the3600s independent-load run passes strict4095ms continuous service p99 with70.074107ms margin"
    master["previous_retained_completed_live_validation_p99_ms"] = 2776.3940359582193
    master["latest_retained_completed_live_validation_p99_ms"] = 4024.9258929979987
    master["latest_retained_completed_live_validation_record"] = "flash-60minute-02/verification.json"
    master["pending"] = "Implement and validate integrated stem playback/fallback, then remaining M2 analysis/cue and prepared/live-stem mixing. The hour passes the unchanged service p99 target under the recorded conditions;5continuous target exceedances,7.705s maximum and up to11.211s explicit control recovery remain. M2 release is pending."
    for name, record in files.items():
        require(name not in master["files"], "Master entry already exists: " + name)
        master["files"][name] = record
    require(read(MASTER) == original, "Master changed before publication")
    with tempfile.NamedTemporaryFile(prefix=".verification-flash-hour-", dir=PARENT, delete=False) as stream:
        stream.write((json.dumps(master, indent=2, allow_nan=False) + "\n").encode())
        temporary = Path(stream.name)
    try:
        os.replace(temporary, MASTER)
    finally:
        temporary.unlink(missing_ok=True)
    print(json.dumps({"status": "updated", "archive_files_added": len(files),
                      "master_file_records": len(master["files"]), "M2_qualified": False}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

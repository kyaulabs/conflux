#!/usr/bin/env python3
"""Archive the closed Flash ABBA metadata; no workload, media or binary access."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
RUN = BASE / "flash-cost-abba-laptop-01"
PARENT = ROOT / "docs/benchmarks/data/m2-graph-phase-2026-10-18"
OUT = PARENT / "flash-cost-abba-01"
MASTER = PARENT / "verification.json"
LEGS = ("A1", "B1", "B2", "A2")
TOP = (
    "flash-cost-expected-pins.json", "flash-cost-expected-pins.template.json",
    "flash-cost-source-pins.json", "flash-cost-human-readiness.json",
    "flash-cost-human-readiness.template.json", "flash-cost-preparation-verification.json",
    "flash-cost-offline-check-result.json", "flash-cost-readiness-offline-result.json",
    "flash-cost-abba-laptop-01-independent-audit.json",
    "flash-cost-abba-laptop-01-independent-timing-analysis.json",
    "flash-cost-abba-laptop-01-gitleaks.log", "flash_cost_runtime.py", "flash_cost_launch.py",
    "flash_cost_offline_check.py", "analyze_flash_cost_abba.py", "archive_flash_cost_abba.py",
    "counterbalanced_runtime.py", "counterbalanced_launch.py", "compare_runtime.py",
    "graph_order_compare.py", "graph_order_compare_launch.py", "observed_launch.py",
    "flash-cost-render-01/runtime.py", "flash-cost-render-01/request-loop.py",
    "flash-cost-render-01/launcher.py", "flash-cost-render-01/provenance.json",
)
PENDING = ("Run the 600-second loaded Flash diagnostic after fresh minimized/AC/open-lid/visible-Harness "
           "readiness. The ordinary-cost ABBA screen is complete, but A2 low-power observations confound "
           "the pooled timing difference. The strict 4095 ms service p99, sustained qualification, integrated "
           "stem playback/fallback and remaining M2 musical analysis/cue work stay open. Latest completed "
           "loaded service p99 remains 5674.156680 ms.")


def require(value, message):
    if not value:
        raise ValueError(message)


def pin(raw):
    return {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def read(path):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= 4 * 1024 * 1024,
            "Not a bounded regular metadata file: " + str(path))
    before = path.stat()
    raw = path.read_bytes()
    after = path.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
            "Source changed while reading: " + str(path))
    raw.decode("utf-8")
    return raw


def encode(value):
    return (json.dumps(value, indent=2, allow_nan=False) + "\n").encode()


def inputs():
    pairs = [(BASE / name, name) for name in TOP]
    pairs += [(RUN / name, "run/" + name) for name in ("session.json", "AC-lid-monitor.jsonl")]
    for leg in LEGS:
        for name in (leg + ".stdout", leg + ".stderr", leg + "/result.json",
                     leg + "/diagnostic.stderr", leg + "/native-trace.jsonl"):
            pairs.append((RUN / name, "run/" + name))
        for name in ("stdout", "stderr", "admission-normal-ready.json",
                     "admission-normal-ready.jsonl", "fresh-admission.json"):
            relative = leg + "-admission/" + name
            pairs.append((RUN / relative, "run/" + relative))
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_plan", "input_files": len(inputs()), "destination": str(OUT),
                          "scope": "Explicit text metadata/helper/log allowlist; deterministic gzip JSONL. No PCM, audio, model or binary."}))
        return 0
    require(not OUT.exists(), "Archive destination already exists")
    master_raw = read(MASTER)
    master = json.loads(master_raw)
    require(master["latest_retained_completed_live_validation_p99_ms"] == 5674.156680004671,
            "Historical loaded p99 changed")
    captured = {target: (source, read(source)) for source, target in inputs()}
    session = json.loads(captured["run/session.json"][1])
    audit = json.loads(captured["flash-cost-abba-laptop-01-independent-audit.json"][1])
    analysis = json.loads(captured["flash-cost-abba-laptop-01-independent-timing-analysis.json"][1])
    require(session["status"] == "completed" and session["completed_requests"] == 16
            and audit["status"] == "evidence_consistent" and audit["check_count"] == 51
            and audit["proofs"]["each_leg_matches_own_closed_arm"] is True
            and audit["cleanup"]["complete"] is True
            and analysis["status"] == "descriptive_analysis_complete"
            and analysis["all_observations_included"] is True, "Closed evidence is incomplete")
    sources = json.loads(captured["flash-cost-source-pins.json"][1])
    for target, (source, raw) in captured.items():
        if str(source) in sources:
            require(pin(raw)["sha256"] == sources[str(source)], "Captured source pin differs: " + target)
    for name, record in analysis["source_artifacts"].items():
        matches = [raw for source, raw in captured.values() if str(source) == record["path"]]
        require(len(matches) == 1 and pin(matches[0]) == {key: record[key] for key in ("bytes", "sha256")},
                "Analysis raw artifact differs: " + name)
    for label, path in (
        ("closed_quality_proof_sha256", PARENT / "flash-f32-01/run/comparison/result.json"),
        ("flash_build_sha256", PARENT / "flash-f32-01/flash-f32-build-verification.json"),
        ("immutable_baseline_proof_sha256", PARENT / "pcm-comparison.json"),
    ):
        require(pin(read(path))["sha256"] == audit["identity"][label], "Existing canonical proof differs: " + label)
    files, mappings, transforms = {}, [], []
    with tempfile.TemporaryDirectory(prefix=".flash-cost-abba-01-", dir=PARENT) as temporary:
        stage = Path(temporary) / "archive"
        stage.mkdir()
        for target, (source, raw) in captured.items():
            archived = raw
            if target.endswith(".jsonl"):
                rows = [json.loads(line) for line in raw.splitlines() if line.strip()]
                stream = io.BytesIO()
                with gzip.GzipFile(filename="", mode="wb", fileobj=stream, compresslevel=9, mtime=0) as gz:
                    gz.write(raw)
                archived = stream.getvalue()
                target += ".gz"
                require(gzip.decompress(archived) == raw, "JSONL gzip roundtrip differs")
                transforms.append({"archive": target, "original_path": str(source), **pin(raw), "rows": len(rows),
                                   "method": "gzip level9, mtime0, empty filename; original bytes unchanged"})
            destination = stage / target
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(archived)
            files[target] = pin(archived)
            mappings.append({"archive": target, "original_path": str(source), "original": pin(raw),
                             "archived": pin(archived)})
        manifest = {"schema": 1, "status": "closed_metadata_archive", "files": mappings,
                    "JSONL_transforms": transforms,
                    "historical_receipt_scope": "Preparation receipts retain their original pending/non-executed status verbatim; the later session and audits establish completion.",
                    "exclusions": "No PCM, WAV, FLAC, model, binary or generated build tree. Original absolute paths remain evidence identifiers, not portable execution paths."}
        raw = encode(manifest)
        (stage / "manifest.json").write_bytes(raw)
        files["manifest.json"] = pin(raw)
        screen = {
            "status": "ordinary_cost_screen_complete_loaded_diagnostic_pending",
            "repository_head": audit["source_head"], "order": list(LEGS), "completed_requests": 16,
            "source_start_frame": 257985, "requests_per_leg": 4, "elapsed_seconds": session["elapsed_seconds"],
            "all_observations_included": True, "own_arm_PCM_repeat_exact": True, "cross_arm_exact_pcm": False,
            "request_wall_ms": analysis["arm_comparison"]["request_wall_ms"],
            "graph_compute_ms": analysis["arm_comparison"]["graph_compute_ms"],
            "paired_cases": analysis["paired_cases"],
            "startup_by_arm_ms": analysis["startup_by_arm"],
            "native_profile_point_counts": {"balanced": 41, "low-power": 16},
            "confound": "All A2 native profile points are low-power; A1/B1/B2 points are balanced. Pooled timing is descriptive and does not isolate Flash causally.",
            "native_telemetry_samples": 57, "supervisor_samples": 4, "startup_native_samples": 0,
            "telemetry_scope": analysis["telemetry_scope"],
            "independent_audit_checks": audit["check_count"],
            "sources_unchanged": True, "cleanup_complete": True,
            "latest_completed_loaded_service_p99_ms": 5674.156680004671,
            "strict_service_target_ms": 4095, "loaded_p99_pass": False,
            "audio_load": False, "UI_load": False, "cancellation_tested": False,
            "scope": analysis["scope"], "next_action": PENDING,
            "session": "run/session.json", "analysis": "flash-cost-abba-laptop-01-independent-timing-analysis.json",
            "independent_audit": "flash-cost-abba-laptop-01-independent-audit.json",
        }
        verification = {"schema": 1, "status": "archived_ordinary_cost_screen_only", "files": files,
                        "screen": screen, "JSONL_transforms": transforms,
                        "previous_proofs": {"closed_quality": "../flash-f32-01/run/comparison/result.json",
                                            "baseline": "../pcm-comparison.json",
                                            "flash_build": "../flash-f32-01/flash-f32-build-verification.json"},
                        "secret_scan": {"raw_run_log": "flash-cost-abba-laptop-01-gitleaks.log",
                                        "archive_scan": "not_run_by_archiver"}}
        raw = encode(verification)
        (stage / "verification.json").write_bytes(raw)
        files["verification.json"] = pin(raw)
        require(read(MASTER) == master_raw, "Master verification changed during archive preparation")
        os.rename(stage, OUT)
    master["status"] = "flash_ordinary_cost_screen_completed_loaded_validation_pending"
    master["pending"] = PENDING
    master["flash_attention_candidate"]["status"] = "tested_excerpts_human_accepted_ordinary_cost_screen_complete_loaded_pending"
    master["flash_attention_candidate"]["ordinary_cost_screen"] = {
        **screen, "verification": "flash-cost-abba-01/verification.json",
        "session": "flash-cost-abba-01/run/session.json",
        "analysis": "flash-cost-abba-01/flash-cost-abba-laptop-01-independent-timing-analysis.json",
        "independent_audit": "flash-cost-abba-01/flash-cost-abba-laptop-01-independent-audit.json"}
    for target, record in files.items():
        master["files"]["flash-cost-abba-01/" + target] = record
    with tempfile.NamedTemporaryFile(prefix=".verification-", dir=PARENT, delete=False) as temporary:
        temporary.write(encode(master))
        staged_master = Path(temporary.name)
    try:
        os.replace(staged_master, MASTER)
    finally:
        staged_master.unlink(missing_ok=True)
    print(json.dumps({"status": "archived", "destination": str(OUT), "files": len(files),
                      "bytes": sum(record["bytes"] for record in files.values()),
                      "JSONL_transforms": len(transforms), "master": str(MASTER)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

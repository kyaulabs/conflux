#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Verify packaged bytes and replay only saved-evidence audits in temporary files.

No host, benchmark, GPU, audio or policy command is invoked. Python subprocesses
execute the three exact audit sources included in the archive, after hash checks.
"""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

BASE = Path(__file__).resolve().parent
EXPECTED_HEAD = "da9575c5c66085570653d88108e4cbdc81a92de4"
SOAK_SHA = "d5d0e5687f93e1597add7c8a28a96a6495dac04143fe578ea4f5a0a00b1d404f"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def load(path):
    return json.loads(path.read_text())


def identity(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest(), path.stat().st_size


def canonical(report, kind):
    value = json.loads(json.dumps(report))
    if kind == "cadence":
        value.pop("audited_at_utc", None)
        value.pop("inputs", None)
    elif kind == "worker":
        value.get("source_snapshot_verified", {}).pop("path", None)
        value.get("source_evidence", {}).pop("events_stored_path", None)
    elif kind == "thermal":
        value.get("source_evidence", {}).pop("trace_stored_path", None)
    elif kind == "native":
        value.pop("inputs", None)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=BASE / "staged-archive")
    parser.add_argument("--output", type=Path, default=BASE / "archived-replay-verification.json")
    args = parser.parse_args()
    archive, output = args.archive.resolve(), args.output.resolve()
    require(archive not in output.parents, "Replay report must remain outside the immutable archive")
    manifest = load(archive / "manifest.json")
    provenance = load(archive / "source-provenance.json")
    require(manifest["repository_head"] == EXPECTED_HEAD
            and provenance["repository_head_from_completed_results"] == EXPECTED_HEAD,
            "Unexpected producer source HEAD")
    entries = manifest["files"]
    actual = {str(path.relative_to(archive)) for path in archive.rglob("*") if path.is_file()}
    require(actual == set(entries) | {"manifest.json"}, "Archive contains missing or unmanifested files")
    verified = []
    audit_replays = []
    with tempfile.TemporaryDirectory(prefix="conflux-software-replay-") as temporary:
        root = Path(temporary)
        restored = {}
        for filename, entry in sorted(entries.items()):
            source = archive / filename
            require(source.is_file() and not source.is_symlink(), "Expected plain archive file")
            require(identity(source) == (entry["sha256"], entry["bytes"]), "Stored identity mismatch: " + filename)
            target_name = entry.get("replay_target")
            if target_name is None:
                verified.append(filename)
                continue
            relative = Path(target_name)
            require(not relative.is_absolute() and ".." not in relative.parts, "Unsafe replay path")
            target = root / relative
            expected = (entry["raw_sha256"], entry["raw_bytes"])
            if target_name in restored:
                require(restored[target_name] == expected, "Conflicting replay sources")
                verified.append(filename)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            if entry.get("compression") == "gzip":
                with gzip.open(source, "rb") as src, target.open("wb") as dst:
                    shutil.copyfileobj(src, dst, length=1024 * 1024)
            else:
                shutil.copyfile(source, target)
            require(identity(target) == expected, "Raw identity mismatch: " + filename)
            restored[target_name] = expected
            verified.append(filename)
        work = root / "benchmarks-local/software-ui-2026-10-17"
        require((work.parent / "cadence-2026-10-17/33ms-both-hour/result.json").is_file(),
                "Explicit prior-cadence Vulkan dependency is missing")
        require(identity(root / "tools/benchmarks/stem_soak.py")[0] == SOAK_SHA, "Soak snapshot is not frozen producer")
        results = root / "replay-results"
        results.mkdir()
        audit_sources = {"cadence": work / "cadence_audit.py", "worker": work / "completed-run-audit.py",
                         "thermal": work / "thermal-snapshot-audit.py"}
        for source in audit_sources.values():
            require(source.is_file(), "Packaged audit source missing: " + str(source))
        for metadata in provenance["completed_runs"]:
            name = metadata["label"]
            run = work / name
            stored_cadence = load(work / (name + "-cadence-audit.json"))
            require(identity(audit_sources["cadence"])[0] == stored_cadence["audit_source_sha256"],
                    "Cadence audit bytes differ from executed report source")
            commands = {
                "cadence": [str(audit_sources["cadence"]), name],
                "worker": [str(audit_sources["worker"]), "--run-directory", str(run),
                           "--expected-head", EXPECTED_HEAD, "--expected-soak-sha256", SOAK_SHA,
                           "--source-snapshot", str(root / "tools/benchmarks/stem_soak.py")],
                "thermal": [str(audit_sources["thermal"]), "--run-directory", str(run)],
            }
            comparison = stored_cadence.get("comparison")
            if comparison:
                commands["cadence"] += ["--compare-control", comparison["control"]]
            if metadata["seconds"] == 900:
                commands["worker"].append("--comparison-short")
            for kind, command in commands.items():
                destination = results / (name + "-" + kind + ".json")
                completed = subprocess.run([sys.executable, "-B", *command, "--output", str(destination)],
                                           cwd=root, capture_output=True, text=True, timeout=120)
                require(completed.returncode == 0, f"Packaged {kind} audit failed for {name}: " + completed.stderr[-4000:])
                report = load(destination)
                saved_name = {"cadence": "cadence-audit", "worker": "independent-audit",
                              "thermal": "thermal-snapshot-audit"}[kind]
                saved_path = work / (name + "-" + saved_name + ".json")
                matched = None
                if saved_path.exists():
                    saved = load(saved_path)
                    matched = canonical(saved, kind) == canonical(report, kind)
                    require(matched, f"Packaged {kind} replay differs from saved facts for {name}")
                facts = ({"status": report["status"], "failed_checks": report["failed_checks"],
                          "frame_rate": report["frame_rate"], "indicator_x_range": report["motion"]["indicator_x_range"],
                          "continuous_timing": report["continuous_timing"]}
                         if kind == "cadence" else
                         {"timing": report["timing"], "control_count": report["controls"]["complete"],
                          "audio": report["audio"], "UI": report["UI"]}
                         if kind == "worker" else
                         {"trace_samples_in_workload": report["trace_samples_in_workload"],
                          "sampled_sensor_types": report["sampled_sensor_types"],
                          "package_sensor": report["package_sensor"], "any_sampled_sensor": report["any_sampled_sensor"]})
                audit_replays.append({"run": name, "audit": kind, "returncode": completed.returncode,
                    "audit_source_sha256": identity(audit_sources[kind])[0],
                    "saved_report_present": saved_path.exists(), "saved_semantic_facts_reproduced": matched,
                    "canonical_replay_sha256": hashlib.sha256(json.dumps(canonical(report, kind), sort_keys=True).encode()).hexdigest(),
                    "facts": facts})
        # Descriptive native-tail reports are optional and are never promoted
        # into acceptance gates. Replay their exact packaged source when saved.
        native_source = work / "native-tail-analysis.py"
        for metadata in provenance["completed_runs"]:
            name = metadata["label"]
            saved_path = work / (name + "-native-tail-analysis.json")
            if not native_source.exists() or not saved_path.exists():
                continue
            saved = load(saved_path)
            require(identity(native_source)[0] == saved["analysis_source_sha256"], "Native-tail source identity differs")
            destination = results / (name + "-native.json")
            completed = subprocess.run([sys.executable, "-B", str(native_source), name,
                                       "--gpu-frequency-path", saved["native_snapshots"]["gpu_frequency_path"],
                                       "--output", str(destination)], cwd=root,
                                       capture_output=True, text=True, timeout=120)
            require(completed.returncode == 0, "Packaged native-tail replay failed: " + completed.stderr[-4000:])
            native = load(destination)
            require(canonical(saved, "native") == canonical(native, "native"), "Native-tail facts differ after replay")
            audit_replays.append({"run": name, "audit": "native descriptive", "returncode": 0,
                "audit_source_sha256": identity(native_source)[0], "saved_report_present": True,
                "saved_semantic_facts_reproduced": True,
                "canonical_replay_sha256": hashlib.sha256(json.dumps(canonical(native, "native"), sort_keys=True).encode()).hexdigest(),
                "facts": {"continuous": native["populations"]["continuous"], "native_snapshots": native["native_snapshots"]}})
    report = {"schema": 1, "replayed_at_utc": datetime.now(timezone.utc).isoformat(), "status": "ok",
              "archive_manifest_sha256": identity(archive / "manifest.json")[0],
              "verified_files": len(verified), "repository_head": EXPECTED_HEAD,
              "audit_replays": audit_replays, "failed_attempt_labels_retained": provenance["failed_attempt_labels"],
              "comparison_scope": "Each completed900second comparison reruns the explicit observed comparison-short event audit with exact2controls. An archived software3600second hour reruns the exact11control default audit. No legacy-smoke qualification or absent-hour claim is used.",
              "software_hour_completed": provenance["software_hour_completed"],
              "scope": "Verified stored/decompressed identities and reran packaged saved-evidence audits in a temporary layout. Audit timestamps and temporary path labels are excluded from semantic comparison. No host queries, producer workloads or PCM recomputation.",
              "replay_source_sha256": identity(Path(__file__))[0]}
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"output": str(output), "status": "ok", "files": len(verified),
                      "audits": len(audit_replays), "saved_reports_reproduced": sum(row["saved_semantic_facts_reproduced"] is True for row in audit_replays)}, indent=2))


if __name__ == "__main__":
    main()

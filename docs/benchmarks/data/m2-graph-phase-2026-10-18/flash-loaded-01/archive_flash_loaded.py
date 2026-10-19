#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Archive one CLOSED loaded diagnostic's text evidence; dry by default."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import tempfile

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
NAME = "flash-loaded-laptop-01"
RUN = BASE / NAME
PARENT = ROOT / "docs/benchmarks/data/m2-graph-phase-2026-10-18"
OUT = PARENT / "flash-loaded-01"
TOP = (
    "flash-loaded-expected-pins.json", "flash-loaded-expected-pins.template.json",
    "flash-loaded-source-pins.json", "flash-loaded-human-readiness.json",
    "flash-loaded-human-readiness.template.json", "flash-loaded-preparation-verification.json",
    "flash-loaded-child-offline-mock-result.json", "flash-loaded-wrapper-offline-mock-result.json",
    "flash-loaded-root-preflight.json", "flash-loaded-unarmed-offline-result.json",
    "flash_loaded_launch.py", "flash_loaded_window_guard.py", "flash_loaded_observer.py",
    "prepare_flash_loaded.py", "flash_loaded_child_review_mocks.py", "flash_loaded_wrapper_review_mocks.py",
    "analyze_flash_loaded.py", "analyze_result.py", "audit_flash_loaded.py", "archive_flash_loaded.py",
    NAME + "-analysis.json", NAME + "-independent-audit.json", NAME + "-gitleaks.log",
    "flash-loaded-render-02/observed-launch.py", "flash-loaded-render-02/observer_source.py",
    "flash-loaded-render-02/window_guard_source.py", "flash-loaded-render-02/provenance.json",
)
RUN_FILES = ("result.json", "events.jsonl", "native-trace.jsonl", "progress.json",
             "ui-progress.json", "ui.json", "audio.json", "ui.stderr", "audio.stderr")
ADMISSION_FILES = ("stdout", "stderr", "admission-normal-ready.json", "admission-normal-ready.jsonl", "fresh-admission.json")
REFERENCES = {
    "closed_quality": (PARENT / "flash-f32-01/run/comparison/result.json",
        "917104a1cc8ca2522eef38bae6d620c972fdb2be826876b2dee88e67dd7a497a"),
    "immutable_baseline": (PARENT / "pcm-comparison.json",
        "38c60301feed297a0484d61ea92cca2413067d15d7ead7e9a27e6cc9909037e3"),
    "flash_build": (PARENT / "flash-f32-01/flash-f32-build-verification.json",
        "9c7d503af0b0c9b4c1b4e7e8c533fe888376c2a45f19942529f2e01247d7053d"),
    **{name: (PARENT / "flash-cost-abba-01" / name, digest) for name, digest in (
        ("flash_cost_runtime.py", "ca0ad3468d20c2ae28d06476b995f50d49823c64ed6c57106aea059bf65baf71"),
        ("counterbalanced_runtime.py", "f905f12613344dd0c0c68265a0937621910538099cf51bf9d1bf98a80e458fd3"),
        ("counterbalanced_launch.py", "f9c53b640d3dc0c992bbbd14e389baf5230b1c9a0a9ff8491f125e3d672e26bf"),
        ("compare_runtime.py", "5022aeed8eaf932adead05404f80b94c7d992f4b6f68dc81d05edb6126077e5b"),
        ("graph_order_compare.py", "28d2712ba427d0f480ad63f8ff3128f0d3999010e7ecf225185c41734861a3e7"),
        ("graph_order_compare_launch.py", "f3a7073e0774f62597d24e5b9e6e49a594f13c0e6429a339947b467cd5a6a6b4"),
        ("observed_launch.py", "511bb712d2f7af01abb1ab25ca92b001c6f82af4ee6f42547a5b43fa32b237d6"))},
    **{name: (PARENT / "flash-f32-01" / name, digest) for name, digest in (
        ("flash_quality.py", "cec1a694a32da562be9ed79d35cfda7e2ee1943b9b7dfd10daf1ed2259fc7495"),
        ("flash_quality_launch.py", "ab912a468791b13ca283bacd84ce30163ed1c7a0576fd099dc859fd4670294c4"))},
    "original_window_guard": (ROOT / "docs/benchmarks/data/software-ui-2026-10-17/source-snapshots/tools/benchmarks/window_guard.py",
        "077bd16eefbdd9b80f787b6aeb2837a3790fedf7f5defd7b140dccc6e1f5cebb"),
    "original_observer": (ROOT / "docs/benchmarks/data/software-ui-2026-10-17/source-snapshots/benchmarks-local/software-ui-2026-10-17/observed-run.py",
        "a6a4b81014f6aec16c8cd68e902348afcd10a1fdf5966f6b2a54ba2f622ef394"),
}
ANALYZER_SHA = "8be73b37e86140dbc24e23e8735476e672e97d073361b1311d5e6a6502439592"
ANALYZER_HELPER_SHA = "44188eb171a5309403380c7a02c3ca96c31e6532923afaf9d5fce870607c9092"


def require(value, message):
    if not value:
        raise ValueError(message)


def pin(raw):
    return {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def identity(value):
    return (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)


def read(path):
    maximum = 512 * 1024 * 1024 if path.name == "native-trace.jsonl" else 64 * 1024 * 1024
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode) and before.st_size <= maximum,
                "Not bounded regular text metadata: " + str(path))
        raw = stream.read(before.st_size + 1)
        require(len(raw) == before.st_size and identity(before) == identity(os.fstat(stream.fileno()))
                == identity(os.stat(path, follow_symlinks=False)), "Input changed while reading: " + str(path))
    raw.decode("utf-8")
    return raw, identity(before)


def decode(raw):
    def invalid(value):
        raise ValueError("Nonfinite JSON constant: " + value)
    return json.loads(raw, parse_constant=invalid)


def encode(value):
    return (json.dumps(value, indent=2, allow_nan=False) + "\n").encode()


def inputs(extra):
    pairs = [(BASE / name, name) for name in TOP]
    pairs += [(RUN / name, "run/" + name) for name in RUN_FILES]
    pairs += [(BASE / (NAME + suffix), "lifecycle/" + NAME + suffix)
              for suffix in ("-launch.json", "-policy.json", "-guard.json", "-guard.js",
                             "-AC-lid-monitor.jsonl", ".stdout", ".stderr")]
    pairs += [(BASE / (NAME + "-admission") / name, "admission/" + name) for name in ADMISSION_FILES]
    pairs += [(path, "root-reports/" + path.name) for path in extra]
    require(len({target for _, target in pairs}) == len(pairs), "Duplicate archive target")
    require(len({str(source) for source, _ in pairs}) == len(pairs), "Duplicate archive input")
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--root-report", type=Path, action="append", default=[])
    args = parser.parse_args()
    require(all(path.is_absolute() and path.parent == BASE and path.name.startswith(NAME + "-")
                and path.suffix in (".json", ".log") for path in args.root_report),
            "Additional reports must be explicit same-run text JSON/logs in the experiment directory")
    pairs = inputs(args.root_report)
    if not args.run:
        print(json.dumps({"status": "dry_plan", "input_files": len(pairs), "destination": str(OUT),
              "files_read": False, "writes": False, "master_modified": False,
              "scope": "Closed metadata/helper/log allowlist only. Deterministic gzip events/native/readiness/AC-lid JSONL with original hashes and row counts. No audio, images, PCM, model, executable or build-tree access; no host/Git/native execution."}))
        return 0
    require(not OUT.exists() and not OUT.is_symlink(), "Use a fresh archive destination")
    # A mistakenly early invocation stops at this closed-state precondition and
    # never reads the active events, trace, progress or other workload outputs.
    launch_raw, _ = read(BASE / (NAME + "-launch.json"))
    launch = decode(launch_raw)
    require(launch.get("status") == "completed" and launch.get("finished_at_utc")
            and launch.get("returncode") == 0 and launch.get("failures") == [], "Launcher is not closed and complete")
    audit_raw, _ = read(BASE / (NAME + "-independent-audit.json"))
    analysis_raw, _ = read(BASE / (NAME + "-analysis.json"))
    audit, analysis = decode(audit_raw), decode(analysis_raw)
    require(audit.get("status") == "evidence_consistent" and audit.get("structural_completion") is True
            and audit.get("errors") == [] and audit.get("source_entries") == 64
            and audit.get("proof_pin_entries") == 68
            and analysis.get("status") == "closed_descriptive_analysis"
            and analysis.get("launcher_status") == "completed"
            and all(analysis.get("event_result_consistency", {}).values())
            and analysis.get("target", {}).get("all_observations_retained") is True,
            "Closed independent audit/reduction is incomplete")
    captured = {target: (source, *read(source)) for source, target in pairs}
    require(captured["lifecycle/" + NAME + "-launch.json"][1] == launch_raw
            and captured[NAME + "-independent-audit.json"][1] == audit_raw
            and captured[NAME + "-analysis.json"][1] == analysis_raw, "Closure evidence changed during capture")
    result = decode(captured["run/result.json"][1])
    config = decode(captured["flash-loaded-expected-pins.json"][1])
    sources = decode(captured["flash-loaded-source-pins.json"][1])
    require(result.get("status") == "ok" and result.get("finished_at_utc") and result.get("requested_seconds") == 600
            and result.get("workload_seconds", 0) >= 600 and config == launch["expected_producer"]
            and audit["source_head"] == config["repository_head"] == result["repository_head"]
            and launch["proof_source_pins_before"] == launch["proof_source_pins_after"], "Closed run/source identity differs")
    require(pin(captured["analyze_flash_loaded.py"][1])["sha256"] == ANALYZER_SHA
            and pin(captured["analyze_result.py"][1])["sha256"] == ANALYZER_HELPER_SHA
            and pin(captured["audit_flash_loaded.py"][1])["sha256"] == audit["auditor_sha256"],
            "Actual analysis/audit source differs")
    by_path = {str(source): raw for source, raw, _ in captured.values()}
    for source, raw, _ in captured.values():
        if str(source) in sources:
            require(pin(raw)["sha256"] == sources[str(source)], "Captured run-pinned source changed: " + str(source))
    references, reference_raw, reference_identities = {}, {}, {}
    for label, (path, digest) in REFERENCES.items():
        raw, before = read(path)
        require(pin(raw)["sha256"] == digest, "Existing canonical reference changed: " + label)
        references[label] = {"archive_relative_path": os.path.relpath(path, OUT), **pin(raw),
                             "scope": "Existing canonical bytes retained in place; not duplicated or rewritten."}
        reference_raw[digest] = raw
        reference_identities[path] = before
    for path, record in audit["files"].items():
        raw = by_path.get(path, reference_raw.get(record["sha256"]))
        require(raw is not None and pin(raw) == record, "Audited artifact not preserved or referenced: " + path)
    for label, record in analysis["artifacts_read"].items():
        require(record["path"] in by_path and pin(by_path[record["path"]]) == {
            key: record[key] for key in ("bytes", "sha256")}, "Analysis artifact changed: " + label)
    files, mappings, transforms = {}, [], []
    with tempfile.TemporaryDirectory(prefix=".flash-loaded-01-", dir=PARENT) as temporary:
        stage = Path(temporary) / "archive"
        stage.mkdir()
        for target, (source, raw, _) in captured.items():
            archived = raw
            if target.endswith(".jsonl"):
                rows = [decode(line) for line in raw.splitlines() if line.strip()]
                stream = io.BytesIO()
                with gzip.GzipFile(filename="", mode="wb", fileobj=stream, compresslevel=9, mtime=0) as gz:
                    gz.write(raw)
                archived = stream.getvalue()
                target += ".gz"
                require(gzip.decompress(archived) == raw, "JSONL compression roundtrip differs")
                transforms.append({"archive": target, "original_path": str(source), **pin(raw),
                    "json_records": len(rows), "physical_lines": len(raw.splitlines()),
                    "method": "gzip level9,mtime0,empty filename; decompressed bytes exactly equal original"})
            destination = stage / target
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(archived)
            files[target] = pin(archived)
            mappings.append({"archive": target, "original_path": str(source), "original": pin(raw), "archived": pin(archived)})
        manifest = {"schema": 1, "status": "closed_text_evidence_archive", "files": mappings,
            "JSONL_transforms": transforms, "canonical_references": references,
            "source_recovery": {"repository_head_reported": config["repository_head"],
                "retained_source_manifest": "flash-loaded-source-pins.json",
                "scope": "Tracked repository source is recoverable from the root-verified signed source HEAD and retained identities; no Git call or signature re-verification by archiver. Build-tree/model/executable identities remain metadata only."},
            "historical_scopes": "Original preparation/pending/not-executed/readiness/proof/check receipts remain byte-identical. Later closed result and independent reports establish what subsequently ran.",
            "exclusions": "No PCM/audio/image, model/executable or build-tree copy/read. No physical-audio, musical, latency or full60-minute qualification inferred by archiving."}
        raw = encode(manifest)
        (stage / "manifest.json").write_bytes(raw)
        files["manifest.json"] = pin(raw)
        verification = {"schema": 1, "status": "archived_closed600s_diagnostic_only", "files": files,
            "run": "run/result.json", "launch": "lifecycle/" + NAME + "-launch.json",
            "analysis": NAME + "-analysis.json", "independent_audit": NAME + "-independent-audit.json",
            "source_head": config["repository_head"], "workload_seconds": result["workload_seconds"],
            "latency_gate": launch["latency_gate"], "strict4095_gate_passed": audit["strict4095_gate_passed"],
            "full60_minute_qualified": audit["full_60minute_qualified"],
            "canonical_references": references, "JSONL_transforms": transforms,
            "secret_scan": {"raw_scan_log": NAME + "-gitleaks.log", "archive_scan": "not_run_by_archiver"},
            "scope": "Structural archival only. Actual strict target outcome remains as reported, distinct from run completion. No document narrative/master metadata modified; root decides subsequent reporting."}
        (stage / "verification.json").write_bytes(encode(verification))
        for source, _, before in captured.values():
            require(identity(os.stat(source, follow_symlinks=False)) == before, "Captured source changed before publication: " + str(source))
        for source, before in reference_identities.items():
            require(identity(os.stat(source, follow_symlinks=False)) == before, "Canonical reference changed before publication: " + str(source))
        os.rename(stage, OUT)
    print(json.dumps({"status": "archived_closed600s_diagnostic_only", "destination": str(OUT),
        "copied_files": len(captured), "JSONL_transforms": len(transforms), "canonical_references": len(references),
        "master_modified": False, "native_executed": False, "host_queried": False}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

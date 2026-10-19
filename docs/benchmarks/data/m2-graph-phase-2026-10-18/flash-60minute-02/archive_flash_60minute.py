#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Archive the CLOSED promoted hour diagnostic's text evidence; dry by default."""
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
NAME = "flash-60minute-laptop-02"
RUN = BASE / NAME
PARENT = ROOT / "docs/benchmarks/data/m2-graph-phase-2026-10-18"
OUT = PARENT / "flash-60minute-02"
TOP = (
    "flash-60minute-expected-pins.json", "flash-60minute-expected-pins.template.json",
    "flash-60minute-source-pins.json", "flash-60minute-human-readiness-03.json",
    "flash-60minute-preparation-verification.json", "flash-60minute-root-offline.json",
    "flash-60minute-proof-offline.json", "flash_60minute_delta_review.py",
    "flash_60minute_launch.py", "prepare_flash_60minute.py",
    "analyze_flash_60minute.py", "audit_flash_60minute.py", "archive_flash_60minute.py",
    "flash-promoted-built.json", "flash-promoted-source-binding.json",
    "flash-promoted-build-execution.json", "verify_promoted_flash_build.py",
    "promoted_flash_repro.py", "flash-promoted-repro-expected-pins.json",
    "flash-promoted-repro-root-verification.json", "flash-promoted-repro-01/result.json",
    "flash-promoted-repro-01/diagnostic.stderr", NAME + "-root-owner.json",
    NAME + "-agent-crosscheck.json", "update_flash_60minute_master.py",
    NAME + "-analysis.json", NAME + "-independent-audit.json", NAME + "-gitleaks.log",
    "flash-60minute-render-01/observed-launch.py", "flash-60minute-render-01/observer_source.py",
    "flash-60minute-render-01/window_guard_source.py", "flash-60minute-render-01/provenance.json",
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
    **{name: (PARENT / "flash-loaded-01" / name, digest) for name, digest in (
        ("flash_loaded_launch.py", "883d537f8f2fe91dc2e5fc5a735ca20067a007cbe8e308e9f1673825bf39e684"),
        ("flash_loaded_window_guard.py", "cd064312e996672fed81e570f2acc0c8efd40e0fa76520cb0b3566da560f18b1"),
        ("flash_loaded_observer.py", "a21b21408ecb1a1108d310eee5ebcbad0187c4894fa308b55c495be4f16a9664"),
        ("flash-loaded-child-offline-mock-result.json", "fa1cea4e65183f7f21a354aa178a85bc96ce984f636f1c5db951df4fcc4fe914"),
        ("flash-loaded-wrapper-offline-mock-result.json", "983137bb086baede60351a50edf568ab35a09bf3e816d5f706b799b52a5e5066"),
        ("analyze_result.py", "44188eb171a5309403380c7a02c3ca96c31e6532923afaf9d5fce870607c9092"))},
}
ANALYZER_SHA = "e4ae08aa915874dfbebd241a19f16a13d3d759297b30a00bc0e8aaba80eae1a4"
AUDITOR_SHA = "651b775781e640a3f3abc943f0f34b06b7f3dbbb312468fffcda95b05698da57"
AGENT_SHA = "076b6bc7f266472032b31d47e3a3c5dd0c318ee24beadbd2eca0cef7141a3b30"


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
            and audit.get("errors") == [] and audit.get("check_count") == len(audit.get("checks", []))
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
    config = decode(captured["flash-60minute-expected-pins.json"][1])
    sources = decode(captured["flash-60minute-source-pins.json"][1])
    require(result.get("status") == "ok" and result.get("finished_at_utc") and result.get("requested_seconds") == 3600
            and result.get("workload_seconds", 0) >= 3600 and config == launch["expected_producer"]
            and audit["source_head"] == config["repository_head"] == result["repository_head"]
            and launch["proof_source_pins_before"] == launch["proof_source_pins_after"], "Closed run/source identity differs")
    before = launch["proof_source_pins_before"]
    require(0 < len(sources) <= 64 and audit["source_entries"] == len(sources)
            and audit["proof_pin_entries"] == len(before)
            and {key[7:] for key in before if key.startswith("source:")} == set(sources)
            and all(before["source:" + path]["sha256"] == digest for path, digest in sources.items())
            and pin(captured["flash-60minute-source-pins.json"][1])["sha256"] == config["source_pins_sha256"],
            "Exact source/proof map cardinality or digest differs")
    require(pin(captured["analyze_flash_60minute.py"][1])["sha256"] == ANALYZER_SHA
            and pin(captured["audit_flash_60minute.py"][1])["sha256"] == audit["auditor_sha256"] == AUDITOR_SHA,
            "Actual analysis/audit source differs")
    require(pin(captured[NAME + "-agent-crosscheck.json"][1])["sha256"] == AGENT_SHA,
            "Independent closed-run agent receipt differs")
    build = decode(captured["flash-promoted-built.json"][1])
    reproduced = decode(captured["flash-promoted-repro-01/result.json"][1])
    binding = decode(captured["flash-promoted-source-binding.json"][1])
    require(pin(captured["flash-promoted-built.json"][1])["sha256"] == config["promoted_build_sha256"]
            and pin(captured["flash-promoted-repro-01/result.json"][1])["sha256"] == config["reproduction_proof_sha256"]
            and binding["source_head"] == reproduced["source_head"] == config["repository_head"]
            and binding["build_receipt_sha256"] == reproduced["build_receipt_sha256"] == config["promoted_build_sha256"]
            and build["runtime"]["sha256"] == reproduced["runtime_sha256"] == config["pins"]["runtime"]
            and pin(captured["verify_promoted_flash_build.py"][1])["sha256"] == build["verifier"]["sha256"]
            and pin(captured["promoted_flash_repro.py"][1])["sha256"] == reproduced["pins_before"]["driver"]["sha256"]
            and pin(captured["flash-promoted-repro-expected-pins.json"][1])["sha256"] == reproduced["pins_before"]["configuration"]["sha256"]
            and pin(captured["flash-promoted-repro-01/diagnostic.stderr"][1])["sha256"] == reproduced["flash_evidence"]["stderr_pin"]["sha256"],
            "Promoted build/reproduction/source binding or actual helper bytes differ")
    gate = launch["latency_gate"]
    require(audit["cancellation_batches"] == len(audit["control_identity_rows"]) == len(result["controls"]) == 11
            and gate["passed"] == audit["strict4095_gate_passed"]
            and gate["producer_full_audio_UI_soak_qualified"] == audit["producer_full_audio_UI_soak_qualified"]
                == result["acceptance"]["full_audio_UI_soak_qualified"]
            and gate["integrated_stem_playback_qualified"] is audit["integrated_stem_playback_qualified"] is False,
            "Control coverage or separate qualification scope differs")
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
    with tempfile.TemporaryDirectory(prefix=".flash-60minute-02-", dir=PARENT) as temporary:
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
                "retained_source_manifest": "flash-60minute-source-pins.json",
                "scope": "Tracked repository source is recoverable from the root-verified signed source HEAD and retained identities; no Git call or signature re-verification by archiver. Build-tree/model/executable identities remain metadata only."},
            "historical_scopes": "Original preparation/pending/not-executed/readiness/proof/check receipts remain byte-identical. Later closed result and independent reports establish what subsequently ran.",
            "exclusions": "No PCM/audio/image, model/executable or build-tree copy/read. No physical-audio, musical, integrated stem-playback or complete M2 qualification inferred by archiving. Producer hour/audio/UI and strict target outcomes are retained separately."}
        raw = encode(manifest)
        (stage / "manifest.json").write_bytes(raw)
        files["manifest.json"] = pin(raw)
        verification = {"schema": 1, "status": "archived_closed3600s_independent_load_diagnostic", "files": files,
            "run": "run/result.json", "launch": "lifecycle/" + NAME + "-launch.json",
            "analysis": NAME + "-analysis.json", "independent_audit": NAME + "-independent-audit.json",
            "source_head": config["repository_head"], "workload_seconds": result["workload_seconds"],
            "latency_gate": launch["latency_gate"], "strict4095_gate_passed": audit["strict4095_gate_passed"],
            "producer_full_audio_UI_soak_qualified": audit["producer_full_audio_UI_soak_qualified"],
            "integrated_stem_playback_qualified": False, "M2_qualified": False,
            "canonical_references": references, "JSONL_transforms": transforms,
            "secret_scan": {"raw_scan_log": NAME + "-gitleaks.log", "archive_scan": "not_run_by_archiver"},
            "scope": "Structural archival only. Actual strict target outcome remains as reported, distinct from run completion. No document narrative/master metadata modified; root decides subsequent reporting."}
        (stage / "verification.json").write_bytes(encode(verification))
        for source, _, before in captured.values():
            require(identity(os.stat(source, follow_symlinks=False)) == before, "Captured source changed before publication: " + str(source))
        for source, before in reference_identities.items():
            require(identity(os.stat(source, follow_symlinks=False)) == before, "Canonical reference changed before publication: " + str(source))
        os.rename(stage, OUT)
    print(json.dumps({"status": "archived_closed3600s_independent_load_diagnostic", "destination": str(OUT),
        "copied_files": len(captured), "JSONL_transforms": len(transforms), "canonical_references": len(references),
        "master_modified": False, "native_executed": False, "host_queried": False}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

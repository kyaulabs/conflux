#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Package completed UI profiling evidence without running host commands.

This helper is deliberately inert until invoked after the full hour and common
guard cleanup. It only reads saved files and writes a fresh ignored staging tree.
"""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import shutil


BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
EXPECTED_HEAD = "38be36aba0232ba3e47691ddb091bf80274f4a7a"
FROZEN_PINS = {
    "runtime": "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6",
    "model": "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6",
    "soak_source": "52605983fdd2416caf63477bc9314096a0daa4f251e798c92928be19e5fea8b8",
    "shared_benchmark_source": "a9b7cc5904883460baf7ef7e19b7a5f227f676945283f33564e2ade6d906e256",
    "worker_source": "308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8",
    "sampler_source": "8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b",
    "ui": "011c4ced62384ca0364c8381db22bcc619b18f2e8c7b86a8fa79cf7adfedf01e",
    "audio_probe": "b8cbfb0a255c94cf8430b9d7ef97823a9e2f3b11eedf32b3dec179ba1d197ea2",
}
WINDOW_GUARD_SHA = "077bd16eefbdd9b80f787b6aeb2837a3790fedf7f5defd7b140dccc6e1f5cebb"
RUN_FILES = (
    "result.json", "progress.json", "events.jsonl", "native-trace.jsonl",
    "ui.json", "ui-progress.json", "ui.stderr", "audio.json", "audio.stderr",
)
EXTERNAL_FILES = {
    "before.json": "before-{label}.json",
    "after.json": "after-{label}.json",
    "launcher.stdout": "{label}.stdout",
    "launcher.stderr": "{label}.stderr",
    "launch.json": "{label}-launch.json",
    "admission.json": "{label}-admission.json",
    "normal-ready.json": "{label}-normal-ready.json",
    "normal-ready.jsonl": "{label}-normal-ready.jsonl",
    "guard.json": "{label}-guard.json",
    "guard.js": "{label}-guard.js",
    "policy-lifecycle.json": "{label}-policy.json",
    "cleanup-host.json": "{label}-cleanup.json",
    "kernel-compact.json": "{label}-kernel.json",
    "thermal.json": "{label}-thermal.json",
    "verification.json": "{label}-verification.json",
    "AC-monitor.jsonl": "{label}-AC-monitor.jsonl",
}
DIAGNOSTIC_FILES = {
    "qt-timing-capture.json": "{label}-qt-timing-capture.json",
    "qt-timing.jsonl": "{label}-qt-timing.jsonl",
    "ui-process.jsonl": "{label}-ui-process.jsonl",
    "ui-profile-summary.json": "{label}-ui-profile-summary.json",
}
OPTIONAL_RUN_FILES = {
    "ui-process.jsonl": "{label}-ui-process.jsonl",
    "independent-audit.json": "{label}-independent-audit.json",
    "thermal-snapshot-audit.json": "{label}-thermal-snapshot-audit.json",
    "profile-audit.json": "{label}-profile-audit.json",
    "archived-replay-verification.json": "{label}-archived-replay-verification.json",
    "cooldown.json": "{label}-cooldown.json",
    "cooldown.jsonl": "{label}-cooldown.jsonl",
}
SERIES_FILES = (
    "profile-series-guard.json", "profile-series-wait.json",
    "ui-profile-parser-revision.json", "ui_profile_summary.v1.py",
    "ui_profile_summary.py", "diagnostic-opengl-ui-profile-summary.v1.json",
    "baseline-stage-audit.py", "baseline-stage-audit.json",
    "hour-sequence.json", "diagnostic-comparison.py", "diagnostic-comparison.json",
)
OPTIONAL_SERIES_FILES = (
    "profile-request.json", "profile-preflight.json", "series-state.json",
    "profile-series-cleanup.json", "ui-profile-comparison.json",
    "ui-profile-protocol-audit.json", "profile-audit-verification.json",
    "hour-analysis.py", "hour-analysis.json",
    "resume-audit-verification.json", "thermal-snapshot-audit-verification.json",
    "thermal-assessment-user-guidance.json", "audio-assessment-user-guidance.json",
)
ANALYSIS_HELPERS = (
    "finish_trial.py", "kernel_capture.py", "thermal_capture.py",
    "thermal-snapshot-audit.py", "ui_process_profile.py", "qt_timing_capture.py",
    "watch.py", "series_wait.py", "hour_sequence.py", "cooldown.py", "monitor.py", "resume-audit.py",
    "completed-hour-audit.py",
    "ui-profile-audit.py", "ui-profile-comparison.py", "stage_ui_profile.py",
)
INITIAL_OBSERVER_BASE = ROOT / "benchmarks-local/isolation-2026-10-16"
INITIAL_OBSERVER_FILES = (
    "ui-profile-preflight.json", "ui-profile-initial-normal-ready.json",
    "ui-profile-initial-normal-ready.jsonl",
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_json(path):
    return json.loads(path.read_text())


def sha256_file(path):
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
            size += len(block)
    return digest.hexdigest(), size


def validate_json(path, compressed=False):
    suffix = Path(path.stem).suffix if compressed else path.suffix
    if suffix not in {".json", ".jsonl"}:
        return None
    opener = gzip.open if compressed else open
    with opener(path, "rt", encoding="utf-8") as stream:
        if suffix == ".json":
            json.load(stream)
            return {"format": "json", "valid": True}
        lines = 0
        for line in stream:
            require(bool(line.strip()), f"Blank JSONL line in {path}")
            json.loads(line)
            lines += 1
        return {"format": "jsonl", "valid": True, "records": lines}


def check_completed(label, renderer, seconds, diagnostic):
    run = BASE / label
    result = read_json(run / "result.json")
    launch = read_json(BASE / f"{label}-launch.json")
    admission = read_json(BASE / f"{label}-admission.json")
    ready = read_json(BASE / f"{label}-normal-ready.json")
    guard = read_json(BASE / f"{label}-guard.json")
    policy = read_json(BASE / f"{label}-policy.json")
    cleanup = read_json(BASE / f"{label}-cleanup.json")
    verification = read_json(BASE / f"{label}-verification.json")
    kernel = read_json(BASE / f"{label}-kernel.json")
    require(result["status"] == "ok", f"{label}: unsuccessful result")
    require(result["repository_head"] == EXPECTED_HEAD, f"{label}: unexpected source HEAD")
    require(result["load_mode"] == "both" and set(result["selected_loads"]) == {"ui", "audio"},
            f"{label}: incomplete selected load")
    require(launch["returncode"] == 0 and launch["seconds"] == seconds and
            launch["renderer"] == renderer and launch["diagnostic_ui_logging"] is diagnostic,
            f"{label}: unexpected or unfinished launch")
    require(admission["stage"] == "admitted" and ready["status"] == "matched",
            f"{label}: no successful readiness/admission")
    require(not launch["profile_mutation"] and not policy["profile_mutation"] and
            policy["runner_returncode"] == 0 and policy["benchmark_returncode"] == 0,
            f"{label}: policy lifecycle failed")
    require(guard["status"] == "ok" and guard["returncode"] == 0 and
            guard["idle_inhibit_released"] and guard["script_unloaded"] and
            not guard["errors"], f"{label}: owned guard not released")
    require(cleanup["worker_closed"] and cleanup["backend_pid"] is None and
            not cleanup["fixture_exists"] and not any(cleanup["PID_exists"].values()) and
            all(code == 0 for code in cleanup["probe_returncodes"].values()),
            f"{label}: worker/probe/fixture cleanup incomplete")
    require(verification["small_report_and_lifecycle_checks_passed"],
            f"{label}: final report verification missing")
    require(kernel["returncode"] == 0 and not kernel["parse_errors"],
            f"{label}: kernel query failed")
    for name, sha in FROZEN_PINS.items():
        require(result["pins"][name]["sha256"] == sha, f"{label}: frozen {name} changed")
    # A missed p99 target is a valid reportable measurement, not an archive error.
    require(verification["workload_seconds"] >= seconds,
            f"{label}: requested duration not completed")
    if not diagnostic:
        require(verification["controls"] == 11, f"{label}: incomplete hourly controls")
    return {"label": label, "renderer": renderer, "seconds": seconds,
            "diagnostic_logging": diagnostic, "result": result, "launch": launch}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hour-label", default="combined-vulkan-hour")
    parser.add_argument("--output", type=Path, default=BASE / "staged-archive")
    args = parser.parse_args()
    require(args.hour_label.replace("-", "").isalnum(), "Unsafe hour label")
    output = args.output.resolve()
    require(output.parent == BASE and not output.exists(),
            "Output must be a fresh immediate child of the ignored task base")
    runs = [check_completed("diagnostic-opengl", "opengl", 300, True),
            check_completed("diagnostic-vulkan", "vulkan", 300, True),
            check_completed(args.hour_label, "vulkan", 3600, False)]
    common_guard = read_json(BASE / "profile-series-guard.json")
    require(common_guard["status"] == "ok" and common_guard["returncode"] == 0 and
            common_guard["idle_inhibit_released"] and not common_guard["errors"],
            "Common series inhibitor has not been released")
    revision = read_json(BASE / "ui-profile-parser-revision.json")
    for name, pin in revision["files"].items():
        require(sha256_file(BASE / name) == (pin["sha256"], pin["bytes"]),
                f"Parser revision provenance changed: {name}")

    output.mkdir()
    files = {}
    omitted_optional = []
    sources = {}

    def add(source, target, expected_sha=None, compress=False, classification="saved evidence"):
        source = Path(source)
        require(source.is_file() and not source.is_symlink(), f"Missing plain file: {source}")
        require(source.suffix.lower() not in {".wav", ".f32", ".gguf", ".so", ".a"},
                f"Excluded media/model/binary: {source}")
        dest = output / target
        require(not dest.exists() and output in dest.parents, f"Duplicate or unsafe target: {target}")
        raw_sha, raw_size = sha256_file(source)
        if expected_sha:
            require(raw_sha == expected_sha, f"Pinned source changed: {source}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        if compress:
            with source.open("rb") as src, dest.open("wb") as dst:
                with gzip.GzipFile(filename="", fileobj=dst, mode="wb", mtime=0,
                                   compresslevel=6) as zipped:
                    shutil.copyfileobj(src, zipped, length=1024 * 1024)
        else:
            shutil.copyfile(source, dest)
        stored_sha, stored_size = sha256_file(dest)
        entry = {"source": str(source.relative_to(ROOT)), "classification": classification,
                 "sha256": stored_sha, "bytes": stored_size}
        if compress:
            digest = hashlib.sha256()
            roundtrip_size = 0
            with gzip.open(dest, "rb") as stream:
                while block := stream.read(1024 * 1024):
                    digest.update(block)
                    roundtrip_size += len(block)
            require((digest.hexdigest(), roundtrip_size) == (raw_sha, raw_size),
                    f"Gzip roundtrip mismatch: {target}")
            entry.update(decompressed_sha256=raw_sha, decompressed_bytes=raw_size,
                         gzip_mtime=0, gzip_filename="", exact_raw_verification=True)
        else:
            require((stored_sha, stored_size) == (raw_sha, raw_size), f"Copy mismatch: {target}")
        validation = validate_json(dest, compress)
        if validation:
            entry["validation"] = validation
        files[target] = entry

    def snapshot(source, sha, reason):
        source = Path(source).resolve()
        require(ROOT in source.parents, f"Source outside repository: {source}")
        relative = str(source.relative_to(ROOT))
        target = "source-snapshots/" + relative
        if target in sources:
            require(sources[target]["sha256"] == sha, f"Source pins differ across runs: {relative}")
            sources[target]["pin_origins"].append(reason)
            return
        add(source, target, expected_sha=sha, classification="pinned producer source")
        sources[target] = {"sha256": sha, "pin_origins": [reason]}

    for metadata in runs:
        label = metadata["label"]
        run = BASE / label
        for name in RUN_FILES:
            compress = name.endswith(".jsonl")
            add(run / name, f"{label}/{name}" + (".gz" if compress else ""), compress=compress)
        for target, template in EXTERNAL_FILES.items():
            compress = target.endswith(".jsonl")
            add(BASE / template.format(label=label), f"{label}/{target}" + (".gz" if compress else ""),
                compress=compress)
        if metadata["diagnostic_logging"]:
            for target, template in DIAGNOSTIC_FILES.items():
                compress = target.endswith(".jsonl")
                add(BASE / template.format(label=label), f"{label}/{target}" + (".gz" if compress else ""),
                    compress=compress, classification="diagnostic-only profiling evidence")
        for target, template in OPTIONAL_RUN_FILES.items():
            source = BASE / template.format(label=label)
            destination = f"{label}/{target}" + (".gz" if target.endswith(".jsonl") else "")
            if destination in files:
                continue
            if source.exists():
                add(source, destination, compress=target.endswith(".jsonl"), classification="saved derived audit or process sample")
            else:
                omitted_optional.append(str(source.relative_to(ROOT)))
        if not metadata["diagnostic_logging"]:
            # Do not silently archive an instrumented run as the untraced hour.
            for name in (f"{label}-qt-timing.jsonl", f"{label}-qt-timing-capture.json"):
                require(not (BASE / name).exists(), f"Unexpected timing instrumentation for hour: {name}")
        for name in ("soak_source", "shared_benchmark_source", "worker_source", "sampler_source"):
            pin = metadata["result"]["pins"][name]
            snapshot(pin["path"], pin["sha256"], f"{label}/result.json:pins.{name}")
        for name, sha in metadata["launch"]["wrapper_pins"].items():
            snapshot(BASE / name, sha, f"{label}/launch.json:wrapper_pins.{name}")
        before = read_json(BASE / f"before-{label}.json")
        for name, pin in before["pins"].items():
            source = Path(name)
            if source.suffix not in {".py", ".cpp", ".qml"}:
                continue
            if not source.is_absolute():
                source = ROOT / source
            snapshot(source, pin["sha256"], f"{label}/before.json:pins.{name}")
    snapshot(ROOT / "tools/benchmarks/window_guard.py", WINDOW_GUARD_SHA, "frozen window guard pin")

    for name in SERIES_FILES:
        add(BASE / name, "series/" + name,
            classification="series lifecycle or analysis provenance")
    for name in INITIAL_OBSERVER_FILES:
        add(INITIAL_OBSERVER_BASE / name,
            "series/initial-observer/" + name + (".gz" if name.endswith(".jsonl") else ""),
            compress=name.endswith(".jsonl"),
            classification="initial preflight/readiness observer provenance")
    for name in OPTIONAL_SERIES_FILES:
        if (BASE / name).exists():
            add(BASE / name, "series/" + name, classification="saved series derived evidence")
        else:
            omitted_optional.append(str((BASE / name).relative_to(ROOT)))
    for name in ANALYSIS_HELPERS:
        if (BASE / name).exists():
            target = "series/analysis-source/" + name
            add(BASE / name, target,
                classification="current-byte analysis source; execution identity only if separately pinned")
        else:
            omitted_optional.append(str((BASE / name).relative_to(ROOT)))

    provenance = {
        "schema": 1, "repository_head_from_all_results": EXPECTED_HEAD,
        "scope": "File-only packaging after all workloads and owned guards completed; no host commands, GPU, builds or benchmarks.",
        "runs": [{key: row[key] for key in ("label", "renderer", "seconds", "diagnostic_logging")}
                 for row in runs],
        "selected_factor": "Qt UI renderer changes from OpenGL to Vulkan; worker, native runtime, model, UI and audio binaries and worker sources retain identical pins.",
        "diagnostic_scope": "The two 300-second runs enable Qt per-frame timing and are diagnostic only. The 3600-second combined run disables per-frame timing; diagnostic results do not replace its target assessment.",
        "source_snapshots": sources,
        "analysis_source_scope": "Pinned producer snapshots match saved result, launch or before-snapshot SHA256. Other helper copies preserve current bytes for analysis provenance and do not independently prove historical execution identity.",
        "parser_revision": "Original v1 parser/output and the revision record are preserved; revision hashes must match saved files.",
        "initial_observer": "A redundant initial read-only readiness observer ran beside the first OpenGL diagnostic and was interrupted after that report. Its saved preflight and interrupted readiness records are retained as a first-diagnostic observer confound, not as workload admission evidence. The selected per-run admission gates remain separately archived.",
        "thermal_user_guidance": "User accepts brief ultrabook heat spikes if they do not persist. No numeric sustained-duration threshold was invented; sampled spans and recovery must be reported with gaps.",
        "audio_user_guidance": "User has no external audio interface and does not require ideal interface measurements. Available laptop callback and transport observations are assessed; silent-probe results do not measure stem playback or physical roundtrip latency.",
        "excluded": "No runtime/UI/audio binaries, model, WAV/F32 audio, full local journals, stop markers, bytecode or unrelated files.",
        "optional_files_absent": omitted_optional,
        "raw_and_derived_scope": "Manifest classifies saved raw reports/traces, diagnostic logs, derived audits, lifecycle records and exact source snapshots separately; all files beneath this archive except manifest.json itself are covered.",
    }
    target = output / "source-provenance.json"
    target.write_text(json.dumps(provenance, indent=2) + "\n")
    sha, size = sha256_file(target)
    files["source-provenance.json"] = {
        "source": "generated by stage_ui_profile.py from saved completed metadata",
        "classification": "generated provenance", "sha256": sha, "bytes": size,
        "validation": {"format": "json", "valid": True},
    }
    actual = {str(path.relative_to(output)) for path in output.rglob("*") if path.is_file()}
    require(actual == set(files), "Unexpected or unmanifested staged file")
    manifest = {
        "schema": 1, "created_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Every file in this fresh archive except manifest.json itself.",
        "archive_helper_sha256": sha256_file(Path(__file__))[0],
        "repository_head": EXPECTED_HEAD,
        "frozen_result_pins": FROZEN_PINS, "files": files,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for name, entry in files.items():
        require(sha256_file(output / name) == (entry["sha256"], entry["bytes"]),
                f"Final manifest mismatch: {name}")
    print(json.dumps({"path": str(output), "files": len(files),
                      "stored_bytes": sum(entry["bytes"] for entry in files.values()),
                      "manifest_sha256": sha256_file(output / "manifest.json")[0],
                      "hashes_json_and_gzip_roundtrips_verified": True,
                      "optional_files_absent": omitted_optional}, indent=2))


if __name__ == "__main__":
    main()

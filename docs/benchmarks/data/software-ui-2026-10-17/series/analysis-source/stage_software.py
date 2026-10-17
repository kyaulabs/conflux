#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Package completed software UI comparisons into a fresh ignored tree; no host queries.

Adapted from the preceding stage_ui_profile.py. Invoke only after all trials,
monitors and owned guards have finished. Packaging does not rerun workloads.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
EXPECTED_HEAD = "da9575c5c66085570653d88108e4cbdc81a92de4"
FROZEN_PINS = {
    "runtime": "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6",
    "model": "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6",
    "soak_source": "d5d0e5687f93e1597add7c8a28a96a6495dac04143fe578ea4f5a0a00b1d404f",
    "shared_benchmark_source": "87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7",
    "worker_source": "308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8",
    "sampler_source": "8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b",
    "ui": "a2c1430535a14fa92d70ed1dd94015319616f1d67080af5a6bebe0ec5220dc9f",
    "audio_probe": "b8cbfb0a255c94cf8430b9d7ef97823a9e2f3b11eedf32b3dec179ba1d197ea2",
}
RUN_FILES = ("result.json", "progress.json", "events.jsonl", "native-trace.jsonl",
             "ui.json", "ui-progress.json", "ui.stderr", "audio.json", "audio.stderr")
EXTERNAL_FILES = {
    "before.json": "before-{label}.json", "after.json": "after-{label}.json",
    "launcher.stdout": "{label}.stdout", "launcher.stderr": "{label}.stderr",
    "launch.json": "{label}-launch.json", "admission.json": "{label}-admission.json",
    "normal-ready.json": "{label}-normal-ready.json", "normal-ready.jsonl": "{label}-normal-ready.jsonl",
    "guard.json": "{label}-guard.json", "guard.js": "{label}-guard.js",
    "policy-lifecycle.json": "{label}-policy.json", "cleanup-host.json": "{label}-cleanup.json",
    "kernel-compact.json": "{label}-kernel.json", "thermal.json": "{label}-thermal.json",
    "verification.json": "{label}-verification.json", "AC-lid-monitor.jsonl": "{label}-AC-lid-monitor.jsonl",
    "cadence-audit.json": "{label}-cadence-audit.json",
    "independent-audit.json": "{label}-independent-audit.json",
    "thermal-snapshot-audit.json": "{label}-thermal-snapshot-audit.json",
    "analysis.json": "{label}-analysis.json",
    "native-tail-analysis.json": "{label}-native-tail-analysis.json",
    "cooldown.json": "{label}-cooldown.json", "cooldown.jsonl": "{label}-cooldown.jsonl",
}
REQUIRED_EXTERNAL = {"before.json", "after.json", "launcher.stdout", "launcher.stderr", "launch.json",
    "admission.json", "normal-ready.json", "normal-ready.jsonl", "guard.json", "guard.js",
    "policy-lifecycle.json", "cleanup-host.json", "kernel-compact.json", "thermal.json",
    "verification.json", "AC-lid-monitor.jsonl", "cadence-audit.json", "independent-audit.json",
    "thermal-snapshot-audit.json"}
FAILED_VERIFICATION_JSON = set()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def load(path):
    return json.loads(path.read_text())


def identity(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest(), path.stat().st_size


def label(value):
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", value), "Unsafe trial label")
    return value


def validate(path, compressed=False, strict=True):
    suffix = Path(path.stem).suffix if compressed else path.suffix
    if suffix not in (".json", ".jsonl"):
        return None
    try:
        with (gzip.open if compressed else open)(path, "rt", encoding="utf-8") as stream:
            if suffix == ".json":
                json.load(stream)
                return {"format": "json", "valid": True}
            count = 0
            for line in stream:
                require(bool(line.strip()), "Blank JSONL record")
                json.loads(line)
                count += 1
            return {"format": "jsonl", "valid": True, "records": count}
    except (ValueError, UnicodeError, RuntimeError) as error:
        if strict:
            raise
        return {"format": suffix[1:], "valid": False, "error": str(error),
                "qualification": "Exact failed-attempt bytes retained; not a successful report"}


def completed(name, renderer, seconds):
    result = load(BASE / name / "result.json")
    launch = load(BASE / (name + "-launch.json"))
    cleanup = load(BASE / (name + "-cleanup.json"))
    guard = load(BASE / (name + "-guard.json"))
    admission = load(BASE / (name + "-admission.json"))
    readiness = load(BASE / (name + "-normal-ready.json"))
    policy = load(BASE / (name + "-policy.json"))
    cadence = load(BASE / (name + "-cadence-audit.json"))
    with (BASE / (name + "-AC-lid-monitor.jsonl")).open("rb") as stream:
        stream.seek(max(0, stream.seek(0, 2) - 8192))
        monitor_tail = stream.read().splitlines()
    monitor_finished = json.loads(monitor_tail[-1]) if monitor_tail else {}
    require(monitor_finished.get("event") == "finished" and monitor_finished.get("returncode") == 0,
            name + ": AC/lid monitor is not complete")
    require(result["status"] == "ok" and not result["failures"] and result["repository_head"] == EXPECTED_HEAD,
            name + ": unsuccessful result or unexpected source HEAD")
    require(result["requested_seconds"] == seconds and result["workload_seconds"] >= seconds
            and result["load_mode"] == "both" and result["renderer"] == renderer,
            name + ": duration or selected load differs")
    require(launch.get("finished_at_utc") and launch["returncode"] == 0
            and launch["repaint_ms"] == 33 and launch["renderer"] == renderer and not launch["diagnostic_ui_logging"]
            and launch["host_conditions_before_spawn"]["ready"], name + ": incomplete launch")
    require(admission["stage"] == "admitted" and admission["host_conditions_before_admission"]["ready"]
            and readiness["status"] == "matched", name + ": admission not established")
    require(guard["status"] == "ok" and guard["idle_inhibit_released"] and guard["script_unloaded"]
            and guard["restored"] and not guard["errors"], name + ": owned guard not released")
    require(cleanup["worker_closed"] and cleanup["backend_pid"] is None and not cleanup["fixture_exists"]
            and not any(cleanup["PID_exists"].values()) and all(code == 0 for code in cleanup["probe_returncodes"].values()),
            name + ": owned workload cleanup incomplete")
    require(policy["mode"] == "observe" and not policy["profile_mutation"] and not policy["restored_by_runner"]
            and policy["runner_returncode"] == policy["benchmark_returncode"] == 0, name + ": policy observer failed")
    require(cadence["status"] == "valid" and not cadence["failed_checks"], name + ": cadence efficacy not valid")
    require(len(result["controls"]) == (11 if seconds == 3600 else 2),
            name + ": incomplete controls")
    for key, sha in FROZEN_PINS.items():
        require(result["pins"][key]["sha256"] == sha, name + ": frozen pin changed: " + key)
    require(result["ui_coverage"]["passed"] and result["audio_coverage"]["passed"], name + ": selected coverage failed")
    return {"label": name, "renderer": renderer, "repaint_ms": 33, "seconds": seconds,
            "result": result, "launch": launch}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-label", type=label, default="vulkan-33ms-15min")
    parser.add_argument("--candidate-label", type=label, default="software-33ms-15min")
    parser.add_argument("--hour-label", type=label, default="software-33ms-hour")
    parser.add_argument("--failed-label", type=label, action="append", default=[])
    parser.add_argument("--output", type=Path, default=BASE / "staged-archive")
    args = parser.parse_args()
    output = args.output.resolve()
    require(output.parent == BASE and not output.exists(), "Use a fresh immediate child of the ignored software UI base")
    sequence = load(BASE / "series-state.json")
    require(sequence.get("status") in ("completed", "stopped"), "Renderer sequence is still active")
    runs = [completed(args.control_label, "vulkan", 900), completed(args.candidate_label, "software", 900)]
    hour_completed = sequence.get("software_hour_completed", False)
    if hour_completed:
        runs.append(completed(args.hour_label, "software", 3600))
    if sequence["status"] == "completed":
        require(hour_completed == sequence["selection"]["run_software_hour"], "Conditional hour state is inconsistent")
    require(set(sequence["completed_runs"]) == {row["label"] for row in runs}, "Completed-run list differs from evidence")
    series_guard = load(BASE / "software-series-guard.json")
    require(series_guard["status"] == "ok" and series_guard["idle_inhibit_released"]
            and not series_guard["errors"] and series_guard.get("finished_at_utc"),
            "Common series guard has not been released")
    successful = {row["label"] for row in runs}
    discovered = {path.name[:-len("-admission.json")] for path in BASE.glob("*-admission.json")}
    discovered.update(path.name[:-len("-launch.json")] for path in BASE.glob("*-launch.json"))
    failed = sorted((discovered | set(args.failed_label)) - successful)
    for name in failed:
        label(name)
    output.mkdir()
    files, source_snapshots, omitted, consumed = {}, {}, [], set()

    def add(source, target, *, expected=None, classification="saved evidence", strict=True, replay_target=None):
        source = Path(source)
        require(source.is_file() and not source.is_symlink(), "Missing plain file: " + str(source))
        require(source.suffix.lower() not in (".wav", ".f32", ".gguf", ".so", ".a"), "Excluded PCM/model/binary")
        dest = output / target
        require(output in dest.parents, "Unsafe archive target: " + target)
        raw_sha, raw_bytes = identity(source)
        require(expected is None or expected == raw_sha, "Pinned source changed: " + str(source))
        compress = source.suffix == ".jsonl"
        if compress:
            dest = dest.with_name(dest.name + ".gz")
        require(not dest.exists(), "Duplicate archive target: " + target)
        dest.parent.mkdir(parents=True, exist_ok=True)
        if compress:
            with source.open("rb") as src, dest.open("wb") as dst:
                with gzip.GzipFile(filename="", fileobj=dst, mode="wb", mtime=0, compresslevel=6) as zipped:
                    shutil.copyfileobj(src, zipped, length=1024 * 1024)
            with gzip.open(dest, "rb") as stream:
                decompressed_sha = hashlib.file_digest(stream, "sha256").hexdigest()
            with gzip.open(dest, "rb") as stream:
                decompressed_bytes = sum(len(block) for block in iter(lambda: stream.read(1024 * 1024), b""))
            require((decompressed_sha, decompressed_bytes) == (raw_sha, raw_bytes), "Gzip roundtrip mismatch")
        else:
            shutil.copyfile(source, dest)
            require(identity(dest) == (raw_sha, raw_bytes), "Copy mismatch")
        stored_sha, stored_bytes = identity(dest)
        entry = {"source": str(source.relative_to(ROOT)), "classification": classification,
                 "sha256": stored_sha, "bytes": stored_bytes, "raw_sha256": raw_sha, "raw_bytes": raw_bytes,
                 "compression": "gzip" if compress else None}
        if compress:
            entry.update(gzip_mtime=0, gzip_filename="", exact_raw_verification=True)
        checked = validate(dest, compressed=compress, strict=strict)
        if checked is not None:
            entry["validation"] = checked
        if replay_target is None:
            replay_target = str(source.relative_to(ROOT))
        entry["replay_target"] = replay_target
        files[str(dest.relative_to(output))] = entry
        consumed.add(source.resolve())

    def snapshot(source, sha, origin):
        source = Path(source).resolve()
        require(ROOT in source.parents, "Pinned source outside repository")
        target = "source-snapshots/" + str(source.relative_to(ROOT))
        if target in source_snapshots:
            require(source_snapshots[target]["sha256"] == sha, "Conflicting executed source hashes")
            source_snapshots[target]["pin_origins"].append(origin)
            return
        add(source, target, expected=sha, classification="pinned producer source")
        source_snapshots[target] = {"sha256": sha, "pin_origins": [origin]}

    def archive_run(name, completed_run):
        run = BASE / name
        namespace = name if completed_run else "failed-attempts/" + name
        for filename in RUN_FILES:
            source = run / filename
            if source.exists():
                add(source, namespace + "/" + filename, strict=completed_run,
                    classification="completed workload evidence" if completed_run else "failed-attempt evidence")
            elif completed_run:
                raise FileNotFoundError(source)
        for target, template in EXTERNAL_FILES.items():
            source = BASE / template.format(label=name)
            if source.exists():
                add(source, namespace + "/" + target, strict=completed_run,
                    classification="completed lifecycle or audit" if completed_run else "failed-attempt lifecycle")
            elif completed_run and target in REQUIRED_EXTERNAL:
                raise FileNotFoundError(source)
            else:
                omitted.append(str(source.relative_to(ROOT)))
        # Preserve additional owned JSON failure reports without copying generated PCM.
        if run.exists():
            for source in sorted(run.iterdir()):
                if source.is_file() and source.resolve() not in consumed and source.suffix == ".json":
                    add(source, namespace + "/additional/" + source.name, strict=completed_run,
                        classification="additional owned report")

    for row in runs:
        name = row["label"]
        archive_run(name, True)
        for key in ("soak_source", "shared_benchmark_source", "worker_source", "sampler_source"):
            pin = row["result"]["pins"][key]
            snapshot(pin["path"], pin["sha256"], name + "/result:pins." + key)
        for filename, sha in row["launch"]["wrapper_pins"].items():
            snapshot(BASE / filename, sha, name + "/launch:wrapper_pins." + filename)
        before = load(BASE / ("before-" + name + ".json"))
        for filename, pin in before["pins"].items():
            source = Path(filename)
            if source.suffix in (".py", ".cpp", ".qml"):
                snapshot(source if source.is_absolute() else ROOT / source, pin["sha256"], name + "/before:pins." + filename)
    for name in failed:
        archive_run(name, False)
    baseline = BASE.parent / "cadence-2026-10-17/33ms-both-hour/result.json"
    baseline_sha = identity(baseline)[0]
    for row in runs:
        audit = load(BASE / (row["label"] + "-cadence-audit.json"))
        require(audit["inputs"][str(baseline)]["sha256"] == baseline_sha, "Baseline differs from executed cadence audit")
    add(baseline, "dependencies/prior-cadence-vulkan-hour/result.json", expected=baseline_sha,
        classification="explicit cadence/pin-audit baseline dependency")
    source_verification = load(BASE / "source-verification.json")
    for filename, sha in source_verification["source_pins"].items():
        snapshot(ROOT / filename, sha, "source-verification.json:source_pins." + filename)
    require(identity(BASE / "source-CTest.log")[0] == source_verification["CTest"]["log_sha256"], "CTest log identity differs")
    visual = source_verification["screenshot_visual_review"]
    require(identity(BASE / visual["path"])[0] == visual["sha256"], "Functional screenshot identity differs")
    for source in sorted(BASE.iterdir()):
        if not source.is_file() or source.resolve() in consumed:
            continue
        if source.suffix in (".py", ".cpp", ".json", ".jsonl", ".js", ".stdout", ".stderr", ".png", ".log"):
            target = "series/analysis-source/" + source.name if source.suffix in (".py", ".cpp") else "series/" + source.name
            failed_verification = source.name in FAILED_VERIFICATION_JSON
            add(source, target, strict=not failed_verification,
                classification="failed verification attempt; never successful validation" if failed_verification
                else "series source, lifecycle, saved audit or visual evidence")
    provenance = {
        "schema": 1, "repository_head_from_completed_results": EXPECTED_HEAD,
        "completed_runs": [{key: row[key] for key in ("label", "renderer", "repaint_ms", "seconds")} for row in runs],
        "series_status": sequence["status"], "conditional_selection": sequence.get("selection"),
        "software_hour_completed": hour_completed,
        "failed_attempt_labels": failed, "source_snapshots": source_snapshots,
        "intended_documentation_destination": "docs/benchmarks/data/software-ui-2026-10-17",
        "selected_factor": "Vulkan versus software UI rendering at the same33ms QTimeLine update interval and unchanged UI/audio/native/model/worker identities. Both required comparisons use900seconds/control300. An optional software3600second hour is archived only when series-state records it completed; absence remains explicit.",
        "selection_scope": "Software900second continuous p99<=4095ms and valid existing checks select the optional hour; this does not qualify M0 or create a new acceptance gate.",
        "cadence_audit_dependency": {"stored_path": "dependencies/prior-cadence-vulkan-hour/result.json",
                                     "sha256": baseline_sha,
                                     "replay_path": "benchmarks-local/cadence-2026-10-17/33ms-both-hour/result.json"},
        "replay": "replay_software.py restores exact packaged bytes in an isolated temporary repository layout, then runs only the packaged cadence/completed-run/thermal saved-evidence audits, plus native descriptive replay when present. It does not call producer, GPU, audio, policy or host tools.",
        "scope": "File-only packaging after all workloads/guards complete; missed latency targets are preserved as measurements. Fixed order, thermal/policy/cache history and background load limit causal conclusions.",
        "source_identity_scope": "Pinned snapshots match saved completed-run SHA256 metadata. Other helper copies preserve current bytes; they do not independently prove historical execution. Failed attempts remain separate and do not qualify as completed runs.",
        "thermal_user_guidance": "Brief ultrabook heat spikes are acceptable; no sustained threshold is invented. Report sampled spans and gaps.",
        "audio_user_guidance": "Assess available laptop callback/transport observations; no external interface or perfect timing is required. Silence does not measure stem playback or physical latency.",
        "excluded": "Runtime/UI/audio binaries, models, generated WAV/F32 PCM, full journals, stop markers, bytecode and unrelated files.",
        "optional_files_absent": omitted,
    }
    provenance_path = output / "source-provenance.json"
    provenance_path.write_text(json.dumps(provenance, indent=2) + "\n")
    sha, size = identity(provenance_path)
    files["source-provenance.json"] = {"classification": "generated provenance", "sha256": sha, "bytes": size,
                                      "validation": {"format": "json", "valid": True}}
    actual = {str(path.relative_to(output)) for path in output.rglob("*") if path.is_file()}
    require(actual == set(files), "Unexpected unmanifested archive file")
    manifest = {"schema": 1, "scope": "Every stored file except this manifest; gzip has zero mtime and no filename",
                "repository_head": EXPECTED_HEAD, "frozen_result_pins": FROZEN_PINS,
                "archive_helper_sha256": identity(Path(__file__))[0], "files": files}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for filename, pin in files.items():
        require(identity(output / filename) == (pin["sha256"], pin["bytes"]), "Final manifest mismatch")
    print(json.dumps({"output": str(output), "files": len(files), "failed_attempts": failed,
                      "manifest_sha256": identity(output / "manifest.json")[0],
                      "deterministic_gzip_roundtrips_and_manifest_verified": True}, indent=2))


if __name__ == "__main__":
    main()

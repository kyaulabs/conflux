#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Package completed cadence evidence into a fresh ignored tree; no host queries.

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
EXPECTED_HEAD = "d00692ffe1b7654a9946b5e2ed84f8c044586fc2"
BASELINE_RESULT_SHA = "6048032e360724b7382a392677bb3174d102000099050f12d1b6029924de2df0"
FROZEN_PINS = {
    "runtime": "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6",
    "model": "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6",
    "soak_source": "52605983fdd2416caf63477bc9314096a0daa4f251e798c92928be19e5fea8b8",
    "shared_benchmark_source": "a9b7cc5904883460baf7ef7e19b7a5f227f676945283f33564e2ade6d906e256",
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
    "cooldown.json": "{label}-cooldown.json", "cooldown.jsonl": "{label}-cooldown.jsonl",
}
REQUIRED_EXTERNAL = {"before.json", "after.json", "launcher.stdout", "launcher.stderr", "launch.json",
    "admission.json", "normal-ready.json", "normal-ready.jsonl", "guard.json", "guard.js",
    "policy-lifecycle.json", "cleanup-host.json", "kernel-compact.json", "thermal.json",
    "verification.json", "AC-lid-monitor.jsonl", "cadence-audit.json"}
FAILED_VERIFICATION_JSON = {"repaint-behavior-not-run-attempt1.json", "behavior-build-attempt1.json"}


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


def completed(name, repaint_ms, seconds):
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
            and result["load_mode"] == "both" and result["renderer"] == "vulkan",
            name + ": duration or selected load differs")
    require(launch.get("finished_at_utc") and launch["returncode"] == 0
            and launch["repaint_ms"] == repaint_ms and not launch["diagnostic_ui_logging"]
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
    require(len(result["controls"]) == 11 if seconds == 3600 else len(result["controls"]) >= 1,
            name + ": incomplete controls")
    for key, sha in FROZEN_PINS.items():
        require(result["pins"][key]["sha256"] == sha, name + ": frozen pin changed: " + key)
    require(result["ui_coverage"]["passed"] and result["audio_coverage"]["passed"], name + ": selected coverage failed")
    return {"label": name, "repaint_ms": repaint_ms, "seconds": seconds, "result": result, "launch": launch}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-label", type=label, default="timeline-16ms-smoke")
    parser.add_argument("--candidate-label", type=label, default="timeline-33ms-smoke")
    parser.add_argument("--hour-label", type=label, default="33ms-both-hour")
    parser.add_argument("--failed-label", type=label, action="append", default=[])
    parser.add_argument("--output", type=Path, default=BASE / "staged-archive")
    args = parser.parse_args()
    output = args.output.resolve()
    require(output.parent == BASE and not output.exists(), "Use a fresh immediate child of the ignored cadence base")
    runs = [completed(args.control_label, 16, 180), completed(args.candidate_label, 33, 180),
            completed(args.hour_label, 33, 3600)]
    series_guard = load(BASE / "cadence-series-guard.json")
    sequence = load(BASE / "hour-sequence.json")
    require(series_guard["status"] == "ok" and series_guard["idle_inhibit_released"]
            and not series_guard["errors"] and series_guard.get("finished_at_utc"),
            "Common series guard has not been released")
    require(sequence.get("status") == "completed", "Hour sequence is unfinished")
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
    baseline = BASE.parent / "ui-profile-2026-10-17/combined-vulkan-hour/result.json"
    add(baseline, "dependencies/prior-vulkan-hour/result.json", expected=BASELINE_RESULT_SHA,
        classification="explicit cadence-audit baseline dependency")
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
        "completed_runs": [{key: row[key] for key in ("label", "repaint_ms", "seconds")} for row in runs],
        "failed_attempt_labels": failed, "source_snapshots": source_snapshots,
        "intended_documentation_destination": "docs/benchmarks/data/cadence-2026-10-17",
        "selected_factor": "QTimeLine update interval 16 versus 33 milliseconds; not a global frame-rate limit. Both short trials use the same new UI binary, Vulkan renderer, worker/audio/runtime/model and 180-second protocol. The 33 ms acceptance run lasts 3600 seconds.",
        "cadence_audit_dependency": {"stored_path": "dependencies/prior-vulkan-hour/result.json",
                                     "sha256": BASELINE_RESULT_SHA,
                                     "replay_path": "benchmarks-local/ui-profile-2026-10-17/combined-vulkan-hour/result.json"},
        "replay": "replay_cadence.py restores exact packaged bytes in an isolated temporary repository layout, then runs only the packaged cadence/completed-hour/thermal saved-evidence audits. It does not call producer, GPU, audio, policy or host tools.",
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

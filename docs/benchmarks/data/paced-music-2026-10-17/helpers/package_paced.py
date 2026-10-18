#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stage explicit media-free playback-paced music-load evidence and replay saved-file audits."""
import argparse
import contextlib
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
VALIDATION = "paced-both-10min"
HOUR = "paced-both-hour"
RUNS = (VALIDATION, HOUR)
SERIES = ("initial-host.json", "expected-pins.json", "corpus-pins.json",
          "source-verification.json", "launcher-unarmed-check.json",
          "observed-launch-review.json", "independent-validation-check.json",
          "observed-launch-review-attempt1.json",
          "audit_paced-attempt1.py", "paced-both-10min-audit-attempt1.json",
          "source-CI-pr.log", "source-CI-push.log",
          "guard-journal.jsonl", "guard-journal-audit.json")
HELPERS = ("observed_launch.py", "audit_paced.py", "thermal-snapshot-audit.py", "analyze_music.py", "package_paced.py")
USER = "docs/benchmarks/data/user-tracks-2026-10-17"
STEM = "docs/benchmarks/data/stem-quality-2026-10-17"
SOFTWARE = "docs/benchmarks/data/software-ui-2026-10-17"
PROFILE = "docs/benchmarks/data/music-profile-2026-10-17"
MUSIC_LOAD = "docs/benchmarks/data/music-load-2026-10-17"
REFERENCES = {
    "user_archive": USER + "/archive-manifest.json",
    "music_load_archive": MUSIC_LOAD + "/archive-manifest.json",
    "profile_archive": PROFILE + "/archive-manifest.json",
    "software_archive": SOFTWARE + "/manifest.json",
    "stem_archive": STEM + "/archive-manifest.json",
    "manifest": USER + "/series/manifest.json",
    "original_Vulkan_events": USER + "/vulkan/events.jsonl.gz",
    "prior_worker_source": STEM + "/source-snapshots/tools/stems/worker.py",
    "sampler_source": STEM + "/source-snapshots/tools/benchmarks/worker_support.py",
    "shared_benchmark_source": STEM + "/source-snapshots/tools/benchmarks/stem_worker.py",
    "runtime_build": STEM + "/runtime/historical-build.json",
    "prior_UI_audio_pins": SOFTWARE + "/software-33ms-hour/result.json",
    "admission_source": PROFILE + "/helpers/interleaved_replay.py",
    "native_replay_source": PROFILE + "/helpers/native_replay.py",
    "window_guard_source": PROFILE + "/helpers/window_guard.py",
    "readiness_source": USER + "/helpers/normal_ready.py",
    "observer_source": SOFTWARE + "/source-snapshots/benchmarks-local/software-ui-2026-10-17/observed-run.py",
    "saved_audit_source": SOFTWARE + "/series/analysis-source/completed-hour-audit.py",
    "thermal_source": SOFTWARE + "/series/analysis-source/thermal-snapshot-audit.py",
    "ui_cpp_source": SOFTWARE + "/source-snapshots/src/main.cpp",
    "ui_qml_source": SOFTWARE + "/source-snapshots/qml/Main.qml",
    "audio_cpp_source": SOFTWARE + "/source-snapshots/src/probe_main.cpp",
    "profile_reducer": PROFILE + "/helpers/profile_music.py",
    "span_reducer": USER + "/helpers/summarize_runs.py",
}
FIXED_MANIFESTS = {
    "user_archive": "f4e40e1eb626b4b5929c8cddcfc13f6b221ef681e236f07f9fda4597446615d7",
    "music_load_archive": "51cd7d50dea1605f170ce9dd1bf3b69044700ed0f605d81840ed142d93330bac",
    "profile_archive": "09b1f516438bd92863af47f57d457369bf9c7c4b5d9ac312becaac00531f87c0",
    "software_archive": "445612ea59c1e8dfb377a9b738215cddd9785e39938ba465d97505db4765d31e",
}
FIXED_REDUCERS = {"profile_reducer": "9e2d3c5eedd6c7f00714619d45de51aee69713a4a4971d8779452979af74b6b6",
                  "span_reducer": "f704971b6a70862126de8f1d91498630dd7592d9d4ba63139ad8c49ef0e18fe5"}
SCOPE = ("Saved closed 600-second validation and optional conditional-hour files, exact source/helper references, JSONL decoding and packaged event/control/"
         "coverage/timing/cadence/thermal audit and descriptive analysis replay. No corpus WAV, original music, PCM, NumPy arrays, model, "
         "runtime/UI/audio binary, HTML or artwork is included or recomputed. Prior canonical archives remain "
         "required dependencies. Archive/replay integrity success does not imply latency or deadline qualification. "
         "Audio is an independent silent probe; no musical, physical-audio or M1 acceptance claim.")


def require(value, message):
    if not value:
        raise ValueError(message)


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"bytes": path.stat().st_size, "sha256": digest}


def reject_constant(value):
    raise ValueError("Nonfinite JSON constant: " + value)


def load(path):
    data = path.read_bytes()
    return json.loads(gzip.decompress(data) if path.suffix == ".gz" else data, parse_constant=reject_constant)


def write(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def members(runs):
    files = {name: BASE / name for name in SERIES}
    if HOUR in runs:
        files["independent-hour-check.json"] = BASE / "independent-hour-check.json"
    files.update({"helpers/" + name: BASE / name for name in HELPERS})
    for name in ("CMakeLists.txt", "tools/benchmarks/stem_soak.py", "tools/stems/worker.py",
                 "tests/stem_soak_test.py", "tests/stem_worker_test.py"):
        files["source-snapshots/" + name] = ROOT / name
    for label in runs:
        for suffix in ("launch.json", "guard.json", "guard.js", "policy.json", "AC-lid-monitor.jsonl",
                       "audit.json", "thermal-audit.json", "analysis.json"):
            files[label + "-" + suffix] = BASE / (label + "-" + suffix)
        for suffix in ("stdout", "stderr"):
            files[label + "." + suffix] = BASE / (label + "." + suffix)
        for name in ("admission-normal-ready.json", "admission-normal-ready.jsonl", "fresh-admission.json", "stdout", "stderr"):
            files[label + "-admission/" + name] = BASE / (label + "-admission") / name
        for name in ("result.json", "events.jsonl", "native-trace.jsonl", "audio.json", "audio.stderr",
                     "ui.json", "ui.stderr", "ui-progress.json", "progress.json"):
            files[label + "/" + name] = BASE / label / name
    return {(name + ".gz" if compressed(source) else name): source for name, source in files.items()}


def compressed(path):
    return path.suffix == ".jsonl" or path.name.endswith("-analysis.json")


def import_helper(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def replay(archive, repository, label, thermal=False):
    name = "thermal-snapshot-audit.py" if thermal else "audit_paced.py"
    helper = import_helper(archive / "helpers" / name, "packaged_music_" + ("thermal" if thermal else "events"))
    helper.BASE, helper.ROOT = archive, repository
    helper.OLD = repository / REFERENCES["saved_audit_source"]
    with tempfile.TemporaryDirectory(prefix="conflux-music-audit-") as temporary:
        output = Path(temporary) / "result.json"
        arguments = (["--run-directory", str(archive / label)] if thermal else
                     [label, "--source-snapshot", str(archive / "source-snapshots/tools/benchmarks/stem_soak.py"),
                      "--baseline-events", str(repository / REFERENCES["original_Vulkan_events"])])
        previous = sys.argv
        try:
            sys.argv = [str(archive / "helpers" / name), *arguments, "--output", str(output)]
            with contextlib.redirect_stdout(io.StringIO()):
                code = helper.main()
            require(code in (None, 0), "Packaged saved-event audit failed: " + label)
            derived = load(output)
        finally:
            sys.argv = previous
    saved = load(archive / (label + ("-thermal-audit.json" if thermal else "-audit.json")))
    if thermal:
        require({key: value for key, value in saved.items() if key != "source_evidence"}
                == {key: value for key, value in derived.items() if key != "source_evidence"}, "Thermal facts differ: " + label)
        require(saved["source_evidence"]["audit_source_sha256"] == pin(archive / "helpers" / name)["sha256"], "Thermal helper binding differs")
        return {"samples": derived["trace_samples_in_workload"], "package_peak_C": derived["package_sensor"]["peak_C"]}
    fields = ("status", "checks", "failed_checks", "timing", "latency_qualification", "playback_clock",
              "pacing_qualification", "controls", "sources",
              "source_admission_first_output", "admission_timing_scope", "event_counts", "worker", "audio", "UI", "AC_lid", "scope")
    require(all(saved[key] == derived[key] for key in fields), "Saved-event/cadence facts differ: " + label)
    require(saved["audit_source_sha256"] == pin(archive / "helpers" / name)["sha256"], "Event audit helper binding differs")
    return {"status": derived["status"], "continuous": derived["timing"]["continuous"],
            "controls": derived["controls"]["count"], "latency_qualification": derived["latency_qualification"],
            "pacing_qualification": derived["pacing_qualification"], "playback_clock": derived["playback_clock"]}


def replay_analysis(archive, repository, label):
    source = archive / "helpers/analyze_music.py"
    helper = import_helper(source, "packaged_music_description")
    require(pin(source)["sha256"] == "95608b848335620b0b39b614d11f168f547fceb8b7d1aca9b30d621d90c80aaa", "Reviewed descriptive helper changed")
    with tempfile.TemporaryDirectory(prefix="conflux-music-analysis-") as temporary:
        output = Path(temporary) / "result.json"
        previous = sys.argv
        try:
            sys.argv = [str(source), label, "--base", str(archive), "--output", str(output),
                        "--profile-helper", str(repository / REFERENCES["profile_reducer"]),
                        "--span-helper", str(repository / REFERENCES["span_reducer"])]
            with contextlib.redirect_stdout(io.StringIO()):
                helper.main()
            derived = load(output)
        finally:
            sys.argv = previous
    saved = load(archive / (label + "-analysis.json.gz"))
    omitted = {"created_at_utc", "inputs"}
    require({key: value for key, value in saved.items() if key not in omitted}
            == {key: value for key, value in derived.items() if key not in omitted}, "Descriptive analysis differs: " + label)
    require(saved["analysis_source_sha256"] == pin(source)["sha256"], "Descriptive analysis source binding differs")
    return {"status": derived["status"], "continuous_hops": derived["populations"]["continuous"]["hops"],
            "continuous_misses": derived["populations"]["continuous"]["misses_gt_4095ms"],
            "scope": "Descriptive saved-trace associations only; no additional latency or thermal gate."}


def audit(archive, repository):
    archive, repository = archive.resolve(), repository.resolve()
    manifest = load(archive / "archive-manifest.json")
    runs = tuple(manifest["runs"])
    require(runs in ((VALIDATION,), RUNS), "Only validation or validation plus conditional hour may be archived")
    require(set(manifest["files"]) == set(members(runs)), "Explicit allowlist differs")
    require({str(path.relative_to(archive)) for path in archive.rglob("*") if path.is_file()}
            == set(manifest["files"]) | {"archive-manifest.json"}, "Unmanifested/missing file")
    streams = {}
    for relative, identity in manifest["files"].items():
        path = archive / relative
        require(not path.is_symlink() and archive in path.resolve().parents
                and pin(path) == {key: identity[key] for key in ("bytes", "sha256")}, "Stored member differs: " + relative)
        if relative.endswith(".gz"):
            header = path.read_bytes()[:10]
            require(header[:2] == b"\x1f\x8b" and not header[3] & 8 and header[4:8] == b"\0" * 4, "Nondeterministic gzip header")
            raw = gzip.decompress(path.read_bytes())
            require(len(raw) == identity["raw_bytes"] and hashlib.sha256(raw).hexdigest() == identity["raw_sha256"], "Decoded identity differs")
            if "jsonl_records" in identity:
                rows = [json.loads(line, parse_constant=reject_constant) for line in raw.splitlines() if line.strip()]
                require(len(rows) == identity["jsonl_records"], "JSONL record count differs")
                streams[relative] = len(rows)
            else:
                load(path)
        elif path.suffix == ".json":
            load(path)
    require(set(manifest["external_references"]) == set(REFERENCES), "Prior reference inventory differs")
    for key, relative in REFERENCES.items():
        reference = manifest["external_references"][key]
        require(reference["repository_path"] == relative and pin(repository / relative)
                == {field: reference[field] for field in ("bytes", "sha256")}, "Prior reference identity differs: " + key)
    require(all(manifest["external_references"][key]["sha256"] == value for key, value in FIXED_MANIFESTS.items()), "Prior canonical manifest changed")
    require(all(manifest["external_references"][key]["sha256"] == value for key, value in FIXED_REDUCERS.items()), "Prior descriptive reducer changed")
    expected = load(archive / "expected-pins.json")
    verification = load(archive / "source-verification.json")
    corpus = load(archive / "corpus-pins.json")
    require(verification["source_head"] == expected["repository_head"] and verification["signature_status"] == "G"
            and verification["source_pins"] == expected["pins"], "Signed verification/source pin binding differs")
    require(pin(archive / "source-snapshots/tools/benchmarks/stem_soak.py")["sha256"] == expected["pins"]["soak_source"], "New soak snapshot differs")
    require(manifest["external_references"]["manifest"]["sha256"] == corpus["manifest"]["sha256"], "Prior corpus manifest differs")
    require(pin(archive / "source-snapshots/tools/stems/worker.py")["sha256"] == expected["pins"]["worker_source"], "New worker snapshot differs")
    for key in ("sampler_source", "shared_benchmark_source"):
        require(manifest["external_references"][key]["sha256"] == expected["pins"][key], "Frozen producer differs: " + key)
    build = load(repository / REFERENCES["runtime_build"])
    prior = load(repository / REFERENCES["prior_UI_audio_pins"])
    require(build["files"]["blocking_binary"]["sha256"] == expected["pins"]["runtime"]
            and build["files"]["model"]["sha256"] == expected["pins"]["model"]
            and all(prior["pins"][key]["sha256"] == expected["pins"][key] for key in ("ui", "audio_probe")), "Runtime/model/UI/audio baseline binding differs")
    require(pin(archive / "helpers/thermal-snapshot-audit.py") == pin(repository / REFERENCES["thermal_source"]), "Thermal helper was changed")
    for label in runs:
        launch = load(archive / (label + "-launch.json"))
        require(launch["pins"]["launcher"]["sha256"] == pin(archive / "helpers/observed_launch.py")["sha256"], "Executed launcher source differs")
        require(all(launch["pins"][key]["sha256"] == manifest["external_references"][key]["sha256"] for key in
                    ("admission_source", "native_replay_source", "window_guard_source", "readiness_source", "observer_source", "ui_cpp_source", "ui_qml_source", "audio_cpp_source")), "Executed prior helper/source differs")
    results = {label: {"events": replay(archive, repository, label), "thermal": replay(archive, repository, label, thermal=True),
                       "analysis": replay_analysis(archive, repository, label)} for label in runs}
    if HOUR in runs:
        require(results[VALIDATION]["events"]["pacing_qualification"]["target_and_deadlines_met"],
                "The conditional hour requires a qualifying validation")
    return {"status": "passed", "manifest_sha256": pin(archive / "archive-manifest.json")["sha256"],
            "files": len(manifest["files"]), "stored_bytes": sum(item["bytes"] for item in manifest["files"].values()),
            "jsonl_records": streams, "external_references": len(REFERENCES), "packaged_replays": 3 * len(runs),
            "runs": results, "scope": SCOPE, "PCM_or_listening_replayed": False}


def stage(output, approved, include_hour=False):
    runs = RUNS if include_hour else (VALIDATION,)
    output = output.resolve()
    require(approved and output == BASE / "staged-archive" and not output.exists(), "Root approval and fresh ignored staged-archive required")
    for label in runs:
        launch = load(BASE / (label + "-launch.json"))
        require(launch.get("finished_at_utc") and launch["status"] == "completed" and launch["returncode"] == 0
                and not launch["cleanup"]["remaining_owned_processes"], "Wait for closed trial and released guards: " + label)
        require(load(BASE / (label + "-audit.json"))["status"] == "valid", "Final completed-run audit missing/invalid")
    if include_hour:
        require(load(BASE / (VALIDATION + "-audit.json"))["pacing_qualification"]["target_and_deadlines_met"],
                "The conditional hour requires a qualifying validation")
    sources = members(runs)
    require(all(path.is_file() and not path.is_symlink() for path in sources.values()), "Missing/nonplain allowlisted final evidence")
    output.mkdir()
    files = {}
    for relative, source in sources.items():
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        digest, raw_bytes, records = hashlib.sha256(), 0, 0
        before = source.stat()
        with contextlib.ExitStack() as stack:
            inp = stack.enter_context(source.open("rb"))
            out = stack.enter_context(target.open("xb"))
            dest = stack.enter_context(gzip.GzipFile(filename="", fileobj=out, mode="wb", mtime=0, compresslevel=6)) if compressed(source) else out
            for data in inp:
                digest.update(data)
                raw_bytes += len(data)
                records += bool(data.strip())
                dest.write(data)
        after = source.stat()
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns), "Source changed while copying: " + str(source))
        files[relative] = {**pin(target), "source": str(source), "raw_bytes": raw_bytes, "raw_sha256": digest.hexdigest(),
                           "compression": "gzip" if compressed(source) else None}
        if source.suffix == ".jsonl":
            files[relative]["jsonl_records"] = records
    references = {key: {"repository_path": relative, **pin(ROOT / relative)} for key, relative in REFERENCES.items()}
    write(output / "archive-manifest.json", {"schema": 1, "scope": SCOPE, "root_approved_final_allowlist": True,
          "runs": list(runs), "external_references": references, "files": dict(sorted(files.items()))})
    return audit(output, ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    packing = commands.add_parser("stage")
    packing.add_argument("--approved", action="store_true")
    packing.add_argument("--include-hour", action="store_true", help="Include the closed conditional hour after qualifying validation")
    packing.add_argument("--output", type=Path, default=BASE / "staged-archive")
    checking = commands.add_parser("audit")
    checking.add_argument("archive", type=Path)
    checking.add_argument("--repository-root", type=Path, default=Path.cwd())
    checking.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = stage(args.output, args.approved, args.include_hour) if args.mode == "stage" else audit(args.archive, args.repository_root)
    if args.mode == "audit" and args.output:
        require(args.archive.resolve() not in args.output.resolve().parents, "Audit output must remain outside archive")
        write(args.output, result)
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()

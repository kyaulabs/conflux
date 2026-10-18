#!/usr/bin/env python3
"""Explicit-allowlist media-free M0 archive and saved-evidence audit.

Stage only after root confirms all four plots and both runs/guard/facts finish.
No inference, playback, downloads, host commands, or original-file writes.
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import shutil
import struct

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
BASELINE = "docs/benchmarks/data/stem-quality-2026-10-17"
IDS = ["club_xl", "when_the_light_breaks", "under_my_skin", "stfu"]
RATE, WINDOW, HOP, OVERLAP, N = 44100, 343980, 257985, 85995, 1031940
STEMS = ["drums", "bass", "other", "vocals"]
SIGNALS = ["source", *STEMS, "recombined"]
JSON_FILES = ("manifest.json", "prelaunch-review.json", "initial-host.json", "sequence.json", "series-guard.json",
              "id-revision.json", "cpu-normal-ready.json", "vulkan-normal-ready.json", "run-facts.json", "corpus-facts.json")
PY_FILES = ("package_user_tracks.py", "quality_run.py", "quality_sequence.py", "normal_ready.py", "analyze_user_tracks.py",
            "prepare_user_tracks.py", "prepare_user_tracks-attempt1.py", "revise_track_ids.py", "summarize_runs.py")
LOCAL_RECORDS = ("excerpt-plan.json", "excerpt-plan-attempt1.json", "corpus-provenance.json",
                 "corpus-provenance-attempt1.json", "manifest-attempt1.json")
DEPENDENCIES = {"metrics": ("quality_analyze.py", "4131400abe7ed071d31f6da6263815e4eb14eb02b32f5e7c979cc41e86b83bac"),
                "quiet": ("waveform_numerics.py", "66f9215974fe110455d1f0e21d6de529c18a0c55084b3116338c48c479e53325"),
                "waveform": ("waveform_review.py", "f1d84b8cc467bc6195d3450e4c6f63415b98a669b89d35eb9ab0647098f5037f")}
PRODUCTION = {"worker_source": "tools/stems/worker.py", "shared_benchmark_source": "tools/benchmarks/stem_worker.py",
              "sampler_source": "tools/benchmarks/worker_support.py", "native_adapter": "tools/runtime/htdemucs_segment.cpp",
              "native_cmake": "tools/runtime/CMakeLists.txt", "blocking_wait": "tools/runtime/BlockingVulkanWait.cmake"}
FROZEN = {"runtime": "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6",
          "model": "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6",
          "worker_source": "308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8",
          "shared_benchmark_source": "87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7",
          "sampler_source": "8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b"}
PLOTS = {"analysis/" + identifier + "-continuous-waveforms.png" for identifier in IDS}
SCOPE = ("Saved-file identities, raw controls/currentness/coverage and JSON timing/thermal reductions only. "
         "The canonical stem-quality baseline is an explicit external dependency. Original FLAC/WAV/NPY, "
         "models/binaries, full ffprobe/tag/plan/provenance dumps, raw waveform panels and local listening guide are excluded. "
         "PCM numerical metrics, finiteness, decoded-WAV roundtrips and musical/physical-audio acceptance cannot be independently replayed here.")


def require(test, message):
    if not test:
        raise ValueError(message)


def load(path):
    def reject(value):
        raise ValueError("Nonfinite JSON: " + value)
    return json.loads(Path(path).read_text(), parse_constant=reject)


def pin(path):
    path = Path(path).resolve()
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest}


def same(left, right):
    return all(left[key] == right[key] for key in ("bytes", "sha256"))


def write(path, value):
    with Path(path).open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def rows(path):
    def reject(value):
        raise ValueError("Nonfinite JSONL: " + value)
    with (gzip.open if str(path).endswith(".gz") else open)(path, "rt") as stream:
        for line in stream:
            require(bool(line.strip()), "Blank JSONL record")
            yield json.loads(line, parse_constant=reject)


def distribution(values):
    ordered = sorted(values)
    require(bool(ordered) and all(math.isfinite(v) and v >= 0 for v in ordered), "Invalid latency values")
    return {"count": len(ordered), **{"p" + str(p): ordered[math.ceil(len(ordered) * p / 100) - 1] for p in (50, 95, 99)}, "max": ordered[-1]}


def lifecycle(sequence, guard):
    require(sequence["status"] == "completed" and sequence["completed_backends"] == ["cpu", "vulkan"]
            and sequence["source_head"] == "f4dc5eef82ee42b7ebe052ab449780fd4c0a2e02"
            and not sequence["profile_mutation"] and sequence["elapsed_seconds"] <= sequence["maximum_seconds"], "Finite sequence incomplete/changed")
    require(len(sequence["launches"]) == 4 and all(row["returncode"] == 0 and row["host"]["AC"] == "1"
            and row["host"]["lid"] == "b false" for row in sequence["launches"]), "Saved spawn/AC/lid evidence differs")
    require(guard["status"] == "ok" and guard["idle_inhibit_released"] and guard["finished_at_utc"]
            and guard["child_returncode"] == guard["returncode"] == 0 and not guard["errors"], "Series guard not finished/released")


def audit_run(archive, backend, run, manifest, analysis, media, facts, summarizer):
    require(run["status"] == "ok" and not run["failures"] and run["backend"] == backend and run["threads"] == 4
            and run["elapsed_seconds"] <= 1800 and (run["window_frames"], run["hop_frames"], run["overlap_frames"]) == (WINDOW, HOP, OVERLAP), "Run setting/status differs")
    require([track["id"] for track in run["tracks"]] == IDS, "Run track order differs")
    for key, expected in FROZEN.items():
        require(run["pins"][key]["sha256"] == expected, "Frozen producer/model differs: " + key)
    require(same(run["pins"]["manifest"], pin(archive / "series/manifest.json"))
            and same(run["pins"]["driver"], pin(archive / "helpers/quality_run.py")), "Executed manifest/driver differs")
    stats = run["worker_statistics"]
    require(stats["closed"] and stats["backend_pid"] is None and stats["backend_start_count"] == 1
            and stats["telemetry_dropped"] == 0 and stats["inflight"] is None and stats["pending_latest_generation"] is None, "Persistent worker closure/evidence differs")
    require(len(run["runtime_ready"]) == 1 and run["runtime_ready"][0]["runtime"]["backend"] == backend
            and run["runtime_ready"][0]["runtime"]["threads"] == 4, "Captured native settings differ")
    events = list(rows(archive / backend / "events.jsonl.gz"))
    trace = list(rows(archive / backend / "native-trace.jsonl.gz"))
    require(sum(e["event"] == "runtime_ready" for e in events) == 1 and all(e["event"] in
            {"control", "runtime_ready", "source_ready", "started", "chunk", "complete"} for e in events), "Unexpected/missing runtime event")
    controls = [e for e in events if e["event"] == "control"]
    starts = {e["request_id"]: e for e in events if e["event"] == "started"}
    require(len(starts) == sum(e["event"] == "started" for e in events), "Duplicate request ID")
    groups, completed, seen = [], {}, set()
    for index, track in enumerate(run["tracks"]):
        require(track["input"] == manifest["tracks"][index]["input_pin"] and same(track["input"], media[index]["prepared_wav"])
                and same(track["output"], media[index]["outputs"][backend]) and track["output"]["complete"]
                and track["output"]["shape"] == [4, N, 2] and track["output"]["dtype"] == "<f4", "Saved source/output identity/geometry differs")
        require(track["continuous"]["start_frame"] == 0 and track["continuous"]["frames"] == N and track["continuous"]["chunks"] == 4, "Incomplete four-hop source")
        groups.append((track, "continuous", track["continuous"]))
    seeks = run["seeks"]
    require([s["start_frame"] for s in seeks] == ([] if backend == "cpu" else [HOP - 1, WINDOW + 1, N - 4096]), "Seek targets differ")
    for seek in seeks:
        require(seek["track_id"] == IDS[0] and seek["numeric_parity"] and seek["parity_tolerance"] == {"rtol": 1e-4, "atol": 1e-5}, "Recorded same-grid seek failed")
        groups.append((run["tracks"][0], "seek", seek))
    for track, scenario, summary in groups:
        generation = summary["generation"]
        require(generation not in seen, "Duplicate collected generation")
        seen.add(generation)
        group = [e for e in events if e["generation"] == generation]
        control = [e for e in group if e["event"] == "control"]
        ready = [e for e in group if e["event"] == "source_ready"]
        ends = [e for e in group if e["event"] == "complete"]
        chunks = [e for e in group if e["event"] == "chunk"]
        require(len(control) == len(ready) == len(ends) == 1 and chunks and all(e["track_id"] == track["id"]
                and e["scenario"] == scenario for e in group), "Missing/duplicate control/source/complete or context mismatch")
        control = control[0]; completed[generation] = ends[0]
        require(control["command"] == ("load" if scenario == "continuous" else "seek") and control["frame_start"] == summary["start_frame"]
                and control["source_path"] == ready[0]["source_path"] == track["input"]["path"]
                and ready[0]["source_id"] == ends[0]["source_id"] == track["input"]["sha256"]
                and ready[0]["source_frames"] == N, "Raw command/start/source/complete identity differs")
        position = summary["start_frame"]
        for event in chunks:
            request = starts[event["request_id"]]
            require(request["generation"] == generation and not request["preroll"]
                    and request["source_id"] == track["input"]["sha256"] and request["window_start_frame"] == event["window_start_frame"], "Chunk/request identity differs")
            require(control["timestamp"] <= request["timestamp"] <= event["ready_at"] <= event["consumed_at"], "Control/request/ready/consume order differs")
            latest = max((c for c in controls if c["timestamp"] <= event["consumed_at"]), key=lambda c: c["timestamp"])
            require(latest["generation"] == generation and event["source_id"] == track["input"]["sha256"]
                    and event["model_id"] == FROZEN["model"] and event["source_frames"] == N and event["sample_rate"] == RATE
                    and event["channels"] == 2 and event["stems"] == STEMS and re.fullmatch("[0-9a-f]{64}", event["samples_sha256"]), "Saved chunk currentness/identity differs")
            require(event["frame_start"] == position and event["window_start_frame"] == position // HOP * HOP
                    and event["frames"] == min(HOP - position % HOP, N - position), "Chunk frame coverage differs")
            position += event["frames"]
        values = distribution([e["hop_service_ms"] for e in chunks])
        first = summary["first_chunk"]
        latency = (chunks[0]["ready_at"] - control["timestamp"]) * 1000
        require(position == ends[0]["frame_start"] == N and summary["frames"] == N - summary["start_frame"]
                and summary["chunks"] == len(chunks) and summary["hop_service_max_ms"] == values["max"]
                and summary["hop_service_percentiles_ms"] == {str(p): values["p" + str(p)] for p in (50, 95, 99)}, "Coverage/timing summary differs")
        require(first["ready_monotonic"] == chunks[0]["ready_at"] and first["delivered_monotonic"] >= chunks[0]["consumed_at"]
                and first["control_to_ready_ms"] == chunks[0]["control_latency_ms"]
                and math.isclose(first["control_to_ready_ms"], latency, rel_tol=1e-12, abs_tol=1e-6), "First-control timing reduction differs")
    require(len(controls) == len(seen) and {e["generation"] for e in events if e["event"] in ("control", "chunk", "complete")} == seen,
            "Unexpected/duplicate delivered control generation")
    continuous = [e for e in events if e["event"] == "chunk" and e["scenario"] == "continuous"]
    require(len(continuous) == 16 and facts["continuous_timing"] == summarizer.timing(continuous), "Sixteen continuous latency reductions differ")
    require(facts["all_delivered_timing"] == summarizer.timing([e for e in events if e["event"] == "chunk"]), "All delivered timing reduction differs")
    require(facts["native_sampling"] == summarizer.native_facts(run, trace, controls, completed), "Saved trace/thermal reductions differ")
    return {"events": len(events), "native_trace_records": len(trace), "continuous_latency_ms": distribution([e["hop_service_ms"] for e in continuous]),
            "seek_controls": len(seeks), "control_currentness_full_coverage_verified": True, "saved_json_thermal_reduction_matches": True}


def audit(archive, repository):
    archive, repository = archive.resolve(), repository.resolve()
    manifest = load(archive / "archive-manifest.json")
    allowed = {"series/" + name for name in JSON_FILES} | {"helpers/" + name for name in PY_FILES} | PLOTS
    allowed |= {"analysis-dependencies/" + name for name, _ in DEPENDENCIES.values()}
    allowed |= {"analysis/result.json", "analysis/metric-selfcheck.json", "excluded-local-identities.json"}
    for backend in ("cpu", "vulkan"):
        allowed |= {"series/" + backend + "-normal-ready.jsonl.gz", backend + "/result.json", backend + "/events.jsonl.gz", backend + "/native-trace.jsonl.gz"}
    require(set(manifest["files"]) == allowed, "Archive differs from explicit member allowlist")
    require({str(p.relative_to(archive)) for p in archive.rglob("*") if p.is_file()} == set(manifest["files"]) | {"archive-manifest.json"}, "Unmanifested/missing archive member")
    counts = {}
    for relative, expected in manifest["files"].items():
        path = archive / relative
        require(archive in path.resolve().parents and not path.is_symlink() and same(pin(path), expected), "Stored member identity/path differs")
        require(path.suffix in (".json", ".py", ".gz") or relative in PLOTS, "Excluded member type")
        if expected["compression"] == "gzip":
            with path.open("rb") as stream: header = stream.read(10)
            require(header[:2] == b"\x1f\x8b" and header[3] & 8 == 0 and header[4:8] == b"\0" * 4, "Non-deterministic gzip header")
            with gzip.open(path, "rb") as stream: raw = stream.read()
            require(len(raw) == expected["raw_bytes"] and hashlib.sha256(raw).hexdigest() == expected["raw_sha256"], "Gzip roundtrip differs")
            counts[relative] = sum(1 for _ in rows(path))
            require(counts[relative] == expected["jsonl_records"], "JSONL count differs")
        elif path.suffix == ".json": load(path)
    require({name for name in manifest["files"] if name.endswith(".png")} == PLOTS, "Only exact four composites are allowed")
    references = manifest["external_references"]
    reference_paths = {**{key: "source-snapshots/" + value for key, value in PRODUCTION.items()},
                       "historical_build": "runtime/historical-build.json", "baseline_manifest": "archive-manifest.json"}
    require(set(references) == set(reference_paths), "External reference list differs")
    for key, entry in references.items():
        require(entry["repository_path"] == BASELINE + "/" + reference_paths[key], "Unexpected canonical baseline path")
        path = repository / entry["repository_path"]
        require(repository in path.resolve().parents and same(pin(path), entry), "Canonical baseline identity differs: " + entry["repository_path"])
    sequence, guard = load(archive / "series/sequence.json"), load(archive / "series/series-guard.json")
    lifecycle(sequence, guard)
    require(set(sequence["producer_pins"].values()) == set(FROZEN.values())
            and sequence["producer_pins"] == sequence["admitted_producer_pins"], "Captured admission producer pins differ")
    for name, expected in sequence["helper_pins"].items():
        require(expected == pin(archive / "helpers" / name)["sha256"], "Admitted helper differs")
    inputs = load(archive / "series/manifest.json"); require([t["id"] for t in inputs["tracks"]] == IDS, "Manifest track list differs")
    require(inputs["excerpt_plan"]["frames"] == N and inputs["excerpt_plan"]["source_offset_frames"] == 1323000
            and inputs["excerpt_plan"]["looping"] is False, "Manifest excerpt plan differs")
    require(sequence["manifest_sha256"] == pin(archive / "series/manifest.json")["sha256"]
            and sequence["driver_sha256"] == pin(archive / "helpers/quality_run.py")["sha256"], "Sequence manifest/driver binding differs")
    require(sequence["input_pins"] == {track["id"]: track["input_pin"] for track in inputs["tracks"]}, "Admitted prepared-input pins differ")
    for backend in ("cpu", "vulkan"):
        ready = load(archive / "series" / (backend + "-normal-ready.json"))
        require(ready["status"] == "matched" and ready["profile_mutation"] is False and ready["service_mutation"] is False
                and ready["rows"] == list(rows(archive / "series" / (backend + "-normal-ready.jsonl.gz"))), "Saved natural-readiness evidence differs")
    for key in ("worker_source", "shared_benchmark_source", "sampler_source"):
        require(references[key]["sha256"] == FROZEN[key], "Baseline producer binding differs")
    build = load(repository / references["historical_build"]["repository_path"])
    require(build["files"]["blocking_binary"]["sha256"] == FROZEN["runtime"] and build["files"]["model"]["sha256"] == FROZEN["model"]
            and build["files"]["native_helper_source"]["sha256"] == references["native_adapter"]["sha256"], "Existing build/source identity differs")
    facts, corpus = load(archive / "series/run-facts.json"), load(archive / "series/corpus-facts.json")
    require(facts["status"] == "ok" and corpus["status"] == "ready" and same(facts["corpus_facts"], pin(archive / "series/corpus-facts.json"))
            and same(facts["analysis_source"], pin(archive / "helpers/summarize_runs.py")), "Facts/source binding differs")
    require(all(same(record, pin(archive / "series" / Path(record["path"]).name)) for record in facts["inputs"]), "Run-facts input hash differs")
    local = load(archive / "excluded-local-identities.json")
    media = local["tracks"]; require([t["id"] for t in media] == IDS, "Excluded asset inventory differs")
    require([track["id"] for track in corpus["tracks"]] == IDS, "Corpus-facts track order differs")
    for record in corpus["inputs"]:
        name = Path(record["path"]).name
        identity = local["records"][name] if name in local["records"] else pin(archive / "series" / name)
        require(same(record, identity), "Safe corpus facts/full local record identity differs")
    revision = load(archive / "series/id-revision.json")
    require(len(revision["tracks"]) == 4 and len({row["old_id"] for row in revision["tracks"]}) == 4, "ID revision count differs")
    for index, row in enumerate(revision["tracks"]):
        require(row["new_id"] == IDS[index] and same(row["old_wav"], row["new_wav"])
                and same(row["new_wav"], media[index]["prepared_wav"]), "ID-only rename altered WAV identity")
        track, source = inputs["tracks"][index], corpus["tracks"][index]
        require(track["annotations"]["source_sha256"] == media[index]["source"]["sha256"]
                and row["source_filename"] == track["annotations"]["source_filename"] == source["source_filename"]
                and same(source["source"], media[index]["source"]) and same(source["prepared_wav"], media[index]["prepared_wav"])
                and source["decode"]["source_frame_offset"] == 1323000 and source["decode"]["frames"] == N
                and source["decode"]["common_stereo_gain"] == track["annotations"]["common_stereo_gain"], "Source/crop/gain/ID revision binding differs")
    for record in revision["initial_documents"].values():
        require(same(record, local["records"][Path(record["path"]).name]), "Initial ID-revision record hash differs")
    require(same(revision["revision_source"], pin(archive / "helpers/revise_track_ids.py"))
            and same(revision["initial_decoder_source_preserved_as"], pin(archive / "helpers/prepare_user_tracks-attempt1.py"))
            and same(revision["corrected_preparation_source"], pin(archive / "helpers/prepare_user_tracks.py")), "ID-revision helper bytes differ")
    analysis = load(archive / "analysis/result.json")
    check = load(archive / "analysis/metric-selfcheck.json")
    require(analysis["status"] == "ok" and analysis["human_listening_verdict"] is None and [t["id"] for t in analysis["tracks"]] == IDS
            and check["status"] == "passed" and len(check["checks"]) == 9, "Saved analysis/selfchecks differ")
    require(same(analysis["inputs"]["analysis_source"], pin(archive / "helpers/analyze_user_tracks.py"))
            and same(check["analysis_source"], analysis["inputs"]["analysis_source"])
            and same(analysis["inputs"]["manifest"], pin(archive / "series/manifest.json")), "Analysis input/source identity differs")
    require(same(analysis["inputs"]["series_guard"], pin(archive / "series/series-guard.json"))
            and analysis["inputs"]["worker_source"]["sha256"] == FROZEN["worker_source"], "Analysis guard/worker identity differs")
    for key, (name, expected) in DEPENDENCIES.items():
        actual = pin(archive / "analysis-dependencies" / name)
        require(actual["sha256"] == expected and same(analysis["inputs"]["dependency_sources"][key], actual)
                and same(check["dependency_sources"][key], actual), "Pinned analysis dependency differs")
    spec = importlib.util.spec_from_file_location("saved_user_track_summary", archive / "helpers/summarize_runs.py")
    summarizer = importlib.util.module_from_spec(spec); spec.loader.exec_module(summarizer)
    runs = {backend: load(archive / backend / "result.json") for backend in ("cpu", "vulkan")}
    results = {}
    for backend, run in runs.items():
        require(same(analysis["inputs"]["runs"][backend], pin(archive / backend / "result.json")), "Analyzed run identity differs")
        for record in facts["backends"][backend]["inputs"]:
            name = Path(record["path"]).name
            if name == "result.json": identity = pin(archive / backend / name)
            else:
                stored = manifest["files"][backend + "/" + name + ".gz"]
                identity = {"bytes": stored["raw_bytes"], "sha256": stored["raw_sha256"]}
            require(same(record, identity), "Backend-facts raw input identity differs")
        require(all(analysis["producer_pins"][key]["sha256"] == run["pins"][key]["sha256"] for key in analysis["producer_pins"]), "Analysis producer pins differ")
        results[backend] = audit_run(archive, backend, run, inputs, analysis, media, facts["backends"][backend], summarizer)
    for index, track in enumerate(analysis["tracks"]):
        entry = media[index]; continuous = track["continuous"]
        require(same(track["input"], entry["prepared_wav"]) and track["geometry_finite_full_coverage_verified"]
                and all(same(track["outputs"][backend], entry["outputs"][backend]) for backend in runs), "Analysis source/output binding differs")
        require(continuous["frames"] == N and continuous["seconds"] == 23.4 and set(continuous["signals"]) == set(entry["continuous_wavs"]) == set(SIGNALS), "Continuous WAV inventory/geometry differs")
        for label in SIGNALS: require(same(continuous["signals"][label]["wav"], entry["continuous_wavs"][label]), "Continuous WAV saved identity differs")
        plot = archive / "analysis" / (track["id"] + "-continuous-waveforms.png")
        require(same(continuous["waveform_plot"], pin(plot)), "Composite PNG identity differs")
        require((continuous["waveform_plot"]["width"], continuous["waveform_plot"]["height"]) == (1800, 1270), "Saved plot dimensions differ")
        with plot.open("rb") as stream: header = stream.read(24)
        require(header[:16] == b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR" and struct.unpack(">II", header[16:24]) == (1800, 1270), "PNG geometry differs")
    return {"status": "passed", "manifest_sha256": pin(archive / "archive-manifest.json")["sha256"], "files": len(manifest["files"]),
            "stored_bytes": sum(v["bytes"] for v in manifest["files"].values()), "jsonl_records": counts, "runs": results,
            "composite_pngs": 4, "excluded_media_identities": 40, "scope": SCOPE,
            "numeric_pcm_metrics_replayed": False, "human_listening_or_physical_audio_verified": False}


def stage(output, visual_confirmed):
    output = output.resolve()
    require(visual_confirmed and output.parent == BASE and not output.exists(), "Root plot approval and fresh ignored output are required")
    lifecycle(load(BASE / "sequence.json"), load(BASE / "series-guard.json"))
    require(load(BASE / "analysis/result.json")["status"] == "ok" and load(BASE / "run-facts.json")["status"] == "ok"
            and load(BASE / "corpus-facts.json")["status"] == "ready", "Completed analysis/facts are required")
    output.mkdir(); files = {}

    def add(source, relative, expected=None):
        source = Path(source); require(source.is_file() and not source.is_symlink(), "Missing plain source: " + str(source))
        original = pin(source); require(expected is None or original["sha256"] == expected, "Pinned source changed")
        dest = output / relative; require(output in dest.resolve().parents and not dest.exists(), "Unsafe duplicate target")
        if source.suffix == ".jsonl": dest = dest.with_name(dest.name + ".gz")
        dest.parent.mkdir(parents=True, exist_ok=True)
        if source.suffix == ".jsonl":
            with source.open("rb") as inp, dest.open("wb") as out:
                with gzip.GzipFile(filename="", fileobj=out, mode="wb", mtime=0, compresslevel=6) as zipped: shutil.copyfileobj(inp, zipped)
        else: shutil.copyfile(source, dest)
        identity = pin(dest)
        entry = {"source": str(source.resolve()), "bytes": identity["bytes"], "sha256": identity["sha256"],
                 "raw_bytes": original["bytes"], "raw_sha256": original["sha256"], "compression": "gzip" if source.suffix == ".jsonl" else None}
        if source.suffix == ".jsonl": entry["jsonl_records"] = sum(1 for _ in rows(source))
        elif source.suffix == ".json": load(source)
        files[str(dest.relative_to(output))] = entry

    for name in JSON_FILES: add(BASE / name, "series/" + name)
    for name in PY_FILES: add(BASE / name, "helpers/" + name)
    for backend in ("cpu", "vulkan"):
        add(BASE / (backend + "-normal-ready.jsonl"), "series/" + backend + "-normal-ready.jsonl")
        for name in ("result.json", "events.jsonl", "native-trace.jsonl"): add(BASE / backend / name, backend + "/" + name)
    for name in ("result.json", "metric-selfcheck.json"): add(BASE / "analysis" / name, "analysis/" + name)
    analysis = load(BASE / "analysis/result.json")
    for track in analysis["tracks"]:
        relative = "analysis/" + track["id"] + "-continuous-waveforms.png"
        require(relative in PLOTS, "Unexpected composite path"); add(BASE / relative, relative, track["continuous"]["waveform_plot"]["sha256"])
    for name, expected in DEPENDENCIES.values(): add(ROOT / BASELINE / "helpers" / name, "analysis-dependencies/" + name, expected)
    baseline_manifest = load(ROOT / BASELINE / "archive-manifest.json")
    references = {}
    for key, relative in {**{k: "source-snapshots/" + v for k, v in PRODUCTION.items()}, "historical_build": "runtime/historical-build.json", "baseline_manifest": "archive-manifest.json"}.items():
        source = ROOT / BASELINE / relative; identity = pin(source)
        if key != "baseline_manifest": require(same(identity, baseline_manifest["files"][relative]), "Canonical baseline member identity differs")
        if key in PRODUCTION: require(same(identity, pin(ROOT / PRODUCTION[key])), "Current production source differs from baseline")
        references[key] = {"repository_path": BASELINE + "/" + relative, "bytes": identity["bytes"], "sha256": identity["sha256"]}
    corpus = load(BASE / "corpus-facts.json"); runs = {k: load(BASE / k / "result.json") for k in ("cpu", "vulkan")}
    media = []
    for index, track in enumerate(corpus["tracks"]):
        entry = {"id": track["id"], "outputs": {}, "continuous_wavs": {}}
        for key in ("source", "prepared_wav"):
            actual = pin(track[key]["path"]); require(same(actual, track[key]), "Excluded original/prepared file changed"); entry[key] = actual
        for backend, run in runs.items():
            info = run["tracks"][index]["output"]; actual = pin(info["path"])
            require(same(actual, info), "Excluded output array changed"); entry["outputs"][backend] = actual
        for label, info in analysis["tracks"][index]["continuous"]["signals"].items():
            actual = pin(info["wav"]["path"]); require(same(actual, info["wav"]), "Excluded continuous WAV changed"); entry["continuous_wavs"][label] = actual
        media.append(entry)
    records = {name: pin(BASE / name) for name in LOCAL_RECORDS}
    generated = {"excluded-local-identities.json": {"tracks": media, "records": records,
        "scope": "Stage-time read-only hashes; no media or full metadata dump copied. Original records and samples are unavailable to media-free replay."}}
    for relative, value in generated.items():
        dest = output / relative; write(dest, value); identity = pin(dest)
        files[relative] = {"source": "generated identity-only inventory", "bytes": identity["bytes"], "sha256": identity["sha256"],
                           "raw_bytes": identity["bytes"], "raw_sha256": identity["sha256"], "compression": None}
    write(output / "archive-manifest.json", {"schema": 1, "scope": SCOPE, "root_confirmed_four_composites_visually_reviewed": True,
          "facts_derivation_qualification": "A prepublication assertion correction selected the continuous source_ready generation separately from completed-source seek reuse; only final successful facts/helper bytes are included.",
          "external_references": references, "files": dict(sorted(files.items()))})
    return audit(output, ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__); sub = parser.add_subparsers(dest="mode", required=True)
    packing = sub.add_parser("stage"); packing.add_argument("--output", type=Path, default=BASE / "staged-archive")
    packing.add_argument("--visual-confirmed", action="store_true")
    checking = sub.add_parser("audit"); checking.add_argument("archive", type=Path)
    checking.add_argument("--repository-root", type=Path, default=Path.cwd()); checking.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.mode == "stage": result = stage(args.output, args.visual_confirmed)
    else:
        if args.output: require(args.archive.resolve() not in args.output.resolve().parents, "Audit output must be outside archive")
        result = audit(args.archive, args.repository_root)
        if args.output: write(args.output, result)
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__": main()

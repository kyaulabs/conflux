#!/usr/bin/env python3
"""Package completed M0 music evidence, or audit its saved facts without media.

Stage only after both workloads and the series guard exit. No inference, host
queries, playback, build, download, or numerical quality replay is performed.
"""
import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import struct

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
RATE, WINDOW, HOP, OVERLAP, FRAMES = 44100, 343980, 257985, 85995, 1031940
STEMS = ["drums", "bass", "other", "vocals"]
SIGNALS = ["source", *STEMS, "recombined"]
PLOTS = {"waveform-review/original-preview-layout.png": (1800, 1030),
         **{"waveform-review/" + name + "-continuous-waveforms.png": (1800, 1270)
            for name in ("parting_glass", "funkorama", "club_diver")}}
FROZEN = {
    "runtime": "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6",
    "model": "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6",
    "worker_source": "308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8",
    "shared_benchmark_source": "87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7",
    "sampler_source": "8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b",
}
SCOPE = ("Archive identity and saved-event/geometry/control audit only. Original MP3/WAV/NPY, "
         "runtime/model binaries and raw site dumps are excluded. Saved sample/output hashes are "
         "identities, not reconstructed samples. Numerical metrics, sample finiteness, audio quality "
         "and audible seams cannot be independently replayed from this media-free archive. "
         "Four composite waveform PNGs are derived summaries; raw FFmpeg panels are excluded. "
         "Image identities/dimensions and saved assertions do not establish musical acceptance.")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def invalid_constant(value):
    raise ValueError("Nonfinite JSON constant: " + value)


def load(path):
    return json.loads(Path(path).read_text(), parse_constant=invalid_constant)


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def pin(path):
    path = Path(path).resolve()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}


def same_pin(left, right):
    return all(left[key] == right[key] for key in ("bytes", "sha256"))


def sha(value):
    return isinstance(value, str) and re.fullmatch("[0-9a-f]{64}", value) is not None


def write(path, value):
    with Path(path).open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def rows(path):
    with (gzip.open if str(path).endswith(".gz") else open)(path, "rt") as stream:
        for line in stream:
            require(bool(line.strip()), "Blank JSONL record: " + str(path))
            yield json.loads(line, parse_constant=invalid_constant)


def completed(sequence, guard):
    require(sequence["status"] == "completed" and sequence["completed_backends"] == ["cpu", "vulkan"]
            and sequence["elapsed_seconds"] <= sequence["maximum_seconds"]
            and sequence["profile_mutation"] is False, "Finite sequence has not completed")
    require(guard["status"] == "ok" and guard["idle_inhibit_released"] and not guard["errors"]
            and guard.get("finished_at_utc"), "Series guard has not finished/released")


def waveform_audit(archive, analysis, identities, ids):
    report = load(archive / "waveform-review/result.json")
    numerical = load(archive / "series/waveform-numerics.json")
    require(report["status"] == "ok" and [row["id"] for row in report["tracks"]] == ids
            and [row["id"] for row in numerical["tracks"]] == ids, "Waveform report status/track list differs")
    bindings = {"manifest": "series/manifest.json", "analysis": "analysis/result.json", "vulkan": "vulkan/result.json",
                "numerical_report": "series/waveform-numerics.json", "source": "helpers/waveform_review.py"}
    for key, relative in bindings.items():
        require(same_pin(report["inputs"][key], pin(archive / relative)), "Waveform input/helper identity differs: " + key)
    require(numerical["analysis_source_sha256"] == digest(archive / "helpers/waveform_numerics.py")
            and numerical["window_frames"] == 882 and numerical["window_seconds"] == .02, "Numerical helper/window metadata differs")
    numeric_inputs = {report["inputs"]["manifest"]["path"]: pin(archive / "series/manifest.json"),
                      report["inputs"]["analysis"]["path"]: pin(archive / "analysis/result.json")}
    for backend in ("cpu", "vulkan"):
        numeric_inputs[analysis["inputs"]["runs"][backend]["path"]] = pin(archive / backend / "result.json")
    plots = {"waveform-review/original-preview-layout.png": report["original_preview_layout_plot"]}
    for index, row in enumerate(report["tracks"]):
        local = identities[index]
        require(same_pin(row["source"], local["input_wav"])
                and same_pin(row["raw_vulkan"], local["outputs"]["vulkan"])
                and same_pin(row["original_preview"], local["auditions"]["lossless_wav"]), "Waveform source/output/old-preview identity differs")
        require(set(row["signals"]) == set(local["continuous_wavs"]) == set(SIGNALS), "Continuous signal inventory differs")
        for label in SIGNALS:
            require(same_pin(row["signals"][label]["wav"], local["continuous_wavs"][label]), "Excluded continuous WAV identity differs")
        numeric = numerical["tracks"][index]
        require(numeric["frames"] == FRAMES and numeric["seconds"] == 23.4
                and numeric["original_decoded_source_offset_seconds"] == 30.0, "Numerical report excerpt geometry differs")
        numeric_inputs[local["input_wav"]["path"]] = local["input_wav"]
        numeric_inputs.update({info["path"]: info for info in local["outputs"].values()})
        plots["waveform-review/" + row["id"] + "-continuous-waveforms.png"] = row["continuous_waveform_plot"]
    require(set(numerical["inputs"]) == set(numeric_inputs)
            and all(same_pin(numerical["inputs"][path], info) for path, info in numeric_inputs.items()), "Numerical report input identities differ")
    require(set(plots) == set(PLOTS), "Composite plot list differs")
    for relative, recorded in plots.items():
        path = archive / relative
        require(same_pin(recorded, pin(path)), "Composite PNG identity differs")
        with path.open("rb") as stream:
            header = stream.read(24)
        require(header[:8] == b"\x89PNG\r\n\x1a\n" and header[8:16] == b"\x00\x00\x00\x0dIHDR"
                and struct.unpack(">II", header[16:24]) == PLOTS[relative], "Composite PNG header/dimensions differ")
    require((archive / "series/human-listening-report.json").is_file()
            and (archive / "series/waveform-independent-review.json").is_file()
            and (archive / "series/waveform-followup.json").is_file(), "Waveform follow-up/user review facts missing")
    return {"composite_pngs": 4, "excluded_continuous_wav_identities": 18, "helper_input_hashes_match": True,
            "png_dimensions_match": True, "saved_numeric_findings_recomputed": False,
            "scope": "Input/image identities and saved metadata only; original PCM/arrays are excluded and no numerical/audio/visual acceptance verdict is replayed."}


def audit(archive):
    archive = Path(archive).resolve()
    manifest = load(archive / "archive-manifest.json")
    expected = set(manifest["files"]) | {"archive-manifest.json"}
    actual = {str(path.relative_to(archive)) for path in archive.rglob("*") if path.is_file()}
    require(actual == expected, "Archive has missing or unmanifested files")
    jsonl_counts, raw_bytes = {}, 0
    for relative, info in manifest["files"].items():
        path = archive / relative
        require(archive in path.resolve().parents and not path.is_symlink(), "Unsafe archive member")
        require(path.suffix in (".json", ".gz", ".py", ".cpp", ".cmake", ".txt")
                or (path.suffix == ".png" and relative in PLOTS), "Excluded member type")
        require(path.stat().st_size == info["bytes"] and digest(path) == info["sha256"], "Stored identity differs: " + relative)
        if info["compression"] == "gzip":
            with path.open("rb") as stream:
                header = stream.read(10)
            require(header[:2] == b"\x1f\x8b" and header[3] & 8 == 0 and header[4:8] == b"\0" * 4,
                    "Gzip header is not deterministic")
            with gzip.open(path, "rb") as stream:
                raw = stream.read()
            require(len(raw) == info["raw_bytes"] and hashlib.sha256(raw).hexdigest() == info["raw_sha256"], "Gzip raw identity differs")
            count = sum(1 for _ in rows(path))
            require(count == info["jsonl_records"], "JSONL count differs")
            jsonl_counts[relative] = count
        elif path.suffix == ".json":
            load(path)
        raw_bytes += info["raw_bytes"]
    sequence = load(archive / "series/sequence.json")
    completed(sequence, load(archive / "series/series-guard.json"))
    require(set(sequence["producer_pins"].values()) == set(FROZEN.values())
            and sequence["producer_pins"] == sequence["admitted_producer_pins"]
            and sequence["host"]["AC"] == "1" and sequence["host"]["lid"] == "b false", "Saved producer admission/host conditions differ")
    corpus = load(archive / "series/manifest.json")
    provenance = load(archive / "series/corpus-provenance.json")
    readiness = load(archive / "series/corpus-readiness.json")
    ids = [track["id"] for track in corpus["tracks"]]
    require(ids == ["parting_glass", "funkorama", "club_diver"] and len(set(ids)) == 3, "Corpus track list differs")
    require(provenance["status"] == readiness["status"] == "ready"
            and provenance["audio_download_bytes"] <= 40000000
            and provenance["manifest"]["sha256"] == digest(archive / "series/manifest.json") == readiness["manifest_sha256"], "Corpus readiness/manifest differs")
    plan = corpus["excerpt_plan"]
    require((plan["source_offset_frames"], plan["excerpt_frames"], plan["hop_frames"], plan["looping"])
            == (1323000, FRAMES, HOP, False), "Predeclared excerpt geometry differs")
    runs = {backend: load(archive / backend / "result.json") for backend in ("cpu", "vulkan")}
    checks = {}
    all_local = load(archive / "excluded-local-identities.json")["tracks"]
    for backend, run in runs.items():
        ready = load(archive / "series" / (backend + "-normal-ready.json"))
        require(ready["status"] == "matched" and ready["profile_mutation"] is False
                and ready["service_mutation"] is False, "Saved natural readiness did not match")
        require(run["status"] == "ok" and not run["failures"] and run["backend"] == backend
                and run["threads"] == 4 and run["elapsed_seconds"] <= 1800
                and (run["window_frames"], run["hop_frames"], run["overlap_frames"]) == (WINDOW, HOP, OVERLAP), "Run settings/status differ")
        for key, expected_sha in FROZEN.items():
            require(run["pins"][key]["sha256"] == expected_sha, "Frozen pin differs: " + key)
        require(run["pins"]["manifest"]["sha256"] == sequence["manifest_sha256"] == digest(archive / "series/manifest.json")
                and run["pins"]["driver"]["sha256"] == sequence["driver_sha256"] == digest(archive / "helpers/quality_run.py"), "Executed manifest/driver differs")
        stats = run["worker_statistics"]
        require(stats["closed"] and stats["backend_pid"] is None and stats["backend_start_count"] == 1
                and stats["telemetry_dropped"] == 0, "Worker closure/persistence evidence differs")
        require(len(run["runtime_ready"]) == 1, "Expected one persistent native identity")
        native = run["runtime_ready"][0]["runtime"]
        require(native["backend"] == backend and native["threads"] == 4, "Captured native settings differ")
        events = list(rows(archive / backend / "events.jsonl.gz"))
        require(sum(event["event"] == "runtime_ready" for event in events) == 1
                and not any(event["event"] in ("error", "discarded") for event in events), "Event stream has errors/discards or multiple native identities")
        started = {event["request_id"]: event for event in events if event["event"] == "started"}
        require(len(started) == sum(event["event"] == "started" for event in events), "Duplicate native request ID")
        require([track["id"] for track in run["tracks"]] == ids, "Run track order differs")
        groups, seen_generations = [], set()
        for index, track in enumerate(run["tracks"]):
            source = provenance["tracks"][index]
            require(track["input"]["frames"] == FRAMES and same_pin(track["input"], source["input_wav"])
                    and same_pin(track["input"], all_local[index]["input_wav"])
                    and same_pin(track["output"], all_local[index]["outputs"][backend])
                    and track["output"]["complete"] and track["output"]["shape"] == [4, FRAMES, 2]
                    and track["output"]["dtype"] == "<f4" and sha(track["output"]["sha256"]), "Saved source/output identity or declared geometry differs")
            groups.append((track, "continuous", track["continuous"]))
        seeks = run["seeks"]
        expected_seeks = [] if backend == "cpu" else [HOP - 1, HOP + OVERLAP + 1, FRAMES - 4096]
        require([seek["start_frame"] for seek in seeks] == expected_seeks, "Seek controls differ")
        for seek in seeks:
            require(seek["track_id"] == ids[0] and seek["numeric_parity"]
                    and seek["parity_tolerance"] == {"rtol": 1e-4, "atol": 1e-5}, "Saved same-grid seek comparison did not pass")
            groups.append((run["tracks"][0], "seek", seek))
        for track, scenario, summary in groups:
            generation = summary["generation"]
            require(generation not in seen_generations, "Duplicate collected generation")
            seen_generations.add(generation)
            group = [event for event in events if event["generation"] == generation]
            require(all(event["track_id"] == track["id"] and event["scenario"] == scenario for event in group), "Generation/scenario context differs")
            controls = [event for event in group if event["event"] == "control"]
            require(len(controls) == 1, "Expected exactly one raw control per collected generation")
            control = controls[0]
            require(control["command"] == ("load" if scenario == "continuous" else "seek")
                    and control["frame_start"] == summary["start_frame"]
                    and control["source_path"] == track["input"]["path"]
                    and (scenario != "continuous" or summary["start_frame"] == 0), "Raw command/start/source differs from collected scenario")
            chunks = [event for event in group if event["event"] == "chunk"]
            complete = [event for event in group if event["event"] == "complete"]
            prepared = [event for event in group if event["event"] == "source_ready"]
            require(len(prepared) == len(complete) == 1 and prepared[0]["source_id"] == track["input"]["sha256"]
                    and prepared[0]["source_frames"] == FRAMES
                    and complete[0]["source_id"] == track["input"]["sha256"], "Source-ready/complete identity differs")
            position, services = summary["start_frame"], []
            for event in chunks:
                require(event["source_id"] == track["input"]["sha256"] and event["source_frames"] == FRAMES
                        and event["model_id"] == FROZEN["model"] and event["sample_rate"] == RATE
                        and event["channels"] == 2 and event["stems"] == STEMS and sha(event["samples_sha256"])
                        and event["frame_start"] == position and event["window_start_frame"] == position // HOP * HOP
                        and event["frames"] == min(HOP - position % HOP, FRAMES - position), "Chunk identity/frame coverage differs")
                request = started[event["request_id"]]
                require(request["generation"] == generation and request["window_start_frame"] == event["window_start_frame"]
                        and not request["preroll"], "Chunk/request identity differs")
                require(control["timestamp"] <= request["timestamp"] <= event["ready_at"] <= event["consumed_at"],
                        "Raw control/request/ready/consumption ordering differs")
                require(math.isfinite(event["hop_service_ms"]) and event["hop_service_ms"] >= 0, "Invalid service observation")
                position += event["frames"]
                services.append(event["hop_service_ms"])
            require(position == complete[0]["frame_start"] == FRAMES and summary["frames"] == FRAMES - summary["start_frame"]
                    and summary["chunks"] == len(chunks) and sha(summary["chunk_stream_sha256"]), "Saved summary coverage differs")
            ordered = sorted(services)
            percentiles = {str(p): ordered[math.ceil(len(ordered) * p / 100) - 1] for p in (50, 95, 99)}
            require(summary["hop_service_percentiles_ms"] == percentiles and summary["hop_service_max_ms"] == max(services), "Saved latency reduction differs")
            first = summary["first_chunk"]
            first_control_ms = (chunks[0]["ready_at"] - control["timestamp"]) * 1000
            require(first["ready_monotonic"] == chunks[0]["ready_at"]
                    and first["control_to_ready_ms"] == chunks[0]["control_latency_ms"]
                    and math.isclose(first["control_to_ready_ms"], first_control_ms, rel_tol=1e-12, abs_tol=1e-6)
                    and first["delivered_monotonic"] >= chunks[0]["consumed_at"] >= first["ready_monotonic"], "Saved first-control timing differs")
        require({event["generation"] for event in events if event["event"] in ("chunk", "complete")} == seen_generations, "Unexpected delivered generation")
        require({event["generation"] for event in events if event["event"] == "control"} == seen_generations, "Unexpected raw control generation")
        trace = list(rows(archive / backend / "native-trace.jsonl.gz"))
        trace_info = run["native_memory_cpu"]["trace"]
        require(len(trace) == trace_info["samples_written"] and not trace_info["errors"] and len(trace) > 0
                and all(row["pid"] == native["pid"] for row in trace)
                and len({row["start_ticks"] for row in trace}) == 1
                and all(a["timestamp_monotonic"] <= b["timestamp_monotonic"] for a, b in zip(trace, trace[1:])), "Native trace identity/count/order differs")
        checks[backend] = {"event_records": len(events), "native_trace_records": len(trace), "continuous_tracks": len(ids),
                           "seek_controls": len(seeks), "collected_generations": len(groups), "raw_controls_verified": True,
                           "saved_latency_reductions_match": True, "first_control_latency_recomputed": True}
    analysis = load(archive / "analysis/result.json")
    require(analysis["status"] == "ok" and analysis["stem_order"] == STEMS
            and (analysis["sample_rate"], analysis["hop_frames"], analysis["overlap_frames"]) == (RATE, HOP, OVERLAP), "Saved analysis geometry differs")
    require(analysis["inputs"]["manifest"]["sha256"] == digest(archive / "series/manifest.json"), "Analyzed manifest identity differs")
    require(analysis["inputs"]["analysis_source"]["sha256"] == digest(archive / "helpers/quality_analyze.py")
            == load(archive / "series/metric-selfcheck.json")["analysis_source_sha256"], "Analysis/selfcheck source identity differs")
    require(load(archive / "series/metric-selfcheck.json")["status"] == "passed", "Metric hand checks did not pass")
    for backend, run in runs.items():
        require(analysis["inputs"]["runs"][backend]["sha256"] == digest(archive / backend / "result.json"), "Analyzed run identity differs")
    require([row["id"] for row in analysis["tracks"]] == ids, "Analysis track list differs")
    for index, row in enumerate(analysis["tracks"]):
        require(same_pin(row["input"], all_local[index]["input_wav"])
                and row["geometry_finite_full_coverage_verified"] is True
                and all(same_pin(row["outputs"][backend], all_local[index]["outputs"][backend]) for backend in runs), "Analyzed source/output hash or saved geometry assertion differs")
        require(same_pin(row["audition"], all_local[index]["auditions"]["lossy_mp3"])
                and same_pin(row["audition"]["lossless_wav"], all_local[index]["auditions"]["lossless_wav"]), "Saved excluded audition identity differs")
    waveform_checks = waveform_audit(archive, analysis, all_local, ids)
    return {"status": "passed", "manifest_sha256": digest(archive / "archive-manifest.json"),
            "files": len(manifest["files"]), "stored_bytes": sum(info["bytes"] for info in manifest["files"].values()),
            "raw_bytes": raw_bytes, "jsonl_records": jsonl_counts, "runs": checks, "waveform_checks": waveform_checks, "scope": SCOPE,
            "numerical_metrics_replayed": False, "human_listening_verified_by_this_audit": False}


def stage(output, analysis_dir):
    output, analysis_dir = output.resolve(), analysis_dir.resolve()
    require(output.parent == BASE and not output.exists(), "Use a fresh immediate child of the ignored quality base")
    completed(load(BASE / "sequence.json"), load(BASE / "series-guard.json"))
    runs = {backend: load(BASE / backend / "result.json") for backend in ("cpu", "vulkan")}
    require(all(run["status"] == "ok" and not run["failures"] for run in runs.values()), "Both successful runs are required")
    analysis = load(analysis_dir / "result.json")
    require(analysis["status"] == "ok", "Completed saved-output analysis is required")
    waveform = load(BASE / "waveform-review/result.json")
    require(waveform["status"] == "ok" and (BASE / "waveform-followup.json").is_file(), "Completed waveform investigation/follow-up facts required before fresh staging")
    output.mkdir()
    files = {}

    def add(source, relative, expected=None, classification="saved evidence"):
        source = Path(source)
        require(source.is_file() and not source.is_symlink(), "Missing plain source: " + str(source))
        require(source.suffix in (".json", ".jsonl", ".py", ".cpp", ".cmake", ".txt")
                or (source.suffix == ".png" and relative in PLOTS), "Excluded source type")
        raw = pin(source)
        require(expected is None or raw["sha256"] == expected, "Frozen source changed: " + str(source))
        target = output / relative
        compressed = source.suffix == ".jsonl"
        if compressed:
            target = target.with_name(target.name + ".gz")
        require(output in target.resolve().parents and not target.exists(), "Unsafe or duplicate target")
        target.parent.mkdir(parents=True, exist_ok=True)
        if compressed:
            with source.open("rb") as inp, target.open("wb") as out:
                with gzip.GzipFile(filename="", fileobj=out, mode="wb", mtime=0, compresslevel=6) as zipped:
                    shutil.copyfileobj(inp, zipped)
        else:
            shutil.copyfile(source, target)
        entry = {"source": str(source.resolve()), "classification": classification, "raw_sha256": raw["sha256"],
                 "raw_bytes": raw["bytes"], "sha256": digest(target), "bytes": target.stat().st_size,
                 "compression": "gzip" if compressed else None}
        if compressed:
            entry["jsonl_records"] = sum(1 for _ in rows(source))
        elif source.suffix == ".json":
            load(source)
        files[str(target.relative_to(output))] = entry

    for source in sorted(BASE.iterdir()):
        if source.is_file() and source.suffix in (".py", ".json", ".jsonl"):
            require(source.name not in ("archive-manifest.json", "archived-audit.json"), "Keep package output outside root source-file set")
            add(source, ("helpers/" if source.suffix == ".py" else "series/") + source.name,
                classification="historical earlier-stage audit; not the current archive audit" if re.fullmatch(r"staged-file-audit(?:[0-9]+)?\.json", source.name)
                else "failed preparation evidence" if "attempt1" in source.name else "saved helper or structured fact")
    for name in ("metadata-http.json", "selected-track-metadata.json", "selected-track-metadata-attempt1.json", "doxent-head-attempt2.json"):
        add(BASE / "assets/source-evidence" / name, "corpus/" + name, classification="selected factual metadata; full site dumps excluded")
    for backend, run in runs.items():
        for name in ("result.json", "events.jsonl", "native-trace.jsonl"):
            add(BASE / backend / name, backend + "/" + name)
    add(analysis_dir / "result.json", "analysis/result.json")
    add(BASE / "waveform-review/result.json", "waveform-review/result.json")
    plot_records = {"waveform-review/original-preview-layout.png": waveform["original_preview_layout_plot"],
                    **{"waveform-review/" + row["id"] + "-continuous-waveforms.png": row["continuous_waveform_plot"]
                       for row in waveform["tracks"]}}
    require(set(plot_records) == set(PLOTS), "Only the exact four composite images may be packaged")
    for relative, recorded in plot_records.items():
        add(BASE / relative, relative, expected=recorded["sha256"], classification="visually reviewed composite waveform summary; no raw FFmpeg panel")
    for key, relative in (("worker_source", "tools/stems/worker.py"), ("shared_benchmark_source", "tools/benchmarks/stem_worker.py"),
                          ("sampler_source", "tools/benchmarks/worker_support.py")):
        require(all(run["pins"][key]["sha256"] == FROZEN[key] for run in runs.values()), "Source pins differ")
        add(ROOT / relative, "source-snapshots/" + relative, expected=FROZEN[key], classification="executed source pin")
    build = load(ROOT / "docs/benchmarks/data/latency/build.json")
    require(build["files"]["blocking_binary"]["sha256"] == FROZEN["runtime"]
            and build["files"]["model"]["sha256"] == FROZEN["model"], "Historical build record differs from run pins")
    add(ROOT / "docs/benchmarks/data/latency/build.json", "runtime/historical-build.json", classification="existing historical build metadata; not new independent attestation")
    for relative in ("tools/runtime/htdemucs_segment.cpp", "tools/runtime/CMakeLists.txt", "tools/runtime/BlockingVulkanWait.cmake"):
        expected = build["files"]["native_helper_source"]["sha256"] if relative.endswith(".cpp") else None
        add(ROOT / relative, "source-snapshots/" + relative, expected=expected,
            classification="current adapter/build source; existing historical record retained, no new build attestation")
    corpus = load(BASE / "corpus-provenance.json")
    identities, credits = [], []
    for index, track in enumerate(corpus["tracks"]):
        item = {"id": track["id"], "outputs": {}, "auditions": {}, "continuous_wavs": {}}
        for key in ("source_mp3", "input_wav"):
            actual = pin(track[key]["path"])
            require(same_pin(actual, track[key]), "Local excluded source identity changed")
            item[key] = actual
        for backend, run in runs.items():
            saved = run["tracks"][index]
            require(saved["id"] == track["id"], "Local output track order differs")
            actual = pin(saved["output"]["path"])
            require(same_pin(actual, saved["output"]), "Local excluded output identity changed")
            item["outputs"][backend] = actual
        audition = analysis["tracks"][index]
        require(audition["id"] == track["id"], "Local audition track order differs")
        for key, saved in (("lossy_mp3", audition["audition"]), ("lossless_wav", audition["audition"]["lossless_wav"])):
            actual = pin(saved["path"])
            require(same_pin(actual, saved), "Local excluded audition identity changed")
            item["auditions"][key] = actual
        continuous = waveform["tracks"][index]
        require(continuous["id"] == track["id"] and set(continuous["signals"]) == set(SIGNALS), "Continuous waveform signal/track inventory differs")
        for label in SIGNALS:
            saved = continuous["signals"][label]["wav"]
            expected_path = BASE / "waveform-review" / (track["id"] + "-" + label + ".wav")
            require(Path(saved["path"]).resolve() == expected_path, "Unexpected continuous WAV path")
            actual = pin(expected_path)
            require(same_pin(actual, saved), "Local excluded continuous WAV identity changed")
            item["continuous_wavs"][label] = actual
        identities.append(item)
        annotation = track["annotations"]
        credits.append({key: annotation[key] for key in ("id", "artist", "title", "source_page", "download_url", "license", "license_url", "attribution", "official_tags", "bpm")})
    generated = {"excluded-local-identities.json": {"tracks": identities, "scope": "Original audio/arrays hashed at packaging, then excluded. Archive audit cross-checks these saved identities only."},
                 "corpus/attribution-facts.json": {"tracks": credits, "source_snapshot_identities": corpus["source_evidence"],
                     "site_and_audio_license_distinction": corpus["source_page_license_note"], "obligations": corpus["license_obligations"]}}
    for relative, value in generated.items():
        path = output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        write(path, value)
        info = pin(path)
        files[relative] = {"source": "generated from saved structured facts/local identities", "classification": "factual provenance",
                           "sha256": info["sha256"], "bytes": info["bytes"], "raw_sha256": info["sha256"], "raw_bytes": info["bytes"], "compression": None}
    write(output / "archive-manifest.json", {"schema": 1, "scope": SCOPE, "frozen_pins": FROZEN,
          "expected_source_head": "fd4aee1bba0c529277b20ae15ffc4484ca8d7dc7", "files": dict(sorted(files.items()))})
    return audit(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    packing = sub.add_parser("stage")
    packing.add_argument("--output", type=Path, default=BASE / "staged-archive")
    packing.add_argument("--analysis-dir", type=Path, default=BASE / "analysis")
    checking = sub.add_parser("audit")
    checking.add_argument("archive", type=Path)
    checking.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.mode == "stage":
        result = stage(args.output, args.analysis_dir)
    else:
        if args.output:
            require(args.archive.resolve() not in args.output.resolve().parents, "Audit output must be outside archive")
        result = audit(args.archive)
        if args.output:
            write(args.output, result)
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()

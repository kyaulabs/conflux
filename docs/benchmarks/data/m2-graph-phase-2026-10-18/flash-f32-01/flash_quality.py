#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Dry by default: pinned native PCM comparison, without a quality threshold."""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import time

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
COMPARE = BASE / "compare_runtime.py"
COMPARE_SHA = "5022aeed8eaf932adead05404f80b94c7d992f4b6f68dc81d05edb6126077e5b"
BASELINE = Path("/tmp/conflux-stem-phase-timing-2026-10-18/build/conflux-stem-runtime")
BASELINE_SHA = "ded9cb2c1000af78917a784e281d905da201d735338221519958d9d7df1d681f"
CANDIDATE_ROOT = Path("/tmp/conflux-stem-flash-f32-2026-10-18")
CANDIDATE = CANDIDATE_ROOT / "build/conflux-stem-runtime"
PROOF = BASE / "phase-timing-laptop-01-comparison/result.json"
PROOF_SHA = "38c60301feed297a0484d61ea92cca2413067d15d7ead7e9a27e6cc9909037e3"
OVERLAY = BASE / "flash_f32_overlay.cmake"
OVERLAY_SHA = "a4951aadce8272b103e31a12c121c237ca8545b926690ebcc60708d979e7eb15"
HEADER_SHA = "7f48bbd4964ebcce721f7544a84303c568b460bf21c24090899ccc8b983833cd"
PIPELINE = CANDIDATE_ROOT / "build/conflux-flash-f32/pipeline.cpp"
VULKAN = CANDIDATE_ROOT / "build/conflux-flash-f32/ggml-vulkan.cpp"
HEADER = CANDIDATE_ROOT / "build/conflux-graph-phase-timing/include/engine/models/demucs/pipeline.h"
SPOOL_PARENT = BASE / "native-temp"
OUTPUT_BYTES = 4 * 343980 * 2 * 4
BASELINE_BYTES = 16 * OUTPUT_BYTES
REQUEST_SECONDS, TOTAL_SECONDS, CLEANUP_RESERVE = 30, 600, 60
SCOPE = ("Numerical comparison only: sixteen original normalized windows per persistent runtime, "
         "baseline then Flash, one child at a time. All output samples/hashes and valid/padded regions "
         "remain represented. No numerical acceptance threshold, musical-quality verdict, latency "
         "qualification, p99, UI/audio load or host policy change.")


def require(value, message):
    if not value:
        raise ValueError(message)


def helper():
    require(COMPARE.stat().st_size <= 128 * 1024
            and hashlib.sha256(COMPARE.read_bytes()).hexdigest() == COMPARE_SHA,
            "Frozen comparison helper differs")
    spec = importlib.util.spec_from_file_location("conflux_flash_quality_common", COMPARE)
    compare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(compare)
    compare.BASELINE, compare.DIAGNOSTIC = BASELINE, CANDIDATE
    shaders = compare.UPSTREAM / "external/ggml/src/ggml-vulkan/vulkan-shaders"
    compare.REQUIRED_SOURCES = tuple(dict.fromkeys(compare.REQUIRED_SOURCES + (
        Path(__file__).resolve(), BASE / "flash_quality_launch.py", OVERLAY, PIPELINE, VULKAN, HEADER,
        BASELINE.parent / "conflux-graph-phase-timing/pipeline.cpp",
        *(shaders / name for name in ("vulkan-shaders-gen.cpp", "flash_attn.comp", "flash_attn_base.glsl",
                                      "flash_attn_dequant.glsl", "types.glsl")),
        *(CANDIDATE_ROOT / "source" / name for name in
          ("CMakeLists.txt", "GraphPhaseTiming.cmake", "BlockingVulkanWait.cmake", "htdemucs_segment.cpp")))))
    return compare


def reject_environment():
    require(not any(key in os.environ for key in ("INTEL_DEBUG", "MESA_GPU_TRACES", "MESA_GPU_TRACEFILE"))
            and not any(key.startswith(("GGML_VK_", "GGML_VULKAN_")) for key in os.environ),
            "GPU diagnostic/backend flags must be absent, including empty values")


def head(compare, deadline):
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                                  timeout=min(5, compare.check_deadline(deadline))).strip()


def configuration(compare, path, source_path, deadline):
    config_pin, contents = compare.inspect_file(path, 64 * 1024, deadline, keep=True)
    config = compare.decode_json(contents)
    candidate = config.get("candidate", {})
    require(config.get("status") == "ready" and re.fullmatch(r"[0-9a-f]{40}", config.get("repository_head", ""))
            and config.get("baseline") == {"path": str(BASELINE), "sha256": BASELINE_SHA}
            and candidate.get("path") == str(CANDIDATE)
            and isinstance(candidate.get("sha256"), str) and compare.HEX.fullmatch(candidate["sha256"])
            and config.get("model_sha256") == compare.MODEL_SHA
            and config.get("proof_sha256") == PROOF_SHA
            and config.get("overlay_sha256") == OVERLAY_SHA,
            "Exact quality configuration is unarmed or differs")
    sources_pin, sources = compare.source_specs(source_path, deadline)
    require(sources_pin["sha256"] == config.get("source_pins_sha256"), "Source-pin configuration differs")
    for path_, digest in ((OVERLAY, OVERLAY_SHA), (HEADER, HEADER_SHA), (COMPARE, COMPARE_SHA)):
        require(sources.get(str(path_)) == digest, "Required exact source pin differs: " + str(path_))
    build = config.get("build_verification", {})
    require(isinstance(build.get("path"), str) and Path(build["path"]).is_absolute()
            and isinstance(build.get("sha256"), str) and compare.HEX.fullmatch(build["sha256"]),
            "Exact build verification pin required")
    build_path = Path(build["path"])
    _, contents = compare.inspect_file(build_path, 2 * 1024 * 1024, deadline,
                                       expected=build["sha256"], keep=True)
    proof = compare.decode_json(contents)
    require(proof.get("status") == "built" and proof.get("runtime_sha256") == candidate["sha256"]
            and proof.get("overlay_sha256") == OVERLAY_SHA
            and proof.get("phase_header_sha256") == HEADER_SHA
            and proof.get("generated_pipeline_sha256") == sources[str(PIPELINE)]
            and proof.get("generated_vulkan_sha256") == sources[str(VULKAN)]
            and proof.get("full_f32_scalar") is True and proof.get("graph_order") is False,
            "Build proof does not bind this full-F32 scalar candidate")
    specs = {"configuration": (path, config_pin["sha256"], 64 * 1024),
             "source_configuration": (source_path, sources_pin["sha256"], 64 * 1024),
             "baseline": (BASELINE, BASELINE_SHA, 256 * 1024 * 1024),
             "candidate": (CANDIDATE, candidate["sha256"], 256 * 1024 * 1024),
             "model": (compare.MODEL, compare.MODEL_SHA, 2 * 1024 * 1024 * 1024),
             "proof": (PROOF, PROOF_SHA, 2 * 1024 * 1024),
             "build_verification": (build_path, build["sha256"], 2 * 1024 * 1024)}
    for source, digest in sources.items():
        specs["source:" + source] = (Path(source), digest, 32 * 1024 * 1024)
    for label, digest in compare.TRACKS:
        specs["asset:" + label] = (compare.ASSETS / f"{label}-30s-23p4s.wav", digest, compare.ASSET_BYTES)
    return config, specs


def validate_spool(path):
    require(path.is_absolute() and path.parent == SPOOL_PARENT and path.name.startswith("flash-quality-"),
            "Use the launcher's private workspace spool")
    for directory in (SPOOL_PARENT, path, path / "baseline", path / "native"):
        info = directory.lstat()
        require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
                and stat.S_IMODE(info.st_mode) == 0o700, "Spool must be an owned mode0700 directory")
    require(not any((path / "baseline").iterdir()) and not any((path / "native").iterdir()),
            "Private spool subdirectories must start empty")
    require(os.environ.get("TMPDIR") == str(path / "native"), "Child TMPDIR must be the private native spool")


def channel_metrics(reference, candidate, np):
    count = reference.size
    require(count > 0, "Do not calculate empty-region metrics")
    ref = reference.astype(np.float64)
    delta = candidate.astype(np.float64) - ref
    ref_energy = float(np.sum(ref * ref, dtype=np.float64))
    error_energy = float(np.sum(delta * delta, dtype=np.float64))
    exact = error_energy == 0
    if ref_energy == 0:
        relative, snr = (0.0 if exact else None), None
        status = "zero_reference_exact" if exact else "zero_reference_nonzero_error"
    elif exact:
        relative, snr, status = 0.0, None, "exact_nonzero_reference"
    else:
        relative = math.sqrt(error_energy / ref_energy)
        snr = 10 * (math.log10(ref_energy) - math.log10(error_energy))
        status = "finite_nonzero_reference"
    return {"samples": count, "max_absolute_error": float(np.max(np.abs(delta))),
            "rmse": math.sqrt(error_energy / count), "reference_rms": math.sqrt(ref_energy / count),
            "relative_l2": relative, "snr_dB": snr, "reference_status": status}


def numerical_difference(reference, candidate, valid_frames, np):
    regions = {"whole": (0, 343980), "valid": (0, valid_frames), "padding": (valid_frames, 343980)}
    result = {}
    for region, (begin, end) in regions.items():
        if begin == end:
            result[region] = {"frames": 0, "metrics": None, "status": "not_applicable"}
            continue
        result[region] = {"frames": end - begin, "metrics": {
            stem: {channel: channel_metrics(reference[index, begin:end, ch], candidate[index, begin:end, ch], np)
                   for ch, channel in enumerate(("left", "right"))}
            for index, stem in enumerate(("drums", "bass", "other", "vocals"))}}
    return result


def flash_logs(record, compare, deadline):
    stderr = record["stderr"]
    require(not stderr["truncated"] and stderr["original_bytes"] == stderr["bytes"], "Complete Flash stderr required")
    pin, contents = compare.inspect_file(Path(stderr["path"]), 128 * 1024, deadline,
                                         expected=stderr["sha256"], keep=True)
    require(b"conflux_graph_order" not in contents, "Graph ordering must be absent")
    lines = [line for line in contents.decode().splitlines() if line.startswith("conflux_flash_")]
    signatures, indices, pipelines, graphs = [0, 0, 0, 0], set(), [], []
    node_pattern = r"conflux_flash_f32_node index=(\d+) q=64,(1344|2688),8,1 k=64,(1344|2688),8,1 v=64,(1344|2688),8,1 out=64,8,(1344|2688),1 scale=0.125 precision=f32 supported=1"
    pipeline_pattern = r"conflux_flash_f32_pipeline shader=flash_attn_f32_f16_fp32 path=scalar intermediate_bytes=4 accumulation_bytes=4 hsk=64 hsv=64 Br=(\d+) Bc=(\d+) workgroup=(\d+) subgroup=(\d+) k=f32 v=f32"
    graph_pattern = r"conflux_flash_f32_graph nodes=(\d+) flash_nodes=10 self1344=3 self2688=3 cross2688to1344=2 cross1344to2688=2"
    for line in lines:
        node = re.fullmatch(node_pattern, line)
        pipeline = re.fullmatch(pipeline_pattern, line)
        graph = re.fullmatch(graph_pattern, line)
        if node:
            index, q, k, v, out = map(int, node.groups())
            require(index not in indices and k == v and q == out, "Invalid Flash node signature")
            indices.add(index)
            signatures[(0 if q == 1344 else 1) if q == k else (2 if q == 2688 else 3)] += 1
        elif pipeline:
            br, bc, workgroup, subgroup = map(int, pipeline.groups())
            require(min(br, bc, workgroup) > 0 and (subgroup == 0 or subgroup & (subgroup - 1) == 0),
                    "Invalid full-F32 scalar pipeline parameters")
            pipelines.append({"Br": br, "Bc": bc, "workgroup": workgroup, "subgroup": subgroup})
        elif graph:
            graphs.append(int(graph[1]))
        else:
            raise ValueError("Unexpected Flash evidence line: " + line)
    require(len(indices) == 10 and signatures == [3, 3, 2, 2] and len(graphs) == 1 and graphs[0] > 0
            and 1 <= len(pipelines) <= 16, "Incomplete ten-node/full-F32 scalar evidence")
    return {"stderr_pin": pin, "node_indices": sorted(indices), "signature_counts": signatures,
            "graph_nodes": graphs[0], "pipelines": pipelines,
            "scope": "Exact-build runtime selection/geometry evidence; not a musical-quality verdict."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    for name in ("output", "spool", "expected-pins", "source-pins"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--deadline-monotonic", type=float)
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_run", "native_started": False, "host_queried": False,
              "requests_per_runtime": 16, "baseline_spool_bytes": BASELINE_BYTES,
              "request_seconds": REQUEST_SECONDS, "total_seconds": TOTAL_SECONDS,
              "cleanup_reserve_seconds": CLEANUP_RESERVE, "scope": SCOPE}, indent=2))
        return 0
    require(all(p is not None and p.is_absolute() for p in
            (args.output, args.spool, args.expected_pins, args.source_pins)), "Explicit absolute paths required")
    began = time.monotonic()
    require(args.deadline_monotonic is not None and math.isfinite(args.deadline_monotonic)
            and began < args.deadline_monotonic <= began + TOTAL_SECONDS, "Launcher deadline required")
    deadline, work_deadline = args.deadline_monotonic, args.deadline_monotonic - CLEANUP_RESERVE
    validate_spool(args.spool)
    args.output.mkdir(mode=0o700)
    compare = helper()
    reject_environment()
    report = {"schema": 1, "status": "checking", "scope": SCOPE, "started_at_utc": compare.utc(),
              "native_started": False, "runtimes": [], "completed_requests": 0, "errors": [],
              "pins_before": {}, "pins_after": {}, "spool": str(args.spool),
              "musical_quality": "not_assessed", "latency_qualification": "not_attempted",
              "metric_domain": "Native normalized stem PCM, before denormalization/shared listening gain/OLA.",
              "baseline_spool_expected_bytes": BASELINE_BYTES, "baseline_spool_removed": False}
    specs, owner, constructing, interrupted_signal = {}, None, False, None
    def checkpoint():
        report["elapsed_seconds"] = time.monotonic() - began
        compare.checkpoint(args.output, report)
    def interrupted(signum, _frame):
        nonlocal interrupted_signal
        interrupted_signal = signum
        report["interruption_signal"] = signum
        if not constructing:
            raise KeyboardInterrupt
    handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        config, specs = configuration(compare, args.expected_pins, args.source_pins, work_deadline)
        require(head(compare, work_deadline) == config["repository_head"], "Source HEAD differs")
        report["pins_before"] = compare.capture_pins(specs, work_deadline)
        _, contents = compare.inspect_file(PROOF, 2 * 1024 * 1024, work_deadline, expected=PROOF_SHA, keep=True)
        proof = compare.decode_json(contents)
        require(proof.get("status") == "passed" and proof.get("exact_pcm_match") is True
                and proof.get("input_sha_match") is True and proof.get("phase_fields_valid") is True
                and proof.get("pins_unchanged") is True and proof.get("cleanup_complete") is True
                and proof.get("completed_cases_per_runtime") == {"baseline": 16, "diagnostic": 16}
                and proof["pins_before"]["diagnostic"]["sha256"] == BASELINE_SHA
                and proof["pins_before"]["model"]["sha256"] == compare.MODEL_SHA,
                "Immutable baseline proof is incomplete or differs")
        np, NativeRuntime, cases = compare.prepare_cases(compare.load_track_plan(work_deadline), work_deadline)
        immutable = {row["id"]: row for row in proof["runtimes"][1]["requests"]}
        for case in cases:
            require(all(case[k] == immutable[case["id"]][k] for k in
                        ("input_sha256", "normalization", "shape", "dtype", "valid_frames")), "Input differs from immutable proof")
        report["cases"] = [{k: v for k, v in case.items() if k != "samples"} for case in cases]
        for label, executable in (("baseline", BASELINE), ("flash", CANDIDATE)):
            require(compare.capture_pins(specs, work_deadline) == report["pins_before"], "Pins changed before child")
            record = {"label": label, "executable": str(executable), "requests": [], "cleanup": {"complete": False}}
            report["runtimes"].append(record)
            owner = NativeRuntime.__new__(NativeRuntime)
            constructing = True
            startup = time.monotonic()
            try:
                try:
                    owner.__init__(executable, compare.MODEL, "vulkan", 2,
                                   min(REQUEST_SECONDS, compare.check_deadline(work_deadline)), passive_openmp=True)
                finally:
                    constructing = False
                if interrupted_signal is not None:
                    raise KeyboardInterrupt
                report["native_started"] = True
                record["pid"] = owner.process.pid
                ready = owner.start()
                record["startup_observed_ms"] = (time.monotonic() - startup) * 1000
                record["ready"] = ready
                require(ready.get("backend") == "vulkan" and ready.get("threads") == 2
                        and ready.get("revision") == compare.REVISION and ready.get("model") == str(compare.MODEL)
                        and ready.get("pid") == owner.process.pid and ready.get("weight_storage") == "native"
                        and ready.get("fence_wait_mode") == "blocking"
                        and ready.get("process_environment") == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"},
                        "Ready contract differs")
                compare.validate_number(ready, "load_ms")
                checkpoint()
                for index, case in enumerate(cases):
                    owner.timeout = min(REQUEST_SECONDS, compare.check_deadline(work_deadline))
                    row = {k: v for k, v in case.items() if k != "samples"}
                    row.update(request_id=index + 1, completed=False)
                    record["requests"].append(row)
                    produced, metrics = owner.infer(case["samples"], index + 1)
                    require(produced.shape == (4, 343980, 2) and produced.dtype.str == "<f4"
                            and produced.flags.c_contiguous and np.isfinite(produced).all(), "Output geometry/finite contract differs")
                    for name in compare.COMMON_TIMINGS + compare.PHASE_TIMINGS:
                        compare.validate_number(metrics, name)
                    for name in compare.SWITCH_COUNTS:
                        compare.validate_number(metrics, name, integral=True)
                    pin, _ = compare.inspect_file(owner.root / "input.f32", 343980 * 2 * 4, work_deadline,
                                                  expected=case["input_sha256"])
                    require(pin["bytes"] == 343980 * 2 * 4
                            and hashlib.sha256(memoryview(case["samples"])).hexdigest() == case["input_sha256"], "Input identity changed")
                    digest = hashlib.sha256(memoryview(produced)).hexdigest()
                    row.update(native_metrics=metrics, output_sha256=digest, output_bytes=produced.nbytes,
                               output_shape=list(produced.shape), output_dtype="<f4", finite=True, input_sha_match=True)
                    baseline_path = args.spool / "baseline" / f"{index:02}.f32"
                    expected_hash = immutable[case["id"]]["output_sha256"]
                    if label == "baseline":
                        require(digest == expected_hash, "Fresh baseline differs from immutable PCM proof")
                        with baseline_path.open("xb") as stream:
                            produced.tofile(stream)
                        saved, _ = compare.inspect_file(baseline_path, OUTPUT_BYTES, work_deadline, expected=expected_hash)
                        require(saved["bytes"] == OUTPUT_BYTES, "Baseline spool size differs")
                        row["matches_immutable_proof"] = True
                    else:
                        saved, contents = compare.inspect_file(baseline_path, OUTPUT_BYTES, work_deadline,
                                                               expected=expected_hash, keep=True)
                        require(saved["bytes"] == OUTPUT_BYTES, "Baseline spool size differs")
                        reference = np.frombuffer(contents, dtype="<f4").reshape(4, 343980, 2)
                        require(np.isfinite(reference).all(), "Baseline spool contains nonfinite PCM")
                        row["exact_pcm_match"] = digest == expected_hash
                        row["baseline_output_sha256"] = expected_hash
                        row["difference"] = numerical_difference(reference, produced, case["valid_frames"], np)
                        del reference, contents
                        baseline_path.unlink()
                    del produced
                    for name in ("input.f32", "output.f32"):
                        (owner.root / name).unlink(missing_ok=True)
                    row["completed"] = True
                    report["completed_requests"] += 1
                    checkpoint()
            finally:
                ignored = {sig: signal.signal(sig, signal.SIG_IGN) for sig in handlers}
                process = getattr(owner, "process", None)
                report["native_started"] = report["native_started"] or process is not None
                try:
                    if hasattr(owner, "stderr") and not owner.stderr.closed:
                        record["stderr"] = compare.save_stderr(owner, args.output, label)
                finally:
                    try:
                        if hasattr(owner, "_cleanup"):
                            NativeRuntime.close(owner)
                    finally:
                        root = getattr(owner, "root", None)
                        record["cleanup"] = {"complete": (process is None or process.poll() is not None)
                                             and (root is None or not root.exists()),
                                             "reaped": process is None or process.poll() is not None,
                                             "temporary_directory_removed": root is None or not root.exists()}
                        for sig, handler in ignored.items():
                            signal.signal(sig, handler)
                        checkpoint()
            require(record["cleanup"]["complete"] and len(record["requests"]) == 16
                    and all(row["completed"] for row in record["requests"]), "Incomplete runtime or cleanup")
            require(record["stderr"]["truncated"] is False, "Complete runtime stderr required")
            require(b"conflux_graph_order" not in Path(record["stderr"]["path"]).read_bytes(), "Graph order must be absent")
            if label == "flash":
                report["flash_evidence"] = flash_logs(record, compare, work_deadline)
            else:
                report["baseline_spool_peak_bytes"] = sum((args.spool / "baseline" / f"{index:02}.f32").stat().st_size
                                                         for index in range(16))
                require(report["baseline_spool_peak_bytes"] == BASELINE_BYTES, "Baseline spool bound differs")
            owner = None
        report["exact_pcm_match"] = all(r["exact_pcm_match"] for r in report["runtimes"][1]["requests"])
        report["input_sha_match"] = all(r["input_sha_match"] for runtime in report["runtimes"] for r in runtime["requests"])
        report["all_outputs_finite"] = all(r["finite"] for runtime in report["runtimes"] for r in runtime["requests"])
        report["phase_fields_valid"] = True  # Every completed request was validated above.
        require(report["completed_requests"] == 32 and report["input_sha_match"] and report["all_outputs_finite"],
                "Incomplete 32-request comparison")
        report["status"] = "comparison_complete"
    except KeyboardInterrupt:
        report["status"] = "cancelled"
    except Exception as error:
        report["status"] = "incomplete"
        report["errors"].append({"type": type(error).__name__, "message": str(error)[:8192]})
    finally:
        for sig in handlers:
            signal.signal(sig, signal.SIG_IGN)
        try:
            for index in range(16):
                (args.spool / "baseline" / f"{index:02}.f32").unlink(missing_ok=True)
            report["baseline_spool_removed"] = not any((args.spool / "baseline").iterdir())
            report["cleanup_complete"] = report["baseline_spool_removed"] and all(r["cleanup"]["complete"] for r in report["runtimes"])
        except Exception as error:
            report["errors"].append({"phase": "spool_cleanup", "message": str(error)})
            report["cleanup_complete"] = False
        try:
            if specs:
                report["pins_after"] = compare.capture_pins(specs, deadline, verify=False)
                report["pins_unchanged"] = bool(report["pins_before"]) and report["pins_before"] == report["pins_after"]
                require(report["pins_unchanged"] and head(compare, deadline) == config["repository_head"], "After-pins/HEAD changed")
            compare.check_deadline(deadline)
        except Exception as error:
            report["errors"].append({"phase": "after_pins", "message": str(error)})
        if report["status"] == "comparison_complete" and (report["errors"] or not report["cleanup_complete"]):
            report["status"] = "incomplete"
        report["finished_at_utc"] = compare.utc()
        checkpoint()
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    print(json.dumps({"status": report["status"], "completed_requests": report["completed_requests"],
                      "result": str(args.output / "result.json")}, allow_nan=False))
    return 0 if report["status"] == "comparison_complete" else 2 if report["status"] == "cancelled" else 1


if __name__ == "__main__":
    raise SystemExit(main())

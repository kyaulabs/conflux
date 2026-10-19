#!/usr/bin/env python3
"""Numerical reproducibility of the tracked opt-in Flash build; unarmed by default."""
import argparse
import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import sys
import tempfile
import time

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
BUILD = BASE / "flash-promoted-native-build"
RUNTIME = BUILD / "conflux-stem-runtime"
CLOSED = BASE / "flash-quality-laptop-01/comparison/result.json"
CLOSED_SHA = "917104a1cc8ca2522eef38bae6d620c972fdb2be826876b2dee88e67dd7a497a"
OLD_FLASH_SHA = "6f311e461e7af5f12ff38f90ddb8720179c2c3dce23d593437d2b34524dffd9d"
COMPARE = BASE / "compare_runtime.py"
COMPARE_SHA = "5022aeed8eaf932adead05404f80b94c7d992f4b6f68dc81d05edb6126077e5b"
QUALITY = BASE / "flash_quality.py"
QUALITY_SHA = "cec1a694a32da562be9ed79d35cfda7e2ee1943b9b7dfd10daf1ed2259fc7495"
VERIFIER = BASE / "verify_promoted_flash_build.py"
VERIFIER_SHA = "89ca78650f2fd61c229473ced20f804eec826c528952290c53c284fe2a79df44"
MODEL_SHA = "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"
LOCAL = {
    "tools/runtime/CMakeLists.txt": "d5fec7cbe05fa85784a1f1d68271e06294aac1de0f8e454947dea1d9a3adcf83",
    "tools/runtime/FlashAttentionF32.cmake": "55ebc619f561ac1042489cdc02c1fa14db6c8e0b524e19f85080783abb865600",
    "tools/runtime/GraphPhaseTiming.cmake": "91891a8a2dc01be4cb02a835d0c83f1b8b41a98da9059a36021c4a3b1e38d524",
    "tools/runtime/BlockingVulkanWait.cmake": "4ae34a1fee27c3fec3b5a4b9884a0c86fe7395acabf9d7c2006345e56384ddb3",
    "tools/runtime/htdemucs_segment.cpp": "a00c5c84bf57adeea7aa21eea992da4ee50d219fbc6994180de4dbf9fd03d7d9",
    "tools/stems/worker.py": "5327d8d9c8ff531059811077669eff1b4c0f4cb76cd00b94ed84708f9407206b",
    "benchmarks-local/music-profile-2026-10-17/interleaved_replay.py": "72d202f04fd787ab8f0871854c8c6feeb8dc6aa4f00c2cbb84359389779441ee",
    "docs/benchmarks/data/user-tracks-2026-10-17/series/manifest.json": "4a0a50eb0adcdd8649189f9f6e5850c54ead9bf01b58c9f0e4bcae424414af53",
}
GENERATED = {
    "conflux-flash-f32/pipeline.cpp": "de537c7b01fb1041d9195f4a93721cbec4aad9537efb1bcd4bf3b3bcc4a56e0c",
    "conflux-flash-f32/ggml-vulkan.cpp": "e36ece6d9bd1219085c77e58e6f624da19c349fc1dd214469367d8824501f996",
    "conflux-graph-phase-timing/include/engine/models/demucs/pipeline.h": "7f48bbd4964ebcce721f7544a84303c568b460bf21c24090899ccc8b983833cd",
}
SCOPE = ("Sixteen original normalized windows through one promoted Vulkan/two-thread/blocking/passive "
         "native child, compared exactly to closed Flash B PCM. All valid and padded frames retained. "
         "No timing aggregates, p99, speedup, new musical verdict, audio/UI load, host-policy mutation "
         "or sustained qualification. Codex may remain open. Handled signals unwind owned resources; "
         "forced termination still requires caller cleanup of the reported private spool/group.")


def require(value, message):
    if not value:
        raise ValueError(message)


def load(path, digest, name):
    raw = path.read_bytes()
    require(len(raw) <= 128 * 1024 and hashlib.sha256(raw).hexdigest() == digest, "Frozen helper differs: " + str(path))
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, raw.decode()


def request_loop(compare, source):
    tree = ast.parse(source)
    node = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "run_runtime")
    text = "\n".join(source.splitlines()[node.lineno - 1:node.end_lineno]) + "\n"
    require(text.count("output_matches_baseline") == 3 and text.count("PCM SHA256 differs for") == 1,
            "Frozen request-loop anchors changed")
    text = text.replace("output_matches_baseline", "output_matches_closed_B")
    text = text.replace("PCM SHA256 differs for", "Closed Flash B PCM SHA256 differs for")
    anchor = '                row["output_shape"], row["output_dtype"] = list(produced.shape), "<f4"'
    require(text.count(anchor) == 1, "Frozen finite-output observation anchor changed")
    text = text.replace(anchor, '                row["finite"] = True  # Existing np.isfinite guard passed.\n' + anchor)
    exec(compile(text, str(COMPARE) + "[own-B-labels]", "exec"), compare.__dict__)
    return hashlib.sha256(text.encode()).hexdigest()


def configuration(compare, config_path, config, deadline):
    require(config.get("schema") == 1 and config.get("status") == "ready"
            and re.fullmatch(r"[0-9a-f]{40}", config.get("repository_head", ""))
            and config.get("model_sha256") == MODEL_SHA and config.get("closed_B_proof_sha256") == CLOSED_SHA,
            "Reproduction configuration is unarmed or differs")
    runtime = config.get("runtime", {})
    receipt = config.get("build_receipt", {})
    require(runtime.get("path") == str(RUNTIME) and compare.HEX.fullmatch(runtime.get("sha256", ""))
            and compare.HEX.fullmatch(config.get("driver_sha256", ""))
            and Path(receipt.get("path", "")).is_absolute() and compare.HEX.fullmatch(receipt.get("sha256", "")),
            "Exact driver/runtime/built-receipt pins required")
    build_path = Path(receipt["path"])
    _, data = compare.inspect_file(build_path, 8 * 1024 * 1024, deadline, expected=receipt["sha256"], keep=True)
    build = compare.decode_json(data)
    inspection = build.get("inspection", {})
    require(build.get("status") == "built_metadata_verified_native_execution_pending" and build.get("stage") == "built"
            and build.get("before_after_inputs_unchanged") is True and build.get("verifier", {}).get("sha256") == VERIFIER_SHA
            and build.get("runtime", {}).get("path") == str(RUNTIME)
            and build["runtime"]["sha256"] == runtime["sha256"]
            and inspection.get("build_directory") == str(BUILD)
            and inspection.get("full_f32_scalar_compilation_source") is True
            and inspection.get("graph_order_overlay") is False
            and inspection.get("model", {}).get("sha256") == MODEL_SHA
            and inspection.get("configured_options", {}).get("CONFLUX_STEM_FLASH_F32") == "ON"
            and inspection["configured_options"].get("CONFLUX_STEM_BLOCKING_WAIT") == "ON",
            "Promoted build metadata is incomplete or differs")
    for relative, digest in LOCAL.items():
        if relative.startswith("tools/runtime/"):
            require(inspection["worktree_inputs"][relative]["sha256"] == digest
                    and inspection["worktree_inputs"][relative]["path"] == str(ROOT / relative),
                    "Promoted tracked input differs: " + relative)
    for relative, digest in GENERATED.items():
        require(inspection["generated_inputs"][relative]["sha256"] == digest
                and inspection["generated_inputs"][relative]["path"] == str(BUILD / relative),
                "Promoted generated source/header differs: " + relative)
    config_pin, _ = compare.inspect_file(config_path, 64 * 1024, deadline)
    specs = {
        "configuration": (config_path, config_pin["sha256"], 64 * 1024),
        "driver": (Path(__file__).resolve(), config["driver_sha256"], 128 * 1024),
        "request_loop_source": (COMPARE, COMPARE_SHA, 128 * 1024),
        "Flash_log_parser_source": (QUALITY, QUALITY_SHA, 128 * 1024),
        "build_verifier_source": (VERIFIER, VERIFIER_SHA, 128 * 1024),
        "build_receipt": (build_path, receipt["sha256"], 8 * 1024 * 1024),
        "runtime": (RUNTIME, runtime["sha256"], 256 * 1024 * 1024),
        "model": (compare.MODEL, MODEL_SHA, 2 * 1024 * 1024 * 1024),
        "closed_B_proof": (CLOSED, CLOSED_SHA, 8 * 1024 * 1024),
    }
    for relative, digest in LOCAL.items():
        specs["source:" + relative] = (ROOT / relative, digest, 32 * 1024 * 1024)
    # Recheck the actual promoted source/configuration/tool records already
    # established by the configured/built verifier, not the old overlay paths.
    for group in ("worktree_inputs", "upstream_pinned_and_affected_sources", "generated_inputs", "config_inputs", "tools_and_header"):
        for name, record in inspection[group].items():
            specs["build_input:" + group + ":" + name] = (Path(record["path"]), record["sha256"], max(1, record["bytes"]))
    for name, digest in compare.TRACKS:
        specs["asset:" + name] = (compare.ASSETS / (name + "-30s-23p4s.wav"), digest, compare.ASSET_BYTES)
    return specs


def expected_cases(compare, deadline):
    _, data = compare.inspect_file(CLOSED, 8 * 1024 * 1024, deadline, expected=CLOSED_SHA, keep=True)
    proof = compare.decode_json(data)
    require(proof.get("status") == "comparison_complete" and proof.get("completed_requests") == 32
            and all(proof.get(key) is True for key in ("all_outputs_finite", "input_sha_match", "phase_fields_valid", "pins_unchanged", "cleanup_complete"))
            and not proof.get("errors") and [row["label"] for row in proof["runtimes"]] == ["baseline", "flash"]
            and all(proof[side]["candidate"]["sha256"] == OLD_FLASH_SHA
                    and proof[side]["model"]["sha256"] == MODEL_SHA for side in ("pins_before", "pins_after")),
            "Immutable closed Flash B proof differs")
    rows = proof["runtimes"][1]["requests"]
    identities = [f"{track}:{start}" for track, _ in compare.TRACKS for start in compare.STARTS]
    require([row["id"] for row in rows] == identities
            and proof["runtimes"][1]["cleanup"]["complete"] is True, "Closed B case coverage differs")
    for row in rows:
        require(row.get("completed") is True and row.get("finite") is True and row.get("input_sha_match") is True
                and row.get("output_shape") == [4, 343980, 2] and row.get("output_dtype") == "<f4"
                and row.get("output_bytes") == 11007360 and compare.HEX.fullmatch(row.get("output_sha256", "")),
                "Closed B output proof is incomplete")
    return {row["id"]: row for row in rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--expected-pins", type=Path, default=BASE / "flash-promoted-repro-expected-pins.json")
    parser.add_argument("--output", type=Path, default=BASE / "flash-promoted-repro-01")
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "unarmed_dry_plan", "native_started": False, "requests": 16,
            "scope": SCOPE, "expected_configuration": {"schema": 1, "status": "unarmed", "repository_head": None,
                "driver_sha256": None, "runtime": {"path": str(RUNTIME), "sha256": None},
                "build_receipt": {"path": str(BASE / "flash-promoted-built.json"), "sha256": None},
                "model_sha256": MODEL_SHA, "closed_B_proof_sha256": CLOSED_SHA}}))
        return 0
    require(args.expected_pins.is_absolute() and args.output.is_absolute() and args.output.parent == BASE,
            "Use explicit absolute config and fresh direct-child output paths")
    require(args.expected_pins.is_file() and args.expected_pins.stat().st_size <= 64 * 1024,
            "No bounded armed configuration exists")
    config = json.loads(args.expected_pins.read_text())
    require(config.get("status") == "ready", "Reproduction configuration is unarmed")
    require(not args.output.exists(), "Use a fresh output directory")
    compare, source = load(COMPARE, COMPARE_SHA, "conflux_promoted_repro_common")
    quality, _ = load(QUALITY, QUALITY_SHA, "conflux_promoted_repro_log_parser")
    loop_sha = request_loop(compare, source)
    quality.reject_environment()
    began = time.monotonic()
    deadline, work_deadline = began + 600, began + 540
    report = {"schema": 1, "status": "checking", "scope": SCOPE, "native_started": False,
        "runtimes": [], "completed_cases_per_runtime": {"diagnostic": 0}, "total_completed_requests": 0,
        "errors": [], "closed_B_proof_sha256": CLOSED_SHA, "model_sha256": MODEL_SHA,
        "tracked_module_sha256": LOCAL["tools/runtime/FlashAttentionF32.cmake"],
        "rendered_request_loop_sha256": loop_sha, "pins_before": {}, "pins_after": {},
        "cleanup_complete": False, "musical_quality": "no_new_verdict", "latency_qualification": "not_attempted"}
    args.output.mkdir(mode=0o700)
    specs, owner, constructing, spool = {}, None, False, None
    previous_tmp = os.environ.get("TMPDIR")
    previous_tempdir = tempfile.tempdir
    def checkpoint():
        report["elapsed_seconds"] = time.monotonic() - began
        compare.checkpoint(args.output, report)
    def interrupted(signum, _frame):
        report["interruption_signal"] = signum
        if not constructing:
            raise KeyboardInterrupt
    handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        specs = configuration(compare, args.expected_pins, config, work_deadline)
        require(quality.head(compare, work_deadline) == config["repository_head"], "Repository HEAD differs")
        report["source_head"] = config["repository_head"]
        report["runtime_sha256"] = config["runtime"]["sha256"]
        report["build_receipt_sha256"] = config["build_receipt"]["sha256"]
        report["pins_before"] = compare.capture_pins(specs, work_deadline)
        expected = expected_cases(compare, work_deadline)
        np, NativeRuntime, cases = compare.prepare_cases(compare.load_track_plan(work_deadline), work_deadline)
        for case in cases:
            require(all(case[key] == expected[case["id"]][key] for key in
                        ("input_sha256", "normalization", "shape", "dtype", "valid_frames")),
                    "Fresh normalized case differs from closed B proof: " + case["id"])
        spool = tempfile.TemporaryDirectory(prefix="promoted-flash-repro-", dir=BASE)
        report["private_spool"] = spool.name
        os.environ["TMPDIR"] = spool.name
        tempfile.tempdir = None
        checkpoint()
        class OwnedNative(NativeRuntime):
            def __new__(cls, *_args, **_kwargs):
                nonlocal owner, constructing
                constructing = True
                owner = object.__new__(cls)
                return owner
            def __init__(self, *a, **kw):
                nonlocal constructing
                try:
                    super().__init__(*a, **kw)
                finally:
                    constructing = False
                if "interruption_signal" in report:
                    self.close()
                    raise KeyboardInterrupt
        compare.run_runtime("diagnostic", RUNTIME, cases,
            {key: row["output_sha256"] for key, row in expected.items()}, np, OwnedNative,
            argparse.Namespace(request_timeout=30), args.output, report, work_deadline)
        report["flash_evidence"] = quality.flash_logs(report["runtimes"][0], compare, work_deadline)
        require(quality.head(compare, work_deadline) == config["repository_head"], "Repository HEAD changed")
    except KeyboardInterrupt:
        report["status"] = "interrupted"
    except Exception as error:
        report["status"] = "error"
        report["errors"].append({"phase": "reproduction", "type": type(error).__name__, "message": str(error)})
    finally:
        for sig in handlers:
            signal.signal(sig, signal.SIG_IGN)
        try:
            if owner is not None:
                try:
                    owner.close()
                except Exception as error:
                    report["errors"].append({"phase": "native_cleanup", "message": str(error)})
            if previous_tmp is None:
                os.environ.pop("TMPDIR", None)
            else:
                os.environ["TMPDIR"] = previous_tmp
            tempfile.tempdir = previous_tempdir
            if spool is not None:
                try:
                    spool.cleanup()
                except Exception as error:
                    report["errors"].append({"phase": "spool_cleanup", "message": str(error)})
            report["private_spool_removed"] = spool is None or not Path(spool.name).exists()
            process = getattr(owner, "process", None)
            report["cleanup_complete"] = (process is None or process.poll() is not None) and report["private_spool_removed"]
            if specs:
                try:
                    report["pins_after"] = compare.capture_pins(specs, deadline, verify=False)
                    report["pins_unchanged"] = report["pins_before"] == report["pins_after"]
                    require(report["pins_unchanged"], "Pins changed")
                except Exception as error:
                    report["errors"].append({"phase": "after_pins", "message": str(error)})
            requests = report["runtimes"][0]["requests"] if report["runtimes"] else []
            report["completed_requests"] = report["total_completed_requests"]
            report["all_outputs_match_closed_B"] = len(requests) == 16 and all(row.get("output_matches_closed_B") is True for row in requests)
            report["input_sha_match"] = len(requests) == 16 and all(row.get("input_sha_match") is True for row in requests)
            report["phase_fields_valid"] = len(requests) == 16 and all(row.get("phase_fields_valid") is True for row in requests)
            if report["status"] == "checking" and not report["errors"] and report["completed_requests"] == 16 and all(
                    report.get(key) is True for key in ("all_outputs_match_closed_B", "input_sha_match", "phase_fields_valid", "pins_unchanged", "cleanup_complete")) and "flash_evidence" in report:
                report["status"] = "reproduction_complete"
            elif report["status"] == "checking":
                report["status"] = "error"
            checkpoint()
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
    print(json.dumps({"status": report["status"], "completed_requests": report["completed_requests"],
                      "result": str(args.output / "result.json"), "scope": "Numerical reproducibility only"}))
    return 0 if report["status"] == "reproduction_complete" else 130 if report["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

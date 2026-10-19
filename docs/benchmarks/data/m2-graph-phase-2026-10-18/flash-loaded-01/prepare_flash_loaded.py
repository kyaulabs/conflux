#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Prepare exact loaded-diagnostic pins only; no host, Git, tests or native execution."""
import argparse
import ast
from datetime import datetime, timezone
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import signal
import sys
import time
import wave

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
WRAPPER = BASE / "flash_loaded_launch.py"
WRAPPER_SHA = "883d537f8f2fe91dc2e5fc5a735ca20067a007cbe8e308e9f1673825bf39e684"
SEED = BASE / "flash-cost-source-pins.json"
SEED_SHA = "d816e77b49ff5888d7c263bd399345424a2d95704ba8688f628e348acd51c534"
TEMPLATE = BASE / "flash-loaded-expected-pins.template.json"
TEMPLATE_SHA = "75f70ddfe4834c9a4bf64625c89ff6dc2e517f7faf0725eb88296466deb1d819"
# Only these unused offline validators may leave the inherited59-entry map.
REMOVABLE = tuple(BASE / name for name in (
    "flash_quality_launcher_review_mocks.py", "flash_quality_offline_check.py", "verify_flash_f32_overlay.py"))


def require(value, message):
    if not value:
        raise ValueError(message)


def load_wrapper():
    raw = WRAPPER.read_bytes()
    require(len(raw) <= 128 * 1024 and hashlib.sha256(raw).hexdigest() == WRAPPER_SHA,
            "Reviewed loaded wrapper changed")
    spec = importlib.util.spec_from_file_location("conflux_loaded_preparation", WRAPPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true", help="Write fresh source/expected pins after file validation")
    parser.add_argument("--head", help="Exact final signed HEAD supplied and independently verified by root")
    parser.add_argument("--render-dir", type=Path)
    parser.add_argument("--check-result", type=Path, action="append", default=[],
                        help="Root-executed offline result JSON; repeat to retain every check receipt")
    parser.add_argument("--receipt", type=Path, default=BASE / "flash-loaded-preparation-verification.json")
    args = parser.parse_args()
    if not args.prepare:
        print(json.dumps({"status": "dry_plan", "native_executed": False, "host_queried": False,
              "git_executed": False, "writes": False, "human_readiness_created": False,
              "source_entry_maximum": 64,
              "scope": "Root must supply --prepare, final --head, --render-dir and --check-result. Only file bytes, JSON, source syntax and existing closed proof metadata are validated; no workloads or physical-readiness record."}, indent=2))
        return 0
    require(isinstance(args.head, str) and re.fullmatch(r"[0-9a-f]{40}", args.head), "Supply exact final --head")
    require(args.render_dir is not None and args.render_dir.is_absolute() and args.check_result
            and all(path.is_absolute() for path in (*args.check_result, args.receipt)),
            "Supply absolute render/result/receipt paths")
    require(args.receipt.parent == BASE and args.render_dir.parent == BASE, "Keep preparation artifacts inside this experiment")
    source_path, expected_path = BASE / "flash-loaded-source-pins.json", BASE / "flash-loaded-expected-pins.json"
    outputs = (source_path, expected_path, args.receipt)
    require(len(set(outputs)) == 3 and all(not path.exists() and not path.is_symlink()
            and not path.with_suffix(path.suffix + ".tmp").exists()
            and not path.with_suffix(path.suffix + ".tmp").is_symlink() for path in outputs),
            "Preparation outputs must be fresh, including temporary files")
    began, deadline = time.monotonic(), time.monotonic() + 120
    module = load_wrapper()
    c = module.cost()
    compare = c.helpers()[0]
    compare.REQUIRED_SOURCES = tuple(dict.fromkeys(compare.REQUIRED_SOURCES + (
        WRAPPER, module.FROZEN, *(file for file, _, _, _ in module.NESTED.values()),
        *(original for _, _, original, _ in module.NESTED.values()))))
    verified = {}

    def inspect(label, path, maximum, digest=None, *, keep=False):
        record, raw = compare.inspect_file(path, maximum, deadline, expected=digest, keep=keep)
        verified[label] = record
        return record, raw

    _, raw = inspect("seed_configuration", SEED, 64 * 1024, SEED_SHA, keep=True)
    mapping = compare.decode_json(raw)
    require(isinstance(mapping, dict) and len(mapping) == 59, "Expected retained59-entry source map")
    for file, digest in mapping.items():
        require(isinstance(file, str) and Path(file).is_absolute()
                and isinstance(digest, str) and compare.HEX.fullmatch(digest), "Invalid inherited source entry")
        inspect("inherited_source:" + file, Path(file), 32 * 1024 * 1024, digest)

    # Execute definitions only, never main(): obtain the exact rendered static
    # producer/helper table without invoking policy(), head(), Popen or readiness.
    rendered = module.source()
    namespace = {"__file__": str(WRAPPER), "__name__": "conflux_preparation_definitions_only"}
    exec(compile(rendered, "reviewed-loaded-launcher-definitions-only", "exec"), namespace)
    render_files = {
        "observed-launch.py": rendered.encode(),
        **{name + ".py": c.frozen(file, digest).encode()
           for name, (file, digest, _, _) in module.NESTED.items()},
    }
    for name, expected in render_files.items():
        path = args.render_dir / name
        pin, raw = inspect("rendered:" + name, path, 128 * 1024,
                           hashlib.sha256(expected).hexdigest(), keep=True)
        require(raw == expected, "Rendered executed bytes differ: " + name)
        ast.parse(raw.decode(), filename=str(path))
    provenance = module.provenance()
    pin, raw = inspect("rendered_provenance", args.render_dir / "provenance.json", 64 * 1024, keep=True)
    require(compare.decode_json(raw) == provenance, "Rendered provenance differs from reviewed current adapter")

    for path in compare.REQUIRED_SOURCES:
        if str(path) not in mapping:
            pin, _ = inspect("required_source:" + str(path), path, 32 * 1024 * 1024)
            mapping[str(path)] = pin["sha256"]
    for path in (Path(__file__).resolve(), args.render_dir / "observed-launch.py", args.render_dir / "provenance.json"):
        pin, _ = inspect("preparation_source:" + str(path), path, 128 * 1024)
        mapping[str(path)] = pin["sha256"]
    removed = {}
    for path in REMOVABLE:
        if len(mapping) <= 64:
            break
        require(path not in compare.REQUIRED_SOURCES, "Cannot remove a required source")
        if str(path) in mapping:
            removed[str(path)] = mapping.pop(str(path))
    require(len(mapping) <= 64 and all(str(path) in mapping for path in compare.REQUIRED_SOURCES),
            "Source map cannot meet64-entry limit without removing required/non-validator evidence")
    mapping = dict(sorted(mapping.items()))
    source_bytes = (json.dumps(mapping, indent=2, allow_nan=False) + "\n").encode()
    source_sha = hashlib.sha256(source_bytes).hexdigest()

    _, raw = inspect("expected_template", TEMPLATE, 64 * 1024, TEMPLATE_SHA, keep=True)
    expected = compare.decode_json(raw)
    require(expected.get("status") == "unarmed" and set(expected["pins"]) == set(namespace["PRODUCERS"]),
            "Expected unarmed producer template differs")
    for name, (path, digest) in namespace["PRODUCERS"].items():
        require(digest is not None and expected["pins"][name] == digest, "Producer template differs: " + name)
        inspect("producer:" + name, path, 2 * 1024 * 1024 * 1024 if name == "model" else 256 * 1024 * 1024, digest)
    for name, (path, digest) in namespace["HELPERS"].items():
        inspect("transitive_helper:" + name, path, 32 * 1024 * 1024, digest)
    for name, digest in namespace["WAVS"].items():
        path = compare.ASSETS / (name + "-30s-23p4s.wav")
        _, raw = inspect("source_asset:" + name, path, compare.ASSET_BYTES, digest, keep=True)
        with wave.open(io.BytesIO(raw), "rb") as stream:
            require(stream.getparams()[:4] == (2, 2, 44100, 1031940) and stream.getcomptype() == "NONE",
                    "Prepared source geometry differs: " + name)
    # Baseline is not executed, but its exact binary is retained in this lineage.
    inspect("baseline_runtime", c.ARMS["A"][0], 256 * 1024 * 1024, c.ARMS["A"][1])

    check_reports, child_check_seen, wrapper_check_seen = [], False, False
    for index, path in enumerate(args.check_result):
        pin, raw = inspect("supplied_check:" + str(index), path, 2 * 1024 * 1024, keep=True)
        report = compare.decode_json(raw)
        require(report.get("status") == "passed", "Supplied check report does not report a pass: " + str(path))
        if report.get("scenario_count") == 9 and "source_sha256" in report:
            require(len(report.get("scenarios", {})) == 9
                    and all(report["source_sha256"].get(file.name) == digest
                        for file, digest, _, _ in module.NESTED.values()), "Child mock receipt binds different helper source")
            mock_path = BASE / "flash_loaded_child_review_mocks.py"
            inspect("child_mock_source", mock_path, 128 * 1024, report["mock_script_sha256"])
            child_check_seen = True
        if "wrapper_sha256" in report:
            require(report.get("check_count") == 20 and len(report.get("checks", [])) == 20
                    and report["wrapper_sha256"] == WRAPPER_SHA
                    and report.get("rendered_sha256") == provenance["rendered_launcher_sha256"],
                    "Outer mock receipt binds different wrapper/rendered source")
            inspect("wrapper_mock_source", BASE / "flash_loaded_wrapper_review_mocks.py", 128 * 1024,
                    report["mock_script_sha256"])
            wrapper_check_seen = True
        check_reports.append({"pin": pin, "reported_metadata": report,
                              "execution_scope": "Supplied root-executed receipt; this preparation script did not execute these checks."})
    require(child_check_seen and wrapper_check_seen,
            "Supply actual nine-scenario child and twenty-check outer receipts bound to these exact sources")

    expected.update(status="ready", repository_head=args.head, source_pins_sha256=source_sha,
                    scope=module.SCOPE)
    created = []
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt
    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        created.append(source_path)
        compare.atomic_json(source_path, mapping)
        source_pin, _ = inspect("final_source_configuration", source_path, 64 * 1024, source_sha)
        # Existing validator performs only source/JSON/file-hash checks. No HEAD,
        # process, readiness, model inference or human input function is called.
        module.prepare_proof_sources(source_path, expected, deadline)
        proof = module.own_pcm_proof(c.CLOSED, expected)
        compare.check_deadline(deadline)
        created.append(expected_path)
        compare.atomic_json(expected_path, expected)
        expected_pin, _ = inspect("final_expected_configuration", expected_path, 64 * 1024)
        receipt = {"schema": 1, "status": "prepared_fresh_human_readiness_pending",
            "prepared_at_utc": datetime.now(timezone.utc).isoformat(), "elapsed_seconds": time.monotonic() - began,
            "repository_head": args.head, "head_scope": "Caller-supplied final signed HEAD; root verifies Git/signature. This script never invokes Git.",
            "source_entries": len(mapping), "source_entry_maximum": 64, "removed_unused_offline_validators": removed,
            "source_configuration": source_pin, "expected_configuration": expected_pin,
            "execution_provenance": provenance, "closed_own_arm_proof": proof,
            "verified_file_records": verified, "loaded_proof_source_pins": module._context["pins_before"],
            "supplied_check_reports_not_executed_here": check_reports,
            "preparation_checks": ["retained_source_bytes", "rendered_sources_and_provenance", "required_source_coverage_and_limit",
                "all_producer_and_transitive_helper_bytes", "source_WAV_header_geometry", "exact_closed_PCM_and_F32_build_lineage"],
            "native_executed": False, "host_queried": False, "git_executed": False, "tests_executed": False,
            "audio_samples_decoded": False, "human_readiness_created": False, "human_readiness_consumed": False,
            "scope": "Arming/file preparation only. No latency, playback, physical, musical or runtime qualification claim. Actual source/proof/producer bytes were read; supplied offline check metadata is retained without re-execution.",
            "next": "Root obtains a new direct same-label AC/open-lid/Codex-minimized/visible-Harness reply, then separately writes its actual readiness record. No native/admission workload before that reply."}
        created.append(args.receipt)
        compare.atomic_json(args.receipt, receipt)
    except BaseException:
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        for path in reversed(created):
            path.unlink(missing_ok=True)
            path.with_suffix(path.suffix + ".tmp").unlink(missing_ok=True)
        raise
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    print(json.dumps({"status": receipt["status"], "source_entries": len(mapping), "receipt": str(args.receipt),
                      "native_executed": False, "human_readiness_created": False}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

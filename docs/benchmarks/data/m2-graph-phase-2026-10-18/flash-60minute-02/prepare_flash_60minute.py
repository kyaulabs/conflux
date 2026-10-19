#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Prepare promoted3600s source/proof pins; never run a workload or create human readiness."""
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
WRAPPER = BASE / "flash_60minute_launch.py"
WRAPPER_SHA = "3406ca6308e8e43924b3935a6da2e8c6d09db8c2d02b0a2b4e7e173cb18ffd14"
TEMPLATE = BASE / "flash-60minute-expected-pins.template.json"
TEMPLATE_SHA = "0709e4b4a74e58db94f9fae4396cfb44be7597188e4833da9e8486cf08f8724f"


def require(value, message):
    if not value:
        raise ValueError(message)


def wrapper():
    data = WRAPPER.read_bytes()
    require(len(data) <= 128 * 1024 and hashlib.sha256(data).hexdigest() == WRAPPER_SHA,
            "Reviewed3600s adapter differs")
    spec = importlib.util.spec_from_file_location("conflux_60minute_preparation", WRAPPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--head", help="Final signed promotion commit independently verified by root")
    parser.add_argument("--render-dir", type=Path)
    parser.add_argument("--check-result", type=Path, action="append", default=[],
                        help="Root-executed metadata checks bound to this wrapper/render; repeat if needed")
    parser.add_argument("--receipt", type=Path, default=BASE / "flash-60minute-preparation-verification.json")
    args = parser.parse_args()
    if not args.prepare:
        print(json.dumps({"status": "dry_plan", "input_contents_read": False, "writes": False,
            "native_executed": False, "host_queried": False, "git_executed": False,
            "human_readiness_created": False, "pending": ["final signed promotion commit", "closed built receipt",
            "complete16-case own-B reproduction", "root offline wrapper/render checks", "fresh actual human readiness"],
            "workload_seconds": 3600, "maximum_total_seconds": 4350, "source_entry_maximum": 64}, indent=2))
        return 0
    require(isinstance(args.head, str) and re.fullmatch(r"[0-9a-f]{40}", args.head), "Supply final signed --head")
    require(args.render_dir is not None and args.render_dir.is_absolute() and args.render_dir.parent == BASE
        and args.receipt.is_absolute() and args.receipt.parent == BASE and args.check_result
        and all(path.is_absolute() for path in args.check_result), "Supply absolute fresh render/receipt/check paths")
    source_path, expected_path = BASE / "flash-60minute-source-pins.json", BASE / "flash-60minute-expected-pins.json"
    outputs = (source_path, expected_path, args.receipt)
    require(len(set(outputs)) == 3 and all(not path.exists() and not path.is_symlink()
        and not path.with_suffix(path.suffix + ".tmp").exists()
        and not path.with_suffix(path.suffix + ".tmp").is_symlink() for path in outputs),
        "Arming outputs, including temporary files, must be fresh")
    began = time.monotonic()
    deadline = began + 180
    module = wrapper()
    c, compare = module.proof_helpers()
    verified = {}

    def inspect(label, path, maximum, digest=None, *, keep=False):
        record, raw = compare.inspect_file(path, maximum, deadline, expected=digest, keep=keep)
        verified[label] = record
        return record, raw

    _, raw = inspect("template", TEMPLATE, 64 * 1024, TEMPLATE_SHA, keep=True)
    expected = compare.decode_json(raw)
    require(expected.get("status") == "unarmed", "Use the original unarmed template")
    build_pin, raw = inspect("promoted_build", module.BUILD, 4 * 1024 * 1024, keep=True)
    build = compare.decode_json(raw)
    require(build.get("status") == "built_metadata_verified_native_execution_pending"
        and build.get("before_after_inputs_unchanged") is True, "Promoted build is pending or incomplete")
    reproduction_pin, raw = inspect("promoted_reproduction", module.REPRODUCTION, 8 * 1024 * 1024, keep=True)
    reproduction = compare.decode_json(raw)
    require(reproduction.get("status") == "reproduction_complete" and reproduction.get("source_head") == args.head,
            "Numerical reproduction/final signed source binding is pending")
    expected.update(status="ready", repository_head=args.head,
        promoted_build_sha256=build_pin["sha256"], reproduction_proof_sha256=reproduction_pin["sha256"],
        scope=module.SCOPE)
    expected["pins"]["runtime"] = build["runtime"]["sha256"]

    rendered = module.source()
    namespace = {"__file__": str(WRAPPER), "__name__": "conflux_60minute_preparation_definitions_only"}
    exec(compile(rendered, "reviewed3600s-definitions-only", "exec"), namespace)
    require(set(expected["pins"]) == set(namespace["PRODUCERS"]), "Producer template keys differ")
    for name, (path, fixed) in namespace["PRODUCERS"].items():
        require(fixed is None or expected["pins"][name] == fixed, "Fixed producer pin differs: " + name)
        inspect("producer:" + name, path, 256 * 1024 * 1024, expected["pins"][name])
    for name, (path, digest) in namespace["HELPERS"].items():
        inspect("transitive_helper:" + name, path, 32 * 1024 * 1024, digest)
    for name, digest in namespace["WAVS"].items():
        path = compare.ASSETS / (name + "-30s-23p4s.wav")
        _, raw = inspect("source_asset:" + name, path, compare.ASSET_BYTES, digest, keep=True)
        with wave.open(io.BytesIO(raw), "rb") as stream:
            require(stream.getparams()[:4] == (2, 2, 44100, 1031940) and stream.getcomptype() == "NONE",
                    "Prepared source WAV header differs")
    provenance = module.provenance()
    render_files = {"observed-launch.py": rendered.encode(),
        **{name + ".py": c.frozen(path, digest).encode()
            for name, (path, digest, _, _) in module.loaded().NESTED.items()}}
    for name, expected_bytes in render_files.items():
        _, raw = inspect("rendered:" + name, args.render_dir / name, 128 * 1024,
            hashlib.sha256(expected_bytes).hexdigest(), keep=True)
        require(raw == expected_bytes, "Rendered executed source differs: " + name)
        ast.parse(raw.decode("utf-8"), filename=name)
    _, raw = inspect("rendered_provenance", args.render_dir / "provenance.json", 64 * 1024, keep=True)
    require(compare.decode_json(raw) == provenance, "Rendered provenance differs")
    checks, current_check_seen = [], False
    for path in args.check_result:
        record, raw = inspect("offline_check:" + str(path), path, 2 * 1024 * 1024, keep=True)
        report = compare.decode_json(raw)
        require(report.get("status") == "passed", "Supplied root offline check is not passed")
        if report.get("wrapper_sha256") == WRAPPER_SHA and report.get("rendered_sha256") == provenance["rendered_launcher_sha256"]:
            current_check_seen = True
        checks.append({"file": record, "reported_metadata": report,
            "scope": "Root-supplied check receipt, not executed by this preparation helper"})
    require(current_check_seen, "Supply a passed root check bound to the current wrapper and rendered launcher")
    # A fresh map includes only actual required definitions/current sources and
    # rendered provenance. Historical maps contain obsolete runtime-CMake bytes.
    mapping = {}
    for path in dict.fromkeys((*compare.REQUIRED_SOURCES, Path(__file__).resolve(),
            args.render_dir / "observed-launch.py", args.render_dir / "provenance.json")):
        record, _ = inspect("source:" + str(path), path, 32 * 1024 * 1024)
        mapping[str(path)] = record["sha256"]
    require(len(mapping) <= 64 and all(str(path) in mapping for path in compare.REQUIRED_SOURCES),
            "Required source map exceeds the unchanged64-entry bound")
    mapping = dict(sorted(mapping.items()))
    expected["source_pins_sha256"] = hashlib.sha256((json.dumps(mapping, indent=2) + "\n").encode()).hexdigest()
    created = []
    def interrupted(_signal, _frame):
        raise KeyboardInterrupt
    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        created.append(source_path)
        compare.atomic_json(source_path, mapping)
        source_pin, _ = inspect("final_source_map", source_path, 64 * 1024, expected["source_pins_sha256"])
        # File/proof/environment validation only; no process, policy, Git or
        # physical-readiness function is called by this admission validator.
        module.prepare_proof_sources(source_path, expected, deadline)
        proof = module.own_pcm_proof(module.REPRODUCTION, expected)
        compare.check_deadline(deadline)
        created.append(expected_path)
        compare.atomic_json(expected_path, expected)
        expected_pin, _ = inspect("final_expected_map", expected_path, 64 * 1024)
        receipt = {"schema": 1, "status": "prepared_fresh_human_readiness_pending",
            "prepared_at_utc": datetime.now(timezone.utc).isoformat(), "elapsed_seconds": time.monotonic() - began,
            "repository_head": args.head, "head_scope": "Root-supplied signed promotion commit; no Git queried here",
            "source_entries": len(mapping), "source_entry_maximum": 64,
            "source_configuration": source_pin, "expected_configuration": expected_pin,
            "execution_provenance": provenance, "promoted_own_B_proof": proof,
            "verified_file_records": verified, "loaded_proof_source_pins": module._context["pins_before"],
            "supplied_root_checks_not_executed_here": checks,
            "workload_seconds": 3600, "maximum_total_seconds": 4350,
            "native_executed": False, "host_queried": False, "git_executed": False,
            "tests_executed": False, "human_readiness_created": False,
            "scope": "File/arming preparation only. No latency/playback/physical/thermal qualification claim.",
            "next": "Root obtains a fresh direct same-run AC/open-lid/minimized/visible-Harness reply, then records its actual time. No native admission/workload before that reply."}
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
    print(json.dumps({"status": receipt["status"], "receipt": str(args.receipt), "source_entries": len(mapping),
                      "native_executed": False, "human_readiness_created": False}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

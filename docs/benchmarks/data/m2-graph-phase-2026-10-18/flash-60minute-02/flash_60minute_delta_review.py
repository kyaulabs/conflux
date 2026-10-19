#!/usr/bin/env python3
"""Offline checks of the reviewed hour-specific delta; root executes explicitly."""
import argparse
import ast
import copy
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import time
from types import SimpleNamespace

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
WRAPPER = BASE / "flash_60minute_launch.py"
PREPARER = BASE / "prepare_flash_60minute.py"
WRAPPER_SHA = "3406ca6308e8e43924b3935a6da2e8c6d09db8c2d02b0a2b4e7e173cb18ffd14"
PREPARER_SHA = "4c764a302e09562a6a97cf734c56e79c73d4ac7ffa9accd7da373d47c3597943"
PARENT_SHA = "18ef046ff4a7eb5e1254af315653dd8d3200499ae3f500fd8c3597f471259c7a"
RENDER_SHA = "b0d4076224a2ee4a8d758704bcc3e40d6072fecbaa510c99baa115c3bcedc606"


def require(value, message):
    if not value:
        raise ValueError(message)


def read_pinned(path, digest):
    raw = path.read_bytes()
    require(len(raw) <= 128 * 1024 and hashlib.sha256(raw).hexdigest() == digest,
            "Reviewed source differs: " + str(path))
    return raw.decode()


def function(tree, name):
    return next(node for node in ast.walk(tree) if isinstance(node, ast.FunctionDef) and node.name == name)


def extracted(node, namespace):
    code = ast.Module(body=[node], type_ignores=[])
    exec(compile(ast.fix_missing_locations(code), "[offline-extracted-source]", "exec"), namespace)
    return namespace[node.name]


def exact_delta(parent, current):
    substitutions = [
        ("Adapted Flash loaded diagnostic, unchanged soak and independent UI/silent-audio load.",
         "Promoted Flash3600s loaded diagnostic, unchanged soak/UI/silent-audio semantics."),
        ("RUNTIME = Path('/tmp/conflux-stem-flash-f32-2026-10-18/build/conflux-stem-runtime')",
         "RUNTIME = Path(" + repr(str(BASE / "flash-promoted-native-build/conflux-stem-runtime")) + ")"),
        ('"runtime": (RUNTIME, \'6f311e461e7af5f12ff38f90ddb8720179c2c3dce23d593437d2b34524dffd9d\')',
         '"runtime": (RUNTIME, None)'),
        ('"runtime_cmake_source": (ROOT / "tools/runtime/CMakeLists.txt", "1537fb26525b7e8fc715d83718427918ff81df0b11f2de054a02a46bcf416b45")',
         '"runtime_cmake_source": (ROOT / "tools/runtime/CMakeLists.txt", "d5fec7cbe05fa85784a1f1d68271e06294aac1de0f8e454947dea1d9a3adcf83")'),
        ('            and value.get("codex_minimized") is True',
         '            and value.get("codex_minimized") is True\n'
         '            and type(value.get("workload_seconds")) is int and value.get("workload_seconds") == 3600'),
        ("A fresh human reply must cover this run, AC, open lid, visible Harness and Codex minimized",
         "A fresh human reply must cover this exact3600s run, AC, open lid, visible Harness and Codex minimized"),
        ('default=600)', 'default=3600)'),
        ('(args.seconds, args.control_interval) != (600, 300)', '(args.seconds, args.control_interval) != (3600, 300)'),
        ('exactly 600s/300s diagnostic', 'exactly3600s/300s loaded diagnostic'),
        ('default=BASE / "flash-loaded-human-readiness.json"', 'default=BASE / "flash-60minute-human-readiness.json"'),
        ('default=BASE / "flash-quality-laptop-01/comparison/result.json"', 'default=BASE / "flash-promoted-repro-01/result.json"'),
        ('default=BASE / "flash-loaded-source-pins.json"', 'default=BASE / "flash-60minute-source-pins.json"'),
        ('config_path = BASE / "flash-loaded-expected-pins.json"', 'config_path = BASE / "flash-60minute-expected-pins.json"'),
        ('"--seconds", "600",', '"--seconds", "3600",'),
        ('"expected_controls": 1,', '"expected_controls": 11,'),
    ]
    for old, new in substitutions:
        require(parent.count(old) == 1, "Independent delta anchor count differs: " + old)
        parent = parent.replace(old, new, 1)
    require(parent == current, "Rendered launcher contains a change outside the reviewed delta")


class MemoryFile:
    def __init__(self, value):
        self.raw = json.dumps(value)

    def is_file(self):
        return True

    def stat(self):
        return SimpleNamespace(st_size=len(self.raw.encode()))

    def read_text(self):
        return self.raw


def run_checks(args, report):
    checks = report["checks"]
    def checked(name):
        checks.append({"name": name, "passed": True})
    wrapper = read_pinned(WRAPPER, WRAPPER_SHA)
    read_pinned(PREPARER, PREPARER_SHA)
    parent = read_pinned(BASE / "flash-loaded-render-02/observed-launch.py", PARENT_SHA)
    current = read_pinned(args.render_dir / "observed-launch.py", RENDER_SHA)
    wrapper_tree, current_tree = ast.parse(wrapper), ast.parse(current)
    ast.parse(PREPARER.read_text())
    exact_delta(parent, current)
    checked("only_reviewed_duration_proof_path_readiness_and_cmake_delta")
    for leaf, digest in (("window_guard_source.py", "cd064312e996672fed81e570f2acc0c8efd40e0fa76520cb0b3566da560f18b1"),
                         ("observer_source.py", "a21b21408ecb1a1108d310eee5ebcbad0187c4894fa308b55c495be4f16a9664")):
        read_pinned(args.render_dir / leaf, digest)
    provenance = json.loads((args.render_dir / "provenance.json").read_text())
    require(provenance["adapter_sha256"] == WRAPPER_SHA and provenance["rendered_launcher_sha256"] == RENDER_SHA,
            "Rendered provenance differs")
    checked("same_nested_lifecycle_sources_and_current_provenance")
    main = function(current_tree, "main")
    wait = function(main, "monitored_wait")
    require(not any(isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id == "human_readiness"
                    for n in ast.walk(wait)), "Human age is checked during the workload")
    calls = [n for n in ast.walk(main) if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
             and n.func.id == "human_readiness"]
    require(len(calls) == 2, "Expected exactly setup and workload-admission human checks")
    require(current.count("deadline = began + args.seconds + 750") == 1
            and current.index("deadline = began + args.seconds + 750") < current.index("prepare_proof_sources(args.source_pins")
            and '"maximum_total_seconds": args.seconds + 750' in current
            and '"expected_controls": 11' in current and 3600 + 750 == 4350,
            "Fixed initial budget/control count changed")
    checked("fixed_4350_budget_eleven_controls_and_no_midworkload_human_age_renewal")

    now = datetime(2026, 10, 19, 3, 0, tzinfo=timezone.utc)
    class Clock:
        fromisoformat = staticmethod(datetime.fromisoformat)
        @staticmethod
        def now(_zone):
            return now
    def memory_pin(path):
        return {"sha256": hashlib.sha256(path.raw.encode()).hexdigest(), "bytes": len(path.raw.encode())}
    ns = {"json": json, "datetime": Clock, "timezone": timezone, "require": require, "pin": memory_pin}
    human = extracted(function(current_tree, "human_readiness"), ns)
    label = "offline-hour-fixture-only"
    value = {"schema": 1, "decision": "ready", "ready_for_run": label, "AC_power": True,
             "lid_open": True, "visible_window": True, "codex_minimized": True, "workload_seconds": 3600,
             "human_reply": "Synthetic offline fixture; not human authorization",
             "recorded_at_utc": (now - timedelta(seconds=10)).isoformat()}
    require(human(MemoryFile(value), label)["age_seconds"] == 10, "Valid hour fixture rejected")
    checked("rendered_human_function_accepts_exact_hour_label_and_duration")
    invalid = [("600s_record", {"workload_seconds": 600}), ("boolean_duration", {"workload_seconds": True}),
               ("old_label", {"ready_for_run": "offline-old-ten-minute-fixture"}),
               ("stale", {"recorded_at_utc": (now - timedelta(seconds=901)).isoformat()}),
               ("future", {"recorded_at_utc": (now + timedelta(seconds=1)).isoformat()})]
    missing = dict(value)
    del missing["workload_seconds"]
    invalid.append(("missing_duration", None))
    for name, changes in invalid:
        candidate = missing if changes is None else {**value, **changes}
        try:
            human(MemoryFile(candidate), label)
        except ValueError:
            checked("rendered_human_rejects_" + name)
        else:
            raise ValueError("Invalid readiness accepted: " + name)
    same_pin = next(n for n in ast.walk(main) if isinstance(n, ast.Expr) and isinstance(n.value, ast.Call)
                    and any(isinstance(v, ast.Constant) and v.value == "Human-readiness decision changed during setup"
                            for v in n.value.args))
    memory = MemoryFile(value)
    ns["initial_human"] = human(memory, label)
    memory.raw += "\n"
    ns["state"] = {"human_readiness_before_workload": human(memory, label)}
    try:
        exec(compile(ast.fix_missing_locations(ast.Module(body=[same_pin], type_ignores=[])), "[same-pin-check]", "exec"), ns)
    except ValueError as error:
        require(str(error) == "Human-readiness decision changed during setup", "Unexpected pin rejection")
        checked("actual_before_after_admission_statement_rejects_changed_record_pin")
    else:
        raise ValueError("Changed readiness pin accepted")

    if args.proof_config is None:
        report["promoted_proof_checks"] = "not_executed_pending_actual_armed_configuration_and_closed_numerical_proof"
        return
    require(args.source_pins is not None, "--proof-config requires actual --source-pins")
    config = json.loads(args.proof_config.read_text())
    actual = json.loads((BASE / "flash-promoted-repro-01/result.json").read_text())
    require(actual.get("status") == "reproduction_complete" and actual.get("completed_requests") == 16,
            "Actual closed numerical proof is not available; no proof checks fabricated")
    spec = importlib.util.spec_from_file_location("conflux_hour_actual_proof_review", WRAPPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.prepare_proof_sources(args.source_pins, config, time.monotonic() + 180)
    checked("actual_promoted_proof_and_source_map_positive")
    mutations = [("old_runtime_pin", ("pins", "runtime"), "6f311e461e7af5f12ff38f90ddb8720179c2c3dce23d593437d2b34524dffd9d"),
                 ("old_build_pin", ("promoted_build_sha256",), "9c7d503af0b0c9b4c1b4e7e8c533fe888376c2a45f19942529f2e01247d7053d"),
                 ("old_closed_quality_as_reproduction", ("reproduction_proof_sha256",), "917104a1cc8ca2522eef38bae6d620c972fdb2be826876b2dee88e67dd7a497a")]
    for name, keys, digest in mutations:
        changed = copy.deepcopy(config)
        target = changed
        for key in keys[:-1]:
            target = target[key]
        target[keys[-1]] = digest
        try:
            module.prepare_proof_sources(args.source_pins, changed, time.monotonic() + 180)
        except (ValueError, RuntimeError):
            checked("actual_proof_rejects_" + name)
        else:
            raise ValueError("Old proof identity accepted: " + name)
    report["promoted_proof_checks"] = "actual_positive_and_three_identity_mutations_checked"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--render-dir", type=Path, default=BASE / "flash-60minute-render-01")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--proof-config", type=Path)
    parser.add_argument("--source-pins", type=Path)
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_plan", "native_executed": False, "host_queried": False,
                          "scope": "Source/AST and fake-file/clock checks only; optional actual closed-proof metadata checks."}))
        return 0
    require(args.output is not None and args.output.is_absolute() and args.output.parent == BASE
            and not args.output.exists() and not args.output.is_symlink(), "Use a fresh absolute same-experiment receipt")
    report = {"schema": 1, "status": "checking", "wrapper_sha256": WRAPPER_SHA, "rendered_sha256": RENDER_SHA,
              "preparer_sha256": PREPARER_SHA, "validator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "checks": [], "errors": [], "native_executed": False, "host_queried": False,
              "human_readiness_created": False, "scope": "Hour-specific delta only; unchanged parent20/child9 lifecycle scenarios not rerun. Memory-only readiness fixtures are not authorization."}
    try:
        run_checks(args, report)
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["errors"].append({"type": type(error).__name__, "message": str(error)})
    report["check_count"] = len(report["checks"])
    with args.output.open("x") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"status": report["status"], "checks": report["check_count"], "receipt": str(args.output)}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())

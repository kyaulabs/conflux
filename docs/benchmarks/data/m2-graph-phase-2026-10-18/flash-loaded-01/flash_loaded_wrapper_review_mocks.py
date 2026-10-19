#!/usr/bin/env python3
"""Offline checks of reviewed loaded-wrapper boundaries; never launch children."""
import argparse
import ast
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import io
import json
import math
from pathlib import Path
from types import SimpleNamespace

BASE = Path(__file__).resolve().parent
WRAPPER_SHA = "883d537f8f2fe91dc2e5fc5a735ca20067a007cbe8e308e9f1673825bf39e684"
MOCKS_SHA = "470d0cd7a9bf2d641bd87f9ad25224660fd6a56c8ca730903c0eb11c38ee09a2"
FROZEN_SHA = "511bb712d2f7af01abb1ab25ca92b001c6f82af4ee6f42547a5b43fa32b237d6"


def require(value, message):
    if not value:
        raise AssertionError(message)


def read(path, expected=None):
    raw = path.read_bytes()
    require(0 < len(raw) <= 128 * 1024, "Oversized/empty offline source")
    if expected:
        require(hashlib.sha256(raw).hexdigest() == expected, "Reviewed source changed: " + str(path))
    return raw


def function(tree, name):
    return next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == name)


def compile_nodes(nodes, namespace):
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "<offline-AST>", "exec"), namespace)


def rejects(callback):
    try:
        callback()
    except ValueError:
        return
    raise AssertionError("Expected fail-closed validation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--render-dir", type=Path, default=BASE / "flash-loaded-render-02")
    args = parser.parse_args()
    wrapper_raw = read(BASE / "flash_loaded_launch.py", WRAPPER_SHA)
    rendered_raw = read(args.render_dir / "observed-launch.py")
    frozen_raw = read(BASE / "observed_launch.py", FROZEN_SHA)
    provenance = json.loads(read(args.render_dir / "provenance.json"))
    require(provenance["adapter_sha256"] == WRAPPER_SHA
            and provenance["rendered_launcher_sha256"] == hashlib.sha256(rendered_raw).hexdigest()
            and provenance["frozen_observed_launcher_sha256"] == FROZEN_SHA,
            "Render provenance does not bind reviewed wrapper/source")
    for item in provenance["nested_helpers"].values():
        read(Path(item["selected_path"]), item["selected_sha256"])
        read(Path(item["original_path"]), item["original_sha256"])
    mock_path = BASE / "flash_loaded_child_review_mocks.py"
    read(mock_path, MOCKS_SHA)
    spec = importlib.util.spec_from_file_location("loaded_offline_fakes", mock_path)
    mocks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mocks)  # Definitions only; its main guard does not run.
    wrapper, rendered, original = ast.parse(wrapper_raw), ast.parse(rendered_raw), ast.parse(frozen_raw)
    main_node, original_main = function(rendered, "main"), function(original, "main")
    scenarios = []

    def passed(name):
        scenarios.append(name)

    def assignment(node, target):
        return isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == target for t in node.targets)

    command = next(n for n in ast.walk(main_node) if assignment(n, "command"))
    old_command = next(n for n in ast.walk(original_main) if assignment(n, "command"))
    require(ast.dump(command, include_attributes=False) == ast.dump(old_command, include_attributes=False),
            "Soak/UI/audio command changed")
    deadlines = [n for n in ast.walk(main_node) if assignment(n, "deadline")]
    prepare = next(n for n in ast.walk(main_node) if isinstance(n, ast.Call)
                   and isinstance(n.func, ast.Name) and n.func.id == "prepare_proof_sources")
    require(len(deadlines) == 1 and deadlines[0].lineno < prepare.lineno
            and ast.unparse(deadlines[0].value) == "began + args.seconds + 750",
            "Original1350-second deadline was reset or moved after proof preparation")
    passed("Fixed1350-second deadline precedes proof setup; exact soak/UI/audio command unchanged")

    nested = {node.name: node for node in main_node.body if isinstance(node, ast.FunctionDef)}
    spawn_blocks = []
    for node in ast.walk(main_node):
        if isinstance(node, ast.With) and node.body and assignment(node.body[0], "constructing_child"):
            spawn_blocks.append(node.body[:3])
    require(len(spawn_blocks) == 2, "Expected exactly readiness and workload publication blocks")
    for phase, block in zip(("normal_readiness", "workload"), sorted(spawn_blocks, key=lambda b: b[0].lineno)):
        for signum in (2, 15):
            world = mocks.World(injected=signum)
            state, groups, remembered = {"child_started": False}, {}, []
            namespace = {"child": None, "known_groups": groups, "state": state, "constructing_child": False,
                         "active_phase": phase, "time": world.clock, "os": world.os, "signal": world.signal,
                         "subprocess": world.subprocess, "require": lambda v, msg: v or (_ for _ in ()).throw(ValueError(msg)),
                         "ROOT": Path("/offline"), "out": io.StringIO(), "err": io.StringIO(),
                         "readiness_command": ["offline-readiness"], "command": ["offline-workload"], "environment": {}}
            namespace["remember_owned"] = lambda: remembered.append(namespace["child"].pid)
            compile_nodes([nested[name] for name in ("signal_child", "finish_group", "stop")], namespace)
            for number in (2, 15):
                world.signal.signal(number, namespace["stop"])
            try:
                compile_nodes(block, namespace)
            except KeyboardInterrupt:
                pass
            else:
                raise AssertionError("Deferred construction interrupt did not propagate")
            require(namespace["child"] is world.child and world.child.pid in groups and remembered == [world.child.pid]
                    and state["interruption_signal"] == signum
                    and state["child_started"] is (phase == "workload"), "Interrupted child publication metadata is inaccurate")
            if phase == "workload":
                require(state["owned_guard_process_group"] == world.child.pid
                        and state["child_spawned_monotonic"] > 0, "Workload ownership receipt missing")
            namespace["finish_group"](world.child.pid)
            require(not groups and not world.group_alive and state["owned_group_cleanup"][-1]["disappearance_observed"],
                    "Interrupted published group was not cleaned")
            passed(phase + " publication SIG" + ("INT" if signum == 2 else "TERM") + " preserves ownership/started flag and cleanup")

    world = mocks.World(dead=True)
    world.group_alive = True
    state, groups = {}, {world.child.pid: {"phase": "offline"}}
    namespace = {"child": world.child, "known_groups": groups, "state": state,
                 "time": world.clock, "os": world.os, "signal": world.signal,
                 "require": lambda v, msg: v or (_ for _ in ()).throw(ValueError(msg))}
    compile_nodes([nested[name] for name in ("signal_child", "finish_group")], namespace)
    namespace["signal_child"](2)
    namespace["finish_group"](world.child.pid)
    require(not groups and not world.group_alive and 2 in world.kills and 9 in world.kills,
            "Outer cleanup depended on a live leader")
    passed("Known outer group is signalled/cleaned/probed after leader exit")

    model, digest = Path("/offline/model.gguf"), "f" * 64
    context = {"compare": SimpleNamespace(MODEL=model, MODEL_SHA=digest)}
    namespace = {"math": math, "_context": context, "require": lambda v, msg: v or (_ for _ in ()).throw(ValueError(msg))}
    compile_nodes([function(wrapper, "latency_gate"), function(wrapper, "own_pcm_proof")], namespace)
    def result(p99, target=4095.0):
        flag = p99 < target
        return {"timing": {"continuous": {"hop_service_percentiles_ms": {"99": p99}, "chunks": 3,
                    "observed_p99_within_target": flag, "hops_above_target": 0},
                    "headroom_target_ms": target, "percentile_method": "nearest rank"},
                "acceptance": {"observed_continuous_p99_within_target": flag, "full_audio_UI_soak_qualified": False},
                "runtime_ready": [{"runtime": {"backend": "vulkan", "threads": 2, "fence_wait_mode": "blocking",
                    "weight_storage": "native", "model": str(model),
                    "process_environment": {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"}}, "model_id": digest}]}
    gate = namespace["latency_gate"]
    require(gate(result(4094.99))["passed"] is True and gate(result(4095.0))["passed"] is False
            and gate(result(4095.01))["passed"] is False, "Strict4095ms boundary changed")
    passed("Strict p99 below/equal/above4095ms boundary is preserved")
    for name, changed in (("relaxed target", result(4000, 4096)), ("nonfinite p99", result(float("nan")))):
        rejects(lambda value=changed: gate(value))
        passed("Gate rejects " + name)
    inconsistent = result(4095)
    inconsistent["acceptance"]["observed_continuous_p99_within_target"] = True
    rejects(lambda: gate(inconsistent))
    passed("Gate rejects inconsistent producer strict-pass flag")
    for name, value in (("runtime restart", result(4000)), ("wrong native backend", result(4000))):
        if name == "runtime restart":
            value["runtime_ready"] *= 2
        else:
            value["runtime_ready"][0]["runtime"]["backend"] = "cpu"
        rejects(lambda changed=value: gate(changed))
        passed("Gate rejects " + name)
    context.update(cost=SimpleNamespace(CLOSED=Path("/offline/closed.json"), FLASH_SHA="b" * 64),
                   proof={"cross_arm_exact_pcm": False})
    expected = {"pins": {"runtime": "b" * 64, "model": digest}}
    own = namespace["own_pcm_proof"]
    require(own(Path("/offline/closed.json"), expected)["selected_arm"] == "B", "Wrong selected proof arm")
    for wrong_path, wrong_config in ((Path("/offline/other.json"), expected),
            (Path("/offline/closed.json"), {"pins": {"runtime": "a" * 64, "model": digest}}),
            (Path("/offline/closed.json"), {"pins": {"runtime": "b" * 64, "model": "e" * 64}})):
        rejects(lambda p=wrong_path, c=wrong_config: own(p, c))
    passed("Own-arm admission selects B and rejects wrong proof path/runtime/model")

    now = datetime(2026, 10, 18, 23, tzinfo=timezone.utc)
    class FixedDate:
        fromisoformat = staticmethod(datetime.fromisoformat)
        @staticmethod
        def now(_zone):
            return now
    class Readiness:
        def __init__(self, value):
            self.raw = json.dumps(value)
        def is_file(self):
            return True
        def stat(self):
            return SimpleNamespace(st_size=len(self.raw))
        def read_text(self):
            return self.raw
    ns = {"json": json, "datetime": FixedDate, "timezone": timezone,
          "pin": lambda _: {"sha256": "offline"},
          "require": lambda v, msg: v or (_ for _ in ()).throw(ValueError(msg))}
    compile_nodes([function(rendered, "human_readiness")], ns)
    human = {"schema": 1, "decision": "ready", "ready_for_run": "offline", "AC_power": True,
             "lid_open": True, "visible_window": True, "codex_minimized": True,
             "human_reply": "Ready", "recorded_at_utc": (now - timedelta(seconds=60)).isoformat()}
    require(ns["human_readiness"](Readiness(human), "offline")["age_seconds"] == 60, "Fresh readiness rejected")
    for name, changed in (("stale original time", {"recorded_at_utc": (now - timedelta(seconds=901)).isoformat()}),
            ("future time", {"recorded_at_utc": (now + timedelta(seconds=1)).isoformat()}),
            ("wrong label", {"ready_for_run": "other"}), ("missing minimized", {"codex_minimized": False}),
            ("hidden Harness", {"visible_window": False}), ("AC disconnected", {"AC_power": False}),
            ("lid closed", {"lid_open": False})):
        value = {**human, **changed}
        rejects(lambda v=value: ns["human_readiness"](Readiness(v), "offline"))
        passed("Readiness rejects " + name)
    receipt = {"schema": 1, "status": "passed", "checks": scenarios, "check_count": len(scenarios),
               "wrapper_sha256": WRAPPER_SHA, "rendered_sha256": provenance["rendered_launcher_sha256"],
               "mock_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "scope": "Exact AST-extracted publication/cleanup/gate/readiness functions with fake children/signals/clock/records. No native, host, model, PCM, Git or actual test workload.",
               "limitations": ["Closed numerical proof/source lineage validation is delegated to the existing pinned preparation helper.",
                               "Handled SIGINT/TERM and known owned groups only; no arbitrary SIGKILL assurance.",
                               "Synthetic values test validator failures; they are not benchmark observations."]}
    output = BASE / "flash-loaded-wrapper-offline-mock-result.json"
    with output.open("x") as stream:
        json.dump(receipt, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"status": "passed", "checks": len(scenarios), "receipt": str(output)}))


if __name__ == "__main__":
    main()

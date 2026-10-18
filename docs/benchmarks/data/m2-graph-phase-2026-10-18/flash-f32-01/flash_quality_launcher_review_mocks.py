#!/usr/bin/env python3
"""Offline launcher AST cleanup checks; fake processes/files/signals only."""
import ast
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import signal
import subprocess
from types import SimpleNamespace

BASE = Path(__file__).resolve().parent
SOURCE = BASE / "flash_quality_launch.py"
WANTED = "ab912a468791b13ca283bacd84ce30163ed1c7a0576fd099dc859fd4670294c4"
DRIVER_WANTED = "cec1a694a32da562be9ed79d35cfda7e2ee1943b9b7dfd10daf1ed2259fc7495"
checks = []


def require(value, message):
    if not value:
        raise ValueError(message)


def passed(name):
    checks.append(name)


def execute(nodes, namespace):
    node = ast.fix_missing_locations(ast.Module(body=deepcopy(nodes), type_ignores=[]))
    exec(compile(node, "<reviewed-quality-launcher-extract>", "exec"), namespace)


def contains(node, value):
    return any(isinstance(x, ast.Constant) and x.value == value for x in ast.walk(node))


raw = SOURCE.read_bytes()
assert hashlib.sha256(raw).hexdigest() == WANTED
assert hashlib.sha256((BASE / "flash_quality.py").read_bytes()).hexdigest() == DRIVER_WANTED
tree = ast.parse(raw)
main = next(x for x in tree.body if isinstance(x, ast.FunctionDef) and x.name == "main")
functions = {x.name: x for x in main.body if isinstance(x, ast.FunctionDef)}
outer = next(x for x in main.body if isinstance(x, ast.Try) and x.finalbody)
process_cleanup = next(x for x in outer.finalbody if isinstance(x, ast.Try) and contains(x, "Owned processes remain after cleanup"))
spool_cleanup = next(x for x in outer.finalbody if isinstance(x, ast.Try) and contains(x, "spool_cleanup"))
cleanup_assignment = next(x for x in outer.finalbody if isinstance(x, ast.Assign)
                          and any(contains(t, "cleanup_complete") for t in x.targets))


class Clock:
    def __init__(self):
        self.value = 100.0

    def monotonic(self):
        self.value += 0.01
        return self.value

    def sleep(self, _):
        self.value += 0.1


class Spool:
    def __init__(self):
        self.present = True

    def exists(self):
        return self.present


def environment(mode):
    events, clock, spool = [], Clock(), Spool()
    child = SimpleNamespace(pid=810001, returncode=None if mode == "timeout" else 0)
    child.poll = lambda: child.returncode

    def wait(**kwargs):
        events.append(("wait", kwargs.get("timeout")))
        if child.returncode is None:
            raise subprocess.TimeoutExpired("fake-child", kwargs.get("timeout"))
        return child.returncode

    child.wait = wait

    def killpg(group, value):
        assert group == child.pid
        events.append(("signal", value))
        if value == signal.SIGKILL:
            child.returncode = -9
        if value == 0:
            raise ProcessLookupError()

    def cleanup():
        events.append(("spool-cleanup", None))
        assert child.returncode is not None
        spool.present = False

    ns = {"state": {"errors": []}, "child": child, "group": child.pid,
          "deadline": 700.0, "time": clock, "signal": signal, "subprocess": subprocess,
          "os": SimpleNamespace(getpgrp=lambda: 123, killpg=killpg), "require": require,
          "remember": lambda: events.append(("remember", None)), "remaining": lambda: {},
          "temporary": SimpleNamespace(cleanup=cleanup), "spool": spool}
    execute([functions["signal_group"]], ns)
    return ns, events


for mode in ("already-exited-leader", "timeout"):
    ns, events = environment(mode)
    execute([process_cleanup, spool_cleanup, cleanup_assignment], ns)
    assert ns["state"]["process_cleanup_complete"] and ns["state"]["cleanup_complete"]
    assert not ns["state"]["errors"] and not ns["spool"].exists()
    assert ("signal", signal.SIGKILL) in events and ("signal", 0) in events
    assert events.index(("signal", signal.SIGKILL)) < events.index(("spool-cleanup", None))
    passed(mode + ": known group killed/probed and only then private spool removed")

# Inject a pending signal while actual source code is inside its Popen block.
construction = next(x for x in ast.walk(main) if isinstance(x, ast.With)
                    and x.body and isinstance(x.body[0], ast.Assign)
                    and any(isinstance(t, ast.Name) and t.id == "constructing" for t in x.body[0].targets))
for sig in (signal.SIGINT, signal.SIGTERM):
    ns, events = environment("already-exited-leader")
    fake_child = ns["child"]
    ns.update(child=None, group=None, constructing=False, command=[], ROOT="unused", stdout=None, stderr=None, environment={})
    execute([functions["interrupted"]], ns)

    def popen(*args, **kwargs):
        assert kwargs["start_new_session"] is True
        ns["interrupted"](sig, None)
        events.append(("Popen-returned", None))
        return fake_child

    ns["subprocess"] = SimpleNamespace(Popen=popen, TimeoutExpired=subprocess.TimeoutExpired)
    try:
        execute(construction.body[:3], ns)
    except KeyboardInterrupt:
        pass
    else:
        raise AssertionError("Pending interruption was lost")
    assert ns["group"] == fake_child.pid and ns["child"] is fake_child
    assert ns["state"]["driver_started"] is True and ns["state"]["native_started"] is None
    assert not ns["constructing"]
    execute([process_cleanup, spool_cleanup, cleanup_assignment], ns)
    assert ns["state"]["cleanup_complete"] and not ns["spool"].exists()
    passed(sig.name + ": deferred constructor interruption preserves group and cleans process/spool")

# Parent-owned workspace cleanup also executes if setup failed before Popen.
ns, events = environment("already-exited-leader")
ns.update(child=None, group=None, temporary=None, spool=None)
execute([process_cleanup, spool_cleanup, cleanup_assignment], ns)
assert ns["state"]["cleanup_complete"] and not [x for x in events if x[0] == "signal"]
passed("Pre-child setup error cleans safely without signalling any process group")


class ResultPath:
    def __init__(self, present=True):
        self.present = present

    def __truediv__(self, _):
        return self

    def is_file(self):
        return self.present


for label, value, present, previous, wanted in (
    ("interim false", {"status": "running", "native_started": False}, True, None, None),
    ("partial true", {"status": "running", "native_started": True}, True, None, True),
    ("final false", {"status": "incomplete", "native_started": False, "cleanup_complete": False}, True, None, False),
    ("missing result", {}, False, None, None),
    ("prior true", {"status": "incomplete", "native_started": False, "cleanup_complete": False}, True, True, True)):
    state = {"driver_started": True, "native_started": previous, "errors": []}
    ns = {"state": state, "output": ResultPath(present), "deadline": 700,
          "compare": SimpleNamespace(inspect_file=lambda *_args, **_kwargs: ({"sha256": "fixture"}, json.dumps(value).encode()),
                                     decode_json=json.loads)}
    execute([functions["harvest"]], ns)
    ns["harvest"]()
    assert state["native_started"] is wanted
    passed(label + ": native-start observation retains authoritative/unknown distinction")

# Exercise actual AC/lid parsing with fake files and bounded fake busctl only.
ac = next(x for x in tree.body if isinstance(x, ast.FunctionDef) and x.name == "ac_lid")
for ac_value, lid_value, fail, ready in (("1", "b false", False, True),
                                         ("0", "b false", False, False),
                                         ("1", "b true", False, False),
                                         ("1", "", True, False)):
    calls = []

    def busctl(command, **kwargs):
        calls.append((command, kwargs))
        assert kwargs["timeout"] == 2
        if fail:
            raise subprocess.TimeoutExpired(command, 2)
        return lid_value

    ns = {"time": Clock(), "utc": lambda: "mock-time", "Path": lambda _: SimpleNamespace(read_text=lambda: ac_value),
          "subprocess": SimpleNamespace(check_output=busctl)}
    execute([ac], ns)
    result = ns["ac_lid"]()
    assert result["ready"] is ready and len(calls) == 1
    assert bool(result["errors"]) is fail
    passed("AC/lid parser " + str((ac_value, lid_value, fail)) + " stays fail-closed with2s bound")

deadline_assignments = [x for x in ast.walk(main) if isinstance(x, ast.Assign)
                        and any(isinstance(t, ast.Name) and t.id == "deadline" for t in x.targets)]
assert len(deadline_assignments) == 1
ns = {"began": 100.0, "TOTAL_SECONDS": 600}
execute(deadline_assignments, ns)
assert ns["deadline"] == 700.0
assert sum(isinstance(x, ast.Call) and isinstance(x.func, ast.Name) and x.func.id == "monitor" for x in ast.walk(main)) == 3
assert contains(main, "--deadline-monotonic") and contains(main, "latency_qualification") and contains(main, "not_attempted")
passed("One600second deadline forwarded to child; actual monitoring before/during/after; no latency pass")

receipt = {"schema": 1, "status": "passed_offline_quality_launcher_mocks", "launcher_sha256": WANTED,
           "driver_sha256": DRIVER_WANTED, "checks_passed": len(checks), "checks": checks,
           "scope": "Extracted reviewed AST with fake child/groups/signals/files/clock/readiness/spool only. No native process, real signal, host query, filesystem spool deletion, model/media/binary read, build/test/Git operation or numerical/musical/latency qualification."}
destination = BASE / "flash-quality-launcher-offline-mock-result.json"
destination.write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps({"status": receipt["status"], "checks_passed": len(checks), "receipt": str(destination)}))

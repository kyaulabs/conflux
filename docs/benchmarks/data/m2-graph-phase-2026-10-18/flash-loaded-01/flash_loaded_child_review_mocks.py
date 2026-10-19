#!/usr/bin/env python3
"""Offline AST/fake-only checks of the two scoped child-publication fixes."""
import argparse
import ast
from contextlib import redirect_stdout
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
from types import SimpleNamespace

BASE = Path(__file__).resolve().parent
PINS = {
    "flash_loaded_window_guard.py": "cd064312e996672fed81e570f2acc0c8efd40e0fa76520cb0b3566da560f18b1",
    "flash_loaded_observer.py": "a21b21408ecb1a1108d310eee5ebcbad0187c4894fa308b55c495be4f16a9664",
}


def require(value, message):
    if not value:
        raise AssertionError(message)


class Signal:
    SIGINT, SIGTERM, SIGKILL, SIG_IGN = 2, 15, 9, "ignored"

    def __init__(self):
        self.original = lambda *_: None
        self.handlers = {2: self.original, 15: self.original}

    def signal(self, number, handler):
        before = self.handlers[number]
        self.handlers[number] = handler
        return before

    def emit(self, number):
        require(self.handlers[number] is not self.original, "Handler was not installed before child construction")
        if callable(self.handlers[number]):
            self.handlers[number](number, None)


class Clock:
    value = 100.0

    def monotonic(self):
        self.value += .01
        return self.value

    def sleep(self, seconds):
        self.value += seconds


class MemoryPath:
    files = {}

    def __init__(self, value):
        self.value = str(value)

    def write_text(self, text):
        self.files[self.value] = text
        return len(text)


class Child:
    pid = 4242

    def __init__(self, dead):
        self.returncode = 0 if dead else None
        self.waits = []

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        self.waits.append(timeout)
        if self.returncode is None:
            self.returncode = 0
        return self.returncode


class World:
    def __init__(self, injected=None, dead=False, resistant=False):
        self.signal, self.clock = Signal(), Clock()
        self.injected, self.resistant = injected, resistant
        self.child = Child(dead)
        self.group_alive, self.spawns, self.kills = False, 0, []
        self.os = SimpleNamespace(getpgrp=lambda: 9999, killpg=self.killpg)
        self.subprocess = SimpleNamespace(Popen=self.popen, TimeoutExpired=TimeoutError)

    def popen(self, _command, **kwargs):
        require(kwargs.get("start_new_session") is True, "Owned group must be a new session")
        self.spawns += 1
        self.group_alive = True
        if self.injected is not None:
            self.signal.emit(self.injected)  # Child exists, caller assignment has not happened.
        return self.child

    def killpg(self, group, number):
        require(group == self.child.pid and group != self.os.getpgrp(), "Unexpected/unsafe group")
        self.kills.append(number)
        if not self.group_alive:
            raise ProcessLookupError
        if number == self.signal.SIGINT and self.child.returncode is None:
            self.child.returncode = 130  # Leader exits; retain a fake group member until KILL.
        if number == self.signal.SIGKILL and not self.resistant:
            self.group_alive = False

    def handlers_restored(self):
        return all(value is self.signal.original for value in self.signal.handlers.values())


def extracted(path, name, namespace):
    raw = path.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == PINS[path.name], "Reviewed source pin changed: " + path.name)
    tree = ast.parse(raw)
    node = next(node for node in tree.body if isinstance(node, (ast.ClassDef, ast.FunctionDef)) and node.name == name)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(path) + "[AST-only]", "exec"), namespace)
    return namespace[name]


def guard_case(injected=None, dead=False, resistant=False, prelaunch=False, restore=False):
    world, calls, saves = World(injected, dead, resistant), [], []
    holder = {}
    connection = SimpleNamespace(register_object=lambda *_: None, get_unique_name=lambda: ":offline",
                                 close_sync=lambda _: calls.append("connection_closed"))

    def loop_run():
        if restore:
            holder["guard"].script_id = 7
            holder["guard"].record["restored"] = {"reason": "offline-owned-window-restored"}

    namespace = {"datetime": datetime, "timezone": timezone, "time": world.clock,
                 "signal": world.signal, "os": world.os, "subprocess": world.subprocess,
                 "uuid": SimpleNamespace(uuid4=lambda: SimpleNamespace(hex="offline")),
                 "Gio": SimpleNamespace(bus_get_sync=lambda *_: connection,
                     BusType=SimpleNamespace(SESSION=0),
                     DBusNodeInfo=SimpleNamespace(new_for_xml=lambda _: SimpleNamespace(interfaces=[None]))),
                 "GLib": SimpleNamespace(MainLoop=lambda: SimpleNamespace(run=loop_run),
                                         timeout_add=lambda *_: None),
                 "OBJECT": "/offline", "XML": "<offline/>"}
    Guard = extracted(BASE / "flash_loaded_window_guard.py", "Guard", namespace)
    guard = Guard(SimpleNamespace(command=["offline-child"], idle_only=False))
    holder["guard"] = guard
    guard.save = lambda: saves.append(json.loads(json.dumps(guard.record)))

    def call(_service, _path, _interface, method, *_args):
        calls.append(method)
        if prelaunch and method == "Inhibit":
            world.signal.emit(world.signal.SIGTERM)
        value = (23,) if method == "Inhibit" else (False,) if method == "isScriptLoaded" else ()
        return SimpleNamespace(unpack=lambda: value)

    guard.call = call
    result = guard.run()
    require(world.handlers_restored(), "Guard did not restore signal handlers")
    require(guard.record.get("idle_inhibit_released") is True and "UnInhibit" in calls
            and "connection_closed" in calls, "Guard skipped idle/connection cleanup")
    if prelaunch:
        require(world.spawns == 0 and result != 0, "Prelaunch stop spawned a child")
    elif resistant:
        require(result != 0 and any("child_cleanup:" in error for error in guard.record["errors"])
                and guard.record["child_group_cleanup"]["disappearance_observed"] is False,
                "Guard claimed failed group cleanup succeeded")
    else:
        require(world.spawns == 1 and not world.group_alive and world.child.waits
                and guard.record["owned_child_group"] == world.child.pid
                and guard.record["child_group_cleanup"]["disappearance_observed"] is True
                and world.signal.SIGKILL in world.kills, "Guard did not clean/reap the known child group")
        if injected is not None:
            require(world.signal.SIGINT in world.kills and result == 130,
                    "Guard lost a construction-time pending stop")
    if restore:
        require("unloadScript" in calls and guard.record.get("script_unloaded") is True
                and guard.record.get("restored"), "Guard skipped owned-window restoration/unload")
    return {"returncode": result, "spawns": world.spawns, "signals": world.kills,
            "group_disappeared": not world.group_alive, "idle_released": True,
            "window_unloaded": guard.record.get("script_unloaded"), "errors": guard.record["errors"]}


def observer_case(injected=None, dead=False, resistant=False):
    MemoryPath.files.clear()
    world = World(injected, dead, resistant)
    policies = []

    def policy():
        policies.append("observe")
        return {"profile": "balanced", "holds": "", "firmware": "balanced", "cpu_epp": "balance_performance"}

    namespace = {"argparse": argparse, "Path": MemoryPath, "datetime": datetime, "timezone": timezone,
                 "json": json, "signal": world.signal, "time": world.clock, "os": world.os,
                 "subprocess": world.subprocess, "policy": policy}
    main = extracted(BASE / "flash_loaded_observer.py", "main", namespace)
    with redirect_stdout(io.StringIO()):
        result = main(["offline-policy.json", "--seconds", "600"])
    record = json.loads(MemoryPath.files["offline-policy.json"])
    require(world.handlers_restored() and len(policies) == 4, "Observer lost handlers or before/after policy observations")
    require(record["profile_mutation"] is False and record["mode"] == "observe"
            and record["owned_child_group"] == world.child.pid, "Observer altered policy or lost ownership")
    if resistant:
        require(result == 1 and "child_cleanup_error" in record
                and record["child_group_cleanup"]["disappearance_observed"] is False,
                "Observer claimed failed cleanup succeeded")
    else:
        require(not world.group_alive and world.child.waits and world.signal.SIGKILL in world.kills
                and record["child_group_cleanup"]["disappearance_observed"] is True,
                "Observer did not clean/reap the known child group after leader exit")
        if injected is not None:
            require(result == 130 and record["interrupted"] is True and record["interruption_signal"] == injected,
                    "Observer lost a deferred construction interrupt")
    return {"returncode": result, "signals": world.kills, "group_disappeared": not world.group_alive,
            "policy_observations": len(policies), "cleanup_error": record.get("child_cleanup_error")}


def main():
    scenarios = {}
    for number, label in ((2, "SIGINT"), (15, "SIGTERM")):
        scenarios["guard-publication-" + label] = guard_case(injected=number)
        scenarios["observer-publication-" + label] = observer_case(injected=number)
    scenarios["guard-stop-before-child"] = guard_case(prelaunch=True)
    scenarios["guard-dead-leader-and-window-restore"] = guard_case(dead=True, restore=True)
    scenarios["guard-bounded-failure-still-restores-window-idle"] = guard_case(dead=True, resistant=True, restore=True)
    scenarios["observer-dead-leader"] = observer_case(dead=True)
    scenarios["observer-bounded-failure-still-observes-policy"] = observer_case(dead=True, resistant=True)
    report = {"schema": 1, "status": "passed", "scenarios": scenarios, "scenario_count": len(scenarios),
              "source_sha256": PINS, "mock_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "scope": "Actual AST-extracted cleanup/publication code with fake signals, process groups, Popen, time, GLib and D-Bus. No native/host/policy/test workload.",
              "limitations": ["Handled SIGINT/TERM and owned-group behavior only; no arbitrary SIGKILL assurance.",
                              "Fake resistant groups intentionally test failure reporting; no actual process exists.",
                              "This is not UI restoration, hardware, latency or performance qualification."]}
    destination = BASE / "flash-loaded-child-offline-mock-result.json"
    with destination.open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"status": report["status"], "scenarios": len(scenarios), "receipt": str(destination)}))


if __name__ == "__main__":
    main()

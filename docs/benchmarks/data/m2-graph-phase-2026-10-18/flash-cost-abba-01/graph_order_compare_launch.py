#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Dry by default: guard the isolated baseline/graph-order exact PCM comparison."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
LAUNCHER = BASE / "observed_launch.py"
LAUNCHER_SHA = "511bb712d2f7af01abb1ab25ca92b001c6f82af4ee6f42547a5b43fa32b237d6"
DRIVER = BASE / "graph_order_compare.py"
DRIVER_SHA = "28d2712ba427d0f480ad63f8ff3128f0d3999010e7ecf225185c41734861a3e7"
READINESS_MAX_SECONDS = 620
COMPARISON_MAX_SECONDS = 600
AFTER_PINS_MAX_SECONDS = 60
TOTAL_MAX_SECONDS = 1320


def require(value, message):
    if not value:
        raise RuntimeError(message)


def utc():
    return datetime.now(timezone.utc).isoformat()


def pin(path):
    before = path.stat()
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    after = path.stat()
    require(signature(before) == signature(after), "Artifact changed while hashing: " + str(path))
    return {"path": str(path.resolve()), "bytes": after.st_size, "sha256": digest}


def signature(value):
    return [value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns]


def source_signature(path):
    return signature(path.stat())


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def human_readiness(path, label):
    require(path.is_file() and path.stat().st_size <= 16384, "Supply a small fresh human-readiness JSON")
    value = json.loads(path.read_text())
    require(value.get("schema") == 1 and value.get("decision") == "ready"
            and value.get("ready_for_run") == label and value.get("AC_power") is True
            and value.get("lid_open") is True and isinstance(value.get("human_reply"), str)
            and 0 < len(value["human_reply"]) <= 8192,
            "Fresh human readiness must cover this exact label, AC power and open lid")
    issued = datetime.fromisoformat(value.get("recorded_at_utc", ""))
    require(issued.tzinfo is not None, "Human-readiness time must include timezone")
    age = (datetime.now(timezone.utc) - issued).total_seconds()
    require(0 <= age <= 900, "Human readiness is future-dated or older than 15 minutes; ask afresh")
    return {"record": value, "age_seconds": age, "maximum_age_seconds": 900, "pin": pin(path),
            "window_required_for_this_phase": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", help="Fresh label for this graph-order comparison and human readiness")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--readiness-decision", type=Path, default=BASE / "human-readiness.json")
    parser.add_argument("--source-pins", type=Path, help="Absolute source-pin map required by the frozen comparison driver")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", args.name):
        parser.error("Use a fresh plain run label")
    output = BASE / (args.name + "-comparison")
    admission = BASE / (args.name + "-comparison-admission")
    state_path = BASE / (args.name + "-comparison-launch.json")
    monitor_path = BASE / (args.name + "-comparison-AC-lid-monitor.jsonl")
    stdout_path = BASE / (args.name + "-comparison.stdout")
    stderr_path = BASE / (args.name + "-comparison.stderr")
    config_path = BASE / "graph-order-expected-pins.json"
    expected = json.loads(config_path.read_text()) if config_path.is_file() else {"status": "unarmed"}
    if not args.run:
        print(json.dumps({"schema": 1, "status": "dry_run", "native_started": False, "host_queried": False,
              "name": args.name, "pins_status": expected.get("status"), "driver": str(DRIVER),
              "output": str(output), "source_pins": str(args.source_pins) if args.source_pins else None,
              "readiness_decision": str(args.readiness_decision), "readiness_maximum_age_seconds": 900,
              "normal_readiness_maximum_seconds": READINESS_MAX_SECONDS,
              "comparison_maximum_seconds": COMPARISON_MAX_SECONDS,
              "driver_after_pins_maximum_seconds": AFTER_PINS_MAX_SECONDS,
              "maximum_total_seconds": TOTAL_MAX_SECONDS, "UI_load": False, "window_guard": False,
              "scope": "Plan only. Explicit --run, armed runtime/source pins and fresh human AC/open-lid readiness are required. Exact PCM comparison only; no latency or listening qualification."}, indent=2))
        return 0
    if (expected.get("status") != "ready"
            or not re.fullmatch(r"[0-9a-f]{40}", expected.get("repository_head") or "")
            or any(not re.fullmatch(r"[0-9a-f]{64}", expected.get("pins", {}).get(name) or "")
                   for name in ("runtime", "model", "worker_source", "shared_benchmark_source", "sampler_source"))
            or args.source_pins is None or not args.source_pins.is_absolute()):
        parser.error("Supply --source-pins and arm graph-order-expected-pins.json with exact candidate/HEAD/source hashes first")
    initial_human = human_readiness(args.readiness_decision, args.name)
    require(not any(path.exists() for path in (output, admission, state_path, monitor_path, stdout_path, stderr_path)),
            "Comparison/admission paths already exist; use a fresh label")
    admission.mkdir()
    began = time.monotonic()
    deadline = began + TOTAL_MAX_SECONDS
    state = {"schema": 1, "status": "checking", "started_at_utc": utc(), "started_monotonic": began,
             "expected_producer": expected, "human_readiness_before_setup": initial_human,
             "child_started": False, "pins": {}, "failures": [], "UI_load": False, "window_guard": False,
             "maximum_total_seconds": TOTAL_MAX_SECONDS, "profile_mutation": False, "service_mutation": False,
             "scope": "Sequential two-thread baseline/graph-order exact PCM check with fresh normal readiness and AC/lid monitoring. Driver verifies exact source/runtime/model/assets including candidate overlay/build sources. Per-child VmHWM and candidate graph-buffer bytes are observations, without a new threshold or baseline graph-buffer equivalent. No UI, playback, latency, GPU timestamp or perceptual qualification."}
    child = helper = retained = adapter = comparison_driver = None
    constructing_child = False
    owned, signatures, known_groups = {}, {}, {}
    phase = "checking"

    def save(**fields):
        state.update(fields, elapsed_seconds=time.monotonic() - began, updated_at_utc=utc())
        temporary = state_path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(state, indent=2, allow_nan=False) + "\n")
        temporary.replace(state_path)

    def remember_owned():
        if child is None:
            return
        pending, visited = [child.pid], set()
        while pending and len(visited) < 64:
            pid = pending.pop()
            if pid in visited:
                continue
            visited.add(pid)
            proc = Path("/proc") / str(pid)
            try:
                require(proc.stat().st_uid == os.getuid(), "Owned descendant UID changed")
                fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
                owned[pid] = {"start_ticks": int(fields[19]), "process_group": int(fields[2])}
                pending.extend(int(value) for value in (proc / "task" / str(pid) / "children").read_text().split())
            except FileNotFoundError:
                pass

    def live_owned():
        live = {}
        for pid, identity in owned.items():
            try:
                fields = (Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()
                if int(fields[19]) == identity["start_ticks"] and int(fields[2]) == identity["process_group"]:
                    live[pid] = dict(identity, state=fields[0])
            except FileNotFoundError:
                pass
        return live

    def signal_child(signum):
        if child is not None and child.poll() is None:
            try:
                os.killpg(child.pid, signum)
            except ProcessLookupError:
                pass

    def finish_owned_group(group):
        require(group in known_groups and group > 1 and group != os.getpgrp(),
                "Refusing cleanup of an unknown/unsafe process group")
        action = {"group": group, **known_groups[group], "started_monotonic": time.monotonic(),
                  "disappearance_observed": False}
        state.setdefault("owned_group_cleanup", []).append(action)
        try:
            os.killpg(group, signal.SIGKILL)
            action["kill_sent"] = True
        except ProcessLookupError:
            action["kill_sent"] = False
        until = time.monotonic() + 2
        while True:
            # Reap our direct leader once killed; an unreaped zombie can keep
            # its process group visible even after all running members stopped.
            if child is not None and child.pid == group:
                child.poll()
            try:
                os.killpg(group, 0)
            except ProcessLookupError:
                action.update(disappearance_observed=True, finished_monotonic=time.monotonic())
                del known_groups[group]
                return
            require(time.monotonic() < until, "Owned process group remains after bounded cleanup")
            time.sleep(.05)

    def interrupted(signum, _frame):
        state["interruption_signal"] = signum
        if not constructing_child:
            signal_child(signal.SIGINT)
            raise KeyboardInterrupt

    def monitored_wait(until):
        next_ac_check = next_owned_check = 0.0
        while child.poll() is None:
            now = time.monotonic()
            require(now < until, phase + " exhausted finite wall budget")
            if now >= next_owned_check:
                next_owned_check = now + 1
                remember_owned()
            if now >= next_ac_check:
                next_ac_check = now + 45
                row = {"event": "snapshot", "phase": phase, "at_utc": utc(), "monotonic": now,
                       "AC_lid": helper.ac_lid()}
                with monitor_path.open("a") as stream:
                    stream.write(json.dumps(row, allow_nan=False) + "\n")
                print(json.dumps(row, allow_nan=False), flush=True)
                require(row["AC_lid"]["ready"], "AC/lid changed or observation failed; abort owned comparison")
            time.sleep(min(.25, max(0, until - time.monotonic())))
        return child.wait()

    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        save()
        for name, path, wanted in (("retained_launcher", LAUNCHER, LAUNCHER_SHA), ("comparison_driver", DRIVER, DRIVER_SHA)):
            before = source_signature(path)
            actual = pin(path)
            require(actual["sha256"] == wanted and source_signature(path) == before, "Frozen helper changed: " + name)
            state["pins"][name], signatures[path] = actual, before
        retained = load_module("conflux_phase_retained_launcher", LAUNCHER)
        require(retained.head() == expected["repository_head"], "Expected source HEAD differs")
        adapter = load_module("conflux_graph_order_adapter", DRIVER)
        adapter.reject_diagnostic_environment()
        comparison_driver = adapter.verified_driver()
        comparison_driver.source_specs(args.source_pins, min(deadline, time.monotonic() + 10))
        require(expected["pins"]["model"] == comparison_driver.MODEL_SHA,
                "Expected model differs from the frozen comparison model")
        state["mandatory_experiment_sources"] = [str(path) for path in adapter.REQUIRED_EXPERIMENT_SOURCES]
        for name in ("admission_source", "readiness_source", "native_replay_source"):
            path, wanted = retained.HELPERS[name]
            before = source_signature(path)
            actual = pin(path)
            require(actual["sha256"] == wanted and before == source_signature(path), "Admission helper changed: " + name)
            state["pins"][name], signatures[path] = actual, before
        for name in ("worker_source", "shared_benchmark_source", "sampler_source"):
            path, _ = retained.PRODUCERS[name]
            before = source_signature(path)
            actual = pin(path)
            require(actual["sha256"] == expected["pins"][name] and before == source_signature(path), "Readiness import changed: " + name)
            state["pins"][name], signatures[path] = actual, before
        for name, path in (("launcher", Path(__file__).resolve()), ("expected_pins_file", config_path),
                           ("source_pins_file", args.source_pins), ("human_readiness_file", args.readiness_decision)):
            before = source_signature(path)
            state["pins"][name] = pin(path)
            require(before == source_signature(path), "Launcher/config changed while pinning")
            signatures[path] = before
        helper = load_module("conflux_phase_comparison_admission", retained.HELPERS["admission_source"][0])
        helper.readiness_module()  # Finish safe imports before normal readiness.
        require(not {key: os.environ[key] for key in helper.replay.DEBUG_FLAGS if key in os.environ},
                "Inherited GPU diagnostic flags present")
        state["host_conditions_before_admission"] = helper.ac_lid()
        require(state["host_conditions_before_admission"]["ready"], "AC online/open lid required")
        phase = "normal_readiness"
        command = [sys.executable, "-B", "-c", "import runpy,sys;from pathlib import Path;sys.exit(runpy.run_path(sys.argv[1])['run']('admission',Path(sys.argv[2])))",
                   str(retained.HELPERS["readiness_source"][0]), str(admission)]
        save(status=phase, readiness_command=command)
        with (admission / "stdout").open("x") as out, (admission / "stderr").open("x") as err:
            constructing_child = True
            try:
                child = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err, start_new_session=True)
                known_groups[child.pid] = {"phase": phase, "leader_pid": child.pid,
                                           "created_monotonic": time.monotonic()}
                remember_owned()
            finally:
                constructing_child = False
            if "interruption_signal" in state:
                signal_child(signal.SIGINT)
                raise KeyboardInterrupt
            code = monitored_wait(min(deadline - 30, time.monotonic() + READINESS_MAX_SECONDS))
            finish_owned_group(child.pid)
        state["readiness_returncode"] = code
        require(code == 0, "Bounded normal readiness did not match")
        child = None
        require(retained.head() == expected["repository_head"] and all(source_signature(path) == value for path, value in signatures.items()),
                "HEAD/helper/config changed during readiness")
        state["human_readiness_before_comparison"] = human_readiness(args.readiness_decision, args.name)
        require(state["human_readiness_before_comparison"]["pin"] == initial_human["pin"], "Human readiness changed during setup")
        state["admission"] = helper.fresh_admission(admission)
        phase = "comparison"
        command = ["systemd-inhibit", "--what=sleep:idle", "--mode=block", "--who=CONFLUX",
                   "--why=Bounded graph-order exact PCM comparison", sys.executable, "-B", str(DRIVER), "--run",
                   "--diagnostic-sha256", expected["pins"]["runtime"], "--source-pins", str(args.source_pins),
                   "--output", str(output), "--request-timeout", "30", "--max-seconds", str(COMPARISON_MAX_SECONDS)]
        with stdout_path.open("x") as out, stderr_path.open("x") as err:
            constructing_child = True
            try:
                child = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err, start_new_session=True)
                known_groups[child.pid] = {"phase": phase, "leader_pid": child.pid,
                                           "created_monotonic": time.monotonic()}
                state["child_started"] = True
                remember_owned()
            finally:
                constructing_child = False
            if "interruption_signal" in state:
                signal_child(signal.SIGINT)
                raise KeyboardInterrupt
            save(status="running", child_started=True, comparison_command=command, owned_comparison_process_group=child.pid)
            code = monitored_wait(min(deadline - 30, time.monotonic() + COMPARISON_MAX_SECONDS + AFTER_PINS_MAX_SECONDS + 10))
            finish_owned_group(child.pid)
        state["comparison_returncode"] = code
        require(code == 0, "Bounded PCM comparison returned failure")
        require(retained.head() == expected["repository_head"] and all(source_signature(path) == value for path, value in signatures.items()),
                "HEAD/helper/config changed during comparison")
        state["comparison"] = retained.comparison_proof(output / "result.json", expected)
        comparison_result = json.loads((output / "result.json").read_text())
        state["graph_order_observation"] = adapter.graph_order_observation(
            comparison_driver, comparison_result["runtimes"][1], min(deadline, time.monotonic() + 10))
        require(state["graph_order_observation"] == comparison_result["runtimes"][1].get("graph_order"),
                "Candidate graph-order stderr changed after driver validation")
        state["native_memory_before_close"] = {
            row["label"]: row.get("native_memory_before_close", {"available": False, "VmHWM_KiB": None})
            for row in comparison_result["runtimes"]}
        state["host_conditions_after_comparison"] = helper.ac_lid()
        require(state["host_conditions_after_comparison"]["ready"], "AC/lid changed at comparison completion")
        save(status="completed")
    except KeyboardInterrupt:
        state["status"] = "interrupted"
    except Exception as error:
        state["status"] = "error"
        state["failures"].append({"phase": phase, "type": type(error).__name__, "error": str(error)})
    finally:
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        try:
            remember_owned()
            if child is not None and child.poll() is None:
                signal_child(signal.SIGINT)
                try:
                    child.wait(timeout=min(25, max(.1, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    pass
            groups = set(known_groups) | {value["process_group"] for value in live_owned().values()}
            for group in groups:
                if group not in known_groups:
                    known_groups[group] = {"phase": "observed_owned_descendant"}
                finish_owned_group(group)
            if child is not None:
                child.wait(timeout=3)
            for _ in range(20):
                if not live_owned():
                    break
                time.sleep(.1)
            state["cleanup"] = {"owned_process_identities": owned, "remaining_owned_processes": live_owned(),
                                "remaining_known_owned_groups": dict(known_groups),
                                "last_child_reaped": child is None or child.poll() is not None}
            require(not state["cleanup"]["remaining_owned_processes"]
                    and not state["cleanup"]["remaining_known_owned_groups"] and state["cleanup"]["last_child_reaped"],
                    "Owned descendants remain after comparison cleanup")
        except Exception as error:
            state["status"] = "error"
            state["failures"].append({"phase": "cleanup", "error": str(error)})
        state.setdefault("cleanup", {})["remaining_known_owned_groups"] = dict(known_groups)
        for name, path in (("fresh_admission_capture", admission / "fresh-admission.json"),
                           ("readiness_proof", admission / "admission-normal-ready.json"),
                           ("comparison_result", output / "result.json"), ("stdout", stdout_path), ("stderr", stderr_path)):
            if path.is_file():
                state[name] = pin(path)
        with monitor_path.open("a") as stream:
            stream.write(json.dumps({"event": "finished", "at_utc": utc(), "monotonic": time.monotonic(), "status": state["status"]}) + "\n")
        state["AC_lid_monitor"] = pin(monitor_path)
        save(finished_at_utc=utc())
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    print(json.dumps({"launch": str(state_path), "status": state["status"], "failures": state["failures"]}), flush=True)
    return 0 if state["status"] == "completed" else 130 if state["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

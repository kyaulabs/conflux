#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Dry by default: A1 B1 B2 A2, fresh ordinary admission before every leg."""
import argparse
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
DRIVER = BASE / "counterbalanced_runtime.py"
DRIVER_SHA = "f905f12613344dd0c0c68265a0937621910538099cf51bf9d1bf98a80e458fd3"
LEGS = ("A1", "B1", "B2", "A2")
TOTAL_SECONDS, READINESS_SECONDS, LEG_SECONDS = 1800, 620, 250


def require(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--expected-pins", type=Path, default=BASE / "counterbalanced-expected-pins.json")
    parser.add_argument("--source-pins", type=Path, default=BASE / "counterbalanced-source-pins.json")
    parser.add_argument("--readiness-decision", type=Path, default=BASE / "counterbalanced-human-readiness.json")
    args = parser.parse_args()
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", args.name), "Use a fresh plain session label")
    if not args.run:
        print(json.dumps({"schema": 1, "status": "dry_run", "native_started": False, "host_queried": False,
              "order": LEGS, "requests_per_leg": 4, "source_start_frame": 257985,
              "maximum_total_seconds": TOTAL_SECONDS, "maximum_readiness_seconds_per_leg": READINESS_SECONDS,
              "maximum_leg_driver_and_reporting_seconds": LEG_SECONDS,
              "human_readiness_maximum_age_seconds": 900, "expected_pins": str(args.expected_pins),
              "source_pins": str(args.source_pins), "human_readiness": str(args.readiness_decision),
              "scope": "Plan only. Ordinary estimate 6–10 minutes; operational cap 30 minutes with cleanup reserve, not an OS realtime guarantee. Fresh AC/open-lid/minimized human attestation and normal/fresh admission before every leg. No UI/audio/profiler, exclusions, p99 qualification or speed claim."}, indent=2))
        return 0
    began = time.monotonic()
    deadline = began + TOTAL_SECONDS  # Never reset between legs or after imports.
    require(all(path.is_absolute() for path in (args.expected_pins, args.source_pins, args.readiness_decision)),
            "Use absolute pin/readiness paths")
    require(DRIVER.stat().st_size <= 128 * 1024 and hashlib.sha256(DRIVER.read_bytes()).hexdigest() == DRIVER_SHA,
            "Frozen leg driver differs")
    spec = importlib.util.spec_from_file_location("conflux_abba_driver", DRIVER)
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    compare, graph, common, retained = driver.helpers()
    graph.reject_diagnostic_environment()
    config_pin, data = compare.inspect_file(args.expected_pins, 64 * 1024, deadline, keep=True)
    expected = compare.decode_json(data)
    require(expected.get("status") == "ready" and re.fullmatch(r"[0-9a-f]{40}", expected.get("repository_head", ""))
            and expected.get("arms") == {k: v[1] for k, v in driver.ARMS.items()}
            and expected.get("pins", {}).get("model") == compare.MODEL_SHA
            and expected["pins"].get("sampler_source") == driver.SAMPLER_SHA,
            "Arm exact session HEAD/runtime/model/source pins first")
    require(retained.head() == expected["repository_head"], "Expected HEAD differs")
    sources_pin, sources = compare.source_specs(args.source_pins, deadline)
    require(sources_pin["sha256"] == expected.get("source_pins_sha256"), "Source-pin configuration differs")
    def human():
        value = common.human_readiness(args.readiness_decision, args.name)
        require(value["record"].get("codex_minimized") is True, "Fresh human reply must confirm Codex minimized")
        return value
    initial_human = human()
    output = BASE / args.name
    require(not output.exists(), "Use a fresh session label")
    output.mkdir(mode=0o700)
    state_path, monitor_path = output / "session.json", output / "AC-lid-monitor.jsonl"
    state = {"schema": 1, "status": "checking", "started_at_utc": common.utc(), "started_monotonic": began,
             "order": LEGS, "legs": [], "expected_producer": expected, "pins_before": {}, "pins_after": {},
             "human_readiness_initial": initial_human, "failures": [], "native_started": False,
             "native_driver_started": False,
             "maximum_total_seconds": TOTAL_SECONDS, "deadline_monotonic": deadline,
             "UI_load": False, "audio_load": False, "profile_mutation": False, "service_mutation": False,
             "codex_minimized_scope": "Fresh human attestation; window state is not automatically observed.",
             "scope": "Predeclared A1 B1 B2 A2; four same-start music windows per child. No tail exclusions, latency qualification, causal clock/speed inference, thermal pass threshold or host policy changes."}
    child = admission_helper = None
    constructing_child = False
    owned, groups, signatures = {}, {}, {}
    phase, active_leg = "checking", None
    results = []

    def save(**fields):
        state.update(fields, elapsed_seconds=time.monotonic() - began, updated_at_utc=common.utc())
        compare.atomic_json(state_path, state)

    def capture_partial_result(row):
        if not row.get("native_driver_started"):
            return
        row.setdefault("native_started", None)
        path = output / row["leg"] / "result.json"
        row["result_path"] = str(path)
        if path.is_file():
            try:
                pin, data = compare.inspect_file(path, 2 * 1024 * 1024, deadline, keep=True)
                row["result_pin"] = {key: pin[key] for key in ("path", "bytes", "sha256")}
                value = compare.decode_json(data)
                row["reported_native_started"] = value.get("native_started")
                row["reported_result_status"] = value.get("status")
                # An interim checkpoint's false predates native construction.
                if (value.get("native_started") is True
                        or ("cleanup_complete" in value and value.get("status") in ("passed", "incomplete", "interrupted"))):
                    row["native_started"] = value.get("native_started") if type(value.get("native_started")) is bool else None
            except Exception as error:
                row["partial_result_error"] = {"type": type(error).__name__, "message": str(error)}
        flags = [item.get("native_started") for item in state["legs"] if item.get("native_driver_started")]
        state["native_started"] = True if True in flags else None if None in flags else False

    def remember_owned():
        if child is None:
            return
        pending, seen = [child.pid], set()
        while pending and len(seen) < 64:
            pid = pending.pop()
            if pid in seen:
                continue
            seen.add(pid)
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

    def signal_current(signum):
        if child is not None and child.pid in groups:
            try:
                os.killpg(child.pid, signum)  # Independent of leader.poll().
            except ProcessLookupError:
                pass

    def finish_group(group):
        require(group in groups and group > 1 and group != os.getpgrp(), "Unsafe/unknown owned group")
        row = {"group": group, **groups[group], "started_monotonic": time.monotonic(), "disappearance_observed": False}
        state.setdefault("owned_group_cleanup", []).append(row)
        try:
            os.killpg(group, signal.SIGKILL)
            row["kill_sent"] = True
        except ProcessLookupError:
            row["kill_sent"] = False
        until = time.monotonic() + 2
        while True:
            if child is not None and child.pid == group:
                child.poll()
            try:
                os.killpg(group, 0)
            except ProcessLookupError:
                row.update(disappearance_observed=True, finished_monotonic=time.monotonic())
                del groups[group]
                return
            require(time.monotonic() < until, "Owned group remains after bounded cleanup")
            time.sleep(.05)

    def interrupted(signum, _frame):
        state["interruption_signal"] = signum
        if not constructing_child:
            signal_current(signal.SIGINT)
            raise KeyboardInterrupt

    def ac_lid():
        row = {"phase": phase, "leg": active_leg, "monotonic": time.monotonic(), "AC_lid": admission_helper.ac_lid()}
        with monitor_path.open("a") as stream:
            stream.write(json.dumps(row, allow_nan=False) + "\n")
        require(row["AC_lid"]["ready"], "AC/lid changed or observation failed")

    def spawn_wait(command, stdout, stderr, until):
        nonlocal child, constructing_child
        require(time.monotonic() < min(deadline - 30, until), "Session budget exhausted before child launch")
        with stdout.open("x") as out, stderr.open("x") as err:
            constructing_child = True
            try:
                child = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err, start_new_session=True)
                groups[child.pid] = {"phase": phase, "leg": active_leg, "created_monotonic": time.monotonic()}
                if phase == "native_leg":
                    state["native_driver_started"] = True
                    state["legs"][-1].update(native_driver_started=True, native_started=None)
                    if state["native_started"] is not True:
                        state["native_started"] = None
                remember_owned()
            finally:
                constructing_child = False
            if "interruption_signal" in state:
                signal_current(signal.SIGINT)
                raise KeyboardInterrupt
            save(current_command=command, current_group=child.pid)
            next_ac = next_owned = 0
            while child.poll() is None:
                now = time.monotonic()
                require(now < min(deadline - 30, until), "Bounded phase/session budget exhausted")
                if now >= next_owned:
                    remember_owned()
                    next_owned = now + 1
                if now >= next_ac:
                    ac_lid()
                    next_ac = time.monotonic() + 45
                time.sleep(.25)
            code = child.wait()
            finish_group(child.pid)
            return code

    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        for name, path in (("driver", DRIVER), ("launcher", Path(__file__).resolve()),
                           ("config", args.expected_pins), ("source_pins", args.source_pins),
                           ("human_readiness", args.readiness_decision)):
            state["pins_before"][name] = common.pin(path)
            signatures[path] = common.source_signature(path)
        for path in sources:
            signatures[Path(path)] = common.source_signature(Path(path))
        for name in ("admission_source", "readiness_source", "native_replay_source"):
            path, digest = retained.HELPERS[name]
            actual = common.pin(path)
            require(actual["sha256"] == digest, "Frozen admission helper differs: " + name)
            state["pins_before"][name] = actual
            signatures[path] = common.source_signature(path)
        for name in ("worker_source", "shared_benchmark_source", "sampler_source"):
            path, _ = retained.PRODUCERS[name]
            require(common.pin(path)["sha256"] == expected["pins"].get(name), "Readiness import differs: " + name)
        admission_helper = common.load_module("conflux_abba_admission", retained.HELPERS["admission_source"][0])
        admission_helper.readiness_module()
        save()
        for leg in LEGS:
            active_leg, phase = leg, "normal_readiness"
            row = {"leg": leg, "arm": leg[0], "status": phase}
            state["legs"].append(row)
            admission = output / (leg + "-admission")
            admission.mkdir()
            ac_lid()  # Actual AC/lid observation precedes the readiness child.
            command = [sys.executable, "-B", "-c",
                "import runpy,sys;from pathlib import Path;sys.exit(runpy.run_path(sys.argv[1])['run']('admission',Path(sys.argv[2])))",
                str(retained.HELPERS["readiness_source"][0]), str(admission)]
            row["readiness_returncode"] = spawn_wait(command, admission / "stdout", admission / "stderr",
                time.monotonic() + READINESS_SECONDS)
            require(row["readiness_returncode"] == 0, "Normal readiness did not match")
            require(retained.head() == expected["repository_head"]
                    and all(common.source_signature(path) == value for path, value in signatures.items()),
                    "HEAD/source/config changed during session")
            row["fresh_admission"] = admission_helper.fresh_admission(admission)
            row["human_readiness"] = human()  # Original <=900s clock; never renewed automatically.
            require(row["human_readiness"]["pin"] == initial_human["pin"], "Human readiness record changed")
            phase = row["status"] = "native_leg"
            ac_lid()
            command = ["systemd-inhibit", "--what=sleep:idle", "--mode=block", "--who=CONFLUX",
                "--why=Bounded ABBA graph-order diagnostic", sys.executable, "-B", str(DRIVER), "--run", "--leg", leg,
                "--output", str(output / leg), "--source-pins", str(args.source_pins), "--expected-pins", str(args.expected_pins)]
            row["driver_returncode"] = spawn_wait(command, output / (leg + ".stdout"), output / (leg + ".stderr"),
                time.monotonic() + LEG_SECONDS)
            capture_partial_result(row)
            require(row["driver_returncode"] == 0, "Leg driver did not complete")
            value = driver.verify_leg(output / leg / "result.json", leg, compare)
            if leg[0] == "B":
                observed = graph.graph_order_observation(compare, value["runtimes"][0], min(deadline, time.monotonic() + 10))
                require(observed == value.get("graph_order"), "B graph-order stderr changed after driver validation")
            row.update(status="completed", result_pin=common.pin(output / leg / "result.json"),
                       completed_requests=value["completed_requests"], observation_summary=value["observation_summary"])
            row["native_started"] = value["native_started"]
            state["native_started"] = True
            results.append(value)
            ac_lid()
            save()
        require(len(results) == 4 and [value["leg"] for value in results] == list(LEGS), "ABBA order incomplete")
        pairs = {}
        for value in results:
            for request in value["runtimes"][0]["requests"]:
                case = pairs.setdefault(request["id"], {"input_sha256": request["input_sha256"], "output_sha256": request["output_sha256"], "legs": []})
                require((case["input_sha256"], case["output_sha256"]) == (request["input_sha256"], request["output_sha256"]),
                        "Paired PCM hash differs between legs")
                case["legs"].append(value["leg"])
        require(len(pairs) == 4 and all(case["legs"] == list(LEGS) for case in pairs.values()), "Paired four-case coverage incomplete")
        save(status="completed", paired_exact_hashes=pairs, completed_requests=16)
    except KeyboardInterrupt:
        state["status"] = "interrupted"
    except Exception as error:
        state["status"] = "incomplete"
        state["failures"].append({"phase": phase, "leg": active_leg, "type": type(error).__name__, "message": str(error)[:8192]})
    finally:
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        try:
            remember_owned()
            signal_current(signal.SIGINT)
            if child is not None and child.poll() is None:
                try:
                    child.wait(timeout=min(25, max(.1, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    pass
            pending = set(groups) | {entry["process_group"] for entry in live_owned().values()}
            for group in pending:
                groups.setdefault(group, {"phase": "observed_owned_descendant"})
                finish_group(group)
            if child is not None:
                child.wait(timeout=3)
            for _ in range(20):
                if not live_owned():
                    break
                time.sleep(.1)
            require(not groups and not live_owned(), "Owned groups/processes remain after cleanup")
            state["cleanup_complete"] = child is None or child.poll() is not None
        except Exception as error:
            state["status"] = "incomplete"
            state["failures"].append({"phase": "cleanup", "message": str(error)})
            state["cleanup_complete"] = False
        state["cleanup"] = {"owned_identities": owned, "remaining_known_groups": dict(groups), "remaining_observed": live_owned()}
        for row in state["legs"]:
            capture_partial_result(row)
        try:
            for name, record in state["pins_before"].items():
                pin, _ = compare.inspect_file(Path(record["path"]), 128 * 1024, deadline, expected=record["sha256"])
                state["pins_after"][name] = {key: pin[key] for key in ("path", "bytes", "sha256")}
            require(compare.check_deadline(deadline) > 5, "Reporting budget cannot admit final bounded HEAD check")
            state["head_after"] = retained.head()  # Existing five-second command timeout.
            require(state["pins_after"] == state["pins_before"] and state["head_after"] == expected["repository_head"],
                    "HEAD/pins changed during session")
            for path, signature in signatures.items():
                compare.check_deadline(deadline)
                require(common.source_signature(path) == signature, "Source changed during session: " + str(path))
        except Exception as error:
            state["status"] = "incomplete"
            state["failures"].append({"phase": "after_pins", "message": str(error)})
        if time.monotonic() >= deadline:
            state["status"] = "incomplete"
            state["failures"].append({"phase": "reporting", "message": "Operational reporting budget exhausted; cleanup retained priority"})
        save(finished_at_utc=common.utc())
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    print(json.dumps({"status": state["status"], "session": str(state_path), "completed_legs": len(results)}))
    return 0 if state["status"] == "completed" else 130 if state["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Dry-by-default, automatic AC/lid supervision of the numerical comparison."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
DRIVER = BASE / "flash_quality.py"
DRIVER_SHA = "cec1a694a32da562be9ed79d35cfda7e2ee1943b9b7dfd10daf1ed2259fc7495"
TOTAL_SECONDS = 600


def require(value, message):
    if not value:
        raise ValueError(message)


def utc():
    return datetime.now(timezone.utc).isoformat()


def ac_lid():
    row = {"capture_started_monotonic": time.monotonic(), "at_utc": utc(), "errors": []}
    try:
        row["AC"] = Path("/sys/class/power_supply/AC/online").read_text().strip()
        row["lid"] = subprocess.check_output(["busctl", "--system", "get-property",
            "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "LidClosed"],
            text=True, timeout=2).strip()
    except Exception as error:
        row["errors"].append({"type": type(error).__name__, "message": str(error)})
    row.update(ready=not row["errors"] and row.get("AC") == "1" and row.get("lid") == "b false",
               timestamp_monotonic=time.monotonic())
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--expected-pins", type=Path, default=BASE / "flash-quality-expected-pins.json")
    parser.add_argument("--source-pins", type=Path, default=BASE / "flash-quality-source-pins.json")
    args = parser.parse_args()
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", args.name), "Use a fresh plain label")
    if not args.run:
        print(json.dumps({"status": "dry_run", "native_started": False, "host_queried": False,
              "order": ["baseline", "flash"], "requests_per_runtime": 16, "total_seconds": TOTAL_SECONDS,
              "AC_lid_requested_interval_seconds": 5, "human_minimized_attestation_required": False,
              "scope": "Numerical PCM comparison only. No latency/quality pass, playback, UI load or host policy mutation."}, indent=2))
        return 0
    began = time.monotonic()
    deadline = began + TOTAL_SECONDS
    require(args.expected_pins.is_absolute() and args.source_pins.is_absolute(), "Absolute configuration paths required")
    require(DRIVER.stat().st_size <= 128 * 1024 and hashlib.sha256(DRIVER.read_bytes()).hexdigest() == DRIVER_SHA,
            "Frozen numerical driver differs")
    spec = importlib.util.spec_from_file_location("conflux_flash_quality_driver", DRIVER)
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    compare = driver.helper()
    driver.reject_environment()
    config, _ = driver.configuration(compare, args.expected_pins, args.source_pins, deadline - 60)
    require(driver.head(compare, deadline - 60) == config["repository_head"], "Source HEAD differs")
    output = BASE / args.name
    output.mkdir(mode=0o700)
    state_path = output / "session.json"
    state = {"schema": 1, "status": "checking", "started_at_utc": utc(), "started_monotonic": began,
             "deadline_monotonic": deadline, "maximum_total_seconds": TOTAL_SECONDS,
             "driver_started": False, "native_started": False, "errors": [], "pins_before": {}, "pins_after": {},
             "profile_mutation": False, "service_mutation": False, "UI_load": False, "audio_load": False,
             "musical_quality": "not_assessed", "latency_qualification": "not_attempted",
             "scope": "Parent-owned workspace spool and owned process group. Actual AC/lid checks before/through/after; no minimized-window or thermal admission gate for numerical math."}
    child, temporary, spool, constructing, group = None, None, None, False, None
    owned = {}
    def save():
        state.update(elapsed_seconds=time.monotonic() - began, updated_at_utc=utc())
        compare.atomic_json(state_path, state)
    def monitor():
        row = ac_lid()
        with (output / "AC-lid-monitor.jsonl").open("a") as stream:
            stream.write(json.dumps(row, allow_nan=False) + "\n")
        require(row["ready"], "AC/lid changed or observation failed")
    def remember():
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
                identity = {"start_ticks": int(fields[19]), "group": int(fields[2])}
                require(pid not in owned or owned[pid] == identity, "Owned PID identity changed")
                owned[pid] = identity
                pending.extend(map(int, (proc / "task" / str(pid) / "children").read_text().split()))
            except FileNotFoundError:
                pass
    def remaining():
        result = {}
        for pid, identity in owned.items():
            try:
                fields = (Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()
                if int(fields[19]) == identity["start_ticks"] and int(fields[2]) == identity["group"]:
                    result[pid] = dict(identity, state=fields[0])
            except FileNotFoundError:
                pass
        return result
    def signal_group(sig):
        if group is not None:
            require(group > 1 and group != os.getpgrp(), "Unsafe owned process group")
            try:
                os.killpg(group, sig)
            except ProcessLookupError:
                pass
    def harvest():
        if not state["driver_started"]:
            return
        path = output / "comparison/result.json"
        if path.is_file():
            try:
                pin, contents = compare.inspect_file(path, 8 * 1024 * 1024, deadline, keep=True)
                value = compare.decode_json(contents)
                state.update(result_pin=pin, reported_status=value.get("status"),
                             completed_requests=value.get("completed_requests"))
                if value.get("native_started") is True or "cleanup_complete" in value:
                    flag = value.get("native_started")
                    if state["native_started"] is not True:
                        state["native_started"] = flag if type(flag) is bool else None
                return value
            except Exception as error:
                state["errors"].append({"phase": "partial_result", "message": str(error)})
        return None
    def interrupted(signum, _frame):
        state["interruption_signal"] = signum
        if not constructing:
            signal_group(signal.SIGINT)
            raise KeyboardInterrupt
    handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        for name, path, maximum in (("driver", DRIVER, 128 * 1024), ("launcher", Path(__file__).resolve(), 128 * 1024),
                                    ("config", args.expected_pins, 64 * 1024), ("sources", args.source_pins, 64 * 1024)):
            state["pins_before"][name] = compare.inspect_file(path, maximum, deadline - 60)[0]
        monitor()
        driver.SPOOL_PARENT.mkdir(mode=0o700, exist_ok=True)
        info = driver.SPOOL_PARENT.lstat()
        require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700,
                "Workspace native-temp must be owned mode0700")
        space = os.statvfs(driver.SPOOL_PARENT)
        require(space.f_bavail * space.f_frsize >= driver.BASELINE_BYTES + 64 * 1024 * 1024,
                "Insufficient workspace space for bounded baseline/native spool")
        temporary = tempfile.TemporaryDirectory(prefix="flash-quality-", dir=driver.SPOOL_PARENT)
        spool = Path(temporary.name)
        (spool / "baseline").mkdir(mode=0o700)
        (spool / "native").mkdir(mode=0o700)
        state.update(spool=str(spool), baseline_spool_expected_bytes=driver.BASELINE_BYTES)
        environment = os.environ.copy()
        environment["TMPDIR"] = str(spool / "native")
        command = ["systemd-inhibit", "--what=sleep:idle", "--mode=block", "--who=CONFLUX",
            "--why=Bounded native numerical PCM comparison", sys.executable, "-B", str(DRIVER), "--run",
            "--output", str(output / "comparison"), "--spool", str(spool),
            "--expected-pins", str(args.expected_pins), "--source-pins", str(args.source_pins),
            "--deadline-monotonic", str(deadline)]
        require(time.monotonic() < deadline - 60, "Setup exhausted work budget")
        with (output / "driver.stdout").open("x") as stdout, (output / "driver.stderr").open("x") as stderr:
            constructing = True
            try:
                child = subprocess.Popen(command, cwd=ROOT, stdout=stdout, stderr=stderr,
                                         env=environment, start_new_session=True)
                group = child.pid
                state.update(driver_started=True, native_started=None, command=command, owned_group=group)
                remember()
            finally:
                constructing = False
            if "interruption_signal" in state:
                raise KeyboardInterrupt
            save()
            next_monitor = next_owned = 0
            while child.poll() is None:
                now = time.monotonic()
                require(now < deadline - 30, "Numerical comparison operational budget exhausted")
                if now >= next_monitor:
                    monitor()
                    next_monitor = time.monotonic() + 5
                if now >= next_owned:
                    remember()
                    next_owned = now + 1
                time.sleep(.25)
            state["driver_returncode"] = child.wait()
        value = harvest()
        require(state["driver_returncode"] == 0 and value is not None
                and value.get("status") == "comparison_complete" and value.get("completed_requests") == 32
                and value.get("cleanup_complete") is True and value.get("pins_unchanged") is True,
                "Numerical comparison is incomplete; partial evidence retained")
        monitor()
        state["status"] = "comparison_complete"
    except KeyboardInterrupt:
        state["status"] = "cancelled"
    except Exception as error:
        state["status"] = "incomplete"
        state["errors"].append({"type": type(error).__name__, "message": str(error)[:8192]})
    finally:
        for sig in handlers:
            signal.signal(sig, signal.SIG_IGN)
        try:
            remember()
            signal_group(signal.SIGINT)
            if child is not None and child.poll() is None:
                try:
                    child.wait(timeout=min(25, max(.1, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    pass
            signal_group(signal.SIGKILL)
            until = time.monotonic() + 2
            while group is not None:
                child.poll()
                try:
                    os.killpg(group, 0)
                except ProcessLookupError:
                    break
                require(time.monotonic() < until, "Owned group remains after bounded cleanup")
                time.sleep(.05)
            if child is not None:
                child.wait(timeout=2)
            require(not remaining(), "Owned processes remain after cleanup")
            state["process_cleanup_complete"] = True
        except Exception as error:
            state["process_cleanup_complete"] = False
            state["errors"].append({"phase": "process_cleanup", "message": str(error)})
        harvest()
        state["cleanup"] = {"owned_identities": owned, "remaining_observed": remaining()}
        try:
            if temporary is not None:
                temporary.cleanup()
            state["spool_removed"] = spool is None or not spool.exists()
        except Exception as error:
            state["spool_removed"] = False
            state["errors"].append({"phase": "spool_cleanup", "message": str(error)})
        state["cleanup_complete"] = state.get("process_cleanup_complete", False) and state["spool_removed"]
        try:
            for name, record in state["pins_before"].items():
                state["pins_after"][name] = compare.inspect_file(Path(record["path"]), 128 * 1024, deadline,
                                                                 expected=record["sha256"])[0]
            require(state["pins_before"] == state["pins_after"] and driver.head(compare, deadline) == config["repository_head"],
                    "Final source/config/HEAD pins changed")
            compare.check_deadline(deadline)
        except Exception as error:
            state["errors"].append({"phase": "after_pins", "message": str(error)})
        if state["status"] == "comparison_complete" and (state["errors"] or not state["cleanup_complete"]):
            state["status"] = "incomplete"
        state["finished_at_utc"] = utc()
        save()
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    print(json.dumps({"status": state["status"], "session": str(state_path),
                      "completed_requests": state.get("completed_requests")}, allow_nan=False))
    return 0 if state["status"] == "comparison_complete" else 2 if state["status"] == "cancelled" else 1


if __name__ == "__main__":
    raise SystemExit(main())

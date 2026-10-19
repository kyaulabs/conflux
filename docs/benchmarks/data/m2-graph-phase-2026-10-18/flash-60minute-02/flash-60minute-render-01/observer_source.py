#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Scoped observer snapshot: preserve policy observation and publish owned children safely."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def policy():
    return {
        "observed_at_utc": datetime.now(timezone.utc).isoformat(),
        "profile": subprocess.check_output(["powerprofilesctl", "get"], text=True, timeout=2).strip(),
        "holds": subprocess.check_output(["powerprofilesctl", "list-holds"], text=True, timeout=2).strip(),
        "firmware": Path("/sys/firmware/acpi/platform_profile").read_text().strip(),
        "cpu_epp": Path("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference").read_text().strip(),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lifecycle", type=Path)
    parser.add_argument("benchmark_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if not args.benchmark_args:
        parser.error("supply arguments for tools/benchmarks/stem_soak.py")
    command = ["python3", "-B", "tools/benchmarks/stem_soak.py", *args.benchmark_args]
    record = {"command": command, "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "mode": "observe", "profile_mutation": False, "restored_by_runner": False}
    record_path = args.lifecycle
    # Prove the lifecycle path is writable before launching the owned trial.
    record_path.write_text(json.dumps(record, indent=2) + "\n")
    child = None
    child_group = None
    constructing_child = False
    def interrupted(signum, _frame):
        record["interruption_signal"] = signum
        if not constructing_child:
            raise KeyboardInterrupt
    previous_handlers = {sig: signal.signal(sig, interrupted)
                         for sig in (signal.SIGINT, signal.SIGTERM)}
    exit_code = 1
    try:
        record["before"] = policy()
        if record["before"]["holds"]:
            raise RuntimeError("existing power holds prevent this comparison")
        if record["before"]["profile"] != "balanced":
            raise RuntimeError("balanced readiness was not observed before launch")
        record["during"] = policy()
        if record["during"]["holds"]:
            raise RuntimeError("a power hold appeared before launch")
        record_path.write_text(json.dumps(record, indent=2) + "\n")
        print(json.dumps(record["during"]), flush=True)
        constructing_child = True
        try:
            child = subprocess.Popen(command, start_new_session=True)
            child_group = child.pid
            record["owned_child_group"] = child_group
        finally:
            constructing_child = False
        if "interruption_signal" in record:
            raise KeyboardInterrupt
        exit_code = child.wait()
        if exit_code < 0:
            exit_code = 128 - exit_code
    except KeyboardInterrupt:
        record["interrupted"] = True
        exit_code = 130
    except Exception as error:
        record["error"] = f"{type(error).__name__}: {error}"
    finally:
        # A repeated terminal signal must not skip child cleanup or final observations.
        for sig in previous_handlers:
            signal.signal(sig, signal.SIG_IGN)
        try:
            try:
                if child_group is not None:
                    if child_group <= 1 or child_group == os.getpgrp():
                        raise RuntimeError("Refusing unsafe owned observer child group")
                    cleanup = record["child_group_cleanup"] = {"group": child_group,
                        "started_monotonic": time.monotonic(), "disappearance_observed": False}
                    try:
                        os.killpg(child_group, signal.SIGINT)
                    except ProcessLookupError:
                        pass
                    if child.poll() is None:
                        try:
                            child.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            pass
                    # Retain the known new-session group after its leader exits.
                    try:
                        os.killpg(child_group, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    until = time.monotonic() + 2
                    while True:
                        child.poll()
                        try:
                            os.killpg(child_group, 0)
                        except ProcessLookupError:
                            cleanup.update(disappearance_observed=True, finished_monotonic=time.monotonic())
                            break
                        if time.monotonic() >= until:
                            raise RuntimeError("Owned observer child group remains after bounded cleanup")
                        time.sleep(.05)
                    child.wait(timeout=2)
            except Exception as error:
                record["child_cleanup_error"] = f"{type(error).__name__}: {error}"
                exit_code = 1
            try:
                record["before_restore"] = policy()
            except Exception as error:
                record["before_restore_policy_error"] = f"{type(error).__name__}: {error}"
                exit_code = 1
            try:
                record["after"] = policy()
            except Exception as error:
                record["after_policy_error"] = f"{type(error).__name__}: {error}"
                exit_code = 1
            record["benchmark_returncode"] = child.returncode if child is not None else None
            record["runner_returncode"] = exit_code
            record["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
            record_path.write_text(json.dumps(record, indent=2) + "\n")
            print(json.dumps({"observed_policy": record.get("after"), "runner_returncode": exit_code}), flush=True)
        finally:
            for sig, previous in previous_handlers.items():
                signal.signal(sig, previous)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())

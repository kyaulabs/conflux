#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Run one M0 benchmark under balanced, then restore the prior power profile."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess


def policy():
    return {
        "observed_at_utc": datetime.now(timezone.utc).isoformat(),
        "profile": subprocess.check_output(["powerprofilesctl", "get"], text=True, timeout=2).strip(),
        "holds": subprocess.check_output(["powerprofilesctl", "list-holds"], text=True, timeout=2).strip(),
        "firmware": Path("/sys/firmware/acpi/platform_profile").read_text().strip(),
        "cpu_epp": Path("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference").read_text().strip(),
    }


def interrupted(_signum, _frame):
    raise KeyboardInterrupt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lifecycle", type=Path)
    parser.add_argument("benchmark_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if not args.benchmark_args:
        parser.error("supply arguments for tools/benchmarks/native_replay.py")
    command = ["python3", "-B", "tools/benchmarks/native_replay.py", *args.benchmark_args]
    record = {"command": command, "started_at_utc": datetime.now(timezone.utc).isoformat()}
    record_path = args.lifecycle
    # Prove the lifecycle path is writable before changing any machine policy.
    record_path.write_text(json.dumps(record, indent=2) + "\n")
    previous_handlers = {sig: signal.signal(sig, interrupted)
                         for sig in (signal.SIGINT, signal.SIGTERM)}
    child = None
    attempted_set = False
    exit_code = 1
    try:
        record["before"] = policy()
        if record["before"]["holds"]:
            raise RuntimeError("existing power holds prevent this comparison")
        attempted_set = True
        subprocess.run(["powerprofilesctl", "set", "balanced"], check=True, timeout=2)
        record["during"] = policy()
        if record["during"]["profile"] != "balanced":
            raise RuntimeError("balanced profile did not activate")
        record_path.write_text(json.dumps(record, indent=2) + "\n")
        print(json.dumps(record["during"]), flush=True)
        child = subprocess.Popen(command, start_new_session=True)
        exit_code = child.wait()
        if exit_code < 0:
            exit_code = 128 - exit_code
    except KeyboardInterrupt:
        record["interrupted"] = True
        exit_code = 130
    except Exception as error:
        record["error"] = f"{type(error).__name__}: {error}"
    finally:
        # A repeated terminal signal must not skip child cleanup or restoration.
        for sig in previous_handlers:
            signal.signal(sig, signal.SIG_IGN)
        try:
            try:
                if child is not None and child.poll() is None:
                    try:
                        os.killpg(child.pid, signal.SIGINT)
                    except ProcessLookupError:
                        pass
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        try:
                            os.killpg(child.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        child.wait(timeout=2)
            except Exception as error:
                record["child_cleanup_error"] = f"{type(error).__name__}: {error}"
                exit_code = 1
            try:
                record["before_restore"] = policy()
                if attempted_set and record["before_restore"]["profile"] == "balanced":
                    subprocess.run(["powerprofilesctl", "set", record["before"]["profile"]], check=True, timeout=2)
                    record["restored_by_runner"] = True
                else:
                    record["restored_by_runner"] = False
                    if attempted_set:
                        record["preserved_intervening_profile"] = record["before_restore"]["profile"]
            except Exception as error:
                record["restore_error"] = f"{type(error).__name__}: {error}"
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
            print(json.dumps({"restored_policy": record.get("after"), "runner_returncode": exit_code}), flush=True)
        finally:
            for sig, previous in previous_handlers.items():
                signal.signal(sig, previous)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())

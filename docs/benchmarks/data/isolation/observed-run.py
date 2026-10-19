#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Observe host policy while running one M0 benchmark; never change a profile."""

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
        parser.error("supply arguments for tools/benchmarks/stem_soak.py")
    command = ["python3", "-B", "tools/benchmarks/stem_soak.py", *args.benchmark_args]
    record = {"command": command, "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "mode": "observe", "profile_mutation": False, "restored_by_runner": False}
    record_path = args.lifecycle
    # Prove the lifecycle path is writable before launching the owned trial.
    record_path.write_text(json.dumps(record, indent=2) + "\n")
    previous_handlers = {sig: signal.signal(sig, interrupted)
                         for sig in (signal.SIGINT, signal.SIGTERM)}
    child = None
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
        # A repeated terminal signal must not skip child cleanup or final observations.
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

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Read bounded per-thread CPU/scheduling and DRM counters for one owned UI."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import time


def process_stat(path):
    raw = path.read_text()
    comm, fields = raw[raw.index("(") + 1:].rsplit(")", 1)
    fields = fields.split()
    return {"name": comm, "state": fields[0], "minor_faults": int(fields[7]),
            "major_faults": int(fields[9]), "user_ticks": int(fields[11]),
            "system_ticks": int(fields[12]), "start_ticks": int(fields[19]),
            "processor": int(fields[36])}


def ui_identity(pid, run, executable):
    directory = Path("/proc") / str(pid)
    if directory.stat().st_uid != os.getuid():
        raise RuntimeError("UI PID is not owned by the current user")
    command = (directory / "cmdline").read_bytes().rstrip(b"\0").split(b"\0")
    command = [os.fsdecode(value) for value in command]
    if (not command or Path(command[0]).resolve() != executable
            or "--smoke-test" not in command):
        raise RuntimeError("UI PID command is not the expected smoke-test executable")
    try:
        progress_path = command[command.index("--smoke-progress-file") + 1]
    except (ValueError, IndexError) as error:
        raise RuntimeError("UI PID command has no smoke progress path") from error
    if Path(progress_path).resolve() != run / "ui-progress.json":
        raise RuntimeError("UI PID command belongs to a different run")
    return process_stat(directory / "stat")["start_ticks"]


def snapshot(pid, expected_start):
    directory = Path("/proc") / str(pid)
    process = process_stat(directory / "stat")
    if process["start_ticks"] != expected_start:
        raise RuntimeError("UI PID was reused")
    threads, task_errors = [], {}
    for task in sorted((directory / "task").iterdir(), key=lambda path: int(path.name)):
        row = {"tid": int(task.name)}
        try:
            row.update(process_stat(task / "stat"))
        except (OSError, ValueError, IndexError) as error:
            key = f"stat:{type(error).__name__}"
            task_errors[key] = task_errors.get(key, 0) + 1
            continue
        try:
            runtime, wait, slices = map(int, (task / "schedstat").read_text().split())
            row["schedstat"] = {"runtime_ns": runtime, "runqueue_wait_ns": wait,
                                "timeslices": slices}
        except (OSError, ValueError) as error:
            row["schedstat_error"] = type(error).__name__
        threads.append(row)
    clients, fd_errors = {}, {}
    for fd in (directory / "fdinfo").iterdir():
        try:
            fields = {}
            for line in fd.read_text().splitlines():
                key, _, value = line.partition(":")
                if key.startswith("drm-"):
                    fields[key] = value.strip()
            if "drm-client-id" in fields:
                identity = (fields.get("drm-pdev", fields.get("drm-driver", "unknown")),
                            fields["drm-client-id"])
                clients.setdefault(identity, fields)
        except (OSError, ValueError) as error:
            key = type(error).__name__
            fd_errors[key] = fd_errors.get(key, 0) + 1
    # Recheck after reading tasks/fds so a PID exit/reuse cannot appear as a
    # complete sample. Individual task/fd exits remain annotated races.
    if process_stat(directory / "stat")["start_ticks"] != expected_start:
        raise RuntimeError("UI PID identity changed during the sample")
    return {"pid": pid, "process": process, "threads": threads,
            "task_read_errors": task_errors, "drm_clients": list(clients.values()),
            "fd_read_errors": fd_errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--seconds", type=int, default=900)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--ui", type=Path,
                        default=Path(__file__).resolve().parents[2] / "build/dev/conflux")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 900:
        parser.error("--seconds must be between 1 and 900")
    run, executable = args.run.resolve(), args.ui.resolve()
    output = args.output or run.with_name(run.name + "-ui-process.jsonl")
    started = time.monotonic()
    pid, identity, samples, errors = None, None, 0, {}
    stop_signal = None

    def stop(signum, _frame):
        nonlocal stop_signal
        stop_signal = signum

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    status, code = "deadline_reached", 124
    with output.open("x") as stream:
        def emit(value):
            stream.write(json.dumps({"timestamp_monotonic": time.monotonic(), **value}) + "\n")
            stream.flush()

        try:
            schedstats = Path("/proc/sys/kernel/sched_schedstats").read_text().strip()
        except OSError:
            schedstats = None
        emit({"event": "started", "utc": datetime.now(timezone.utc).isoformat(),
              "run": str(run), "ui": str(executable), "maximum_seconds": args.seconds,
              "interval_seconds": 1, "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
              "sched_schedstats": schedstats,
              "scope": "Owned UI process only; cumulative /proc thread and deduplicated DRM counters, not GPU kernel or physical presentation timings"})
        due = started
        while time.monotonic() - started < args.seconds:
            if stop_signal is not None:
                status, code = "interrupted", 128 + stop_signal
                break
            if (run / "result.json").exists():
                status, code = "result_present", 0
                break
            try:
                pulse = json.loads((run / "ui-progress.json").read_text())
                observed_pid = pulse.get("pid")
                if isinstance(observed_pid, bool) or not isinstance(observed_pid, int) or observed_pid <= 0:
                    raise ValueError("invalid heartbeat PID")
                if pid is None:
                    identity = ui_identity(observed_pid, run, executable)
                    pid = observed_pid
                    emit({"event": "identified", "pid": pid, "start_ticks": identity})
                elif observed_pid != pid:
                    raise RuntimeError("heartbeat PID changed")
                row = snapshot(pid, identity)
                row["heartbeat"] = {key: pulse.get(key) for key in
                                    ("timestamp_monotonic_ns", "frames_swapped",
                                     "last_frame_monotonic_ns", "exposed", "scene_graph_initialized")}
                emit({"event": "sample", **row})
                samples += 1
            except RuntimeError as error:
                emit({"event": "identity_error", "error": str(error)})
                status, code = "identity_error", 1
                break
            except (OSError, ValueError, TypeError, IndexError, AttributeError) as error:
                label = type(error).__name__
                errors[label] = errors.get(label, 0) + 1
                emit({"event": "read_error", "error": label, "identified_pid": pid})
            due += 1
            time.sleep(max(0, min(1, due - time.monotonic())))
        emit({"event": "finished", "status": status, "returncode": code,
              "samples": samples, "read_errors": errors, "elapsed_seconds": time.monotonic() - started})
    return code


if __name__ == "__main__":
    raise SystemExit(main())

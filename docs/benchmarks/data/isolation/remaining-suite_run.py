# SPDX-License-Identifier: AGPL-3.0-or-later
"""Sequential, finite isolated trials; retain failures and stop on unsafe cleanup."""
from datetime import datetime, timezone
import json
from pathlib import Path
import signal
import subprocess
import sys

base = Path(__file__).resolve().parent
state_path = base / "suite-state.json"
child = None


def state(**fields):
    record = json.loads(state_path.read_text())
    record.update(fields, updated_at_utc=datetime.now(timezone.utc).isoformat())
    state_path.write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(fields), flush=True)


def stop(signum, _frame):
    if child is not None and child.poll() is None:
        child.send_signal(signal.SIGINT)
    raise KeyboardInterrupt


for sig in (signal.SIGINT, signal.SIGTERM):
    signal.signal(sig, stop)


def run(arguments):
    global child
    child = subprocess.Popen([sys.executable, "-B", *map(str, arguments)])
    code = child.wait()
    child = None
    return code


try:
    trials = (("none-hour-rerun", "none"), ("audio-hour", "audio"),
              ("both-hour", "both"), ("ui-hour", "ui"))
    if sys.argv[1:] == ["--remaining"]:
        trials = (("both-hour-rerun", "both"), ("ui-hour", "ui"))
    elif sys.argv[1:]:
        raise ValueError("expected no arguments or --remaining")
    for name, mode in trials:
        state(phase="cooldown", active_run=name, active_mode=mode)
        if run([base / "cooldown.py", name]):
            raise RuntimeError("cooldown failed")
        state(phase="measurement", active_run=name, active_mode=mode)
        code = run([base / "launch.py", name, mode])
        result = json.loads((base / name / "result.json").read_text())
        guard = json.loads((base / (name + "-guard.json")).read_text())
        policy = json.loads((base / (name + "-policy.json")).read_text())
        pids = [row["runtime"]["pid"] for row in result["runtime_ready"]]
        pids += [row["pid"] for row in result["supervisor_memory_cpu"]["processes"]]
        ui_pid = result.get("ui_coverage", {}).get("progress", {}).get("last_progress", {}).get("pid")
        if ui_pid is not None:
            pids.append(ui_pid)
        cleanup = {"native_and_probe_PIDs": pids,
                   "PID_exists": {str(pid): Path(f"/proc/{pid}").exists() for pid in pids},
                   "worker_closed": result["worker_statistics"]["closed"],
                   "probe_returncodes": {key: row["returncode"] for key, row in result["load"]["children"].items()},
                   "fixture_exists": (base / name / "source.wav").exists(),
                   "idle_inhibit_released": guard.get("idle_inhibit_released"),
                   "guard_errors": guard["errors"],
                   "policy_lifecycle": policy}
        (base / (name + "-cleanup.json")).write_text(json.dumps(cleanup, indent=2) + "\n")
        if (any(cleanup["PID_exists"].values()) or not cleanup["worker_closed"]
                or cleanup["fixture_exists"] or not cleanup["idle_inhibit_released"]
                or cleanup["guard_errors"] or policy.get("errors")):
            raise RuntimeError("trial cleanup requires investigation")
        if run([base / "capture.py", "after-" + name]):
            raise RuntimeError("post-trial host capture failed")
        if run([base / "kernel_capture.py", name]):
            raise RuntimeError("post-trial kernel capture failed")
        record = json.loads(state_path.read_text())
        attempted = record.get("sustained_attempts", [])
        attempted.append({"run": name, "mode": mode, "returncode": code,
                          "status": result["status"], "seconds": result["workload_seconds"],
                          "failures": result["failures"]})
        state(sustained_attempts=attempted, phase="between trials", active_run=None)
    state(phase="matrix attempted; analysis pending", active_run=None)
except BaseException as error:
    state(phase="suite stopped; investigation required", suite_error=str(error))
    raise

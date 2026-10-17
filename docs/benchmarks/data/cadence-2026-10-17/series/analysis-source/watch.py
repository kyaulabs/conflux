# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded 45-second AC/lid and small-progress observer; no policy changes."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import time

base = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("name")
parser.add_argument("--maximum-seconds", type=int, default=5400)
args = parser.parse_args()
if not 60 <= args.maximum_seconds <= 10800:
    parser.error("--maximum-seconds must be 60..10800")
name = args.name
started = time.monotonic()
stop_reason = "deadline_reached"
code = 124
with (base / (name + "-AC-lid-monitor.jsonl")).open("x") as log:
    while time.monotonic() - started < args.maximum_seconds:
        row = {"event": "snapshot", "at_utc": datetime.now(timezone.utc).isoformat(),
               "monotonic": time.monotonic(), "errors": []}
        try:
            row["mains_online"] = Path("/sys/class/power_supply/AC/online").read_text().strip()
        except OSError as error:
            row["mains_online"] = None
            row["errors"].append(f"AC: {type(error).__name__}: {error}")
        try:
            command = ["busctl", "--system", "get-property", "org.freedesktop.login1",
                       "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "LidClosed"]
            value = subprocess.check_output(command, text=True, timeout=2).strip()
            if value not in ("b false", "b true"):
                raise ValueError("unexpected login1 LidClosed value")
            row["lid_closed"] = value == "b true"
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            row["lid_closed"] = None
            row["errors"].append(f"LidClosed: {type(error).__name__}: {error}")
        row["conditions_ready"] = (not row["errors"] and row["mains_online"] == "1"
                                   and row["lid_closed"] is False)
        row["finished_monotonic"] = time.monotonic()
        log.write(json.dumps(row) + "\n")
        log.flush()
        print(json.dumps({"run": name, "AC": row["mains_online"],
                          "lid_closed": row["lid_closed"],
                          "conditions_ready": row["conditions_ready"],
                          "observation_errors": row["errors"]}), flush=True)
        if (base / name / "progress.json").exists():
            subprocess.run([sys.executable, "-B", str(base / "monitor.py"), name],
                           check=True, timeout=12)
        else:
            admission_path = base / (name + "-admission.json")
            admission = json.loads(admission_path.read_text()) if admission_path.exists() else {}
            sequence_path = base / "hour-sequence.json"
            sequence = json.loads(sequence_path.read_text()) if sequence_path.exists() else {}
            print(json.dumps({"run": name, "stage": admission.get("stage", sequence.get("stage", "waiting"))}),
                  flush=True)
        if (base / name / "result.json").exists():
            stop_reason, code = "result_present", 0
            break
        launch_path = base / (name + "-launch.json")
        if launch_path.exists():
            launch = json.loads(launch_path.read_text())
            if launch.get("finished_at_utc"):
                stop_reason, code = "launcher_finished", 0
                break
        admission_path = base / (name + "-admission.json")
        if admission_path.exists() and json.loads(admission_path.read_text()).get("status") == "error":
            stop_reason, code = "admission_stopped", 0
            break
        if (base / "cadence-series-stop").exists():
            stop_reason, code = "series_stopped", 0
            break
        time.sleep(min(45, max(0, started + args.maximum_seconds - time.monotonic())))
    log.write(json.dumps({"event": "finished", "at_utc": datetime.now(timezone.utc).isoformat(),
                          "monotonic": time.monotonic(), "reason": stop_reason,
                          "returncode": code, "maximum_seconds": args.maximum_seconds}) + "\n")
    log.flush()
print(json.dumps({"run": name, "monitor_stopped": stop_reason, "returncode": code}), flush=True)
raise SystemExit(code)

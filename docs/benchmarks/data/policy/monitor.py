# SPDX-License-Identifier: AGPL-3.0-or-later
"""Read one progress checkpoint and the final complete host row, never scan traces."""
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import time

began = time.monotonic()
root = Path(__file__).resolve().parent
run = root / sys.argv[1]
record = {"observed_at_utc": datetime.now(timezone.utc).isoformat(), "run": sys.argv[1]}
for key, command in (("profile", ["powerprofilesctl", "get"]),
                     ("holds", ["powerprofilesctl", "list-holds"])):
    result = subprocess.run(command, capture_output=True, text=True)
    record[key] = result.stdout.strip() if result.returncode == 0 else {"error": result.stderr.strip()}
if (run / "progress.json").exists():
    record["progress"] = json.loads((run / "progress.json").read_text())
if (run / "ui-progress.json").exists():
    record["ui_pulse"] = json.loads((run / "ui-progress.json").read_text())
    record["ui_frame_age_seconds"] = time.monotonic() - record["ui_pulse"]["last_frame_monotonic_ns"] / 1e9
guard = root / (sys.argv[1] + "-guard.json")
if guard.exists():
    record["window_guard"] = json.loads(guard.read_text())
trace = run / "native-trace.jsonl"
if trace.exists():
    with trace.open("rb") as stream:
        stream.seek(max(0, trace.stat().st_size - 65536))
        tail = stream.read()
    lines = tail.split(b"\n")
    for line in reversed(lines[:-1]):
        try:
            row = json.loads(line)
            break
        except ValueError:
            continue
    else:
        row = {}
    record["latest_native"] = {key: row.get(key) for key in (
        "timestamp_monotonic", "pid", "process_status_kB", "process_counters",
        "capture_elapsed_ms", "previous_sample_gap_ms", "system_values")}
record["monitor_wall_ms"] = (time.monotonic() - began) * 1000
with (root / "monitor.jsonl").open("a") as stream:
    stream.write(json.dumps(record) + "\n")
progress = record.get("progress", {})
system = record.get("latest_native", {}).get("system_values") or {}
print(json.dumps({"at": record["observed_at_utc"], "run": record["run"], "profile": record["profile"],
                  "hold_active": bool(record["holds"]), "status": progress.get("status"),
                  "seconds": progress.get("elapsed_workload_seconds"), "timing": progress.get("timing"),
                  "controls": progress.get("completed_controls"), "failures": progress.get("failures"),
                  "ui_pulse": record.get("ui_pulse"),
                  "ui_frame_age_seconds": record.get("ui_frame_age_seconds"),
                  "ui_guard_ready": record.get("window_guard", {}).get("ready"),
                  "native": record.get("latest_native", {}).get("process_status_kB"),
                  "host": {key: value for key, value in system.items() if
                           key.endswith('/temp') or 'act_freq' in key or 'cur_freq_mhz' in key
                           or key.endswith('MemAvailable') or key.endswith('some.avg10')
                           or key.endswith('available_bytes')},
                  "monitor_wall_ms": record["monitor_wall_ms"]}, indent=2))

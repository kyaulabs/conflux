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
    result = subprocess.run(command, capture_output=True, text=True, timeout=2)
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
timing = progress.get("timing", {})
print(json.dumps({"run": record["run"], "minutes": round(progress.get("elapsed_workload_seconds", 0) / 60, 2),
                  "status": progress.get("status"), "chunks": timing.get("chunks"),
                  "p99_ms": timing.get("hop_service_percentiles_ms", {}).get("99"),
                  "max_ms": timing.get("hop_service_max_ms"), "misses": timing.get("hops_above_target"),
                  "controls": progress.get("completed_controls"), "failures": progress.get("failures"),
                  "profile": record["profile"], "holds": bool(record["holds"]),
                  "guard_ready": record.get("window_guard", {}).get("ready"),
                  "UI_age_s": record.get("ui_frame_age_seconds"),
                  "package_C": (system.get('/sys/class/thermal/thermal_zone7/temp', 0) / 1000)
                               if '/sys/class/thermal/thermal_zone7/temp' in system else None,
                  "GPU_MHz": system.get('/sys/class/drm/card1/gt_act_freq_mhz'),
                  "native_RSS_kB": record.get("latest_native", {}).get("process_status_kB", {}).get("VmRSS")}))

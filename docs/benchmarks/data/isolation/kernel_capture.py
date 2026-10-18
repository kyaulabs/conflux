# SPDX-License-Identifier: AGPL-3.0-or-later
"""Retain bounded kernel messages matching graphics/fence terms after a trial."""
import json
from pathlib import Path
import re
import subprocess
import sys

base = Path(__file__).resolve().parent
name = sys.argv[1]
launch = json.loads((base / (name + "-launch.json")).read_text())
command = ["journalctl", "--dmesg", "--since", launch["started_at_utc"],
           "--until", launch["finished_at_utc"], "--output=json", "--no-pager"]
result = subprocess.run(command, capture_output=True, text=True, timeout=15)
pattern = re.compile(r"i915|drm|fence|GPU|hang|reset", re.IGNORECASE)
rows, errors, examined = [], [], 0
for line in result.stdout.splitlines():
    try:
        row = json.loads(line)
        examined += 1
        if pattern.search(str(row.get("MESSAGE", ""))):
            rows.append({key: row[key] for key in ("MESSAGE", "PRIORITY", "__REALTIME_TIMESTAMP",
                         "__MONOTONIC_TIMESTAMP", "_SOURCE_MONOTONIC_TIMESTAMP",
                         "_SOURCE_BOOTTIME_TIMESTAMP") if key in row})
    except ValueError as error:
        errors.append(str(error))
record = {"command": command, "returncode": result.returncode, "stderr": result.stderr,
          "examined_entries": examined, "matched_entries": rows, "parse_errors": errors,
          "scope": "Host kernel journal over launch-to-wrapper-exit bounds, filtered by i915/drm/fence/GPU/hang/reset; retain message, priority and timestamps only. No claim outside these bounds or about unreadable logs."}
(base / (name + "-kernel.json")).write_text(json.dumps(record, indent=2) + "\n")
print(json.dumps({"run": name, "kernel_query_returncode": result.returncode,
                  "matched_entries": len(rows), "parse_errors": len(errors)}))

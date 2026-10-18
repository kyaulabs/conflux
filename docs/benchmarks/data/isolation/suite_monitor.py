# SPDX-License-Identifier: AGPL-3.0-or-later
"""One bounded progress observation for the active suite phase."""
import json
from pathlib import Path
import subprocess
import sys

base = Path(__file__).resolve().parent
state = json.loads((base / "suite-state.json").read_text())
name = state.get("active_run")
if state["phase"] == "measurement" and name:
    raise SystemExit(subprocess.call([sys.executable, "-B", str(base / "monitor.py"), name]))
record = {"phase": state["phase"], "run": name}
if state["phase"] == "cooldown" and name:
    path = base / (name + "-cooldown.jsonl")
    if path.exists():
        with path.open("rb") as stream:
            stream.seek(max(0, path.stat().st_size - 4096))
            lines = stream.read().splitlines()
        for line in reversed(lines):
            try:
                record["last_cooldown_sample"] = json.loads(line)
                break
            except ValueError:
                pass
print(json.dumps(record))

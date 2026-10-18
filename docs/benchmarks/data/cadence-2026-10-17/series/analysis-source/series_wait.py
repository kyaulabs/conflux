# SPDX-License-Identifier: AGPL-3.0-or-later
"""Keep the common series idle guard alive through trials and cooldowns."""
import json
import os
from pathlib import Path
import time

base = Path(__file__).resolve().parent
stop = base / "cadence-series-stop"
if stop.exists():
    raise RuntimeError("stale series stop path")
started = time.monotonic()
record = {"pid": os.getpid(), "started_monotonic": started, "maximum_seconds": 10800,
          "scope": "No benchmark load; common sleep/desktop idle inhibition remains owned by the outer wrapper."}
(base / "cadence-series-wait.json").write_text(json.dumps(record, indent=2) + "\n")
while not stop.exists():
    if time.monotonic() - started > 10800:
        raise TimeoutError("series guard three-hour safety limit")
    time.sleep(1)
record.update(stopped_monotonic=time.monotonic(), stopped_by_file=True)
(base / "cadence-series-wait.json").write_text(json.dumps(record, indent=2) + "\n")

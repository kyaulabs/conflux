# SPDX-License-Identifier: AGPL-3.0-or-later
"""Record a finite idle cooldown without changing system settings."""
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import time

base = Path(__file__).resolve().parent
label = sys.argv[1]
zones = [path for path in Path("/sys/class/thermal").glob("thermal_zone*")
         if (path / "type").read_text().strip() == "x86_pkg_temp"]
if len(zones) != 1:
    raise RuntimeError("expected one readable x86 package temperature zone")
started = time.monotonic()
cool_since = None
rows = []
with (base / (label + "-cooldown.jsonl")).open("x") as log:
    while True:
        now = time.monotonic()
        temperature = int((zones[0] / "temp").read_text())
        cool_since = (cool_since if cool_since is not None else now) if temperature <= 55000 else None
        cool_span = now - cool_since if cool_since is not None else 0
        row = {"at_utc": datetime.now(timezone.utc).isoformat(), "elapsed_seconds": now - started,
               "package_millidegrees_C": temperature, "consecutive_cool_sample_span_seconds": cool_span}
        rows.append(row)
        log.write(json.dumps(row) + "\n")
        log.flush()
        if now - started >= 600 and cool_span >= 60:
            matched = True
            break
        if now - started >= 1200:
            matched = False
            break
        time.sleep(10)
result = {"status": "matched" if matched else "maximum_wait_reached", "temperature_zone": str(zones[0]),
          "minimum_seconds": 600, "maximum_seconds": 1200, "threshold_millidegrees_C": 55000,
          "required_cool_sample_span_seconds": 60, "rows": rows,
          "scope": "Ten-second package snapshots during idle waiting, not continuously observed temperature; no profile change."}
(base / (label + "-cooldown.json")).write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps({key: value for key, value in result.items() if key != "rows"}), flush=True)

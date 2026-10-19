# SPDX-License-Identifier: AGPL-3.0-or-later
"""Record a finite idle cooldown without changing system settings."""
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import time

base = Path(__file__).resolve().parent
label = sys.argv[1]
pause_path = base / "pause-after-current.json"
if pause_path.exists():
    pause = {}
    pause_error = None
    try:
        pause = json.loads(pause_path.read_text())
        if not isinstance(pause.get("current_run"), str):
            raise ValueError("pause marker has no current run")
    except Exception as error:
        pause_error = f"{type(error).__name__}: {error}"
        if not isinstance(pause, dict):
            pause = {}
    if pause_error or label != pause["current_run"]:
        decision = {"status": "paused", "blocked_run": label,
                    "current_run": pause.get("current_run"), "pause_marker": str(pause_path),
                    "at_utc": datetime.now(timezone.utc).isoformat(),
                    "guard_release_requested": False, "cleanup_proven": False,
                    "pending_root_cleanup": True, "errors": [],
                    "scope": "User pause before temperature reads or next trial; current workload is not signaled."}
        try:
            if pause_error:
                raise ValueError(pause_error)
            if pause["current_run"] != "observed-both-hour":
                raise ValueError("pause marker is outside this owned current run")
            cleanup_path = base / (pause["current_run"] + "-cleanup.json")
            cleanup = json.loads(cleanup_path.read_text())
            pids = cleanup.get("PID_exists")
            decision["cleanup_source"] = str(cleanup_path)
            decision["cleanup_proven"] = (
                isinstance(pids, dict) and bool(pids) and all(value is False for value in pids.values())
                and cleanup.get("worker_closed") is True and cleanup.get("fixture_exists") is False
                and cleanup.get("guard_errors") == [] and cleanup.get("idle_inhibit_released") is True)
            if not decision["cleanup_proven"]:
                raise RuntimeError("saved current-run cleanup is incomplete or unsafe")
            (base / "observed-series-stop").touch()
            decision.update(guard_release_requested=True, pending_root_cleanup=False,
                            guard_release_scope="Owned common guard stop requested; release acknowledgment remains in guard lifecycle.")
            (base / "pause-completed.json").write_text(json.dumps(decision, indent=2) + "\n")
        except Exception as error:
            decision["errors"].append(f"{type(error).__name__}: {error}")
        try:
            (base / (label + "-pause.json")).write_text(json.dumps(decision, indent=2) + "\n")
        except OSError as error:
            decision["errors"].append(f"pause_record: {error}")
        print(json.dumps(decision), flush=True)
        raise SystemExit(75)
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

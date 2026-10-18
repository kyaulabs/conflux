#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Capture only a completed owned UI's bounded Qt scenegraph journal entries."""

import argparse
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import re
import subprocess
import tempfile


FIELDS = ("MESSAGE", "QT_CATEGORY", "PRIORITY", "__MONOTONIC_TIMESTAMP",
          "__REALTIME_TIMESTAMP", "_SOURCE_MONOTONIC_TIMESTAMP",
          "_SOURCE_BOOTTIME_TIMESTAMP", "_PID", "_BOOT_ID")
CATEGORY = re.compile(r"\b(qt\.(?:scenegraph|rhi)(?:\.[A-Za-z0-9_]+)*)\b")
TIMING = re.compile(r"Frame (?:prepared|rendered)|syncAndRender|time in renderer|"
                    r"polishAndSync|renderLoop|swapBuffers|elapsed since last call", re.I)


def utc(value):
    result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if result.utcoffset() != timedelta(0):
        raise ValueError("launch bounds must explicitly use UTC")
    return result.astimezone(timezone.utc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", args.label):
        parser.error("label must be a plain filename label")
    base = Path(__file__).resolve().parent
    run = base / args.label
    launch = json.loads((base / (args.label + "-launch.json")).read_text())
    result = json.loads((run / "result.json").read_text())
    pulse = json.loads((run / "ui-progress.json").read_text())
    if "returncode" not in launch:
        parser.error("wrapper has not completed")
    since, until = utc(launch["started_at_utc"]), utc(launch["finished_at_utc"])
    if not timedelta(0) < until - since <= timedelta(hours=2):
        parser.error("launch bounds must increase and span at most two hours")
    pid = pulse.get("pid")
    if isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0:
        parser.error("final UI heartbeat PID is invalid")
    gated_pid = result["ui_coverage"]["progress"]["last_progress"]["pid"]
    if gated_pid != pid:
        parser.error("final UI PID differs from the runtime-gated PID")
    ui_command = result["load"]["children"]["ui"]["command"]
    if (Path(ui_command[0]).resolve() != base.parents[1] / "build/dev/conflux"
            or "--smoke-test" not in ui_command
            or str(run / "ui-progress.json") not in ui_command):
        parser.error("final UI command does not belong to this trial")
    output = base / (args.label + "-qt-timing.jsonl")
    metadata = base / (args.label + "-qt-timing-capture.json")
    if output.exists() or metadata.exists():
        parser.error("capture already exists; refuse to overwrite")
    command = ["journalctl", "--user", "_PID=" + str(pid), "--since",
               since.strftime("%Y-%m-%d %H:%M:%S.%f UTC"), "--until",
               until.strftime("%Y-%m-%d %H:%M:%S.%f UTC"), "--output=json", "--no-pager"]
    record = {"schema": 1, "label": args.label, "pid": pid, "ui_command": ui_command,
              "launch_bounds_utc": {"since": since.isoformat(), "until": until.isoformat()},
              "command": command, "timeout_seconds": 20, "returncode": None,
              "examined_entries": 0, "matched_entries": 0, "timing_entries": 0,
              "parse_errors": 0, "errors": [], "output": str(output),
              "scope": "User journal for final runtime-gated UI PID within launch-to-wrapper-exit bounds. Only Qt scenegraph/RHI categories and recognized Qt timing messages are retained; absent/rate-limited entries remain unobserved.",
              "clock_scope": "Journal __MONOTONIC_TIMESTAMP is microseconds in its _BOOT_ID clock domain. UI process /proc and Python samples use monotonic time. Qt's time-boot prefix may use a different suspend accounting; compare journal monotonic fields, do not blindly align a textual prefix across suspend."}
    with tempfile.TemporaryFile() as raw:
        try:
            query = subprocess.run(command, stdout=raw, stderr=subprocess.PIPE, timeout=20)
            record.update(returncode=query.returncode,
                          stderr=query.stderr.decode(errors="replace")[:4096],
                          stderr_truncated=len(query.stderr) > 4096)
            if query.returncode:
                record["errors"].append("journal query returned nonzero")
        except subprocess.TimeoutExpired as error:
            record.update(timed_out=True, stderr=(error.stderr or b"").decode(errors="replace")[:4096])
            record["errors"].append("journal query exceeded 20 seconds; retained output may be partial")
        record["query_stdout_bytes"] = raw.tell()
        raw.seek(0)
        with output.open("x") as retained:
            for line in raw:
                try:
                    row = json.loads(line)
                except (ValueError, UnicodeError):
                    record["parse_errors"] += 1
                    continue
                record["examined_entries"] += 1
                message = str(row.get("MESSAGE", ""))
                category = str(row.get("QT_CATEGORY", ""))
                timing = bool(TIMING.search(message))
                if not (CATEGORY.search(category) or CATEGORY.search(message) or timing):
                    continue
                selected = {key: row[key] for key in FIELDS if key in row}
                selected["recognized_timing_message"] = timing
                retained.write(json.dumps(selected) + "\n")
                record["matched_entries"] += 1
                record["timing_entries"] += timing
    metadata.write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({key: record[key] for key in ("label", "returncode", "examined_entries", "matched_entries", "timing_entries", "parse_errors", "errors")}))
    return 1 if record["errors"] or record["parse_errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

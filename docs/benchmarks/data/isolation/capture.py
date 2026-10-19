# SPDX-License-Identifier: AGPL-3.0-or-later
"""Capture host launch conditions and immutable producer identities; no GPU work."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
label = sys.argv[1]
record = {"observed_at_utc": datetime.now(timezone.utc).isoformat(),
          "scope": "Host snapshot; collected outside the restricted namespace",
          "label": label, "pins": {}, "read_errors": {}}
for key, command in (("head", ["git", "rev-parse", "HEAD"]),
                     ("profile", ["powerprofilesctl", "get"]),
                     ("holds", ["powerprofilesctl", "list-holds"])):
    record[key] = subprocess.check_output(command, text=True, timeout=2, cwd=ROOT).strip()
for path in ("/proc/meminfo", "/proc/swaps", "/sys/firmware/acpi/platform_profile",
             "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference",
             "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"):
    try:
        record[path] = Path(path).read_text().strip()
    except OSError as error:
        record["read_errors"][path] = str(error)
for zone in Path("/sys/class/thermal").glob("thermal_zone*"):
    try:
        record[str(zone)] = {"type": (zone / "type").read_text().strip(),
                             "millidegrees_C": int((zone / "temp").read_text())}
    except OSError as error:
        record["read_errors"][str(zone)] = str(error)
for supply in Path("/sys/class/power_supply").glob("*"):
    for key in ("type", "online", "status"):
        path = supply / key
        if path.exists():
            record[str(path)] = path.read_text().strip()
for name in ("src/main.cpp", "src/probe_main.cpp", "tools/benchmarks/stem_soak.py",
             "tools/benchmarks/stem_worker.py", "tools/benchmarks/window_guard.py",
             "tools/benchmarks/worker_support.py", "tools/stems/worker.py",
             "build/dev/conflux", "build/dev/conflux-audio-probe",
             "/tmp/conflux-stem-m0/conflux-stem-runtime-blocking",
             "/tmp/conflux-stem-m0/models/htdemucs-f16.gguf",
             str(BASE / "balanced-run.py"), str(BASE / "monitor.py")):
    path = Path(name)
    if not path.is_absolute():
        path = ROOT / path
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    record["pins"][name] = {"sha256": digest, "bytes": path.stat().st_size}
record["scratch"] = subprocess.check_output(["df", "-B1", "/tmp", str(BASE)], text=True)
(BASE / (label + ".json")).write_text(json.dumps(record, indent=2) + "\n")
print(json.dumps({key: record[key] for key in ("observed_at_utc", "label", "head", "profile", "holds")}))

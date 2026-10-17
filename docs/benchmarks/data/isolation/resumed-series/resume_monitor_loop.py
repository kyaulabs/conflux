# SPDX-License-Identifier: AGPL-3.0-or-later
"""Finite low-cost host progress observations; never scans complete live traces."""
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import time

base = Path(__file__).resolve().parent
started = time.monotonic()
while time.monotonic() - started < 7 * 3600:
    row = {'at_utc': datetime.now(timezone.utc).isoformat()}
    try:
        row['AC_online'] = int(Path('/sys/class/power_supply/AC/online').read_text())
        result = subprocess.run([sys.executable, '-B', str(base/'suite_monitor.py')],
                                text=True, capture_output=True, timeout=12)
        row['monitor_returncode'] = result.returncode
        if result.stdout.strip():
            row['progress'] = json.loads(result.stdout)
        if result.stderr.strip():
            row['error'] = result.stderr[-2048:]
    except Exception as error:
        row['error'] = f'{type(error).__name__}: {error}'
    with (base/'resume-monitor.jsonl').open('a') as stream:
        stream.write(json.dumps(row)+'\n')
    print(json.dumps(row), flush=True)
    if (base/'resume-series-stop').exists():
        break
    time.sleep(45)

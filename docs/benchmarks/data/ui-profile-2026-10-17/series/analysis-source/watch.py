# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded low-frequency progress/AC observer; no profile or service changes."""
import json
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime, timezone
base = Path(__file__).resolve().parent
name = sys.argv[1]
started = time.monotonic()
with (base / (name+'-AC-monitor.jsonl')).open('x') as log:
    while time.monotonic()-started < 5400:
        row={'at_utc':datetime.now(timezone.utc).isoformat(),'monotonic':time.monotonic(),
             'mains_online':Path('/sys/class/power_supply/AC/online').read_text().strip()}
        log.write(json.dumps(row)+'\n'); log.flush()
        if (base/name/'progress.json').exists():
            subprocess.run([sys.executable,'-B',str(base/'monitor.py'),name],check=True,timeout=12)
        else:
            p=base/(name+'-normal-ready.jsonl')
            last=json.loads(p.read_text().splitlines()[-1]) if p.exists() and p.stat().st_size else {}
            print(json.dumps({'run':name,'stage':json.loads((base/'hour-sequence.json').read_text()).get('stage','readiness') if name=='combined-vulkan-hour' and (base/'hour-sequence.json').exists() else 'readiness','AC':row['mains_online'],
                  'profile':last.get('profile'),'package_mC':last.get('package_millidegrees_C'),
                  'ready_span_s':last.get('consecutive_ready_sample_span_seconds')}),flush=True)
        if (base/name/'result.json').exists():
            break
        time.sleep(45)
    else:
        raise TimeoutError('finite monitor deadline')

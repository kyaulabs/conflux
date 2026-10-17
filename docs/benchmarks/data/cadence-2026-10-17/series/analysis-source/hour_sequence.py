# SPDX-License-Identifier: AGPL-3.0-or-later
"""Run the selected M0 hour after finite natural recovery; always release series guard."""
import json
from pathlib import Path
import signal
import subprocess
import sys
from datetime import datetime, timezone
base=Path(__file__).resolve().parent
name='33ms-both-hour'
child=None
record={'run':name,'renderer':'vulkan','repaint_ms':33,'profile_mutation':False,'started_at_utc':datetime.now(timezone.utc).isoformat()}
path=base/'hour-sequence.json'
if path.exists():raise FileExistsError(path)
def save(**fields):
    record.update(fields,updated_at_utc=datetime.now(timezone.utc).isoformat())
    path.write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(fields),flush=True)
def stop(_s,_f):
    if child is not None and child.poll() is None:child.send_signal(signal.SIGINT)
    raise KeyboardInterrupt
for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,stop)
try:
    for stage, command in (('cooldown',['cooldown.py',name]),
        ('hour',['observed_launch.py',name,'both','--seconds','3600','--control-interval','300','--repaint-ms','33']),
        ('cleanup',['finish_trial.py',name])):
        save(stage=stage)
        command[0]=str(base/command[0])
        child=subprocess.Popen([sys.executable,'-B',*command])
        code=child.wait();child=None
        if code:raise RuntimeError(f'{stage} returned {code}')
    save(status='completed',stage='report pending')
except BaseException as error:
    save(status='stopped',error=f'{type(error).__name__}: {error}')
    raise
finally:
    if child is not None and child.poll() is None:
        child.send_signal(signal.SIGINT)
        try:child.wait(timeout=20)
        except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
    (base/'cadence-series-stop').touch()

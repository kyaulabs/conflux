# SPDX-License-Identifier: AGPL-3.0-or-later
"""Retain final cleanup and bounded policy/kernel evidence after one owned run."""
from pathlib import Path
import json
import subprocess
import sys
base=Path(__file__).resolve().parent
name=sys.argv[1]
result=json.loads((base/name/'result.json').read_text())
guard=json.loads((base/(name+'-guard.json')).read_text())
policy=json.loads((base/(name+'-policy.json')).read_text())
pids=[r['runtime']['pid'] for r in result['runtime_ready']]
pids += [r['pid'] for r in result['supervisor_memory_cpu']['processes']]
ui=result.get('ui_coverage',{}).get('progress',{}).get('last_progress',{}).get('pid')
if ui: pids.append(ui)
record={'PIDs':pids,'PID_exists':{str(p):Path('/proc',str(p)).exists() for p in pids},
        'worker_closed':result['worker_statistics']['closed'],
        'backend_pid':result['worker_statistics'].get('backend_pid'),
        'fixture_exists':(base/name/'source.wav').exists(),
        'probe_returncodes':{k:v['returncode'] for k,v in result['load']['children'].items()},
        'idle_inhibit_released':guard.get('idle_inhibit_released'),
        'script_unloaded':guard.get('script_unloaded'),'window_restored':guard.get('restored'),
        'guard_errors':guard['errors'],'policy_runner_returncode':policy['runner_returncode']}
with (base/(name+'-cleanup.json')).open('x') as f:json.dump(record,f,indent=2);f.write('\n')
assert not any(record['PID_exists'].values()) and record['worker_closed'] and not record['fixture_exists']
assert record['idle_inhibit_released'] and not record['guard_errors'] and not record['policy_runner_returncode']
assert all(v==0 for v in record['probe_returncodes'].values())
for command in ([str(base/'observed-capture.py'),'after-'+name], [str(base/'kernel_capture.py'),name]):
    subprocess.run([sys.executable,'-B',*command],check=True,timeout=30)
launch=json.loads((base/(name+'-launch.json')).read_text())
subprocess.run([sys.executable,'-B',str(base/'thermal_capture.py'),name,'--since',launch['started_at_utc'],'--until',launch['finished_at_utc']],check=True,timeout=30)
kernel=json.loads((base/(name+'-kernel.json')).read_text())
assert kernel['returncode']==0 and not kernel['parse_errors']
print(json.dumps({'run':name,'cleanup_passed':True,'kernel_matches':len(kernel['matched_entries'])}))

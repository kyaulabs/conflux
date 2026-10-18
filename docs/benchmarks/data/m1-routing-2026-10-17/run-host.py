import datetime
import json
import os
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent
def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()
def capture(argv):
    return subprocess.check_output(argv, text=True, timeout=10)
def host():
    return {
        'guard': capture(['systemctl', '--user', 'show', 'thermal-guard.service', '-p', 'ActiveState', '-p', 'SubState', '-p', 'MainPID', '-p', 'InvocationID', '-p', 'NRestarts']),
        'profile': capture(['powerprofilesctl', 'get']),
    }
report = {'started_utc': utc(), 'before': host(), 'runs': []}
assert 'ActiveState=active\n' in report['before']['guard']
env = dict(os.environ, QT_QPA_PLATFORM='wayland', QT_QUICK_CONTROLS_STYLE='Basic')
env.pop('QT_QUICK_BACKEND', None)
fixtures = root / 'benchmarks-local/cdj-ui-2026-10-17'
jobs = [
    ('manual', [str(root / 'build/dev/conflux-manual-audio-check'), str(fixtures / 'quiet-0.wav'), str(fixtures / 'quiet-1.wav'), str(out / 'routing')], 45),
    ('smoke-wayland', [str(root / 'build/dev/conflux'), '--smoke-test', '--screenshot', str(out / 'smoke-wayland.png')], 15),
]
try:
    for name, argv, timeout in jobs:
        started = time.monotonic()
        with (out / (name + '.json')).open('w') as stdout, (out / (name + '.stderr')).open('w') as stderr:
            process = subprocess.Popen(argv, cwd=root, env=env, stdout=stdout, stderr=stderr)
            item = {'name': name, 'pid': process.pid, 'argv': argv}
            report['runs'].append(item)
            try:
                item['returncode'] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                item['returncode'] = process.returncode
                item['timed_out'] = True
            item['elapsed_seconds'] = time.monotonic() - started
        if item['returncode']:
            raise RuntimeError(f'{name} failed: {item}')
finally:
    report['after'] = host()
    report['stopped_utc'] = utc()
    (out / 'host-run.json').write_text(json.dumps(report, indent=2) + '\n')
    pids = [item['pid'] for item in report['runs']]
    objects = json.loads(capture(['pw-dump']))
    matching = [item.get('id') for item in objects if str(item.get('info', {}).get('props', {}).get('application.process.id', '')) in {str(pid) for pid in pids}]
    cleanup = {'owned_pids': pids, 'still_present': [pid for pid in pids if Path('/proc', str(pid)).exists()], 'pipewire_objects': matching}
    (out / 'cleanup.json').write_text(json.dumps(cleanup, indent=2) + '\n')
    print(json.dumps({'runs': report['runs'], 'cleanup': cleanup, 'guard_unchanged': report['before']['guard'] == report['after']['guard'], 'profiles': [report['before']['profile'].strip(), report['after']['profile'].strip()]}))
    assert not cleanup['still_present'] and not matching

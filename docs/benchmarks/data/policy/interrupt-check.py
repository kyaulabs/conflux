import json
import os
from pathlib import Path
import signal
import subprocess
import time

base = Path('benchmarks-local/policy-2026-10-16')
run = base / 'interrupt-check'
guard = base / 'interrupt-check-guard.json'
policy = base / 'interrupt-check-policy.json'
command = ['python3', '-B', 'tools/benchmarks/window_guard.py', '--output', str(guard),
           '--ui-progress', str(run / 'ui-progress.json'), '--', 'python3', '-B',
           str(base / 'balanced-run.py'), str(policy), '--runtime',
           '/tmp/conflux-stem-m0/conflux-stem-runtime-blocking', '--model',
           '/tmp/conflux-stem-m0/models/htdemucs-f16.gguf', '--ui', 'build/dev/conflux',
           '--audio-probe', 'build/dev/conflux-audio-probe', '--renderer', 'opengl',
           '--seconds', '120', '--control-interval', '300', '--request-timeout', '20',
           '--sample-interval', '0.5', '--output', str(run)]
assert not run.exists() and not guard.exists() and not policy.exists()
record = {'command': command, 'started_monotonic': time.monotonic()}
with (base / 'interrupt-check.stdout').open('w') as out, (base / 'interrupt-check.stderr').open('w') as err:
    child = subprocess.Popen(command, stdout=out, stderr=err)
    try:
        until = time.monotonic() + 15
        while time.monotonic() < until:
            assert child.poll() is None, 'wrapper exited before interruption'
            time.sleep(0.1)
        g = json.loads(guard.read_text())
        assert g['ready']
        record['ui_pid'] = g['ui']['pid']
        trace = run / 'native-trace.jsonl'
        with trace.open('rb') as stream:
            stream.seek(max(0, trace.stat().st_size - 65536))
            last = json.loads(stream.read().splitlines()[-1])
        record['native_pid'] = last['pid']
        record['signal_monotonic'] = time.monotonic()
        child.send_signal(signal.SIGINT)
        record['returncode'] = child.wait(timeout=30)
        record['shutdown_seconds'] = time.monotonic() - record['signal_monotonic']
        g, p = json.loads(guard.read_text()), json.loads(policy.read_text())
        assert record['returncode'] == 130
        assert p['interrupted'] and p['restored_by_runner']
        assert p['before']['profile'] == p['after']['profile']
        assert not p['after']['holds']
        assert g['script_unloaded'] and g['idle_inhibit_released'] and not g['errors']
        assert not Path('/proc', str(record['ui_pid'])).exists()
        assert not Path('/proc', str(record['native_pid'])).exists()
        assert not (run / 'source.wav').exists()
        record.update(passed=True, guard_cleanup=True, policy_restored=True,
                      native_and_UI_exited=True, fixture_removed=True)
    finally:
        if child.poll() is None:
            child.send_signal(signal.SIGINT)
            child.wait(timeout=30)
        (base / 'interrupt-check-verification.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record))

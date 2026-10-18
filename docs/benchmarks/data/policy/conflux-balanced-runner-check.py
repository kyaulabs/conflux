from contextlib import redirect_stdout
import ast
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import signal
import subprocess
import tempfile
from unittest.mock import patch

SOURCE = Path('/home/kyau/projects/kyaulabs/conflux/benchmarks-local/policy-2026-10-16/balanced-run.py')
ast.parse(SOURCE.read_text())
spec = importlib.util.spec_from_file_location('balanced_runner_under_test', SOURCE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
read_text = Path.read_text

class Scenario:
    def __init__(self, name):
        self.name = name
        self.profile = 'power-saver'
        self.gets = 0
        self.calls = []
        self.kills = []
        self.waits = []
        self.returncode = None
        self.pid = 424242
    def check_output(self, command, *, text, timeout):
        assert text is True and timeout == 2
        self.calls.append((tuple(command), timeout))
        if command[-1] == 'get':
            self.gets += 1
            if self.name == 'before_restore_timeout' and self.gets == 3:
                raise subprocess.TimeoutExpired(command, timeout)
            return self.profile + '\n'
        assert command[-1] == 'list-holds'
        return 'existing hold\n' if self.name == 'existing_hold' else ''
    def run(self, command, *, check, timeout):
        assert check is True and timeout == 2
        self.calls.append((tuple(command), timeout))
        if self.name == 'restore_timeout' and command[-1] == 'power-saver':
            raise subprocess.TimeoutExpired(command, timeout)
        if self.name == 'failed_set' and command[-1] == 'balanced':
            self.profile = 'balanced'
            raise subprocess.TimeoutExpired(command, timeout)
        self.profile = command[-1]
    def popen(self, command, *, start_new_session):
        assert command[:3] == ['python3', '-B', 'tools/benchmarks/stem_soak.py']
        assert start_new_session is True
        return self
    def poll(self):
        return self.returncode
    def wait(self, timeout=None):
        self.waits.append(timeout)
        if timeout is None:
            if self.name in ('cancel', 'forcekill'):
                raise KeyboardInterrupt
            if self.name == 'intervening_profile':
                self.profile = 'performance'
            self.returncode = 7 if self.name == 'child_error' else 0
        elif timeout == 5:
            if self.name == 'forcekill':
                raise subprocess.TimeoutExpired('fake-soak', timeout)
            self.returncode = 130
        else:
            assert timeout == 2 and self.returncode == -signal.SIGKILL
        return self.returncode
    def killpg(self, pid, sig):
        assert pid == self.pid
        self.kills.append(sig)
        if sig == signal.SIGKILL:
            self.returncode = -sig


def fake_read(path, *args, **kwargs):
    if str(path) == '/sys/firmware/acpi/platform_profile':
        return 'mock-firmware\n'
    if str(path) == '/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference':
        return 'mock-epp\n'
    return read_text(path, *args, **kwargs)

names = ['success', 'child_error', 'cancel', 'forcekill', 'restore_timeout',
         'before_restore_timeout', 'intervening_profile', 'failed_set', 'existing_hold']
with tempfile.TemporaryDirectory(prefix='conflux-balanced-runner-check-') as directory:
    for name in names:
        fake = Scenario(name)
        record_path = Path(directory) / (name + '.json')
        with patch.object(module.subprocess, 'check_output', fake.check_output), \
             patch.object(module.subprocess, 'run', fake.run), \
             patch.object(module.subprocess, 'Popen', fake.popen), \
             patch.object(module.os, 'killpg', fake.killpg), \
             patch.object(module.signal, 'signal', return_value=signal.SIG_DFL), \
             patch.object(Path, 'read_text', fake_read), redirect_stdout(io.StringIO()):
            code = module.main([str(record_path), '--seconds', '120'])
        record = json.loads(record_path.read_text())
        assert record['runner_returncode'] == code
        assert record['after']['profile'] == fake.profile
        if name == 'success':
            assert code == 0 and record['restored_by_runner'] and fake.profile == 'power-saver'
        elif name == 'child_error':
            assert code == 7 and record['restored_by_runner'] and fake.profile == 'power-saver'
        elif name == 'cancel':
            assert code == 130 and record['interrupted'] and record['restored_by_runner']
            assert fake.kills == [signal.SIGINT] and fake.waits == [None, 5]
        elif name == 'forcekill':
            assert code == 130 and record['interrupted'] and record['restored_by_runner']
            assert fake.kills == [signal.SIGINT, signal.SIGKILL] and fake.waits == [None, 5, 2]
        elif name == 'restore_timeout':
            assert code == 1 and 'TimeoutExpired' in record['restore_error'] and fake.profile == 'balanced'
        elif name == 'before_restore_timeout':
            assert code == 1 and 'TimeoutExpired' in record['restore_error'] and fake.profile == 'balanced'
        elif name == 'intervening_profile':
            assert code == 0 and not record['restored_by_runner']
            assert record['preserved_intervening_profile'] == fake.profile == 'performance'
            assert not any(c[-1] == 'power-saver' for c, _ in fake.calls)
        elif name == 'failed_set':
            assert code == 1 and 'TimeoutExpired' in record['error'] and record['restored_by_runner']
            assert fake.profile == 'power-saver' and not fake.waits
        elif name == 'existing_hold':
            assert code == 1 and 'existing power holds' in record['error'] and not record['restored_by_runner']
            assert not any('set' in c for c, _ in fake.calls) and not fake.waits
        print(name + ': pass')
print('AST + 9 focused scenarios passed; no real subprocesses or profiles used')
print('runner_sha256=' + hashlib.sha256(SOURCE.read_bytes()).hexdigest())
print('configured_cleanup_wait_budget_seconds=5+2+4+2+4=17 (plus local syscall/Python overhead)')

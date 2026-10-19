from collections import deque
from contextlib import ExitStack
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from unittest.mock import patch

import numpy as np
from tools.stems import worker

repo = Path('/home/kyau/projects/kyaulabs/conflux')
source = repo / 'tools/benchmarks/native_replay.py'
expected_driver = '6ee7408415fdf0366cd3282fe57f94409d8f9bc34eb2967eb0e622bf2cf1757e'
assert hashlib.sha256(source.read_bytes()).hexdigest() == expected_driver
forbidden_calls = []
def forbidden(*args, **kwargs):
    forbidden_calls.append((args, kwargs))
    raise AssertionError('Native runtime/subprocess call forbidden during CPU normalization check')

with ExitStack() as stack:
    stack.enter_context(patch.object(worker, 'NativeRuntime', forbidden))
    for name in ('Popen', 'run', 'check_output', 'call', 'check_call'):
        stack.enter_context(patch.object(subprocess, name, forbidden))
    spec = importlib.util.spec_from_file_location('cpu_only_native_replay_check', source)
    replay = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(replay)
    with tempfile.TemporaryDirectory(prefix='conflux-replay-window3-', dir='/tmp') as directory:
        fixture = Path(directory) / 'source.wav'
        metadata = replay.create_fixture(fixture, replay.FIXTURE_FRAMES / replay.RATE)
        assert metadata['sha256'] == replay.EXPECTED['fixture']
        supervisor = worker.StemWorker.__new__(worker.StemWorker)
        supervisor._condition = threading.Condition()
        supervisor._closed = False
        supervisor._cached_source = None
        supervisor._source_id = None
        supervisor._events = deque(maxlen=1024)
        supervisor._telemetry_dropped = supervisor._telemetry_high_water = 0
        command = worker._Command(1, fixture, 0, time.monotonic())
        supervisor._command = command
        source_info = supervisor._source(command)
        start = 3 * worker.HOP
        expected = supervisor._read_window(source_info, start)
        actual, statistics = replay.normalized_window(fixture, start)
        assert actual.dtype == expected.dtype == np.dtype('float32')
        assert actual.shape == expected.shape == (worker.WINDOW, worker.CHANNELS)
        assert actual.tobytes() == expected.tobytes()
        assert statistics['mean'] == float(source_info.mean)
        assert statistics['std'] == float(source_info.std)
        assert statistics['valid_frames'] == worker.WINDOW
        result = {'driver_sha256': expected_driver, 'fixture_sha256': metadata['sha256'],
                  'window_start_frame': start, 'shape': list(actual.shape), 'input_bytes': actual.nbytes,
                  'input_sha256': hashlib.sha256(actual.tobytes()).hexdigest(),
                  'byte_identical_to_actual_worker_methods': True,
                  'mean': statistics['mean'], 'std': statistics['std'],
                  'native_or_subprocess_calls': len(forbidden_calls), 'temporary_fixture_removed': False}
    assert not Path(directory).exists()
    result['temporary_fixture_removed'] = True
assert not forbidden_calls
frozen = json.loads((repo/'docs/benchmarks/data/thermal/continuous-hour/result.json').read_text())['runtime_ready'][0]['runtime']
ready_expected = {'fence_wait_mode': 'blocking', 'backend': 'vulkan', 'threads': 4,
                  'weight_storage': 'native', 'process_environment': {'OMP_WAIT_POLICY': 'PASSIVE', 'GOMP_SPINCOUNT': '0'}}
assert all(frozen.get(key) == value for key, value in ready_expected.items())
result['ready_metadata_names_and_values_match_frozen_runtime'] = True
out = Path('/tmp/conflux-native-replay-window3-check.json')
out.write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))

from contextlib import ExitStack
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch
import numpy as np

REPO = Path('/home/kyau/projects/kyaulabs/conflux')
SOURCE = REPO / 'tools/benchmarks/stem_soak.py'
sys.path.insert(0, str(REPO / 'tools/benchmarks'))
ast.parse(SOURCE.read_text())
spec = importlib.util.spec_from_file_location('parity_failure_soak_test', SOURCE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
MODES = module.Load.MODES
calls = {'native_or_subprocess': 0, 'worker_close': 0, 'load_stop': 0, 'sampler_stop': 0}

def forbidden(*args, **kwargs):
    calls['native_or_subprocess'] += 1
    raise AssertionError('native/process work forbidden')

class Worker:
    backend_pid = 123
    def __init__(self, *args, **kwargs): self.closed = False
    def statistics(self): return {'backend_start_count': 1, 'telemetry_dropped': 0, 'closed': self.closed}
    def close(self, **kwargs): self.closed = True; calls['worker_close'] += 1
class Load:
    MODES = MODES
    def __init__(self, *args): pass
    def start(self): pass
    def stop(self):
        calls['load_stop'] += 1
        return {'children': {}, 'errors': [], 'selected_loads': []}
class Sampler:
    def __init__(self, *args, **kwargs): pass
    def __enter__(self): return self
    def stop(self): calls['sampler_stop'] += 1; return {'trace': {'errors': {}}}
class FailingLog:
    def __init__(self, original): self.original = original
    def write(self, value): raise OSError('injected log write failure')
    def flush(self): raise OSError('injected log flush failure')
    def close(self): self.original.close()

def reject_json_constant(value): raise AssertionError('nonstandard JSON constant: ' + value)
def read_json(path): return json.loads(path.read_text(), parse_constant=reject_json_constant)

def fixture(path, _seconds):
    path.write_bytes(b'owned generated fixture placeholder')
    return {'path': str(path), 'frames': module.FIXTURE_FRAMES, 'sha256': 'fixture-id'}

def event(samples):
    return {'generation': 77, 'request_id': 628, 'source_id': 'fixture-id', 'model_id': 'model-id',
            'frame_start': module.HOP + 333, 'window_start_frame': module.HOP,
            'frames': samples.shape[1], 'samples': samples, 'ready_queue_depth': 1,
            'ready_at': time.monotonic(), 'request_ms': 1.0, 'source_read_ms': 0.1,
            'overlap_ms': 0.1, 'hop_service_ms': 1.2, 'control_latency_ms': 1.3,
            'queue_delay_ms': 0.1, 'metrics': {'graph_ms': 0.8}}

summary = []
with tempfile.TemporaryDirectory(prefix='conflux-soak-parity-check-') as directory:
    parent = Path(directory)
    dummy_runtime = parent / 'dummy-runtime'; dummy_runtime.write_bytes(b'no executable')
    dummy_model = parent / 'dummy-model'; dummy_model.write_bytes(b'no model weights')
    for mode in ['mismatch', 'capture_write_failure', 'nonfinite', 'success']:
        root = parent / mode; root.mkdir()
        full_reference = np.arange(4 * 345 * 2, dtype=np.float32).reshape(4, 345, 2) / 10000
        comparison_reference = full_reference[:, 333:]
        samples = comparison_reference.copy()
        if mode != 'success':
            samples[0, 0, 0] += np.float32(0.25)
            samples[2, 5, 1] -= np.float32(0.5)
        if mode == 'nonfinite':
            samples[0, 0, 0] = np.nan
            samples[2, 5, 1] = np.inf
        reference_hash = hashlib.sha256(full_reference.tobytes()).hexdigest()
        args = SimpleNamespace(load_mode='none', renderer='opengl', seconds=0.000001,
                               control_interval=15, sample_interval=0.5, load_seconds=120,
                               runtime=dummy_runtime, model=dummy_model, threads=4, request_timeout=30)
        def workload(self):
            self.started = time.monotonic() - 0.01
            self.runtime = [{'event': 'runtime_ready'}]
            self.expected = module.HOP + 333
            self.references[module.HOP] = full_reference.copy()
            if mode == 'capture_write_failure':
                (root / 'parity-failure-actual.f32').mkdir()
                self.log = FailingLog(self.log)
            self.accept(event(samples))
            self.finished = time.monotonic()
        with ExitStack() as stack:
            for name, value in [('StemWorker', Worker), ('Load', Load), ('ProcessSampler', Sampler),
                                ('create_fixture', fixture), ('conditions', lambda: {})]:
                stack.enter_context(patch.object(module, name, value))
            stack.enter_context(patch.object(module.Soak, 'workload', workload))
            stack.enter_context(patch.object(subprocess, 'run', return_value=SimpleNamespace(stdout='fake-revision\n')))
            stack.enter_context(patch.object(subprocess, 'Popen', forbidden))
            result = module.Soak(args, root).execute()
        saved = read_json(root / 'result.json')
        assert result == saved and not (root / 'source.wav').exists()
        assert reference_hash == hashlib.sha256(full_reference.tobytes()).hexdigest()
        if mode == 'success':
            assert saved['status'] == 'ok' and saved['failures'] == []
            assert 'parity_failure' not in saved and not list(root.glob('parity-failure-*'))
            assert saved['trimmed_seek_checks'] == 1 and saved['timing']['all_delivered']['chunks'] == 1
            assert saved['reference_hashes'] == {}
        else:
            assert saved['status'] == 'error'
            assert any(f['error'] == 'RuntimeError: seek differs from continuous reference beyond tolerance'
                       for f in saved['failures'])
            record = saved['parity_failure']
            assert record['identity']['request_id'] == 628
            assert record['format'] == 'F32LE' and record['layout'] == 'stem, frame, channel'
            assert record['maximum_artifact_bytes'] == 4 * module.HOP * 2 * 4
            assert record['failed_elements'] == 2 and record['failed_elements_per_stem'] == [1, 0, 1, 0]
            assert saved['trimmed_seek_checks'] == 0 and saved['timing']['all_delivered']['chunks'] == 0
            if mode == 'capture_write_failure':
                assert {x['phase'] for x in record['capture_errors']} == {'actual', 'event_log'}
                assert record['artifacts']['actual']['written'] is False
                assert record['artifacts']['reference']['written'] is True
            else:
                assert record['capture_errors'] == []
                logged = [read_json_line for read_json_line in
                          (json.loads(x, parse_constant=reject_json_constant) for x in (root/'events.jsonl').read_text().splitlines())]
                assert logged == [record]
                loaded = {}
                for name, artifact in record['artifacts'].items():
                    path = Path(artifact['path']); raw = path.read_bytes()
                    assert len(raw) == artifact['bytes'] <= record['maximum_artifact_bytes']
                    assert hashlib.sha256(raw).hexdigest() == artifact['sha256']
                    assert artifact['written']
                    loaded[name] = np.frombuffer(raw, dtype='<f4').reshape(artifact['shape'])
                difference = loaded['actual'] - loaded['reference']
                mask = ~np.isclose(loaded['actual'], loaded['reference'], rtol=1e-4, atol=1e-5)
                assert int(np.count_nonzero(mask)) == record['failed_elements']
                assert np.count_nonzero(mask, axis=(1,2)).tolist() == record['failed_elements_per_stem']
                if mode == 'nonfinite':
                    assert record['parity']['max_abs_difference'] is None
                    assert record['difference_element_counts'] == {'finite': 94, 'nan': 1, 'infinite': 1}
                    assert record['max_abs_difference_per_stem'][0] is None
                    assert record['max_abs_difference_per_stem'][2] is None
                else:
                    assert float(np.max(np.abs(difference))) == record['parity']['max_abs_difference']
                    assert record['difference_element_counts'] == {'finite': 96, 'nan': 0, 'infinite': 0}
        summary.append({'case': mode, 'passed': True, 'status': saved['status']})
    root = parent / 'oversize'; root.mkdir()
    args = SimpleNamespace(load_mode='none')
    soak = module.Soak(args, root)
    with (root/'events.jsonl').open('w') as log:
        soak.log = log
        oversized = np.zeros((4, module.HOP + 1, 2), dtype=np.float32)
        soak.capture_parity_failure(event(oversized), oversized, oversized, oversized,
            {'kind': 'trimmed_seek', 'exact': False, 'numeric_parity': False,
             'max_abs_difference': 0.0, 'rtol': 1e-4, 'atol': 1e-5})
    record = read_json(root/'events.jsonl')
    assert len(record['capture_errors']) == 2
    assert all('one-hop artifact bound' in x['error'] for x in record['capture_errors'])
    assert not list(root.glob('*.f32'))
    assert all(not x['written'] for x in record['artifacts'].values())
    summary.append({'case': 'oversize_capture_rejected', 'passed': True})
assert not Path(directory).exists()
assert calls['native_or_subprocess'] == 0
assert calls['worker_close'] == 4 and calls['load_stop'] == 4 and calls['sampler_stop'] == 8
out = {'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(), 'AST_passed': True,
       'cases': summary, 'calls': calls, 'temporary_artifacts_removed': True}
Path('/tmp/conflux-soak-parity-failure-check.json').write_text(json.dumps(out, indent=2)+'\n')
print(json.dumps(out, indent=2))

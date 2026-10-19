#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""CPU-only harness checks: no native, UI, audio, D-Bus or GPU child is launched."""
import copy
import importlib
import io
import json
from pathlib import Path
import signal
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

REPO = Path('/home/kyau/projects/kyaulabs/conflux')
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO / 'tools/benchmarks'))
import stem_worker
import stem_soak

class FakeLoop:
    def run(self):
        pass
    def quit(self):
        pass

fake_gi = types.ModuleType('gi')
fake_gi.require_version = lambda *args: None
fake_repository = types.ModuleType('gi.repository')
fake_repository.Gio = types.SimpleNamespace(BusType=types.SimpleNamespace(SESSION=0))
fake_repository.GLib = types.SimpleNamespace(MainLoop=FakeLoop, timeout_add=lambda *args: None)
with patch.dict(sys.modules, {'gi': fake_gi, 'gi.repository': fake_repository}):
    window_guard = importlib.import_module('window_guard')

FAULTS = ('callback_execution_deadline_misses', 'missing_buffers', 'invalid_buffers',
          'truncated_frames', 'observed_graph_position_discontinuities',
          'observed_position_clock_xrun_recovery_flags')

def arguments(root, mode='both'):
    return types.SimpleNamespace(load_mode=mode, renderer='opengl', ui=root/'ui-bin',
        audio_probe=root/'audio-bin', runtime=root/'runtime', model=root/'model',
        quantum=256, load_seconds=3720, seconds=3600.0, threads=4,
        control_interval=300.0, sample_interval=0.5, request_timeout=30.0)

def reports():
    return {'ui': {'rendered': True, 'platform': 'wayland', 'graphics_api': 3,
        'frames_swapped': 61, 'frame_observation': {'frames_per_60_seconds': [1]*61,
        'started_monotonic_ns': 99_000_000_000, 'stopped_monotonic_ns': 3701_000_000_000,
        'maximum_notification_gap_ms': 16.7}},
        'audio': {'status': 'ok', 'streaming_started_monotonic_ns': 99_000_000_000,
                  'stopped_monotonic_ns': 3701_000_000_000, **dict.fromkeys(FAULTS, 0)}}

class FakeChild:
    next_pid = 10000
    def __init__(self, *args, **kwargs):
        self.returncode = None
        FakeChild.next_pid += 1
        self.pid = FakeChild.next_pid
        self.killed = False
    def poll(self):
        return self.returncode
    def wait(self, timeout=None):
        self.returncode = 0
        return 0
    def kill(self):
        self.killed = True
        self.returncode = -9

class FakeWorker:
    def __init__(self, *args, **kwargs):
        self.backend_pid = None
        self.closed = False
    def statistics(self):
        return {'backend_start_count': 1, 'telemetry_dropped': 0}
    def close(self, **kwargs):
        self.closed = True

class FakeSampler:
    def __init__(self, *args, **kwargs):
        pass
    def __enter__(self):
        return self
    def stop(self):
        return {}

class HarnessChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='conflux-load-check-')
        self.root = Path(self.temp.name)
        for name in ('runtime', 'model', 'ui-bin', 'audio-bin'):
            (self.root/name).write_bytes(b'fake CPU-only pin\n')
    def tearDown(self):
        self.temp.cleanup()

    def child_factory(self, data, calls, fail_name=None):
        def launch(command, *, stdout, stderr, env):
            name = 'ui' if '--smoke-test' in command else 'audio'
            calls.append(name)
            if name == fail_name:
                raise OSError('synthetic launch failure')
            stdout.write(json.dumps(data[name]).encode())
            stdout.flush()
            return FakeChild()
        return launch

    def test_load_modes_and_default(self):
        for mode, selected in stem_worker.Load.MODES.items():
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(dir=self.root) as directory:
                root = Path(directory)
                args = arguments(root, mode)
                if 'ui' not in selected:
                    args.ui = None
                if 'audio' not in selected:
                    args.audio_probe = None
                calls = []
                load = stem_worker.Load(args, root)
                self.assertEqual(tuple(load.commands), selected)
                with patch.object(stem_worker.subprocess, 'Popen', self.child_factory(reports(), calls)), \
                     patch.object(stem_worker.time, 'sleep') as sleep:
                    load.start()
                    sleep.assert_called_once_with(1)
                    load.check()
                    result = load.stop()
                self.assertEqual(calls, list(selected))
                self.assertEqual(set(result['children']), set(selected))
                self.assertEqual(result['launched_loads'], list(selected))
                self.assertEqual(result['not_run'], [n for n in ('ui', 'audio') if n not in selected])
                self.assertEqual(result['errors'], [])
                self.assertEqual('audio_fault_counters' in result, 'audio' in selected)
                for name in ('ui', 'audio'):
                    self.assertEqual((root/f'{name}.json').exists(), name in selected)
                    self.assertEqual((root/f'{name}.stderr').exists(), name in selected)
        args = arguments(self.root)
        del args.load_mode
        self.assertEqual(tuple(stem_worker.Load(args, self.root).commands), ('ui', 'audio'))

    def test_partial_start_and_early_exit(self):
        load = stem_worker.Load(arguments(self.root), self.root)
        calls = []
        with patch.object(stem_worker.subprocess, 'Popen', self.child_factory(reports(), calls, 'audio')):
            with self.assertRaisesRegex(OSError, 'synthetic launch'):
                load.start()
        self.assertEqual([name for name, *_ in load.children], ['ui'])
        child = load.children[0][1]
        child.returncode = 4
        with self.assertRaisesRegex(RuntimeError, 'ui load exited early: 4'):
            load.check()
        result = load.stop()
        self.assertEqual(set(result['children']), {'ui'})
        self.assertTrue(all(stdout.closed and stderr.closed for _, _, stdout, stderr, _ in load.children))

    def test_disabled_ui_does_not_touch_progress(self):
        for mode in ('none', 'audio'):
            soak = stem_soak.Soak(arguments(self.root, mode), self.root)
            soak.log = io.StringIO()
            soak.load = types.SimpleNamespace(check=lambda: None)
            with patch.object(Path, 'read_text', side_effect=AssertionError('unexpected pulse read')):
                soak.check_ui_progress(999999.0)
                soak.check_load()
            self.assertIsNone(soak.args.ui_progress_file)
            self.assertEqual(soak.ui_checks, 0)
            self.assertEqual(soak.log.getvalue(), '')
            self.assertEqual(soak.ui_coverage({}), {'requested': False, 'status': 'not_run', 'passed': None})

    def test_selected_ui_still_fails_runtime_and_final_coverage(self):
        soak = stem_soak.Soak(arguments(self.root, 'ui'), self.root)
        soak.load = types.SimpleNamespace(children=[])
        soak.log = io.StringIO()
        soak.ui_watch_started = 100.0
        with self.assertRaisesRegex(RuntimeError, 'required UI progress failed'):
            soak.check_ui_progress(106.0)
        self.assertIn('ui_gate_failure', soak.log.getvalue())
        self.assertFalse(soak.ui_coverage({})['passed'])

    def test_audio_interval_requires_valid_selected_stream(self):
        soak = stem_soak.Soak(arguments(self.root, 'audio'), self.root)
        soak.started, soak.finished = 100.0, 3700.0
        self.assertTrue(soak.audio_coverage(reports()['audio'])['passed'])
        for change in ({}, {'streaming_started_monotonic_ns': True},
                       {'streaming_started_monotonic_ns': 4000_000_000_000},
                       {'stopped_monotonic_ns': 3699_000_000_000}, {'status': 'error'}):
            data = {**reports()['audio'], **change} if change else {}
            self.assertFalse(soak.audio_coverage(data)['passed'])
        disabled = stem_soak.Soak(arguments(self.root, 'ui'), self.root).audio_coverage(reports()['audio'])
        self.assertIsNone(disabled['passed'])
        self.assertIsNone(disabled['concurrent_workload_seconds'])

    def execute_fake(self, mode, *, mutate=None, service_ms=2000.0, controls=True, dropped=0):
        root = self.root / f'execute-{mode}-{len(list(self.root.iterdir()))}'
        root.mkdir()
        args = arguments(self.root, mode)
        soak = stem_soak.Soak(args, root)
        data = reports()
        if mutate:
            mutate(data)
        def fixture(path, _seconds):
            path.write_bytes(b'fake fixture')
            return {'path': str(path), 'frames': stem_soak.FIXTURE_FRAMES}
        def workload():
            soak.started, soak.finished = 100.0, 3700.0
            soak.runtime = [{'event': 'runtime_ready'}]
            soak.controls = [{'complete': True}] if controls else []
            soak.hops = [{'hop_service_ms': service_ms, 'frames': 1, 'kind': 'continuous', 'elapsed_seconds': 1}]
            if 'ui' in soak.selected_loads:
                soak.ui_last_progress = {'frames_swapped': 1}
        soak.workload = workload
        fake_statistics = lambda _self: {'backend_start_count': 1, 'telemetry_dropped': dropped}
        with patch.object(stem_soak, 'StemWorker', FakeWorker), \
             patch.object(FakeWorker, 'statistics', fake_statistics), \
             patch.object(stem_soak, 'ProcessSampler', FakeSampler), \
             patch.object(stem_soak, 'create_fixture', fixture), \
             patch.object(stem_soak, 'conditions', lambda: {}), \
             patch.object(stem_soak.subprocess, 'run', return_value=types.SimpleNamespace(stdout='frozen-fake\n')), \
             patch.object(stem_worker.subprocess, 'Popen', self.child_factory(data, [])), \
             patch.object(stem_worker.time, 'sleep'):
            return root, soak.execute()

    def test_end_to_end_modes_coverage_pins_artifacts(self):
        for mode, selected in stem_worker.Load.MODES.items():
            with self.subTest(mode=mode):
                root, result = self.execute_fake(mode)
                self.assertEqual(result['status'], 'ok', result['failures'])
                self.assertEqual(result['load_mode'], mode)
                self.assertEqual(result['selected_loads'], list(selected))
                acceptance = result['acceptance']
                self.assertTrue(acceptance['selected_load_coverage_completed'])
                self.assertEqual(acceptance['full_audio_UI_soak_qualified'], mode == 'both')
                for name, pin, legacy in (('ui', 'ui', 'required_UI_coverage_completed'),
                                          ('audio', 'audio_probe', 'audio_covers_requested_duration')):
                    coverage = result[f'{name}_coverage']
                    self.assertEqual(coverage['requested'], name in selected)
                    self.assertEqual(coverage['status'], 'passed' if name in selected else 'not_run')
                    self.assertEqual(coverage['passed'], True if name in selected else None)
                    self.assertEqual(acceptance[legacy], True if name in selected else None)
                    self.assertEqual(pin in result['pins'], name in selected)
                    self.assertEqual((root/f'{name}.json').exists(), name in selected)
                    self.assertEqual((root/f'{name}.stderr').exists(), name in selected)
                if mode == 'none':
                    self.assertIsNone(result['load_alive_checks'])
                    self.assertIsNone(result['maximum_load_check_gap_seconds'])
                if 'audio' not in selected:
                    self.assertIsNone(result['concurrent_audio_workload_seconds'])
                if 'ui' not in selected:
                    self.assertIsNone(result['ui_progress_file'])
                    self.assertFalse((root/'ui-progress.json').exists())
                self.assertFalse((root/'source.wav').exists())

    def test_selected_failures_controls_telemetry_target_still_gate(self):
        cases = [('audio', lambda r: r['audio'].update(stopped_monotonic_ns=200_000_000_000), 'audio_coverage'),
                 ('ui', lambda r: r['ui']['frame_observation'].update(maximum_notification_gap_ms=40000), 'ui_coverage'),
                 ('audio', lambda r: r['audio'].update(missing_buffers=1), 'audio_ui_load')]
        for mode, mutate, phase in cases:
            _, result = self.execute_fake(mode, mutate=mutate)
            self.assertEqual(result['status'], 'error')
            self.assertIn(phase, [row['phase'] for row in result['failures']])
            self.assertFalse(result['acceptance']['full_audio_UI_soak_qualified'])
        for kwargs, phase in (({'controls': False}, 'control_coverage'), ({'dropped': 1}, 'telemetry')):
            _, result = self.execute_fake('none', **kwargs)
            self.assertEqual(result['status'], 'error')
            self.assertIn(phase, [row['phase'] for row in result['failures']])
        _, result = self.execute_fake('both', service_ms=5000.0)
        self.assertEqual(result['status'], 'ok')
        self.assertFalse(result['acceptance']['observed_continuous_p99_within_target'])
        self.assertFalse(result['acceptance']['full_audio_UI_soak_qualified'])

    def test_cli_only_requires_selected_probe_paths(self):
        base = ['stem_soak.py', '--runtime', str(self.root/'runtime'), '--model', str(self.root/'model'),
                '--output', str(self.root/'cli')]
        def fake_execute(soak):
            return {'status': 'ok', 'workload_seconds': 0, 'acceptance': {}, 'failures': []}
        for mode, selected in stem_worker.Load.MODES.items():
            command = [*base, '--load', mode]
            for name in selected:
                command.extend(['--ui' if name == 'ui' else '--audio-probe', str(self.root/name)])
            with patch.object(sys, 'argv', command), patch.object(stem_soak.Soak, 'execute', fake_execute), \
                 patch('sys.stdout', new=io.StringIO()):
                self.assertEqual(stem_soak.main(), 0)
        with patch.object(sys, 'argv', base), patch('sys.stderr', new=io.StringIO()):
            with self.assertRaises(SystemExit) as failure:
                stem_soak.main()
            self.assertEqual(failure.exception.code, 2)

    def test_idle_only_guard_inhibits_without_window_or_ui(self):
        args = types.SimpleNamespace(command=['fake-owned-trial'], output=self.root/'guard.json',
                                     ui_progress=None, idle_only=True)
        guard = window_guard.Guard(args)
        calls, snapshots = [], []
        class Connection:
            def get_unique_name(self): return ':fake.1'
            def close_sync(self, *_): calls.append('close')
            def register_object(self, *_): raise AssertionError('idle-only registered KWin controller')
        def call(service, path, interface, method, *_):
            calls.append(method)
            if service != 'org.freedesktop.ScreenSaver':
                raise AssertionError('idle-only touched KWin')
            return types.SimpleNamespace(unpack=lambda: (17,))
        guard.call = call
        original_save = guard.save
        def save():
            snapshots.append(copy.deepcopy(guard.record))
            original_save()
        guard.save = save
        child = FakeChild()
        def loop():
            guard.launched -= 20  # Idle mode must not have a UI startup deadline.
            self.assertTrue(guard.tick())
            child.returncode = 0
            self.assertFalse(guard.tick())
        guard.loop.run = loop
        with patch.object(window_guard.Gio, 'bus_get_sync', return_value=Connection(), create=True), \
             patch.object(window_guard.subprocess, 'Popen', return_value=child), \
             patch.object(window_guard.signal, 'signal', return_value=None):
            self.assertEqual(guard.run(), 0)
        self.assertEqual(calls, ['Inhibit', 'UnInhibit', 'close'])
        self.assertTrue(any(row['ready'] and row['idle_inhibit_cookie'] == 17 for row in snapshots))
        self.assertEqual(guard.record['window_guard'], {'requested': False, 'status': 'not_run'})
        self.assertTrue(guard.record['idle_inhibit_released'])
        self.assertFalse(guard.record['ready'])
        self.assertFalse(args.output.with_suffix('.js').exists())

    def test_normal_guard_retains_window_startup_gate_and_cli(self):
        args = types.SimpleNamespace(command=['fake'], output=self.root/'normal.json',
                                     ui_progress=self.root/'absent-ui.json', idle_only=False)
        guard = window_guard.Guard(args)
        guard.child = FakeChild()
        guard.launched = 0
        with patch.object(window_guard.time, 'monotonic', return_value=20), \
             patch.object(window_guard.os, 'killpg') as kill:
            guard.tick()
        self.assertTrue(guard.stopping)
        self.assertIn('15 seconds', guard.record['errors'][0])
        kill.assert_called_once_with(guard.child.pid, signal.SIGINT)
        command = ['window_guard.py', '--output', str(self.root/'cli-guard'), '--idle-only', '--', 'fake']
        with patch.object(sys, 'argv', command), patch.object(window_guard.Guard, 'run', return_value=0):
            self.assertEqual(window_guard.main(), 0)
        command.remove('--idle-only')
        with patch.object(sys, 'argv', command), patch('sys.stderr', new=io.StringIO()):
            with self.assertRaises(SystemExit) as failure:
                window_guard.main()
            self.assertEqual(failure.exception.code, 2)

if __name__ == '__main__':
    unittest.main(verbosity=2)

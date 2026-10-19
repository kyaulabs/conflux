#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""One bounded startup observation; no sustained or audio-playback qualification."""
import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import signal
import sys
import threading
import time

ENTRY = time.monotonic()
ROOT = Path(__file__).resolve().parents[2]


def utc():
    return datetime.now(timezone.utc).isoformat()


class HostSampler:
    """Sparse fixed-path sysfs readings; no subprocesses in the sample loop."""
    def __init__(self, path):
        self.path = path
        self.fields = {}
        self.guard_sensors = {}
        self.sensor_observations = {}
        self.discovery_errors = {}
        self.errors = {}
        self.samples = 0
        self.active_wall_seconds = 0.0
        self.thread_cpu_seconds = 0.0
        self.max_capture_ms = 0.0
        self.stop_event = threading.Event()
        self.thread = None
        self.stream = None
        self.first_finished = threading.Event()
        self.previous_processes = {}
        self.previous_process_at = None
        self.next_cpu_scan = 0.0
        self.cpu_scan_active_wall_seconds = 0.0
        self.cpu_scan_thread_cpu_seconds = 0.0
        self.cpu_scan_samples = 0
        self.thread_error = None
        # Match thermal-guard's complete maximum sensor set, including cores.
        # Unlike the guard, discovery/read failures are retained for coverage QA.
        for zone in Path('/sys/class/thermal').glob('thermal_zone*'):
            try:
                kind = (zone / 'type').read_text().strip()
                if kind in ('x86_pkg_temp', 'TCPU', 'acpitz'):
                    path = zone / 'temp'
                    self.guard_sensors[path] = kind
                    self.fields[path] = 'millidegrees_C'
            except OSError as error:
                self.discovery_errors[str(zone / 'type')] = f'{type(error).__name__}: {error}'
        for directory in Path('/sys/class/hwmon').glob('hwmon*'):
            try:
                if (directory / 'name').read_text().strip() == 'coretemp':
                    for path in directory.glob('temp*_input'):
                        self.guard_sensors[path] = 'coretemp'
                        self.fields[path] = 'millidegrees_C'
            except OSError as error:
                self.discovery_errors[str(directory / 'name')] = f'{type(error).__name__}: {error}'
        for name in ('scaling_cur_freq', 'energy_performance_preference'):
            self.fields[Path('/sys/devices/system/cpu/cpu0/cpufreq') / name] = 'kHz' if name.endswith('freq') else 'string'
        self.fields[Path('/sys/firmware/acpi/platform_profile')] = 'string'
        for card in Path('/sys/class/drm').glob('card[0-9]'):
            for name in ('act', 'cur', 'min', 'max'):
                path = card / f'gt_{name}_freq_mhz'
                if path.exists():
                    self.fields[path] = 'MHz'
            for path in card.glob('gt/gt*/throttle_reason_*'):
                self.fields[path] = 'kernel integer'

    def top_processes(self):
        began, cpu = time.monotonic(), time.thread_time()
        current, deltas, errors = {}, [], 0
        interval = began - self.previous_process_at if self.previous_process_at is not None else None
        for directory in Path('/proc').iterdir():
            if not directory.name.isdigit():
                continue
            try:
                raw = (directory / 'stat').read_text()
                left, _, tail = raw.rpartition(')')
                fields = tail.split()
                identity = (int(directory.name), int(fields[19]))
                ticks = int(fields[11]) + int(fields[12])
                name = left.partition('(')[2]
                current[identity] = ticks
                previous = self.previous_processes.get(identity)
                if previous is not None and ticks >= previous and interval > 0:
                    used = (ticks - previous) / os.sysconf('SC_CLK_TCK')
                    if used > 0:
                        deltas.append({'pid': identity[0], 'start_ticks': identity[1],
                            'name': name, 'cpu_seconds_delta': used,
                            'one_core_percent': used / interval * 100, 'cpu_ticks': ticks})
            except (OSError, ValueError, IndexError):
                errors += 1
        self.previous_processes = current
        self.previous_process_at = began
        self.cpu_scan_samples += 1
        self.cpu_scan_active_wall_seconds += time.monotonic() - began
        self.cpu_scan_thread_cpu_seconds += time.thread_time() - cpu
        return {'interval_seconds': interval, 'processes_observed': len(current),
                'read_errors': errors, 'top': sorted(deltas, key=lambda row: row['cpu_seconds_delta'], reverse=True)[:10],
                'capture_elapsed_ms': (time.monotonic() - began) * 1000,
                'scope': 'Top ten available /proc/stat process CPU deltas, identities checked with start_ticks; comm names only. Processes that exit between samples are omitted, so this is a sampled lower bound rather than complete CPU accounting.'}

    def start(self):
        self.stream = self.path.open('x', buffering=1)
        self.thread = threading.Thread(target=self.guarded_run, name='startup-host-observer', daemon=True)
        self.thread.start()
        if not self.first_finished.wait(2):
            raise TimeoutError('first host observer sample did not finish')
        if self.thread_error is not None:
            raise RuntimeError('first host observer sample failed: ' + self.thread_error)

    def guarded_run(self):
        try:
            self.run()
        except Exception as error:
            self.thread_error = f'{type(error).__name__}: {error}'
            self.first_finished.set()

    def run(self):
        while not self.stop_event.is_set():
            began, cpu = time.monotonic(), time.thread_time()
            values = {}
            for path, unit in self.fields.items():
                try:
                    raw = path.read_text().strip()
                    if path in self.guard_sensors and (not raw or not raw.isascii() or not raw.isdecimal()):
                        raise ValueError('guard sensor must contain nonnegative decimal integer')
                    values[str(path)] = raw if unit == 'string' else int(raw)
                    if path in self.guard_sensors:
                        observed = time.monotonic()
                        row = self.sensor_observations.setdefault(str(path), {'samples': 0, 'first_monotonic': observed, 'last_monotonic': observed, 'maximum_gap_seconds': 0.0})
                        row['maximum_gap_seconds'] = max(row['maximum_gap_seconds'], observed - row['last_monotonic'])
                        row['last_monotonic'] = observed
                        row['samples'] += 1
                except (OSError, ValueError) as error:
                    key = f'{path}: {type(error).__name__}'
                    self.errors[key] = self.errors.get(key, 0) + 1
            try:
                cpu_totals = next(line for line in Path('/proc/stat').read_text().splitlines() if line.startswith('cpu '))
                values['/proc/stat:cpu_ticks'] = list(map(int, cpu_totals.split()[1:]))
            except (OSError, ValueError, StopIteration) as error:
                key = f'/proc/stat: {type(error).__name__}'
                self.errors[key] = self.errors.get(key, 0) + 1
            process_activity = None
            if began >= self.next_cpu_scan:
                process_activity = self.top_processes()
                self.next_cpu_scan = began + 1
            now = time.monotonic()
            elapsed_ms = (now - began) * 1000
            self.stream.write(json.dumps({'timestamp_monotonic': now,
                'capture_started_monotonic': began, 'capture_elapsed_ms': elapsed_ms,
                'values': values, 'process_activity': process_activity}, separators=(',', ':')) + '\n')
            self.samples += 1
            self.max_capture_ms = max(self.max_capture_ms, elapsed_ms)
            self.active_wall_seconds += time.monotonic() - began
            self.thread_cpu_seconds += time.thread_time() - cpu
            self.first_finished.set()
            self.stop_event.wait(.25)

    def stop(self):
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join(timeout=2)
        if self.thread is not None and self.thread.is_alive():
            raise TimeoutError('host observer failed to stop')
        if self.stream is not None:
            self.stream.close()
        if self.thread_error is not None:
            raise RuntimeError('host observer stopped early: ' + self.thread_error)
        return {'interval_seconds': .25, 'samples': self.samples,
                'thread_cpu_seconds_including_output': self.thread_cpu_seconds,
                'active_wall_seconds_including_output': self.active_wall_seconds,
                'process_scan_interval_seconds': 1, 'process_scan_samples': self.cpu_scan_samples,
                'process_scan_active_wall_seconds': self.cpu_scan_active_wall_seconds,
                'process_scan_thread_cpu_seconds': self.cpu_scan_thread_cpu_seconds,
                'maximum_capture_ms_excluding_output': self.max_capture_ms,
                'fields': {str(path): unit for path, unit in self.fields.items()},
                'guard_sensor_kinds': {str(path): kind for path, kind in self.guard_sensors.items()},
                'guard_sensor_observations': self.sensor_observations,
                'discovery_errors': self.discovery_errors,
                'errors': self.errors,
                'scope': 'Sparse sampled values, not continuous temperature, effective-clock or causal proof. Host CPU cumulative ticks are separate counters in kernel USER_HZ; per-mode deltas may wrap and must not be double-counted.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('runtime', 'model', 'ui', 'audio-probe', 'source-wav', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=60, choices=(60,))
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    events = (args.output / 'events.jsonl').open('x', buffering=1)
    result = {'schema': 1, 'status': 'starting', 'errors': [], 'started_at_utc': utc(),
              'entry_monotonic': ENTRY, 'threads': 2, 'seconds': args.seconds,
              'scope': 'Startup-only diagnostic with one first output then logical cancellation and idle observation. No p99, seek, sustained service, physical audio or stem playback qualification.',
              'runtime_environment': {key: os.environ.get(key) for key in ('OMP_WAIT_POLICY', 'GOMP_SPINCOUNT')},
              'initial_startup_imports': 'Standard library imports and fixed-path discovery precede sparse sampling. NumPy and production worker imports occur inside the observed interval.'}
    lock = threading.Lock()
    load = worker = native_sampler = supervisor_sampler = None
    host = HostSampler(args.output / 'host-trace.jsonl')
    native_holder = {}

    def mark(event, **fields):
        row = {'event': event, 'timestamp_monotonic': time.monotonic(), 'at_utc': utc(), **fields}
        with lock:
            events.write(json.dumps(row, allow_nan=False, separators=(',', ':')) + '\n')
        return row

    def interrupted(signum, _frame):
        result['signal'] = signum
        raise KeyboardInterrupt

    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    began = time.monotonic()
    deadline = began + args.seconds
    result['sampling_started_monotonic'] = began
    try:
        host.start()
        mark('ambient_baseline_begin', planned_until=began + 2)
        while time.monotonic() < began + 2:
            time.sleep(min(.1, max(0, began + 2 - time.monotonic())))
        mark('ambient_baseline_complete')
        mark('imports_begin')
        sys.path.insert(0, str(ROOT))
        sys.path.insert(0, str(ROOT / 'tools/benchmarks'))
        from tools.stems.worker import NativeRuntime, StemWorker
        from stem_worker import Load, json_event
        from worker_support import ProcessSampler
        from types import SimpleNamespace
        mark('imports_complete')
        spec = importlib.util.spec_from_file_location('startup_policy', ROOT / 'benchmarks-local/software-ui-2026-10-17/observed-run.py')
        policy_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(policy_module)
        result['policy_before'] = policy_module.policy()
        if result['policy_before']['profile'] != 'balanced' or result['policy_before']['holds']:
            raise RuntimeError('normal balanced/no-holds policy missing before load')

        class FirstSampleSampler(ProcessSampler):
            def __init__(self, *a, **kw):
                self.first_finished = threading.Event()
                self.successful_samples = 0
                self.observer_cpu_seconds = 0.0
                self.observer_active_wall_seconds = 0.0
                super().__init__(*a, **kw)
            def _accumulate(self, sample):
                super()._accumulate(sample)
                self.successful_samples += 1
            def _sample(self):
                began, cpu = time.monotonic(), time.thread_time()
                try:
                    super()._sample()
                finally:
                    self.observer_cpu_seconds += time.thread_time() - cpu
                    self.observer_active_wall_seconds += time.monotonic() - began
                    self.first_finished.set()
            def stop(self):
                report = super().stop()
                report['observer_cpu_seconds_including_output'] = self.observer_cpu_seconds
                report['observer_active_wall_seconds_including_output'] = self.observer_active_wall_seconds
                return report

        class ObservedNative(NativeRuntime):
            def __init__(self, *a, **kw):
                nonlocal native_sampler
                mark('native_constructor_begin')
                super().__init__(*a, **kw)
                native_holder['runtime'] = self
                self.observer_closed = False
                mark('native_popen_returned', pid=self.process.pid)
                native_sampler = FirstSampleSampler(lambda: self.process.pid if self.process.poll() is None else None,
                    .5, trace_path=args.output / 'native-trace.jsonl', include_system=False)
                native_sampler.__enter__()
                if not native_sampler.first_finished.wait(1):
                    raise TimeoutError('first native process observation did not finish')
                if native_sampler.successful_samples == 0:
                    raise RuntimeError('first native process observation failed')
                mark('native_first_sampler_attempt_complete')
            def start(self):
                mark('native_start_receive_begin')
                ready = super().start()
                mark('native_start_receive_complete', native_ready=ready)
                return ready
            def infer(self, samples, request_id):
                mark('native_infer_begin', request_id=request_id)
                output, report = super().infer(samples, request_id)
                mark('native_infer_complete', request_id=request_id, native=report)
                return output, report
            def close(self):
                mark('native_close_begin', pid=self.process.pid)
                try:
                    self.stderr.flush()
                    with (self.root / 'runtime.stderr').open('rb') as stream:
                        data = stream.read(131073)
                    if len(data) > 131072:
                        raise RuntimeError('native diagnostic stderr exceeded128KiB')
                    (args.output / 'runtime.stderr').write_bytes(data)
                finally:
                    super().close()
                    self.observer_closed = True
                mark('native_close_complete', pid=self.process.pid, returncode=self.process.returncode)

        supervisor_sampler = FirstSampleSampler(os.getpid, .5, trace_path=args.output / 'supervisor-trace.jsonl')
        supervisor_sampler.__enter__()
        # This order matches the measured soak: audio/UI become active before load()
        # causes native process creation. The worker constructor itself stays idle.
        mark('worker_constructor_begin')
        worker = StemWorker(args.runtime, args.model, threads=2, max_ready=2,
                            request_timeout=30, _backend_factory=ObservedNative)
        mark('worker_constructor_complete')
        load_args = SimpleNamespace(load_mode='both', renderer='software', ui=args.ui,
            audio_probe=args.audio_probe, quantum=256, load_seconds=90,
            ui_progress_file=args.output / 'ui-progress.json')
        load = Load(load_args, args.output)
        mark('load_start_begin')
        load.start()
        mark('load_start_complete', children={name: child.pid for name, child, *_ in load.children})
        native_not_before = time.monotonic() + 10
        mark('loaded_baseline_begin', planned_until=native_not_before, scope='Independent audio/UI active; no native process yet. This labeled10s diagnostic pause is not qualification timing.')
        while time.monotonic() < native_not_before:
            load.check()
            time.sleep(min(.1, max(0, native_not_before - time.monotonic())))
        mark('loaded_baseline_complete')
        generation = worker.load(args.source_wav)
        mark('source_load_issued', generation=generation)
        first = None
        while time.monotonic() < deadline:
            load.check()
            event = worker.poll(min(.1, max(0, deadline - time.monotonic())))
            if event is None:
                continue
            received = time.monotonic()
            record = json_event(event)
            record['consumed_at'] = received
            mark('worker_event', worker=record)
            if event['event'] == 'error':
                raise RuntimeError(event['message'])
            if event['event'] == 'chunk' and first is None:
                if not worker.is_current(event):
                    raise RuntimeError('first chunk was stale')
                first = {'generation': generation, 'ready_at': event['ready_at'],
                         'consumed_at': received, 'hop_service_ms': event['hop_service_ms'],
                         'samples_sha256': record['samples_sha256'], 'frames': event['frames']}
                result['first_output'] = first
                cancelled = worker.cancel()
                mark('logical_cancel_after_first_output', generation=cancelled)
        mark('observation_end')
        if first is None:
            raise TimeoutError('no first output in the fixed diagnostic interval')
        result['status'] = 'ok'
    except KeyboardInterrupt:
        result['status'] = 'interrupted'
    except Exception as error:
        result['status'] = 'error'
        result['errors'].append(f'{type(error).__name__}: {error}')
    finally:
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        for name, sampler in (('native_observer', native_sampler), ('supervisor_observer', supervisor_sampler)):
            if sampler is not None:
                try:
                    result[name] = sampler.stop()
                except Exception as error:
                    result['errors'].append(f'{name}: {type(error).__name__}: {error}')
        if worker is not None:
            result['worker_before_close'] = worker.statistics()
            try:
                worker.close(timeout=.1)
                result['worker_after_close'] = worker.statistics()
            except Exception as error:
                result['errors'].append(f'worker cleanup: {type(error).__name__}: {error}')
        # A failed factory may not yet have transferred ownership to StemWorker.
        native = native_holder.get('runtime')
        if native is not None and not native.observer_closed:
            try:
                native.close()
            except Exception as error:
                result['errors'].append(f'factory child cleanup: {type(error).__name__}: {error}')
        if load is not None:
            try:
                result['load'] = load.stop()
                result['errors'].extend(result['load']['errors'])
            except Exception as error:
                result['errors'].append(f'load cleanup: {type(error).__name__}: {error}')
        try:
            result['policy_after'] = policy_module.policy() if 'policy_module' in locals() else None
        except Exception as error:
            result['errors'].append(f'final policy observation: {type(error).__name__}: {error}')
        try:
            result['host_observer'] = host.stop()
        except Exception as error:
            result['errors'].append(f'host observer: {type(error).__name__}: {error}')
        coverage_errors = []
        for name in ('native_observer', 'supervisor_observer'):
            report = result.get(name) or {}
            if report.get('samples', 0) < 1 or not report.get('processes'):
                coverage_errors.append(name + ': no successful process observations')
            if report.get('sample_errors') or report.get('trace', {}).get('errors'):
                coverage_errors.append(name + ': sampler/trace errors recorded')
            if report.get('trace', {}).get('samples_written', 0) < 1:
                coverage_errors.append(name + ': no successful trace rows')
        report = result.get('host_observer') or {}
        sensors = report.get('guard_sensor_kinds') or {}
        observations = report.get('guard_sensor_observations') or {}
        if not sensors:
            coverage_errors.append('host_observer: no guard sensors discovered')
        if report.get('errors') or report.get('discovery_errors'):
            coverage_errors.append('host_observer: discovery/read errors recorded')
        first_output = (result.get('first_output') or {}).get('consumed_at')
        # Diagnostic coverage thresholds, not a thermal or performance gate:
        # at least ten readings per discovered guard sensor, before the ambient
        # baseline ends and through first output, with no gap over one second.
        for path in sensors:
            observed = observations.get(path) or {}
            if (observed.get('samples', 0) < 10
                    or observed.get('first_monotonic', float('inf')) > began + 2
                    or first_output is None
                    or observed.get('last_monotonic', 0) < first_output
                    or observed.get('maximum_gap_seconds', float('inf')) > 1):
                coverage_errors.append('host_observer: inadequate sensor coverage: ' + path)
        result['diagnostic_coverage'] = {
            'status': 'usable' if not coverage_errors and not result['errors'] else 'unusable',
            'errors': coverage_errors,
            'minimum_successful_native_supervisor_samples': 1,
            'minimum_successful_samples_per_guard_sensor': 10,
            'maximum_guard_sensor_sample_gap_seconds': 1,
            'scope': 'Coverage QA for startup phase observations only. A captured subset with missing coverage is diagnostically unusable for attributing startup heat; even usable sparse snapshots do not identify cause or prove continuous temperature.'}
        result['errors'].extend(coverage_errors)
        result.update(finished_monotonic=time.monotonic(), finished_at_utc=utc())
        result['elapsed_seconds'] = result['finished_monotonic'] - began
        if result['errors']:
            result['status'] = 'error'
        events.close()
        (args.output / 'result.json').write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    print(json.dumps({'status': result['status'], 'result': str(args.output / 'result.json'), 'errors': result['errors']}), flush=True)
    return 0 if result['status'] == 'ok' else 130 if result['status'] == 'interrupted' else 1


if __name__ == '__main__':
    raise SystemExit(main())

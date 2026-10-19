#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Finite sequential four-track CPU/Vulkan preparation; no host setting changes."""
from datetime import datetime, timezone
import hashlib, json, os, pathlib, re, signal, subprocess, sys, time, wave

BASE = pathlib.Path(__file__).resolve().parent
ROOT = BASE.parents[1]
START = time.monotonic()
MAXIMUM_SECONDS = 5000
EXPECTED_HEAD = 'f4dc5eef82ee42b7ebe052ab449780fd4c0a2e02'
RUNTIME = pathlib.Path('/tmp/conflux-stem-m0/conflux-stem-runtime-blocking')
MODEL = pathlib.Path('/tmp/conflux-stem-m0/models/htdemucs-f16.gguf')
EXPECTED = {
    str(RUNTIME): '8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6',
    str(MODEL): 'f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6',
    'tools/stems/worker.py': '308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8',
    'tools/benchmarks/worker_support.py': '8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b',
    'tools/benchmarks/stem_worker.py': '87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7',
}
HELPERS = {
    'quality_run.py': '1773500b26bd1a9ed584decba453005f72c64d770663f03cc7d13d01646af719',
    'normal_ready.py': '4e94000d5966357c653a6ef64729f9b513df723ce0badb17e496d4b85c07135f',
}
state_path = BASE / 'sequence.json'
for name in ('sequence.json', 'cpu', 'vulkan', 'cpu-normal-ready.json',
             'cpu-normal-ready.jsonl', 'vulkan-normal-ready.json', 'vulkan-normal-ready.jsonl'):
    if (BASE / name).exists():
        raise FileExistsError('Use a fresh sequence: ' + name)
state = {'status': 'running', 'started_at_utc': datetime.now(timezone.utc).isoformat(),
         'maximum_seconds': MAXIMUM_SECONDS, 'backend_maximum_seconds': 1800,
         'source_head': EXPECTED_HEAD, 'completed_backends': [], 'launches': [],
         'profile_mutation': False,
         'scope': 'Four user-provided excerpts, sequential CPU then Vulkan; no UI/audio playback load, analysis child or sustained latency qualification.'}
child = None


def save(**fields):
    state.update(fields, elapsed_seconds=time.monotonic() - START,
                 updated_at_utc=datetime.now(timezone.utc).isoformat())
    state_path.write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps(fields), flush=True)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def host():
    ac = pathlib.Path('/sys/class/power_supply/AC/online').read_text().strip()
    lid = subprocess.check_output(['busctl', '--system', 'get-property',
          'org.freedesktop.login1', '/org/freedesktop/login1',
          'org.freedesktop.login1.Manager', 'LidClosed'], text=True, timeout=5).strip()
    if ac != '1' or lid != 'b false':
        raise RuntimeError('AC/open-lid conditions absent')
    return {'AC': ac, 'lid': lid, 'at_utc': datetime.now(timezone.utc).isoformat()}


def command(args, maximum):
    global child
    current_host = host()  # Fresh check immediately before every child spawn.
    remaining = START + MAXIMUM_SECONDS - 30 - time.monotonic()
    if remaining <= 0:
        raise TimeoutError('Finite sequence deadline reached')
    launch = {'stage': state['stage'], 'host': current_host,
              'maximum_seconds': min(maximum, remaining),
              'started_monotonic': time.monotonic()}
    state['launches'].append(launch)
    save(spawn_host=current_host)
    child = subprocess.Popen([sys.executable, '-B', *map(str, args)],
                             cwd=ROOT, start_new_session=True)
    launch['pid'] = child.pid
    code = child.wait(timeout=min(maximum, remaining))
    launch.update(returncode=code, finished_monotonic=time.monotonic())
    if code:
        raise RuntimeError('Child returned ' + str(code))
    child = None


def verify_producers():
    if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip() != EXPECTED_HEAD:
        raise RuntimeError('Source HEAD changed')
    actual = {name: digest(pathlib.Path(name) if pathlib.Path(name).is_absolute() else ROOT / name)
              for name in EXPECTED}
    if actual != EXPECTED:
        raise RuntimeError('Frozen runtime/model/worker/sampler changed')
    if {name: digest(BASE / name) for name in HELPERS} != HELPERS:
        raise RuntimeError('Reviewed driver/readiness helper changed')
    return actual


def input_pins(manifest):
    plan = manifest.get('excerpt_plan', {})
    required = {'source_offset_seconds': 30.0, 'duration_seconds': 23.4,
                'frames': 1031940, 'rate': 44100, 'channels': 2, 'sample_bytes': 2}
    if any(plan.get(key) != value for key, value in required.items()):
        raise ValueError('Manifest excerpt geometry differs from the four-track plan')
    tracks = manifest['tracks']
    if not isinstance(tracks, list) or len(tracks) != 4:
        raise ValueError('Exactly four tracks are required')
    pins = {}
    for track in tracks:
        identifier = track['id']
        if (not isinstance(identifier, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,63}', identifier)
                or identifier in pins):
            raise ValueError('Track IDs must be unique safe file-name components')
        path = pathlib.Path(track['input_wav'])
        path = (BASE / path).resolve() if not path.is_absolute() else path.resolve()
        with wave.open(str(path), 'rb') as audio:
            if audio.getparams()[:3] != (2, 2, 44100) or audio.getnframes() != 1031940 or audio.getcomptype() != 'NONE':
                raise ValueError('Prepared WAV geometry differs: ' + identifier)
        pins[identifier] = {'path': str(path), 'bytes': path.stat().st_size,
                            'sha256': digest(path), 'frames': 1031940,
                            'sample_rate': 44100, 'channels': 2, 'sample_bytes': 2}
    return pins


def stop(_signum, _frame):
    if child is not None:
        try:
            os.killpg(child.pid, signal.SIGINT)
        except ProcessLookupError:
            pass
    raise KeyboardInterrupt


for signum in (signal.SIGINT, signal.SIGTERM):
    signal.signal(signum, stop)
try:
    actual = verify_producers()
    manifest = BASE / 'manifest.json'
    manifest_sha = digest(manifest)
    sources = input_pins(json.loads(manifest.read_text()))
    save(producer_pins=actual, manifest_sha256=manifest_sha,
         driver_sha256=HELPERS['quality_run.py'], helper_pins=HELPERS, input_pins=sources)
    for backend in ('cpu', 'vulkan'):
        save(stage='normal readiness ' + backend, host=host())
        command([BASE / 'normal_ready.py', backend], 620)
        if digest(manifest) != manifest_sha or input_pins(json.loads(manifest.read_text())) != sources:
            raise RuntimeError('Experiment manifest/prepared inputs changed')
        save(stage='workload ' + backend, host=host(), admitted_producer_pins=verify_producers())
        command([BASE / 'quality_run.py', '--manifest', manifest, '--runtime', RUNTIME,
                 '--model', MODEL, '--backend', backend, '--threads', '4',
                 '--request-timeout', '120', '--output', BASE / backend], 1820)
        result = json.loads((BASE / backend / 'result.json').read_text())
        verify_producers()
        for key, name in [('runtime', str(RUNTIME)), ('model', str(MODEL)),
                          ('worker_source', 'tools/stems/worker.py'),
                          ('sampler_source', 'tools/benchmarks/worker_support.py'),
                          ('shared_benchmark_source', 'tools/benchmarks/stem_worker.py')]:
            if result['pins'][key]['sha256'] != EXPECTED[name]:
                raise RuntimeError('Captured run pin differs: ' + key)
        if (result['pins']['manifest']['sha256'] != manifest_sha
                or result['pins']['driver']['sha256'] != HELPERS['quality_run.py']):
            raise RuntimeError('Captured experiment pin differs')
        if (result['status'] != 'ok' or result['failures'] or not result['worker_statistics']['closed']
                or result['worker_statistics']['backend_pid'] is not None):
            raise RuntimeError('Unsuccessful or unclosed worker')
        if [row['id'] for row in result['tracks']] != list(sources):
            raise RuntimeError('Result track count/order differs')
        for track in result['tracks']:
            continuous, output = track['continuous'], track['output']
            if (track['input'] != sources[track['id']] or not output['complete']
                    or output['shape'] != [4, 1031940, 2] or output['dtype'] != '<f4'
                    or continuous['start_frame'] != 0 or continuous['frames'] != 1031940
                    or continuous['chunks'] != 4):
                raise RuntimeError('Incomplete or changed four-track result: ' + track['id'])
        if (len(result['seeks']) != (3 if backend == 'vulkan' else 0)
                or any(not row['numeric_parity'] for row in result['seeks'])):
            raise RuntimeError('Expected first-track seek checks did not pass')
        pids = [int(row['runtime']['pid']) for row in result['runtime_ready']]
        if any(pathlib.Path('/proc', str(pid)).exists() for pid in pids):
            raise RuntimeError('Owned backend PID remains')
        state['completed_backends'].append(backend)
        save(stage='completed ' + backend)
    save(status='completed', stage='report and listening pending')
except BaseException as error:
    save(status='stopped', error=type(error).__name__ + ': ' + str(error))
    raise
finally:
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, signal.SIG_IGN)
    if child is not None:
        try:
            os.killpg(child.pid, signal.SIGINT)
        except ProcessLookupError:
            pass
        try:
            child.wait(timeout=10)  # Finish before window_guard's 20-second hard stop.
        except subprocess.TimeoutExpired:
            pass
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait(timeout=5)

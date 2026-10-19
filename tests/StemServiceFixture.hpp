// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "StemExportFixture.hpp"

namespace conflux::test {
struct StemServiceFixture {
    library::StemEngine engine;
    QString service;
    QString marker(const char* suffix) const
    { return service.left(service.size() - 3) + QString::fromLatin1(suffix); }
};

// Independent stdlib-only child. It imports no product worker, runs no model,
// and makes actual immutable role-major slots/JSONL through actual OS pipes.
inline StemServiceFixture makeStemServiceFixture(const QString& directory,
                                                const QByteArray& mode = "good")
{
    StemServiceFixture fixture{makeStemEngineFixture(directory), QDir(directory).filePath("service.py")};
    fixture.engine.maxSeconds = 120;
    fixture.engine.requestTimeout = 2;
    fixture.engine.maxDiagnosticBytes = 4096;
    QByteArray script = "MODE = '" + mode + "'\n";
    script += R"PY(import argparse, hashlib, json, math, os, pathlib, selectors, signal, struct, subprocess, sys, threading, time
P = pathlib.Path(__file__)
ROLES = ['drums', 'bass', 'other', 'vocals']
HOP, WINDOW = 257985, 343980
wire_bytes = 0
def marker(suffix): return P.with_suffix(suffix)
def identity(path):
    h = hashlib.sha256(); size = 0
    with pathlib.Path(path).open('rb') as f:
        while b := f.read(65536): h.update(b); size += len(b)
    return {'sha256': h.hexdigest(), 'bytes': size}
def trace(value):
    value = dict(value, fixture_pid=os.getpid(), fixture_start_ticks=start_ticks, fixture_job=str(job))
    with marker('.trace').open('a') as f: f.write(json.dumps(value, separators=(',', ':')) + '\n')
def emit(value):
    global wire_bytes
    value = dict(v=1, **value)
    data = (json.dumps(value, separators=(',', ':'), allow_nan=False) + '\n').encode()
    assert len(data) <= 16384
    sys.stdout.buffer.write(data); sys.stdout.buffer.flush()
    wire_bytes += len(data)
def describe():
    return {'schema': 1, 'numpy_version': 'fixture-1',
        'input': {'format': 'f32le', 'sample_rate': 44100, 'channels': 2},
        'window_frames': WINDOW, 'hop_frames': HOP, 'overlap_frames': 85995,
        'normalization': 'track-mono-sample-variance-f32-v1', 'overlap_add': 'fixed-grid-triangle-f32-v1',
        'helper_sha256': identity(P.with_name('exporter.py'))['sha256'],
        'worker_sha256': identity(P.with_name('worker.py'))['sha256'],
        'service_sha256': identity(P)['sha256']}
if sys.argv[1:] == ['--describe']:
    print(json.dumps(describe())); sys.exit(0)
ap = argparse.ArgumentParser()
for name in ['input', 'directory', 'runtime', 'model', 'backend']:
    ap.add_argument('--' + name, required=True)
for name in ['threads', 'max-input-bytes', 'max-job-bytes', 'max-diagnostic-bytes']:
    ap.add_argument('--' + name, required=True, type=int)
for name in ['max-seconds', 'request-timeout']:
    ap.add_argument('--' + name, required=True, type=float)
a = ap.parse_args()
deadline = time.monotonic() + a.max_seconds
job = pathlib.Path(a.directory); job.mkdir(mode=0o700)
start_ticks = pathlib.Path('/proc/self/stat').read_text().rsplit(') ', 1)[1].split()[19]
marker('.started').write_text(str(os.getpid()))
marker('.job').write_text(str(job))
trace({'fixture': 'started'})
stop = False
def stopping(signum, frame):
    global stop
    stop = True
signal.signal(signal.SIGINT, stopping); signal.signal(signal.SIGTERM, stopping)
source = identity(a.input); n = source['bytes'] // 8
source.update(format='f32le', sample_rate=44100, channels=2, frames=n)
model, runtime = identity(a.model), identity(a.runtime)
if MODE == 'exit-before-ready': sys.exit(3)
if MODE == 'error-before-ready':
    emit({'event': 'error', 'generation': 0, 'code': 'RuntimeError', 'message': 'fixture setup failure', 'retained_job': False})
    sys.exit(1)
while MODE == 'ready-gate' and not marker('.release').exists() and not stop and time.monotonic() < deadline: time.sleep(.005)
if stop: sys.exit(130)
if time.monotonic() >= deadline: sys.exit(124)
reservation = source['bytes'] + 2 * 8255520 + 13759200 + a.max_diagnostic_bytes
ready = {'event': 'ready', 'generation': 0, 'describe': describe(), 'source': source,
    'model': model, 'runtime': runtime,
    'limits': {'max_input_bytes': a.max_input_bytes, 'max_job_bytes': a.max_job_bytes,
        'max_seconds': a.max_seconds, 'request_timeout': a.request_timeout,
        'max_diagnostic_bytes': a.max_diagnostic_bytes}, 'reservation_bytes': reservation}
if MODE == 'ready-model': ready['model'] = dict(model, sha256='0' * 64)
if MODE == 'ready-source': ready['source'] = dict(source, sha256='0' * 64)
if MODE == 'ready-helper': ready['describe']['helper_sha256'] = '0' * 64
if MODE == 'ready-extra': ready['extra'] = True
if MODE == 'ready-missing': del ready['model']
if MODE == 'ready-bool-frames': ready['source'] = dict(source, frames=True)
if MODE == 'truncated-ready':
    sys.stdout.buffer.write(json.dumps(dict(v=1, **ready)).encode()); sys.stdout.buffer.flush(); sys.exit(0)
emit(ready)
generation = 0; sequence = 0; cursor = 0; active = False
runtime_seen = False; source_seen = False; complete = False; held_ack = False
leases = [None, None]; chunks = 0; saw_two = False
selector = selectors.DefaultSelector(); selector.register(sys.stdin.buffer, selectors.EVENT_READ)
pending = bytearray(); reason = 'close'; child = None
if MODE in ['descendant', 'dead-leader']:
    child = subprocess.Popen([sys.executable, '-B', '-c', 'import time; time.sleep(120)'])
    fields = pathlib.Path('/proc/%d/stat' % child.pid).read_text().rsplit(') ', 1)[1].split()
    marker('.native-start').write_text(fields[19])
    marker('.native').write_text(str(child.pid))
def runtime_event(tag):
    emit({'event': 'runtime', 'worker_generation': tag, 'model_id': model['sha256'],
        'ready': {'sample_rate': 44100, 'channels': 2, 'segment_frames': WINDOW,
            'stems': ROLES, 'backend': a.backend, 'threads': a.threads, 'revision': 'a' * 40,
            'weight_storage': 'native', 'fence_wait_mode': 'blocking'}})
    trace({'fixture': 'runtime', 'generation': tag})
def publish():
    global cursor, sequence, chunks, saw_two
    slot = leases.index(None); frames = min(HOP - cursor % HOP, n - cursor)
    leaf = job / ('slot%d.f32' % slot)
    with leaf.open('xb') as out:
        for role in range(4):
            with pathlib.Path(a.input).open('rb') as src:
                src.seek(cursor * 8); remaining = frames
                while remaining:
                    count = min(4096, remaining); raw = src.read(count * 8)
                    assert len(raw) == count * 8
                    values = struct.unpack('<%df' % (count * 2), raw)
                    result = [value * (role + 1) / 4 for value in values]
                    if MODE == 'nonfinite' and role == 2: result[0] = float('nan')
                    if MODE == 'late-nonfinite' and role == 2 and remaining == count: result[-1] = float('nan')
                    out.write(struct.pack('<%df' % len(result), *result)); remaining -= count
    value = identity(leaf); sequence += 1
    event = {'event': 'chunk', 'slot': slot, 'sequence': sequence, 'generation': generation,
        'source_id': source['sha256'], 'model_id': model['sha256'], 'file': leaf.name,
        'source_frames': n, 'source_format': 'f32le', 'sample_rate': 44100, 'channels': 2,
        'stems': ROLES, 'frame_start': cursor, 'window_start_frame': cursor // HOP * HOP,
        'frames': frames, 'final': cursor + frames == n, 'bytes': value['bytes'], 'sha256': value['sha256'],
        'request_id': sequence, 'worker_ready_at': time.monotonic(), 'published_at': time.monotonic()}
    if MODE in ['timing', 'timing-no-hop']:
        event['worker_ready_at'] = 1000.25; event['published_at'] = 1000.375
        if MODE == 'timing': event['hop_service_ms'] = 125.0
    if MODE == 'wrong-hash': event['sha256'] = '0' * 64
    if MODE == 'wrong-origin': event['frame_start'] += 1
    if MODE == 'wrong-final': event['final'] = not event['final']
    if MODE == 'wrong-order': event['stems'] = list(reversed(ROLES))
    if MODE == 'sequence-gap': event['sequence'] += 1
    if MODE == 'chunk-bool-frames': event['frames'] = True
    if MODE == 'short':
        with leaf.open('r+b') as out: out.truncate(value['bytes'] - 4)
    if MODE == 'changed-lease':
        # Deterministic metadata-before-mutation; no scheduler-sensitive race.
        with leaf.open('r+b') as out: out.write(struct.pack('<f', .8125))
    if MODE in ['fifo', 'symlink']:
        leaf.unlink()
        if MODE == 'fifo':
            os.mkfifo(leaf, 0o600)
            # Escape only a broken blocking-open implementation after its10s
            # functional oracle has failed; avoid stranding the test process.
            def unblock_broken_reader():
                try:
                    fd = os.open(leaf, os.O_RDWR | os.O_NONBLOCK); os.close(fd)
                except OSError: pass
            escape = threading.Timer(15, unblock_broken_reader)
            escape.daemon = True; escape.start()
        else: leaf.symlink_to(marker('.outside'))
    lease = {key: event[key] for key in ['slot', 'sequence', 'generation', 'source_id', 'model_id']}
    leases[slot] = lease; cursor += frames; chunks += 1
    emit(event); trace({'fixture': 'chunk', **lease, 'end': cursor})
    if chunks == 1: marker('.first').touch()
    if all(lease is not None for lease in leases) and not saw_two:
        saw_two = True; trace({'fixture': 'two_leases'})
try:
    while not stop:
        if MODE == 'dead-leader' and marker('.exit').exists():
            # Deliberately bypass finally: the parent must finish the already
            # known group after its direct leader has exited.
            os._exit(7)
        if time.monotonic() >= deadline:
            emit({'event': 'error', 'generation': generation, 'code': 'TimeoutError',
                  'message': 'fixture lifetime deadline', 'retained_job': False})
            reason = 'error'; break
        if held_ack:
            # No later command can overtake the still-unprocessed ACK. A stop
            # signal ends this fixture with its original lease outstanding.
            time.sleep(.005); continue
        if selector.select(.005):
            data = os.read(sys.stdin.fileno(), 4096)
            if not data: reason = 'eof'; break
            pending.extend(data)
        closing = False
        for _ in range(8):
            end = pending.find(b'\n')
            if end < 0: break
            command = json.loads(pending[:end]); del pending[:end + 1]
            kind = command['command']; trace(command)
            accepted = {'event': 'accepted', 'id': command['id'], 'command': kind,
                        'generation': generation, 'accepted_at': time.monotonic()}
            if kind in ['load', 'seek', 'cancel']:
                generation += 1
                if MODE == 'accepted-gap': generation += 1
                if MODE == 'accepted-reset' and generation > 1: generation = 1
                accepted['generation'] = generation
                active = kind != 'cancel'; source_seen = complete = False
                if active:
                    cursor = command['frame']; accepted['frame'] = cursor
                if MODE.startswith('late-runtime-') and generation == 1: marker('.load-wait').touch()
            elif kind == 'ack':
                if MODE == 'ack-held':
                    held_ack = True; marker('.held').touch(); break
                slot = command['slot']; lease = leases[slot]
                assert lease is not None
                assert all(command[key] == lease[key] for key in lease)
                (job / ('slot%d.f32' % slot)).unlink(); leases[slot] = None
                accepted.update(lease, current_generation=generation)
                if MODE == 'bad-ack': accepted['sequence'] += 1
            elif kind == 'close': closing = True
            else: raise AssertionError('unexpected fixture command')
            emit(accepted)
            if closing: break
        if closing: break
        if MODE.startswith('late-runtime-') and generation > 1 and not runtime_seen:
            runtime_seen = True
            runtime_event(1 if MODE == 'late-runtime-old' else generation)
        if not active: continue
        if MODE.startswith('late-runtime-') and generation == 1: continue
        if not runtime_seen:
            if MODE == 'stderr-flood':
                sys.stderr.buffer.write(b'x' * 131072); sys.stderr.buffer.flush()
            runtime_seen = True
            tag = generation + 1 if MODE == 'runtime-future' else generation
            runtime_event(tag)
        if not source_seen:
            source_seen = True
            source_id = identity(P.with_name('source.wav'))['sha256'] if MODE == 'source-original' else source['sha256']
            tag = generation - 1 if MODE == 'stale-after-seek' and generation > 1 else generation
            # Deliberately nonidentity diagnostics; the raw fake stem files are
            # ALREADY restored PCM. The receiver must not apply these again.
            emit({'event': 'source', 'generation': tag, 'source_id': source_id,
                'source_frames': n, 'source_format': 'f32le', 'mean': .375, 'std': .0625})
        if MODE == 'error-then-clean':
            emit({'event': 'error', 'generation': generation, 'code': 'RuntimeError',
                  'message': 'fixture primary error', 'retained_job': False}); break
        if MODE == 'complete-early' and not complete:
            emit({'event': 'complete', 'generation': generation, 'source_id': source['sha256'],
                  'frame_start': n, 'telemetry_dropped': 7, 'completed_at': time.monotonic()})
            complete = True
        if MODE == 'unexpected-closed': break
        if MODE == 'unknown-event':
            emit({'event': 'surprise'}); break
        gated = MODE == 'gate-after-first' and chunks >= 1 and not marker('.release').exists()
        if not gated and cursor < n and None in leases: publish()
        if cursor == n and not complete and any(lease is None for lease in leases):
            complete = True
            emit({'event': 'complete', 'generation': generation, 'source_id': source['sha256'],
                  'frame_start': n, 'telemetry_dropped': 7, 'completed_at': time.monotonic()})
            trace({'fixture': 'complete', 'generation': generation})
finally:
    selector.close()
    if MODE == 'preserve-job': (job / 'unknown-owner').write_bytes(b'preserve this fixed job')
    if child is not None:
        child.terminate()
        try: child.wait(timeout=2)
        except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=2)
    trace({'fixture': 'closed', 'outstanding_slots': sum(lease is not None for lease in leases),
           'wire_bytes_before_closed': wire_bytes})
    actual_outstanding = sum(lease is not None for lease in leases)
    emit({'event': 'closed', 'generation': generation,
        'reason': 'signal' if stop else reason, 'worker_closed': True,
        'outstanding_slots': (actual_outstanding + 1) % 3 if MODE == 'bad-closed-count' else actual_outstanding,
        'telemetry_dropped': 7, 'closed_at': time.monotonic()})
)PY";
    writeFixtureBytes(fixture.service, script);
    writeFixtureBytes(fixture.marker(".outside"), "outside sentinel must survive\n");
    return fixture;
}
} // namespace conflux::test

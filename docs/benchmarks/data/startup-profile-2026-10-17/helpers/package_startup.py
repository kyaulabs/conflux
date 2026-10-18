#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stage and verify explicit media-free closed startup evidence; never run a workload."""
import argparse
import contextlib
import gzip
import hashlib
import json
from pathlib import Path

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
HEAD = '825841849ea157272a7b27e8481d9a8565c47cb6'
LABEL = 'startup-observe'
REPEAT = 'threads2-both-repeat-10min'
REPEAT_BASE = ROOT / 'benchmarks-local/two-thread-music-2026-10-17'
CANONICAL = 'docs/benchmarks/data/startup-profile-2026-10-17'
EXPECTED_CI = {'push': 39091127963, 'pull_request': 39091130736}
REFERENCES = {
    'prior_two_thread_archive': 'docs/benchmarks/data/two-thread-music-2026-10-17/archive-manifest.json',
    'prior_two_thread_launcher': 'docs/benchmarks/data/two-thread-music-2026-10-17/helpers/observed_launch.py',
    'prior_expected_pins': 'docs/benchmarks/data/two-thread-music-2026-10-17/expected-pins.json',
    'manifest': 'docs/benchmarks/data/user-tracks-2026-10-17/series/manifest.json',
    'runtime_build': 'docs/benchmarks/data/stem-quality-2026-10-17/runtime/historical-build.json',
    'UI_audio_baseline': 'docs/benchmarks/data/software-ui-2026-10-17/software-33ms-hour/result.json',
    'soak_source': 'docs/benchmarks/data/paced-music-2026-10-17/source-snapshots/tools/benchmarks/stem_soak.py',
    'worker_source': 'docs/benchmarks/data/paced-music-2026-10-17/source-snapshots/tools/stems/worker.py',
    'sampler_source': 'docs/benchmarks/data/stem-quality-2026-10-17/source-snapshots/tools/benchmarks/worker_support.py',
    'shared_benchmark_source': 'docs/benchmarks/data/stem-quality-2026-10-17/source-snapshots/tools/benchmarks/stem_worker.py',
    'admission_source': 'docs/benchmarks/data/music-profile-2026-10-17/helpers/interleaved_replay.py',
    'native_replay_source': 'docs/benchmarks/data/music-profile-2026-10-17/helpers/native_replay.py',
    'window_guard_source': 'docs/benchmarks/data/music-profile-2026-10-17/helpers/window_guard.py',
    'readiness_source': 'docs/benchmarks/data/user-tracks-2026-10-17/helpers/normal_ready.py',
    'observer_source': 'docs/benchmarks/data/software-ui-2026-10-17/source-snapshots/benchmarks-local/software-ui-2026-10-17/observed-run.py',
}
SERIES = ('expected-pins.json', 'source-verification.json', 'source-CI.json',
          'startup-analysis.json', 'independent-startup-check.json',
          'm0-m1-decision.json', 'm0-m1-decision-proposal.md',
          'repeat-guard-after.json', 'repeat-guard-journal.json')
HELPERS = ('startup_probe.py', 'observed_launch.py', 'package_startup.py')
GUARD = ('inspection.json', 'manifest.json', 'recent-journal.json', 'service-show.txt',
         'thermal-guard', 'thermal-guard.service')
SCOPE = ('Closed60s startup diagnostic with explicit ambient/import/loaded/native phases and logical cancellation after first output. '
         'Integrity and saved-evidence reduction only: no model, runtime/UI/audio binary, music, WAV, PCM, NumPy array, artwork or HTML. '
         'Eight measured producer hashes and four prepared source hashes refer to prior canonical evidence, without copying media or recomputing separation. '
         'Original raw fields and descriptor-read errors remain unchanged. The aborted repeat stopped during readiness without a measured workload; its truncated final readiness JSON is retained as opaque partial bytes. '
         'The exact user-approved feasibility/manual-M1 decision is preserved with the failed live-stem target retained as an M2 blocker. No new numeric latency pass, causal startup fix, physical audio or completed M1 claim.')


def require(value, message):
    if not value:
        raise ValueError(message)


def pin(path):
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'bytes': path.stat().st_size, 'sha256': digest}


def invalid(value):
    raise ValueError('nonfinite JSON value: ' + value)


def read(path):
    data = path.read_bytes()
    return json.loads(gzip.decompress(data) if path.suffix == '.gz' else data, parse_constant=invalid)


def rows(path):
    data = gzip.decompress(path.read_bytes()) if path.suffix == '.gz' else path.read_bytes()
    return [json.loads(line, parse_constant=invalid) for line in data.splitlines() if line.strip()]


def write(path, value):
    with path.open('x') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')


def members():
    paths = {name: BASE / name for name in SERIES}
    paths.update({'helpers/' + name: BASE / name for name in HELPERS})
    paths.update({'guard-inspection/' + name: BASE / 'guard-inspection' / name for name in GUARD})
    for suffix in ('launch.json', 'guard.json', 'guard.js', 'guard-after.json', 'guard-journal.json', 'AC-lid-monitor.jsonl'):
        paths[LABEL + '-' + suffix] = BASE / (LABEL + '-' + suffix)
    for suffix in ('stdout', 'stderr'):
        paths[LABEL + '.' + suffix] = BASE / (LABEL + '.' + suffix)
    for name in ('admission-normal-ready.json', 'admission-normal-ready.jsonl', 'fresh-admission.json', 'stdout', 'stderr'):
        paths[LABEL + '-admission/' + name] = BASE / (LABEL + '-admission') / name
    for name in ('result.json', 'events.jsonl', 'host-trace.jsonl', 'native-trace.jsonl', 'supervisor-trace.jsonl',
                 'runtime.stderr', 'audio.json', 'audio.stderr', 'ui.json', 'ui.stderr', 'ui-progress.json'):
        paths[LABEL + '/' + name] = BASE / LABEL / name
    for suffix in ('launch.json', 'stop-decision.json', 'AC-lid-monitor.jsonl'):
        paths['aborted-repeat/' + REPEAT + '-' + suffix] = REPEAT_BASE / (REPEAT + '-' + suffix)
    admission = REPEAT_BASE / (REPEAT + '-admission')
    # Outer workload logs are never opened when readiness is interrupted.
    for suffix in ('stdout', 'stderr'):
        paths['aborted-repeat/admission/' + suffix] = admission / suffix
    paths['aborted-repeat/admission-normal-ready.jsonl'] = admission / 'admission-normal-ready.jsonl'
    paths['aborted-repeat/admission-normal-ready.json.partial'] = admission / 'admission-normal-ready.json'
    return {(name + '.gz' if source.suffix == '.jsonl' else name): source for name, source in paths.items()}


def saved_facts(archive):
    result = read(archive / LABEL / 'result.json')
    events = rows(archive / LABEL / 'events.jsonl.gz')
    host = rows(archive / LABEL / 'host-trace.jsonl.gz')
    native = rows(archive / LABEL / 'native-trace.jsonl.gz')
    supervisor = rows(archive / LABEL / 'supervisor-trace.jsonl.gz')
    marker = lambda name: next(row['timestamp_monotonic'] for row in events if row['event'] == name)
    chunk = next(row['worker'] for row in events if row['event'] == 'worker_event' and row['worker']['event'] == 'chunk')
    kinds = result['host_observer']['guard_sensor_kinds']
    peak = max(row['values'][path] for row in host for path in kinds)
    gaps = lambda trace: max(b['timestamp_monotonic'] - a['timestamp_monotonic'] for a, b in zip(trace, trace[1:]))
    require(result['status'] == 'ok' and not result['errors'] and result['diagnostic_coverage']['status'] == 'usable', 'Startup diagnostic itself failed coverage')
    analysis = read(archive / 'startup-analysis.json')
    require(analysis['native_startup']['constructor_to_ready_ms'] == (marker('native_start_receive_complete') - marker('native_constructor_begin')) * 1000,
            'Saved startup phase reduction differs')
    require(analysis['native_startup']['first_hop_service_ms'] == chunk['hop_service_ms'] == result['first_output']['hop_service_ms'], 'First-hop timing differs')
    require(analysis['thermal']['maximum_guard_sensor_millidegrees_C'] == peak, 'Saved sensor peak differs')
    for name, trace in (('host', host), ('native', native), ('supervisor', supervisor)):
        require(analysis['traces'][name]['rows'] == len(trace) and analysis['traces'][name]['maximum_gap_seconds'] == gaps(trace), 'Saved trace coverage/gap differs: ' + name)
    return {'native_constructor_to_ready_ms': analysis['native_startup']['constructor_to_ready_ms'],
            'first_hop_service_ms': chunk['hop_service_ms'], 'sensor_peak_millidegrees_C': peak,
            'host_rows': len(host), 'native_rows': len(native), 'supervisor_rows': len(supervisor),
            'guard_sensor_count': len(kinds),
            'fd_read_errors': {name: result[name + '_observer']['processes'][0]['fd_read_errors'] for name in ('native', 'supervisor')},
            'scope': 'Selected saved facts independently recomputed. Full phase/top-process/observer fields remain original analysis evidence, not a causal replay.'}


def audit(archive, repository):
    archive, repository = archive.resolve(), repository.resolve()
    manifest = read(archive / 'archive-manifest.json')
    require(set(manifest['files']) == set(members()), 'Allowlist changed')
    require({str(path.relative_to(archive)) for path in archive.rglob('*') if path.is_file()} == set(manifest['files']) | {'archive-manifest.json'}, 'Missing or unmanifested member')
    record_count = 0
    for relative, identity in manifest['files'].items():
        path = archive / relative
        require(not path.is_symlink() and archive in path.resolve().parents and pin(path) == {key: identity[key] for key in ('bytes', 'sha256')}, 'Stored identity differs: ' + relative)
        raw = path.read_bytes()
        if relative.endswith('.gz'):
            require(raw[:2] == b'\x1f\x8b' and not raw[3] & 8 and raw[4:8] == b'\0' * 4, 'Nondeterministic gzip header')
            raw = gzip.decompress(raw)
        require(len(raw) == identity['raw_bytes'] and hashlib.sha256(raw).hexdigest() == identity['raw_sha256'], 'Raw identity differs: ' + relative)
        if 'jsonl_records' in identity:
            records = [json.loads(line, parse_constant=invalid) for line in raw.splitlines() if line.strip()]
            require(len(records) == identity['jsonl_records'], 'JSONL count differs')
            record_count += len(records)
        elif relative.endswith('.json'):
            json.loads(raw, parse_constant=invalid)
    require(set(manifest['external_references']) == set(REFERENCES), 'Prior-reference inventory differs')
    for name, relative in REFERENCES.items():
        reference = manifest['external_references'][name]
        require(reference['repository_path'] == relative and pin(repository / relative) == {key: reference[key] for key in ('bytes', 'sha256')}, 'Prior canonical reference differs: ' + name)
    expected = read(archive / 'expected-pins.json')
    require(expected['repository_head'] == HEAD and expected['status'] == 'ready' and expected['pins'] == read(repository / REFERENCES['prior_expected_pins'])['pins'], 'Signed HEAD or eight producer hashes differ')
    verification = read(archive / 'source-verification.json')
    require(verification['source_head'] == HEAD and verification['signature_status'] == 'G' and verification['source_pins'] == expected['pins'], 'Signed source proof differs')
    ci = read(archive / 'source-CI.json')
    require(ci['repository_head'] == HEAD and len(ci['runs']) == 2 and {row['event'] for row in ci['runs']} == set(EXPECTED_CI), 'CI inventory differs')
    require(all(row['headSha'] == HEAD and row['databaseId'] == EXPECTED_CI[row['event']] and row['status'] == 'completed' and row['conclusion'] == 'success' and row['url'] == 'https://github.com/kyaulabs/conflux/actions/runs/' + str(row['databaseId']) for row in ci['runs']), 'Source CI identity/outcome differs')
    launch = read(archive / (LABEL + '-launch.json'))
    require(launch['status'] == 'completed' and launch['returncode'] == 0 and launch.get('finished_at_utc') and not launch['cleanup']['remaining_owned_processes'], 'Trial not closed/cleaned')
    require(launch['expected_producer'] == expected and all(launch['pins'][key]['sha256'] == value for key, value in expected['pins'].items()), 'Executed producer differs')
    for key, helper in (('launcher', 'observed_launch.py'), ('startup_probe_source', 'startup_probe.py')):
        require(launch['pins'][key]['sha256'] == pin(archive / 'helpers' / helper)['sha256'], 'Executed helper differs: ' + key)
    for key in ('admission_source', 'native_replay_source', 'window_guard_source', 'readiness_source', 'observer_source'):
        require(launch['pins'][key]['sha256'] == manifest['external_references'][key]['sha256'], 'Executed prior helper differs: ' + key)
    corpus = read(repository / REFERENCES['manifest'])
    require(set(launch['inputs']) == {track['id'] for track in corpus['tracks']} and all(launch['inputs'][track['id']] == track['input_pin'] for track in corpus['tracks']), 'Four prepared source pins differ')
    require(manifest['measured_inputs'] == launch['inputs'] and manifest['measured_producers'] == {key: launch['pins'][key] for key in expected['pins']}, 'Manifest measured pins differ')
    independent = read(archive / 'independent-startup-check.json')
    require(independent.get('status') in ('ok', 'passed', 'valid') and not independent.get('failed_checks') and not independent.get('errors'), 'Independent startup check not passed')
    before = read(archive / 'guard-inspection/inspection.json')
    after = read(archive / (LABEL + '-guard-after.json'))
    require(after['same_as_inspection'] and after['service']['ActiveState'] == 'active' and after['service']['SubState'] == 'running' and all(after['service'][key] == before['service'][key] for key in ('MainPID', 'InvocationID', 'NRestarts', 'ExecMainStartTimestampMonotonic')) and after['start_ticks'] == before['process']['start_ticks'], 'Active guard identity changed')
    require(after['journal_records'] == len(read(archive / (LABEL + '-guard-journal.json'))), 'Bounded guard journal count differs')
    for snapshot in before['source_snapshots']:
        require(pin(archive / 'guard-inspection' / snapshot['snapshot']) == {key: snapshot[key] for key in ('bytes', 'sha256')} and after['sources'][snapshot['source_path']] == snapshot['sha256'], 'Guard source snapshot differs')
    decision = read(archive / 'm0-m1-decision.json')
    require(decision['status'] == 'approved' and decision['answer'] == 'Enter M1 manual audio; retain M2 live-stem blocker (Recommended)', 'Exact user milestone decision missing')
    repeated = read(archive / 'aborted-repeat' / (REPEAT + '-launch.json'))
    stopped = read(archive / 'aborted-repeat' / (REPEAT + '-stop-decision.json'))
    require(repeated['status'] == 'interrupted' and repeated['interruption_signal'] == 2 and repeated['child_started'] is False and repeated.get('finished_at_utc') and not repeated['cleanup']['remaining_owned_processes'] and repeated['cleanup']['last_child_reaped'], 'Aborted repeat did not stop cleanly before workload launch')
    require(stopped['signal'] == 'SIGINT' and stopped['state_status'] == 'normal_readiness' and stopped['child_started'] is False and stopped['decision'] == decision['answer'], 'Aborted repeat decision binding differs')
    require(repeated['expected_producer']['repository_head'] == HEAD and repeated['expected_producer']['pins'] == expected['pins'] and repeated['inputs'] == launch['inputs'] and repeated['pins']['launcher']['sha256'] == manifest['external_references']['prior_two_thread_launcher']['sha256'], 'Aborted repeat planned source/input binding differs')
    partial = archive / 'aborted-repeat/admission-normal-ready.json.partial'
    require(pin(partial) == {key: repeated['readiness_proof'][key] for key in ('bytes', 'sha256')}, 'Raw partial readiness bytes differ')
    try:
        json.loads(partial.read_bytes(), parse_constant=invalid)
    except json.JSONDecodeError as error:
        partial_fact = {'status': 'malformed_intentionally_preserved', 'line': error.lineno, 'column': error.colno, **pin(partial)}
    else:
        raise ValueError('Expected truncated readiness evidence changed to valid JSON; review allowlist again')
    require(manifest['known_partial_json'] == {'aborted-repeat/admission-normal-ready.json.partial': partial_fact}, 'Partial JSON metadata differs')
    repeat_after = read(archive / 'repeat-guard-after.json')
    require(repeat_after['repeat_status'] == 'interrupted' and repeat_after['child_started'] is False and not repeat_after['remaining_matching_owned'] and repeat_after['no_CONFLUX_inhibitors'] and repeat_after['launcher_cleanup'] == repeated['cleanup'], 'Actual post-repeat owned cleanup not confirmed')
    service = repeat_after['guard_service']
    require(service['ActiveState'] == 'active' and service['SubState'] == 'running' and all(service[key] == before['service'][key] for key in ('MainPID', 'InvocationID', 'NRestarts')), 'Guard identity after repeat differs')
    require(repeat_after['journal_records'] == len(read(archive / 'repeat-guard-journal.json')), 'Repeat guard journal count differs')
    return {'status': 'passed', 'files': len(manifest['files']), 'stored_bytes': sum(row['bytes'] for row in manifest['files'].values()), 'jsonl_records': record_count, 'external_references': len(REFERENCES), 'manifest_sha256': pin(archive / 'archive-manifest.json')['sha256'], 'startup_facts': saved_facts(archive), 'aborted_repeat': {'status': 'interrupted_during_readiness', 'measured_workload_launched': False, 'new_p99_result': None, 'partial_readiness': partial_fact, 'readiness_trace_rows': len(rows(archive / 'aborted-repeat/admission-normal-ready.jsonl.gz')), 'guard_journal_records': repeat_after['journal_records']}, 'scope': SCOPE}


def stage(output, approved):
    require(approved, 'Root review of final inputs required; pass --approved-inputs only after review')
    output = output.resolve()
    require(output == BASE / 'staged-archive' and not output.exists(), 'Use fresh ignored staged-archive only; root publishes after audit')
    sources = members()
    require(all(path.is_file() and not path.is_symlink() for path in sources.values()), 'An explicit required evidence member is missing')
    launch = read(BASE / (LABEL + '-launch.json'))
    require(launch['status'] == 'completed' and launch.get('finished_at_utc') and not launch['cleanup']['remaining_owned_processes'], 'Wait for final closed startup evidence')
    output.mkdir()
    files = {}
    for relative, source in sources.items():
        before = source.stat()
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        digest, raw_bytes, records = hashlib.sha256(), 0, 0
        with contextlib.ExitStack() as stack:
            inp = stack.enter_context(source.open('rb'))
            out = stack.enter_context(target.open('xb'))
            destination = stack.enter_context(gzip.GzipFile(filename='', fileobj=out, mode='wb', mtime=0, compresslevel=6)) if source.suffix == '.jsonl' else out
            for data in inp:
                digest.update(data); raw_bytes += len(data); records += bool(data.strip())
                destination.write(data)
        after = source.stat()
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'Evidence changed while staging: ' + str(source))
        files[relative] = {**pin(target), 'source': str(source), 'raw_bytes': raw_bytes, 'raw_sha256': digest.hexdigest(), 'compression': 'gzip' if source.suffix == '.jsonl' else None}
        if source.suffix == '.jsonl': files[relative]['jsonl_records'] = records
    expected = read(BASE / 'expected-pins.json')
    partial_path = REPEAT_BASE / (REPEAT + '-admission/admission-normal-ready.json')
    try:
        json.loads(partial_path.read_bytes(), parse_constant=invalid)
    except json.JSONDecodeError as error:
        partial_fact = {'status': 'malformed_intentionally_preserved', 'line': error.lineno, 'column': error.colno, **pin(partial_path)}
    else:
        raise ValueError('Truncated readiness evidence unexpectedly parses; review inputs')
    write(output / 'archive-manifest.json', {'schema': 1, 'scope': SCOPE, 'canonical_destination': CANONICAL,
          'root_approved_final_allowlist': True, 'source_head': HEAD, 'run': LABEL, 'aborted_repeat': REPEAT,
          'known_partial_json': {'aborted-repeat/admission-normal-ready.json.partial': partial_fact},
          'measured_producers': {key: launch['pins'][key] for key in expected['pins']}, 'measured_inputs': launch['inputs'],
          'external_references': {key: {'repository_path': relative, **pin(ROOT / relative)} for key, relative in REFERENCES.items()},
          'files': dict(sorted(files.items()))})
    return audit(output, ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='mode', required=True)
    staging = commands.add_parser('stage')
    staging.add_argument('--approved-inputs', action='store_true')
    staging.add_argument('--output', type=Path, default=BASE / 'staged-archive')
    checking = commands.add_parser('audit')
    checking.add_argument('archive', type=Path)
    checking.add_argument('--repository-root', type=Path, default=Path.cwd())
    checking.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = stage(args.output, args.approved_inputs) if args.mode == 'stage' else audit(args.archive, args.repository_root)
    if args.mode == 'audit' and args.output:
        require(args.archive.resolve() not in args.output.resolve().parents, 'Keep audit output outside archive')
        write(args.output, result)
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()

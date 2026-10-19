#!/usr/bin/env python3
"""Explicit media-free M0 evidence staging and saved-JSON audit; no host work."""
import argparse
from collections import Counter
import gzip
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import shutil
import statistics

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
USER = 'docs/benchmarks/data/user-tracks-2026-10-17'
STEM = 'docs/benchmarks/data/stem-quality-2026-10-17'
USER_MANIFEST = 'f4e40e1eb626b4b5929c8cddcfc13f6b221ef681e236f07f9fda4597446615d7'
IDS = ['club_xl', 'when_the_light_breaks', 'under_my_skin', 'stfu']
N, HOP, WINDOW, RATE = 1031940, 257985, 343980, 44100
FROZEN = {'runtime': '8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6',
          'model': 'f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6',
          'worker_source': '308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8',
          'sampler_source': '8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b',
          'shared_benchmark_source': '87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7'}
SERIES = ('guard-profile-facts.json', 'vulkan-hop-profile.json', 'replay-facts.json', 'worker-facts.json',
          'prelaunch-review.json', 'prelaunch-review-attempt2.json', 'prelaunch-review-attempt3.json',
          'interleaved-replay-review.json', 'interleaved-replay-admission-review.json',
          'interleaved-replay-final-admission-review.json', 'replay-guard.json',
          'replay-guard-attempt2.json', 'replay-guard-attempt3.json',
          'worker-followup-review.json', 'worker-followup-launch.json', 'worker-guard.json',
          'replay-normal-ready.json', 'replay-normal-ready.jsonl')
HELPERS = ('profile_music.py', 'interleaved_replay.py', 'interleaved_replay-attempt1.py',
           'interleaved_replay-attempt2.py', 'summarize_replay.py', 'worker_followup.py',
           'summarize_worker.py', 'package_profile.py')
REFERENCES = {'user_manifest': USER + '/archive-manifest.json', 'stem_manifest': STEM + '/archive-manifest.json',
              'manifest': USER + '/series/manifest.json', 'quality_driver': USER + '/helpers/quality_run.py',
              'readiness_source': USER + '/helpers/normal_ready.py', 'worker_summarizer': USER + '/helpers/summarize_runs.py',
              'original_result': USER + '/vulkan/result.json', 'original_events': USER + '/vulkan/events.jsonl.gz',
              'original_trace': USER + '/vulkan/native-trace.jsonl.gz', 'original_facts': USER + '/series/run-facts.json',
              'worker_source': STEM + '/source-snapshots/tools/stems/worker.py',
              'sampler_source': STEM + '/source-snapshots/tools/benchmarks/worker_support.py',
              'shared_benchmark_source': STEM + '/source-snapshots/tools/benchmarks/stem_worker.py',
              'native_adapter': STEM + '/source-snapshots/tools/runtime/htdemucs_segment.cpp',
              'native_cmake': STEM + '/source-snapshots/tools/runtime/CMakeLists.txt',
              'blocking_wait': STEM + '/source-snapshots/tools/runtime/BlockingVulkanWait.cmake',
              'historical_build': STEM + '/runtime/historical-build.json'}
SCOPE = ('Saved file/hash inventory, JSONL decoding, refused-admission provenance and saved-event timing/currentness/coverage checks. '
         'Canonical user-track/stem-quality archives are exact external dependencies. No PCM, normalized input, arrays, '
         'model/binary, full source tags, local HTML or interim derivations are included. Excluded PCM hashes and numerical '
         'or listening assertions cannot be independently recomputed from this archive. No inference or host queries run here.')


def require(test, message):
    if not test:
        raise ValueError(message)


def load(path):
    def reject(value):
        raise ValueError('Nonfinite JSON constant: ' + value)
    with (gzip.open if str(path).endswith('.gz') else open)(path, 'rt') as stream:
        return json.load(stream, parse_constant=reject)


def rows(path):
    def reject(value):
        raise ValueError('Nonfinite JSONL constant: ' + value)
    with (gzip.open if str(path).endswith('.gz') else open)(path, 'rt') as stream:
        return [json.loads(line, parse_constant=reject) for line in stream if line.strip()]


def pin(path):
    path = Path(path)
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'bytes': path.stat().st_size, 'sha256': digest}


def same(left, right):
    return all(left[key] == right[key] for key in ('bytes', 'sha256'))


def write(path, value):
    with path.open('x') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')


def members():
    files = {'series/' + name: BASE / name for name in SERIES}
    files.update({'helpers/' + name: BASE / name for name in HELPERS})
    files['helpers/native_replay.py'] = ROOT / 'tools/benchmarks/native_replay.py'
    files['helpers/window_guard.py'] = ROOT / 'tools/benchmarks/window_guard.py'
    runs = {'replay': 'abba-48-attempt3', 'refused-attempt1': 'replay-four-threads',
            'refused-attempt2': 'abba-48'}
    for target, source in runs.items():
        names = ['result.json', 'phases.jsonl']
        if target != 'refused-attempt1':
            names += ['admission-normal-ready.json', 'admission-normal-ready.jsonl', 'fresh-admission.json']
        if target == 'replay':
            names += ['requests.jsonl', 'native-trace.jsonl']
        files.update({target + '/' + name: BASE / source / name for name in names})
    for name in ('result.json', 'events.jsonl', 'native-trace.jsonl'):
        files['worker/' + name] = BASE / 'worker-four-tracks' / name
    for name in ('admission-normal-ready.json', 'admission-normal-ready.jsonl', 'fresh-admission.json'):
        files['worker-admission/' + name] = BASE / 'worker-admission' / name
    return {(name + '.gz' if path.suffix == '.jsonl' else name): path for name, path in files.items()}


def timing(values):
    ordered = sorted(values)
    require(ordered and all(math.isfinite(value) and value >= 0 for value in ordered), 'Invalid timing samples')
    return {'count': len(ordered), 'min': ordered[0], 'mean': statistics.mean(ordered),
            'median': statistics.median(ordered),
            **{'p' + str(p): ordered[math.ceil(len(ordered) * p / 100) - 1] for p in (50, 95, 99)},
            'max': ordered[-1], 'above_4095ms': sum(value > 4095 for value in ordered)}


def guard(value):
    require(value['status'] == 'ok' and value['returncode'] == value['child_returncode'] == 0
            and value['idle_inhibit_released'] and not value['errors'], 'Guard incomplete or unreleased')


def audit_replay(archive):
    run = load(archive / 'replay/result.json')
    requests = rows(archive / 'replay/requests.jsonl.gz')
    facts = load(archive / 'series/replay-facts.json')
    guard(load(archive / 'series/replay-guard-attempt3.json'))
    require(run['status'] == 'ok' and not run['failures'] and run['native_started']
            and run['threads'] == 4 and run['backend'] == 'vulkan'
            and run['completed_requests'] == run['planned_requests'] == len(requests) == 48
            and run['requests'] == requests, 'Successful 48-request replay differs')
    require([row['input_label'] for row in requests] == run['order'] == ['synthetic', 'music', 'music', 'synthetic'] * 12
            and [row['request_id'] for row in requests] == list(range(1, 49)), 'ABBA request order differs')
    require(run['cleanup']['process_returncode'] is not None and run['cleanup']['temporary_directory_gone']
            and not run['cleanup']['PID_exists_after_close'], 'Native replay did not close')
    for key, expected in FROZEN.items():
        require(run['pins'][key]['sha256'] == expected, 'Replay producer pin differs: ' + key)
    require(same(run['pins']['driver'], pin(archive / 'helpers/interleaved_replay.py'))
            and same(run['pins']['replay_source'], pin(archive / 'helpers/native_replay.py'))
            and same(facts['analysis_source'], pin(archive / 'helpers/summarize_replay.py')), 'Executed replay/helper differs')
    for row in requests:
        require(row['metrics']['id'] == row['request_id'] and row['metrics']['frames'] == WINDOW
                and row['metrics']['stems'] == 4 and row['matches_first_output_for_input']
                and row['input_sha256'] == run['inputs'][row['input_label']]['normalized_sha256']
                and row['output_sha256'] == run['reference_output_sha256'][row['input_label']]
                and math.isclose(row['round_trip_ms'], (row['finished_monotonic'] - row['started_monotonic']) * 1000,
                                 rel_tol=1e-12, abs_tol=1e-6), 'Replay request identity/timing differs')
    result = {}
    for label in ('synthetic', 'music'):
        selected = [row for row in requests if row['input_label'] == label]
        result[label] = {'round_trip_ms': timing([row['round_trip_ms'] for row in selected]),
                         'graph_ms': timing([row['metrics']['graph_ms'] for row in selected])}
        require(all(facts['per_input'][label][key] == value for key, value in result[label].items()), 'Replay timing facts differ')
    for target, version, guard_name in [('refused-attempt1', 'interleaved_replay-attempt1.py', 'replay-guard.json'),
                                      ('refused-attempt2', 'interleaved_replay-attempt2.py', 'replay-guard-attempt2.json')]:
        refused = load(archive / target / 'result.json')
        phases = rows(archive / target / 'phases.jsonl.gz')
        require(refused['status'] == 'error' and not refused['native_started']
                and refused['completed_requests'] == 0 and refused['requests'] == [] and refused['failures']
                and not any(row['phase'] in ('native_spawn', 'native_ready', 'request_started') for row in phases)
                and same(refused['pins']['driver'], pin(archive / 'helpers' / version)), 'Refused admission/version differs')
        require(load(archive / 'series' / guard_name)['idle_inhibit_released'], 'Refused-attempt inhibitor remains')
    return result


def audit_worker(archive, repository, references):
    run = load(archive / 'worker/result.json')
    events = rows(archive / 'worker/events.jsonl.gz')
    trace = rows(archive / 'worker/native-trace.jsonl.gz')
    launch = load(archive / 'series/worker-followup-launch.json')
    facts = load(archive / 'series/worker-facts.json')
    inventory = load(archive / 'archive-manifest.json')['files']
    require(facts['status'] == 'ok' and same(facts['analysis_source'], pin(archive / 'helpers/summarize_worker.py'))
            and len(facts['dependencies']) == 1 and same(facts['dependencies'][0], references['worker_summarizer']),
            'Final worker facts/helper dependency differs')
    for key, relative in {'result': 'worker/result.json', 'events': 'worker/events.jsonl.gz',
                          'trace': 'worker/native-trace.jsonl.gz', 'launch': 'series/worker-followup-launch.json',
                          'guard': 'series/worker-guard.json', 'readiness': 'worker-admission/admission-normal-ready.json',
                          'readiness_rows': 'worker-admission/admission-normal-ready.jsonl.gz',
                          'admission': 'worker-admission/fresh-admission.json'}.items():
        stored = inventory[relative]
        require(same(facts['inputs'][key], {'bytes': stored['raw_bytes'], 'sha256': stored['raw_sha256']}),
                'Worker facts input binding differs: ' + key)
    require(same(facts['inputs']['manifest'], references['manifest'])
            and same(facts['inputs']['previous_vulkan_result'], references['original_result']),
            'Worker facts canonical input binding differs')
    guard(load(archive / 'series/worker-guard.json'))
    require(launch['status'] == 'completed' and launch['child_returncode'] == 0 and not launch['failures']
            and launch['cleanup']['child_reaped'] and not launch['cleanup']['owned_process_group_exists']
            and same(launch['worker_result'], pin(archive / 'worker/result.json')), 'Worker launcher did not close')
    require(run['status'] == 'ok' and not run['failures'] and run['backend'] == 'vulkan' and run['threads'] == 4
            and [track['id'] for track in run['tracks']] == IDS and run['elapsed_seconds'] <= 360
            and run['worker_statistics']['closed'] and run['worker_statistics']['backend_pid'] is None
            and run['worker_statistics']['backend_start_count'] == 1 and run['worker_statistics']['telemetry_dropped'] == 0,
            'Worker run incomplete/settings differ')
    for key, expected in FROZEN.items():
        require(run['pins'][key]['sha256'] == expected, 'Worker producer differs: ' + key)
    require(same(run['pins']['manifest'], references['manifest']) and same(run['pins']['driver'], references['quality_driver'])
            and same(launch['pins']['launcher'], pin(archive / 'helpers/worker_followup.py')), 'Worker executed sources differ')
    controls = [row for row in events if row['event'] == 'control']
    completed = {row['generation']: row for row in events if row['event'] == 'complete'}
    groups = [(track, track['continuous']) for track in run['tracks']]
    require([row['start_frame'] for row in run['seeks']] == [HOP - 1, WINDOW + 1, N - 4096], 'Worker seek list differs')
    groups += [(run['tracks'][0], row) for row in run['seeks']]
    require(len(controls) == len(groups) == len(completed) == 7, 'Worker control/generation count differs')
    scenarios = []
    for track, summary in groups:
        generation = summary['generation']
        selected = [row for row in events if row.get('generation') == generation]
        control = [row for row in selected if row['event'] == 'control']
        source = [row for row in selected if row['event'] == 'source_ready']
        chunks = [row for row in selected if row['event'] == 'chunk']
        require(len(control) == len(source) == 1 and chunks and control[0]['frame_start'] == summary['start_frame'], 'Worker control/chunks differ')
        position = summary['start_frame']
        for row in chunks:
            latest = max((c for c in controls if c['timestamp'] <= row['consumed_at']), key=lambda c: c['timestamp'])
            require(latest['generation'] == generation and row['track_id'] == track['id']
                    and row['source_id'] == track['input']['sha256'] and row['model_id'] == FROZEN['model']
                    and row['source_frames'] == N and row['sample_rate'] == RATE and row['channels'] == 2
                    and row['frame_start'] == position and row['frames'] == min(HOP - position % HOP, N - position),
                    'Worker currentness/coverage differs')
            position += row['frames']
        reduced = timing([row['hop_service_ms'] for row in chunks])
        require(position == completed[generation]['frame_start'] == N and summary['frames'] == N - summary['start_frame']
                and summary['chunks'] == len(chunks) and summary['hop_service_max_ms'] == reduced['max']
                and summary['hop_service_percentiles_ms'] == {str(p): reduced['p' + str(p)] for p in (50, 95, 99)},
                'Worker timing summary differs')
        first = summary['first_chunk']
        require(first['ready_monotonic'] == chunks[0]['ready_at'] and first['delivered_monotonic'] >= chunks[0]['consumed_at']
                and math.isclose(first['control_to_ready_ms'], (chunks[0]['ready_at'] - control[0]['timestamp']) * 1000,
                                 rel_tol=1e-12, abs_tol=1e-6), 'Worker first-control timing differs')
        scenarios.append({'track_id': track['id'], 'scenario': 'continuous' if summary['start_frame'] == 0 else 'seek',
                          'generation': generation, 'start_frame': summary['start_frame'], 'frames': summary['frames'],
                          'first_chunk': first, 'wall_seconds': summary['wall_seconds'],
                          'source_preparation_ms': source[0]['preparation_ms'], 'source_reused': source[0]['reused'],
                          'timing': None})
    continuous = [row for row in events if row['event'] == 'chunk' and row['scenario'] == 'continuous']
    require(len(continuous) == 16, 'Worker continuous population differs')
    spec = importlib.util.spec_from_file_location('canonical_worker_summary', repository / REFERENCES['worker_summarizer'])
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    for scenario in scenarios:
        scenario['timing'] = helper.timing([row for row in events if row['event'] == 'chunk' and row['generation'] == scenario['generation']])
    continuous_timing = helper.timing(continuous)
    all_timing = helper.timing([row for row in events if row['event'] == 'chunk'])
    require(facts['continuous_timing'] == continuous_timing and facts['all_delivered_timing'] == all_timing
            and facts['event_counts'] == dict(Counter(row['event'] for row in events))
            and facts['scenarios'] == sorted(scenarios, key=lambda row: row['generation'])
            and facts['seeks'] == run['seeks'] and facts['producer_pins'] == run['pins']
            and facts['worker_statistics'] == run['worker_statistics'], 'Worker facts timing/seek summaries differ')
    require(facts['native_sampling'] == helper.native_facts(run, trace, controls, completed), 'Worker thermal/process reductions differ')
    old = load(repository / REFERENCES['original_result'])
    expected_outputs = []
    for current, previous in zip(run['tracks'], old['tracks'], strict=True):
        require(current['id'] == previous['id'] and same(current['output'], previous['output'])
                and current['continuous']['chunk_stream_sha256'] == previous['continuous']['chunk_stream_sha256'],
                'Worker output identity differs from canonical baseline')
        expected_outputs.append({'id': current['id'],
            'current': {key: current['output'][key] for key in ('path', 'bytes', 'sha256')},
            'previous': {key: previous['output'][key] for key in ('path', 'bytes', 'sha256')},
            'file_bytes_and_sha256_match': True, 'chunk_stream_sha256': current['continuous']['chunk_stream_sha256']})
    require(facts['output_identity'] == expected_outputs, 'Worker saved output-identity summaries differ')
    return {'continuous': continuous_timing, 'all_delivered': all_timing, 'seek_controls': len(run['seeks']),
            'output_identity_summaries': len(expected_outputs), 'native_sampling_reproduced': True,
            'excluded_PCM_rehashed': False}


def audit(archive, repository):
    archive, repository = archive.resolve(), repository.resolve()
    manifest = load(archive / 'archive-manifest.json')
    require(set(manifest['files']) == set(members()), 'Explicit allowlist differs')
    require({str(path.relative_to(archive)) for path in archive.rglob('*') if path.is_file()}
            == set(manifest['files']) | {'archive-manifest.json'}, 'Unmanifested/missing member')
    counts = {}
    for relative, expected in manifest['files'].items():
        path = archive / relative
        require(archive in path.resolve().parents and not path.is_symlink() and same(pin(path), expected), 'Stored member identity/path differs')
        if relative.endswith('.gz'):
            with path.open('rb') as stream:
                header = stream.read(10)
            require(header[:2] == b'\x1f\x8b' and not header[3] & 8 and header[4:8] == b'\0' * 4, 'Gzip header not deterministic')
            with gzip.open(path, 'rb') as stream:
                data = stream.read()
            require(len(data) == expected['raw_bytes'] and hashlib.sha256(data).hexdigest() == expected['raw_sha256'], 'Decoded identity differs')
            counts[relative] = len(rows(path))
            require(counts[relative] == expected['jsonl_records'], 'Decoded JSONL count differs')
        elif path.suffix == '.json':
            load(path)
    references = manifest['external_references']
    require(set(references) == set(REFERENCES), 'External dependency list differs')
    for key, relative in REFERENCES.items():
        require(references[key]['repository_path'] == relative and same(pin(repository / relative), references[key]), 'External dependency identity differs: ' + key)
    require(references['user_manifest']['sha256'] == USER_MANIFEST, 'Canonical user-track archive changed')
    for key in ('worker_source', 'sampler_source', 'shared_benchmark_source'):
        require(references[key]['sha256'] == FROZEN[key], 'Canonical producer binding differs')
    build = load(repository / REFERENCES['historical_build'])
    require(build['files']['blocking_binary']['sha256'] == FROZEN['runtime']
            and build['files']['model']['sha256'] == FROZEN['model'], 'Canonical runtime/model binding differs')
    replay = audit_replay(archive)
    worker = audit_worker(archive, repository, references)
    return {'status': 'passed', 'manifest_sha256': pin(archive / 'archive-manifest.json')['sha256'],
            'files': len(manifest['files']), 'stored_bytes': sum(row['bytes'] for row in manifest['files'].values()),
            'jsonl_records': counts, 'replay': replay, 'worker': worker, 'scope': SCOPE,
            'PCM_or_listening_replayed': False}


def stage(output, approved):
    output = output.resolve()
    require(approved and output == BASE / 'staged-archive' and not output.exists(), 'Root approval and fresh ignored staged-archive required')
    guard(load(BASE / 'worker-guard.json'))
    require(load(BASE / 'worker-followup-launch.json')['status'] == 'completed', 'Wait for worker closure and final facts')
    sources = members()
    require(all(path.is_file() and not path.is_symlink() for path in sources.values()), 'Final allowlist has missing/nonplain member')
    output.mkdir()
    files = {}
    for relative, source in sources.items():
        identity = pin(source)
        dest = output / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        if source.suffix == '.jsonl':
            with source.open('rb') as inp, dest.open('wb') as out:
                with gzip.GzipFile(filename='', fileobj=out, mode='wb', mtime=0, compresslevel=6) as zipped:
                    shutil.copyfileobj(inp, zipped)
        else:
            shutil.copyfile(source, dest)
        files[relative] = {**pin(dest), 'source': str(source), 'raw_bytes': identity['bytes'],
                           'raw_sha256': identity['sha256'], 'compression': 'gzip' if source.suffix == '.jsonl' else None}
        if source.suffix == '.jsonl':
            files[relative]['jsonl_records'] = len(rows(source))
    references = {key: {'repository_path': relative, **pin(ROOT / relative)} for key, relative in REFERENCES.items()}
    write(output / 'archive-manifest.json', {'schema': 1, 'scope': SCOPE, 'root_approved_final_allowlist': True,
          'refused_attempts': 'Both admission refusals started no native process and completed zero requests; never latency samples.',
          'external_references': references, 'files': dict(sorted(files.items()))})
    return audit(output, ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='mode', required=True)
    packing = commands.add_parser('stage')
    packing.add_argument('--approved', action='store_true')
    packing.add_argument('--output', type=Path, default=BASE / 'staged-archive')
    checking = commands.add_parser('audit')
    checking.add_argument('archive', type=Path)
    checking.add_argument('--repository-root', type=Path, default=Path.cwd())
    checking.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = stage(args.output, args.approved) if args.mode == 'stage' else audit(args.archive, args.repository_root)
    if args.mode == 'audit' and args.output:
        require(args.archive.resolve() not in args.output.resolve().parents, 'Audit output must be outside archive')
        write(args.output, result)
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()

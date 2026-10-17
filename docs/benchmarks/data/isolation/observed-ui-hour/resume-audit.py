#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Audit completed saved isolation evidence; no host queries, GPU or PCM recomputation."""
import argparse
import collections
import gzip
import hashlib
import json
import math
from pathlib import Path

SOAK_SHA = '52605983fdd2416caf63477bc9314096a0daa4f251e798c92928be19e5fea8b8'
HEAD = 'aec8501bf0db4487d3a851b5106372574b191942'
FAULTS = ('callback_execution_deadline_misses', 'missing_buffers', 'invalid_buffers',
          'truncated_frames', 'observed_graph_position_discontinuities',
          'observed_position_clock_xrun_recovery_flags')


def load(path):
    return json.loads(path.read_text())


def records(path):
    if not path.exists():
        path = path.with_name(path.name + '.gz')
    with (gzip.open if path.suffix == '.gz' else open)(path, 'rt') as stream:
        for line in stream:
            yield json.loads(line)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def timing(rows):
    values = sorted(row['hop_service_ms'] for row in rows)
    return {'chunks': len(rows), 'frames': sum(row['frames'] for row in rows),
            'hop_service_percentiles_ms': {str(q): values[math.ceil(q * len(values) / 100) - 1]
                                          for q in (50, 95, 99)} if values else {},
            'hop_service_max_ms': max(values, default=None),
            'hops_above_target': sum(value > 4095 for value in values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-head', default=HEAD)
    parser.add_argument('--expected-soak-sha256', default=SOAK_SHA)
    parser.add_argument('--source-snapshot', type=Path,
                        default=Path('tools/benchmarks/stem_soak.py'))
    parser.add_argument('--legacy-smoke', action='store_true',
                        help='Validate saved short pre-observer fixtures; do not qualify them as resumed hours')
    args = parser.parse_args()
    path = args.run_directory.resolve()
    result = load(path / 'result.json')
    events = list(records(path / 'events.jsonl'))
    hops = [event for event in events if event['event'] == 'soak_hop']
    continuous = [event for event in hops if event['kind'] == 'continuous']
    chunks = [event for event in events if event['event'] == 'chunk']
    begin, end = result['workload_started_monotonic'], result['workload_stopped_monotonic']
    assert result['status'] == 'ok' and not result['failures']
    assert end - begin == result['workload_seconds'] >= result['requested_seconds']
    if args.legacy_smoke:
        assert 60 <= result['requested_seconds'] < 3600
    else:
        assert result['requested_seconds'] == 3600
    selected = {'none': set(), 'ui': {'ui'}, 'audio': {'audio'},
                'both': {'ui', 'audio'}}[result['load_mode']]
    assert set(result['selected_loads']) == set(result['load']['children']) == selected
    assert result['acceptance']['selected_load_coverage_completed']
    derived = {'all_delivered': timing(hops), 'continuous': timing(continuous),
               'bins_300_seconds': []}
    for key in ('all_delivered', 'continuous'):
        assert all(value == result['timing'][key][field]
                   for field, value in derived[key].items())
    for interval in result['timing']['bins_300_seconds']:
        subset = timing([event for event in hops if interval['start_seconds'] <=
                         event['elapsed_seconds'] < interval['end_seconds']])
        assert all(value == interval[key] for key, value in subset.items())
        derived['bins_300_seconds'].append(dict(start_seconds=interval['start_seconds'],
                                               end_seconds=interval['end_seconds'], **subset))
    assert len(chunks) == len(hops) == len({(event['generation'], event['request_id'])
                                          for event in chunks})
    assert len({event['source_id'] for event in chunks}) == 1
    assert {event['model_id'] for event in chunks} == {result['pins']['model']['sha256']}
    active = expected = None
    for event in events:
        if event['event'] == 'control':
            active, expected = event['generation'], event.get('frame_start')
        if event['event'] == 'chunk':
            assert event['generation'] == active and event['frame_start'] == expected
            expected += event['frames']
    for event in hops:
        assert event['parity']['exact'] is True
        if event['kind'] == 'continuous':
            assert event['samples_sha256'] == result['reference_hashes'][str(event['window_start_frame'])]
    controls = result['controls']
    assert len(controls) >= 1 if args.legacy_smoke else len(controls) == 11
    assert all(control['complete'] and control['held_chunk_rejected'] and
               control['post_cancel_chunks'] == 0 and control['parity']['exact'] and
               control['parity']['numeric_parity'] and control['parity']['max_abs_difference'] == 0 and
               control['started_to_cancel_ms'] >= 150 for control in controls)
    discarded = {(event['generation'], event['request_id']) for event in events
                 if event['event'] == 'discarded'}
    assert discarded == {(control['superseded_generation'], control['superseded_request_id'])
                         for control in controls}
    ranges = {key: {'min': min(control[key] for control in controls),
                    'max': max(control[key] for control in controls)}
              for key in ('started_to_cancel_ms', 'cancel_ack_ms', 'residual_finish_ms',
                          'resume_ack_ms', 'ready_after_resume_ms', 'ready_after_cancel_ms',
                          'delivered_after_cancel_ms')}
    stats = result['worker_statistics']
    assert stats['closed'] and stats['backend_start_count'] == 1
    assert stats['cancel_count'] == stats['discarded_count'] == len(controls)
    assert stats['telemetry_dropped'] == result['stale_chunks_delivered'] == 0
    assert stats['ready_high_water'] <= stats['max_ready'] == 2
    probes = {}
    for name in ('audio', 'ui'):
        coverage = result[name + '_coverage']
        if name not in selected:
            assert coverage['requested'] is False and coverage['status'] == 'not_run'
            assert coverage['passed'] is None
            assert not (path / (name + '.json')).exists()
            assert not (path / (name + '.stderr')).exists()
            if name == 'ui':
                assert not (path / 'ui-progress.json').exists()
            probes[name] = {'requested': False, 'status': 'not_run', 'passed': None,
                            'faults': None, 'scope': 'Disabled probe; no health measurement.'}
            continue
        probe = load(path / (name + '.json'))
        assert probe == result['load']['children'][name]['report']
        assert result['load']['children'][name]['returncode'] == 0
        assert coverage['requested'] and coverage['passed']
        if name == 'audio':
            assert all(probe[key] == 0 for key in FAULTS)
            start, stop = probe['streaming_started_monotonic_ns'] / 1e9, probe['stopped_monotonic_ns'] / 1e9
            overlap = max(0, min(end, stop) - max(begin, start))
            assert overlap == result['workload_seconds']
            assert probe['callbacks'] * 256 == probe['frames_rendered']
            assert all(probe['negotiated'][key] == 256 for key in
                       ('stream_frames_min', 'stream_frames_max', 'graph_quantum_min', 'graph_quantum_max'))
            probes[name] = {'requested': True, 'sha256': sha(path / 'audio.json'),
                            'callbacks': probe['callbacks'], 'frames': probe['frames_rendered'],
                            'stream_interval_seconds': [start, stop],
                            'exact_workload_overlap_seconds': overlap,
                            'faults': {key: probe[key] for key in FAULTS},
                            'callback_elapsed_us': probe['callback_elapsed_us'],
                            'quantum_frames': 256, 'scope': probe['measurement_scope']}
        else:
            observation = probe['frame_observation']
            bins = observation['frames_per_60_seconds']
            assert sum(bins) == probe['frames_swapped']
            start, stop = observation['started_monotonic_ns'] / 1e9, observation['stopped_monotonic_ns'] / 1e9
            assert start <= begin and stop >= end
            complete = [index for index in range(len(bins)) if start + (index + 1) * 60 <= stop
                        and start + index * 60 < end and start + (index + 1) * 60 > begin]
            assert len(complete) >= math.floor(result['requested_seconds'] / 60)
            assert all(bins[index] > 0 for index in complete)
            assert complete == coverage['complete_minute_bins_intersecting_workload']
            assert coverage['progress']['unexposed_observations'] == 0
            probes[name] = {'requested': True, 'sha256': sha(path / 'ui.json'),
                            'frames': probe['frames_swapped'], 'complete_bins': complete,
                            'bins': bins, 'maximum_notification_gap_ms': observation['maximum_notification_gap_ms'],
                            'progress': coverage['progress'], 'scope': observation['scope']}
    policy_counts, transitions, previous, trace_count = collections.Counter(), [], None, 0
    for row in records(path / 'native-trace.jsonl'):
        values = row.get('system_values', {})
        value = (values.get('/sys/firmware/acpi/platform_profile', 'MISSING'),
                 values.get('/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference', 'MISSING'))
        policy_counts[value] += 1
        trace_count += 1
        if value != previous:
            transitions.append({'timestamp_monotonic': row['timestamp_monotonic'],
                                'workload_offset_seconds': row['timestamp_monotonic'] - begin,
                                'firmware': value[0], 'cpu0_EPP': value[1]})
            previous = value
    assert trace_count == result['native_memory_cpu']['samples']

    def meta(suffix):
        candidate = path / f'{path.name}-{suffix}.json'
        if candidate.exists():
            return load(candidate)
        sibling = path.parent / candidate.name
        if sibling.exists():
            return load(sibling)
        archived = {'cleanup': 'cleanup-host', 'guard': 'guard',
                    'policy': 'policy-lifecycle'}[suffix]
        return load(path / (archived + '.json'))

    guard, lifecycle = meta('guard'), meta('policy')
    assert guard['idle_inhibit_released'] and guard['errors'] == [] and guard['status'] == 'ok'
    assert guard['window_guard']['requested'] == ('ui' in selected)
    if 'ui' in selected:
        assert guard['script_unloaded'] and guard['restored']
    else:
        assert guard['idle_only'] and guard['window_guard']['status'] == 'not_run'
    if args.legacy_smoke:
        cleanup = {'status': 'not_retained', 'scope': 'Legacy smoke has no separate saved PID cleanup record.'}
        assert lifecycle.get('mode') != 'observe' and lifecycle['restored_by_runner']
    else:
        cleanup = meta('cleanup')
        assert cleanup['PID_exists'] and all(value is False for value in cleanup['PID_exists'].values())
        assert cleanup['fixture_exists'] is False and cleanup['worker_closed']
        assert cleanup['idle_inhibit_released'] and cleanup['guard_errors'] == []
        assert lifecycle['mode'] == 'observe' and lifecycle['profile_mutation'] is False
        assert lifecycle['restored_by_runner'] is False
    assert lifecycle['runner_returncode'] == lifecycle['benchmark_returncode'] == 0
    pin = result['pins']['soak_source']
    assert pin['sha256'] == args.expected_soak_sha256 and result['repository_head'] == args.expected_head
    assert sha(args.source_snapshot) == pin['sha256']
    source = path / 'events.jsonl'
    if not source.exists():
        source = source.with_name(source.name + '.gz')
    facts = {'scope': 'Saved completed synthetic isolation evidence; no fresh numerical PCM recomputation or new workload. Legacy fixtures do not qualify as resumed hours.',
             'status': result['status'], 'load_mode': result['load_mode'],
             'requested_seconds': result['requested_seconds'], 'workload_seconds': result['workload_seconds'],
             'timing': derived, 'event_counts': dict(collections.Counter(event['event'] for event in events)),
             'chunk_currentness_checks': len(chunks),
             'controls': {'complete': len(controls), 'discarded_events': len(discarded),
                          'held_chunk_rejected': sum(control['held_chunk_rejected'] for control in controls),
                          'ranges_ms': ranges,
                          'recorded_parity': {'exact_and_numeric': True, 'max_abs_difference': 0,
                                              'scope': 'Recorded success assertions and event hashes checked; successful PCM arrays are not retained/recomputed.'},
                          'range_scope': 'Retained control measurements; return instants are not separately reconstructible from events.',
                          'hardware_submission_confirmed': sum(control['hardware_submission_confirmed'] for control in controls)},
             'worker': stats, 'audio': probes['audio'], 'UI': probes['ui'],
             'native_policy': {'samples': trace_count, 'phase_counts': [
                 {'firmware': key[0], 'cpu0_EPP': key[1], 'samples': value}
                 for key, value in policy_counts.items()], 'transitions': transitions,
                 'scope': 'Native snapshots only; no interpolation, stable-profile or private-latch claim.'},
             'fixture': result['fixture'], 'reference_hashes': result['reference_hashes'],
             'same_grid_hash_checks': result['same_grid_hash_checks'], 'trimmed_seek_checks': result['trimmed_seek_checks'],
             'pins': result['pins'], 'repository_head': result['repository_head'],
             'source_snapshot_verified': {'path': str(args.source_snapshot), 'sha256': sha(args.source_snapshot)},
             'acceptance': result['acceptance'], 'failures': result['failures'], 'cleanup': cleanup,
             'guard_cleanup': {'script_unloaded': guard.get('script_unloaded'),
                               'idle_inhibit_released': guard['idle_inhibit_released'], 'errors': guard['errors']},
             'sampler_errors': {'sample': result['native_memory_cpu']['sample_errors'],
                                'trace': result['native_memory_cpu']['trace']['errors']},
             'source_evidence': {'result_sha256': sha(path / 'result.json'),
                                 'events_stored_path': str(source), 'events_stored_sha256': sha(source),
                                 'audit_source_sha256': sha(Path(__file__))}}
    args.output.write_text(json.dumps(facts, indent=2) + '\n')
    print(json.dumps({'output': str(args.output), 'mode': result['load_mode'],
                      'continuous': derived['continuous'], 'controls': len(controls),
                      'audio_faults': probes['audio'].get('faults'),
                      'UI_frames': probes['ui'].get('frames')}, indent=2))


if __name__ == '__main__':
    main()

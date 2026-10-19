#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Compare final saved matrix hashes, controls, AC snapshots and bounded kernel captures."""
from collections import Counter
from datetime import datetime
import hashlib
import json
from pathlib import Path

BASE = Path(__file__).resolve().parent
NAMES = ('observed-both-hour', 'observed-ui-hour', 'observed-none-hour', 'observed-audio-hour')


def load(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def utc(value):
    return datetime.fromisoformat(value)


def gaps(rows):
    values = [(utc(right['at_utc']) - utc(left['at_utc'])).total_seconds()
              for left, right in zip(rows, rows[1:])]
    return {'maximum_adjacent_sample_gap_seconds': max(values, default=None),
            'nonpositive_adjacent_sample_gaps': sum(value <= 0 for value in values)}


def control_summary(results):
    controls = [control for result in results for control in result['controls']]
    assert controls and all(control['complete'] and control['held_chunk_rejected'] and
                            control['post_cancel_chunks'] == 0 and control['parity']['exact'] and
                            control['parity']['numeric_parity'] and
                            control['parity']['max_abs_difference'] == 0 for control in controls)
    return {'complete': len(controls),
            'held_chunk_rejected': sum(control['held_chunk_rejected'] for control in controls),
            'post_cancel_chunks': sum(control['post_cancel_chunks'] for control in controls),
            'recorded_exact_and_numeric_parity': True, 'recorded_max_abs_difference': 0,
            'hardware_submission_confirmed': sum(control['hardware_submission_confirmed'] for control in controls),
            'ranges_ms': {key: {'min': min(control[key] for control in controls),
                                'max': max(control[key] for control in controls)}
                          for key in ('cancel_ack_ms', 'residual_finish_ms', 'delivered_after_cancel_ms')},
            'scope': 'Recorded logical supervisor cancellation/seek checks; native computation drains. Hardware submission is not observed, and successful PCM arrays are not retained or freshly recomputed.'}


results = {name: load(BASE / name / 'result.json') for name in NAMES}
baseline = results[NAMES[0]]
shared = set.intersection(*(set(result['pins']) for result in results.values()))
comparisons = {}
for name, result in results.items():
    differences = []
    for frame in sorted(set(baseline['reference_hashes']) | set(result['reference_hashes']), key=int):
        expected, actual = baseline['reference_hashes'].get(frame), result['reference_hashes'].get(frame)
        if expected != actual:
            differences.append({'window_start_frame': int(frame), 'baseline': expected, 'run': actual})
    comparisons[name] = {'repository_head': result['repository_head'],
                         'reference_window_count': len(result['reference_hashes']),
                         'reference_differences_vs_both': differences,
                         'fixture_sha256': result['fixture']['sha256'],
                         'pins': {key: value['sha256'] for key, value in result['pins'].items()}}
pin_differences = {key: {name: result['pins'][key]['sha256'] for name, result in results.items()}
                   for key in sorted(shared)
                   if len({result['pins'][key]['sha256'] for result in results.values()}) != 1}
monitor = BASE / 'resume-monitor.jsonl'
rows = [json.loads(line) for line in monitor.read_text().splitlines()]
assert rows
ac = {'samples': len(rows), 'AC_online_counts': dict(Counter(str(row.get('AC_online')) for row in rows)),
      'first_sample_utc': rows[0]['at_utc'], 'last_sample_utc': rows[-1]['at_utc'],
      **gaps(rows), 'monitor_errors': [row for row in rows if row.get('error') or row.get('monitor_returncode') != 0],
      'trial_bounds': {},
      'scope': 'AC/online snapshots taken near each recorded UTC timestamp, about every 45 seconds plus query time. These do not prove uninterrupted mains power between observations. Bounds below use launcher-to-wrapper-exit UTC timestamps and include preparation/cleanup; they are not exact workload-clock bounds. Wall-clock gaps are derived from saved UTC timestamps only.'}
kernels = {}
for name in NAMES[1:]:
    launch = load(BASE / (name + '-launch.json'))
    start, stop = utc(launch['started_at_utc']), utc(launch['finished_at_utc'])
    selected = [row for row in rows if start <= utc(row['at_utc']) <= stop]
    preceding = [row for row in rows if utc(row['at_utc']) < start]
    following = [row for row in rows if utc(row['at_utc']) > stop]
    assert selected
    ac['trial_bounds'][name] = {
        'launch_utc': launch['started_at_utc'], 'wrapper_exit_utc': launch['finished_at_utc'],
        'samples_within_bounds': len(selected),
        'AC_online_counts': dict(Counter(str(row.get('AC_online')) for row in selected)),
        'first_sample_utc': selected[0]['at_utc'], 'last_sample_utc': selected[-1]['at_utc'],
        'launch_to_first_sample_seconds': (utc(selected[0]['at_utc']) - start).total_seconds(),
        'last_sample_to_wrapper_exit_seconds': (stop - utc(selected[-1]['at_utc'])).total_seconds(),
        'preceding_sample_utc': preceding[-1]['at_utc'] if preceding else None,
        'following_sample_utc': following[0]['at_utc'] if following else None, **gaps(selected)}
    kernel_path = BASE / (name + '-kernel.json')
    kernel = load(kernel_path)
    kernels[name] = {'command': kernel['command'], 'returncode': kernel['returncode'],
                     'stderr': kernel['stderr'], 'examined_entries': kernel['examined_entries'],
                     'matched_entries': kernel['matched_entries'], 'parse_errors': kernel['parse_errors'],
                     'scope': kernel['scope'], 'sha256': sha(kernel_path)}
facts = {'scope': 'Saved-result cross-run comparison only; reference hash identity does not freshly recompute successful PCM arrays or assess listening quality. Missing selected-probe pins are expected when their loads were disabled. The earlier both-load HEAD differs from the resumed documentation-only HEAD; frozen producer hashes are compared explicitly.',
         'runs': comparisons, 'shared_pin_names': sorted(shared), 'shared_pin_differences': pin_differences,
         'all_recorded_reference_hashes_identical': all(not result['reference_differences_vs_both'] for result in comparisons.values()),
         'all_fixture_hashes_identical': len({result['fixture']['sha256'] for result in results.values()}) == 1,
         'resumed_controls': control_summary([results[name] for name in NAMES[1:]]),
         'all_four_controls': control_summary(list(results.values())),
         'sampled_AC': ac, 'bounded_kernel_captures': kernels,
         'source_evidence': {'results': {name: {'path': str(BASE / name / 'result.json'),
                                               'sha256': sha(BASE / name / 'result.json')} for name in NAMES},
                             'monitor_path': str(monitor), 'monitor_sha256': sha(monitor),
                             'audit_source_sha256': sha(Path(__file__))}}
output = BASE / 'resume-matrix-comparison.json'
output.write_text(json.dumps(facts, indent=2) + '\n')
print(json.dumps({'output': str(output), 'output_sha256': sha(output),
                  'audit_source_sha256': sha(Path(__file__)),
                  'all_recorded_reference_hashes_identical': facts['all_recorded_reference_hashes_identical'],
                  'shared_pin_differences': pin_differences, 'resumed_controls': facts['resumed_controls'],
                  'sampled_AC': ac, 'kernel_match_counts': {name: len(value['matched_entries'])
                                                          for name, value in kernels.items()}}, indent=2))

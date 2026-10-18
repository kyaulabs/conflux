#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Summarize retained workload thermal samples; never query the live host."""
import argparse
import gzip
import hashlib
import json
import math
import re
from pathlib import Path


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def snapshots(path):
    with (gzip.open if path.suffix == '.gz' else open)(path, 'rt') as stream:
        for line in stream:
            yield json.loads(line)


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def episodes(series, threshold, begin):
    groups, current = [], []
    for index, (timestamp, value) in enumerate(series):
        if value is not None and value >= threshold:
            current.append(index)
        elif current:
            groups.append(current)
            current = []
    if current:
        groups.append(current)
    output = []
    for group in groups:
        first, last = group[0], group[-1]
        previous = series[first - 1] if first else None
        following = series[last + 1] if last + 1 < len(series) else None

        def bound(observation):
            if observation is None:
                return None
            timestamp, value = observation
            return {'workload_offset_seconds': timestamp - begin,
                    'temperature_C': value / 1000 if value is not None else None,
                    'observed_below_threshold': value is not None and value < threshold}

        bounded = (previous is not None and previous[1] is not None and previous[1] < threshold
                   and following is not None and following[1] is not None and following[1] < threshold)
        output.append({'first_hot_workload_offset_seconds': series[first][0] - begin,
                       'last_hot_workload_offset_seconds': series[last][0] - begin,
                       'first_to_last_hot_sample_span_seconds': series[last][0] - series[first][0],
                       'hot_samples': len(group),
                       'peak_C': max(series[index][1] for index in group) / 1000,
                       'largest_between_hot_observation_gap_seconds': max(
                           (series[right][0] - series[left][0]
                            for left, right in zip(group, group[1:])), default=0),
                       'previous_observation': bound(previous), 'following_observation': bound(following),
                       'previous_to_first_hot_gap_seconds': series[first][0] - previous[0] if previous else None,
                       'last_hot_to_following_gap_seconds': following[0] - series[last][0] if following else None,
                       'adjacent_cool_observation_window_seconds': following[0] - previous[0] if bounded else None})
    return {'threshold_C': threshold / 1000,
            'hot_samples': sum(len(group) for group in groups),
            'episodes': output,
            'maximum_first_to_last_hot_sample_span_seconds': max(
                (episode['first_to_last_hot_sample_span_seconds'] for episode in output), default=0),
            'maximum_adjacent_cool_observation_window_seconds': max(
                (episode['adjacent_cool_observation_window_seconds'] for episode in output
                 if episode['adjacent_cool_observation_window_seconds'] is not None), default=None)}


def summarize(series, begin):
    observed = [(timestamp, value) for timestamp, value in series if value is not None]
    peak = max((value for _, value in observed), default=None)
    gaps = [right[0] - left[0] for left, right in zip(series, series[1:])]
    return {'samples': len(series), 'valid_samples': len(observed),
            'missing_or_invalid_samples': len(series) - len(observed),
            'peak_C': peak / 1000 if peak is not None else None,
            'peak_workload_offsets_seconds': [timestamp - begin for timestamp, value in observed if value == peak],
            'first_observed_workload_offset_seconds': observed[0][0] - begin if observed else None,
            'last_observed_workload_offset_seconds': observed[-1][0] - begin if observed else None,
            'maximum_adjacent_observation_gap_seconds': max(gaps, default=None),
            'thresholds': {str(threshold): episodes(series, threshold * 1000, begin)
                           for threshold in (85, 95)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    path = args.run_directory.resolve()
    result_path = path / 'result.json'
    result = json.loads(result_path.read_text())
    begin, end = result['workload_started_monotonic'], result['workload_stopped_monotonic']
    assert finite(begin) and finite(end) and begin < end
    mapping = {}
    for key, value in result['conditions_at_start'].items():
        if re.fullmatch(r'/sys/class/thermal/thermal_zone[0-9]+', key):
            assert isinstance(value, dict) and isinstance(value.get('type'), str)
            mapping[key + '/temp'] = value['type']
    assert mapping
    trace = path / 'native-trace.jsonl'
    if not trace.exists():
        trace = trace.with_name(trace.name + '.gz')
    rows, before, after, keys = [], 0, 0, set()
    previous = None
    for row in snapshots(trace):
        timestamp = row['timestamp_monotonic']
        assert finite(timestamp) and (previous is None or timestamp >= previous)
        previous = timestamp
        if timestamp < begin:
            before += 1
            continue
        if timestamp > end:
            after += 1
            continue
        values = {key: value for key, value in row.get('system_values', {}).items()
                  if re.fullmatch(r'/sys/class/thermal/thermal_zone[0-9]+/temp', key)}
        assert all(key in mapping for key in values), 'sampled sensor lacks recorded type'
        keys.update(values)
        rows.append((timestamp, values))
    assert rows and keys
    keys = sorted(keys)
    by_sensor = {}
    for key in keys:
        series = [(timestamp, values.get(key) if finite(values.get(key)) else None)
                  for timestamp, values in rows]
        by_sensor[key] = {'recorded_type': mapping[key], **summarize(series, begin)}
    package = [key for key in keys if mapping[key] == 'x86_pkg_temp']
    assert len(package) == 1, 'expected one recorded sampled package sensor'
    any_sensor = []
    for timestamp, values in rows:
        complete = all(finite(values.get(key)) for key in keys)
        # Unknown sensors must not create an apparent cool/recovery observation.
        any_sensor.append((timestamp, max(values[key] for key in keys) if complete else None))
    facts = {'scope': 'Retained native thermal snapshots inside the inclusive workload interval only. Any-sensor means the sampled and type-mapped sensors listed here, not all laptop sensors. Thresholds describe samples and do not establish sustained heat or a user-approved sustained threshold. No interpolation or continuous-duration proof; an isolated hot sample has zero first-to-last span. Adjacent cool observations bound an observation window, not the actual length of continuous heat. Missing observations break episodes; gaps between hot observations may hide recovery or further spikes.',
             'run': path.name, 'load_mode': result['load_mode'], 'workload_seconds': end - begin,
             'workload_interval_monotonic': [begin, end],
             'trace_samples_in_workload': len(rows), 'trace_samples_before': before,
             'trace_samples_after': after, 'sampled_sensor_types': {key: mapping[key] for key in keys},
             'mapped_but_not_sampled_sensor_types': {key: value for key, value in mapping.items() if key not in keys},
             'any_sampled_sensor': summarize(any_sensor, begin),
             'package_sensor': {'path': package[0], **by_sensor[package[0]]},
             'by_sensor': by_sensor,
             'source_evidence': {'result_sha256': sha(result_path), 'trace_stored_path': str(trace),
                                 'trace_stored_sha256': sha(trace), 'audit_source_sha256': sha(Path(__file__))}}
    args.output.write_text(json.dumps(facts, indent=2, allow_nan=False) + '\n')
    print(json.dumps({'output': str(args.output), 'run': path.name, 'samples': len(rows),
                      'sampled_sensor_types': facts['sampled_sensor_types'],
                      'any_sampled_peak_C': facts['any_sampled_sensor']['peak_C'],
                      'package_peak_C': facts['package_sensor']['peak_C'],
                      'thresholds': {key: {field: value for field, value in details.items() if field != 'episodes'}
                                     for key, details in facts['any_sampled_sensor']['thresholds'].items()}}, indent=2))


if __name__ == '__main__':
    main()

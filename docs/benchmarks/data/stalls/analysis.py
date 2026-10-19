#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Join non-preroll worker chunks to strictly interior host trace samples.

No interpolation: source/OLA are outside the supervisor native-request span.
DRM counters are host accounting, not GPU kernel timestamps. Flat counters can
also reflect delayed accounting; they alone do not prove a blocked GPU queue.
"""
import argparse
import bisect
import gzip
import json
import math
import os
from pathlib import Path
import statistics


def read_jsonl(path):
    opener = gzip.open if str(path).endswith('.gz') else open
    records, errors = [], []
    with opener(path, 'rt') as stream:
        for number, line in enumerate(stream, 1):
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                errors.append(number)
    return records, errors


def numeric_summary(values):
    return {'min': min(values), 'median': statistics.median(values),
            'max': max(values)} if values else None


def render_counters(sample):
    values = {}
    for client in sample.get('drm_clients', []):
        raw = client.get('drm-engine-render', '').split()
        if len(raw) != 2 or raw[1] != 'ns':
            continue
        identity = (sample['pid'], sample['start_ticks'],
                    client.get('drm-pdev', client.get('drm-driver', 'unknown')),
                    client.get('drm-client-id'))
        values[identity] = int(raw[0])
    return values


def render_summary(samples):
    if len(samples) < 2:
        return None
    counters = [render_counters(sample) for sample in samples]
    common = set(counters[0]).intersection(counters[-1])
    if not common:
        return {'error': 'No matching DRM render client at both interior endpoints'}
    # The inference client's counter advances; the initialization client's stays 0.
    client = max(common, key=lambda key: max(counter.get(key, 0) for counter in counters))
    first, last = counters[0][client], counters[-1][client]
    times = [sample['timestamp_monotonic'] for sample in samples]
    flat, max_flat = 0.0, 0.0
    missing_intervals, resets = 0, 0
    for i in range(1, len(samples)):
        previous, current = counters[i - 1].get(client), counters[i].get(client)
        if previous is None or current is None:
            missing_intervals += 1
            flat = 0.0
        elif current < previous:
            resets += 1
            flat = 0.0
        elif current == previous:
            flat += times[i] - times[i - 1]
            max_flat = max(max_flat, flat)
        else:
            flat = 0.0
    duration_ms = (times[-1] - times[0]) * 1000
    return {'client': {'pid': client[0], 'start_ticks': client[1],
                       'device': client[2], 'client_id': client[3]},
            'first_render_ns': first, 'last_render_ns': last,
            'delta_render_ms': (last - first) / 1e6,
            'interior_span_ms': duration_ms,
            'render_busy_fraction_inner': (last - first) / 1e6 / duration_ms
                                          if duration_ms > 0 and not resets and not missing_intervals else None,
            'max_sample_gap_ms': max((b - a) * 1000 for a, b in zip(times, times[1:])),
            'max_flat_render_counter_span_ms': max_flat * 1000,
            'missing_client_intervals': missing_intervals, 'counter_resets': resets}


def system_summary(samples):
    keys = set().union(*(sample.get('system_values', {}) for sample in samples)) if samples else set()
    output = {}
    for key in sorted(keys):
        wanted = (key.endswith('_freq_mhz') or key.endswith('/scaling_cur_freq')
                  or key.endswith('/temp') or 'throttle_reason_' in key
                  or '/cpu0/thermal_throttle/' in key
                  or key.endswith('/platform_profile')
                  or key.endswith('/energy_performance_preference')
                  or key.endswith('/sched_schedstats'))
        if not wanted:
            continue
        values = [sample['system_values'][key] for sample in samples
                  if key in sample.get('system_values', {})]
        if all(isinstance(value, (int, float)) for value in values):
            result = numeric_summary(values)
            if 'throttle_reason_' in key:
                result['nonzero_samples'] = sum(value != 0 for value in values)
                result['samples'] = len(values)
                result['nonzero_fraction'] = result['nonzero_samples'] / len(values)
            if '/thermal_throttle/' in key:
                result['last_minus_first'] = values[-1] - values[0]
        else:
            result = {'observed_values': sorted(set(values))}
        output[key] = result
    return output


def scheduling_summary(samples):
    if len(samples) < 2:
        return None
    enabled = sorted(set(sample.get('system_values', {}).get('/proc/sys/kernel/sched_schedstats')
                         for sample in samples), key=str)
    tasks = [sample.get('task_schedstat_ns') for sample in samples]
    valid = (enabled == [1] and all(tasks) and
             all(task['task_ids'] == tasks[0]['task_ids'] and not task['read_errors']
                 for task in tasks))
    result = {'sched_schedstats_observed': enabled, 'delta_valid': valid}
    if valid:
        result.update(runtime_delta_ms=(tasks[-1]['runtime'] - tasks[0]['runtime']) / 1e6,
                      runqueue_wait_delta_ms=(tasks[-1]['runqueue_wait'] - tasks[0]['runqueue_wait']) / 1e6)
    else:
        result['note'] = 'Wait deltas omitted: schedstats missing/disabled, task identities changed, or read errors.'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--events', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--generation', type=int, default=1)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    events, event_errors = read_jsonl(args.events)
    trace, trace_errors = read_jsonl(args.trace)
    trace.sort(key=lambda sample: sample['timestamp_monotonic'])
    times = [sample['timestamp_monotonic'] for sample in trace]
    starts, duplicate_starts = {}, []
    for event in events:
        if event.get('event') == 'started':
            key = event['generation'], event['request_id']
            if key in starts:
                duplicate_starts.append(key)
            starts[key] = event
    rows, excluded = [], []
    for chunk in events:
        if chunk.get('event') != 'chunk' or chunk['generation'] != args.generation:
            continue
        key = chunk['generation'], chunk['request_id']
        start = starts.get(key)
        if start is None or start.get('preroll'):
            excluded.append({'generation': key[0], 'request_id': key[1],
                             'reason': 'missing started event or preroll'})
            continue
        if start['window_start_frame'] != chunk['window_start_frame']:
            raise ValueError(f'Conflicting window for request {key}')
        left = start['timestamp']
        right = left + chunk['request_ms'] / 1000
        begin, end = bisect.bisect_right(times, left), bisect.bisect_left(times, right)
        samples = trace[begin:end]
        metrics = chunk['metrics']
        row = {key: chunk[key] for key in ('generation', 'request_id', 'frame_start', 'frames',
                                           'window_start_frame', 'hop_service_ms', 'request_ms',
                                           'source_read_ms', 'overlap_ms', 'queue_delay_ms')}
        row.update(native_metrics=metrics, native_request_span={'start': left, 'end': right},
                   trace_slice={'first_zero_based_index': begin if samples else None,
                                'last_zero_based_index': end - 1 if samples else None,
                                'samples': len(samples)},
                   system=system_summary(samples), drm_render=render_summary(samples),
                   scheduling=scheduling_summary(samples))
        if samples:
            row['trace_slice'].update(first_timestamp=times[begin], last_timestamp=times[end - 1],
                                      leading_unsampled_ms=(times[begin] - left) * 1000,
                                      trailing_unsampled_ms=(right - times[end - 1]) * 1000)
            first, last = samples[0], samples[-1]
            if (first['pid'], first['start_ticks']) == (last['pid'], last['start_ticks']):
                row['sampled_cpu_delta_inner_ms'] = ((last['cpu_ticks'] - first['cpu_ticks'])
                                                     * 1000 / os.sysconf('SC_CLK_TCK'))
        if metrics.get('inference_wall_ms'):
            row['inference_cpu_per_wall'] = metrics['inference_cpu_ms'] / metrics['inference_wall_ms']
        rows.append(row)
    report = {'events': str(args.events), 'trace': str(args.trace),
              'generation': args.generation, 'trace_samples_read': len(trace),
              'event_parse_error_lines': event_errors, 'trace_parse_error_lines': trace_errors,
              'duplicate_start_keys': duplicate_starts, 'excluded_chunks': excluded,
              'scope': ['Only chunks with a matching non-preroll started event, keyed by generation and request ID.',
                        'Trace slices include only timestamps strictly inside the supervisor request span; no interpolation.',
                        'Bounds use the started event timestamp and reported request duration; these are not exact GPU graph boundaries.',
                        'DRM busy and flat-counter summaries use interior samples, omit unsampled edges, and can reflect delayed kernel accounting.',
                        'Frequency/flag summaries count snapshots equally rather than weighting by time.',
                        'Host snapshots do not establish whole-workload or GPU kernel timing.'],
              'hops': rows}
    rendered = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(rendered)
        print(json.dumps({'output': str(args.output), 'hops': len(rows),
                          'trace_samples': len(trace), 'parse_errors': len(event_errors) + len(trace_errors)}))
    else:
        print(rendered, end='')


if __name__ == '__main__':
    main()

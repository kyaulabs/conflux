#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Post-run only: join delivered soak hops to strictly interior host snapshots.

Requires the final result.json. Does not run a worker or change machine policy.
Prepared from frozen stem_soak.py, worker_support.py and prior stalls/analysis.py.
No interpolation, GPU timestamp calibration or automatic causal classification.
"""
import argparse
import bisect
from collections import Counter, defaultdict
from datetime import datetime, timezone
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import statistics


def numeric(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def percentiles(values):
    values = sorted(values)
    return {str(p): values[max(0, math.ceil(len(values) * p / 100) - 1)]
            for p in (50, 95, 99)} if values else {}


def quantities(values):
    if not values:
        return None
    return {"first": values[0], "last": values[-1], "min": min(values),
            "median": statistics.median(values), "max": max(values)}


def jsonl(path):
    records, bad, bad_count = [], [], 0
    digest = hashlib.sha256()
    before = path.stat()
    opener = gzip.open if path.suffix == '.gz' else open
    with opener(path, 'rb') as stream:
        for line_number, raw in enumerate(stream, 1):
            digest.update(raw)
            try:
                value = json.loads(raw)
                if not isinstance(value, dict):
                    raise ValueError('record is not an object')
                records.append((line_number, value))
            except (ValueError, UnicodeDecodeError) as error:
                bad_count += 1
                if len(bad) < 50:
                    bad.append({"line": line_number, "error": str(error)[:160]})
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError(f'Input changed during analysis: {path}')
    return records, {"path": str(path.resolve()), "file_bytes": before.st_size,
                     "mtime_ns": before.st_mtime_ns,
                     "decompressed_jsonl_sha256": digest.hexdigest(),
                     "valid_records": len(records), "parse_error_count": bad_count,
                     "parse_error_examples": bad}


def identity(sample):
    return sample.get('pid'), sample.get('start_ticks')


def counter_summary(values, *, unit, expected, same_identity=True):
    observed = [value for value in values if numeric(value)]
    regression_indices = [i for i in range(1, len(values))
                          if numeric(values[i - 1]) and numeric(values[i])
                          and values[i] < values[i - 1]]
    valid = (len(values) >= 2 and len(observed) == expected
             and not regression_indices and same_identity)
    return {"unit": unit, "observations": len(observed), "missing_samples": expected - len(observed),
            "first_observed": observed[0] if observed else None,
            "last_observed": observed[-1] if observed else None,
            "regression_indices": regression_indices, "delta_valid": valid,
            "delta": observed[-1] - observed[0] if valid else None}


def system_is_counter(key):
    return (key.startswith('/proc/vmstat:') or key.endswith('.total')
            or key.endswith('/rc6_residency_ms')
            or key.endswith('/runtime_active_time') or key.endswith('/runtime_suspended_time')
            or '/thermal_throttle/' in key or key.endswith('/energy_uj'))


def system_summary(samples, units, span_ms):
    keys = set(units).union(*(set(s.get('system_values', {})) for s in samples))
    report = {}
    for key in sorted(keys):
        values = [s.get('system_values', {}).get(key) for s in samples]
        present = [v for v in values if v is not None]
        unit = units.get(key, 'not supplied by sampler metadata')
        if system_is_counter(key):
            entry = counter_summary(values, unit=unit, expected=len(samples))
            if entry['delta_valid'] and span_ms and span_ms > 0:
                if key.endswith('.total') and key.startswith('/proc/pressure/'):
                    entry['pressure_fraction_of_interior_span'] = entry['delta'] / (span_ms * 1000)
                elif key.endswith('/rc6_residency_ms') or key.endswith('/runtime_active_time') or key.endswith('/runtime_suspended_time'):
                    entry['residency_fraction_of_interior_span'] = entry['delta'] / span_ms
        elif present and all(numeric(v) for v in present):
            entry = {"unit": unit, "samples": len(present),
                     "missing_samples": len(samples) - len(present), **quantities(present)}
            if 'throttle_reason_' in key:
                entry.update(nonzero_samples=sum(v != 0 for v in present),
                             nonzero_fraction=sum(v != 0 for v in present) / len(present))
        else:
            entry = {"unit": unit, "samples": len(present),
                     "missing_samples": len(samples) - len(present),
                     "observed_values": sorted(set(present), key=str)}
        report[key] = entry
    return report


def drm_values(sample):
    values, memories, duplicates, malformed = {}, {}, [], []
    seen = set()
    for client in sample.get('drm_clients', []):
        client_id = (sample.get('pid'), sample.get('start_ticks'),
                     client.get('drm-pdev', client.get('drm-driver', 'unknown')),
                     client.get('drm-client-id'))
        if client_id in seen:
            duplicates.append(client_id)
            continue
        seen.add(client_id)
        for field, raw in client.items():
            if field.startswith('drm-engine-') and not field.startswith('drm-engine-capacity-'):
                parts = str(raw).split()
                if len(parts) == 2 and parts[1] == 'ns' and parts[0].isdigit():
                    values[(*client_id, field)] = int(parts[0])
                else:
                    malformed.append({"client": client_id, "field": field, "raw": raw})
            elif field.startswith(('drm-total-', 'drm-resident-', 'drm-active-', 'drm-purgeable-', 'drm-shared-')):
                memories[(*client_id, field)] = raw
    return values, memories, duplicates, malformed


def drm_summary(samples, span_ms):
    snapshots = [drm_values(sample) for sample in samples]
    keys = set().union(*(set(s[0]) for s in snapshots)) if snapshots else set()
    engines = []
    for key in sorted(keys, key=str):
        values = [s[0].get(key) for s in snapshots]
        entry = {"pid": key[0], "start_ticks": key[1], "device": key[2],
                 "client_id": key[3], "field": key[4],
                 **counter_summary(values, unit='ns', expected=len(samples))}
        if entry['delta_valid'] and span_ms and span_ms > 0:
            entry.update(delta_ms=entry['delta'] / 1e6,
                         engine_busy_fraction_of_interior_span=entry['delta'] / 1e6 / span_ms)
        # Flat host accounting is not proof of an unscheduled GPU queue.
        flat_ms, maximum = 0.0, 0.0
        for i in range(1, len(samples)):
            if numeric(values[i - 1]) and values[i] == values[i - 1]:
                flat_ms += (samples[i]['timestamp_monotonic'] - samples[i - 1]['timestamp_monotonic']) * 1000
                maximum = max(maximum, flat_ms)
            else:
                flat_ms = 0.0
        entry['max_flat_counter_span_ms'] = maximum
        engines.append(entry)
    memory_keys = set().union(*(set(s[1]) for s in snapshots)) if snapshots else set()
    memory = []
    for key in sorted(memory_keys, key=str):
        raw = [s[1].get(key) for s in snapshots]
        # Raw zero and a quantity such as "10 KiB" are unlike representations.
        # Retain values rather than silently inventing a unit for bare zero.
        observed = [v for v in raw if v is not None]
        memory.append({"pid": key[0], "start_ticks": key[1], "device": key[2],
                       "client_id": key[3], "field": key[4],
                       "missing_samples": len(samples) - len(observed),
                       "raw_observed_values": sorted(set(observed), key=str)})
    return {"engines": engines, "memory_fields": memory,
            "duplicate_client_examples": [d for s in snapshots for d in s[2]][:30],
            "malformed_engine_fields": [d for s in snapshots for d in s[3]][:30],
            "fd_read_errors": sum(s.get('fd_read_errors', 0) for s in samples)}


def summarize(samples, units, clock_ticks, expected_interval):
    times = [s['timestamp_monotonic'] for s in samples]
    span_ms = (times[-1] - times[0]) * 1000 if len(times) > 1 else None
    identities = sorted(set(identity(s) for s in samples), key=str)
    stable = len(identities) == 1
    fields = set().union(*(set(s.get('process_counters', {})) for s in samples)) if samples else set()
    process = {key: counter_summary([s.get('process_counters', {}).get(key) for s in samples],
                                    unit='count', expected=len(samples), same_identity=stable)
               for key in sorted(fields)}
    cpu = counter_summary([s.get('cpu_ticks') for s in samples], unit='clock ticks',
                          expected=len(samples), same_identity=stable)
    cpu['clock_ticks_per_second'] = clock_ticks
    cpu['delta_ms'] = cpu['delta'] * 1000 / clock_ticks if cpu['delta_valid'] else None
    gauges = {}
    for key in ('VmRSS', 'VmHWM', 'VmSwap'):
        values = [s.get('process_status_kB', {}).get(key) for s in samples]
        present = [v for v in values if numeric(v)]
        gauges[key] = {"unit": 'kB', "missing_samples": len(samples) - len(present),
                       "summary": quantities(present)}
    gaps = [(b - a) * 1000 for a, b in zip(times, times[1:])]
    capture = [s['capture_elapsed_ms'] for s in samples if numeric(s.get('capture_elapsed_ms'))]
    enabled = sorted(set(s.get('system_values', {}).get('/proc/sys/kernel/sched_schedstats') for s in samples), key=str)
    tasks = [s.get('task_schedstat_ns') for s in samples]
    sched_valid = (len(samples) >= 2 and stable and enabled == [1] and all(tasks)
                   and all(t.get('task_ids') == tasks[0].get('task_ids') and not t.get('read_errors') for t in tasks))
    scheduling = {"sched_schedstats_observed": enabled, "delta_valid": sched_valid}
    if sched_valid:
        for key in ('runtime', 'runqueue_wait', 'timeslices'):
            item = counter_summary([t.get(key) for t in tasks],
                                   unit='count' if key == 'timeslices' else 'ns', expected=len(samples))
            scheduling[key] = item
            scheduling['delta_valid'] &= item['delta_valid']
    else:
        scheduling['note'] = 'Deltas omitted when schedstats disabled/missing, process/task identities differ, or reads failed.'
    return {"samples": len(samples), "first_timestamp": times[0] if times else None,
            "last_timestamp": times[-1] if times else None, "interior_span_ms": span_ms,
            "process_identities": identities, "process_identity_stable": stable,
            "sample_gaps_ms": quantities(gaps), "sample_gap_percentiles_ms": percentiles(gaps),
            "gaps_above_twice_requested_interval": sum(g > expected_interval * 2000 for g in gaps),
            "capture_elapsed_ms": quantities(capture), "process_status": gauges,
            "process_counters": process, "sampled_cpu": cpu,
            "system": system_summary(samples, units, span_ms),
            "drm": drm_summary(samples, span_ms), "scheduling": scheduling}


def key(event):
    return event.get('generation'), event.get('request_id')


def hop_timing(hops, target):
    values = [hop['hop_service_ms'] for hop in hops]
    return {"chunks": len(values), "percentiles_ms": percentiles(values),
            "max_ms": max(values, default=None),
            "above_target": sum(v > target for v in values)}


def choose_representatives(hops, misses, target):
    selected = defaultdict(set)
    good = [h for h in hops if h['hop_service_ms'] <= target and h.get('kind') == 'continuous']
    for miss in misses:
        selected[key(miss)].add('target_miss')
        before = [h for h in good if h['ready_at'] < miss['ready_at']]
        after = [h for h in good if h['ready_at'] > miss['ready_at']]
        for candidates, label, latest in ((before, 'nearest_good_before', True), (after, 'nearest_good_after', False)):
            if candidates:
                picked = max(candidates, key=lambda h: h['ready_at']) if latest else min(candidates, key=lambda h: h['ready_at'])
                selected[key(picked)].add(f'{label}_miss_{key(miss)}')
            same = [h for h in candidates if h.get('window_start_frame') == miss.get('window_start_frame')]
            if same:
                picked = max(same, key=lambda h: h['ready_at']) if latest else min(same, key=lambda h: h['ready_at'])
                selected[key(picked)].add(f'{label}_same_source_window_miss_{key(miss)}')
    if good:
        first, last = min(good, key=lambda h: h['ready_at']), max(good, key=lambda h: h['ready_at'])
        selected[key(first)].add('first_good_continuous')
        selected[key(last)].add('last_good_continuous')
        median = statistics.median(h['hop_service_ms'] for h in good)
        selected[key(min(good, key=lambda h: (abs(h['hop_service_ms'] - median), h['ready_at'])))].add('median_service_good_continuous')
        midpoint = (first['ready_at'] + last['ready_at']) / 2
        selected[key(min(good, key=lambda h: (abs(h['ready_at'] - midpoint), h['ready_at'])))].add('middle_time_good_continuous')
    return selected



def ui_coverage(result):
    observation = result.get('load', {}).get('children', {}).get('ui', {}).get('report', {}).get('frame_observation', {})
    start_ns = observation.get('started_monotonic_ns')
    bins = observation.get('frames_per_60_seconds', [])
    if not numeric(start_ns):
        return {'available': False, 'continuous_ui_load_verified': False}
    start = start_ns / 1e9
    work_start, work_stop = result['workload_started_monotonic'], result['workload_stopped_monotonic']
    full_bins = [i for i in range(len(bins)) if work_start <= start + 60 * i and start + 60 * (i + 1) <= work_stop]
    zeros = [i for i in full_bins if bins[i] == 0]
    return {'available': True, 'raw_result_status': result['status'],
            'minute_bin_origin_monotonic': start,
            'raw_frames_per_60_seconds': bins,
            'full_minute_bins_in_workload': full_bins,
            'zero_full_minute_bins_in_workload': zeros,
            'maximum_notification_gap_ms': observation.get('maximum_notification_gap_ms'),
            'nonzero_frames_in_every_full_minute': bool(full_bins and not zeros),
            'continuous_ui_load_verified': False if zeros else None,
            'qualification': 'Zero full-minute bins disprove continuous frame-notification load. Nonzero minute counts alone cannot prove exact concurrent rendering at any particular hop or physical presentation.'}


def ui_bins_for_span(coverage, left, right):
    if not coverage.get('available'):
        return None
    origin = coverage['minute_bin_origin_monotonic']
    bins = coverage['raw_frames_per_60_seconds']
    indices = [i for i in range(len(bins)) if max(left, origin + 60 * i) < min(right, origin + 60 * (i + 1))]
    return {'native_span_overlaps_ui_bins': [{'index': i, 'frames_in_full_ui_bin': bins[i],
                                             'bin_start_monotonic': origin + 60 * i,
                                             'bin_end_monotonic': origin + 60 * (i + 1)} for i in indices],
            'scope': 'Whole UI-minute counts only; this does not identify frame events within the native request span.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_directory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--clock-ticks-per-second', type=int)
    args = parser.parse_args()
    root = args.run_directory.resolve()
    result_path = root / 'result.json'
    result_bytes = result_path.read_bytes()
    result = json.loads(result_bytes)
    if not result.get('finished_at_utc') or not numeric(result.get('workload_stopped_monotonic')) or result.get('status') in ('starting', 'running'):
        parser.error('A completed final result.json is required; do not analyze a live run.')
    event_path = root / 'events.jsonl'
    trace_path = root / 'native-trace.jsonl'
    if not event_path.exists(): event_path = root / 'events.jsonl.gz'
    if not trace_path.exists(): trace_path = root / 'native-trace.jsonl.gz'
    events, event_info = jsonl(event_path)
    raw_samples, trace_info = jsonl(trace_path)
    invalid_times = [line for line, sample in raw_samples if not numeric(sample.get('timestamp_monotonic'))]
    samples_in_file = [(line, s) for line, s in raw_samples if numeric(s.get('timestamp_monotonic'))]
    backwards = [{"previous_line": a[0], "line": b[0], "previous_time": a[1]['timestamp_monotonic'],
                  "time": b[1]['timestamp_monotonic']} for a, b in zip(samples_in_file, samples_in_file[1:])
                 if b[1]['timestamp_monotonic'] < a[1]['timestamp_monotonic']]
    duplicate_times = sum(a[1]['timestamp_monotonic'] == b[1]['timestamp_monotonic'] for a, b in zip(samples_in_file, samples_in_file[1:]))
    ordered = sorted(samples_in_file, key=lambda item: item[1]['timestamp_monotonic'])
    times = [s['timestamp_monotonic'] for _, s in ordered]
    samples = [s for _, s in ordered]
    starts, chunks, discarded, soak = defaultdict(list), defaultdict(list), defaultdict(list), defaultdict(list)
    for line, event in events:
        event_type = event.get('event')
        target_map = {'started': starts, 'chunk': chunks, 'discarded': discarded, 'soak_hop': soak}.get(event_type)
        if target_map is not None:
            target_map[key(event)].append((line, event))
    duplicate_keys = {name: [{'key': k, 'lines': [line for line, _ in group]} for k, group in mapping.items() if len(group) > 1]
                      for name, mapping in [('started', starts), ('chunk', chunks), ('discarded', discarded), ('soak_hop', soak)]}
    hops = [group[0][1] for group in soak.values() if len(group) == 1]
    hops.sort(key=lambda h: h['ready_at'])
    target = result.get('timing', {}).get('headroom_target_ms', 4095.0)
    misses = [h for h in hops if h['hop_service_ms'] > target]
    selected = choose_representatives(hops, misses, target)
    clock_ticks = args.clock_ticks_per_second or os.sysconf('SC_CLK_TCK')
    if clock_ticks <= 0: parser.error('clock ticks per second must be positive')
    interval = result.get('sample_interval_seconds', 0.5)
    units = result.get('native_memory_cpu', {}).get('system_sampling', {}).get('fields', {})
    coverage = ui_coverage(result)
    selected_rows, join_errors, all_joined = [], [], []
    timing_keys = ('generation', 'request_id', 'kind', 'elapsed_seconds', 'frame_start', 'frames',
                   'window_start_frame', 'ready_at', 'request_ms', 'hop_service_ms', 'source_read_ms',
                   'overlap_ms', 'queue_delay_ms', 'control_latency_ms', 'ready_gap_ms', 'same_generation_as_previous')
    for hop in hops:
        k = key(hop)
        group = starts.get(k, [])
        if len(group) != 1:
            join_errors.append({'key': k, 'reason': 'started event missing or duplicated'})
            continue
        started = group[0][1]
        if started.get('preroll') or started.get('window_start_frame') != hop.get('window_start_frame'):
            join_errors.append({'key': k, 'reason': 'preroll or conflicting source window'})
            continue
        left, right = started['timestamp'], started['timestamp'] + hop['request_ms'] / 1000
        begin, end = bisect.bisect_right(times, left), bisect.bisect_left(times, right)
        interior = samples[begin:end]
        row = {name: hop[name] for name in timing_keys if name in hop}
        row.update(above_target=hop['hop_service_ms'] > target, native_metrics=hop.get('metrics'),
                   started_event_line=group[0][0], native_request_span={'start': left, 'end': right},
                   interior_samples=len(interior),
                   first_trace_line=ordered[begin][0] if interior else None,
                   last_trace_line=ordered[end - 1][0] if interior else None,
                   leading_unsampled_ms=(times[begin] - left) * 1000 if interior else None,
                   trailing_unsampled_ms=(right - times[end - 1]) * 1000 if interior else None,
                   captures_began_at_or_before_request=sum(numeric(s.get('capture_started_monotonic')) and s['capture_started_monotonic'] <= left for s in interior))
        metrics = hop.get('metrics', {})
        if metrics.get('inference_wall_ms'):
            row['inference_cpu_per_wall'] = metrics.get('inference_cpu_ms', 0) / metrics['inference_wall_ms'] if 'inference_cpu_ms' in metrics else None
        row['ui_minute_relation'] = ui_bins_for_span(coverage, left, right)
        all_joined.append(row.copy())
        if k in selected:
            row.update(selection_reasons=sorted(selected[k]),
                       host=summarize(interior, units, clock_ticks, interval))
            if hop['hop_service_ms'] > target:
                # Exact selected host snapshots enable later phase review without another full scan.
                row['strict_interior_host_samples'] = [dict(s, at_native_ms=(s['timestamp_monotonic'] - left) * 1000) for s in interior]
            selected_rows.append(row)
    unpaired_starts = []
    for k, group in starts.items():
        if k not in soak:
            labels = []
            if any(event.get('preroll') for _, event in group): labels.append('preroll_has_no_delivery_event_by_protocol')
            if k in discarded: labels.append('discarded')
            if k in chunks: labels.append('chunk_not_accepted_as_soak_hop')
            if not labels: labels.append('no_terminal_event_observed_possible_shutdown_or_telemetry_gap')
            unpaired_starts.append({'key': k, 'started_lines': [line for line, _ in group], 'classification': labels})
    run_left, run_right = result['workload_started_monotonic'], result['workload_stopped_monotonic']
    run_begin, run_end = bisect.bisect_right(times, run_left), bisect.bisect_left(times, run_right)
    run_samples = samples[run_begin:run_end]
    expected = result.get('timing', {}).get('all_delivered', {})
    recalculated = hop_timing(hops, target)
    checks = {'soak_hop_count_matches_result': len(hops) == expected.get('chunks'),
              'target_miss_count_matches_result': recalculated['above_target'] == expected.get('hops_above_target'),
              'no_duplicate_join_keys': not any(duplicate_keys.values()),
              'no_join_errors': not join_errors,
              'no_jsonl_parse_errors': not event_info['parse_error_count'] and not trace_info['parse_error_count'],
              'trace_rows_match_sampler_written_count': len(raw_samples) + trace_info['parse_error_count'] == result.get('native_memory_cpu', {}).get('trace', {}).get('samples_written'),
              'no_trace_time_regressions': not backwards, 'no_invalid_trace_times': not invalid_times}
    first_last = {}
    for label, left, right in [('first_300_seconds', run_left, min(run_right, run_left + 300)),
                               ('last_300_seconds', max(run_left, run_right - 300), run_right)]:
        left_index, right_index = bisect.bisect_right(times, left), bisect.bisect_left(times, right)
        first_last[label] = {'bounds_monotonic': {'start': left, 'end': right},
                             'host': summarize(samples[left_index:right_index], units, clock_ticks, interval),
                             'scope': 'Host observed timestamps strictly inside these workload-relative bounds, with no interpolation.'}
    output = {'schema': 1, 'analyzed_at_utc': datetime.now(timezone.utc).isoformat(),
              'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'run_directory': str(root), 'result_sha256': hashlib.sha256(result_bytes).hexdigest(),
              'input_events': event_info, 'input_trace': trace_info,
              'run_status': result['status'], 'workload_seconds': result.get('workload_seconds'),
              'workload_monotonic_bounds': {'start': run_left, 'end': run_right},
              'workload_trace_edges': {'leading_unsampled_ms': (run_samples[0]['timestamp_monotonic'] - run_left) * 1000 if run_samples else None,
                                       'trailing_unsampled_ms': (run_right - run_samples[-1]['timestamp_monotonic']) * 1000 if run_samples else None,
                                       'captures_began_at_or_before_workload': sum(numeric(s.get('capture_started_monotonic')) and s['capture_started_monotonic'] <= run_left for s in run_samples)},
              'target_ms': target, 'sampling_interval_seconds': interval,
              'clock_ticks_source': 'explicit CLI argument' if args.clock_ticks_per_second else 'analysis host os.sysconf(SC_CLK_TCK); same-host analysis required for CPU ms',
              'source_pins_from_result': result.get('pins'), 'result_timing': result.get('timing'),
              'result_sampler_errors': {key: result.get('native_memory_cpu', {}).get(key) for key in ('sample_errors', 'trace', 'system_sampling')},
              'ui_coverage_qualification': coverage,
              'raw_run_acceptance': result.get('acceptance'),
              'first_last_300_seconds_host': first_last,
              'checks': checks, 'recalculated_delivered': recalculated,
              'recalculated_continuous': hop_timing([h for h in hops if h.get('kind') == 'continuous'], target),
              'trace_timestamp_regressions': backwards, 'trace_duplicate_timestamps': duplicate_times,
              'trace_invalid_timestamp_lines': invalid_times, 'duplicate_event_keys': duplicate_keys,
              'join_errors': join_errors, 'starts_without_delivered_hop': unpaired_starts,
              'discarded_request_events': [event for group in discarded.values() for _, event in group],
              'workload_host': summarize(run_samples, units, clock_ticks, interval),
              'all_joined_hop_timing': all_joined, 'selected_hops': selected_rows,
              'scope': [
                  'Delivered hops are soak_hop records, joined to unique non-preroll started by (generation, request_id), with window identity checked.',
                  'Native bounds are supervisor started timestamp plus request_ms, not exact GPU graph boundaries. Emission and host/native exchange add uncertainty.',
                  'Host selection uses observed timestamps strictly inside native bounds; no interpolation or unsampled edge attribution. Captures beginning before the left bound are separately counted.',
                  'System reads within a snapshot are sequential, not simultaneous. Snapshot medians/flags are sample-weighted, not duration-weighted.',
                  'Counter deltas require every selected sample, no observed regressions, and stable identities where applicable. Missing/denied fields are never zero-filled.',
                  'DRM engine counters describe client engine-class busy accounting, not useful arithmetic, global GPU utilization, frequency-normalized work or physical queue scheduling.',
                  'RC6/device-PM deltas describe cumulative residency across the strict interior span; sampled clocks can be zero during idle. Requested frequency differs from actual frequency.',
                  'PL1/thermal flags are sampled reason bits, not watt measurements or causal ordering. RAPL energy is permission-limited; no wattage estimate or energy-wrap repair is attempted.',
                  'Memory/IO PSI and vmstat are system-wide. Zero native faults/VmSwap do not exclude GPU object backing-store pressure or memory activity elsewhere.',
                  'Context-switch status counters refer to the main thread. Disabled schedstats do not support runqueue-wait conclusions. CPU sibling throttle counters must not be summed.',
                  'Representative good hops are deterministic descriptive comparisons, not randomized controls or independent percentile tail samples.',
                  'UI coverage is separately qualified from raw frame-notification minute bins; raw run status does not validate continuous UI load.',
                  'No profile, memory-pressure, throttling, preemption, reset or trace-overhead cause is inferred by this analysis.'
              ]}
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps({'output': str(args.output.resolve()), 'hops': len(hops), 'target_misses': len(misses),
                      'selected_hops': len(selected_rows), 'trace_samples': len(samples), 'checks': checks}))


if __name__ == '__main__':
    main()

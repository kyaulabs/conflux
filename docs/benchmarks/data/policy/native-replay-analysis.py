#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Analyze completed native replay requests using strict interior host snapshots.

Standard-library-only; accepts plain/gzip JSONL and never loads a model or GPU.
Counter/join helpers follow the prior M0 strict stall analyzer. Original pin paths
are informational. Supply original clock ticks explicitly when analyzing elsewhere.
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


def metric_summary(rows, key):
    values = [row[key] if key in row else row['metrics'][key] for row in rows]
    return {'unit': 'ms', 'count': len(values), 'percentiles': percentiles(values),
            'min': min(values, default=None), 'max': max(values, default=None),
            'above_4095ms': sum(value > 4095 for value in values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_directory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--balanced-summary', type=Path)
    parser.add_argument('--clock-ticks-per-second', type=int)
    args = parser.parse_args()
    root = args.run_directory.resolve()
    result_bytes = (root / 'result.json').read_bytes()
    result = json.loads(result_bytes)
    if not result.get('finished_at_utc') or not numeric(result.get('replay_stopped_monotonic')):
        parser.error('A completed final native replay result.json is required.')
    request_path, trace_path = root / 'requests.jsonl', root / 'native-trace.jsonl'
    if not request_path.exists(): request_path = root / 'requests.jsonl.gz'
    if not trace_path.exists(): trace_path = root / 'native-trace.jsonl.gz'
    parsed_requests, request_info = jsonl(request_path)
    parsed_samples, trace_info = jsonl(trace_path)
    rows = [record for _, record in parsed_requests]
    invalid_times = [line for line, row in parsed_samples if not numeric(row.get('timestamp_monotonic'))]
    valid_samples = [(line, row) for line, row in parsed_samples if numeric(row.get('timestamp_monotonic'))]
    regressions = [{'previous_line': before[0], 'line': after[0]}
                   for before, after in zip(valid_samples, valid_samples[1:])
                   if after[1]['timestamp_monotonic'] < before[1]['timestamp_monotonic']]
    ordered = sorted(valid_samples, key=lambda pair: pair[1]['timestamp_monotonic'])
    times = [sample['timestamp_monotonic'] for _, sample in ordered]
    samples = [sample for _, sample in ordered]
    resources = result.get('resources', {})
    units = resources.get('system_sampling', {}).get('fields', {})
    interval = result.get('sample_interval_seconds', resources.get('interval_seconds', .5))
    ticks = args.clock_ticks_per_second or os.sysconf('SC_CLK_TCK')
    if ticks <= 0: parser.error('clock ticks per second must be positive')
    ids = [row['request_id'] for row in rows]
    checks = {
        'requests_match_final_result': rows == result.get('requests'),
        'request_count_matches_report': len(rows) == result.get('completed_requests'),
        'requested_count_completed': len(rows) == result.get('requested_requests'),
        'unique_request_ids': len(ids) == len(set(ids)),
        'native_request_id_matches': all(row['metrics'].get('id') == row['request_id'] for row in rows),
        'round_trip_matches_exact_driver_bounds': all(abs((row['finished_at'] - row['started_at']) * 1000 - row['round_trip_ms']) < 1e-7 for row in rows),
        'intervals_positive': all(row['started_at'] < row['finished_at'] for row in rows),
        'serial_request_intervals': all(a['finished_at'] <= b['started_at'] for a,b in zip(rows,rows[1:])),
        'recorded_source_hashes_match': all(row['source_sha256'] == result['fixture']['sha256'] for row in rows),
        'recorded_input_hashes_match': all(row['input_sha256'] == result['input']['sha256'] for row in rows),
        'one_output_hash_and_all_match_flags': bool(rows and len(set(row['output_sha256'] for row in rows)) == 1 and all(row['matches_first_output'] for row in rows)),
        'no_JSONL_parse_errors': not request_info['parse_error_count'] and not trace_info['parse_error_count'],
        'trace_count_matches_sampler': len(parsed_samples) + trace_info['parse_error_count'] == resources.get('trace', {}).get('samples_written'),
        'no_trace_time_regressions': not regressions,
        'all_trace_timestamps_numeric': not invalid_times,
    }
    statistics = {key: metric_summary(rows, key) for key in ('round_trip_ms', 'graph_ms', 'inference_cpu_ms', 'inference_wall_ms', 'graph_rebuild_ms')}
    checks['round_trip_percentiles_match'] = statistics['round_trip_ms']['percentiles'] == result.get('round_trip_percentiles_ms')
    checks['graph_percentiles_match'] = statistics['graph_ms']['percentiles'] == result.get('graph_percentiles_ms')
    input_verification = {'available': False, 'verification_performed': False, 'status': 'unavailable',
                          'saved_input_sha256_claim': result['input']['sha256'],
                          'saved_input_bytes_claim': result['input']['bytes'],
                          'scope': 'Original absolute input path is informational and is never opened; only a local retained input.f32 or input.f32.gz can be verified.'}
    input_file = root / 'input.f32'
    compressed_input = root / 'input.f32.gz'
    if input_file.exists() or compressed_input.exists():
        use = input_file if input_file.exists() else compressed_input
        digest = hashlib.sha256(); size = 0
        opener = gzip.open if use.suffix == '.gz' else open
        with opener(use, 'rb') as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block); size += len(block)
        input_verification.update(available=True, verification_performed=True,
                                  path=str(use), decompressed_bytes=size, sha256=digest.hexdigest())
        checks['retained_input_matches_bytes_and_hash'] = size == result['input']['bytes'] and digest.hexdigest() == result['input']['sha256']
        input_verification['status'] = 'verified' if checks['retained_input_matches_bytes_and_hash'] else 'mismatch'
    selected = defaultdict(set)
    if rows:
        selected[rows[0]['request_id']].add('first')
        selected[rows[-1]['request_id']].add('last')
        for field in ('round_trip_ms','graph_ms'):
            selected[max(rows,key=lambda row:row.get(field,row['metrics'].get(field)))['request_id']].add('maximum_'+field)
        median = statistics['round_trip_ms']['percentiles']['50']
        selected[min(rows,key=lambda row:abs(row['round_trip_ms']-median))['request_id']].add('median_round_trip')
        for row in rows:
            if row['round_trip_ms'] > 4095 or row['metrics']['graph_ms'] > 4095:
                selected[row['request_id']].add('above_diagnostic_threshold')
    compact, detailed = [], []
    for row in rows:
        left, right = row['started_at'], row['finished_at']
        begin,end = bisect.bisect_right(times,left),bisect.bisect_left(times,right)
        interior = samples[begin:end]
        summary = summarize(interior, units, ticks, interval)
        sy = summary['system']
        def find(suffix): return next((v for key,v in sy.items() if key.endswith(suffix)),{})
        engines = [e for e in summary['drm']['engines'] if e['field']=='drm-engine-render']
        engine = max(engines,key=lambda e:e.get('delta') or -1,default={})
        joined = {'request_id': row['request_id'], 'driver_interval': {'start':left,'end':right},
                  'round_trip_ms':row['round_trip_ms'], 'native_metrics':row['metrics'],
                  'source_sha256':row['source_sha256'],'input_sha256':row['input_sha256'],'output_sha256':row['output_sha256'],
                  'interior_samples':len(interior),'interior_span_ms':summary['interior_span_ms'],
                  'first_trace_line':ordered[begin][0] if interior else None,
                  'last_trace_line':ordered[end-1][0] if interior else None,
                  'leading_unsampled_ms':(times[begin]-left)*1000 if interior else None,
                  'trailing_unsampled_ms':(right-times[end-1])*1000 if interior else None,
                  'capture_straddles_left_bound':sum(numeric(s.get('capture_started_monotonic')) and s['capture_started_monotonic']<=left for s in interior),
                  'actual_GPU_MHz':find('gt_act_freq_mhz'),'requested_GPU_MHz':find('gt_cur_freq_mhz'),
                  'PL1':find('throttle_reason_pl1'),'GPU_thermal':find('throttle_reason_thermal'),
                  'RC6_delta_ms':find('rc6_residency_ms').get('delta'),
                  'runtime_suspended_delta_ms':find('runtime_suspended_time').get('delta'),
                  'render':engine, 'memory_PSI_some_delta_us':find('memory:some.total').get('delta'),
                  'native_major_fault_delta':summary['process_counters'].get('stat.majflt',{}).get('delta'),
                  'native_minor_fault_delta':summary['process_counters'].get('stat.minflt',{}).get('delta')}
        compact.append(joined)
        if row['request_id'] in selected:
            detailed.append({**joined,'selection_reasons':sorted(selected[row['request_id']]),'host':summary})
    left,right=result['replay_started_monotonic'],result['replay_stopped_monotonic']
    begin,end=bisect.bisect_right(times,left),bisect.bisect_left(times,right)
    host=summarize(samples[begin:end],units,ticks,interval)
    checks['one_workload_process_identity'] = len(host['process_identities']) == 1
    checks['no_workload_system_counter_regressions'] = not any(v.get('regression_indices') for v in host['system'].values())
    checks['no_workload_DRM_counter_regressions'] = not any(v.get('regression_indices') for v in host['drm']['engines'])
    comparison=None
    if args.balanced_summary:
        comparison_file_bytes=args.balanced_summary.read_bytes()
        raw=gzip.decompress(comparison_file_bytes) if args.balanced_summary.suffix == '.gz' else comparison_file_bytes
        balanced=json.loads(raw)
        worst=max(balanced['misses'],key=lambda hop:hop['hop_service_ms'])
        comparison={'input_summary':str(args.balanced_summary.resolve()),'input_sha256':hashlib.sha256(comparison_file_bytes).hexdigest(),
                    'decompressed_json_sha256':hashlib.sha256(raw).hexdigest(),
                    'balanced_worst':worst,'same_source_window_start':result['window_start_frame']==worst['window_start_frame'],
                    'runtime_model_pins_match':all(result['pins'][key]['sha256']==balanced['pins_from_result'][key]['sha256'] for key in ('runtime','model')),
                    'scope':'Native replay driver intervals exclude worker source read/OLA/control/ready delivery. Native output is full-window raw output; its hash is not comparable to delivered normalized-restored/OLA hop hashes. The short replay removes controlled UI/audio load and is far shorter than the hour; non-recurrence does not prove cause or a fix.'}
    output={'schema':1,'analyzed_at_utc':datetime.now(timezone.utc).isoformat(),
            'script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'run_directory':str(root),'result_sha256':hashlib.sha256(result_bytes).hexdigest(),
            'input_requests':request_info,'input_trace':trace_info,'retained_input_verification':input_verification,
            'raw_status':result['status'],'replay_seconds':result['replay_seconds'],'pins_from_result':result.get('pins'),
            'window_start_frame':result['window_start_frame'],'normalization':result.get('normalization'),
            'runtime_ready':result.get('runtime_ready'),'conditions_at_start':result.get('conditions_at_start'),
            'conditions_at_finish':result.get('conditions_at_finish'),'checks':checks,'timing':statistics,
            'clock_ticks_per_second':ticks,'clock_ticks_source':'explicit CLI' if args.clock_ticks_per_second else 'analysis host os.sysconf(SC_CLK_TCK)',
            'request_ids_with_diagnostic_threshold_exceeded':[row['request_id'] for row in rows if row['round_trip_ms']>4095 or row['metrics']['graph_ms']>4095],
            'trace_time_regressions':regressions,'trace_invalid_timestamp_lines':invalid_times,
            'resource_metadata':resources,'workload_host':host,
            'workload_bounds_monotonic':{'start':left,'end':right},
            'workload_unsampled_edges_ms':{'leading':(times[begin]-left)*1000 if end>begin else None,
                                           'trailing':(right-times[end-1])*1000 if end>begin else None},
            'all_request_joins':compact,
            'selected_requests':detailed,'balanced_comparison':comparison,
            'scope':['This is a native-only diagnostic, not the M0 delivered-hop latency gate or an hour audio/UI validation.',
                     'Request bounds are exact recorded driver infer-call bounds, including file/native exchange, not GPU batch or graph timestamps.',
                     'Host observed timestamps are strictly interior; unsampled edges and capture-start straddles are retained. Reads within a sample are sequential.',
                     'Sample medians/flag counts are not time-weighted. Missing/denied readings are never zero-filled.',
                     'Counter deltas require complete interior observations and no regressions; process/client identity is retained. DRM engine busy is not useful arithmetic or whole-GPU usage.',
                     'RC6/runtime residency and clock samples do not prove why a client waits. PL1 flags are not watts; no causal attribution is made.',
                     'Memory/IO PSI and vmstat are system-wide. Zero native major faults alone cannot exclude kernel GEM backing-store activity.',
                     'Raw pin paths are metadata; this analyzer never opens runtime/model/source paths. It uses supplied run inputs and optional local input.f32 only.',
                     'The short replay changes workload/thermal history and absence of a stall is inconclusive.']}
    args.output.write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps({'output':str(args.output.resolve()),'requests':len(rows),'samples':len(samples),'checks':checks,'timing':statistics}))


if __name__=='__main__':
    main()

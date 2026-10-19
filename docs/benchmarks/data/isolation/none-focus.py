#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Post-run strict host joins for the none-hour failure; no GPU execution.

Pass the frozen companion analyzer explicitly. Native bounds use supervisor
started + request_ms; preroll ends at the next started event (an upper bound).
"""
import argparse
import bisect
from collections import Counter
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_directory', type=Path)
    parser.add_argument('--analyzer', type=Path, required=True)
    parser.add_argument('--kernel-journal', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('strict_host_analysis', args.analyzer)
    analysis = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(analysis)
    root = args.run_directory.resolve()
    result_path = root/'result.json'
    result = json.loads(result_path.read_bytes())
    if not result.get('finished_at_utc') or result['status'] in ('running', 'starting'):
        parser.error('Final completed result required')
    def resolve(name):
        path = root/name
        return path if path.exists() else root/(name+'.gz')
    events, event_info = analysis.jsonl(resolve('events.jsonl'))
    trace, trace_info = analysis.jsonl(resolve('native-trace.jsonl'))
    ordered = sorted(trace, key=lambda entry: entry[1]['timestamp_monotonic'])
    samples = [sample for _, sample in ordered]
    times = [sample['timestamp_monotonic'] for sample in samples]
    starts = {}
    terminals = {}
    accepted = {}
    for line, event in events:
        request = event.get('request_id')
        if event['event'] == 'started':
            if request in starts:
                raise RuntimeError(f'duplicate request_id {request}')
            starts[request] = (line, event)
        elif event['event'] in ('chunk', 'discarded'):
            terminals[request] = (line, event)
        elif event['event'] == 'soak_hop':
            accepted[request] = (line, event)
    worst = max(accepted, key=lambda request: accepted[request][1]['hop_service_ms'])
    failed = max(request for request, (_, event) in terminals.items() if event['event'] == 'chunk')
    selected = set(range(worst-3, failed+1))
    for origin in (0, 257985, 773955, 1031940):
        candidates = [request for request, (_, event) in accepted.items()
                      if event['window_start_frame'] == origin and event.get('parity', {}).get('exact')]
        if candidates:
            selected.update((min(candidates), max(candidates)))
    for _, event in accepted.values():
        if event.get('kind') == 'seek_first':
            selected.add(event['request_id'])
    units = result['native_memory_cpu']['system_sampling']['fields']
    interval = result['sample_interval_seconds']
    rows = []
    for request in sorted(selected):
        if request not in starts:
            continue
        start_line, start = starts[request]
        left = start['timestamp']
        terminal_line, terminal = terminals.get(request, (None, {}))
        if terminal.get('request_ms') is not None:
            right = left + terminal['request_ms']/1000
            bound_source = 'started timestamp + native request_ms; supervisor/native exchange included'
        elif start.get('preroll'):
            next_started = min((event['timestamp'] for _, event in starts.values() if event['timestamp'] > left), default=None)
            right = next_started
            bound_source = 'next started timestamp; upper bound includes inter-request preparation, not exact preroll completion'
        else:
            raise RuntimeError(f'No supported end bound for {request}')
        if right is None or right <= left:
            raise RuntimeError(f'Invalid bounds for {request}')
        begin, end = bisect.bisect_right(times,left), bisect.bisect_left(times,right)
        interior = samples[begin:end]
        host = analysis.summarize(interior,units,100,interval)
        rows.append({'request_id': request, 'generation': start['generation'],
            'start_event_line': start_line, 'terminal_event_line': terminal_line,
            'accepted_soak_hop': request in accepted,
            'started_event': start, 'terminal_event': terminal or None,
            'accepted_event': accepted.get(request,(None,None))[1],
            'bounds': {'start': left, 'end': right, 'source': bound_source},
            'strict_host': host,
            'leading_unsampled_ms': (times[begin]-left)*1000 if interior else None,
            'trailing_unsampled_ms': (right-times[end-1])*1000 if interior else None,
            'captures_began_before_or_at_left_bound': sum(s.get('capture_started_monotonic',left+1)<=left for s in interior),
            'trace_line_bounds': [ordered[begin][0],ordered[end-1][0]] if interior else None,
            'strict_interior_host_samples': [dict(s,at_request_ms=(s['timestamp_monotonic']-left)*1000) for s in interior]})
    def short(row):
        host = row['strict_host']
        system = host['system']
        def suffix(end):
            return next((value for key,value in system.items() if key.endswith(end)), None)
        def exact(key):
            return system.get(key)
        return {'request_id':row['request_id'],'generation':row['generation'],
            'accepted':row['accepted_soak_hop'],'preroll':row['started_event'].get('preroll'),
            'window_start_frame':row['started_event']['window_start_frame'],
            'request_bound_ms':(row['bounds']['end']-row['bounds']['start'])*1000,
            'bound_source':row['bounds']['source'],
            'hop_service_ms':(row['terminal_event'] or {}).get('hop_service_ms'),
            'metrics':(row['terminal_event'] or {}).get('metrics'),
            'parity':(row['accepted_event'] or {}).get('parity'),
            'samples':host['samples'],'capture_elapsed_ms':host['capture_elapsed_ms'],
            'sample_gaps_ms':host['sample_gaps_ms'],
            'leading_unsampled_ms':row['leading_unsampled_ms'],'trailing_unsampled_ms':row['trailing_unsampled_ms'],
            'actual_gpu_MHz':suffix('/gt_act_freq_mhz'),'requested_gpu_MHz':suffix('/gt_cur_freq_mhz'),
            'PL1':suffix('/throttle_reason_pl1'),'thermal_flag':suffix('/throttle_reason_thermal'),
            'package_temperature':suffix('/thermal_zone7/temp'),'TCPU':suffix('/thermal_zone5/temp'),
            'RC6_ms':suffix('/rc6_residency_ms'),'runtime_status':suffix('/runtime_status'),
            'runtime_suspended_ms':suffix('/runtime_suspended_time'),
            'render': [engine for engine in host['drm']['engines'] if engine['field']=='drm-engine-render'],
            'drm_memory':host['drm']['memory_fields'],'fd_read_errors':host['drm']['fd_read_errors'],
            'process_identity_stable':host['process_identity_stable'],'process_status':host['process_status'],
            'process_counters':host['process_counters'],'sampled_cpu':host['sampled_cpu'],
            'memory_available_kB':exact('/proc/meminfo:MemAvailable'),
            'memory_psi_some':exact('/proc/pressure/memory:some.total'),
            'memory_psi_full':exact('/proc/pressure/memory:full.total'),
            'io_psi_some':exact('/proc/pressure/io:some.total'),
            'vmstat':{key:entry for key,entry in system.items() if key.startswith('/proc/vmstat:')},
            'profile':{key:entry for key,entry in system.items() if 'platform_profile' in key or 'energy_performance_preference' in key}}
    missing = [request for request in sorted(starts) if request not in terminals and not starts[request][1].get('preroll')]
    kernel_relation = None
    if args.kernel_journal:
        journal = json.loads(args.kernel_journal.read_bytes())
        matches = []
        for record in journal['records']:
            stamp = record.get('__MONOTONIC_TIMESTAMP')
            if stamp is None:
                continue
            observed = int(stamp)/1e6
            candidates = [row for row in rows if row['bounds']['start'] < observed < row['bounds']['end']]
            matches.append({'journal_observed_monotonic':observed,'message':record['MESSAGE'],
                'request_interval_candidates':[row['request_id'] for row in candidates],
                'milliseconds_after_interval_start':{str(row['request_id']):(observed-row['bounds']['start'])*1000 for row in candidates}})
        kernel_relation = {'source':str(args.kernel_journal),'sha256':sha(args.kernel_journal),
            'records':matches,'scope':'Journal observed monotonic timestamps are compared without interpolation. Journal ingestion can lag kernel emission; sequence numbers are not Vulkan/native request IDs. Interval overlap is temporal association only.'}
    output = {'schema':1,'analyzed_at_utc':datetime.now(timezone.utc).isoformat(),
        'run_directory':str(root),'status':result['status'],'failures':result['failures'],
        'selected_loads':result.get('selected_loads'),'raw_timing':result['timing'],
        'source_pins_from_result':result['pins'],
        'input_result_sha256':sha(result_path),'input_events':event_info,'input_trace':trace_info,
        'analyzer':{'source':str(args.analyzer),'sha256':sha(args.analyzer)},
        'script_sha256':sha(Path(__file__)),'selected_request_ids':sorted(selected),
        'kernel_journal_relation':kernel_relation,
        'worst_accepted_request_id':worst,'failed_chunk_request_id':failed,
        'nonpreroll_started_without_terminal':missing,
        'trace_fd_read_errors':[{'line':line,'timestamp':sample['timestamp_monotonic'],
            'fd_read_errors':sample['fd_read_errors']} for line,sample in trace if sample.get('fd_read_errors')],
        'summary_rows':[short(row) for row in rows],'strict_rows':rows,
        'scope':['No workload is launched; supplied files must be completed and stable.',
            'Native spans are supervisor exchange bounds, not exact GPU graph timestamps; preroll next-start is only an upper bound.',
            'Host samples are selected strictly by observed timestamp inside each span, without interpolation; sequential capture timing and unsampled edges are preserved.',
            'DRM engine-class busy is client accounting, not useful arithmetic or proof of a scheduling/reset cause; the zero-work auxiliary client is kept separate.',
            'Read errors, missing/regressing counters and identities are retained; fields are never zero-filled.',
            'GPU clocks/PL1/temperatures are sample-weighted observations, not wattage or causal timing.',
            'The final failed chunk has no retained samples, numeric maximum difference or allclose diagnostic. Its hash cannot quantify corruption or distinguish its own window from preroll history.',
            'None omits only benchmark-created UI/audio children; this is not an isolated desktop or zero global GPU load.',
            'Clock tick rate 100 matches the laptop measurement host; original pin paths are informational and not opened.']}
    args.output.write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps({'output':str(args.output),'selected':len(rows),'worst':worst,'failed':failed,
        'events_parse_errors':event_info['parse_error_count'],'trace_parse_errors':trace_info['parse_error_count']}))


if __name__ == '__main__':
    main()

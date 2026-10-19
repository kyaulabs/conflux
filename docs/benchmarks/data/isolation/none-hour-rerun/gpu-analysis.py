#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Post-run strict, bounded-request host audit; never launches a workload.

Requires the frozen earlier strict helper via --strict-helper. The helper source
is hashed and must accompany archival reproduction. JSONL may be plain or gzip.
Recorded runtime/model paths are informational and are never opened.
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
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_directory',type=Path)
    parser.add_argument('--strict-helper',type=Path,required=True)
    parser.add_argument('--kernel',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('strict_helper',args.strict_helper)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    root = args.run_directory.resolve()
    result_path = root/'result.json'
    result = json.loads(result_path.read_bytes())
    if result.get('status') in ('starting','running') or not result.get('finished_at_utc'):
        parser.error('Final completed result required')
    def path(name):
        plain = root/name
        return plain if plain.exists() else root/(name+'.gz')
    event_rows,event_info = helper.jsonl(path('events.jsonl'))
    trace_rows,trace_info = helper.jsonl(path('native-trace.jsonl'))
    events = [event for _,event in event_rows]
    starts, chunks, hops = {}, {}, {}
    duplicate_keys = []
    for line,event in event_rows:
        target = {'started':starts,'chunk':chunks,'soak_hop':hops}.get(event.get('event'))
        if target is not None:
            key = event['request_id']
            if key in target:
                duplicate_keys.append({'event':event['event'],'request_id':key})
            target[key] = (line,event)
    failed = max(chunks)
    worst = max(hops,key=lambda key:hops[key][1]['hop_service_ms'])
    failed_event = chunks[failed][1]
    origins = {failed_event['window_start_frame'],hops[worst][1]['window_start_frame']}
    selected = set(range(max(1,worst-3),failed+1))
    for origin in origins:
        identical = [key for key,(_,event) in hops.items() if event['window_start_frame']==origin
                     and event['frame_start']==origin]
        if identical:
            selected.update((min(identical),max(identical)))
    trace = [sample for _,sample in trace_rows]
    times = [sample['timestamp_monotonic'] for sample in trace]
    regressions = [line for ((_,left),(line,right)) in zip(trace_rows,trace_rows[1:])
                   if right['timestamp_monotonic']<left['timestamp_monotonic']]
    if regressions:
        raise RuntimeError('Trace time regression; do not use bisect join')
    units = result['native_memory_cpu']['system_sampling']['fields']
    wanted_system = ('gt_act_freq_mhz','gt_cur_freq_mhz','throttle_reason_pl1','throttle_reason_thermal',
        'rc6_residency_ms','runtime_status','runtime_active_time','runtime_suspended_time',
        'thermal_zone5/temp','thermal_zone7/temp','platform_profile','energy_performance_preference')
    def reduce_host(host):
        system = host['system']
        host['system'] = {key:value for key,value in system.items() if key.endswith(wanted_system)
            or key in ('/proc/meminfo:MemAvailable','/proc/meminfo:SwapFree','/tmp:available_bytes')
            or key.startswith('/proc/vmstat:') or key.endswith((':some.total',':full.total'))}
        host['drm']['engines'] = [engine for engine in host['drm']['engines']
                                 if engine['field']=='drm-engine-render']
        return host
    rows = []
    for key in sorted(selected):
        if key not in starts or key not in chunks:
            continue
        start_line,started = starts[key]
        terminal_line,chunk = chunks[key]
        if (started['generation']!=chunk['generation'] or started['window_start_frame']!=chunk['window_start_frame']
                or started['source_id']!=chunk['source_id'] or started.get('preroll')):
            raise RuntimeError('Request identity mismatch or unsupported preroll')
        left,right = started['timestamp'],started['timestamp']+chunk['request_ms']/1000
        begin,end = bisect.bisect_right(times,left),bisect.bisect_left(times,right)
        interior = trace[begin:end]
        expected = result['reference_hashes'].get(str(chunk['window_start_frame']))
        rows.append({'request_id':key,'started_event_line':start_line,'chunk_event_line':terminal_line,
            'started':started,'chunk':chunk,'accepted':key in hops,
            'accepted_parity':hops.get(key,(None,{}))[1].get('parity'),
            'known_same_grid_sha256':expected,'matches_known_same_grid_sha256':chunk['samples_sha256']==expected,
            'native_exchange_bounds':{'start':left,'end':right},
            'strict_trace_line_bounds':[trace_rows[begin][0],trace_rows[end-1][0]] if interior else None,
            'leading_unsampled_ms':(times[begin]-left)*1000 if interior else None,
            'trailing_unsampled_ms':(right-times[end-1])*1000 if interior else None,
            'captures_began_at_or_before_left':sum(sample.get('capture_started_monotonic',left+1)<=left for sample in interior),
            'host':reduce_host(helper.summarize(interior,units,100,result['sample_interval_seconds']))})
    kernel = json.loads(args.kernel.read_bytes())
    kernel_rows = []
    for entry in kernel['matched_entries']:
        stamp = int(entry['__MONOTONIC_TIMESTAMP'])/1e6
        intervals = [row for row in rows if row['native_exchange_bounds']['start']<stamp<row['native_exchange_bounds']['end']]
        kernel_rows.append({key:entry.get(key) for key in ('MESSAGE','__MONOTONIC_TIMESTAMP',
            '_SOURCE_MONOTONIC_TIMESTAMP','_SOURCE_BOOTTIME_TIMESTAMP','__REALTIME_TIMESTAMP','_BOOT_ID')}
            | {'journal_observed_monotonic':stamp,'request_interval_candidates':[row['request_id'] for row in intervals],
               'milliseconds_after_interval_start':{str(row['request_id']):(stamp-row['native_exchange_bounds']['start'])*1000 for row in intervals}})
    prior_controls = [event for event in events if event['event']=='control' and event['timestamp']<=failed_event['timestamp']]
    left,right = result['workload_started_monotonic'],result['workload_stopped_monotonic']
    first,last = bisect.bisect_right(times,left),bisect.bisect_left(times,right)
    output = {'schema':1,'analyzed_at_utc':datetime.now(timezone.utc).isoformat(),'run_directory':str(root),
        'status':result['status'],'workload_seconds':result['workload_seconds'],'failures':result['failures'],
        'selected_loads':result['selected_loads'],'accepted_timing':result['timing'],
        'worst_accepted_request_id':worst,'failed_chunk_request_id':failed,
        'identity_context':{'last_control_before_failure':prior_controls[-1],
            'worker_generation_at_shutdown':result['worker_statistics']['generation'],
            'source_ids':dict(Counter(event['source_id'] for event in events if event['event']=='source_ready')),
            'model_ids':sorted(set(event['model_id'] for event in events if event['event']=='chunk')),
            'failed_chunk_not_accepted':failed not in hops,
            'failed_frame_is_untrimmed':failed_event['frame_start']==failed_event['window_start_frame']},
        'diagnostic_capture':{'result_parity_failure_present':bool(result.get('parity_failure')),
            'event_parity_failure_count':sum(event['event']=='parity_failure' for event in events),
            'PCM_artifacts':[file.name for file in sorted(root.glob('parity-failure-*.f32'))],
            'scope':'Capture applies only to trimmed allclose failure. This untrimmed same-grid SHA failure branches around it; no arrays or numeric magnitude retained.'},
        'strict_rows':rows,'workload_host':reduce_host(helper.summarize(trace[first:last],units,100,result['sample_interval_seconds'])),
        'trace_fd_read_errors':[{'line':line,'timestamp':sample['timestamp_monotonic'],'count':sample['fd_read_errors']}
            for line,sample in trace_rows if sample.get('fd_read_errors')],
        'kernel':{'saved_query':kernel['command'],'returncode':kernel['returncode'],'stderr':kernel['stderr'],
            'examined_entries':kernel['examined_entries'],'parse_errors':kernel['parse_errors'],'rows':kernel_rows},
        'checks':{'no_duplicate_event_keys':not duplicate_keys,'no_trace_time_regressions':not regressions,
            'no_event_parse_errors':not event_info['parse_error_count'],'no_trace_parse_errors':not trace_info['parse_error_count'],
            'trace_rows_match_sampler':len(trace_rows)==result['native_memory_cpu']['trace']['samples_written'],
            'accepted_hop_count_matches_result':len(hops)==result['timing']['all_delivered']['chunks']},
        'provenance':{'source_pins_from_result':result['pins'],'result_sha256':sha(result_path),
            'input_events':event_info,'input_trace':trace_info,'kernel_sha256':sha(args.kernel),
            'script_sha256':sha(Path(__file__)),'strict_helper':{'path':str(args.strict_helper),'sha256':sha(args.strict_helper)}},
        'scope':['Native exchange bounds are supervisor started+request_ms, not GPU graph timestamps; strict interior observations, edges and capture timing retained without interpolation.',
            'Journal __MONOTONIC receipt-domain times are compared to host times. SOURCE_MONOTONIC/BOOTTIME remain uncalibrated and are never directly joined. Temporal overlap does not map kernel fence sequence numbers to native requests.',
            'Request identity/currentness is inferred from retained control/generation/source/model events plus frozen harness validation; kernel request cancellation remains independent of supervisor generation safety.',
            'Hash inequality proves lost byte parity, not numerical magnitude or localization. Previous window0 delivered-hop parity does not independently validate its undelivered overlap tail.',
            'PL1/clock/thermal flags and DRM engine-class accounting are observations, not wattage or causal proof; native VmSwap/faults do not cover all driver object memory.',
            'No reset/preemption cause is asserted. Saved kernel records establish native-process fence expirations; matching upstream cancellation mechanism is separately qualified against the unavailable exact hardened build.',
            'None omits benchmark UI/audio children; other desktop activity remains uncontrolled. This aborted attempt does not qualify a60-minute condition.']}
    args.output.write_text(json.dumps(output,indent=2,allow_nan=False)+'\n')
    print(json.dumps({'output':str(args.output),'selected_requests':len(rows),'failed':failed,'worst':worst,'checks':output['checks']}))


if __name__ == '__main__':
    main()

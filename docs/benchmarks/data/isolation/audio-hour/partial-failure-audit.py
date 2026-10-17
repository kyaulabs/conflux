#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded audio-only partial-failure audit of saved worker events; never starts GPU work."""
import argparse
import collections
import gzip
import hashlib
import json
import math
from pathlib import Path


def saved(path):
    return path if path.exists() else path.with_name(path.name+'.gz')


def load(path):
    with (gzip.open if saved(path).suffix=='.gz' else open)(saved(path),'rt') as stream:
        return json.load(stream)


def records(path):
    with (gzip.open if saved(path).suffix=='.gz' else open)(saved(path),'rt') as stream:
        return [json.loads(line) for line in stream]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def summary(rows):
    values=sorted(row['hop_service_ms'] for row in rows)
    return {'chunks':len(rows),'frames':sum(row['frames'] for row in rows),'p50_ms':values[math.ceil(.5*len(values))-1] if values else None,'p95_ms':values[math.ceil(.95*len(values))-1] if values else None,'p99_ms':values[math.ceil(.99*len(values))-1] if values else None,'max_ms':max(values,default=None),'above4095ms':sum(value>4095 for value in values)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-directory',type=Path,required=True)
    parser.add_argument('--reference-directory',type=Path,action='append',default=[])
    parser.add_argument('--output',type=Path,default=Path('/tmp/conflux-audio-hour-failure-audit.json'))
    args=parser.parse_args();path=args.run_directory.resolve();result=load(path/'result.json');events=records(path/'events.jsonl')
    chunks=[event for event in events if event['event']=='chunk'];hops=[event for event in events if event['event']=='soak_hop'];starts={event['request_id']:event for event in events if event['event']=='started'};accepted={(event['generation'],event['request_id']) for event in hops}
    unaccepted=[event for event in chunks if (event['generation'],event['request_id']) not in accepted]
    controls=result['controls'];done=[control for control in controls if control.get('complete')];pending=[control for control in controls if not control.get('complete')]
    assert result['status']=='error' and result['failures'] and unaccepted
    first=result['workload_started_monotonic'];last=result['workload_stopped_monotonic'];current=None;expected=None;stale=[];frame_errors=[]
    for event in events:
        if event['event']=='control':
            current=event['generation'];expected=event['frame_start'] if event['command']!='cancel' else None
        elif event['event']=='chunk':
            if event['generation']!=current:stale.append(event['request_id'])
            if expected!=event['frame_start']:frame_errors.append({'request_id':event['request_id'],'expected':expected,'actual':event['frame_start']})
            expected=event['frame_start']+event['frames']
    identities={(event['source_id'],event['model_id']) for event in chunks}
    assert identities=={(result['fixture']['sha256'],result['pins']['model']['sha256'])}
    golden=result['reference_hashes'];grid=collections.defaultdict(set);trim=collections.defaultdict(set)
    for hop in hops:
        if hop['frame_start']==hop['window_start_frame']:
            grid[str(hop['window_start_frame'])].add(hop['samples_sha256']);assert hop['samples_sha256']==golden[str(hop['window_start_frame'])]
        else:
            trim[hop['frame_start']].add(hop['samples_sha256']);assert hop['parity']['exact'] and hop['parity']['numeric_parity'] and hop['parity']['max_abs_difference']==0
    summaries={'accepted_all':summary(hops),'accepted_continuous':summary([hop for hop in hops if hop['kind']=='continuous']),'returned_all_including_rejected':summary(chunks)}
    for name,tag in [('accepted_all','all_delivered'),('accepted_continuous','continuous')]:
        derived=summaries[name];recorded=result['timing'][tag]
        assert derived['chunks']==recorded['chunks'] and derived['max_ms']==recorded['hop_service_max_ms'] and derived['above4095ms']==recorded['hops_above_target']
        assert all(derived[f'p{pct}_ms']==recorded['hop_service_percentiles_ms'][str(pct)] for pct in [50,95,99])
    reference_checks={}
    for reference in args.reference_directory:
        old=load(reference/'result.json');old_hops=[event for event in records(reference/'events.jsonl') if event['event']=='soak_hop'];old_trim=collections.defaultdict(set)
        for hop in old_hops:
            if hop['frame_start']!=hop['window_start_frame']:old_trim[hop['frame_start']].add(hop['samples_sha256'])
        reference_checks[str(reference)]={'same_source_sha256':old['fixture']['sha256']==result['fixture']['sha256'],'all9_grid_references_identical':old['reference_hashes']==golden,'trimmed_hashes_by_target':{str(target):sorted(values) for target,values in old_trim.items()},'accepted_common_trim_hashes_identical':all(trim[target]==old_trim[target] for target in trim if target in old_trim)}
    timeline=[]
    for index,control in enumerate(controls,1):
        began=first+control['at_workload_seconds'];old=starts[control['superseded_request_id']];cancel=next(event for event in events if event['event']=='control' and event['generation']==control['generation']-1);discard=next(event for event in events if event['event']=='discarded' and event['request_id']==control['superseded_request_id']);seek=next(event for event in events if event['event']=='control' and event['generation']==control['generation'])
        returned=next(event for event in chunks if event['generation']==control['generation']);previous=max((event for event in chunks if event['consumed_at']<began),key=lambda event:event['consumed_at']);start_events=[event for event in starts.values() if event['generation']==control['generation']]
        assert abs((began-old['timestamp'])*1000-control['started_to_cancel_ms'])<1e-5
        assert abs((discard['timestamp']-began)*1000-control['residual_finish_ms'])<1e-5
        timeline.append({'number':index,'complete':control['complete'],'target_frame':control['target_frame'],'superseded_request_id':control['superseded_request_id'],'superseded_generation':control['superseded_generation'],'cancel_generation':cancel['generation'],'resume_generation':seek['generation'],'started_to_cancel_ms':control['started_to_cancel_ms'],'cancel_ack_ms':control['cancel_ack_ms'],'held_request_id':previous['request_id'],'held_current_generation_before_cancel':previous['generation']==control['superseded_generation'],'held_rejected_reported':control['held_chunk_rejected'],'residual_drain_ms_recomputed':(discard['timestamp']-began)*1000,'resume_ack_ms':control['resume_ack_ms'],'returned_request_id':returned['request_id'],'returned_ready_after_cancel_ms':(returned['ready_at']-began)*1000,'returned_ready_after_seek_event_ms':(returned['ready_at']-seek['timestamp'])*1000,'returned_matches_seek_frame':returned['frame_start']==seek['frame_start'],'returned_was_accepted':(returned['generation'],returned['request_id']) in accepted,'starts':start_events})
    failures=[]
    for chunk in unaccepted:
        same_grid=chunk['frame_start']==chunk['window_start_frame'];prior_hashes={golden[str(chunk['window_start_frame'])]} if same_grid else trim[chunk['frame_start']];failure_at=result['failures'][-1]['timestamp_monotonic'];start=starts[chunk['request_id']]
        failures.append({'failure_kind':'same_grid_exact_sha256' if same_grid else 'trimmed_numeric_parity','numeric_tolerance_comparison_performed':False if same_grid else True,'chunk':chunk,'started_event':start,'trim_frames':chunk['frame_start']-chunk['window_start_frame'],'expected_same_target_accepted_hashes':sorted(prior_hashes),'matches_same_target_accepted_hash':chunk['samples_sha256'] in prior_hashes,'matches_other_known_trim_target':any(chunk['samples_sha256'] in values for target,values in trim.items() if target!=chunk['frame_start']),'failure_after_ready_ms':(failure_at-chunk['ready_at'])*1000,'failure_after_consumed_ms':(failure_at-chunk['consumed_at'])*1000,'parity_event_recorded':False,'numeric_difference_retained':False,'compared_PCM_retained':False})
    source_ready=[event for event in events if event['event']=='source_ready'];statistics={(event['mean'],event['std']) for event in source_ready}
    result_memory=result['native_memory_cpu'];facts={'qualification':'ABORTED_PARTIAL_FAILURE; no full-hour or passing-parity claim','run_directory':str(path),'raw_status':result['status'],'requested_seconds':result['requested_seconds'],'workload_seconds':last-first,'raw_failures':result['failures'],'raw_acceptance':result['acceptance'],'counts':{'events':len(events),'returned_chunk_events':len(chunks),'accepted_soak_hops':len(hops),'unaccepted_returned_chunks':len(unaccepted),'controls_started':len(controls),'controls_complete':len(done),'controls_incomplete':len(pending),'event_types':dict(collections.Counter(event['event'] for event in events))},'timing':summaries,'control_timeline':timeline,'unaccepted_failure_chunks':failures,'source_currentness':{'source_model_identities':[list(item) for item in identities],'generation_stale_chunks':stale,'frame_contiguity_errors':frame_errors,'source_ready_count':len(source_ready),'all_source_statistics_equal':len(statistics)==1,'normalization_values':[list(item) for item in statistics],'last_source_ready':source_ready[-1],'scope':'Event-order checks cover returned chunks, including rejected request74. Instrumented accept failed after currentness/source/shape checks; arrays are not archived.'},'parity':{'accepted_grid_hash_counts':{key:len(values) for key,values in grid.items()},'all_accepted_grid_hashes_match_golden':True,'accepted_trimmed_hashes_by_target':{str(target):sorted(values) for target,values in trim.items()},'all_accepted_trimmed_numeric_checks_exact':True,'reference_comparisons':reference_checks,'limitation':'Same-grid exact-SHA mismatch is established by changed hash and rejection; no allclose/tolerance comparison was performed on this branch. No max difference, reference-array hash at failure or compared PCM is retained, so numeric significance, magnitude and divergence source cannot be recovered.'},'worker_statistics_before_shutdown':result['worker_statistics_before_shutdown'],'worker_statistics_after_shutdown':result['worker_statistics'],'backend_ready':result['runtime_ready'],'worker_shutdown_ms':result['worker_shutdown_ms'],'reported_sampler_errors':{key:result_memory.get(key) for key in ['sample_errors','trace']},'raw_pins':result['pins'],'analysis_scope':'Only result/events, small lifecycle records and reference event logs read; no native full trace parsed, no producer edits or GPU work. Request75 is reported in flight at failure but its started event was not consumed before the consumer exception; telemetry_dropped remains0.','script_sha256':digest(Path(__file__)),'evidence':{name:{'bytes':saved(path/name).stat().st_size,'sha256':digest(saved(path/name))} for name in ['result.json','events.jsonl']}}
    meta=path.parent;name=path.name
    lifecycle=load(meta/f'{name}-policy.json');guard=load(meta/f'{name}-guard.json');cleanup=load(meta/f'{name}-cleanup.json');stages={key:lifecycle.get(key,{}) for key in ['before','during','before_restore','after']}
    facts['cleanup_lifecycle']={'policy_record':lifecycle,'guard_record':guard,'host_cleanup_record':cleanup,'checks':{'restored_by_runner':lifecycle.get('restored_by_runner') is True,'no_holds_at_stages':all('holds' in stage and not stage['holds'] for stage in stages.values()),'during_balanced':all(stage.get('profile')=='balanced' and stage.get('firmware')=='balanced' and stage.get('cpu_epp')=='balance_performance' for stage in [stages['during'],stages['before_restore']]),'prior_profile_restored':stages['before'].get('profile')==stages['after'].get('profile'),'prior_firmware_restored':stages['before'].get('firmware')==stages['after'].get('firmware'),'prior_EPP_restored':stages['before'].get('cpu_epp')==stages['after'].get('cpu_epp'),'idle_guard_only':guard.get('idle_only') is True and guard.get('window_guard')=={'requested':False,'status':'not_run'},'idle_inhibitor_released':guard.get('idle_inhibit_released') is True},'scope':'Expected failing benchmark return is preserved; cleanup qualification does not turn partial correctness failure into a pass. Portable audit reads saved timestamped host observations, not live /proc or policy.'}
    facts['cleanup_lifecycle']['checks'].update(expected_failure_returncode=lifecycle.get('benchmark_returncode')==lifecycle.get('runner_returncode')==1,worker_closed=cleanup.get('worker_closed') is True,all_recorded_PIDs_absent=bool(cleanup.get('PID_exists')) and all(value is False for value in cleanup['PID_exists'].values()),fixture_absent=cleanup.get('fixture_exists') is False,guard_errors_empty=cleanup.get('guard_errors')==[])
    audio=load(path/'audio.json');assert audio==result['load']['children']['audio']['report']
    began,ended=audio['streaming_started_monotonic_ns']/1e9,audio['stopped_monotonic_ns']/1e9;overlap=max(0,min(last,ended)-max(first,began))
    assert overlap==result['concurrent_audio_workload_seconds']==last-first
    fields=['callback_execution_deadline_misses','missing_buffers','invalid_buffers','truncated_frames','observed_graph_position_discontinuities','observed_position_clock_xrun_recovery_flags'];faults={field:audio[field] for field in fields}
    assert audio['callbacks']*audio['requested']['quantum_frames']==audio['frames_rendered']
    facts['audio']={'probe_status':audio['status'],'probe_returncode':result['load']['children']['audio']['returncode'],'streaming_interval_seconds':ended-began,'actual_partial_workload_overlap_seconds':overlap,'partial_workload_covered':overlap==last-first,'requested_hour_covered':overlap>=result['requested_seconds'],'raw_audio_coverage':result['audio_coverage'],'callbacks':audio['callbacks'],'frames_rendered':audio['frames_rendered'],'faults':faults,'all6_faults_zero':all(value==0 for value in faults.values()),'callback_elapsed_us':audio['callback_elapsed_us'],'negotiated':audio['negotiated'],'scope':'Audio probe remained healthy over the partial worker run; requested3600-second audio coverage FAILED because worker correctness abort stopped the experiment. Silentprobe is not stem playback; quantile histogram counts are not archived.'}
    facts['disabled_probes']={'UI':result['ui_coverage'],'UI_artifacts_absent':not any(file.name.startswith(('ui.','ui-progress')) for file in path.iterdir()),'unexpected_PCM_capture_artifacts':[file.name for file in path.glob('parity-failure-*')],'scope':'UI was not_run/null; no UI no-fault/coverage claim. Same-grid hash branch does not invoke trimmed-only capture.'}
    assert result['load_mode']=='audio' and result['selected_loads']==['audio'] and set(result['load']['children'])=={'audio'}
    assert result['ui_coverage']=={'requested':False,'status':'not_run','passed':None}
    assert result['audio_coverage']['status']=='failed' and result['audio_coverage']['passed'] is False
    facts['control_schedule']={'interval_seconds':result['control_interval_seconds'],'controls_started':len(controls),'controls_completed':len(done),'controls_incomplete':len(pending),'first_control_due_before_abort':last-first>=result['control_interval_seconds'],'scope':'Abort preceded first300-second control, so no cancellation/seek check was exercised in this trial.'}
    prior_bad=[]
    for reference in args.reference_directory:
        old_events=records(reference/'events.jsonl');old_accepted={(event['generation'],event['request_id']) for event in old_events if event['event']=='soak_hop'}
        for event in old_events:
            if event['event']=='chunk' and (event['generation'],event['request_id']) not in old_accepted:
                prior_bad.append({'run':str(reference),'request_id':event['request_id'],'frame_start':event['frame_start'],'frames':event['frames'],'samples_sha256':event['samples_sha256'],'identical_to_failed_audio_hash':event['samples_sha256']==unaccepted[-1]['samples_sha256']})
    facts['prior_unaccepted_chunk_hash_comparison']=prior_bad
    args.output.write_text(json.dumps(facts,indent=2)+'\n')
    print(json.dumps({'output':str(args.output),'counts':facts['counts'],'timing':summaries,'failed_hashes':[{key:value for key,value in failure.items() if key not in ['chunk','started_event']} for failure in failures],'final_control':timeline[-1] if timeline else None},indent=2))


if __name__=='__main__':main()

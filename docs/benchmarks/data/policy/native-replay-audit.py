#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Audit saved native-only replay evidence without starting native/GPU work."""
import argparse
import collections
from datetime import datetime, timezone
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import statistics

TARGET_MS=4095.0
FIRMWARE='/sys/firmware/acpi/platform_profile'
EPP='/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference'


def stored(path):
    return path if path.exists() else path.with_name(path.name+'.gz')


def load(path):
    path=stored(path)
    opener=gzip.open if path.suffix=='.gz' else open
    with opener(path,'rt') as stream:
        return json.load(stream)


def lines(path):
    path=stored(path)
    opener=gzip.open if path.suffix=='.gz' else open
    with opener(path,'rt') as stream:
        return [json.loads(line) for line in stream]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def fingerprint(path):
    path=stored(path)
    return {'path':str(path),'bytes':path.stat().st_size,'sha256_of_stored_bytes':digest(path)}


def distribution(values):
    values=sorted(values)
    return {'count':len(values),'min':min(values,default=None),'p50':values[math.ceil(len(values)*.5)-1] if values else None,'p95':values[math.ceil(len(values)*.95)-1] if values else None,'p99':values[math.ceil(len(values)*.99)-1] if values else None,'max':max(values,default=None),'above_4095ms':sum(value>TARGET_MS for value in values)}


def current_pin(path,pin):
    return {'recorded_sha256':pin['sha256'],'recorded_bytes':pin['bytes'],'current_sha256':digest(path) if path.is_file() else None,'matches_current':path.is_file() and path.stat().st_size==pin['bytes'] and digest(path)==pin['sha256']}


def audit_run(directory,cpu_check,repository,host_cleanup):
    directory=directory.resolve();metadata=directory.parent
    result=load(directory/'result.json');rows=lines(directory/'requests.jsonl');trace=lines(directory/'native-trace.jsonl')
    before=load(metadata/f'before-{directory.name}.json');lifecycle=load(metadata/f'{directory.name}-policy.json')
    assert rows==result['requests'] and len(rows)==result['completed_requests']
    assert [row['request_id'] for row in rows]==list(range(1,len(rows)+1))
    assert len(rows)==result['requested_requests'] and result['status']=='ok' and not result['failures']
    assert result['replay_seconds']==result['replay_stopped_monotonic']-result['replay_started_monotonic']
    assert result['replay_seconds']<=result['max_replay_seconds']
    gaps=[]
    for index,row in enumerate(rows):
        metrics=row['metrics'];request=row['request_id']
        assert metrics['id']==request and metrics['event']=='result'
        assert metrics['frames']==result['window_frames'] and metrics['stems']==4
        assert row['finished_at']>=row['started_at']>=result['replay_started_monotonic']
        assert row['finished_at']<=result['replay_stopped_monotonic']
        assert abs(row['round_trip_ms']-(row['finished_at']-row['started_at'])*1000)<1e-6
        assert all(math.isfinite(value) for key,value in metrics.items() if key.endswith('_ms'))
        assert 0<=metrics['graph_ms']<=metrics['inference_wall_ms']<=metrics['elapsed_ms']<=row['round_trip_ms']
        if index:
            gap=(row['started_at']-rows[index-1]['finished_at'])*1000
            assert gap>=0
            gaps.append(gap)
    roundtrip=distribution([row['round_trip_ms'] for row in rows]);graph=distribution([row['metrics']['graph_ms'] for row in rows])
    for name,derived in [('round_trip',roundtrip),('graph',graph)]:
        assert {str(p):derived[f'p{p}'] for p in [50,95,99]}==result[f'{name}_percentiles_ms']
    native=result['runtime_ready'];resources=result['resources'];identities={(row['pid'],row['start_ticks']) for row in trace}
    assert len(identities)==1 and len(resources['processes'])==1
    pid,start=next(iter(identities));process=resources['processes'][0]
    assert pid==native['pid']==process['pid'] and start==process['start_ticks']
    assert len(trace)==resources['samples']==resources['trace']['samples_written']==process['samples']
    assert native['fence_wait_mode']=='blocking' and native['backend']=='vulkan' and native['threads']==4 and native['weight_storage']=='native'
    assert native['process_environment']=={'OMP_WAIT_POLICY':'PASSIVE','GOMP_SPINCOUNT':'0'}
    assert native['segment_frames']==result['window_frames']==cpu_check['shape'][0] and native['channels']==cpu_check['shape'][1]==2
    assert result['window_start_frame']==cpu_check['window_start_frame']
    assert result['input']['sha256']==cpu_check['input_sha256'] and result['input']['bytes']==cpu_check['input_bytes']
    input_path=directory/'input.f32';input_hash=digest(input_path)
    assert input_path.stat().st_size==cpu_check['input_bytes'] and input_hash==cpu_check['input_sha256']
    assert result['pins']['driver']['sha256']==cpu_check['driver_sha256']
    assert result['fixture']['sha256']==cpu_check['fixture_sha256']
    assert result['normalization']['mean']==cpu_check['mean'] and result['normalization']['std']==cpu_check['std']
    assert cpu_check['byte_identical_to_actual_worker_methods'] and cpu_check['native_or_subprocess_calls']==0 and cpu_check['temporary_fixture_removed']
    source_hashes={row['source_sha256'] for row in rows};inputs={row['input_sha256'] for row in rows};outputs={row['output_sha256'] for row in rows}
    assert source_hashes=={cpu_check['fixture_sha256']} and inputs=={input_hash} and len(outputs)==1
    assert all(row['matches_first_output'] for row in rows)
    observed_fields=collections.Counter();max_status={};fd_errors=0;sample_gaps=[];capture=[];task_sets=set();task_read_errors=0
    for index,row in enumerate(trace):
        assert row['capture_started_monotonic']<=row['timestamp_monotonic']
        assert abs((row['timestamp_monotonic']-row['capture_started_monotonic'])*1000-row['capture_elapsed_ms'])<1e-6
        capture.append(row['capture_elapsed_ms']);fd_errors+=row['fd_read_errors']
        for key,value in row['process_status_kB'].items():max_status[key]=max(max_status.get(key,0),value)
        for key in row['system_values']:observed_fields[key]+=1
        task=row['task_schedstat_ns'];task_sets.add(tuple(task['task_ids']));task_read_errors+=task['read_errors']
        if index:
            gap=(row['timestamp_monotonic']-trace[index-1]['timestamp_monotonic'])*1000
            assert gap>0 and abs(gap-row['previous_sample_gap_ms'])<1e-6
            sample_gaps.append(gap)
        else:assert row['previous_sample_gap_ms'] is None
    hz=os.sysconf('SC_CLK_TCK');cpu=(trace[-1]['cpu_ticks']-trace[0]['cpu_ticks'])/hz
    assert max_status==process['max_process_status_kB'] and fd_errors==process['fd_read_errors'] and abs(cpu-process['observed_cpu_seconds'])<1e-7
    assert process['first_process_counters']==trace[0]['process_counters'] and process['last_process_counters']==trace[-1]['process_counters']
    policy={}
    for name,path,expected in [('firmware',FIRMWARE,'balanced'),('cpu0_EPP',EPP,'balance_performance')]:
        counts=collections.Counter(row['system_values'][path] for row in trace if path in row['system_values']);missing=len(trace)-sum(counts.values())
        policy[name]={'expected':expected,'counts':dict(counts),'missing_samples':missing,'deviating_samples':sum(count for value,count in counts.items() if value!=expected),'passed':missing==0 and counts.get(expected,0)==len(trace)}
    stages={key:lifecycle.get(key,{}) for key in ['before','during','before_restore','after']}
    cleanup={'benchmark_returned_success':lifecycle.get('benchmark_returncode')==0,'runner_returned_success':lifecycle.get('runner_returncode')==0,'restored_by_runner':lifecycle.get('restored_by_runner') is True,'all_stages_no_holds':all('holds' in stage and not stage['holds'] for stage in stages.values()),'during_and_before_restore_balanced':all(stage.get('profile')=='balanced' and stage.get('firmware')=='balanced' and stage.get('cpu_epp')=='balance_performance' for stage in [stages['during'],stages['before_restore']]),'profile_restored':bool(stages['before'].get('profile')) and stages['after'].get('profile')==stages['before'].get('profile'),'firmware_restored':bool(stages['before'].get('firmware')) and stages['after'].get('firmware')==stages['before'].get('firmware'),'EPP_restored':bool(stages['before'].get('cpu_epp')) and stages['after'].get('cpu_epp')==stages['before'].get('cpu_epp'),'generated_source_absent':not (directory/'source.wav').exists()}
    host_run=host_cleanup.get('runs',{}).get(directory.name,{}) if host_cleanup else {}
    host_pid=host_run.get('recorded_processes',{}).get(str(pid))
    cleanup['native_PID_absent_in_saved_host_snapshot']=(host_pid is False) if host_cleanup else None
    cleanup['fixture_absent_in_saved_host_snapshot']=(host_run.get('fixture_exists') is False) if host_cleanup else None
    cleanup['restored_profile_confirmed_in_saved_host_snapshot']=(host_cleanup.get('profile')==stages['before'].get('profile') and 'holds' in host_cleanup and not host_cleanup['holds'] and host_cleanup.get('firmware')==stages['before'].get('firmware') and host_cleanup.get('epp')==stages['before'].get('cpu_epp')) if host_cleanup else None
    proc_cleanup={'saved_host_record':host_run,'scope':host_cleanup.get('scope') if host_cleanup else 'No host snapshot supplied; no live /proc or policy read by this portable audit','observed_at_utc':host_cleanup.get('observed_at_utc') if host_cleanup else None}
    pins={name:current_pin(Path(pin['path']),pin) for name,pin in result['pins'].items()}
    before_pins={}
    for name,pin in before.get('pins',{}).items():
        path=Path(name);path=path if path.is_absolute() else repository/path
        before_pins[name]=current_pin(path,pin)
    stderr=(directory/'runtime.stderr').read_text()
    stderr_size=(directory/'runtime.stderr').stat().st_size
    assert stderr_size==min(result['stderr_capture']['original_bytes'],result['stderr_capture']['retained_tail_limit_bytes'])
    counter_deltas={key:trace[-1]['process_counters'][key]-trace[0]['process_counters'][key] for key in trace[0]['process_counters']}
    sampled_span=trace[-1]['timestamp_monotonic']-trace[0]['timestamp_monotonic']
    return {'directory':str(directory),'raw_status':result['status'],'completed_requests':len(rows),'requested_requests':result['requested_requests'],'request_ids':{'first':rows[0]['request_id'],'last':rows[-1]['request_id'],'consecutive_from1':True,'native_result_IDs_match':True,'serial_nonoverlapping':True},'duration':{'replay_seconds':result['replay_seconds'],'max_replay_seconds':result['max_replay_seconds'],'first_start_to_last_finish_seconds':rows[-1]['finished_at']-rows[0]['started_at'],'sum_round_trips_seconds':sum(row['round_trip_ms'] for row in rows)/1000,'loop_overhead_seconds':result['replay_seconds']-sum(row['round_trip_ms'] for row in rows)/1000,'between_request_gap_ms':distribution(gaps),'scope':result['budget_scope']},'timing_ms':{'round_trip':roundtrip,'graph':graph,'inference_cpu':distribution([row['metrics']['inference_cpu_ms'] for row in rows]),'graph_rebuild_ms_max':max(row['metrics']['graph_rebuild_ms'] for row in rows),'percentile_method':'nearest rank; p99ceil(0.99*N), finite repeated fixed-window observations'},'hashes':{'fixture':next(iter(source_hashes)),'input':input_hash,'output':next(iter(outputs)),'input_bytes':input_path.stat().st_size,'all_request_input_source_hashes_match':True,'all_logged_output_hashes_identical':True,'matches_frozen_CPU_window3':True,'window_start_frame':result['window_start_frame'],'normalization':result['normalization'],'scope':'Input bytes independently rehashed and compared with frozen CPU-equivalence artifact. Source WAV was removed; source identity and output hashes are logged identities, not a fresh rehash of deleted samples.'},'inherited_GPU_environment':result['inherited_GPU_environment'],'backend':{'ready':native,'observed_process_identity':{'pid':pid,'start_ticks':start},'sampled_process_identity_count':len(identities),'scope':'One ready object and one PID/start identity in native sampler; driver starts one NativeRuntime. No worker-supervisor event stream.'},'policy':{'trace':policy,'all_native_snapshots_balanced':all(value['passed'] for value in policy.values()),'lifecycle':lifecycle,'prelaunch_metadata_profile':before.get('profile'),'prelaunch_vs_wrapper_before_profile_matches':before.get('profile')==stages['before'].get('profile'),'scope':'Native sysfs snapshots plus wrapper lifecycle; pre-smoke metadata used different observation context per replay prelaunch note. Wrapper host before/after are cleanup authority; snapshots do not prove continuous daemon state.'},'cleanup':{'checks':cleanup,'passed':all(value is True for value in cleanup.values()),'process_observation':proc_cleanup,'scope':'Lifecycle records and separate timestamped host cleanup snapshot are replayed from saved files; this portable script does not query live /proc or policy. Generated source removed; saved normalized input intentionally retained. Native scratch path was not logged, so per-directory scratch cleanup is not independently reconstructable.'},'pins_after':pins,'before_pins_after':before_pins,'resources':{'samples':len(trace),'requested_interval_seconds':resources['interval_seconds'],'successful_sample_span_seconds':sampled_span,'first_sample_after_replay_start_seconds':trace[0]['timestamp_monotonic']-result['replay_started_monotonic'],'last_sample_after_replay_stop_seconds':trace[-1]['timestamp_monotonic']-result['replay_stopped_monotonic'],'sample_gap_ms':distribution(sample_gaps),'capture_elapsed_ms':distribution(capture),'native_status_max_kB':max_status,'cpu_seconds_first_to_last_sample':cpu,'clock_ticks_per_second_from_analysis_host':hz,'process_counters_first':trace[0]['process_counters'],'process_counters_last':trace[-1]['process_counters'],'process_counter_deltas':counter_deltas,'sample_errors':resources['sample_errors'],'trace_errors':resources['trace']['errors'],'system_read_errors':resources['system_sampling']['read_errors'],'fd_read_errors':fd_errors,'task_identity_sets':len(task_sets),'task_schedstat_read_errors':task_read_errors,'system_field_missing_samples':{key:len(trace)-count for key,count in observed_fields.items() if count!=len(trace)},'scope':'Exact process kB maxima separately from DRM allocations. CPU conversion requires collection host SC_CLK_TCK; this audit runs on same laptop. A forced final sample follows the loop stop and can shorten the last sample gap. Missing/denied system fields are not zero. Counter deltas apply only between samples in same identity.'},'stderr':{'bytes':stderr_size,'text':stderr,'scope':'Retained bounded diagnostic tail; listed Vulkan device discovery is not an error counter.'},'failures':result['failures'],'input_evidence':{name:fingerprint(directory/name) for name in ['result.json','requests.jsonl','native-trace.jsonl','input.f32','runtime.stderr']},'metadata_evidence':{'before':fingerprint(metadata/f'before-{directory.name}.json'),'lifecycle':fingerprint(metadata/f'{directory.name}-policy.json')},'scope':'Native-only repeat of one frozen normalized input; no audio/UI benchmark load, overlap-add, StemWorker queueing, seeks or cancellation. Passing this replay does not complete M0.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-directory',action='append',type=Path,required=True)
    parser.add_argument('--cpu-window-check',type=Path,required=True)
    parser.add_argument('--repository-root',type=Path,default=Path.cwd())
    parser.add_argument('--host-cleanup-snapshot',type=Path)
    parser.add_argument('--output',type=Path,default=Path('/tmp/conflux-native-replay-audit.json'))
    args=parser.parse_args();cpu_check=load(args.cpu_window_check);host_cleanup=load(args.host_cleanup_snapshot) if args.host_cleanup_snapshot else None
    facts={'generated_utc':datetime.now(timezone.utc).isoformat(),'schema':1,'script_sha256':digest(Path(__file__)),'arguments':{key:[str(item) for item in value] if isinstance(value,list) else str(value) if isinstance(value,Path) else value for key,value in vars(args).items()},'CPU_window3_check':{'record':cpu_check,'evidence':fingerprint(args.cpu_window_check)},'runs':{directory.name:audit_run(directory,cpu_check,args.repository_root.resolve(),host_cleanup) for directory in args.run_directory}}
    facts['host_cleanup_evidence']=fingerprint(args.host_cleanup_snapshot) if args.host_cleanup_snapshot else None
    if args.cpu_window_check.with_suffix('.py').exists():facts['CPU_window3_check']['source_evidence']=fingerprint(args.cpu_window_check.with_suffix('.py'))
    outputs={run['hashes']['output'] for run in facts['runs'].values()};inputs={run['hashes']['input'] for run in facts['runs'].values()}
    facts['cross_run_identity']={'same_input_hash':len(inputs)==1,'same_logged_output_hash':len(outputs)==1,'same_runtime_model_driver_worker_sampler_pins':{label:len({run['pins_after'][label]['recorded_sha256'] for run in facts['runs'].values()})==1 for label in ['runtime','model','driver','worker_source','sampler_source','shared_benchmark_source']}}
    facts['limitations']=['No waveform outputs are archived; output equality is checked through recorded hashes, not a fresh byte comparison of saved arrays.','A finite fixed-window native-only replay cannot establish mixed-input or hour-long audio/UI worker latency.','0.5-second snapshots do not prove every intermediate policy/scheduling state; RAPLenergy reads can be denied.','The pre-smoke before metadata and host wrapper before differ; native-smoke-policy.json records power-saver as authoritative restoration target.','Native scratch path is not recorded. Saved timestamped host snapshot confirms recorded PID/fixture absence and restored policy; portability does not imply a fresh live-host observation. Per-directory scratch removal is not independently reconstructable.']
    args.output.write_text(json.dumps(facts,indent=2)+'\n')
    print(json.dumps({'output':str(args.output),'runs':{name:{'completed_requests':run['completed_requests'],'duration':run['duration']['replay_seconds'],'round_trip_ms':run['timing_ms']['round_trip'],'graph_ms':run['timing_ms']['graph'],'policy_pass':run['policy']['all_native_snapshots_balanced'],'cleanup_pass':run['cleanup']['passed'],'resources':run['resources']} for name,run in facts['runs'].items()}},indent=2))


if __name__=='__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Saved-evidence audit for selected-load M0 isolation; no runtime or GPU work."""
import argparse,collections,gzip,hashlib,json,math,statistics
from pathlib import Path
ROOT=Path.cwd()
TARGET=4095.0

def load(p):return json.loads(p.read_text())
def evidence_path(p):
 return p if p.exists() else p.with_name(p.name+'.gz')
def lines(p):
 p=evidence_path(p)
 opener=gzip.open if p.suffix=='.gz' else open
 with opener(p,'rt') as stream:return [json.loads(x) for x in stream]
def digest(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def summary(rows):
 vals=sorted(x['hop_service_ms'] for x in rows)
 return {'chunks':len(rows),'frames':sum(x['frames'] for x in rows),'p50_ms':vals[math.ceil(.50*len(vals))-1] if vals else None,'p95_ms':vals[math.ceil(.95*len(vals))-1] if vals else None,'p99_ms':vals[math.ceil(.99*len(vals))-1] if vals else None,'max_ms':max(vals,default=None),'above_4095ms':sum(x>TARGET for x in vals)}
def span_numbers(vals):return {'min':min(vals),'median':statistics.median(vals),'max':max(vals)} if vals else None

def numeric_summary(values):
 values=sorted(values)
 return {'count':len(values),'min':min(values,default=None),'p50':values[math.ceil(.5*len(values))-1] if values else None,'p95':values[math.ceil(.95*len(values))-1] if values else None,'p99':values[math.ceil(.99*len(values))-1] if values else None,'max':max(values,default=None)}

def trace_details(trace,r):
 import os
 hz=os.sysconf('SC_CLK_TCK');a,z=trace[0],trace[-1];gaps=[];captures=[];maxima={};fields=collections.Counter();system={};sched_ids=collections.Counter();fd_errors=0
 for i,row in enumerate(trace):
  assert row['capture_started_monotonic']<=row['timestamp_monotonic']
  assert abs((row['timestamp_monotonic']-row['capture_started_monotonic'])*1000-row['capture_elapsed_ms'])<1e-6
  captures.append(row['capture_elapsed_ms']);fd_errors+=row['fd_read_errors']
  for key,value in row['process_status_kB'].items():maxima[key]=max(maxima.get(key,0),value)
  sched=row['task_schedstat_ns'];sched_ids[tuple(sched['task_ids'])]+=1
  if i:
   gap=(row['timestamp_monotonic']-trace[i-1]['timestamp_monotonic'])*1000;assert gap>0
   assert abs(gap-row['previous_sample_gap_ms'])<1e-6;gaps.append(gap)
  else:assert row['previous_sample_gap_ms'] is None
  for key,value in row['system_values'].items():fields[key]+=1;system.setdefault(key,[]).append(value)
 proc=r['native_memory_cpu']['processes'][0]
 assert maxima==proc['max_process_status_kB'] and fd_errors==proc['fd_read_errors']
 cpu=(z['cpu_ticks']-a['cpu_ticks'])/hz;assert abs(cpu-proc['observed_cpu_seconds'])<1e-7
 assert a['process_counters']==proc['first_process_counters'] and z['process_counters']==proc['last_process_counters']
 selected={key:{'min':min(vals),'max':max(vals),'first':vals[0],'last':vals[-1]} for key,vals in system.items() if key.startswith(('/tmp:', '/proc/meminfo:', '/proc/pressure:', '/proc/vmstat:'))}
 return {'interval_requested_seconds':r['native_memory_cpu']['interval_seconds'],'sample_count':len(trace),'first_monotonic':a['timestamp_monotonic'],'last_monotonic':z['timestamp_monotonic'],'successful_sample_span_seconds':z['timestamp_monotonic']-a['timestamp_monotonic'],'sample_gap_ms':numeric_summary(gaps),'capture_elapsed_ms':numeric_summary(captures),'native_max_status_kB_recomputed':maxima,'native_observed_cpu_seconds_recomputed':cpu,'clock_ticks_per_second':hz,'clock_ticks_scope':'SC_CLK_TCK from analysis host; CPU conversion requires matching collection-host clock tick rate','native_fd_read_errors_recomputed':fd_errors,'process_counter_deltas':{k:z['process_counters'][k]-a['process_counters'][k] for k in a['process_counters']},'all_readable_system_fields_present_every_sample':all(count==len(trace) for count in fields.values()),'system_field_missing_samples':{key:len(trace)-count for key,count in fields.items() if count<len(trace)},'system_field_count':len(fields),'system_field_stats':selected,'task_identity_sets':len(sched_ids),'task_schedstat_read_errors':sum(x['task_schedstat_ns']['read_errors'] for x in trace),'sched_schedstats_values':dict(collections.Counter(system.get('/proc/sys/kernel/sched_schedstats',[]))),'firmware_profile_snapshots':dict(collections.Counter(system.get('/sys/firmware/acpi/platform_profile',[]))),'cpu0_EPP_snapshots':dict(collections.Counter(system.get('/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference',[]))),'sampling_scope':'Successful process/system snapshots; requested interval is recorded separately. Capture cost excludes trace write; gaps include work, requested sleep and scheduling. Counter deltas require stable identities; sched_schedstats=0 limits interpretation. CPU first-to-last excludes process work before first sample. FD read errors do not identify a cause.'}

def audit(directory,selected):
 p=Path(directory);r=load(p/'result.json');events=lines(p/'events.jsonl');hops=[x for x in events if x.get('event')=='soak_hop'];chunks=[x for x in events if x.get('event')=='chunk'];starts={(x['generation'],x['request_id']):x for x in events if x.get('event')=='started'}
 assert len(hops)==len(chunks)
 bykey={(x['generation'],x['request_id']):x for x in chunks}
 continuous=[x for x in hops if x['kind']=='continuous']
 first,last=r['workload_started_monotonic'],r['workload_stopped_monotonic'];duration=last-first
 derived={'all_delivered':summary(hops),'continuous':summary(continuous)}
 for tag,rows in [('all_delivered',hops),('continuous',continuous)]:
  s=derived[tag];ref=r['timing'][tag];assert s['chunks']==ref['chunks'] and s['frames']==ref['frames'] and s['max_ms']==ref['hop_service_max_ms'] and s['above_4095ms']==ref['hops_above_target']
  assert all(s[f'p{pct}_ms']==ref['hop_service_percentiles_ms'][pct] for pct in ['50','95','99'])
 bins=[]
 for begin in range(0,math.ceil(duration),300):
  rows=[x for x in hops if begin<=x['ready_at']-first<begin+300]
  s={'start_seconds':begin,'end_seconds':min(begin+300,duration),**summary(rows)};bins.append(s)
  original=next(x for x in r['timing']['bins_300_seconds'] if x['start_seconds']==begin)
  assert s['chunks']==original['chunks'] and s['above_4095ms']==original['hops_above_target'] and s['max_ms']==original['hop_service_max_ms']
  if rows:assert all(s[f'p{pct}_ms']==original['hop_service_percentiles_ms'][pct] for pct in ['50','95','99'])
 # Independently follow the logged epoch controls and frame coverage.
 current=None;expected=None;contiguity=0;stale=0;identity=set();references={};same_grid=0;trimmed=[];complete_counts=collections.Counter();source_start=0
 for e in events:
  kind=e.get('event')
  if kind=='control':
   current=e['generation'];expected=e['frame_start'] if e['command']!='cancel' else None
   if e['command']!='cancel':source_start=e['frame_start']
  elif kind=='chunk':
   stale+=e['generation']!=current
   assert expected is not None and e['frame_start']==expected
   expected+=e['frames'];contiguity+=1
   assert e['ready_queue_depth']<=2
   identity.add((e['source_id'],e['model_id']))
  elif kind=='complete' and e['generation']==current:
   assert expected==r['fixture']['frames'];complete_counts['full' if source_start==0 else 'resumed']+=1
 for h in hops:
  c=bykey[h['generation'],h['request_id']]
  assert all(h[k]==c[k] for k in ['frame_start','frames','window_start_frame','ready_at','hop_service_ms','samples_sha256'])
  assert abs(h['elapsed_seconds']-(h['ready_at']-first))<1e-8
  key=h['window_start_frame'];trim=h['frame_start']-key
  if trim:
   assert h['parity']['exact'] and h['parity']['numeric_parity'] and h['parity']['max_abs_difference']==0
   trimmed.append((h['frame_start'],h['samples_sha256']))
  elif key in references:
   assert h['samples_sha256']==references[key];same_grid+=1
  else:references[key]=h['samples_sha256']
 assert same_grid==r['same_grid_hash_checks'] and len(trimmed)==r['trimmed_seek_checks']
 assert {str(k):v for k,v in references.items()}==r['reference_hashes']
 assert complete_counts['full']==r['full_source_loops'] and complete_counts['resumed']==r['resumed_source_completions']
 # Reconstruct control timings from event timestamps; call-return acknowledgement
 # is recorded only in result, so verify it encloses the matching control event.
 discarded={x['request_id']:x for x in events if x.get('event')=='discarded'}
 control_events=[x for x in events if x.get('event')=='control']
 check_controls=[]
 for c in r['controls']:
  began=first+c['at_workload_seconds'];old=starts[c['superseded_generation'],c['superseded_request_id']]
  discard=discarded[c['superseded_request_id']];resume=next(x for x in control_events if x['generation']==c['generation']);cancel=next(x for x in control_events if x['generation']==c['generation']-1)
  replacement=next(x for x in chunks if x['generation']==c['generation']);resumed=replacement['ready_at']-c['ready_after_resume_ms']/1000
  assert c['complete'] and c['held_chunk_rejected'] and c['post_cancel_chunks']==0 and c['parity']['exact'] and c['parity']['max_abs_difference']==0
  assert abs((began-old['timestamp'])*1000-c['started_to_cancel_ms'])<1e-5
  assert abs((discard['timestamp']-began)*1000-c['residual_finish_ms'])<1e-5
  assert abs((replacement['ready_at']-began)*1000-c['ready_after_cancel_ms'])<1e-5
  assert -1e-4<=(cancel['timestamp']-began)*1000<=c['cancel_ack_ms']+1e-4
  assert -1e-4<=(resume['timestamp']-resumed)*1000<=c['resume_ack_ms']+1e-4
  assert c['delivered_after_cancel_ms']>=(replacement['consumed_at']-began)*1000
  check_controls.append({k:c[k] for k in ['at_workload_seconds','target_frame','cancel_ack_ms','resume_ack_ms','residual_finish_ms','ready_after_resume_ms','ready_after_cancel_ms','delivered_after_cancel_ms','hardware_submission_confirmed']})
 audio_evidence,ui_evidence=audit_selected_loads(p,r,events,selected)
 frozen={}
 for label,pin in r['pins'].items():
  file=Path(pin['path']);exists=file.exists();got=digest(file) if exists else None
  frozen[label]={'pinned_sha256':pin['sha256'],'current_sha256':got,'matches_current':got==pin['sha256'],'bytes':pin['bytes']}
 trace=lines(p/'native-trace.jsonl');identities={(x['pid'],x['start_ticks']) for x in trace}
 detailed_trace=trace_details(trace,r)
 assert len(identities)==1 and len(trace)==r['native_memory_cpu']['samples']==r['native_memory_cpu']['trace']['samples_written']
 assert next(iter(identities))[0]==r['runtime_ready'][0]['runtime']['pid']
 misses=[{k:h[k] for k in ['generation','request_id','elapsed_seconds','hop_service_ms','kind']} | {'graph_ms':h['metrics']['graph_ms'],'inference_cpu_ms':h['metrics']['inference_cpu_ms']} for h in hops if h['hop_service_ms']>TARGET]
 return {'raw_status':r['status'],'duration_seconds':duration,'timing':derived,'bins_300_seconds':bins,'misses':misses,'controls':{'completed':len(check_controls),'checks':check_controls,'hardware_submission_confirmed':False,'same_grid_hash_checks':same_grid,'trimmed_seek_checks':len(trimmed),'trimmed_exact_flags':True,'trimmed_repeated_hashes_consistent':all(len(set(v for k,v in trimmed if k==frame))==1 for frame in {x[0] for x in trimmed}),'parity_recompute_limit':'Waveform samples are not archived; trimmed numerical equality is an instrumented runtime assertion. Repeated whole-grid and trimmed hashes are independently checked.','contiguous_chunks_checked':contiguity,'stale_chunks_in_log':stale,'full_source_loops':complete_counts['full'],'resumed_completions':complete_counts['resumed'],'runtime_ready_count':len(r['runtime_ready']),'backend_trace_identity':list(identities)[0],'source_model_identities_in_chunks':[list(x) for x in identity],'ready_queue_high_water':r['worker_statistics']['ready_high_water'],'ready_queue_limit':r['worker_statistics']['max_ready'],'telemetry_dropped':r['worker_statistics']['telemetry_dropped'],'shutdown_ms':r['worker_shutdown_ms']},'audio':audio_evidence,'UI':ui_evidence,'frozen_pins':frozen,'trace_checks':detailed_trace,'native_trace':{'samples':len(trace),'bytes':evidence_path(p/'native-trace.jsonl').stat().st_size,'trace_errors':r['native_memory_cpu']['trace']['errors'],'sample_errors':r['native_memory_cpu']['sample_errors'],'system_read_errors':r['native_memory_cpu']['system_sampling']['read_errors'],'native_max_status_kB':r['native_memory_cpu']['processes'][0]['max_process_status_kB'],'supervisor_max_status_kB':r['supervisor_memory_cpu']['processes'][0]['max_process_status_kB']},'fixture_deleted':not Path(r['fixture']['path']).exists(),'interpretation':['Global observed p99 alone does not show absence of rare target misses.','Process-alive polling is not proof of continuous UI rendering.','FrameSwapped notifications do not measure physical presentation latency.']}




# Selected-load schema confirmed with the frozen harness owner before measurement.
LOAD_SELECTION={'none':set(),'audio':{'audio'},'both':{'audio','ui'},'ui':{'ui'}}
AUDIO_FAULTS=('callback_execution_deadline_misses','missing_buffers','invalid_buffers','truncated_frames','observed_graph_position_discontinuities','observed_position_clock_xrun_recovery_flags')


def audit_selected_loads(path,result,events,selected):
 children=result.get('load',{}).get('children',{})
 selection_checks={'children_match_selection':set(children)==selected}
 evidence={}
 first,last=result['workload_started_monotonic'],result['workload_stopped_monotonic']
 for name in ['audio','ui']:
  if name not in selected:
   unexpected=[str(evidence_path(file)) for file in [path/f'{name}.json',path/f'{name}.stderr'] if evidence_path(file).exists()]
   if name=='ui':
    unexpected.extend(str(file) for file in [path/'ui-progress.json'] if file.exists())
    if any(event['event'].startswith('ui_') for event in events):unexpected.append('UI events in worker log')
   evidence[name]={'selected':False,'status':'not_run','coverage_passed':None,'faults':None,'measurements':None,'unexpected_evidence':unexpected,'absence_verified':not unexpected and name not in children,'scope':'This probe was not selected; no no-fault or coverage claim is made.'}
   continue
  report_path=path/f'{name}.json'
  report=load(report_path)
  assert report==children[name]['report']
  checks={'child_returned_success':children[name]['returncode']==0}
  if name=='audio':
   began,ended=report['streaming_started_monotonic_ns']/1e9,report['stopped_monotonic_ns']/1e9
   overlap=max(0,min(last,ended)-max(first,began))
   assert overlap==result['concurrent_audio_workload_seconds']
   faults={field:report.get(field) for field in AUDIO_FAULTS};missing=[field for field,value in faults.items() if value is None]
   checks.update(format_valid=report['negotiated']['format_valid'],probe_status_ok=report['status']=='ok',covers_requested_duration=overlap>=result['requested_seconds'],fault_counters_present=not missing,all_six_fault_counters_zero=not missing and all(value==0 for value in faults.values()),fixed_quantum=all(report['negotiated'][field]==report['requested']['quantum_frames'] for field in ['stream_frames_min','stream_frames_max','graph_quantum_min','graph_quantum_max']),callback_frame_accounting=report['callbacks']*report['requested']['quantum_frames']==report['frames_rendered'])
   measurements={'streaming_interval_seconds':ended-began,'overlap_workload_seconds':overlap,'began_before_workload_seconds':first-began,'ended_after_workload_seconds':ended-last,'callbacks':report['callbacks'],'frames':report['frames_rendered'],'negotiated':report['negotiated'],'signal':report['signal'],'callback_elapsed_us':report['callback_elapsed_us'],'callback_budget_us':report['requested']['quantum_frames']/report['negotiated']['sample_rate']*1e6}
   evidence[name]={'selected':True,'status':'verified' if all(checks.values()) else 'failed','coverage_passed':checks['covers_requested_duration'],'checks':checks,'faults':faults,'missing_fault_counters':missing,'measurements':measurements,'scope':report['measurement_scope'],'quantile_scope':'Probe-reported histogram upper bounds; full raw histogram not archived.'}
  else:
   observation=report['frame_observation'];counts=observation['frames_per_60_seconds'];start=observation['started_monotonic_ns']/1e9;stop=observation['stopped_monotonic_ns']/1e9
   complete=[index for index,count in enumerate(counts) if start+(index+1)*60<=stop and start+(index+1)*60>first and start+index*60<last];zero=[index for index in complete if counts[index]==0]
   gap=observation['maximum_notification_gap_ms'];gate=result['ui_coverage'];progress=gate['progress'];changes=[event for event in events if event['event']=='ui_exposure_changed'];spans=[];opened=None
   for event in changes:
    pulse=event['progress'];assert pulse['pid']==progress['last_progress']['pid'];timestamp=pulse['timestamp_monotonic_ns']/1e9
    if event['exposed'] is False:assert opened is None;opened=timestamp
    else:
     assert opened is not None;span={'started_monotonic':opened,'ended_monotonic':timestamp,'seconds':timestamp-opened};assert span==event['unexposed_span'];spans.append(span);opened=None
   if opened is not None:spans.append({'started_monotonic':opened,'ended_monotonic':last,'seconds':last-opened})
   assert max((span['seconds'] for span in spans),default=0)<=progress['maximum_observed_unexposed_span_seconds']
   checks.update(accelerated_Wayland_rendered=report.get('rendered') is True and report.get('platform')=='wayland' and report.get('graphics_api')==(5 if result['renderer']=='vulkan' else 3),all_complete_bins_recorded=len(counts)>=(observation['stopped_monotonic_ns']-observation['started_monotonic_ns'])//60000000000,minute_sum_matches_frames=sum(counts)==report['frames_swapped'],observation_covers_workload=start<=first and stop>=last,all_complete_minute_bins_nonzero=bool(complete) and not zero,notification_gap_within_30s=0<=gap<=30000,runtime_gate_passed=progress['gate_failure'] is None,reported_coverage_passed=gate['passed'],exposure_budget_passed=progress['maximum_observed_unexposed_span_seconds']<=30,exposure_event_count_consistent=(sum(event['exposed'] is False for event in changes)<=progress['unexposed_observations'] and bool(changes)==(progress['unexposed_observations']>0)))
   measurements={'frames_swapped':report['frames_swapped'],'minute_sum':sum(counts),'minute_bins':counts,'complete_bins_intersecting_workload':complete,'zero_complete_bins_intersecting_workload':zero,'observation_span_seconds':stop-start,'maximum_notification_gap_ms':gap,'graphics_api':report['graphics_api'],'progress':progress,'exposure_transition_events':changes,'exposure_spans_recomputed':spans}
   evidence[name]={'selected':True,'status':'verified' if all(checks.values()) else 'failed','coverage_passed':all(checks.values()),'checks':checks,'faults':None,'measurements':measurements,'scope':observation['scope'],'exposure_scope':'Only transition pulses stream; false-pulse observation total and heartbeat maxima are instrumented summary fields.'}
 for item in evidence.values():item['selection_checks']=selection_checks
 return evidence['audio'],evidence['ui']


def declared_mode(result):
 mode=result['load_mode'];selected=LOAD_SELECTION[mode]
 assert set(result['selected_loads'])==selected and len(result['selected_loads'])==len(selected)
 assert result['load']['load_mode']==mode and set(result['load']['selected_loads'])==selected
 assert set(result['load']['children'])==selected
 assert set(result['load']['launched_loads'])==selected
 assert set(result['load']['not_run'])=={'audio','ui'}-selected
 for name,field,legacy in [('audio','audio_coverage','audio_covers_requested_duration'),('ui','ui_coverage','required_UI_coverage_completed')]:
  coverage=result[field]
  assert coverage['requested']==(name in selected)
  if name not in selected:
   assert coverage['status']=='not_run' and coverage['passed'] is None
   assert result['acceptance'][legacy] is None
  else:assert coverage['status'] in ('passed','failed') and coverage['passed']==(coverage['status']=='passed')
 if 'audio' not in selected:assert result['concurrent_audio_workload_seconds'] is None
 if mode!='both':assert result['acceptance']['full_audio_UI_soak_qualified'] is False
 return mode


def main():
 global ROOT
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--run-directory',type=Path,required=True)
 parser.add_argument('--load-mode',choices=LOAD_SELECTION,required=True)
 parser.add_argument('--reference-directory',type=Path)
 parser.add_argument('--before-policy',type=Path)
 parser.add_argument('--after-policy',type=Path)
 parser.add_argument('--policy-lifecycle',type=Path)
 parser.add_argument('--monitor',type=Path)
 parser.add_argument('--guard',type=Path)
 parser.add_argument('--host-cleanup-snapshot',type=Path)
 parser.add_argument('--repository-root',type=Path,default=Path.cwd())
 parser.add_argument('--output',type=Path,default=Path('/tmp/conflux-load-isolation-audit.json'))
 args=parser.parse_args();ROOT=args.repository_root.resolve();path=args.run_directory.resolve();result=load(path/'result.json');events=lines(path/'events.jsonl')
 selected=LOAD_SELECTION[args.load_mode];mode=declared_mode(result)
 assert mode==args.load_mode
 facts=audit(path,selected);facts.update(load_mode=mode,selected_loads=sorted(selected),acceptance=result['acceptance'],failures=result['failures'])
 facts['selected_load_evidence_valid']=all(item['selection_checks']['children_match_selection'] and (item['status']=='verified' if item['selected'] else item['absence_verified']) for item in [facts['audio'],facts['UI']])
 facts['requested_probe_coverage_passed']=all(item['coverage_passed'] for item in [facts['audio'],facts['UI']] if item['selected'])
 facts['probe_status_scope']='With no selected probes, selected-load coverage has no probe requirement; absent audio/UI remains not_run with null faults/measurements, not a measured no-fault pass.'
 facts['selected_load_acceptance_consistent']=result['acceptance']['selected_load_coverage_completed']==all(result[name+'_coverage']['passed'] is True for name in selected)
 before=load(args.before_policy) if args.before_policy else {};facts['before_pins_after']={}
 for name,pin in before.get('pins',{}).items():
  file=Path(name);file=file if file.is_absolute() else ROOT/file;actual=digest(file) if file.is_file() else None
  facts['before_pins_after'][name]={'recorded_sha256':pin['sha256'],'current_sha256':actual,'matches_current':actual==pin['sha256'] and file.stat().st_size==pin['bytes'] if file.is_file() else False}
 controls=result['controls'];hops=[event for event in events if event['event']=='soak_hop']
 keys=['started_to_cancel_ms','cancel_ack_ms','resume_ack_ms','residual_finish_ms','ready_after_resume_ms','ready_after_cancel_ms','delivered_after_cancel_ms']
 facts['controls']['ranges_ms']={key:span_numbers([control[key] for control in controls]) for key in keys}
 facts['controls']['all_started_to_cancel_at_least150ms']=all(control['started_to_cancel_ms']>=150 for control in controls)
 facts['controls']['expected11_controls_for60min']=len(controls)==11 if result['requested_seconds']==3600 else None
 held=[]
 for control in controls:
  when=result['workload_started_monotonic']+control['at_workload_seconds'];prior=[event for event in events if event['event']=='chunk' and event['consumed_at']<when];old=max(prior,key=lambda event:event['consumed_at']) if prior else None
  held.append({'request_id':old['request_id'] if old else None,'generation':old['generation'] if old else None,'current_generation_before_cancel':bool(old and old['generation']==control['superseded_generation'])})
 facts['controls']['held_chunks']=held;facts['controls']['held_current_count']=sum(item['current_generation_before_cancel'] for item in held);facts['controls']['held_already_stale_count']=sum(item['generation'] is not None and not item['current_generation_before_cancel'] for item in held)
 facts['event_counts']=dict(collections.Counter(event['event'] for event in events));facts['backend_ready']=result['runtime_ready'][0]['runtime']
 facts['bins_300_seconds_by_kind']={kind:[{'start_seconds':begin,'end_seconds':min(begin+300,facts['duration_seconds']),**summary([hop for hop in rows if begin<=hop['ready_at']-result['workload_started_monotonic']<begin+300])} for begin in range(0,math.ceil(facts['duration_seconds']),300)] for kind,rows in [('all_delivered',hops),('continuous',[hop for hop in hops if hop['kind']=='continuous'])]}
 facts['policy_qualification']={}
 for name,counts,expected in [('firmware',facts['trace_checks']['firmware_profile_snapshots'],'balanced'),('cpu0_EPP',facts['trace_checks']['cpu0_EPP_snapshots'],'balance_performance')]:
  facts['policy_qualification'][name]={'counts':counts,'expected':expected,'missing_samples':facts['trace_checks']['sample_count']-sum(counts.values()),'deviating_samples':sum(count for value,count in counts.items() if value!=expected),'passed':counts.get(expected,0)==facts['trace_checks']['sample_count']}
 for name,file in [('before',args.before_policy),('after',args.after_policy),('lifecycle',args.policy_lifecycle)]:facts['policy_qualification'][name]=load(file) if file else {'status':'not_supplied'}
 if args.policy_lifecycle:
  life=facts['policy_qualification']['lifecycle'];stages={name:life.get(name,{}) for name in ['before','during','before_restore','after']}
  checks={'benchmark_success':life.get('benchmark_returncode')==0,'runner_success':life.get('runner_returncode')==0,'restored_by_runner':life.get('restored_by_runner') is True,'no_holds_at_all_stages':all('holds' in stage and not stage['holds'] for stage in stages.values()),'during_balanced':all(stage.get('profile')=='balanced' and stage.get('firmware')=='balanced' and stage.get('cpu_epp')=='balance_performance' for stage in [stages['during'],stages['before_restore']]),'prior_profile_restored':bool(stages['before'].get('profile')) and stages['before'].get('profile')==stages['after'].get('profile'),'prior_firmware_restored':bool(stages['before'].get('firmware')) and stages['before'].get('firmware')==stages['after'].get('firmware'),'prior_EPP_restored':bool(stages['before'].get('cpu_epp')) and stages['before'].get('cpu_epp')==stages['after'].get('cpu_epp')}
  facts['policy_qualification']['lifecycle_checks']={'checks':checks,'passed':all(checks.values()),'scope':'Restoration compares recorded entry state; prior profile is not assumed power-saver.'}
 if args.monitor:
  observed=[row for row in lines(args.monitor) if row.get('run')==path.name];running=[row for row in observed if row.get('progress',{}).get('status')=='running']
  facts['policy_qualification']['monitor']={'rows':len(observed),'running_rows':len(running),'profiles':dict(collections.Counter(row.get('profile','MISSING') for row in running)),'nonempty_holds':sum(bool(row.get('holds')) for row in running),'missing_holds':sum('holds' not in row for row in running),'passed':bool(running) and all(row.get('profile')=='balanced' and 'holds' in row and not row['holds'] for row in running)}
 if args.guard:
  guard=load(args.guard);window=guard.get('window_guard',{})
  facts['guard']={'selected_UI':('ui' in selected),'record':guard,'idle_only_matches_selection':guard.get('idle_only')==('ui' not in selected),'window_guard_matches_selection':window.get('requested')==('ui' in selected) and (window.get('status')=='not_run' if 'ui' not in selected else window.get('status')=='restored'),'cleanup_passed':guard['status']=='ok' and not guard['errors'] and guard['idle_inhibit_released'] and guard['returncode']==0 and (guard.get('script_unloaded') is True if 'ui' in selected else window.get('status')=='not_run'),'idle_cookie_recorded':isinstance(guard.get('idle_inhibit_cookie'),int) and guard['idle_inhibit_cookie']>0,'window_covered_workload':guard.get('applied_monotonic',float('inf'))<result['workload_started_monotonic'] and guard.get('restored_monotonic',float('-inf'))>result['workload_stopped_monotonic'] if 'ui' in selected else None}
  if 'ui' in selected:
   facts['guard']['window_application_offset_ms']=(guard['applied_monotonic']-result['workload_started_monotonic'])*1000
   facts['guard']['window_restore_after_workload_ms']=(guard['restored_monotonic']-result['workload_stopped_monotonic'])*1000
   facts['guard']['window_PID_matches_heartbeat']=guard['ui']['pid']==result['ui_coverage']['progress']['last_progress']['pid']
   facts['guard']['scope']='Window application timing is separate from continuous UI frame evidence; a positive application offset means a startup interval before guard installation.'
 else:facts['guard']={'selected_UI':('ui' in selected),'status':'not_supplied' if 'ui' in selected else 'not_run'}
 if args.host_cleanup_snapshot:facts['host_cleanup_snapshot']={'record':load(args.host_cleanup_snapshot),'sha256':digest(args.host_cleanup_snapshot),'scope':'Saved timestamped host snapshot; portable audit does not query live process or policy state.'}
 if args.reference_directory:
  reference=load(args.reference_directory/'result.json');old_trim={(event['frame_start'],event['samples_sha256']) for event in lines(args.reference_directory/'events.jsonl') if event['event']=='soak_hop' and event['parity']['kind']=='trimmed_seek'};new_trim={(event['frame_start'],event['samples_sha256']) for event in hops if event['parity']['kind']=='trimmed_seek'}
  facts['reference_comparison']={'directory':str(args.reference_directory),'same_fixture':reference['fixture']['sha256']==result['fixture']['sha256'],'same_grid_hashes_identical':reference['reference_hashes']==result['reference_hashes'],'trimmed_hash_sets_identical':old_trim==new_trim,'pins_identical':{name:reference['pins'].get(name,{}).get('sha256')==pin['sha256'] for name,pin in result['pins'].items()},'scope':'Sequential trials toggle one selected benchmark child at a time. Fixed cooldown and starting temperature/host conditions must be reviewed separately; no causal proof from this audit.'}
 facts['resources']={name:result[name] for name in ['native_memory_cpu','supervisor_memory_cpu']}
 facts['scope']='Four separate one-hour persistent-worker cases; absent probes are not_run/null, not zero-fault. None means no added benchmark audio/UI children, not absence of desktop activity. Worker cancellation remains logical invalidation; native-only and worker/audio/UI outcomes are distinct.'
 facts['script_sha256']=digest(Path(__file__));facts['arguments']={name:str(value) if isinstance(value,Path) else value for name,value in vars(args).items()}
 args.output.write_text(json.dumps(facts,indent=2)+'\n');print(json.dumps({'output':str(args.output),'mode':mode,'timing':facts['timing'],'audio_status':facts['audio']['status'],'UI_status':facts['UI']['status'],'policy':{name:value for name,value in facts['policy_qualification'].items() if name in ['firmware','cpu0_EPP']}},indent=2))


if __name__=='__main__':main()

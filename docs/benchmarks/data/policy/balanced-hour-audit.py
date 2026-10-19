#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Independent saved-evidence audit; no runtime or GPU work."""
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

def audit(directory):
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
 audio=load(p/'audio.json');ui=load(p/'ui.json');assert audio==r['load']['children']['audio']['report'] and ui==r['load']['children']['ui']['report']
 ast,asp=audio['streaming_started_monotonic_ns']/1e9,audio['stopped_monotonic_ns']/1e9;overlap=max(0,min(last,asp)-max(first,ast));assert overlap==r['concurrent_audio_workload_seconds']
 faults={k:audio[k] for k in ['callback_execution_deadline_misses','missing_buffers','invalid_buffers','truncated_frames','observed_graph_position_discontinuities','observed_position_clock_xrun_recovery_flags']}
 assert audio['callbacks']*256==audio['frames_rendered']
 frame=ui['frame_observation'];minute=frame['frames_per_60_seconds'];assert sum(minute)==ui['frames_swapped']
 ui_start,ui_stop=frame['started_monotonic_ns']/1e9,frame['stopped_monotonic_ns']/1e9
 covered=[];zeros=[]
 for index,count in enumerate(minute):
  begin,end=ui_start+index*60,ui_start+(index+1)*60
  if end<=ui_stop and end>first and begin<last:
   covered.append(index)
   if count==0:zeros.append(index)
 frozen={}
 for label,pin in r['pins'].items():
  file=Path(pin['path']);exists=file.exists();got=digest(file) if exists else None
  frozen[label]={'pinned_sha256':pin['sha256'],'current_sha256':got,'matches_current':got==pin['sha256'],'bytes':pin['bytes']}
 trace=lines(p/'native-trace.jsonl');identities={(x['pid'],x['start_ticks']) for x in trace}
 detailed_trace=trace_details(trace,r)
 assert len(identities)==1 and len(trace)==r['native_memory_cpu']['samples']==r['native_memory_cpu']['trace']['samples_written']
 assert next(iter(identities))[0]==r['runtime_ready'][0]['runtime']['pid']
 misses=[{k:h[k] for k in ['generation','request_id','elapsed_seconds','hop_service_ms','kind']} | {'graph_ms':h['metrics']['graph_ms'],'inference_cpu_ms':h['metrics']['inference_cpu_ms']} for h in hops if h['hop_service_ms']>TARGET]
 return {'raw_status':r['status'],'duration_seconds':duration,'timing':derived,'bins_300_seconds':bins,'misses':misses,'controls':{'completed':len(check_controls),'checks':check_controls,'hardware_submission_confirmed':False,'same_grid_hash_checks':same_grid,'trimmed_seek_checks':len(trimmed),'trimmed_exact_flags':True,'trimmed_repeated_hashes_consistent':all(len(set(v for k,v in trimmed if k==frame))==1 for frame in {x[0] for x in trimmed}),'parity_recompute_limit':'Waveform samples are not archived; trimmed numerical equality is an instrumented runtime assertion. Repeated whole-grid and trimmed hashes are independently checked.','contiguous_chunks_checked':contiguity,'stale_chunks_in_log':stale,'full_source_loops':complete_counts['full'],'resumed_completions':complete_counts['resumed'],'runtime_ready_count':len(r['runtime_ready']),'backend_trace_identity':list(identities)[0],'source_model_identities_in_chunks':[list(x) for x in identity],'ready_queue_high_water':r['worker_statistics']['ready_high_water'],'ready_queue_limit':r['worker_statistics']['max_ready'],'telemetry_dropped':r['worker_statistics']['telemetry_dropped'],'shutdown_ms':r['worker_shutdown_ms']},'audio':{'streaming_interval_seconds':asp-ast,'overlap_workload_seconds':overlap,'began_before_workload_seconds':first-ast,'ended_after_workload_seconds':asp-last,'callbacks':audio['callbacks'],'frames':audio['frames_rendered'],'faults':faults,'callback_elapsed_us':audio['callback_elapsed_us'],'quantile_scope':'Probe-reported histogram upper bounds; raw histogram counts are not archived.'},'UI':{'frames_swapped':ui['frames_swapped'],'minute_sum':sum(minute),'minute_bins':minute,'complete_bins_intersecting_workload':covered,'zero_complete_bins_intersecting_workload':zeros,'observation_span_seconds':ui_stop-ui_start,'first_to_last_frame_span_seconds':(frame['last_frame_monotonic_ns']-frame['first_frame_monotonic_ns'])/1e9,'maximum_notification_gap_ms':frame['maximum_notification_gap_ms'],'graphics_api':ui['graphics_api'],'continuous_UI_notifications_supported':not zeros and frame['maximum_notification_gap_ms']<=30000,'scope':frame['scope']},'frozen_pins':frozen,'trace_checks':detailed_trace,'native_trace':{'samples':len(trace),'bytes':evidence_path(p/'native-trace.jsonl').stat().st_size,'trace_errors':r['native_memory_cpu']['trace']['errors'],'sample_errors':r['native_memory_cpu']['sample_errors'],'system_read_errors':r['native_memory_cpu']['system_sampling']['read_errors'],'native_max_status_kB':r['native_memory_cpu']['processes'][0]['max_process_status_kB'],'supervisor_max_status_kB':r['supervisor_memory_cpu']['processes'][0]['max_process_status_kB']},'fixture_deleted':not Path(r['fixture']['path']).exists(),'interpretation':['Global observed p99 alone does not show absence of rare target misses.','Process-alive polling is not proof of continuous UI rendering.','FrameSwapped notifications do not measure physical presentation latency.']}



def main():
 global ROOT
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--run-directory',type=Path,required=True)
 parser.add_argument('--reference-directory',type=Path)
 parser.add_argument('--output',type=Path,default=Path('/tmp/conflux-balanced-hour-audit.json'))
 parser.add_argument('--repository-root',type=Path,default=Path.cwd())
 parser.add_argument('--before-policy',type=Path)
 parser.add_argument('--after-policy',type=Path)
 parser.add_argument('--monitor',type=Path)
 parser.add_argument('--guard',type=Path)
 parser.add_argument('--policy-lifecycle',type=Path)
 parser.add_argument('--expected-firmware-profile',default='balanced')
 parser.add_argument('--expected-epp',default='balance_performance')
 args=parser.parse_args();ROOT=args.repository_root.resolve();p=args.run_directory.resolve()
 r=load(p/'result.json');events=lines(p/'events.jsonl');facts=audit(p);hops=[e for e in events if e['event']=='soak_hop'];controls=r['controls'];progress=r['ui_coverage']['progress']
 facts['run_directory']=str(p)
 keys=['started_to_cancel_ms','cancel_ack_ms','resume_ack_ms','residual_finish_ms','ready_after_resume_ms','ready_after_cancel_ms','delivered_after_cancel_ms']
 facts['controls']['ranges_ms']={k:span_numbers([c[k] for c in controls]) for k in keys}
 facts['controls']['all_started_to_cancel_at_least_150ms']=all(c['started_to_cancel_ms']>=150 for c in controls)
 facts['event_counts']=dict(collections.Counter(e['event'] for e in events))
 facts['controls']['backend_identity']=r['runtime_ready'][0]['runtime']
 assert facts['controls']['source_model_identities_in_chunks']==[[r['fixture']['sha256'],r['pins']['model']['sha256']]]
 started=[e for e in events if e['event']=='started'];prerolls=sum(e['preroll'] for e in started);discarded=facts['event_counts'].get('discarded',0)
 active=len(started)-len(hops)-discarded-prerolls
 facts['controls']['started_accounting']={'delivered':len(hops),'cancelled_and_discarded':discarded,'seek_prerolls':prerolls,'unmatched_active_at_shutdown':active,'total':len(started)}
 assert active in (0,1) and r['stale_chunks_delivered']==0 and r['worker_statistics']['backend_start_count']==1
 assert r['worker_statistics']['discarded_count']==discarded
 assert r['worker_statistics']['cancel_count']==len(controls)
 held=[]
 for c in controls:
  cancel_at=r['workload_started_monotonic']+c['at_workload_seconds']
  candidates=[e for e in events if e['event']=='chunk' and e['consumed_at']<cancel_at]
  old=max(candidates,key=lambda e:e['consumed_at']) if candidates else None
  if old:assert old['generation']<c['generation']
  held.append({'request_id':old['request_id'] if old else None,'generation':old['generation'] if old else None,'current_generation_before_cancel':bool(old and old['generation']==c['superseded_generation']),'age_at_cancel_ms':(cancel_at-old['ready_at'])*1000 if old else None})
 facts['controls']['held_prior_generation_chunks']=held
 facts['controls']['held_current_generation_before_cancel_count']=sum(x['current_generation_before_cancel'] for x in held)
 facts['controls']['held_already_stale_before_cancel_count']=sum(x['generation'] is not None and not x['current_generation_before_cancel'] for x in held)
 facts['controls']['held_stale_rejection_scope']='Runtime assertions reject held results after cancel. Event history distinguishes current-generation held chunks from already stale chunks. No direct GPU-submission timestamp or saved sample arrays.'
 facts['controls']['source_statistics_reused']=r['source_statistics_reused'];assert r['source_statistics_reused']==sum(e['reused'] for e in events if e['event']=='source_ready')
 changed=[e for e in events if e['event']=='ui_exposure_changed'];open_span=None;spans=[]
 for e in changed:
  pulse=e['progress'];assert pulse['pid']==progress['last_progress']['pid']
  timestamp=pulse['timestamp_monotonic_ns']/1e9
  if not e['exposed']:
   assert open_span is None;open_span=timestamp
  else:
   assert open_span is not None
   span={'started_monotonic':open_span,'ended_monotonic':timestamp,'seconds':timestamp-open_span}
   assert e['unexposed_span']==span;spans.append(span);open_span=None
 if open_span is not None:spans.append({'started_monotonic':open_span,'ended_monotonic':r['workload_stopped_monotonic'],'seconds':r['workload_stopped_monotonic']-open_span})
 assert progress['maximum_observed_unexposed_span_seconds']>=max([s['seconds'] for s in spans],default=0)
 assert progress['unexposed_observations']>=sum(not e['exposed'] for e in changed)
 facts['UI']['coverage_gate']=r['ui_coverage'];facts['UI']['exposure_transition_events']=changed;facts['UI']['exposure_spans_recomputed']=spans
 facts['UI']['exposure_observation_scope']='Only transition pulses are streamed; total false-pulse observations and heartbeat maxima remain instrumented report fields. Open spans include workload end.'
 facts['UI']['minute_bin_scope']='Complete60-second UI-observation bins overlapping workload; partial final bin excluded. Notifications are not physical presentation timing.'
 facts['UI']['expected_hour_complete_bins_observed']=len(facts['UI']['complete_bins_intersecting_workload'])==60 if r['requested_seconds']==3600 else None
 if args.guard:
  guard=load(args.guard);facts['UI']['guard']=guard
  facts['UI']['guard_cleanup_passed']=guard['status']=='ok' and not guard['errors'] and guard['script_unloaded'] and guard['idle_inhibit_released'] and guard['returncode']==guard['child_returncode']==0
  facts['UI']['guard_covered_workload']=guard['ui']['pid']==progress['last_progress']['pid'] and guard['applied_monotonic']<r['workload_started_monotonic'] and guard['restored_monotonic']>r['workload_stopped_monotonic']
 else:facts['UI']['guard_evidence']='Not supplied; cleanup and guard coverage not independently audited'
 audio=load(p/'audio.json');neg=audio['negotiated'];quantum=audio['requested']['quantum_frames']
 assert quantum==neg['stream_frames_min']==neg['stream_frames_max']==neg['graph_quantum_min']==neg['graph_quantum_max']
 assert neg['format_valid'] and audio['status']=='ok'
 facts['audio']['negotiated']=neg;facts['audio']['signal']=audio['signal'];facts['audio']['measurement_scope']=audio['measurement_scope'];facts['audio']['callback_budget_us']=quantum/neg['sample_rate']*1e6
 for sampler in ['native_memory_cpu','supervisor_memory_cpu']:
  data=r[sampler];assert len(data['processes'])==1
  facts.setdefault('resources',{})[sampler]={k:data[k] for k in ['interval_seconds','elapsed_seconds','samples','sample_errors','processes']}
 facts['resources']['scope']='Each process separate; native trace independently reaggregated, supervisor summary only. No RSS+DRM/client-peak sums.'
 facts['bins_300_seconds_by_kind']={}
 for kind,selection in [('all_delivered',hops),('continuous',[e for e in hops if e['kind']=='continuous'])]:
  facts['bins_300_seconds_by_kind'][kind]=[{'start_seconds':start,'end_seconds':min(start+300,facts['duration_seconds']),**summary([e for e in selection if start<=e['ready_at']-r['workload_started_monotonic']<start+300])} for start in range(0,math.ceil(facts['duration_seconds']),300)]
 trace=facts['trace_checks'];trace['first_sample_after_workload_start_seconds']=trace['first_monotonic']-r['workload_started_monotonic'];trace['last_sample_before_workload_stop_seconds']=r['workload_stopped_monotonic']-trace['last_monotonic']
 qualification={}
 for field,expected,counts in [('firmware_profile',args.expected_firmware_profile,trace['firmware_profile_snapshots']),('cpu0_EPP',args.expected_epp,trace['cpu0_EPP_snapshots'])]:
  observed=sum(counts.values());qualification[field]={'expected':expected,'observed_values':counts,'missing_samples':trace['sample_count']-observed,'deviating_samples':sum(n for value,n in counts.items() if value!=expected),'passed':observed==trace['sample_count'] and counts.get(expected,0)==trace['sample_count']}
 facts['policy_qualification']={'native_snapshots':qualification,'all_native_snapshots_match_expected':all(q['passed'] for q in qualification.values()),'scope':'Per-sample observations, not atomic or continuous policy history. Missing values/deviations are explicitly reported, never zero-filled. Balanced runner is expected to set/restore profile without a hold.'}
 for label,path in [('before',args.before_policy),('after',args.after_policy)]:
  facts['policy_qualification'][label]=load(path) if path else {'evidence':'Not supplied'}
 if args.monitor:
  monitor=[x for x in lines(args.monitor) if x.get('run')==p.name];running=[x for x in monitor if x.get('progress',{}).get('status')=='running']
  facts['policy_qualification']['monitor']={'rows':len(monitor),'running_rows':len(running),'running_profiles':dict(collections.Counter(x.get('profile','MISSING') for x in running)),'running_holds_nonempty':sum(bool(x.get('holds')) for x in running),'running_holds_field_missing':sum('holds' not in x for x in running),'passed':bool(running) and all(x.get('profile')==args.expected_firmware_profile and 'holds' in x and not x['holds'] for x in running),'scope':'No hold expected for balanced trial; any nonempty or missing hold/profile field requires qualification.'}
 if args.policy_lifecycle:
  lifecycle=load(args.policy_lifecycle)
  stages={key:lifecycle.get(key,{}) for key in ['before','during','before_restore','after']}
  checks={'benchmark_returned_success':lifecycle.get('benchmark_returncode')==0,'runner_returned_success':lifecycle.get('runner_returncode')==0,'runner_reports_restore':lifecycle.get('restored_by_runner') is True,'all_four_stages_present':all(stages.values()),'all_stages_have_no_hold':all('holds' in stage and not stage['holds'] for stage in stages.values()),'profile_restored':bool(stages['before'].get('profile')) and stages['after'].get('profile')==stages['before'].get('profile'),'firmware_restored':bool(stages['before'].get('firmware')) and stages['after'].get('firmware')==stages['before'].get('firmware'),'EPP_restored':bool(stages['before'].get('cpu_epp')) and stages['after'].get('cpu_epp')==stages['before'].get('cpu_epp'),'both_running_stages_match_balanced':all(stage.get('profile')==args.expected_firmware_profile and stage.get('firmware')==args.expected_firmware_profile and stage.get('cpu_epp')==args.expected_epp for stage in [stages['during'],stages['before_restore']])}
  facts['policy_qualification']['lifecycle']={'raw':lifecycle,'checks':checks,'passed':all(checks.values()),'scope':'Wrapper before/during/before_restore/after snapshots; full native trace and external monitor supply separate run observations.'}
 before=facts['policy_qualification']['before'];frozen={}
 for path,pin in before.get('pins',{}).items():
  file=Path(path);file=file if file.is_absolute() else ROOT/file;actual=digest(file) if file.exists() else None
  frozen[path]={'bytes':pin['bytes'],'before_sha256':pin['sha256'],'current_sha256':actual,'matches':actual==pin['sha256'] and file.stat().st_size==pin['bytes'] if file.exists() else False}
 facts['before_after_pins']=frozen
 facts['source_immutability_scope']='In-run pins plus optional before/current file comparisons; missing or changed current files are reported. Snapshots cannot prove no byte-for-byte-reverted intermediate mutation.'
 if args.reference_directory:
  ref=args.reference_directory.resolve();previous=load(ref/'result.json');reference_events=lines(ref/'events.jsonl')
  old_trim={(e['frame_start'],e['samples_sha256']) for e in reference_events if e['event']=='soak_hop' and e['parity']['kind']=='trimmed_seek'}
  new_trim={(e['frame_start'],e['samples_sha256']) for e in hops if e['parity']['kind']=='trimmed_seek'}
  common=set(previous['reference_hashes'])&set(r['reference_hashes'])
  facts['reference_comparison']={'directory':str(ref),'reference_timing':previous['timing'],'fixture_sha256_matches':previous['fixture']['sha256']==r['fixture']['sha256'],'all_reference_hashes_identical':previous['reference_hashes']==r['reference_hashes'],'common_reference_hashes':len(common),'common_reference_mismatches':[key for key in common if previous['reference_hashes'][key]!=r['reference_hashes'][key]],'trimmed_seek_hash_sets_identical':old_trim==new_trim,'matching_runtime_model_pins':{label:previous['pins'][label]['sha256']==r['pins'][label]['sha256'] for label in ['runtime','model','ui','audio_probe','soak_source','shared_benchmark_source','worker_source','sampler_source']},'scope':'Sequential finite trials on same fixture; does not establish causality or a production tail bound.'}
 facts['fixture']=r['fixture'];facts['acceptance']=r['acceptance'];facts['failures']=r['failures']
 facts['input_evidence_sha256']={name:{'stored_path':str(evidence_path(p/name)),'bytes':evidence_path(p/name).stat().st_size,'sha256_of_stored_bytes':digest(evidence_path(p/name))} for name in ['result.json','events.jsonl','native-trace.jsonl','audio.json','ui.json']}
 facts['limitations']=['Nearest-rank percentiles of consecutive correlated windows do not bound future tail latency.','Control replacement latency includes preroll; supervisor started+150ms is not a GPU-submission timestamp.','No archived waveform arrays or raw callback histogram: exact/numeric parity and callback quantile bounds are instrumented reports, with repeated hashes independently checked.','GUI notifications and heartbeat metadata do not measure physical presentation.','Check optional system-read/fdinfo/sample/trace errors separately; missing counters are not zero.','sched_schedstats=0 limits interpretation of runqueue counters; counter deltas need stable task/PID identities.','Source-path hashes can be compared only where the original files still exist; pin identity remains archived.']
 facts['finding']={'requested_duration_completed':r['acceptance']['requested_wall_duration_completed'],'audio_UI_coverage_passed':r['acceptance']['audio_covers_requested_duration'] and r['acceptance']['required_UI_coverage_completed'],'all_audio_fault_counters_zero':all(v==0 for v in facts['audio']['faults'].values()),'observed_continuous_p99_within_4095ms':facts['timing']['continuous']['p99_ms']<TARGET,'balanced_native_snapshot_qualification_passed':facts['policy_qualification']['all_native_snapshots_match_expected'],'scope':'No M1 advancement is implied by this saved-evidence audit.'}
 facts['audit_script_sha256']=digest(Path(__file__))
 facts['audit_arguments']={key:str(value) if isinstance(value,Path) else value for key,value in vars(args).items()}
 if args.reference_directory:
  ref_timing=facts['reference_comparison']['reference_timing']['continuous'];current=facts['timing']['continuous']
  facts['reference_comparison']['continuous_comparison']={'performance_reference_p99_ms':ref_timing['hop_service_percentiles_ms']['99'],'balanced_p99_ms':current['p99_ms'],'performance_reference_max_ms':ref_timing['hop_service_max_ms'],'balanced_max_ms':current['max_ms'],'performance_reference_miss_fraction':ref_timing['hops_above_target']/ref_timing['chunks'],'balanced_miss_fraction':current['above_4095ms']/current['chunks'],'scope':'The observed p99 can decrease while miss fraction and maximum increase. Neither result passing the target, nor causation, is implied.'}
 args.output.write_text(json.dumps(facts,indent=2)+'\n')
 print(json.dumps({'output':str(args.output),'timing':facts['timing'],'finding':facts['finding'],'control_ranges_ms':facts['controls']['ranges_ms'],'UI_frames':facts['UI']['frames_swapped'],'sample_gap_ms':trace['sample_gap_ms']},indent=2))

if __name__=='__main__':main()

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Independent saved-evidence audit; no runtime or GPU work."""
import collections,datetime,hashlib,json,math,shutil,statistics
from pathlib import Path
ROOT=Path('/home/kyau/projects/kyaulabs/conflux')
BASE=ROOT/'benchmarks-local/soak-2026-10-16'
TARGET=4095.0

def load(p):return json.loads(p.read_text())
def lines(p):return [json.loads(x) for x in p.read_text().splitlines()]
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
  sched=row['task_schedstat_ns'];assert sched['read_errors']==0;sched_ids[tuple(sched['task_ids'])]+=1
  if i:
   gap=(row['timestamp_monotonic']-trace[i-1]['timestamp_monotonic'])*1000;assert gap>0
   assert abs(gap-row['previous_sample_gap_ms'])<1e-6;gaps.append(gap)
  else:assert row['previous_sample_gap_ms'] is None
  for key,value in row['system_values'].items():fields[key]+=1;system.setdefault(key,[]).append(value)
 proc=r['native_memory_cpu']['processes'][0]
 assert maxima==proc['max_process_status_kB'] and fd_errors==proc['fd_read_errors']
 cpu=(z['cpu_ticks']-a['cpu_ticks'])/hz;assert abs(cpu-proc['observed_cpu_seconds'])<1e-7
 assert a['process_counters']==proc['first_process_counters'] and z['process_counters']==proc['last_process_counters']
 for vals in system.values():assert len(vals)==len(trace)
 selected={key:{'min':min(vals),'max':max(vals),'first':vals[0],'last':vals[-1]} for key,vals in system.items() if key.startswith(('/tmp:', '/proc/meminfo:', '/proc/pressure:', '/proc/vmstat:'))}
 return {'interval_requested_seconds':r['native_memory_cpu']['interval_seconds'],'sample_count':len(trace),'first_monotonic':a['timestamp_monotonic'],'last_monotonic':z['timestamp_monotonic'],'successful_sample_span_seconds':z['timestamp_monotonic']-a['timestamp_monotonic'],'sample_gap_ms':numeric_summary(gaps),'capture_elapsed_ms':numeric_summary(captures),'native_max_status_kB_recomputed':maxima,'native_observed_cpu_seconds_recomputed':cpu,'clock_ticks_per_second':hz,'native_fd_read_errors_recomputed':fd_errors,'process_counter_deltas':{k:z['process_counters'][k]-a['process_counters'][k] for k in a['process_counters']},'all_readable_system_fields_present_every_sample':True,'system_field_count':len(fields),'system_field_stats':selected,'task_identity_sets':len(sched_ids),'task_schedstat_read_errors':0,'sched_schedstats_values':dict(collections.Counter(system['/proc/sys/kernel/sched_schedstats'])),'firmware_profile_snapshots':dict(collections.Counter(system['/sys/firmware/acpi/platform_profile'])),'cpu0_EPP_snapshots':dict(collections.Counter(system['/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference'])),'sampling_scope':'Successful process/system snapshots at requested0.5s intervals. Capture cost excludes trace write; gaps include work, requested sleep and scheduling. Counter deltas require stable identities; sched_schedstats=0 limits interpretation. CPU first-to-last excludes process work before first sample. FD read errors do not identify a cause.'}

def audit(name):
 p=BASE/name;r=load(p/'result.json');events=lines(p/'events.jsonl');hops=[x for x in events if x.get('event')=='soak_hop'];chunks=[x for x in events if x.get('event')=='chunk'];starts={(x['generation'],x['request_id']):x for x in events if x.get('event')=='started'}
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
 faults={k:audio[k] for k in ['callback_execution_deadline_misses','missing_buffers','invalid_buffers','truncated_frames','observed_graph_position_discontinuities','observed_position_clock_xrun_recovery_flags']};assert all(v==0 for v in faults.values())
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
 return {'raw_status':r['status'],'duration_seconds':duration,'timing':derived,'bins_300_seconds':bins,'misses':misses,'controls':{'completed':len(check_controls),'checks':check_controls,'hardware_submission_confirmed':False,'same_grid_hash_checks':same_grid,'trimmed_seek_checks':len(trimmed),'trimmed_exact_flags':True,'trimmed_repeated_hashes_consistent':all(len(set(v for k,v in trimmed if k==frame))==1 for frame in {x[0] for x in trimmed}),'parity_recompute_limit':'Waveform samples are not archived; trimmed numerical equality is an instrumented runtime assertion. Repeated whole-grid and trimmed hashes are independently checked.','contiguous_chunks_checked':contiguity,'stale_chunks_in_log':stale,'full_source_loops':complete_counts['full'],'resumed_completions':complete_counts['resumed'],'runtime_ready_count':len(r['runtime_ready']),'backend_trace_identity':list(identities)[0],'source_model_identities_in_chunks':[list(x) for x in identity],'ready_queue_high_water':r['worker_statistics']['ready_high_water'],'ready_queue_limit':r['worker_statistics']['max_ready'],'telemetry_dropped':r['worker_statistics']['telemetry_dropped'],'shutdown_ms':r['worker_shutdown_ms']},'audio':{'streaming_interval_seconds':asp-ast,'overlap_workload_seconds':overlap,'began_before_workload_seconds':first-ast,'ended_after_workload_seconds':asp-last,'callbacks':audio['callbacks'],'frames':audio['frames_rendered'],'faults':faults,'callback_elapsed_us':audio['callback_elapsed_us'],'quantile_scope':'Probe-reported histogram upper bounds; raw histogram counts are not archived.'},'UI':{'frames_swapped':ui['frames_swapped'],'minute_sum':sum(minute),'minute_bins':minute,'complete_bins_intersecting_workload':covered,'zero_complete_bins_intersecting_workload':zeros,'observation_span_seconds':ui_stop-ui_start,'first_to_last_frame_span_seconds':(frame['last_frame_monotonic_ns']-frame['first_frame_monotonic_ns'])/1e9,'maximum_notification_gap_ms':frame['maximum_notification_gap_ms'],'graphics_api':ui['graphics_api'],'continuous_UI_notifications_supported':not zeros and frame['maximum_notification_gap_ms']<=30000,'scope':frame['scope']},'frozen_pins':frozen,'trace_checks':detailed_trace,'native_trace':{'samples':len(trace),'bytes':(p/'native-trace.jsonl').stat().st_size,'trace_errors':r['native_memory_cpu']['trace']['errors'],'sample_errors':r['native_memory_cpu']['sample_errors'],'system_read_errors':r['native_memory_cpu']['system_sampling']['read_errors'],'native_max_status_kB':r['native_memory_cpu']['processes'][0]['max_process_status_kB'],'supervisor_max_status_kB':r['supervisor_memory_cpu']['processes'][0]['max_process_status_kB']},'fixture_deleted':not Path(r['fixture']['path']).exists(),'interpretation':['Global observed p99 alone does not show absence of rare target misses.','Process-alive polling is not proof of continuous UI rendering.','FrameSwapped notifications do not measure physical presentation latency.']}


name='continuous-hour';p=BASE/name;r=load(p/'result.json');events=lines(p/'events.jsonl');facts=audit(name)
controls=r['controls'];keys=['started_to_cancel_ms','cancel_ack_ms','resume_ack_ms','residual_finish_ms','ready_after_resume_ms','ready_after_cancel_ms','delivered_after_cancel_ms']
facts['controls']['ranges_ms']={k:span_numbers([c[k] for c in controls]) for k in keys}
assert all(c['started_to_cancel_ms']>=150 for c in controls)
facts['controls']['all_started_to_cancel_at_least_150ms']=True
facts['controls']['held_stale_rejection_scope']='All11 runtime assertions reject the held prior-generation chunk; independent event-order/frame coverage check finds no stale-generation chunks. No saved sample arrays or direct GPU-submission timestamp.'
facts['event_counts']=dict(collections.Counter(e['event'] for e in events));assert facts['event_counts']['ui_exposure_changed']==2 and 'ui_gate_failure' not in facts['event_counts']
facts['controls']['backend_identity']=r['runtime_ready'][0]['runtime'];assert facts['controls']['source_model_identities_in_chunks']==[[r['fixture']['sha256'],r['pins']['model']['sha256']]]
assert facts['event_counts']['started']==facts['timing']['all_delivered']['chunks']+11+11+1
facts['controls']['started_accounting']={'delivered':1521,'cancelled_and_discarded':11,'seek_prerolls':11,'active_at_shutdown':1,'total':1544}
assert sum(e['preroll'] for e in events if e['event']=='started')==11
assert r['stale_chunks_delivered']==0 and r['worker_statistics']['backend_start_count']==1
assert r['worker_statistics']['discarded_count']==r['worker_statistics']['cancel_count']==11
ui=load(p/'ui.json');progress=r['ui_coverage']['progress'];changed=[e for e in events if e['event']=='ui_exposure_changed'];down,up=changed
assert down['exposed'] is False and up['exposed'] is True
span=up['progress']['timestamp_monotonic_ns']/1e9-down['progress']['timestamp_monotonic_ns']/1e9
assert span==progress['maximum_observed_unexposed_span_seconds']==up['unexposed_span']['seconds']
assert progress['unexposed_observations']==1 and progress['last_unexposed_span']==up['unexposed_span'] and progress['gate_failure'] is None
assert progress['last_progress']['pid']==down['progress']['pid']==up['progress']['pid']
assert up['progress']['frames_swapped']>down['progress']['frames_swapped']
facts['UI']['coverage_gate']=r['ui_coverage'];facts['UI']['exposure_transition_events']=changed
facts['UI']['exposure_span_recomputed_seconds']=span
facts['UI']['minute_bin_scope']='Complete60-second UI-observation bins overlapping workload; partial final bin excluded. UI notifications are not physical presentation timing.'
assert len(facts['UI']['complete_bins_intersecting_workload'])==60 and not facts['UI']['zero_complete_bins_intersecting_workload']
guard=load(BASE/'continuous-hour-guard.json');assert guard['status']=='ok' and not guard['errors'] and guard['script_unloaded'] and guard['idle_inhibit_released'] and guard['returncode']==guard['child_returncode']==0
assert guard['ui']['pid']==progress['last_progress']['pid'] and guard['applied_monotonic']<r['workload_started_monotonic'] and guard['restored_monotonic']>r['workload_stopped_monotonic']
assert guard['restored']['reason']=='owned-window-closed'
facts['UI']['guard']={k:guard[k] for k in ['status','errors','ui','window_policy','applied_monotonic','restored','restored_monotonic','script_unloaded','idle_inhibit_released','child_returncode','returncode']}
audio=load(p/'audio.json');assert audio['requested']['quantum_frames']==audio['negotiated']['stream_frames_min']==audio['negotiated']['stream_frames_max']==audio['negotiated']['graph_quantum_min']==audio['negotiated']['graph_quantum_max']==256
assert audio['negotiated']['format_valid'] and audio['negotiated']['sample_rate']==48000 and audio['negotiated']['channels']==2
assert audio['status']=='ok' and audio['signal']=='silence'
facts['audio']['negotiated']=audio['negotiated'];facts['audio']['signal']='silence';facts['audio']['measurement_scope']=audio['measurement_scope'];facts['audio']['callback_budget_us']=256/48000*1e6
assert audio['callback_elapsed_us']['max']<facts['audio']['callback_budget_us']
for sampler in ['native_memory_cpu','supervisor_memory_cpu']:
 data=r[sampler];assert len(data['processes'])==1 and not data['sample_errors']
 facts.setdefault('resources',{})[sampler]={k:data[k] for k in ['interval_seconds','elapsed_seconds','samples','sample_errors','processes']}
facts['resources']['scope']='Each process separate: exact /proc kB maxima. Native full trace independently reaggregated; supervisor maxima/CPU are sampler-report only because no supervisor full trace was requested. No RSS+DRM or client-peak sums.'
before=load(BASE/'before-continuous-hour.json');after=load(BASE/'after-continuous-hour.json');frozen={}
for path,pin in before['pins'].items():
 file=Path(path);file=file if file.is_absolute() else ROOT/file
 actual=digest(file);assert actual==pin['sha256'] and file.stat().st_size==pin['bytes']
 frozen[path]={'bytes':pin['bytes'],'before_sha256':pin['sha256'],'after_sha256':actual,'matches':True}
assert all(x['matches_current'] for x in facts['frozen_pins'].values())
facts['before_after_pins']=frozen;facts['source_immutability_scope']='Before-run snapshot, in-run result source pins, and post-run current files match; snapshots cannot prove no byte-for-byte-reverted intermediate mutation.'
monitor=[x for x in lines(BASE/'monitor.jsonl') if x['run']==name];running=[x for x in monitor if x['progress']['status']=='running'];final=[x for x in monitor if x['progress']['status']=='ok']
assert len(running)==70 and all(x['profile']=='performance' and x['holds'] for x in running)
assert before['profile']==after['profile']=='power-saver' and before['holds']==after['holds']==''
facts['policy']={'before_profile':before['profile'],'before_holds':before['holds'],'running_monitor_rows':len(running),'running_profiles':dict(collections.Counter(x['profile'] for x in running)),'running_holds_nonempty':sum(bool(x['holds']) for x in running),'final_monitor_rows':[{'at_utc':x['observed_at_utc'],'profile':x['profile'],'holds':x['holds']} for x in final],'after_profile':after['profile'],'after_holds':after['holds'],'conditions_at_workload_finish':after['conditions_at_workload_finish'],'conditions_after_hold_release':after['conditions_after_hold_release'],'scope':'All native samples reported firmware/EPPperformance. Before/after daemon snapshots show power-saver with no holds; explicitly post-release sysfs snapshot shows low-power/EPPpower. Workload-finish conditions were captured before hold release. Snapshots do not prove exact continuous daemon history.'}
facts['fixture']=r['fixture'];facts['acceptance']=r['acceptance'];facts['failures']=r['failures'];assert not r['failures']
facts['input_evidence_sha256']={file.name:{'bytes':file.stat().st_size,'sha256':digest(file)} for file in [p/'result.json',p/'events.jsonl',p/'native-trace.jsonl',p/'audio.json',p/'ui.json']}
facts['limitations']=['Observed nearest-rank p99 and finite consecutive samples do not bound future tail latency.','35delivered target misses are all continuous chunks; cancellation replacement ready latency also includes preroll and may exceed a hop service duration.','Raw waveform samples are not archived: trimmed numeric equality is verified by instrumented assertions and repeated hashes, not fresh array comparison.','Audio callback quantiles are probe-reported histogram upper bounds; full raw histogram/callback timeline is not archived.','GUI frameSwapped notifications and sampled exposure/scene metadata do not measure physical presentation or prove every intermediate heartbeat.','Full-trace writes succeeded; optional RAPL energy reads were denied and fdinfo reads had native2/supervisor7192 errors. Do not silently treat unavailable counters as zero.','sched_schedstats was0; do not infer lack of CPU scheduling delay from small runqueue-wait deltas.','The earlier original hour failed continuous UI coverage; the visible45-minute run aborted under instantaneous exposure gate. Neither is this completed hour.']
facts['finding']='Requested3600seconds of saturated persistent-worker workload with independent silent audio and continuous GUI frame notifications completed. The4.095-second p99 target FAILED: continuousp99=9.768656292s,max=11.795206213s,35/1510continuous hops above target. M0 remains open; no M1 advancement.'
facts['bins_300_seconds_by_kind']={}
hops=[e for e in events if e['event']=='soak_hop']
for kind,selection in [('all_delivered',hops),('continuous',[e for e in hops if e['kind']=='continuous'])]:
 facts['bins_300_seconds_by_kind'][kind]=[{'start_seconds':start,'end_seconds':start+300,**summary([e for e in selection if start<=e['ready_at']-r['workload_started_monotonic']<start+300])} for start in range(0,3600,300)]
held=[]
for c in controls:
 cancel_at=r['workload_started_monotonic']+c['at_workload_seconds']
 old=max((e for e in events if e['event']=='chunk' and e['consumed_at']<cancel_at),key=lambda e:e['consumed_at'])
 assert old['generation']<c['generation']
 held.append({'request_id':old['request_id'],'generation':old['generation'],'current_generation_before_cancel':old['generation']==c['superseded_generation'],'age_at_cancel_ms':(cancel_at-old['ready_at'])*1000})
facts['controls']['held_prior_generation_chunks']=held
facts['controls']['held_current_generation_before_cancel_count']=sum(x['current_generation_before_cancel'] for x in held)
facts['controls']['held_already_stale_before_cancel_count']=sum(not x['current_generation_before_cancel'] for x in held)
facts['controls']['held_stale_rejection_scope']='All11 runtime assertions reject a held result after cancel.9 held results were from the current superseded generation;2 were already stale from the prior completed generation. No stale chunk was delivered after control in independently checked event order.'
facts['cross_run_same_fixture_hashes']={}
for earlier in ['performance-60m','visible-hour']:
 previous=load(BASE/earlier/'result.json');assert previous['fixture']['sha256']==r['fixture']['sha256'] and previous['reference_hashes']==r['reference_hashes']
 previous_trim={(e['frame_start'],e['samples_sha256']) for e in lines(BASE/earlier/'events.jsonl') if e['event']=='soak_hop' and e['parity']['kind']=='trimmed_seek'}
 current_trim={(e['frame_start'],e['samples_sha256']) for e in hops if e['parity']['kind']=='trimmed_seek'}
 assert previous_trim==current_trim
 facts['cross_run_same_fixture_hashes'][earlier]={'reference_hash_count':len(previous['reference_hashes']),'all_reference_hashes_identical':True,'trimmed_seek_hashes_identical':True,'scope':'Exact raw output hash equality; earlier run retains its original UI-coverage/aborted qualification.'}
trace=facts['trace_checks'];trace['first_sample_after_workload_start_seconds']=trace['first_monotonic']-r['workload_started_monotonic'];trace['last_sample_before_workload_stop_seconds']=r['workload_stopped_monotonic']-trace['last_monotonic']
assert len(facts['controls']['source_model_identities_in_chunks'])==1
facts['controls']['source_statistics_reused']=r['source_statistics_reused'];assert r['source_statistics_reused']==sum(e['reused'] for e in events if e['event']=='source_ready')
output=Path('/tmp/conflux-continuous-hour-audit.json');output.write_text(json.dumps(facts,indent=2)+'\n')
print(json.dumps({'output':str(output),'timing':facts['timing'],'control_ranges_ms':facts['controls']['ranges_ms'],'audio':facts['audio'],'UI':{k:facts['UI'][k] for k in ['frames_swapped','minute_sum','maximum_notification_gap_ms','exposure_span_recomputed_seconds']},'sample_gap_ms':facts['trace_checks']['sample_gap_ms'],'capture_elapsed_ms':facts['trace_checks']['capture_elapsed_ms'],'resource_status':{k:v['processes'][0]['max_process_status_kB'] for k,v in facts['resources'].items() if isinstance(v,dict)},'policy':{k:v for k,v in facts['policy'].items() if not k.startswith('conditions_')}},indent=2))

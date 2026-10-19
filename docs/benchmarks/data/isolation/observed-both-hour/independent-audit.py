#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Independent saved-evidence audit; no GPU, live policy query or PCM recomputation."""
import argparse,collections,gzip,hashlib,json,math
from pathlib import Path

def load(p):return json.loads(p.read_text())
def records(p):
 if not p.exists():p=p.with_name(p.name+'.gz')
 with (gzip.open if p.suffix=='.gz' else open)(p,'rt') as f:
  for line in f:yield json.loads(line)
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def timing(rows):
 v=sorted(e['hop_service_ms'] for e in rows)
 return {'chunks':len(rows),'frames':sum(e['frames'] for e in rows),'hop_service_percentiles_ms':{str(q):v[math.ceil(q*len(v)/100)-1] for q in [50,95,99]} if v else {},'hop_service_max_ms':max(v,default=None),'hops_above_target':sum(x>4095 for x in v)}
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--run-directory',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args();p=a.run_directory.resolve();r=load(p/'result.json');es=list(records(p/'events.jsonl'));hops=[e for e in es if e['event']=='soak_hop'];cont=[e for e in hops if e['kind']=='continuous'];chunks=[e for e in es if e['event']=='chunk'];counts=collections.Counter(e['event'] for e in es);begin,end=r['workload_started_monotonic'],r['workload_stopped_monotonic'];assert r['status']=='ok' and end-begin==r['workload_seconds'] and end-begin>=3600
 derived={'all_delivered':timing(hops),'continuous':timing(cont),'bins_300_seconds':[]}
 for key in ['all_delivered','continuous']:
  for k,v in derived[key].items():assert v==r['timing'][key][k],(key,k,v,r['timing'][key][k])
 for b in r['timing']['bins_300_seconds']:
  t=timing([e for e in hops if b['start_seconds']<=e['elapsed_seconds']<b['end_seconds']]);assert all(t[k]==b[k] for k in t);derived['bins_300_seconds'].append(dict(start_seconds=b['start_seconds'],end_seconds=b['end_seconds'],**t))
 assert len(chunks)==len(hops) and len({(e['generation'],e['request_id']) for e in chunks})==len(chunks)
 assert len({e['source_id'] for e in chunks})==1 and {e['model_id'] for e in chunks}=={r['pins']['model']['sha256']}
 active=None;expected=None;golden=r['reference_hashes'];current_checks=0
 for e in es:
  if e['event']=='control':active=e['generation'];expected=e.get('frame_start')
  if e['event']=='chunk':
   assert e['generation']==active and e['frame_start']==expected;expected+=e['frames'];current_checks+=1
 for e in hops:
  assert e['parity']['exact'] is True
  if e['kind']=='continuous':assert e['samples_sha256']==golden[str(e['window_start_frame'])]
 cs=r['controls'];assert len(cs)==11 and all(c['complete'] and c['held_chunk_rejected'] and c['post_cancel_chunks']==0 and c['parity']['exact'] and c['parity']['numeric_parity'] and c['parity']['max_abs_difference']==0 and c['started_to_cancel_ms']>=150 for c in cs)
 discarded={(e['generation'],e['request_id']) for e in es if e['event']=='discarded'};assert discarded=={(c['superseded_generation'],c['superseded_request_id']) for c in cs}
 ranges={k:{'min':min(c[k] for c in cs),'max':max(c[k] for c in cs)} for k in ['started_to_cancel_ms','cancel_ack_ms','residual_finish_ms','resume_ack_ms','ready_after_resume_ms','ready_after_cancel_ms','delivered_after_cancel_ms']}
 stats=r['worker_statistics'];assert stats['closed'] and stats['backend_start_count']==1 and stats['cancel_count']==stats['discarded_count']==11 and stats['telemetry_dropped']==r['stale_chunks_delivered']==0 and stats['ready_high_water']<=stats['max_ready']==2
 audio=load(p/'audio.json');ui=load(p/'ui.json');assert audio==r['load']['children']['audio']['report'] and ui==r['load']['children']['ui']['report'];faultkeys=['callback_execution_deadline_misses','missing_buffers','invalid_buffers','truncated_frames','observed_graph_position_discontinuities','observed_position_clock_xrun_recovery_flags'];assert all(audio[k]==0 for k in faultkeys);astart=audio['streaming_started_monotonic_ns']/1e9;astop=audio['stopped_monotonic_ns']/1e9;overlap=max(0,min(end,astop)-max(begin,astart));assert overlap==r['workload_seconds'] and audio['callbacks']*256==audio['frames_rendered'];assert audio['negotiated']['stream_frames_min']==audio['negotiated']['stream_frames_max']==audio['negotiated']['graph_quantum_min']==audio['negotiated']['graph_quantum_max']==256
 fo=ui['frame_observation'];bins=fo['frames_per_60_seconds'];assert sum(bins)==ui['frames_swapped'];ustart=fo['started_monotonic_ns']/1e9;ustop=fo['stopped_monotonic_ns']/1e9;assert ustart<=begin and ustop>=end;full=[i for i in range(len(bins)) if ustart+(i+1)*60<=ustop and ustart+i*60<end and ustart+(i+1)*60>begin];assert len(full)==60 and all(bins[i]>0 for i in full);assert full==r['ui_coverage']['complete_minute_bins_intersecting_workload'];assert r['ui_coverage']['progress']['unexposed_observations']==0 and r['ui_coverage']['passed'] and r['audio_coverage']['passed']
 policy=collections.Counter();transitions=[];prev=None;tracecount=0
 for row in records(p/'native-trace.jsonl'):
  sv=row.get('system_values',{});value=(sv.get('/sys/firmware/acpi/platform_profile','MISSING'),sv.get('/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference','MISSING'));policy[value]+=1;tracecount+=1
  if value!=prev:transitions.append({'timestamp_monotonic':row['timestamp_monotonic'],'workload_offset_seconds':row['timestamp_monotonic']-begin,'firmware':value[0],'cpu0_EPP':value[1]});prev=value
 assert tracecount==r['native_memory_cpu']['samples']
 def meta(s):
  q=p/f'{p.name}-{s}.json';return load(q if q.exists() else p.parent/q.name)
 cleanup=meta('cleanup');guard=meta('guard');lifecycle=meta('policy');assert all(v is False for v in cleanup['PID_exists'].values()) and cleanup['fixture_exists'] is False and guard['script_unloaded'] and guard['idle_inhibit_released'] and guard['errors']==[];assert lifecycle['mode']=='observe' and lifecycle['profile_mutation'] is False and lifecycle['restored_by_runner'] is False
 pin=r['pins']['soak_source'];assert pin['sha256']=='52605983fdd2416caf63477bc9314096a0daa4f251e798c92928be19e5fea8b8' and r['repository_head']=='b375522216d2d81a9d1d011017fb25474be0dacf';f=Path(pin['path']);pin_now={'exists':f.exists(),'sha256':sha(f) if f.exists() else None};assert pin_now['sha256']==pin['sha256']
 facts={'scope':'Completed synthetic both-load hour; target failed. Saved evidence verification only; no fresh numerical PCM recomputation or new test. User pause remains active; remaining three conditions not started.','status':r['status'],'workload_seconds':r['workload_seconds'],'timing':derived,'event_counts':dict(counts),'chunk_currentness_checks':current_checks,'controls':{'complete':len(cs),'held_chunk_rejected':sum(c['held_chunk_rejected'] for c in cs),'discarded_events':len(discarded),'parity':{'recorded_exact_and_numeric':True,'recorded_max_abs_difference':0,'scope':'Assertions independently checked against retained success events/results; successful PCM arrays are not retained/recomputed.'},'ranges_ms':ranges,'range_scope':'Ranges computed from retained control measurements; acknowledgement return instants are not separately reconstructible from emitted events. Superseded generation/request pairs independently match all discarded events.','hardware_submission_confirmed':sum(c['hardware_submission_confirmed'] for c in cs)},'worker':stats,'audio':{'sha256':sha(p/'audio.json'),'callbacks':audio['callbacks'],'frames':audio['frames_rendered'],'stream_interval_seconds':[astart,astop],'exact_workload_overlap_seconds':overlap,'faults':{k:audio[k] for k in faultkeys},'callback_elapsed_us':audio['callback_elapsed_us'],'quantum_frames':256,'scope':audio['measurement_scope']},'UI':{'sha256':sha(p/'ui.json'),'frames':ui['frames_swapped'],'complete_bins':full,'bins':bins,'sum':sum(bins),'maximum_notification_gap_ms':fo['maximum_notification_gap_ms'],'progress':r['ui_coverage']['progress'],'scope':fo['scope']},'native_policy':{'samples':tracecount,'phase_counts':[{'firmware':key[0],'cpu0_EPP':key[1],'samples':value} for key,value in policy.items()],'transitions':transitions,'scope':'Observed native snapshots only; natural policy variation allowed, no all-balanced requirement/interpolation/private-latch inference.'},'fixture':r['fixture'],'reference_hashes':r['reference_hashes'],'same_grid_hash_checks':r['same_grid_hash_checks'],'trimmed_seek_checks':r['trimmed_seek_checks'],'pins':r['pins'],'repository_head':r['repository_head'],'current_soak_source_verification':pin_now,'backend':r['runtime_ready'],'acceptance':r['acceptance'],'failures':r['failures'],'cleanup':cleanup,'guard_cleanup':{'script_unloaded':guard['script_unloaded'],'idle_inhibit_released':guard['idle_inhibit_released'],'errors':guard['errors']},'sampler_errors':{'sample':r['native_memory_cpu']['sample_errors'],'trace':r['native_memory_cpu']['trace']['errors']},'source_evidence':{'result_sha256':sha(p/'result.json'),'events_sha256':sha(p/'events.jsonl') if (p/'events.jsonl').exists() else None,'audit_source_sha256':sha(Path(__file__))}}
 a.output.write_text(json.dumps(facts,indent=2)+'\n');print(json.dumps({'output':str(a.output),'timing':{k:v for k,v in derived.items() if k!='bins_300_seconds'},'control_ranges_ms':ranges,'native_policy':facts['native_policy'],'UIframes':ui['frames_swapped'],'audio_callbacks':audio['callbacks']},indent=2))
if __name__=='__main__':main()

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Audit saved interrupted both-load trial; no GPU or live host queries."""
import argparse,collections,gzip,hashlib,json,math
from pathlib import Path


def load(path):return json.loads(path.read_text())
def records(path):
 if not path.exists():path=path.with_name(path.name+'.gz')
 with (gzip.open if path.suffix=='.gz' else open)(path,'rt') as stream:return [json.loads(line) for line in stream]
def digest(path):
 with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def timings(rows):
 values=sorted(row['hop_service_ms'] for row in rows)
 return {'chunks':len(rows),'p50_ms':values[math.ceil(.5*len(values))-1] if values else None,'p95_ms':values[math.ceil(.95*len(values))-1] if values else None,'p99_ms':values[math.ceil(.99*len(values))-1] if values else None,'max_ms':max(values,default=None),'above4095ms':sum(value>4095 for value in values)}


def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--run-directory',type=Path,required=True);parser.add_argument('--reference-directory',type=Path,required=True);parser.add_argument('--output',type=Path,default=Path('/tmp/conflux-both-hour-interruption-audit.json'));args=parser.parse_args()
 path=args.run_directory.resolve();base=path.parent;name=path.name;result=load(path/'result.json');events=records(path/'events.jsonl');trace=records(path/'native-trace.jsonl');hops=[event for event in events if event['event']=='soak_hop'];chunks=[event for event in events if event['event']=='chunk']
 assert result['status']=='interrupted' and result['controls']==[] and len(hops)==len(chunks)
 first,last=result['workload_started_monotonic'],result['workload_stopped_monotonic'];assert last-first==result['workload_seconds']
 timing=timings(hops);assert all(timing[f'p{pct}_ms']==result['timing']['continuous']['hop_service_percentiles_ms'][str(pct)] for pct in [50,95,99]);assert timing['max_ms']==result['timing']['continuous']['hop_service_max_ms']
 reference=load(args.reference_directory/'result.json');golden=reference['reference_hashes'];current=None;expected=None
 for event in events:
  if event['event']=='control':current=event['generation'];expected=event['frame_start']
  if event['event']=='chunk':
   assert event['generation']==current and event['frame_start']==expected and event['samples_sha256']==golden[str(event['window_start_frame'])];expected+=event['frames']
 profiles=collections.Counter(row['system_values'].get('/sys/firmware/acpi/platform_profile','MISSING') for row in trace);epp=collections.Counter(row['system_values'].get('/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference','MISSING') for row in trace)
 transitions=[];previous=None
 for row in trace:
  value=(row['system_values'].get('/sys/firmware/acpi/platform_profile','MISSING'),row['system_values'].get('/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference','MISSING'))
  if value!=previous:transitions.append({'timestamp_monotonic':row['timestamp_monotonic'],'relative_to_workload_start_ms':(row['timestamp_monotonic']-first)*1000,'firmware':value[0],'cpu0_EPP':value[1]});previous=value
 resources=result['native_memory_cpu'];assert len(trace)==resources['samples']==resources['trace']['samples_written'];identity={(row['pid'],row['start_ticks']) for row in trace};assert len(identity)==1
 policy=load(base/f'{name}-policy.json');guard=load(base/f'{name}-guard.json');cleanup=load(base/f'{name}-cleanup.json');launch=load(base/f'{name}-launch.json')
 cleanup_checks={'all_owned_PIDs_absent':all(value is False for value in cleanup['PID_exists'].values()),'fixture_absent':cleanup['fixture_exists'] is False,'native_closed':result['worker_statistics']['closed'] is True,'window_guard_restored':guard['window_guard']=={'requested':True,'status':'restored'},'script_unloaded':guard['script_unloaded'] is True,'idle_inhibitor_released':guard['idle_inhibit_released'] is True,'no_guard_errors':guard['errors']==[],'intervening_profile_preserved':policy['restored_by_runner'] is False and policy['preserved_intervening_profile']==policy['before_restore']['profile']==policy['after']['profile'],'expected_interrupt_returns':policy['benchmark_returncode']==policy['runner_returncode']==guard['returncode']==130,'no_holds_after':policy['after']['holds']==cleanup['holds']==''}
 assert all(cleanup_checks.values())
 probes={}
 for probe in ['audio','ui']:
  raw=path/f'{probe}.json';assert raw.stat().st_size==0 and result['load']['children'][probe]['report']=={} and result['load']['children'][probe]['returncode']==-2
  probes[probe]={'selected':True,'raw_final_report_bytes':0,'final_report_status':'unavailable_after_interrupt','child_returncode':-2,'fault_counts':None,'streaming_interval':None,'coverage':result[probe+'_coverage'],'scope':'Empty interrupted report is preserved; no zero-fault, final-frame or complete-workload coverage claim.'}
 assert all(value is None for value in result['load']['audio_fault_counters'].values())
 facts={'qualification':'INTERRUPTED_POLICY_DEVIATED_PARTIAL; not a balanced trial, complete hour, probe-health pass or M0 qualification','raw_status':result['status'],'workload_seconds':last-first,'workload_monotonic_bounds':{'start':first,'stop':last},'timing_ms':timing,'timing_scope':'Descriptive five-hop timing under observed low-power/EPPpower snapshots. Do not use as balanced-profile p99 comparison.','event_counts':dict(collections.Counter(event['event'] for event in events)),'returned_chunks':len(chunks),'accepted_chunks':len(hops),'unaccepted_chunks':0,'controls':{'started':0,'completed':0,'incomplete':0,'scope':'Interrupted before first300-second scheduled control; no cancellation coverage.'},'accepted_hashes_match_reference':True,'source_model_identities':[list(value) for value in {(event['source_id'],event['model_id']) for event in chunks}],'native_trace_policy':{'samples':len(trace),'firmware_counts':dict(profiles),'EPP_counts':dict(epp),'transitions_in_trace':transitions,'all_samples_balanced':profiles.get('balanced',0)==len(trace),'first_timestamp':trace[0]['timestamp_monotonic'],'last_timestamp':trace[-1]['timestamp_monotonic'],'scope':'Only bounded saved policy fields parsed; first snapshot is already low-power, so exact transition instant is not captured by native trace. Lifecycle/journal show startup transition.'},'probes':probes,'partial_UI_heartbeat':result['ui_coverage']['progress'],'raw_acceptance':result['acceptance'],'raw_failures':result['failures'],'cleanup':{'checks':cleanup_checks,'saved_host_record':cleanup,'guard':guard,'policy':policy,'scope':'Saved post-run/lifecycle observations, no live process/policy query. Intervening user/service profile is preserved, not overwritten with entry balanced state.'},'launch_record_scope':{'has_finished_at_utc':'finished_at_utc' in launch,'note':'Launch record lacks finish after parent interrupt; use explicit saved result/guard/policy finished timestamps, not inferred launch duration.','result_start_UTC':result['started_at_utc'],'result_finish_UTC':result['finished_at_utc'],'guard_finish_UTC':guard['finished_at_utc'],'policy_finish_UTC':policy['finished_at_utc']},'reported_sampler_errors':{'sample':resources['sample_errors'],'trace':resources['trace']['errors'],'optional_system_reads':resources['system_sampling']['read_errors']},'producer_pins':result['pins'],'script_sha256':digest(Path(__file__)),'limits':['Audio/UI JSONs are empty and both children exited fromSIGINT; probe fault/timing counts are unknown, not zero.','All64native samples are low-power/EPPpower; no controlled balanced comparison is supported.','UI heartbeat reports partial activity plus a9.991-second observed unexposed span; absent final report prevents minute/frame-gap coverage validation.','No correctness mismatch among five accepted golden chunks; this finite partial sample does not establish correctness or sustained latency.']}
 args.output.write_text(json.dumps(facts,indent=2)+'\n');print(json.dumps({'output':str(args.output),'timing':timing,'profiles':dict(profiles),'EPP':dict(epp),'cleanup':cleanup_checks},indent=2))


if __name__=='__main__':main()

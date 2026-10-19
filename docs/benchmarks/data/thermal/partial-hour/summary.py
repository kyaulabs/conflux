#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Derive partial visible-hour stall evidence without changing first-hour files."""
import hashlib
import json
import math
from pathlib import Path
from datetime import datetime,timezone

prefix=Path('/tmp/conflux-visible-partial')
r=json.loads(Path(str(prefix)+'-analysis.json').read_text())
root=Path(r['run_directory']); raw=json.loads((root/'result.json').read_text())
first=json.loads(Path('/tmp/conflux-soak-performance-analysis.json').read_text())
assert all(raw['pins'][k]['sha256']==first['source_pins_from_result'][k]['sha256'] for k in ['runtime','model','worker_source','sampler_source'])
accepted={}
with (root/'events.jsonl').open() as f:
 for line in f:
  e=json.loads(line)
  if e.get('event')=='soak_hop': accepted[(e['generation'],e['request_id'])]=e
misses=[h for h in r['selected_hops'] if h['above_target']]
strict=Path(str(prefix)+'-strict-miss-rows.json')
strict.write_text(json.dumps({'scope':r['scope'],'ui_coverage_qualification':r['ui_coverage_qualification'],'misses':misses},indent=2)+'\n')
def value(sy,suffix): return next((v for k,v in sy.items() if k.endswith(suffix)),{})
def render(h): return max((e for e in h['host']['drm']['engines'] if e['field']=='drm-engine-render'),key=lambda e:e.get('delta') or -1)
def key(h): return h['generation'],h['request_id']
def small(h):
 sy=h['host']['system']; a=accepted[key(h)]
 return {'generation':h['generation'],'request_id':h['request_id'],'elapsed_seconds':h['elapsed_seconds'],'window_start_frame':h['window_start_frame'],'frames':h['frames'],'frame_start':h['frame_start'],'parity':a['parity'],'hop_service_ms':h['hop_service_ms'],'graph_ms':h['native_metrics']['graph_ms'],'inference_cpu_ms':h['native_metrics']['inference_cpu_ms'],'samples_sha256':a['samples_sha256'],'actual_GPU_MHz_median':value(sy,'gt_act_freq_mhz').get('median'),'PL1_fraction':value(sy,'throttle_reason_pl1').get('nonzero_fraction'),'render_busy_fraction':render(h).get('engine_busy_fraction_of_interior_span')}
rows=[]
for h in misses:
 a=h['host']; sy=a['system']; k=key(h)
 same=[g for g in r['selected_hops'] if not g['above_target'] and any(f'same_source_window_miss_{k}' in reason for reason in g['selection_reasons'])]
 for g in same:
  if (g['frame_start'],g['frames'])==(h['frame_start'],h['frames']):
   assert accepted[key(g)]['samples_sha256']==accepted[k]['samples_sha256']
  else:
   assert accepted[k]['parity']['kind']=='trimmed_seek' and accepted[k]['parity']['exact'] and accepted[k]['parity']['numeric_parity']
 if h['frame_start']==h['window_start_frame']:
  assert accepted[k]['samples_sha256']==raw['reference_hashes'][str(h['window_start_frame'])]
 else:
  assert accepted[k]['parity']['max_abs_difference']==0.0
 ss=h['strict_interior_host_samples']
 assert all(h['native_request_span']['start']<s['timestamp_monotonic']<h['native_request_span']['end'] for s in ss)
 en=render(h)
 endpoint=[]
 for s in [ss[0],ss[-1]]:
  c=next(c for c in s['drm_clients'] if c.get('drm-client-id')==en['client_id'] and c.get('drm-pdev',c.get('drm-driver'))==en['device'])
  endpoint.append(int(c['drm-engine-render'].split()[0]))
 assert endpoint[1]-endpoint[0]==en['delta']
 row=small(h)
 row.update(native_request_span=h['native_request_span'],native_metrics=h['native_metrics'],
  source_read_ms=h['source_read_ms'],overlap_ms=h['overlap_ms'],queue_delay_ms=h['queue_delay_ms'],
  equal_source_good_comparisons=[small(g) for g in same],interior_samples=a['samples'],interior_span_ms=a['interior_span_ms'],
  leading_unsampled_ms=h['leading_unsampled_ms'],trailing_unsampled_ms=h['trailing_unsampled_ms'],capture_straddles=h['captures_began_at_or_before_request'],
  actual_GPU_MHz=value(sy,'gt_act_freq_mhz'),requested_GPU_MHz=value(sy,'gt_cur_freq_mhz'),
  RC6_delta_ms=value(sy,'rc6_residency_ms').get('delta'),runtime_suspended_delta_ms=value(sy,'runtime_suspended_time').get('delta'),runtime_status=value(sy,'runtime_status'),
  render=en,throttle_reasons={k:v for k,v in sy.items() if 'throttle_reason_' in k},
  TCPU_millidegrees_C=value(sy,'thermal_zone5/temp'),x86_package_millidegrees_C=value(sy,'thermal_zone7/temp'),
  memory_PSI_delta_us={k:value(sy,'memory:'+k+'.total').get('delta') for k in ['some','full']},
  IO_PSI_delta_us={k:value(sy,'io:'+k+'.total').get('delta') for k in ['some','full']},
  vmstat_deltas={k.split(':')[-1]:v.get('delta') for k,v in sy.items() if k.startswith('/proc/vmstat:')},
  native_process_counters=a['process_counters'],native_memory=a['process_status'],memory_available_kB=value(sy,'meminfo:MemAvailable'),
  scratch_available_bytes=value(sy,'/tmp:available_bytes'),capture_elapsed_ms=a['capture_elapsed_ms'],sample_gaps_ms=a['sample_gaps_ms'],
  ui_minute_relation=h['ui_minute_relation'])
 rows.append(row)
for label,hs in [('all_delivered',r['all_joined_hop_timing']),('continuous',[h for h in r['all_joined_hop_timing'] if h['kind']=='continuous'])]:
 vs=sorted(h['hop_service_ms'] for h in hs)
 assert max(vs)==raw['timing'][label]['hop_service_max_ms']
 for p in [50,95,99]: assert abs(vs[math.ceil(len(vs)*p/100)-1]-raw['timing'][label]['hop_service_percentiles_ms'][str(p)])<1e-8
# Blocks here mean consecutive above-target delivered hops, not hardware episodes.
blocks=[]; block=[]
for h in r['all_joined_hop_timing']:
 if h['above_target']: block.append(h)
 elif block: blocks.append(block); block=[]
if block: blocks.append(block)
starts=[b[0]['native_request_span']['start'] for b in blocks]
def span(values): return {'min':min(values),'max':max(values)}
aggregates={
 'misses':len(rows),'median_actual_GPU_100MHz':sum(h['actual_GPU_MHz_median']==100 for h in rows),
 'requested_GPU_1350MHz_all_samples':sum(h['requested_GPU_MHz'].get('min')==h['requested_GPU_MHz'].get('max')==1350 for h in rows),
 'requested_GPU_1350MHz_median':sum(h['requested_GPU_MHz'].get('median')==1350 for h in rows),
 'PL1_nonzero_fraction_range':span([h['PL1_fraction'] for h in rows]),
 'GPU_thermal_flag_nonzero_samples':sum(next(v['nonzero_samples'] for k,v in h['throttle_reasons'].items() if k.endswith('throttle_reason_thermal')) for h in rows),
 'render_busy_fraction_range':span([h['render_busy_fraction'] for h in rows]),
 'RC6_delta_ms_range':span([h['RC6_delta_ms'] for h in rows]),
 'runtime_suspended_delta_ms_range':span([h['runtime_suspended_delta_ms'] for h in rows]),
 'inference_CPU_ms_range':span([h['inference_cpu_ms'] for h in rows]),
 'graph_rebuild_ms_max':max(h['native_metrics']['graph_rebuild_ms'] for h in rows),
 'native_major_fault_delta_max':max(h['native_process_counters']['stat.majflt']['delta'] for h in rows),
 'native_minor_fault_deltas':{str((h['generation'],h['request_id'])):h['native_process_counters']['stat.minflt']['delta'] for h in rows if h['native_process_counters']['stat.minflt']['delta']},
 'memory_PSI_some_delta_us_range':span([h['memory_PSI_delta_us']['some'] for h in rows]),
 'IO_PSI_some_delta_us_range':span([h['IO_PSI_delta_us']['some'] for h in rows]),
 'vmstat_reclaim_swap_deltas':{k:{'max_per_miss':max(h['vmstat_deltas'][k] for h in rows),'nonzero_misses':sum(h['vmstat_deltas'][k]!=0 for h in rows)} for k in ['pgscan_direct','pgscan_kswapd','pgsteal_direct','pgsteal_kswapd','pswpin','pswpout','oom_kill','compact_stall']},
 'same_source_comparisons':sum(len(h['equal_source_good_comparisons']) for h in rows),
 'same_delivered_slice_hash_comparisons':sum((g['frame_start'],g['frames'])==(h['frame_start'],h['frames']) for h in rows for g in h['equal_source_good_comparisons']),
 'trimmed_seek_comparisons_recorded_exact':sum((g['frame_start'],g['frames'])!=(h['frame_start'],h['frames']) for h in rows for g in h['equal_source_good_comparisons']),
 'same_source_good_service_ms_range':span([g['hop_service_ms'] for h in rows for g in h['equal_source_good_comparisons']]),
 'consecutive_above_target_delivery_blocks':len(blocks),
 'block_start_spacing_seconds_range':span([b-a for a,b in zip(starts,starts[1:])]),
 'block_scope':'Consecutive above-target delivered hops are grouped; no hardware start/stop or causal inference.'}
summary={'generated_utc':datetime.now(timezone.utc).isoformat(),'trial':'Partial visible-hour; aborted on a transient visibility gate',
 'raw_status':raw['status'],'failures':raw['failures'],'workload_seconds':raw['workload_seconds'],
 'continuous_UI_scope':'Recorded frame notifications have no empty recorded minute bins and maximum 199.384 ms gap, but this aborted run is not a 60-minute validation. Frame notifications do not prove physical presentation latency.',
 'ui_coverage':r['ui_coverage_qualification'],'timing':raw['timing'],'checks':r['checks'],
 'pins_from_result':raw['pins'],'repository_head':raw.get('repository_head'),
 'same_runtime_model_worker_sampler_pins_as_first_trial':True,'aggregates':aggregates,'misses':rows,
 'conclusion':'Recurrent severe GPU-frequency reductions associated with PL1 remain the dominant stall signature under sustained observed UI frame activity. The GT is mostly awake and device runtime suspension does not explain these intervals. Exact firmware/platform-power versus driver-scheduling causation remains unresolved.',
 'limits':['This is a 2724.353-second partial run and must not be promoted to the requested 60-minute result.','Actual GPU medians are 100 MHz for 24 misses; 3 smaller misses have 300/325/400 MHz medians. Requested-frequency medians are 1350 MHz in all 27 misses; 21 misses have 1350 MHz at every selected sample. Sampled RC6 increments are 0–100 ms, much smaller than request durations; no runtime-suspended time advances.','PL1 flags are not actual watts; they also appear on good requests. RAPL energy access remains denied.','No native major faults or process swap are observed. Some native minor faults and modest system pressure/reclaim/swap deltas exist; they do not establish multi-second memory pressure as the common stall cause.','System pressure and engine accounting are sampled, sequential and incomplete at request edges; they do not reveal exact GPU batch boundaries or other client schedules.','Capture/gap durations and retained errors are in the full analysis. No Mesa GPU trace is configured; recurrence does not require that tracing, but host-observer influence is not excluded.','Runtime/model/math/window/output remain pinned, and 52 identical delivered-slice hashes match; two comparisons involve a 137-frame-trimmed seek with recorded exact numerical parity. No fix or kernel/power setting change is justified as proven by these correlations.']}
Path(str(prefix)+'-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
files=[str(prefix)+x for x in ['-analysis.py','-summary.py','-analysis.json','-strict-miss-rows.json','-summary.json']]
manifest={'schema':1,'generated_utc':datetime.now(timezone.utc).isoformat(),'scope':'Partial visible-hour only; first-hour artifacts untouched.',
 'source_inputs':{k:r[k] for k in ['run_directory','result_sha256','input_events','input_trace','source_pins_from_result']},
 'verification':['Nearest-rank and maximum values match final all/continuous reports.','All 27 strict miss boundaries and render endpoint deltas independently checked.','52 same delivered-slice comparisons match output hashes and references; 2 comparisons use a 137-frame-trimmed seek, whose harness parity is exact with maximum difference 0.'],
 'files':[]}
for name in files:
 p=Path(name)
 with p.open('rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
 manifest['files'].append({'path':name,'bytes':p.stat().st_size,'sha256':digest})
Path(str(prefix)+'-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'aggregates':aggregates,'manifest':str(prefix)+'-manifest.json','files':manifest['files']},indent=2))

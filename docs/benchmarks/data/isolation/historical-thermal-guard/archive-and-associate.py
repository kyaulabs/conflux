#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Archive completed diagnostic evidence only; no live host or GPU queries."""
import collections,gzip,hashlib,json,shutil
from pathlib import Path
ROOT=Path('/home/kyau/projects/kyaulabs/conflux');BASE=ROOT/'benchmarks-local/isolation-2026-10-16';DOC=ROOT/'docs/benchmarks/data/isolation'
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def put(dst,src):
 assert not dst.exists(),dst
 shutil.copyfile(src,dst)
 return {'path':dst.name,'source':str(src),'bytes':dst.stat().st_size,'sha256':sha(dst)}
def manifest(dst,items,scope):
 p=dst/'raw-manifest.json';assert not p.exists();p.write_text(json.dumps({'schema':1,'scope':scope,'files':items,'excluded':['This manifest itself','Models and runtime binaries','Generated PCM/WAV fixtures','Full-local journal dumps','Generated KWin window script','Other live or future run records','Separately scoped GPU-derived additions']},indent=2)+'\n');return sha(p)
name='both-hour-rerun';src=BASE/name;dst=DOC/name;dst.mkdir(exist_ok=False);items=[]
for file in src.iterdir():
 if file.name in ['events.jsonl','native-trace.jsonl']:
  out=dst/(file.name+'.gz')
  with file.open('rb') as fi,out.open('wb') as fo:
   with gzip.GzipFile(filename='',mode='wb',fileobj=fo,compresslevel=9,mtime=0) as gz:shutil.copyfileobj(fi,gz)
  with gzip.open(out,'rb') as gz:check=hashlib.file_digest(gz,'sha256').hexdigest()
  assert check==sha(file)
  items.append({'path':out.name,'source':str(file),'bytes':out.stat().st_size,'sha256':sha(out),'decompressed_bytes':file.stat().st_size,'decompressed_sha256':check,'gzip_mtime':0,'gzip_filename':''})
 elif file.name in ['result.json','progress.json','ui-progress.json','audio.json','audio.stderr','ui.json','ui.stderr','stop']:items.append(put(dst/file.name,file))
for suffix in ['policy.json','guard.json','launch.json','cleanup.json','cooldown.json','cooldown.jsonl','stdout','stderr']:
 f=BASE/f'{name}-{suffix}'
 if suffix in ['stdout','stderr']:f=BASE/f'{name}.{suffix}'
 assert f.exists(),f
 items.append(put(dst/f.name,f))
for n in [f'before-{name}.json',f'before-{name}-thermal.json','thermal-latched-prelaunch-stop.json','thermal-guard-cooldown-checkpoint.json','thermal-guard-provenance.json','thermal-policy-amendment.json']:
 f=BASE/n
 if f.exists():items.append(put(dst/n,f))
pin=json.loads((src/'result.json').read_text())['pins']['soak_source'];producer=Path(pin['path']);assert sha(producer)==pin['sha256'];items.append(put(dst/'stem_soak-executed.py',producer))
for suffix in ['py','json']:items.append(put(dst/f'interruption-audit.{suffix}',Path(f'/tmp/conflux-both-hour-rerun-interruption-audit.{suffix}')))
rmanifest=manifest(dst,items,'Exact raw completed second interrupted both-load attempt, frozen producer source and independent qualified partial audit. No complete-hour/audio-health/UI-health or balanced-policy comparative qualification.')
thermal=DOC/'historical-thermal-guard';thermal.mkdir(exist_ok=False);ti=[];summaries=[]
for run in ['none-hour','none-hour-rerun','audio-hour','both-hour','both-hour-rerun']:
 f=BASE/f'audit-{run}-guard-thermal.json';d=json.loads(f.read_text());ti.append(put(thermal/f.name,f));profiles=collections.Counter();epp=collections.Counter();count=0
 raw=BASE/run/'native-trace.jsonl'
 with raw.open() as stream:
  for line in stream:
   row=json.loads(line);values=row.get('system_values',{});profiles[values.get('/sys/firmware/acpi/platform_profile','MISSING')]+=1;epp[values.get('/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference','MISSING')]+=1;count+=1
 summaries.append({'run':run,'journal_bounds':d['bounds_utc'],'last_pre_range_entry':d['last_pre_range_guard_entry'],'in_range_entries':d['in_range_guard_entries'],'capture_errors':d['errors'],'journal_limit_reached_possible_truncation':d['journal_limit_reached_possible_truncation'],'native_policy':{'samples':count,'firmware_counts':dict(profiles),'cpu0_EPP_counts':dict(epp),'source':str(raw),'source_sha256':sha(raw)},'qualification':'Pre-range entry supplies logged guard context only; it is not a direct latch observation. Profile-forcing wrapper can override policy despite guard state. Native snapshots describe observed firmware/EPP, not uninterrupted policy between reads.'})
for entry in summaries:
 result=json.loads((BASE/entry['run']/'result.json').read_text());begin=result['workload_started_monotonic'];end=result['workload_stopped_monotonic']
 entry['workload_monotonic_bounds']={'start':begin,'stop':end}
 entry['journal_receipt_offsets']=[{'MESSAGE':record.get('MESSAGE'),'receipt_monotonic':int(record['__MONOTONIC_TIMESTAMP'])/1e6,'seconds_from_workload_start':int(record['__MONOTONIC_TIMESTAMP'])/1e6-begin,'seconds_from_workload_stop':int(record['__MONOTONIC_TIMESTAMP'])/1e6-end,'phase':'before_workload' if int(record['__MONOTONIC_TIMESTAMP'])/1e6<begin else 'after_workload' if int(record['__MONOTONIC_TIMESTAMP'])/1e6>end else 'within_workload'} for record in [entry['last_pre_range_entry']]+entry['in_range_entries'] if '__MONOTONIC_TIMESTAMP' in record]
for n in ['audit-historical-guard-captures.json','thermal_capture.py']:ti.append(put(thermal/n,BASE/n))
summary={'schema':1,'scope':'Saved bounded host-escalated read-only thermal-guard journal captures across five completed diagnostic attempts, independently associated with saved native policy snapshots. No controlled comparison or causal inference.','runs':summaries,'qualifications':['Current thermal-guard source and unit hashes are stable capture-time bytes only; they do not prove historical executed source bytes.','A last pre-range warning is logged engagement context, not complete latch reconstruction. No direct private latch access exists.','Journal receipt timestamps and message claims are retained; no assumption is made that a logged restore command succeeded.','Empty in-range journals do not prove no thermal/policy change.','The first both attempt sampled low-power; others sampled balanced despite preceding guard warnings. All five remain diagnostic and prior failed/interrupted statuses unchanged.','All captures completed after GPU workloads stopped; no global settings changed.']}
out=thermal/'historical-association.json';out.write_text(json.dumps(summary,indent=2)+'\n');ti.append({'path':out.name,'source':'Derived from exact archived compact captures plus saved native trace policy fields','bytes':out.stat().st_size,'sha256':sha(out)})
ti.append(put(thermal/'archive-and-associate.py',Path(__file__)))
tmanifest=manifest(thermal,ti,'Exact bounded compact historical thermal-guard capture records plus helper, invocation record, archive/association script and qualified policy-count summary. Original attempt evidence remains unchanged.')
for folder in [dst,thermal]:
 m=json.loads((folder/'raw-manifest.json').read_text())
 for item in m['files']:assert (folder/item['path']).stat().st_size==item['bytes'] and sha(folder/item['path'])==item['sha256']
print(json.dumps({'both_archive':str(dst),'files':len(items),'manifest_sha256':rmanifest,'thermal_archive':str(thermal),'thermal_files':len(ti),'thermal_manifest_sha256':tmanifest,'policy_counts':[{k:v for k,v in s.items() if k in ['run','native_policy']} for s in summaries]},indent=2))

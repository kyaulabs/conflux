"""Verify cache outputs, power/thermal observations and teardown without running hardware."""
from pathlib import Path, PurePosixPath
from datetime import datetime
import hashlib,importlib.util,io,json,tarfile

def verify(base):
 manifest=json.loads((base/'verification.json').read_bytes())
 helper=base.parent/'brid-decoder-frontend-2026-11-06/verify_evidence.py'
 assert hashlib.sha256(helper.read_bytes()).hexdigest()==manifest['archive_loader_sha256']
 spec=importlib.util.spec_from_file_location('fixed_cache_archive_loader',helper)
 common=importlib.util.module_from_spec(spec);spec.loader.exec_module(common)
 files=common.load_bundle(base);read=lambda n:json.loads(files[n])
 c=read('c1106/output-01/result.json');d=read('d1106/output-01/result.json');a=read('c1106/apollo-results/result.json')
 assert c['completed']is False and c['cleanup_complete']is True
 assert c['failed_runtime_reaped']is True and c['failed_native_returncode']==0
 assert c['errors']==[dict(phase='comparison',type='ValueError',error='Expected one Intel Mesa disk-cache stats row at real exit')]
 assert d['completed']is True and d['cleanup_complete']is True and d['original_diagnostic_passed']is False
 assert d['all_expected_cache_statistics_collected']is False and a['completed']is True and a['cleanup_complete']is True
 assert c['arms']['baseline']['requests']==d['arms']['baseline']['requests']
 assert all(r['finished_monotonic']<r['original_operator_deadline'] for r in [c,d,a])
 rows=[];codes=[]
 for host,result in [('laptop',d),('apollo',a)]:
  assert result['timing_qualification']is False and result['audio_started']is False and result['hard_cancellation_exercised']is False
  assert result['cache_reuse_observed']is None
  assert set(result['arms'])=={'baseline','candidate'}
  for arm,data in result['arms'].items():
   assert data['graceful_shutdown']is True and data['native_reaped']is True and data['teardown']['returncode']==0
   codes.append(0);assert len(data['requests'])==2
   assert [r['case_id']for r in data['requests']]==['club_xl:0','club_xl:257985']
   for row in data['requests']:
    assert row['exact_match']is True and row['output_sha256']==row['expected_sha256']
    assert row['native_metrics']['frames']==343980 and row['native_metrics']['stems']==4
    rows.append(dict(host=host,cache=arm,case=row['case_id'],round_trip_ms=row['round_trip_ms'],output_sha256=row['output_sha256'],native_metrics=row['native_metrics']))
  assert result['arms']['baseline']['cache_before']==dict(files=[],bytes=0)
  if host=='laptop':
   candidate=data=result['arms']['candidate']
   identity=read('c1106/created-cache-identity.json');assert identity['entry_count']==identity['same_build_renderer_pointer_entries']==62 and identity['mismatched_entries']==0
   assert read('retained/current-Intel-cache-metadata.json')['inventory']==candidate['cache_after']
   assert candidate['cache_before']==read('d1106/retained-seed-inventory.json')
   for arm in result['arms'].values():
    snap=arm['start_snapshot'];assert snap['profile_epp_read_only']['/sys/firmware/acpi/platform_profile']=='low-power'
    assert snap['caps']['/sys/devices/virtual/powercap/intel-rapl-mmio/intel-rapl-mmio:0/constraint_0_power_limit_uw']==7500000
  else:assert result['arms']['baseline']['cache_after']==result['arms']['candidate']['cache_before']==result['arms']['candidate']['cache_after']
 summary=read('analysis/summary.json');assert summary['model_requests']==rows and summary['exact_outputs']==summary['complete_outputs']==8
 assert summary['native_exit_codes']==codes and summary['service_p99']is None
 thermal={}
 for name in ['c1106/output-01/host-observations.jsonl','d1106/output-01/host-observations.jsonl','c1106/output-01/baseline-cooldown.jsonl','d1106/output-01/retained-cooldown.jsonl']:
  values=[json.loads(line)for line in files[name].splitlines()]
  thermal[name]=dict(samples=len(values),maximum_C=max(max(v['temperatures_millidegrees_C'].values())for v in values)/1000,first_utc=values[0]['at_utc'],last_utc=values[-1]['at_utc'])
 assert summary['thermal']==thermal
 retirement=read('c1106/apollo-results/unit-retired.json')
 assert retirement['retired']is True and datetime.fromisoformat(retirement['at_utc'])>datetime.fromisoformat(summary['original_deadline_utc'])
 for name in ['c1106/unit-retired.json','d1106/unit-retired.json']:assert read(name)['retired']is True
 for row in read('cleanup/laptop-cleanup.json')['removed']:assert row['removed']is True
 assert read('cleanup/apollo-cleanup.json')['removed']is True
 # Validate the unchanged, nested Apollo capture as well as its local collection.
 raw=files['analysis/apollo-evidence.tar.gz'];m=read('analysis/apollo-archive-manifest.json');assert len(raw)==m['archive_bytes']and common.digest(raw)==m['archive_sha256']
 expected={r['path']:r for r in m['members']};nested={}
 with tarfile.open(fileobj=io.BytesIO(raw),mode='r:gz')as t:
  for member in t:
   p=PurePosixPath(member.name);assert member.isfile()and not p.is_absolute()and'..'not in p.parts and member.name in expected and member.name not in nested
   pin=expected[member.name];assert member.size==pin['bytes']<=1024**2
   b=t.extractfile(member).read(member.size+1);assert len(b)==pin['bytes']and common.digest(b)==pin['sha256'];nested[member.name]=b
 assert set(nested)==set(expected)and json.loads(nested['packet/container-results-01/output-01/result.json'])==a
 return dict(verified_members=len(files),verified_Apollo_nested_members=len(nested),exact_outputs=8,native_process_exits=codes,original_laptop_statistics_failure_preserved=True,retained_laptop_round_trip_ms=[r['round_trip_ms']for r in d['arms']['candidate']['requests']],laptop_start_profile='Power Saver',laptop_start_long_term_W=7.5,cache_hits_proven=False,service_p99=None,audio_UI_hard_cancellation_exercised=False,passive_Apollo_unit_retirement_after_window=True,qualified=False,models_GPU_audio_or_network_started=False)

if __name__=='__main__':print(json.dumps(verify(Path(__file__).resolve().parent),indent=2))

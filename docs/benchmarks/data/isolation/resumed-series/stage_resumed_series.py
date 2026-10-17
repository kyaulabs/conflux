#!/usr/bin/env python3
"""Preserve explicit completed continuation artifacts; file-only CPU work."""
import datetime,gzip,hashlib,json,pathlib,shutil
root=pathlib.Path.cwd();base=root/'benchmarks-local/isolation-2026-10-16';out=base/'staged-archive/resumed-series'
assert not out.exists();out.mkdir(parents=True)
state=json.loads((base/'suite-state.json').read_text());guard=json.loads((base/'resume-series-guard.json').read_text());request=json.loads((base/'resume-request.json').read_text())
assert state['phase']=='matrix completed; final report pending' and state['remaining_unstarted']==[] and state['active_run'] is None and state['active_session'] is None
assert state['resume_suite_exit']==state['resume_monitor_exit']==state['common_guard_exit']==0 and state['common_guard_released'] is True
assert guard['status']=='ok' and guard['idle_inhibit_released'] and not guard['errors'] and guard['returncode']==0
files={}
def digest(raw):return hashlib.sha256(raw).hexdigest()
def add(name,target=None,expected=None,compress=False):
    source=base/name;target=target or name;raw=source.read_bytes()
    if expected:assert digest(raw)==expected,(name,'pin mismatch')
    if compress:
        with (out/target).open('wb') as f:
            with gzip.GzipFile(filename='',mode='wb',fileobj=f,mtime=0,compresslevel=6) as g:g.write(raw)
        assert gzip.decompress((out/target).read_bytes())==raw
    else:shutil.copyfile(source,out/target)
    stored=(out/target).read_bytes();entry={'source':str(source.relative_to(root)),'bytes':len(stored),'sha256':digest(stored)}
    if compress:entry.update(decompressed_bytes=len(raw),decompressed_sha256=digest(raw),gzip_mtime=0,gzip_filename='',exact_raw_verification=True)
    files[target]=entry
for name,target in {
'resume-request.json':'resume-request.json',
'pause-after-current-completed.json':'prior-pause-request.json',
'pause-completed.json':'prior-pause-completed.json',
'before-resume-suite-state.json':'before-resume-suite-state.json',
'suite-state.json':'final-suite-state.json',
'resume-preflight.json':'resume-preflight-battery.json',
'resume-ac-preflight.json':'resume-preflight-ac.json',
 'thermal-assessment-user-guidance.json':'thermal-assessment-user-guidance.json',
'resume-suite.stdout':'resume-suite.stdout','resume-suite.stderr':'resume-suite.stderr',
'resume-series-guard.json':'resume-series-guard.json','resume-series-wait.json':'resume-series-wait.json','resume-series-stop':'resume-series-stop',
'resume-audit-verification.json':'resume-audit-verification.json',
 'thermal-snapshot-audit-verification.json':'thermal-snapshot-audit-verification.json',
 'observed-both-hour-thermal-snapshot-audit.json':'baseline-both-thermal-snapshot-audit.json',
 'observed-both-hour-resume-audit-check.json':'baseline-both-resume-audit.json',
 'archived-observed-both-hour-resume-audit-check.json':'baseline-both-archive-replay.json',
'none-smoke-resume-audit-check.json':'none-smoke-resume-audit-check.json','audio-smoke-resume-audit-check.json':'audio-smoke-resume-audit-check.json','both-smoke-resume-audit-check.json':'both-smoke-resume-audit-check.json','ui-smoke-resume-audit-check.json':'ui-smoke-resume-audit-check.json',
'observed-both-ui-none-reference-comparison.json':'reference-comparison-both-ui-none.json'
}.items():add(name,target)
for name in ['resume_suite.py','resume_series_wait.py','cooldown.py','observed_launch.py','observed-run.py','normal_ready.py','check_run.py']:
    add(name,expected=request['helper_pins'][name])
for name in ['resume_monitor_loop.py','suite_monitor.py','monitor.py','progress_only.py','observed-capture.py','kernel_capture.py','thermal_capture.py','resume-audit.py','thermal-snapshot-audit.py','stage_completed_raw.py','stage_completed_derived.py']:
    add(name)
add('resume-monitor.jsonl','resume-monitor.jsonl.gz',compress=True)
monitor=[json.loads(line) for line in (base/'resume-monitor.jsonl').read_text().splitlines()]
assert monitor and all('at_utc' in row for row in monitor)
provenance={'schema':1,'scope':'Completed resumed UI-only, worker-only and audio-only continuation orchestration and saved CPU audits only. No host commands, GPU, build/test workload or new measurement ran while packaging.','repository_head_from_preflight':'aec8501bf0db4487d3a851b5106372574b191942','prior_reference_run':'observed-both-hour','baseline_preservation':'Prior committed observed-both-hour and original pause archives were not changed. Prior pause copies are named prior-pause-request.json and prior-pause-completed.json in this separate continuation directory.','suite_state_note':'final-suite-state.json snapshots completed measurement/cleanup before report generation; older historical fields are preserved as recorded.','negative_preflight_note':'resume-preflight-battery.json recorded AC offline before the user connected power; resume-preflight-ac.json is the later AC-online snapshot.','source_note':'Seven orchestration helpers were checked against the recorded resume-request helper pins. Other source copies are exact current-byte snapshots with SHA256 in manifest; they do not independently prove historical execution byte identity unless separately pinned in run launch/result metadata. Relative source paths are unchanged, requiring original workspace layout for orchestration replay.','selected_monitor':'Only resume-monitor.jsonl was copied, deterministic gzip with exact raw hash/size verification; combined old monitor and full-local kernel journals excluded.','monitor_records':len(monitor),'user_thermal_guidance':'Brief hot spikes are acceptable; no new sustained-duration threshold was invented. Existing host policy/thermal guard/readiness remained unchanged.','excluded':'No binaries, model, WAV/F32 audio, media, private full journals or unrelated repository files.'}
(out/'source-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
raw=(out/'source-provenance.json').read_bytes();files['source-provenance.json']={'source':'generated by stage_resumed_series.py from completed saved records','bytes':len(raw),'sha256':digest(raw)}
manifest={'schema':1,'scope':'All explicitly owned resumed-series continuation files except manifest.json itself; immutable prior archives outside this directory are excluded.','created_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'files':files,'archive_helper_sha256':digest(pathlib.Path(__file__).read_bytes())}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
for name,entry in files.items():
    raw=(out/name).read_bytes();assert len(raw)==entry['bytes'] and digest(raw)==entry['sha256']
    if name.endswith('.gz'):
        value=gzip.decompress(raw);assert len(value)==entry['decompressed_bytes'] and digest(value)==entry['decompressed_sha256']
print(json.dumps({'path':str(out),'files':len(files),'stored_bytes':sum(x['bytes'] for x in files.values()),'manifest_sha256':digest((out/'manifest.json').read_bytes()),'all_hashes_and_gzip_roundtrips_verified':True,'monitor_records':len(monitor)},indent=2))

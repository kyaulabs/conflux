import json,runpy,subprocess,sys,uuid
from pathlib import Path
from unittest.mock import patch
b=Path('/home/kyau/projects/kyaulabs/conflux/benchmarks-local/isolation-2026-10-16')
checks=[]
for failure_stage in ('normal_readiness','source_capture'):
 name='cpu-admission-'+uuid.uuid4().hex
 path=b/(name+'-admission.json')
 calls=[]
 def fake_run(command,**options):
  calls.append((command,options))
  stage='normal_readiness' if 'normal_ready.py' in command[2] else 'source_capture'
  assert options['check'] and options['timeout']==(620 if stage=='normal_readiness' else 30)
  if stage==failure_stage:raise subprocess.CalledProcessError(1,command)
 def forbidden(*a,**kw):raise AssertionError('No GPU/guard process may start after rejected admission')
 try:
  with patch.object(sys,'argv',['observed_launch.py',name,'none']),patch.object(subprocess,'run',fake_run),patch.object(subprocess,'Popen',forbidden):
   try:runpy.run_path(str(b/'observed_launch.py'),run_name='__main__')
   except subprocess.CalledProcessError:pass
   else:raise AssertionError('Original admission error lost')
  r=json.loads(path.read_text());assert r['stage']==failure_stage and r['status']=='error' and not r['profile_mutation']
  assert not (b/(name+'-launch.json')).exists() and not (b/name).exists()
  checks.append({'case':failure_stage,'passed':True,'calls':len(calls),'GPU_or_guard_starts':0})
 finally:path.unlink(missing_ok=True)
result={'cases':checks,'temporary_records_removed':True};Path('/tmp/conflux-observed-admission-check.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))

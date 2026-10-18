#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Finite observed-policy CPU/Vulkan music comparison; no setting changes."""
from datetime import datetime,timezone
import hashlib,json,os,pathlib,signal,subprocess,sys,time
BASE=pathlib.Path(__file__).resolve().parent
ROOT=BASE.parents[1]
START=time.monotonic()
MAXIMUM_SECONDS=5000
RUNTIME=pathlib.Path('/tmp/conflux-stem-m0/conflux-stem-runtime-blocking')
MODEL=pathlib.Path('/tmp/conflux-stem-m0/models/htdemucs-f16.gguf')
EXPECTED={str(RUNTIME):'8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6',str(MODEL):'f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6','tools/stems/worker.py':'308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8','tools/benchmarks/worker_support.py':'8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b','tools/benchmarks/stem_worker.py':'87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7'}
state_path=BASE/'sequence.json'
if state_path.exists(): raise FileExistsError('Use a fresh sequence')
state={'status':'running','started_at_utc':datetime.now(timezone.utc).isoformat(),'maximum_seconds':MAXIMUM_SECONDS,'completed_backends':[],'profile_mutation':False,'scope':'Bounded sequential music quality preparation, no UI/audio playback load and no sustained latency qualification.'}
child=None

def save(**fields):
 state.update(fields,elapsed_seconds=time.monotonic()-START,updated_at_utc=datetime.now(timezone.utc).isoformat())
 state_path.write_text(json.dumps(state,indent=2)+'\n')
 print(json.dumps(fields),flush=True)

def digest(path):
 with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def host():
 ac=pathlib.Path('/sys/class/power_supply/AC/online').read_text().strip()
 lid=subprocess.check_output(['busctl','--system','get-property','org.freedesktop.login1','/org/freedesktop/login1','org.freedesktop.login1.Manager','LidClosed'],text=True,timeout=5).strip()
 if ac!='1' or lid!='b false':raise RuntimeError('AC/open-lid conditions absent')
 return {'AC':ac,'lid':lid,'at_utc':datetime.now(timezone.utc).isoformat()}

def command(args,maximum):
 global child
 remaining=START+MAXIMUM_SECONDS-30-time.monotonic()
 if remaining<=0:raise TimeoutError('Finite sequence deadline reached')
 child=subprocess.Popen([sys.executable,'-B',*map(str,args)],cwd=ROOT,start_new_session=True)
 code=child.wait(timeout=min(maximum,remaining))
 if code:raise RuntimeError('Child returned '+str(code))
 child=None

def verify_producers():
 if subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()!='fd4aee1bba0c529277b20ae15ffc4484ca8d7dc7':raise RuntimeError('Source HEAD changed')
 actual={name:digest(pathlib.Path(name) if pathlib.Path(name).is_absolute() else ROOT/name) for name in EXPECTED}
 if actual!=EXPECTED:raise RuntimeError('Frozen runtime/model/worker/sampler changed')
 return actual

def stop(signum,frame):
 if child is not None:
  try:os.killpg(child.pid,signal.SIGINT)
  except ProcessLookupError:pass
 raise KeyboardInterrupt

for signum in (signal.SIGINT,signal.SIGTERM):signal.signal(signum,stop)
try:
 actual=verify_producers()
 manifest=BASE/'manifest.json'
 manifest_sha=digest(manifest)
 driver_sha=digest(BASE/'quality_run.py')
 save(producer_pins=actual,manifest_sha256=manifest_sha,driver_sha256=driver_sha)
 for backend in ('cpu','vulkan'):
  save(stage='normal readiness '+backend,host=host())
  command([BASE/'normal_ready.py',backend],620)
  if digest(manifest)!=manifest_sha or digest(BASE/'quality_run.py')!=driver_sha:raise RuntimeError('Experiment inputs changed')
  save(stage='workload '+backend,host=host(),admitted_producer_pins=verify_producers())
  command([BASE/'quality_run.py','--manifest',manifest,'--runtime',RUNTIME,'--model',MODEL,'--backend',backend,'--threads','4','--request-timeout','120','--output',BASE/backend],1820)
  result=json.loads((BASE/backend/'result.json').read_text())
  verify_producers()
  for key,name in [('runtime',str(RUNTIME)),('model',str(MODEL)),('worker_source','tools/stems/worker.py'),('sampler_source','tools/benchmarks/worker_support.py'),('shared_benchmark_source','tools/benchmarks/stem_worker.py')]:
   if result['pins'][key]['sha256']!=EXPECTED[name]:raise RuntimeError('Captured run pin differs: '+key)
  if result['pins']['manifest']['sha256']!=manifest_sha or result['pins']['driver']['sha256']!=driver_sha:raise RuntimeError('Captured experiment pin differs')
  if result['status']!='ok' or result['failures'] or not result['worker_statistics']['closed'] or result['worker_statistics']['backend_pid'] is not None:raise RuntimeError('Unsuccessful or unclosed worker')
  pids=[int(row['runtime']['pid']) for row in result['runtime_ready']]
  if any(pathlib.Path('/proc',str(pid)).exists() for pid in pids):raise RuntimeError('Owned backend PID remains')
  state['completed_backends'].append(backend)
  save(stage='completed '+backend)
 save(status='completed',stage='report and listening pending')
except BaseException as error:
 save(status='stopped',error=type(error).__name__+': '+str(error))
 raise
finally:
 for signum in (signal.SIGINT,signal.SIGTERM):signal.signal(signum,signal.SIG_IGN)
 if child is not None:
  try:os.killpg(child.pid,signal.SIGINT)
  except ProcessLookupError:pass
  try:child.wait(timeout=20)
  except subprocess.TimeoutExpired:pass
  try:os.killpg(child.pid,signal.SIGKILL)
  except ProcessLookupError:pass
  child.wait(timeout=5)

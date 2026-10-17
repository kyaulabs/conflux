from contextlib import redirect_stdout
import ast,hashlib,importlib.util,io,json,runpy,signal,subprocess,sys,tempfile
from pathlib import Path
from unittest.mock import patch

BASE=Path('/home/kyau/projects/kyaulabs/conflux/benchmarks-local/isolation-2026-10-16')
SOURCE=BASE/'observed-run.py';CHECKER=BASE/'check_run.py'
ast.parse(SOURCE.read_text());ast.parse(CHECKER.read_text())
spec=importlib.util.spec_from_file_location('observed_runner_test',SOURCE)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
original_read=Path.read_text
all_calls=[]
class Fake:
    def __init__(self,name):
        self.name=name;self.profile='power-saver' if name=='not_ready' else 'balanced'
        self.returncode=None;self.pid=424242;self.waits=[];self.kills=[];self.gets=0
    def output(self,command,*,text,timeout):
        assert text is True and timeout==2
        all_calls.append(command)
        assert command in [['powerprofilesctl','get'],['powerprofilesctl','list-holds']]
        if command[-1]=='get':
            self.gets+=1
            if self.name=='cleanup_observation_timeout' and self.gets==3:raise subprocess.TimeoutExpired(command,2)
            return self.profile+'\n'
        if self.name=='held_before' or self.name=='held_during' and self.gets==2:return 'a hold\n'
        return ''
    def popen(self,command,*,start_new_session):
        assert start_new_session and command[:3]==['python3','-B','tools/benchmarks/stem_soak.py']
        return self
    def poll(self):return self.returncode
    def wait(self,timeout=None):
        self.waits.append(timeout)
        if timeout is None:
            if self.name in ['cancel','forcekill']:raise KeyboardInterrupt
            if self.name=='guard_transition':self.profile='power-saver'
            self.returncode=7 if self.name=='child_error' else 0
        elif timeout==5:
            if self.name=='forcekill':raise subprocess.TimeoutExpired('fake-soak',5)
            self.returncode=130
        else:assert timeout==2 and self.returncode==-signal.SIGKILL
        return self.returncode
    def killpg(self,pid,sig):
        assert pid==self.pid;self.kills.append(sig)
        if sig==signal.SIGKILL:self.returncode=-sig

def read(path,*args,**kwargs):
    if str(path)=='/sys/firmware/acpi/platform_profile':return 'mock-firmware\n'
    if str(path)=='/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference':return 'mock-epp\n'
    return original_read(path,*args,**kwargs)
def forbidden(*args,**kwargs):raise AssertionError('profile mutation subprocess.run forbidden')
summary=[]
with tempfile.TemporaryDirectory(prefix='conflux-observed-check-') as directory:
    root=Path(directory)
    for name in ['success','guard_transition','child_error','cancel','forcekill','cleanup_observation_timeout','held_before','held_during','not_ready']:
        fake=Fake(name);record_path=root/(name+'.json')
        with patch.object(module.subprocess,'check_output',fake.output),patch.object(module.subprocess,'Popen',fake.popen),patch.object(module.subprocess,'run',forbidden),patch.object(module.os,'killpg',fake.killpg),patch.object(module.signal,'signal',return_value=signal.SIG_DFL),patch.object(Path,'read_text',read),redirect_stdout(io.StringIO()):
            result=module.main([str(record_path),'--load','none'])
        record=json.loads(record_path.read_text())
        assert record['mode']=='observe' and record['profile_mutation'] is False and record['restored_by_runner'] is False
        assert record['runner_returncode']==result
        assert 'before_restore' in record or 'before_restore_policy_error' in record
        if name in ['success','guard_transition']:assert result==0
        elif name=='child_error':assert result==7
        elif name in ['cancel','forcekill']:
            assert result==130 and record['interrupted']
            assert fake.waits==([None,5,2] if name=='forcekill' else [None,5])
            assert fake.kills==([signal.SIGINT,signal.SIGKILL] if name=='forcekill' else [signal.SIGINT])
        else:assert result==1
        if name=='guard_transition':assert record['before']['profile']=='balanced' and record['after']['profile']=='power-saver'
        if name in ['held_before','held_during','not_ready']:assert not fake.waits
        summary.append({'case':name,'passed':True})
    # Exercise checker policy branches against existing successful report bytes,
    # copied into temporary storage so historical artifacts remain untouched.
    name='none-smoke';run=root/name;run.mkdir()
    (run/'result.json').write_bytes((BASE/name/'result.json').read_bytes())
    (root/(name+'-guard.json')).write_bytes((BASE/(name+'-guard.json')).read_bytes())
    original_policy=json.loads((BASE/(name+'-policy.json')).read_text())
    for kind,valid in [('legacy',True),('observer_transition',True),('observer_not_ready',False),('observer_error',False),('observer_mutation',False)]:
        policy=json.loads(json.dumps(original_policy))
        if kind!='legacy':
            policy.update(mode='observe',profile_mutation=False,restored_by_runner=False)
            policy['before']['profile']='balanced';policy['before']['holds']=''
            policy['during']['profile']='power-saver';policy['before_restore']['profile']='power-saver';policy['after']['profile']='power-saver'
        if kind=='observer_not_ready':policy['before']['profile']='power-saver'
        if kind=='observer_error':policy['after_policy_error']='injected read failure'
        if kind=='observer_mutation':policy['profile_mutation']=True
        (root/(name+'-policy.json')).write_text(json.dumps(policy))
        passed=True
        try:
            with patch.object(sys,'argv',[str(root/'check_run.py'),name]),redirect_stdout(io.StringIO()):
                exec(compile(CHECKER.read_text(),str(root/'check_run.py'),'exec'),{'__file__':str(root/'check_run.py'),'__name__':'__main__'})
        except AssertionError:passed=False
        assert passed==valid,(kind,passed,valid)
        summary.append({'case':'checker_'+kind,'passed':True})
assert not Path(directory).exists()
assert all(call[-1] in ['get','list-holds'] for call in all_calls)
out={'AST_passed':True,'checks':summary,'runner_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
     'checker_sha256':hashlib.sha256(CHECKER.read_bytes()).hexdigest(),'profile_set_calls':0,
     'real_subprocess_or_profile_calls':0,'temporary_artifacts_removed':True,
     'configured_cleanup_wait_budget_seconds':15}
Path('/tmp/conflux-observed-runner-check.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))

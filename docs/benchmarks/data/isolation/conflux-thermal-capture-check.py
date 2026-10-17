import ast
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

source=Path('/home/kyau/projects/kyaulabs/conflux/benchmarks-local/isolation-2026-10-16/thermal_capture.py')
ast.parse(source.read_text())
spec=importlib.util.spec_from_file_location('thermal_capture_cpu_check',source)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
original_read=Path.read_text
checks=[]
with tempfile.TemporaryDirectory(prefix='conflux-thermal-capture-check-') as directory:
    root=Path(directory);home=root/'home';script=home/'.local/bin/thermal-guard';script.parent.mkdir(parents=True);script.write_text('# mock thermal guard\n')
    unit=home/'thermal-guard.service';unit.write_text('[Service]\nExecStart=mock\n')
    for mode in ['filtered_success','journal_timeout']:
        commands=[]
        def run(command,**kwargs):
            assert kwargs=={'capture_output':True,'text':True,'timeout':2,'check':False}
            commands.append(command)
            if command[0]=='systemctl':
                stdout=f'ActiveState=active\nSubState=running\nMainPID=123\nInvocationID=allowed-invocation\nFragmentPath={unit}\nDropInPaths=\n'
            elif command==['powerprofilesctl','get']:stdout='power-saver\n'
            elif command==['powerprofilesctl','list-holds']:stdout=''
            else:
                assert command[:2]==['journalctl','--user']
                if mode=='journal_timeout' and '--lines=512' in command:
                    raise subprocess.TimeoutExpired(command,2)
                row={'MESSAGE':'recovered: balanced profile restored=1','PRIORITY':'6',
                     '__MONOTONIC_TIMESTAMP':'1234','__REALTIME_TIMESTAMP':'1792197300000000',
                     '_PID':'123','_SYSTEMD_INVOCATION_ID':'allowed-invocation',
                     '_MACHINE_ID':'FORBIDDEN-MACHINE-ID','_BOOT_ID':'FORBIDDEN-BOOT-ID','_EXE':'FORBIDDEN-EXE'}
                stdout=json.dumps(row)+'\n'
            return SimpleNamespace(returncode=0,stdout=stdout,stderr='')
        def read(path,*args,**kwargs):
            if str(path)=='/sys/firmware/acpi/platform_profile':return 'low-power\n'
            if str(path)=='/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference':return 'power\n'
            return original_read(path,*args,**kwargs)
        argv=['thermal_capture.py',mode,'--since','2026-10-17 00:35:00 UTC','--until','2026-10-17T00:36:00Z']
        with patch.object(module,'__file__',str(root/'thermal_capture.py')),patch.object(sys,'argv',argv),patch.object(Path,'home',return_value=home),patch.object(Path,'read_text',read),patch.object(module.subprocess,'run',run),redirect_stdout(io.StringIO()):
            result=module.main()
        report=json.loads((root/(mode+'-thermal.json')).read_text());text=json.dumps(report)
        assert not any(x in text for x in ['FORBIDDEN-MACHINE-ID','FORBIDDEN-BOOT-ID','FORBIDDEN-EXE'])
        assert report['current_policy']['profile']=='power-saver'
        assert report['source_snapshot']['sha256']==hashlib.sha256(script.read_bytes()).hexdigest()
        assert report['source_snapshot']['stable_during_read']
        assert report['unit_snapshots'][0]['sha256']==hashlib.sha256(unit.read_bytes()).hexdigest()
        assert report['service_identity']['MainPID']=='123'
        assert set(report['last_pre_range_guard_entry'])<=set(module.JOURNAL_FIELDS)
        assert any('--until=2026-10-17 00:34:59.999999 UTC' in c for c in commands)
        assert all(c[0] in ['systemctl','powerprofilesctl','journalctl'] for c in commands)
        assert not any('set' in c or 'start' in c or 'stop' in c for c in commands)
        if mode=='filtered_success':assert result==0 and not report['errors'] and len(report['in_range_guard_entries'])==1
        else:assert result==1 and report['in_range_guard_entries'] is None and report['errors']
        checks.append({'case':mode,'passed':True,'mocked_commands':len(commands)})
    for bad in ['2026-10-17 00:35:00','2026-10-17T00:35:00-07:00']:
        try:module.utc(bad)
        except ValueError:pass
        else:raise AssertionError('accepted non-UTC timestamp')
report={'AST_passed':True,'checks':checks,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'real_host_commands':0,'temporary_outputs_removed':not Path(directory).exists()}
Path('/tmp/conflux-thermal-capture-check.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))

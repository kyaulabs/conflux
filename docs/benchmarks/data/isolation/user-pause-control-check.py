import ast
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

SOURCE=Path('/home/kyau/projects/kyaulabs/conflux/benchmarks-local/isolation-2026-10-16/cooldown.py')
text=SOURCE.read_text();ast.parse(text)
class ReachedNormalCooldown(Exception):pass
cases=[]
with tempfile.TemporaryDirectory(prefix='conflux-pause-control-check-') as directory:
    parent=Path(directory)
    for name in ['current_not_blocked','future_safe','future_missing_cleanup','future_live_PID','future_empty_PID_map','malformed_marker']:
        root=parent/name;root.mkdir();current='observed-both-hour';label=current if name=='current_not_blocked' else 'observed-ui-hour'
        marker=[] if name=='malformed_marker' else {'current_run':current,'requested_by':'user'}
        (root/'pause-after-current.json').write_text(json.dumps(marker))
        cleanup={'PID_exists':{'101':False,'102':False,'103':False},'worker_closed':True,
                 'fixture_exists':False,'guard_errors':[],'idle_inhibit_released':True}
        if name=='future_live_PID':cleanup['PID_exists']['102']=True
        if name=='future_empty_PID_map':cleanup['PID_exists']={}
        if name not in ['future_missing_cleanup','malformed_marker']:
            (root/(current+'-cleanup.json')).write_text(json.dumps(cleanup))
        discovery=[]
        def no_real_temperature(path,pattern):
            discovery.append(str(path))
            if name=='current_not_blocked':raise ReachedNormalCooldown
            raise AssertionError('future paused case reached temperature discovery')
        code=None;normal=False
        with patch.object(sys,'argv',[str(root/'cooldown.py'),label]),patch.object(Path,'glob',no_real_temperature),redirect_stdout(io.StringIO()):
            try:exec(compile(text,str(root/'cooldown.py'),'exec'),{'__file__':str(root/'cooldown.py'),'__name__':'__main__'})
            except SystemExit as error:code=error.code
            except ReachedNormalCooldown:normal=True
        if name=='current_not_blocked':
            assert normal and discovery and code is None
            assert not (root/'observed-series-stop').exists()
            assert not (root/(label+'-pause.json')).exists()
        else:
            assert code==75 and not discovery
            record=json.loads((root/(label+'-pause.json')).read_text())
            assert record['status']=='paused' and record['blocked_run']==label
            assert not (root/(label+'-cooldown.json')).exists()
            assert not (root/(label+'-cooldown.jsonl')).exists()
            safe=name=='future_safe'
            assert (root/'observed-series-stop').exists()==safe
            assert (root/'pause-completed.json').exists()==safe
            assert record['cleanup_proven']==safe and record['guard_release_requested']==safe
            assert record['pending_root_cleanup']==(not safe)
            if safe:assert record['errors']==[]
            else:assert record['errors']
        cases.append({'case':name,'passed':True})
assert not Path(directory).exists()
record={'AST_passed':True,'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),'cases':cases,
        'actual_temperature_reads':0,'actual_process_or_GPU_calls':0,'temporary_artifacts_removed':True}
Path('/tmp/conflux-user-pause-control-check.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))

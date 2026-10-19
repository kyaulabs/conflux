from contextlib import nullcontext
import ast,hashlib,importlib.util,json,sys,tempfile,time
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import numpy as np

SOURCE=Path('/home/kyau/projects/kyaulabs/conflux/tools/benchmarks/stem_soak.py')
sys.path.insert(0,str(SOURCE.parent));ast.parse(SOURCE.read_text())
spec=importlib.util.spec_from_file_location('same_grid_soak_check',SOURCE)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
results=[]
def forbidden(*args,**kwargs):raise AssertionError('unexpected numerical work on unchanged/no-reference hash path')
with tempfile.TemporaryDirectory(prefix='conflux-same-grid-capture-') as directory:
    parent=Path(directory)
    for mode in ['large_mismatch','within_tolerance_hash_mismatch','success_hash','unknown_origin']:
        root=parent/mode;root.mkdir()
        reference=np.full((4,module.HOP,2),0.2,dtype=np.float32)
        original=hashlib.sha256(reference.tobytes()).hexdigest()
        actual=reference.copy()
        if mode in ['large_mismatch','unknown_origin']:actual[1,42,0]=np.float32(0.5)
        elif mode=='within_tolerance_hash_mismatch':actual[1,42,0]=np.nextafter(actual[1,42,0],np.float32(1))
        actual_hash=hashlib.sha256(actual.tobytes()).hexdigest()
        origin=3*module.HOP if mode=='unknown_origin' else module.HOP
        soak=module.Soak(SimpleNamespace(load_mode='none'),root)
        soak.expected=origin;soak.started=99.0;soak.hashes[origin]=original
        if mode!='unknown_origin':soak.references[origin]=reference
        event={'generation':1,'request_id':2,'source_id':'fixture','model_id':'model',
               'frame_start':origin,'window_start_frame':origin,'frames':module.HOP,
               'samples':actual,'ready_queue_depth':1,'ready_at':100.0,'request_ms':1.0,
               'source_read_ms':0.1,'overlap_ms':0.1,'hop_service_ms':1.2,'control_latency_ms':1.3,
               'queue_delay_ms':0.1,'metrics':{'graph_ms':0.8}}
        error=None
        with (root/'events.jsonl').open('w') as log:
            soak.log=log
            context=patch.object(module.np,'allclose',forbidden) if mode in ['success_hash','unknown_origin'] else nullcontext()
            try:
                with context:soak.accept(event)
            except RuntimeError as caught:error=str(caught)
        assert hashlib.sha256(reference.tobytes()).hexdigest()==original
        assert soak.hashes[origin]==original
        if mode=='success_hash':
            assert error is None and soak.parity_checks==1 and len(soak.hops)==1
            assert soak.parity_failure is None and not list(root.glob('*.f32'))
        else:
            assert error=='repeated same-grid chunk hash changed'
            assert soak.parity_checks==0 and not soak.hops
            if mode=='unknown_origin':
                assert soak.parity_failure is None and not list(root.glob('*.f32'))
            else:
                record=soak.parity_failure
                assert record['parity']['kind']=='same_grid_sha256' and record['parity']['exact'] is False
                assert record['parity']['expected_sha256']==original
                assert record['parity']['actual_sha256']==actual_hash!=original
                assert record['parity']['numeric_parity']==(mode=='within_tolerance_hash_mismatch')
                assert not record['capture_errors']
                assert json.loads((root/'events.jsonl').read_text())==record
                loaded={}
                for name,artifact in record['artifacts'].items():
                    raw=Path(artifact['path']).read_bytes()
                    assert len(raw)==artifact['bytes']==record['maximum_artifact_bytes']==4*module.HOP*2*4
                    assert hashlib.sha256(raw).hexdigest()==artifact['sha256']
                    loaded[name]=np.frombuffer(raw,dtype='<f4').reshape(artifact['shape'])
                difference=loaded['actual']-loaded['reference']
                assert float(np.max(np.abs(difference)))==record['parity']['max_abs_difference']
                failed=~np.isclose(loaded['actual'],loaded['reference'],rtol=1e-4,atol=1e-5)
                assert int(np.count_nonzero(failed))==record['failed_elements']==(0 if mode=='within_tolerance_hash_mismatch' else 1)
        results.append({'case':mode,'passed':True,'original_reference_unchanged':True})
assert not Path(directory).exists()
out={'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),'AST_passed':True,'cases':results,
     'native_or_subprocess_calls':0,'temporary_artifacts_removed':True}
Path('/tmp/conflux-soak-same-grid-capture-check.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))

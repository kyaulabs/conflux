"""Re-score the complete rejected backend without models, media or extraction."""
from pathlib import Path
import hashlib
import importlib.util
import io
import json
import numpy as np


def main():
    base=Path(__file__).resolve().parent
    manifest=json.loads((base/'verification.json').read_bytes())
    helper=base.parent/'brid-decoder-frontend-2026-11-06/verify_evidence.py'
    assert hashlib.sha256(helper.read_bytes()).hexdigest()==manifest['archive_loader_sha256']
    spec=importlib.util.spec_from_file_location('fixed_archive_verifier',helper)
    common=importlib.util.module_from_spec(spec);spec.loader.exec_module(common)
    files=common.load_bundle(base)
    read=lambda name:json.loads(files[name])
    protocol=read('legacy/protocol.json');plan=read('legacy/decoder-plan.json')
    assert protocol['fixed_denominator']==93 and protocol['gates']==plan['gates']
    assert [c['id']for c in protocol['cases']]==list(range(1,94))
    helpers={}
    for name,pin in plan['scoring_helpers'].items():
        path='legacy/'+pin['name'];raw=files[path]
        assert len(raw)==pin['bytes']and common.digest(raw)==pin['sha256']
        helpers[name]=common.namespace(files,path)
    for name in ('score','rows'):
        assert common.definition(files['legacy/original_score_brid.py'],name)==common.definition(files['legacy/score_brid.py'],name)
    scorer=common.namespace(files,'legacy/score_brid.py',{'score','rows'},helpers=helpers)
    recipe='explore-this-final0-threshold-diagnostic-v1'
    adapt=common.namespace(files,'analysis/score_candidate.py',{'candidate_row'},RECIPE=recipe)['candidate_row']
    old_native,terminal=scorer['rows'](files['legacy/baseline-native.jsonl'])
    assert terminal['complete']and terminal['passed']and terminal['completed']==terminal['expected']==len(old_native)==93
    expected=read('analysis/score-result.json');baseline=read('legacy/baseline.json')
    assert expected['gates']==protocol['gates']and expected['recipe']==recipe
    worker_sha=common.digest(files['producer/candidate_worker.py'])
    rows=[];old_rows=[]
    for case,old in zip(protocol['cases'],old_native,strict=True):
        old_score=scorer['score'](case,old,old['source']);old_score['decoder_outcome']='Returned';old_rows.append(old_score)
        root=f"collected/cases/{case['id']:04d}"
        native,end=scorer['rows'](files[root+'/native.stdout'])
        assert len(native)==end['expected']==end['completed']==1 and end['complete']and end['passed']
        native=native[0];raw=files[root+'/candidate.stdout'];receipt=read(root+'/case-result.json')
        assert common.digest(raw)==receipt['response_sha256']
        assert receipt['input_pcm_sha256']==native['pcm_sha256']==old['pcm_sha256']
        row=adapt(native,raw);rows.append(scorer['score'](case,row,native['source']))
        meta=read(root+f"/captures/{case['id']:04d}.json")
        assert meta['analysis_worker_sha256']==worker_sha
        raw=files[root+f"/captures/{case['id']:04d}.npz"]
        assert len(raw)==meta['array_bytes']<=65536 and common.digest(raw)==meta['array_sha256']==receipt['raw_logits_sha256']
        with np.load(io.BytesIO(raw),allow_pickle=False)as archive:
            assert archive.files==['beat_activation','downbeat_activation','grid_mask','beat_mask','downbeat_mask','source_mask']
            arrays={name:archive[name]for name in archive.files}
        assert all(value.shape==(meta['logit_frames'],)and np.isfinite(value).all()for value in arrays.values())
        assert all(arrays[name].dtype==np.dtype('<f4')for name in ['beat_activation','downbeat_activation'])
        assert all(arrays[name].dtype==np.bool_ for name in ['grid_mask','beat_mask','downbeat_mask','source_mask'])
        eligible=np.array([i*48000<native['prepared_frames']*50 for i in range(meta['logit_frames'])],dtype=np.bool_)
        assert np.array_equal(eligible,arrays['source_mask'])
        assert not np.any(arrays['downbeat_mask']&~arrays['beat_mask'])
        events=[dict(frame=int(i)*960,downbeat=bool(arrays['downbeat_mask'][i]))for i in np.flatnonzero(arrays['beat_mask']&eligible)]
        assert events==row['beats']
    assert rows==expected['cases']and old_rows==baseline['cases']
    summary=common.aggregate(rows);old_summary=common.aggregate(old_rows)
    assert all(expected[key]==value for key,value in summary.items())
    assert old_summary=={key:value for key,value in baseline.items()if key!='cases'}
    print(json.dumps(dict(verified_members=len(files),fixed_denominator=93,re_scored_candidate_cases=93,
                         re_scored_baseline_cases=93,raw_mask_protocol_cases=93,candidate=summary,
                         baseline=old_summary,models_GPU_audio_or_network_started=False,qualified=False),indent=2))


if __name__=='__main__':main()

"""Re-score retained BRID predictions and descriptive logits without media or models.

Requires NumPy. Reads the adjacent manifest/archive without extracting files.
Archived scorer code is executed only after every member digest is verified.
"""
from pathlib import Path, PurePosixPath
from decimal import Decimal
import argparse
import ast
import hashlib
import io
import json
import tarfile

import numpy as np


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def load_bundle(base):
    manifest = json.loads((base / 'verification.json').read_bytes())
    archive = (base / manifest['archive']['path']).read_bytes()
    assert len(archive) == manifest['archive']['bytes']
    assert digest(archive) == manifest['archive']['sha256']
    expected = {p['path']: p for p in manifest['members']}
    assert len(expected) == len(manifest['members']) <= 1000
    assert sum(p['bytes'] for p in expected.values()) <= 20 * 1024**2
    files = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as bundle:
        for member in bundle:
            path = PurePosixPath(member.name)
            assert member.isfile() and not path.is_absolute() and '..' not in path.parts
            assert member.name in expected and member.name not in files
            pin = expected[member.name]
            assert member.size == pin['bytes']
            raw = bundle.extractfile(member).read(member.size + 1)
            assert len(raw) == pin['bytes'] and digest(raw) == pin['sha256']
            files[member.name] = raw
    assert set(files) == set(expected)
    return files


def namespace(files, name, functions=None, **injected):
    tree = ast.parse(files[name], name)
    if functions is not None:
        tree.body = [n for n in tree.body if isinstance(n, (ast.Import, ast.ImportFrom))
                     or isinstance(n, ast.FunctionDef) and n.name in functions]
    scope = dict(__name__='retained_evidence', __file__=name, **injected)
    exec(compile(tree, name, 'exec'), scope)
    return scope


def definition(raw, name):
    return ast.dump(next(n for n in ast.parse(raw).body
                         if isinstance(n, ast.FunctionDef) and n.name == name))


def aggregate(rows):
    failures = sum(r['operational_status'] == 'Failed' for r in rows)
    micro = None
    if not failures:
        tp = sum(r['downbeat']['matched_beats'] for r in rows)
        fp = sum(r['downbeat']['false_positives'] for r in rows)
        fn = sum(r['downbeat']['false_negatives'] for r in rows)
        micro = dict(matched=tp, extras=fp, misses=fn, F1=2*tp/(2*tp+fp+fn))
    return dict(denominator=93, failures=failures,
                beat_passes=sum(r['beat_numeric_pass'] for r in rows),
                downbeat_passes=sum(r['downbeat_numeric_pass'] for r in rows),
                both_passes=sum(r['both_numeric_pass'] for r in rows),
                downbeat_micro=micro, qualified=False)


def verify(base):
    files = load_bundle(base)
    read = lambda name: json.loads(files[name])
    protocol = read('capture-01/original-protocol.json')
    assert protocol['fixed_denominator'] == 93
    assert [c['id'] for c in protocol['cases']] == list(range(1, 94))
    plan = read('decode-01/decoder-plan.json')
    assert protocol['gates'] == plan['gates']
    helpers = {}
    for name, pin in plan['scoring_helpers'].items():
        path = 'decode-01/' + pin['name']
        assert len(files[path]) == pin['bytes'] and digest(files[path]) == pin['sha256']
        helpers[name] = namespace(files, path)
    for name in ('score', 'rows'):
        assert definition(files['decode-01/original_score_brid.py'], name) == definition(
            files['decode-01/score_brid.py'], name)
    scorer = namespace(files, 'decode-01/score_brid.py', {'score', 'rows'}, helpers=helpers)
    native_rows = scorer['rows']
    score = scorer['score']
    baseline, terminal = native_rows(files['capture-01/original-laptop.jsonl'])
    assert len(baseline) == terminal['completed'] == terminal['expected'] == 93
    assert terminal['complete'] and terminal['passed']
    predictions = [json.loads(line) for line in files[
        'decode-01/apollo-collected/predictions.jsonl'].splitlines()]
    assert [p['id'] for p in predictions] == list(range(1, 94))
    receipt = read('decode-01/apollo-collected/prediction-result.json')
    assert receipt['complete'] and receipt['all_minimal_roundtrips_exact']
    assert receipt['no_reference_annotations_read']
    assert receipt['predictions_sha256'] == digest(files[
        'decode-01/apollo-collected/predictions.jsonl'])
    expected = read('decode-01/score-result.json')
    variants = {name: [] for name in expected['variants']}
    mono = []
    adapt = namespace(files, 'mono-01/score_mono.py', {'candidate_row'},
                      RECIPE='beat-this-final0-upstream-mono-diagnostic-v1')['candidate_row']
    for case, pred, original in zip(protocol['cases'], predictions, baseline, strict=True):
        stem = f"{case['id']:04d}"
        native, end = native_rows(files[f'decode-01/apollo-collected/native/{stem}.stdout.jsonl'])
        assert len(native) == end['expected'] == end['completed'] == 1
        assert end['complete'] and end['passed']
        native = native[0]
        for key in ('beats', 'summary_bpm', 'pcm_sha256', 'prepared_frames'):
            assert native[key] == original[key]
        assert native['pcm_sha256'] == pred['source_pcm_sha256']
        assert set(pred['outcomes']) == set(variants)
        for name, rows in variants.items():
            outcome = pred['outcomes'][name]
            row = None
            if outcome['state'] == 'Returned':
                assert outcome['prepared_frames'] == native['prepared_frames']
                row = dict(native, beats=outcome['beats'], summary_bpm=outcome['summary_bpm'],
                           analysis_status='Estimated' if outcome['summary_bpm'] is not None else 'Unknown')
                if name == 'minimal':
                    assert outcome['roundtrip_exact'] and row['beats'] == native['beats']
            result = score(case, row, native['source'])
            result['decoder_outcome'] = outcome['state']
            if row is None:
                result['decoder_error'] = outcome['error']
            rows.append(result)
        root = f'mono-01/collected/cases/{stem}'
        transport, end = native_rows(files[root + '/native.stdout'])
        assert len(transport) == end['expected'] == end['completed'] == 1
        assert end['complete'] and end['passed']
        raw = files[root + '/candidate.stdout']
        pin = read(root + '/case-result.json')
        assert digest(raw) == pin['response_sha256']
        assert transport[0]['pcm_sha256'] == pin['input_pcm_sha256'] == native['pcm_sha256']
        row = adapt(transport[0], raw)
        mono.append(score(case, row, transport[0]['source']))
    summaries = {}
    for name, rows in variants.items():
        assert rows == expected['variants'][name]['cases']
        summary = aggregate(rows)
        assert summary == {k: v for k, v in expected['variants'][name].items() if k != 'cases'}
        summaries[name] = summary
    mono_expected = read('mono-01/score-result.json')
    assert mono == mono_expected['cases']
    mono_summary = aggregate(mono)
    for key, value in mono_summary.items():
        assert value == mono_expected[key]
    summaries['mono_frontend'] = mono_summary
    audit = namespace(files, 'head-01/audit_heads.py', {'windows', 'stats', 'check_generated'},
                      TOL=Decimal('0.070'), FPS=50)
    audit['check_generated']()
    all_rows = {1: [], 2: []}
    head_cases = []
    for case in protocol['cases']:
        stem = f"{case['id']:04d}"
        meta = read(f'baseline-logits/{stem}.json')
        raw = files[f'baseline-logits/{stem}.npz']
        assert len(raw) == meta['array_bytes'] and digest(raw) == meta['array_sha256']
        assert meta['pcm_sha256'] == baseline[case['id']-1]['pcm_sha256']
        with np.load(io.BytesIO(raw), allow_pickle=False) as arrays:
            assert arrays.files == ['beat', 'downbeat', 'mask']
            beat, down, mask = (arrays[name] for name in arrays.files)
        assert beat.dtype == down.dtype == np.dtype('<f4') and mask.dtype == np.bool_
        assert beat.shape == down.shape == mask.shape == (meta['logit_frames'],)
        assert np.isfinite(beat).all() and np.isfinite(down).all()
        assert np.array_equal(mask, [i*48000 < meta['frames']*50 for i in range(len(mask))])
        local = {1: [], 2: []}
        for text, pos in zip(case['reference_seconds_text'], case['bar_positions'], strict=True):
            indices = audit['windows'](Decimal(text), mask)
            item = dict(down=max(float(down[i]) for i in indices),
                        beat=max(float(beat[i]) for i in indices))
            local[pos].append(item)
            all_rows[pos].append(item)
        phase = {str(pos): audit['stats'](rows) for pos, rows in local.items()}
        head_cases.append(dict(id=case['id'], name=case['name'], phase=phase,
            position2_downbeat_median_higher=phase['2']['median_window_max_downbeat_logit'] >
                                           phase['1']['median_window_max_downbeat_logit']))
    head = read('head-01/head-result.json')
    assert head_cases == head['cases']
    phases = {str(pos): audit['stats'](rows) for pos, rows in all_rows.items()}
    assert phases == head['phase']
    assert sum(c['position2_downbeat_median_higher'] for c in head_cases) == head[
        'position2_downbeat_median_higher_cases']
    return dict(verified_members=len(files), fixed_denominator=93,
                re_scored_predictions=93*4, variants=summaries,
                re_computed_head_windows=phases,
                model_GPU_or_audio_started=False, qualified=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent)
    print(json.dumps(verify(parser.parse_args().bundle), indent=2, allow_nan=False))

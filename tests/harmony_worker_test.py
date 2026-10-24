#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Synthetic source-clock/tonal truth; no model, playback or downloaded music."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import unittest
import numpy as np

spec=importlib.util.spec_from_file_location('conflux_harmony',Path(__file__).resolve().parents[1]/'tools/timing/analyze_harmony.py')
h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)


def cadence(tonic=0,minor=False):
    # Score-defined I–IV–V–I or i–iv–V–i, not analyzer profile weights.
    root=48+tonic
    chords=((0,3,7),(5,8,12),(7,11,14),(0,3,7)) if minor else ((0,4,7),(5,9,12),(7,11,14),(0,4,7))
    time=np.arange(h.RATE,dtype=np.float64)/h.RATE
    envelope=np.ones(h.RATE);envelope[:240]=np.linspace(0,1,240);envelope[-240:]=np.linspace(1,0,240)
    pieces=[]
    for chord in chords:
        wave=np.zeros(h.RATE)
        for offset in chord:
            frequency=440*2**((root+offset-69)/12)
            for harmonic in (1,2,3):wave+=np.sin(2*np.pi*frequency*harmonic*time)/harmonic
        pieces.append(wave*envelope*.025)
    mono=np.concatenate(pieces)
    return np.column_stack((mono,mono*.7)).astype('<f4')


def records(pcm):
    raw=pcm.astype('<f4',copy=False).tobytes()
    wire=h.analyze(io.BytesIO(raw),len(pcm));rows=[json.loads(line) for line in wire.splitlines()]
    assert rows[0]['pcm_sha256']==hashlib.sha256(raw).hexdigest()
    return rows[0],rows[1:-1],rows[-1]


class HarmonyWorkerTest(unittest.TestCase):
    def test_score_defined_major_minor_cadences_and_transposition(self):
        for tonic,minor in ((0,False),(2,False),(9,True),(3,True)):
            with self.subTest(tonic=tonic,minor=minor):
                header,rows,end=records(cadence(tonic,minor));row=rows[0]
                self.assertEqual((header['frames'],header['sample_rate'],header['channels']),(192000,48000,2))
                self.assertTrue(end['complete']);self.assertEqual(row['status'],'Estimated')
                self.assertEqual((row['candidates'][0]['tonic'],row['candidates'][0]['mode']),
                                 (tonic,'Minor' if minor else 'Major'))
                self.assertEqual(row['confidence'],'Unknown')

    def test_changed_key_is_local_and_keeps_exact_source_partition(self):
        _,rows,_=records(np.concatenate((cadence(0),cadence(2))))
        self.assertEqual([(r['first_frame'],r['end_frame']) for r in rows],[(0,192000),(192000,384000)])
        self.assertEqual([r['status'] for r in rows],['Estimated','Estimated'])
        self.assertEqual([r['candidates'][0]['tonic'] for r in rows],[0,2])

    def test_shared_diatonic_content_does_not_assert_unique_relative_mode(self):
        t=np.arange(h.WINDOW,dtype=np.float64)/h.RATE
        # Exactly shared C-major/A-natural-minor pitch collection, no tonic/cadence.
        wave=sum(np.sin(2*np.pi*440*2**((60+n-69)/12)*t) for n in (0,2,4,5,7,9,11))*.015
        _,rows,_=records(np.column_stack((wave,wave)).astype('<f4'))
        self.assertIn(rows[0]['status'],('Ambiguous','Unknown'));self.assertEqual(rows[0]['confidence'],'Unknown')

    def test_silence_DC_noise_and_single_pitch_are_not_forced_keys(self):
        t=np.arange(h.WINDOW)/h.RATE
        for pcm in (np.zeros((h.WINDOW,2),dtype='<f4'),np.full((h.WINDOW,2),.2,dtype='<f4'),
                    np.random.default_rng(1894).normal(0,.05,(h.WINDOW,2)).astype('<f4')):
            _,rows,_=records(pcm);self.assertEqual(rows[0]['status'],'Unknown');self.assertEqual(rows[0]['candidates'],[])
        mono=np.sin(2*np.pi*440*t)*.2
        _,rows,_=records(np.column_stack((mono,mono)).astype('<f4'))
        self.assertIn(rows[0]['status'],('Ambiguous','Unknown'))

    def test_channel_sign_and_gain_do_not_cancel_tonal_evidence(self):
        pcm=cadence();_,baseline,_=records(pcm)
        for changed in (pcm*np.array([1,-1],dtype='<f4'),pcm*.25):
            _,rows,_=records(changed)
            self.assertEqual(rows[0]['status'],baseline[0]['status'])
            self.assertEqual([(x['tonic'],x['mode']) for x in rows[0]['candidates']],
                             [(x['tonic'],x['mode']) for x in baseline[0]['candidates']])
            np.testing.assert_allclose(rows[0]['chroma'],baseline[0]['chroma'],rtol=0,atol=1e-6)

    def test_unknown_short_tail_is_retained_without_padding_or_extrapolation(self):
        _,rows,end=records(np.concatenate((cadence(),np.zeros((13,2),dtype='<f4'))))
        self.assertEqual((rows[1]['first_frame'],rows[1]['end_frame']),(192000,192013))
        self.assertEqual((rows[1]['status'],rows[1]['reason']),('Unknown','short'));self.assertEqual(end['window_count'],2)

    def test_invalid_extent_nonfinite_and_frame_types_fail(self):
        for frames in (True,0,-1,1.5,h.MAX_FRAMES+1):
            with self.assertRaises(ValueError):h.validate_frames(frames)
        for raw,frames in ((b'',1),(b'\0'*7,1),(b'\0'*9,1)):
            with self.assertRaises(ValueError):h.analyze(io.BytesIO(raw),frames)
        for value in (float('nan'),float('inf'),-float('inf')):
            pcm=np.zeros((1,2),dtype='<f4');pcm[0,1]=value
            with self.assertRaises(ValueError):h.analyze(io.BytesIO(pcm.tobytes()),1)

    def test_short_reads_preserve_exact_identity_and_window_bounds(self):
        class Short(io.BytesIO):
            def read(self,count=-1):return super().read(min(count,3))
        raw=b'\0'*80;wire=h.analyze(Short(raw),10);rows=[json.loads(x) for x in wire.splitlines()]
        self.assertEqual(rows[0]['pcm_sha256'],hashlib.sha256(raw).hexdigest())
        self.assertEqual((rows[1]['first_frame'],rows[1]['end_frame'],rows[1]['status']),(0,10,'Unknown'))


if __name__=='__main__':unittest.main()

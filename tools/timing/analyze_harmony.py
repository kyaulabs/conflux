#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Unqualified local original-source chroma/key candidates; owned offline worker.

Krumhansl profile constants below are attributed to the Music Technology Group,
Universitat Pompeu Fabra, Essentia key.cpp (AGPL-3.0-or-later).
Copyright (C) 2006-2021 Music Technology Group - Universitat Pompeu Fabra:
https://github.com/MTG/essentia/blob/master/src/algorithms/tonal/key.cpp
The documented cognitive profiles are a baseline, not calibrated confidence:
https://essentia.upf.edu/reference/std_Key.html
No Essentia code or runtime dependency is bundled. NumPy1.26+ basic FFT APIs are
used; the existing timing environment's2.5.4 and CI's1.26 use the same recipe.
"""
import argparse
import hashlib
import json
import math
import sys

RATE, CHANNELS, MAX_FRAMES = 48000, 2, 33554432
WINDOW, FFT, HOP = 192000, 32768, 16384
MAX_OUTPUT = 128*1024
RECIPE = "local-chroma-krumhansl-v1"
# C-relative semitone order. These are raw profile weights, not probabilities.
MAJOR = (6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88)
MINOR = (6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17)


def validate_frames(frames):
    if type(frames) is not int or not 1<=frames<=MAX_FRAMES:
        raise ValueError("Invalid prepared frame count")
    return frames


def read_exact(stream,count):
    raw=bytearray()
    while len(raw)<count:
        block=stream.read(count-len(raw))
        if not block:raise ValueError("Truncated prepared PCM")
        raw.extend(block)
    return raw


def window_result(raw,first,end):
    import numpy as np
    samples=np.frombuffer(raw,dtype='<f4').reshape(-1,CHANNELS)
    if not np.isfinite(samples).all():raise ValueError("Nonfinite prepared PCM")
    result=dict(first_frame=first,end_frame=end,status="Unknown",reason="short",
                chroma=[0.]*12,candidates=[],confidence="Unknown")
    if len(samples)<FFT:return result
    if not np.any(np.ptp(samples,axis=0)):
        result['reason']='silence';return result
    hann=np.hanning(FFT)
    frequencies=np.fft.rfftfreq(FFT,1/RATE)
    selected=(frequencies>=55)&(frequencies<=3520)
    pitch=np.rint(69+12*np.log2(frequencies[selected]/440)).astype(np.int64)%12
    starts=list(range(0,len(samples)-FFT+1,HOP))
    if starts[-1]!=len(samples)-FFT:starts.append(len(samples)-FFT)
    chroma=np.zeros(12,dtype=np.float64)
    for start in starts:
        block=samples[start:start+FFT].astype(np.float64)
        block-=block.mean(axis=0)
        left=np.abs(np.fft.rfft(block[:,0]*hann))**2
        right=np.abs(np.fft.rfft(block[:,1]*hann))**2
        values=np.bincount(pitch,weights=((left+right)*.5)[selected],minlength=12)
        total=float(values.sum())
        if total>0:chroma+=values/total
    total=float(chroma.sum())
    if not math.isfinite(total) or total<=0:
        result['reason']='silence';return result
    chroma/=total;result['chroma']=[float(x) for x in chroma]
    # Frozen conservative recipe admission; neither this nor profile margins
    # are an empirically calibrated confidence or real-music acceptance gate.
    flatness=float(np.exp(np.log(np.maximum(chroma,1e-300)).mean())/chroma.mean())
    if flatness>=.9:
        result['reason']='non-tonal';return result
    centered=chroma-chroma.mean();norm=float(np.linalg.norm(centered))
    if norm<=0:
        result['reason']='non-tonal';return result
    candidates=[]
    for mode,profile in (('Major',MAJOR),('Minor',MINOR)):
        profile=np.asarray(profile,dtype=np.float64)
        profile-=profile.mean();profile/=np.linalg.norm(profile)
        for tonic in range(12):
            score=float(np.dot(centered,np.roll(profile,tonic))/norm)
            candidates.append(dict(tonic=tonic,mode=mode,correlation=max(-1.,min(1.,score))))
    candidates.sort(key=lambda x:(-x['correlation'],x['tonic'],x['mode']))
    result['candidates']=candidates[:2]
    if int(np.count_nonzero(chroma>=chroma.max()*.1))<3:
        result.update(status='Ambiguous',reason='insufficient-key-context')
    elif candidates[0]['correlation']>=.6 and candidates[0]['correlation']-candidates[1]['correlation']>=.1:
        result.update(status='Estimated',reason='profile-match')
    else:result.update(status='Ambiguous',reason='competing-keys')
    return result


def analyze(stream,frames):
    frames=validate_frames(frames);digest=hashlib.sha256();windows=[]
    for first in range(0,frames,WINDOW):
        end=min(first+WINDOW,frames);raw=read_exact(stream,(end-first)*8);digest.update(raw)
        windows.append(window_result(raw,first,end))
    if stream.read(1):raise ValueError("Extra prepared PCM bytes")
    header=dict(schema=1,recipe=RECIPE,frames=frames,sample_rate=RATE,channels=CHANNELS,
                pcm_sha256=digest.hexdigest(),window_frames=WINDOW,window_count=len(windows))
    terminal=dict(schema=1,complete=True,window_count=len(windows))
    raw=(''.join(json.dumps(row,allow_nan=False,separators=(',',':'))+'\n'
                 for row in [header]+windows+[terminal])).encode()
    if len(raw)>MAX_OUTPUT:raise ValueError("Harmony output exceeds128KiB")
    return raw


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--frames',type=int,required=True)
    args=parser.parse_args();sys.stdout.buffer.write(analyze(sys.stdin.buffer,args.frames))


if __name__=='__main__':
    try:main()
    except Exception as error:
        print(type(error).__name__+': '+str(error)[:1024],file=sys.stderr);raise SystemExit(1)

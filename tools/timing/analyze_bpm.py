#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fixed offline, estimated BPM worker; never run on the GUI/audio callback.

Beat This b95c8ab0c58c2d9fcfd40508ae8dffbc05ac4f5c and its published
final0 weights are MIT licensed, copyright 2024 Institute of Computational
Perception, JKU Linz, Austria. The caller supplies that unmodified source tree
and its LICENSE; no third-party code or weights are bundled here.

The summary is a median of estimated adjacent beat intervals, not calibrated
confidence, a constant-tempo assertion, or permission to synchronize decks.
The parent owns cancellation, process-group cleanup and the wall/output/address-space budget.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import statistics
import struct
import sys
import types

RATE, CHANNELS, FPS = 48000, 2, 50
MAX_FRAMES = 33554432
MAX_OUTPUT_BYTES = 4096
RECIPE = "beat-this-final0-linear-mel-v1"
MODEL_BYTES = 81058141
MODEL_SHA256 = "8c328b45f59d8dd3dff219253ff6a8d6482be57d0133a29140e2febbf8eb8331"
# Exact critical import closure from the retained b95c8ab0 source provenance.
# Execute these verified buffers in dependency order, not unpinned __init__.py.
SOURCE_PINS = {
    "beat_this/utils.py": (4150, "779d6102ea28bdf94c8227157790191f5abbbb4564d0c36ce5ce0859e067cfab"),
    "beat_this/model/roformer.py": (4374, "c83fa0047fe726bbfdb33658f40a0f9e721c5796067bfda560092e5969d50722"),
    "beat_this/model/beat_tracker.py": (12582, "bfa2fd212b4d6bf00b007eb0e606a6b11f1f7fc44c95bd1121ef5d2a934a923f"),
    "beat_this/model/postprocessor.py": (8406, "431d6aeef41a8e9e9e63ee2577e895a5e214e0dbbcd2d505458cdfec02a611ee"),
    "beat_this/preprocessing.py": (1767, "5aaeefa811c2fd1340c8a45233af4c2e814720b65c2124e59e78538fed03b3bc"),
    "beat_this/inference.py": (12781, "ccccff4665399b931ebf181b73d264a6ad64fc1fe8acb4d3b950df825c47ce2d"),
}
# Versions of the existing, separately installed 15-wheel CPU lock.
DEPENDENCIES = {
    "torch": "2.11.0+cpu", "torchaudio": "2.11.0+cpu", "numpy": "2.5.4",
    "einops": "0.8.2", "rotary-embedding-torch": "0.9.1", "soxr": "1.1.0",
    "fsspec": "2026.9.0", "networkx": "3.7", "setuptools": "81.0.0",
    "sympy": "1.14.0", "typing_extensions": "4.16.0", "filelock": "4.2.4",
    "Jinja2": "3.1.6", "MarkupSafe": "3.0.4", "mpmath": "1.3.0",
}


def validate_frames(frames):
    if type(frames) is not int or not 1 <= frames <= MAX_FRAMES:
        raise ValueError("frames must be an integer between 1 and 33554432")
    return frames


def validate_pcm(pcm, frames):
    validate_frames(frames)
    if not isinstance(pcm, (bytes, bytearray)) or len(pcm) != frames * CHANNELS * 4:
        raise ValueError("expected exact stereo 48000 Hz little-endian float32 PCM")
    if any(not math.isfinite(value) for (value,) in struct.iter_unpack("<f", pcm)):
        raise ValueError("nonfinite input PCM")


def read_pcm(stream, frames):
    expected = validate_frames(frames) * CHANNELS * 4
    pcm = bytearray()
    while len(pcm) <= expected:
        block = stream.read(min(1048576, expected + 1 - len(pcm)))
        if not block:
            break
        pcm.extend(block)
    validate_pcm(pcm, frames)
    return pcm


def source_mask(frames, resampled_lengths, logit_frames):
    validate_frames(frames)
    numerator = frames * 22050
    permitted = {numerator // RATE, (numerator + RATE - 1) // RATE}
    if (type(resampled_lengths) not in (list, tuple) or len(resampled_lengths) != 2 or
            any(type(n) is not int or n <= 0 or n not in permitted for n in resampled_lengths) or
            resampled_lengths[0] != resampled_lengths[1] or type(logit_frames) is not int or
            logit_frames != resampled_lengths[0] // 441 + 1):
        raise ValueError("unexpected resampled/centered frontend geometry")
    # Contiguous source prefix BEFORE peak pooling; exclude a centered EOF bin
    # without shifting eligible source timestamps or suppressing its neighbour.
    return tuple(k * RATE < frames * FPS for k in range(logit_frames))


def source_beat_frames(seconds, frames):
    validate_frames(frames)
    if type(seconds) not in (list, tuple) or len(seconds) > frames * FPS // RATE + 1:
        raise ValueError("invalid bounded beat stream")
    result, previous = [], None
    for value in seconds:
        if type(value) not in (int, float):
            raise ValueError("invalid beat timestamp type")
        try:
            valid = math.isfinite(value) and 0 <= value < frames / RATE
        except OverflowError:
            valid = False
        if not valid:
            raise ValueError("nonfinite or out-of-source beat timestamp")
        frame = round(value * RATE)
        if (not 0 <= frame < frames or
                (previous is not None and (value <= previous or frame <= result[-1]))):
            raise ValueError("beat timestamps must have distinct increasing source frames")
        result.append(frame)
        previous = value
    return result


def summarize_beats(seconds, frames):
    beat_frames = source_beat_frames(seconds, frames)
    if len(beat_frames) < 2:
        return None, len(beat_frames)
    bpm = statistics.median(60 * RATE / (b - a) for a, b in zip(beat_frames, beat_frames[1:]))
    return (round(bpm, 1) if 20 <= bpm <= 400 else None), len(beat_frames)


def encode_result(frames, pcm_sha256, bpm, beat_count):
    validate_frames(frames)
    if (type(pcm_sha256) is not str or len(pcm_sha256) != 64 or
            any(c not in "0123456789abcdef" for c in pcm_sha256) or
            type(beat_count) is not int or not 0 <= beat_count <= frames * FPS // RATE + 1 or
            (bpm is not None and (type(bpm) not in (int, float) or not 20 <= bpm <= 400 or
                                 not math.isfinite(bpm) or beat_count < 2 or round(bpm, 1) != bpm))):
        raise ValueError("invalid BPM summary output")
    value = {"schema": 1, "recipe": RECIPE, "frames": frames,
             "pcm_sha256": pcm_sha256, "bpm": bpm, "beat_count": beat_count}
    raw = (json.dumps(value, allow_nan=False, separators=(",", ":")) + "\n").encode("ascii")
    if len(raw) > MAX_OUTPUT_BYTES:
        raise ValueError("BPM summary exceeds output limit")
    return raw


def identity(value):
    return (value.st_dev, value.st_ino, value.st_mode, value.st_size,
            value.st_mtime_ns, value.st_ctime_ns)


def open_verified(path, expected_bytes, expected_sha256):
    path = Path(path)
    if not path.is_absolute():
        raise ValueError("source and model paths must be absolute")
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC)
    stream = os.fdopen(fd, "rb")
    try:
        before = os.fstat(stream.fileno())
        if (not stat.S_ISREG(before.st_mode) or before.st_size != expected_bytes or
                hashlib.file_digest(stream, "sha256").hexdigest() != expected_sha256 or
                identity(before) != identity(os.fstat(stream.fileno())) or
                identity(before) != identity(path.lstat())):
            raise ValueError("pinned local content or identity differs: " + str(path))
        stream.seek(0)
        return stream, before
    except BaseException:
        stream.close()
        raise


def verified_sources(source_dir):
    root = Path(source_dir)
    if not root.is_absolute():
        raise ValueError("source directory must be absolute")
    buffers = {}
    for relative, (size, digest) in SOURCE_PINS.items():
        path = root / relative
        stream, before = open_verified(path, size, digest)
        with stream:
            raw = stream.read(size + 1)
            if (len(raw) != size or hashlib.sha256(raw).hexdigest() != digest or
                    identity(before) != identity(os.fstat(stream.fileno())) or
                    identity(before) != identity(path.lstat())):
                raise ValueError("source changed while retaining verified code")
            buffers[relative] = raw
    return buffers


def load_upstream(source_dir, buffers):
    if any(name == "beat_this" or name.startswith("beat_this.") for name in sys.modules):
        raise ValueError("Beat This was already imported outside the verified source closure")
    for name in ("beat_this", "beat_this.model"):
        package = types.ModuleType(name)
        package.__path__ = []
        package.__package__ = name
        sys.modules[name] = package
        if "." in name:
            setattr(sys.modules["beat_this"], "model", package)
    for relative, raw in buffers.items():
        name = relative[:-3].replace("/", ".")
        module = types.ModuleType(name)
        module.__file__ = str(Path(source_dir) / relative)
        module.__package__ = name.rpartition(".")[0]
        sys.modules[name] = module
        setattr(sys.modules[module.__package__], name.rpartition(".")[2], module)
        exec(compile(raw, module.__file__, "exec"), module.__dict__)
    return sys.modules["beat_this.inference"]


def run(source_dir, model_path, frames, stream):
    # All heavy imports and model work belong to this one bounded child only.
    from importlib import metadata

    buffers = verified_sources(source_dir)
    if not Path(model_path).is_absolute():
        raise ValueError("model path must be absolute")
    for name, version in DEPENDENCIES.items():
        if metadata.version(name) != version:
            raise ValueError("pinned dependency version differs: " + name)
    pcm = read_pcm(stream, frames)
    pcm_sha256 = hashlib.sha256(pcm).hexdigest()
    import numpy as np
    import soxr
    import torch

    torch.set_num_threads(2)
    torch.set_num_interop_threads(1)
    if torch.__version__ != "2.11.0+cpu" or torch.version.cuda is not None:
        raise ValueError("pinned CPU runtime required")

    def forbidden(*_args, **_kwargs):
        raise ValueError("network checkpoint loading is forbidden")

    torch.hub.load_state_dict_from_url = forbidden
    upstream = load_upstream(source_dir, buffers)

    def safe_checkpoint(path, device="cpu"):
        if str(path) != str(model_path) or str(device) != "cpu":
            raise ValueError("unexpected checkpoint/device")
        weights, before = open_verified(model_path, MODEL_BYTES, MODEL_SHA256)
        with weights:
            value = torch.load(weights, map_location="cpu", weights_only=True)
            if (identity(before) != identity(os.fstat(weights.fileno())) or
                    identity(before) != identity(Path(model_path).lstat())):
                raise ValueError("checkpoint changed during safe loading")
        if (not isinstance(value, dict) or not isinstance(value.get("hyper_parameters"), dict) or
                not isinstance(value.get("state_dict"), dict) or not value["state_dict"] or
                any(not isinstance(t, torch.Tensor) or t.device.type != "cpu" or
                    not torch.isfinite(t).all().item() for t in value["state_dict"].values())):
            raise ValueError("invalid safe checkpoint structure/tensors")
        return value

    upstream.load_checkpoint = safe_checkpoint
    frontend = upstream.Audio2Frames(str(model_path), device="cpu", float16=False)
    if frontend.model.training or next(frontend.model.parameters()).dtype != torch.float32:
        raise ValueError("model must be CPU float32 evaluation mode")
    stereo = np.frombuffer(pcm, dtype="<f4").reshape(frames, CHANNELS)
    linear, lengths = [], []
    for channel in range(CHANNELS):
        signal = soxr.resample(stereo[:, channel], in_rate=RATE, out_rate=22050)
        lengths.append(len(signal))
        linear.append(frontend.spect.spect_class(torch.tensor(signal, dtype=torch.float32, device="cpu")))
    if linear[0].shape != linear[1].shape:
        raise ValueError("channel spectrogram geometry differs")
    spect = torch.log1p(frontend.spect.log_multiplier * ((linear[0] + linear[1]) * 0.5)).T
    if spect.ndim != 2 or spect.shape[1] != 128 or not torch.isfinite(spect).all().item():
        raise ValueError("invalid finite linear-mel channel-mean spectrogram")
    mask = source_mask(frames, lengths, len(spect))
    beat, downbeat = frontend.spect2frames(spect)
    if any(x.ndim != 1 or len(x) != len(mask) or x.dtype != torch.float32 or
           x.device.type != "cpu" or not torch.isfinite(x).all().item() for x in (beat, downbeat)):
        raise ValueError("invalid finite source-aligned model logits")
    postprocessor = upstream.Postprocessor(type="minimal", fps=FPS)
    beats, downbeats = postprocessor(beat, downbeat, padding_mask=torch.tensor(mask, dtype=torch.bool))
    seconds, down_seconds = [float(x) for x in beats], [float(x) for x in downbeats]
    source_beat_frames(down_seconds, frames)
    if not set(down_seconds).issubset(seconds):
        raise ValueError("downbeat stream is not a subset of beats")
    bpm, beat_count = summarize_beats(seconds, frames)
    return encode_result(frames, pcm_sha256, bpm, beat_count)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--frames", required=True, type=int)
    args = parser.parse_args()
    validate_frames(args.frames)
    # Libraries cannot add protocol lines to the bounded JSON response.
    from contextlib import redirect_stdout
    with redirect_stdout(sys.stderr):
        result = run(args.source_dir, args.model, args.frames, sys.stdin.buffer)
    sys.stdout.buffer.write(result)
    sys.stdout.buffer.flush()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("BPM analysis failed: " + str(error)[:1024], file=sys.stderr)
        raise SystemExit(1)

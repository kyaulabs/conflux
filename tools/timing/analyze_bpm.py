#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fixed offline, estimated BPM worker; never run on the GUI/audio callback.

Beat This b95c8ab0c58c2d9fcfd40508ae8dffbc05ac4f5c and its published
final0 weights are MIT licensed, copyright 2024 Institute of Computational
Perception, JKU Linz, Austria. The caller supplies that unmodified source tree
and its LICENSE; no third-party code or weights are bundled here.

The summary is a median of estimated adjacent beat intervals, not calibrated
confidence, a constant-tempo assertion, or permission to synchronize decks.
Timing rows retain source-aligned beat and downbeat estimates. Confirmed padding
at the edges or a long break in the source does not supply evidence
for a continuous beat grid. Source validation does not establish musical accuracy.
The parent owns cancellation, process-group cleanup and the wall/output/address-space budget.
"""

import argparse
from bisect import bisect_left, bisect_right
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import statistics
import sys
import types

RATE, CHANNELS, FPS = 48000, 2, 50
MODEL_RATE, MODEL_FFT_SIZE = 22050, 1024
MAX_FRAMES = 33554432
MAX_EVENTS = 34953
MAX_OUTPUT_BYTES = 4 * 1024**2
SILENCE_MARGIN_FRAMES = 70 * RATE // 1000
# The longest local tempo interval is 3 seconds (20 BPM). Leave a longer
# hole after allowing the same margin as at the track edges.
MIN_INTERIOR_SILENCE = 3 * RATE + 2 * SILENCE_MARGIN_FRAMES
# Local intervals need audible support even when the detector continues to
# predict a musical grid through a break. Leave shorter musical rests alone.
MIN_LOCAL_SILENCE = 2 * RATE + 2 * SILENCE_MARGIN_FRAMES
# Confirm dither-sized samples only next to clearly active music. At least
# 5% of each required 250 ms side must exceed 256 steps of the digital floor.
DITHER_FLOOR = 2 ** -15
DITHER_CONTEXT_FRAMES = RATE // 4
DITHER_CONTEXT_FLOOR = DITHER_FLOOR * 256
DITHER_CORRELATION_FRAMES = RATE // 100
MAX_DITHER_ABS_CORRELATION = .25
MAX_DITHER_WINDOW_POWER_RATIO = 4
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
    import numpy as np

    samples = np.frombuffer(pcm, dtype="<f4")
    # Keep the temporary finite-sample mask bounded even for a maximum-length track.
    block_samples = 1 << 20
    for start in range(0, samples.size, block_samples):
        if not np.isfinite(samples[start:start + block_samples]).all():
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
    numerator = frames * MODEL_RATE
    permitted = {numerator // RATE, (numerator + RATE - 1) // RATE}
    if (type(resampled_lengths) not in (list, tuple) or len(resampled_lengths) != 2 or
            any(type(n) is not int or n <= 0 or n not in permitted for n in resampled_lengths) or
            resampled_lengths[0] != resampled_lengths[1] or type(logit_frames) is not int or
            logit_frames != resampled_lengths[0] // (MODEL_RATE // FPS) + 1):
        raise ValueError("unexpected resampled/centered frontend geometry")
    # Contiguous source prefix BEFORE peak pooling; exclude a centered EOF bin
    # without shifting eligible source timestamps or suppressing its neighbour.
    return tuple(k * RATE < frames * FPS for k in range(logit_frames))


def first_nonzero_frame(stereo, noise_floor=0):
    import numpy as np

    # Find nonzero PCM in either channel without allocating a mask for the
    # entire track. Signed zeros are silent in the default exact check.
    block_frames = 1 << 16
    for start in range(0, len(stereo), block_frames):
        block = stereo[start:start + block_frames]
        present = np.any(np.abs(block) > noise_floor if noise_floor else block, axis=1)
        if present.any():
            return start + int(present.argmax())
    return None


def interior_inactive_spans(stereo, first, last, noise_floor=0, minimum_frames=MIN_INTERIOR_SILENCE):
    import numpy as np

    # A qualifying gap cannot fit inside one block. Only the first and last
    # active frames of each nonempty block are needed to find such a gap.
    block_frames = 1 << 16
    spans = []
    previous_end = first + 1
    for start in range(first, last + 1, block_frames):
        left, right = stereo[start:min(start + block_frames, last + 1)].T
        if noise_floor:
            present = (np.abs(left) > noise_floor) | (np.abs(right) > noise_floor)
        else:
            present = np.logical_or(left, right)
        if not present.any():
            continue
        active_start = start + int(present.argmax())
        if active_start - previous_end >= minimum_frames:
            spans.append((previous_end, active_start))
        previous_end = start + len(present) - int(present[::-1].argmax())
    return spans


def has_signal_structure(stereo):
    import numpy as np

    width = DITHER_CORRELATION_FRAMES
    first_power = (stereo[:width].astype(np.float64)**2).sum(axis=0)

    def structured_windows(samples):
        windows = samples.reshape(-1, width, CHANNELS).astype(np.float64)
        power = (windows**2).sum(axis=1)
        if np.any(np.maximum(power, first_power) >
                  MAX_DITHER_WINDOW_POWER_RATIO * np.minimum(power, first_power)):
            return True

        before, after = windows[:, :-1], windows[:, 1:]
        product = np.abs((before * after).sum(axis=1))
        pair_energy = np.sqrt((before**2).sum(axis=1) * (after**2).sum(axis=1))
        return bool(np.any(product > MAX_DITHER_ABS_CORRELATION * pair_energy))

    # Evaluate the same windows in bounded batches. Include a full window
    # ending at the core boundary when the final window is partial.
    block = 64 * width
    full_frames = len(stereo) // width * width
    for start in range(0, full_frames, block):
        if structured_windows(stereo[start:min(start + block, full_frames)]):
            return True
    return full_frames != len(stereo) and structured_windows(stereo[-width:])


def has_dither_context(stereo):
    import numpy as np

    return np.count_nonzero(np.any(np.abs(stereo) >= DITHER_CONTEXT_FLOOR, axis=1)) >= DITHER_CONTEXT_FRAMES // 20


def dither_edge_length(stereo):
    # Use one adjoining passage at a file edge. Check the whole low-level
    # core for structure so quiet fades and rhythmic effects stay eligible.
    boundary = first_nonzero_frame(stereo, DITHER_FLOOR)
    if boundary is None or boundary < MIN_INTERIOR_SILENCE:
        return 0
    if not has_dither_context(stereo[boundary:boundary + DITHER_CONTEXT_FRAMES]):
        return 0
    if has_signal_structure(stereo[:boundary - SILENCE_MARGIN_FRAMES]):
        return 0
    return boundary


def interior_silence_cores(stereo, first, last, minimum_frames=MIN_INTERIOR_SILENCE):
    """Return source ranges of prolonged exact silence or dither-like activity."""
    if last - first <= minimum_frames:
        return []
    margin = SILENCE_MARGIN_FRAMES
    spans = set(interior_inactive_spans(stereo, first, last, minimum_frames=minimum_frames))
    context = DITHER_CONTEXT_FRAMES
    for begin, end in interior_inactive_spans(stereo, first, last, DITHER_FLOOR, minimum_frames):
        if (begin, end) in spans:
            continue
        sides = (stereo[max(first, begin - context):begin],
                 stereo[end:min(last + 1, end + context)])
        if all(map(has_dither_context, sides)) and not has_signal_structure(stereo[begin + margin:end - margin]):
            spans.add((begin, end))

    cores = []
    for begin, end in sorted(spans):
        begin += margin
        end -= margin
        if cores and begin <= cores[-1][1]:
            cores[-1] = (cores[-1][0], max(cores[-1][1], end))
        else:
            cores.append((begin, end))
    return cores


def source_beat_frames(seconds, frames):
    validate_frames(frames)
    if type(seconds) not in (list, tuple) or len(seconds) > min(MAX_EVENTS, frames * FPS // RATE + 1):
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


def supported_logit_ranges(mask, first, last, gaps):
    """Keep the original model frame indices when removing silent spans."""
    tick = RATE // FPS
    margin = SILENCE_MARGIN_FRAMES
    start = max(0, (first - margin + tick - 1) // tick)
    stop = min(sum(mask), (last + margin) // tick + 1)
    for begin, end in gaps:
        before = min(stop, (begin + tick - 1) // tick)
        if start < before:
            yield start, before
        start = max(start, (end + tick - 1) // tick)
        if start >= stop:
            return
    if start < stop:
        yield start, stop


def decode_source_events(postprocessor, beat, downbeat, mask, first, last, gaps):
    beats, downbeats = [], []
    for start, stop in supported_logit_ranges(mask, first, last, gaps):
        # Peak pooling and nearest-beat matching must stay within the same
        # supported source span. Passing interior holes as padding would
        # squeeze the masked frames out of the upstream decoder's clock.
        local_beats, local_downbeats = postprocessor(beat[start:stop], downbeat[start:stop])
        if not len(local_beats):
            continue  # The decoder may emit downbeat peaks without a beat anchor.
        offset = start / FPS
        beats.extend(float(time) + offset for time in local_beats)
        downbeats.extend(float(time) + offset for time in local_downbeats)
    return beats, downbeats


def steady_tempo_period(periods, typical, measured):
    """Estimate one period from long, consistent runs of measured beats."""
    # The model places events on a 50 Hz grid. Averaging eight adjacent
    # intervals reduces the effect of that timestamp quantization.
    width = 8
    agreement = max(RATE // FPS, typical * .05)
    windows, block = [], []
    for period in periods:
        if period is None or abs(period - typical) > agreement:
            block.clear()
            continue
        block.append(period)
        if len(block) == width:
            windows.append(statistics.mean(block))
            block.clear()
    # Require a well-supported steady clock before replacing the per-beat
    # median, which remains useful for short or changing-tempo sources.
    if len(windows) < 12 or len(windows) * width < .75 * measured:
        return None
    deciles = statistics.quantiles(windows, n=10, method="inclusive")
    low, high = deciles[0], deciles[-1]
    if high - low > .02 * statistics.median(windows):
        return None
    body = [period for period in windows if low <= period <= high]
    return statistics.mean(body) if body else None


def summarize_beats(seconds, frames, gap_starts=(), *, silent_cores=()):
    beat_frames = source_beat_frames(seconds, frames)
    unmeasured = unmeasured_intervals(beat_frames, silent_cores)
    periods = []
    for index, (first, last) in enumerate(zip(beat_frames, beat_frames[1:]), 1):
        period = last - first
        # Use the same supported range as the deck's source-local tempo.
        # Store an explicit gap so a window cannot bridge a silent break.
        if (unmeasured[index] or not RATE * 15 // 100 <= period <= RATE * 3 or
                (gap_starts and bisect_right(gap_starts, first) != bisect_right(gap_starts, last))):
            periods.append(None)
        else:
            periods.append(period)
    measured = [period for period in periods if period is not None]
    if not measured:
        return None, len(beat_frames)
    steady = steady_tempo_period(periods, statistics.median(measured), len(measured))
    summary = 60 * RATE / steady if steady is not None else statistics.median(60 * RATE / period for period in measured)
    return round(summary, 1), len(beat_frames)


def unmeasured_intervals(beat_frames, silent_cores):
    """Identify adjacent predicted intervals without source-local tempo support."""
    unmeasured = [False] * len(beat_frames)
    for begin, end in silent_cores:
        first = bisect_left(beat_frames, begin)
        last = bisect_left(beat_frames, end)
        # Intervals entirely inside confirmed silence have no audible
        # support, even when one flank is too short to assess its clock.
        for index in range(first + 1, last):
            unmeasured[index] = True
        left = beat_frames[max(0, first - 4):first]
        right = beat_frames[last:last + 4]
        if len(left) < 3 or len(right) < 3:
            continue
        periods = [statistics.median(b - a for a, b in zip(side, side[1:]))
                   for side in (left, right)]
        # Sparse material can have near-silence between actual beats. Require
        # at least two periods at the slower side before withholding estimates.
        if not all(7200 <= period <= 144000 for period in periods) or end - begin < 2 * max(periods):
            continue
        for index in range(first, min(last + 1, len(beat_frames))):
            unmeasured[index] = True
    return unmeasured


def encode_result(frames, pcm_sha256, bpm, beat_seconds, downbeat_seconds, silent_cores=()):
    # Keep the existing source-time validation, including rejection of a
    # timestamp that rounds to EOF. Never sort, filter, or repair detector output.
    beat_frames = source_beat_frames(beat_seconds, frames)
    downbeat_frames = source_beat_frames(downbeat_seconds, frames)
    if not set(downbeat_seconds).issubset(beat_seconds):
        raise ValueError("downbeat stream is not a subset of beats")
    beat_count = len(beat_frames)
    if (type(pcm_sha256) is not str or len(pcm_sha256) != 64 or
            any(c not in "0123456789abcdef" for c in pcm_sha256) or
            (bpm is not None and (type(bpm) not in (int, float) or not 20 <= bpm <= 400 or
                                 not math.isfinite(bpm) or beat_count < 2 or round(bpm, 1) != bpm))):
        raise ValueError("invalid BPM summary output")
    value = {"schema": 3, "recipe": RECIPE, "frames": frames,
             "pcm_sha256": pcm_sha256, "bpm": bpm, "beat_count": beat_count}
    raw = bytearray((json.dumps(value, allow_nan=False, separators=(",", ":")) + "\n").encode("ascii"))
    if len(raw) > MAX_OUTPUT_BYTES:
        raise ValueError("BPM timing output exceeds output limit")
    downbeats = set(downbeat_frames)
    for frame, unsupported in zip(beat_frames, unmeasured_intervals(beat_frames, silent_cores), strict=True):
        event = {"frame": frame, "downbeat": frame in downbeats, "unmeasured_before": unsupported}
        raw.extend((json.dumps(event, allow_nan=False, separators=(",", ":")) + "\n").encode("ascii"))
        if len(raw) > MAX_OUTPUT_BYTES:
            raise ValueError("BPM timing output exceeds output limit")
    return bytes(raw)


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
    # Reflection padding needs more than half an FFT of resampled audio.
    # The nearest-frame cutoff is half an FFT plus half a model frame. It
    # falls between two source-frame counts at these pinned sample rates.
    if 2 * frames * MODEL_RATE < (MODEL_FFT_SIZE + 1) * RATE:
        return encode_result(frames, pcm_sha256, None, [], [])
    import numpy as np

    stereo = np.frombuffer(pcm, dtype="<f4").reshape(frames, CHANNELS)
    first = first_nonzero_frame(stereo)
    if first is None:
        return encode_result(frames, pcm_sha256, None, [], [])
    last = frames - 1 - first_nonzero_frame(stereo[::-1])

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
    linear, lengths = [], []
    for channel in range(CHANNELS):
        signal = soxr.resample(stereo[:, channel], in_rate=RATE, out_rate=MODEL_RATE)
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
    gaps = interior_silence_cores(stereo, first, last)
    gap_starts = tuple(begin for begin, _ in gaps)
    span = stereo[first:last + 1]
    signal_first = first + dither_edge_length(span)
    signal_last = last - dither_edge_length(span[::-1])
    seconds, down_seconds = decode_source_events(postprocessor, beat, downbeat, mask,
                                                signal_first, signal_last, gaps)
    local_silence = interior_silence_cores(stereo, first, last, MIN_LOCAL_SILENCE)
    bpm, _beat_count = summarize_beats(seconds, frames, gap_starts, silent_cores=local_silence)
    return encode_result(frames, pcm_sha256, bpm, seconds, down_seconds, local_silence)


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

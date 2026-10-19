#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Compare completed local CPU/Vulkan outputs and prepare listening excerpts.

No inference, playback or downloads. Numerical agreement and join diagnostics
do not establish separation quality or audible seams; those require listening.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import wave

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.stems.worker import RATE, HOP, OVERLAP, STEMS

LAG_LIMIT = 256
NEIGHBOR_FRAMES = round(RATE * 0.020)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def pin(path):
    path = Path(path).resolve()
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest}


def load_json(path):
    return json.loads(Path(path).read_text())


def pcm16(path):
    with wave.open(str(path), "rb") as audio:
        require(audio.getparams()[:3] == (2, 2, RATE) and audio.getcomptype() == "NONE",
                "Expected stereo 44.1 kHz PCM16 WAV: " + str(path))
        frames = audio.getnframes()
        data = np.frombuffer(audio.readframes(frames), dtype="<i2")
        require(frames > 0 and data.size == frames * 2, "Incomplete WAV: " + str(path))
    return data.reshape(frames, 2).astype(np.float64) / 32768


def errors(reference, candidate):
    require(reference.shape == candidate.shape and reference.ndim == 2,
            "Comparison geometry differs")
    reference, candidate = np.asarray(reference, dtype=np.float64), np.asarray(candidate, dtype=np.float64)
    delta = candidate - reference
    reference_rms = math.sqrt(float(np.mean(np.square(reference))))
    error_rms = math.sqrt(float(np.mean(np.square(delta))))
    relative = error_rms / reference_rms if reference_rms > 0 else None
    return {"reference_rms": reference_rms, "candidate_rms": math.sqrt(float(np.mean(np.square(candidate)))),
            "rms_error": error_rms, "maximum_absolute_error": float(np.max(np.abs(delta))),
            "bias": float(np.mean(delta)), "bias_per_channel": np.mean(delta, axis=0).tolist(),
            "rms_error_relative_to_reference": relative,
            "error_to_reference_db": 20 * math.log10(relative) if relative is not None and relative > 0 else None,
            "exact_zero_error": bool(error_rms == 0)}


def lag_diagnostic(reference, candidate):
    reference = np.asarray(reference, dtype=np.float64)
    candidate = np.asarray(candidate, dtype=np.float64)
    reference = reference - reference.mean(axis=0)
    candidate = candidate - candidate.mean(axis=0)
    energy = math.sqrt(float(np.square(reference).sum()) * float(np.square(candidate).sum()))
    limit = min(LAG_LIMIT, len(reference) - 1)
    if energy == 0:
        return {"search_limit_samples": limit, "lag_samples": None, "correlation": None,
                "reason": "At least one demeaned signal has zero energy"}
    size = 1 << (2 * len(reference) - 1).bit_length()
    spectrum = np.zeros(size // 2 + 1, dtype=np.complex128)
    for channel in range(2):
        spectrum += np.conj(np.fft.rfft(reference[:, channel], n=size)) * np.fft.rfft(candidate[:, channel], n=size)
    correlation = np.fft.irfft(spectrum, n=size)
    lags = np.arange(-limit, limit + 1)
    values = correlation[lags % size] / energy
    best = int(np.argmax(values))
    return {"search_limit_samples": limit, "lag_samples": int(lags[best]),
            "lag_seconds": float(lags[best]) / RATE, "correlation": float(values[best])}


def join_diagnostic(signal, frame):
    start, stop = max(0, frame - NEIGHBOR_FRAMES), min(len(signal), frame + NEIGHBOR_FRAMES)
    local = np.asarray(signal[start:stop], dtype=np.float64)
    derivative = np.diff(local, axis=0)
    jump = np.asarray(signal[frame], dtype=np.float64) - signal[frame - 1]
    jump_rms = math.sqrt(float(np.mean(np.square(jump))))
    local_rms = math.sqrt(float(np.mean(np.square(derivative))))
    return {"signed_jump_per_channel": jump.tolist(), "jump_rms": jump_rms,
            "jump_maximum_absolute": float(np.max(np.abs(jump))),
            "neighborhood_frame_interval": [start, stop],
            "neighborhood_derivative_rms": local_rms,
            "neighborhood_derivative_maximum_absolute": float(np.max(np.abs(derivative))),
            "jump_rms_over_neighborhood_derivative_rms": jump_rms / local_rms if local_rms else None}


def audition(source, stems, output, ffmpeg):
    count, gap = RATE * 5, round(RATE * 0.25)
    start, stop = HOP - count // 2, HOP - count // 2 + count
    require(stop <= len(source), "A five-second excerpt centered on first HOP needs at least 8.35 seconds")
    fade_frames = int(RATE * 0.005 + 0.5)
    fade = np.linspace(0, 1, fade_frames)
    signals = [("source", source), *zip(STEMS, stems),
               ("recombined", np.sum(stems, axis=0, dtype=np.float64))]
    segments, labels = [], []
    for index, (label, signal) in enumerate(signals):
        excerpt = np.array(signal[start:stop], dtype=np.float64, copy=True)
        excerpt[:fade_frames] *= fade[:, None]
        excerpt[-fade_frames:] *= fade[::-1, None]
        segments.append(excerpt)
        labels.append({"label": label, "preview_start_seconds": index * (count + gap) / RATE,
                       "preview_end_seconds": (index * (count + gap) + count) / RATE,
                       "source_frame_interval": [start, stop],
                       "source_seconds_interval": [start / RATE, stop / RATE]})
        if index < len(signals) - 1:
            segments.append(np.zeros((gap, 2), dtype=np.float64))
    combined = np.concatenate(segments)
    peak = float(np.max(np.abs(combined)))
    gain = 0.98 / peak if peak > 1 else 1.0
    attempts = []
    encode = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
              "-f", "f32le", "-ar", str(RATE), "-ac", "2", "-i", "pipe:0",
              "-codec:a", "libmp3lame", "-q:a", "2", str(output)]
    decode = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(output),
              "-f", "f32le", "-acodec", "pcm_f32le", "-ar", str(RATE), "-ac", "2", "pipe:1"]
    for _ in range(3):
        data = np.asarray(combined * gain, dtype="<f4").tobytes()
        subprocess.run(encode, input=data, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=True, timeout=60)
        decoded = subprocess.run(decode, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=60).stdout
        values = np.frombuffer(decoded, dtype="<f4")
        require(values.size > 0 and values.size % 2 == 0 and np.isfinite(values).all(), "Invalid decoded MP3")
        decoded_peak = float(np.max(np.abs(values)))
        attempts.append({"common_gain": gain, "decoded_sample_peak": decoded_peak})
        if decoded_peak <= 1:
            break
        gain *= 0.98 / decoded_peak
    else:
        raise RuntimeError("Cannot produce preview without decoded sample clipping after three common-gain attempts")
    lossless = output.with_suffix(".wav")
    # Reuse the same segment layout and final common gain as the MP3 input.
    lossless_encode = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                       "-f", "f32le", "-ar", str(RATE), "-ac", "2", "-i", "pipe:0",
                       "-codec:a", "pcm_f32le", str(lossless)]
    lossless_pcm = np.asarray(combined * gain, dtype="<f4").tobytes()
    subprocess.run(lossless_encode, input=lossless_pcm, stdout=subprocess.DEVNULL,
                   stderr=subprocess.PIPE, check=True, timeout=60)
    lossless_decode = [*decode]
    lossless_decode[lossless_decode.index(str(output))] = str(lossless)
    roundtrip = subprocess.run(lossless_decode, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               check=True, timeout=60).stdout
    require(roundtrip == lossless_pcm, "Lossless float WAV preview roundtrip differs")
    return {**pin(output), "backend": "vulkan", "segments": labels,
            "preview_pcm_frames": len(combined), "preview_pcm_seconds": len(combined) / RATE,
            "decoded_frames": values.size // 2,
            "gap_frames": gap, "edge_fade_frames_per_segment": fade_frames,
            "edge_fade_nominal_ms": 5, "common_gain": gain,
            "peak_before_gain_after_edge_fades": peak, "decoded_sample_peak": decoded_peak,
            "encoding_attempts": attempts, "encode_command": encode, "decode_command": decode,
            "lossless_wav": {**pin(lossless), "codec": "pcm_f32le", "exact_float32_roundtrip": True,
                             "encode_command": lossless_encode, "decode_command": lossless_decode},
            "internal_join_frames_in_each_excerpt": [HOP - start, HOP + OVERLAP - start],
            "scope": "Same source interval and one common attenuation for all six segments; only outer excerpt edges faded. Internal joins untouched. MP3 is lossy; raw NPY output is the numerical evidence. Sample peak check does not measure intersample true peak. No sound played."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("manifest", "cpu-run", "vulkan-run", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    manifest_path = args.manifest.resolve()
    manifest = load_json(manifest_path)
    run_paths = {"cpu": args.cpu_run.resolve() / "result.json",
                 "vulkan": args.vulkan_run.resolve() / "result.json"}
    runs = {key: load_json(path) for key, path in run_paths.items()}
    for backend, run in runs.items():
        require(run["status"] == "ok" and not run["failures"] and run["backend"] == backend,
                "Both backend runs must have completed successfully")
        require(run["pins"]["manifest"]["sha256"] == pin(manifest_path)["sha256"], "Manifest identity differs")
        require((run["hop_frames"], run["overlap_frames"]) == (HOP, OVERLAP), "Worker grid differs")
    common = ("runtime", "model", "worker_source", "shared_benchmark_source", "sampler_source", "driver")
    require(all(runs["cpu"]["pins"][key]["sha256"] == runs["vulkan"]["pins"][key]["sha256"] for key in common),
            "CPU/Vulkan producer or model pins differ")
    worker_pin = pin(ROOT / "tools/stems/worker.py")
    require(worker_pin["sha256"] == runs["cpu"]["pins"]["worker_source"]["sha256"], "Imported worker geometry source changed")
    tracks = manifest["tracks"]
    ids = [track["id"] for track in tracks]
    require(bool(ids) and len(ids) == len(set(ids)) and all(isinstance(value, str) and
            re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", value) for value in ids), "Invalid/duplicate track IDs")
    indexed = {backend: {track["id"]: track for track in run["tracks"]} for backend, run in runs.items()}
    require(all(set(rows) == set(ids) and len(rows) == len(runs[key]["tracks"]) for key, rows in indexed.items()),
            "Backend track lists differ from manifest")
    output = args.output.resolve()
    require(not output.exists(), "Output must be fresh")
    ffmpeg = shutil.which("ffmpeg")
    require(ffmpeg is not None, "Installed ffmpeg is required for listening MP3s")
    output.mkdir(parents=True)
    report = {"schema": 1, "status": "ok", "analyzed_at_utc": datetime.now(timezone.utc).isoformat(),
              "inputs": {"manifest": pin(manifest_path), "runs": {key: pin(path) for key, path in run_paths.items()},
                         "analysis_source": pin(Path(__file__)), "worker_source": worker_pin, "ffmpeg": pin(ffmpeg)},
              "producer_pins": {key: runs["cpu"]["pins"][key] for key in common},
              "sample_rate": RATE, "stem_order": list(STEMS), "hop_frames": HOP, "overlap_frames": OVERLAP,
              "metric_definitions": {
                  "units": "Unshifted full-length linear PCM, source PCM16 divided by32768; float64 reductions over frames and both channels",
                  "cpu_vulkan": "Reference=CPU; candidate=Vulkan. Error=candidate-reference, RMS=sqrt(mean(error²)), maximum=max(abs(error)), bias=mean(error), relative RMS=error RMS/reference RMS",
                  "decibels": "20log10(relative RMS); null when reference RMS is zero or error is exactly zero. exact_zero_error distinguishes zero error",
                  "lag": "Channel-demeaned zero-padded FFT cross correlation summed across channels, normalized by full-signal energies, searched within±256samples. Positive lag means Vulkan occurs later than CPU. No signals are shifted for raw errors. Periodic or weak signals may give ambiguous peaks",
                  "recombination": "Unshifted sum of four stems minus source; bias_per_channel is DC residual. This is not a separation-quality score",
                  "joins": "At kHOP and kHOP+OVERLAP for k>=1 inside source. Jump=sample[b]-sample[b-1]; neighborhood derivatives use±20ms, clipped at source edges. No pass thresholds or attribution of artifacts",
                  "references": "Only supplied aligned reference WAVs produce reference metrics; raw amplitude/phase alignment retained, no gain fitting or time shifting"},
              "tracks": [], "human_listening_verdict": None,
              "scope": "Objective saved-output diagnostics and lossy audition files only. CPU agreement/recombination/join statistics do not establish musical quality or audible artifacts. No inference, playback or download performed."}
    for raw in tracks:
        identifier = raw["id"]
        source_path = Path(raw["input_wav"])
        source_path = (manifest_path.parent / source_path).resolve() if not source_path.is_absolute() else source_path.resolve()
        source_pin, source = pin(source_path), pcm16(source_path)
        arrays, outputs = {}, {}
        for backend in runs:
            saved = indexed[backend][identifier]
            require(saved["input"]["sha256"] == source_pin["sha256"], "Input identity differs: " + identifier)
            require(saved["input"]["frames"] == len(source) and saved["continuous"]["start_frame"] == 0
                    and saved["continuous"]["frames"] == len(source), "Continuous source coverage differs")
            item = saved["output"]
            actual = pin(item["path"])
            require(item["complete"] and actual["sha256"] == item["sha256"] and actual["bytes"] == item["bytes"],
                    "Incomplete/changed output: " + identifier)
            array = np.load(item["path"], mmap_mode="r", allow_pickle=False)
            require(array.shape == (4, len(source), 2) and list(array.shape) == item["shape"]
                    and array.dtype == np.dtype("<f4") and np.isfinite(array).all(), "Invalid output geometry/finite coverage")
            arrays[backend], outputs[backend] = array, actual
        cpu, vulkan = arrays["cpu"], arrays["vulkan"]
        row = {"id": identifier, "input": {**source_pin, "frames": len(source)}, "outputs": outputs,
               "annotations": raw["annotations"], "geometry_finite_full_coverage_verified": True,
               "cpu_vulkan": {stem: {**errors(cpu[index], vulkan[index]), "lag_diagnostic": lag_diagnostic(cpu[index], vulkan[index])}
                              for index, stem in enumerate(STEMS)},
               "recombination": {backend: errors(source, np.sum(array, axis=0, dtype=np.float64)) for backend, array in arrays.items()},
               "references": {"status": "supplied" if raw.get("reference_wavs") else "not_supplied", "stems": {}}, "joins": []}
        for stem, value in raw.get("reference_wavs", {}).items():
            require(stem in STEMS, "Unknown reference stem")
            path = Path(value)
            path = (manifest_path.parent / path).resolve() if not path.is_absolute() else path.resolve()
            reference_pin, reference = pin(path), pcm16(path)
            require(reference.shape == source.shape, "Reference/source geometry differs")
            require(all(indexed[key][identifier]["references"][stem]["sha256"] == reference_pin["sha256"] for key in runs),
                    "Reference identity differs from execution")
            index = STEMS.index(stem)
            row["references"]["stems"][stem] = {"input": reference_pin,
                "cpu": errors(reference, cpu[index]), "vulkan": errors(reference, vulkan[index])}
        for boundary in range(HOP, len(source), HOP):
            for kind, frame in (("hop", boundary), ("overlap_exit", boundary + OVERLAP)):
                if frame >= len(source):
                    continue
                row["joins"].append({"kind": kind, "frame": frame, "seconds": frame / RATE,
                    "source": join_diagnostic(source, frame),
                    **{backend: {stem: join_diagnostic(array[index], frame) for index, stem in enumerate(STEMS)}
                       for backend, array in arrays.items()}})
        row["audition"] = audition(source, vulkan, output / (identifier + "-listen.mp3"), ffmpeg)
        report["tracks"].append(row)
    with (output / "result.json").open("x") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"output": str(output), "tracks": ids, "status": "ok", "human_listening_verdict": None}))


if __name__ == "__main__":
    main()

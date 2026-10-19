#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Inspect saved source/CPU/Vulkan waveforms for missing blocks; no inference."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import wave
import numpy as np

BASE = Path(__file__).resolve().parent
RATE, BIN = 44100, 882
STEMS = ("drums", "bass", "other", "vocals")
THRESHOLDS = {"-80": 1e-4, "-60": 1e-3}
ACTIVE = {"-60": 1e-3, "-40": 1e-2}


def spans(mask, samples_per_item=BIN):
    mask = np.asarray(mask, dtype=bool)
    edges = np.diff(np.concatenate(([False], mask, [False])).astype(np.int8))
    return [{"start_seconds": int(start) * samples_per_item / RATE,
             "end_seconds": int(end) * samples_per_item / RATE,
             "duration_seconds": int(end-start) * samples_per_item / RATE}
            for start, end in zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1))]


def rms_windows(signal):
    # The prepared 23.4-second clips contain exactly 1170 complete 20ms windows.
    if len(signal) % BIN:
        raise ValueError("Unexpected nonintegral 20ms source geometry")
    return np.sqrt(np.mean(np.square(np.asarray(signal, dtype=np.float64).reshape(-1, BIN, 2)), axis=(1, 2)))


def zero_summary(signal):
    mask = np.all(signal == 0, axis=1)
    runs = spans(mask, 1)
    return {"exact_zero_stereo_frames": int(mask.sum()), "exact_zero_runs": len(runs),
            "longest_run_seconds": max((run["duration_seconds"] for run in runs), default=0),
            "longest_runs": sorted(runs, key=lambda row: row["duration_seconds"], reverse=True)[:10],
            "all_runs_at_least_20ms": [run for run in runs if run["duration_seconds"] >= .02]}


def main():
    inputs = {}
    def read(path):
        raw = path.read_bytes()
        inputs[str(path)] = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        return json.loads(raw)
    def verify(path, expected):
        with path.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != expected:
            raise ValueError("Saved waveform identity changed: " + str(path))
        inputs[str(path)] = {"bytes": path.stat().st_size, "sha256": actual}
    manifest = read(BASE / "manifest.json")
    runs = {backend: read(BASE / backend / "result.json") for backend in ("cpu", "vulkan")}
    analysis = read(BASE / "analysis/result.json")
    if not all(run["status"] == "ok" for run in runs.values()):
        raise ValueError("Requires both completed waveform runs")
    report = {"schema": 1, "at_utc": datetime.now(timezone.utc).isoformat(),
        "window_frames": BIN, "window_seconds": .02,
        "low_energy_rms_thresholds_linear": THRESHOLDS, "source_active_rms_thresholds_linear": ACTIVE,
        "scope": "Raw saved source WAV and native float32 arrays, before preview assembly/encoding. Descriptive absolute RMS thresholds are not musical-quality pass thresholds. No inference, playback or source modification.",
        "tracks": [], "inputs": inputs}
    for index, raw in enumerate(manifest["tracks"]):
        source_path = Path(raw["input_wav"])
        if not source_path.is_absolute():
            source_path = BASE / source_path
        pinned = runs["cpu"]["tracks"][index]
        verify(source_path, pinned["input"]["sha256"])
        with wave.open(str(source_path), "rb") as stream:
            if stream.getparams()[:3] != (2, 2, RATE):
                raise ValueError("Unexpected source geometry")
            source = np.frombuffer(stream.readframes(stream.getnframes()), "<i2").reshape(-1, 2).astype(np.float64) / 32768
        srms = rms_windows(source)
        count = len(srms)
        track = {"id": raw["id"], "frames": len(source), "seconds": len(source)/RATE,
            "original_decoded_source_offset_seconds": raw["annotations"]["excerpt_source_offset_seconds"],
            "source": {"rms_minimum": float(srms.min()), "rms_maximum": float(srms.max()),
                "exact_zero": zero_summary(source),
                "low_energy_spans": {label: spans(srms < threshold) for label, threshold in THRESHOLDS.items()}},
            "backends": {}, "seam_windows": []}
        rms_by_backend = {}
        for backend in ("cpu", "vulkan"):
            saved = runs[backend]["tracks"][index]
            if saved["id"] != raw["id"] or saved["input"]["sha256"] != pinned["input"]["sha256"]:
                raise ValueError("Input/track mismatch")
            path = Path(saved["output"]["path"])
            verify(path, saved["output"]["sha256"])
            array = np.load(path, mmap_mode="r")
            if array.shape != (4, len(source), 2) or not np.isfinite(array).all():
                raise ValueError("Invalid output geometry/finite samples")
            recombined = np.sum(array, axis=0, dtype=np.float64)
            signals = {**{stem: array[i] for i, stem in enumerate(STEMS)}, "recombined": recombined}
            energies = {label: rms_windows(signal) for label, signal in signals.items()}
            rms_by_backend[backend] = energies
            summary = {"signals": {}, "source_active_output_collapse": {},
                "exact_all_stem_zero": zero_summary(np.max(np.abs(array), axis=0))}
            for label, signal in signals.items():
                energy = energies[label]
                summary["signals"][label] = {"rms_minimum": float(energy.min()), "rms_maximum": float(energy.max()),
                    "exact_zero": zero_summary(signal),
                    "low_energy_spans": {key: spans(energy < threshold) for key, threshold in THRESHOLDS.items()}}
            stem_energy = np.stack([energies[stem] for stem in STEMS])
            for active_label, active_level in ACTIVE.items():
                active = srms >= active_level
                ratio = energies["recombined"][active] / srms[active]
                summary["source_active_output_collapse"][active_label] = {
                    "source_active_windows": int(active.sum()),
                    "minimum_recombined_to_source_rms_ratio": float(ratio.min()) if len(ratio) else None,
                    "recombined_below_one_tenth_source_spans": spans(active & (energies["recombined"] < .1 * srms)),
                    "thresholds": {low_label: {
                        "all_four_stems_below_threshold_spans": spans(active & np.all(stem_energy < low_level, axis=0)),
                        "recombined_below_threshold_spans": spans(active & (energies["recombined"] < low_level))}
                        for low_label, low_level in THRESHOLDS.items()}}
            residual = rms_windows(recombined-source)
            active_indices = np.flatnonzero(srms >= .01)
            worst = sorted(active_indices, key=lambda j: energies["recombined"][j]/srms[j])[:10]
            summary["lowest_recombined_source_ratio_active_windows"] = [{"start_seconds": int(j)*.02,
                "end_seconds": (int(j)+1)*.02, "source_rms": float(srms[j]),
                "recombined_rms": float(energies["recombined"][j]), "residual_rms": float(residual[j]),
                "recombined_source_ratio": float(energies["recombined"][j]/srms[j])} for j in worst]
            summary["final_100ms"] = {"start_seconds": (count-5)*.02, "end_seconds": count*.02,
                "source_rms_windows": srms[-5:].tolist(),
                "signal_rms_windows": {label: values[-5:].tolist() for label, values in energies.items()}}
            track["backends"][backend] = summary
        track["CPU_Vulkan_low_energy_mask_comparison"] = {label: {low: {
            "both_below_windows": int(((rms_by_backend['cpu'][label] < level) & (rms_by_backend['vulkan'][label] < level)).sum()),
            "CPU_only_below_spans": spans((rms_by_backend['cpu'][label] < level) & (rms_by_backend['vulkan'][label] >= level)),
            "Vulkan_only_below_spans": spans((rms_by_backend['vulkan'][label] < level) & (rms_by_backend['cpu'][label] >= level))}
            for low, level in THRESHOLDS.items()} for label in (*STEMS, "recombined")}
        for join in analysis["tracks"][index]["joins"]:
            j = min(int(join["frame"] // BIN), count-1)
            track["seam_windows"].append({"kind": join["kind"], "boundary_seconds": join["seconds"],
                "containing_20ms_window": [j*.02, (j+1)*.02], "source_rms": float(srms[j]),
                **{backend: {label: float(values[j]) for label, values in energies.items()}
                   for backend, energies in rms_by_backend.items()}})
        report["tracks"].append(track)
    report["limits"] = ["No all-output collapse in 20ms RMS bins does not establish correctness or absence of shorter/partial dropouts.",
        "Exact-zero runs use frames where both stereo channels are exactly zero; tiny nonzero values can still be inaudible.",
        "Per-stem quiet spans may reflect absent instruments, model routing or separation loss. Source mixture and CPU agreement cannot identify the correct isolated stem without a reference/listening assessment.",
        "All interval times are within the prepared23.4s clip; add30s for the original decoded track timeline. 20ms spans are bin boundaries, not exact acoustic transition times.",
        "The separate original preview alternates source, drums, bass, other, vocals and recombined in5s blocks with .25s inserted silence. This raw analysis neither assumes those gaps explain the report nor analyzes preview codec artifacts."]
    report["analysis_source_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    output = BASE / "waveform-numerics.json"
    with output.open("x") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"output": str(output), "tracks": len(report["tracks"])}))


if __name__ == "__main__":
    main()

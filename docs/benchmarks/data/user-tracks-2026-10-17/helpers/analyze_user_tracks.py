#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Analyze completed local user-track CPU/Vulkan outputs; no inference/playback.

Creates only full 23.4-second single-signal float WAVs and four composite plots.
Quiet/collapse/join diagnostics are descriptive and do not establish quality.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import tempfile

import numpy as np
from PIL import Image, ImageDraw, __version__ as PILLOW_VERSION

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
RATE, WINDOW, HOP, OVERLAP, FRAMES, BIN = 44100, 343980, 257985, 85995, 1031940, 882
STEMS = ("drums", "bass", "other", "vocals")
LABELS = ("source", *STEMS, "recombined")
DEPENDENCIES = {
    "metrics": ("quality_analyze.py", "4131400abe7ed071d31f6da6263815e4eb14eb02b32f5e7c979cc41e86b83bac"),
    "quiet": ("waveform_numerics.py", "66f9215974fe110455d1f0e21d6de529c18a0c55084b3116338c48c479e53325"),
    "waveform": ("waveform_review.py", "f1d84b8cc467bc6195d3450e4c6f63415b98a669b89d35eb9ab0647098f5037f"),
}


def require(test, message):
    if not test:
        raise ValueError(message)


def pin(path):
    path = Path(path).resolve()
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest}


def load(path):
    def reject(value):
        raise ValueError("Nonfinite JSON constant: " + value)
    return json.loads(Path(path).read_text(), parse_constant=reject)


def write(path, value):
    with Path(path).open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def dependencies():
    modules, identities = {}, {}
    for name, (filename, expected) in DEPENDENCIES.items():
        path = ROOT / "benchmarks-local/quality-2026-10-17" / filename
        identity = pin(path)
        require(identity["sha256"] == expected, "Proven helper changed: " + filename)
        spec = importlib.util.spec_from_file_location("m0_user_tracks_" + name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        modules[name], identities[name] = module, identity
    return modules, identities


def selfchecks(metric, quiet):
    checks = []
    reference = np.ones((512, 2))
    error = metric.errors(reference, reference + .25)
    require(error["rms_error"] == error["maximum_absolute_error"] == error["bias"] == .25, "Constant-bias hand result differs")
    checks.append("constant bias hand result")
    exact = metric.errors(reference, reference)
    require(exact["rms_error"] == 0 and exact["exact_zero_error"] and exact["error_to_reference_db"] is None, "Exact-zero JSON representation differs")
    checks.append("exact zero error JSON representation")
    noise = np.random.default_rng(731).normal(size=(4096, 2))
    require(metric.lag_diagnostic(noise, np.vstack((np.zeros((7, 2)), noise[:-7])))["lag_samples"] == 7, "Delay sign differs")
    checks.append("known seven-sample delay sign")
    require(metric.lag_diagnostic(noise, np.vstack((noise[7:], np.zeros((7, 2)))))["lag_samples"] == -7, "Advance sign differs")
    checks.append("known seven-sample advance sign")
    require(metric.lag_diagnostic(reference, reference)["lag_samples"] is None, "Zero-energy lag should be undefined")
    checks.append("zero-energy lag undefined")
    step = np.zeros((200, 2)); step[100:] = 1
    join = metric.join_diagnostic(step, 100)
    require(join["signed_jump_per_channel"] == [1, 1] and join["jump_rms"] == 1
            and math.isclose(join["neighborhood_derivative_rms"], 1 / math.sqrt(199)), "Step-join hand result differs")
    checks.append("stereo step jump and local derivative hand result")
    signal = np.zeros((2 * BIN, 2)); signal[BIN:] = .01
    require(np.allclose(quiet.rms_windows(signal), [0, .01], rtol=1e-14, atol=0), "20ms RMS hand result differs")
    checks.append("two complete 20ms RMS bins")
    zero = quiet.zero_summary(signal)
    require(zero["exact_zero_stereo_frames"] == BIN and zero["longest_run_seconds"] == .02, "Exact-zero duration differs")
    checks.append("exact-zero stereo duration")
    span = quiet.spans(np.array([False, True, True, False]))
    require(len(span) == 1 and span[0]["start_seconds"] == .02 and span[0]["end_seconds"] == .06
            and span[0]["duration_seconds"] == .04, "Quiet span boundaries differ")
    checks.append("20ms span boundaries")
    return {"status": "passed", "checks": checks, "scope": "Small deterministic hand checks; no music acceptance threshold"}


def quiet_backend(source_rms, array, quiet, waveform):
    signals = {**{stem: array[index] for index, stem in enumerate(STEMS)},
               "recombined": np.sum(array, axis=0, dtype=np.float64)}
    energy = {label: quiet.rms_windows(signal) for label, signal in signals.items()}
    report = {"signals": {}, "exact_all_stem_zero": quiet.zero_summary(np.max(np.abs(array), axis=0)),
              "source_active_output_collapse": {}}
    for label, signal in signals.items():
        values = energy[label]
        report["signals"][label] = {"raw": waveform.stats(signal), "exact_zero": quiet.zero_summary(signal),
            "rms_20ms_minimum": float(values.min()), "rms_20ms_maximum": float(values.max()),
            "low_energy_spans": {key: quiet.spans(values < threshold) for key, threshold in quiet.THRESHOLDS.items()}}
    stem_energy = np.stack([energy[stem] for stem in STEMS])
    for active_label, active_level in quiet.ACTIVE.items():
        active = source_rms >= active_level
        ratio = energy["recombined"][active] / source_rms[active]
        report["source_active_output_collapse"][active_label] = {
            "source_active_windows": int(active.sum()),
            "minimum_recombined_to_source_rms_ratio": float(ratio.min()) if len(ratio) else None,
            "recombined_below_one_tenth_source_spans": quiet.spans(active & (energy["recombined"] < .1 * source_rms)),
            "thresholds": {low_label: {
                "all_four_stems_below_threshold_spans": quiet.spans(active & np.all(stem_energy < low_level, axis=0)),
                "recombined_below_threshold_spans": quiet.spans(active & (energy["recombined"] < low_level))}
                for low_label, low_level in quiet.THRESHOLDS.items()}}
    report["final_100ms"] = {"start_seconds": 23.3, "end_seconds": 23.4,
        "source_rms_windows": source_rms[-5:].tolist(), "signal_rms_windows": {label: values[-5:].tolist() for label, values in energy.items()}}
    return report, energy


def composite(title, identifier, signals, gain, output, waveform):
    canvas = Image.new("RGB", (waveform.WIDTH, 1270), waveform.BG)
    draw = ImageDraw.Draw(canvas)
    heading = title + ": continuous source and Vulkan output"
    size = 30
    while size > 14 and draw.textbbox((0, 0), heading, font=waveform.font(size))[2] > waveform.WIDTH - 70:
        size -= 1
    draw.text((35, 20), heading, fill=waveform.FG, font=waveform.font(size))
    draw.text((35, 68), f"23.4 s per signal; common gain {gain:.6g}; no cuts, inserted gaps, fades or time shifts.", fill=waveform.FG, font=waveform.font(21))
    draw.text((35, 103), "L/R share +/-1 full scale. Orange: hop. Teal: overlap exit. Add 30 s for the original track timeline.", fill=waveform.FG, font=waveform.font(20))
    with tempfile.TemporaryDirectory(prefix="own-wave-panels-", dir=output) as temporary:
        for index, (label, signal) in enumerate(zip(LABELS, signals, strict=True)):
            top = 160 + index * 166
            values = np.asarray(signal * gain, dtype="<f4")
            draw.text((25, top + 30), label, fill=waveform.COLORS[index], font=waveform.font(23))
            energy_db = waveform.db(waveform.rms(values))
            draw.text((25, top + 64), "silence" if energy_db is None else f"{energy_db:.1f} dBFS RMS", fill=waveform.FG, font=waveform.font(17))
            layer = waveform.draw_wave(values, Path(temporary) / (label + ".png"), waveform.COLORS[index], 136)
            canvas.paste(layer, (waveform.LEFT, top), layer)
            layer.close()
            for boundary in range(HOP, FRAMES, HOP):
                for frame, color in ((boundary, "#ef9d58"), (boundary + OVERLAP, "#42b7b1")):
                    if frame >= FRAMES:
                        continue
                    x = waveform.LEFT + round(frame / FRAMES * waveform.PLOT_WIDTH)
                    for dash in range(top, top + 136, 10):
                        draw.line((x, dash, x, min(dash + 4, top + 136)), fill=color)
            draw.text((waveform.LEFT - 10, top + 16), "L", fill=waveform.FG, font=waveform.font(15), anchor="rt")
            draw.text((waveform.LEFT - 10, top + 84), "R", fill=waveform.FG, font=waveform.font(15), anchor="rt")
    waveform.axis(draw, 1165, FRAMES / RATE)
    path = output / (identifier + "-continuous-waveforms.png")
    canvas.save(path)
    canvas.close()
    return {**pin(path), "width": waveform.WIDTH, "height": 1270,
            "plot_method": "FFmpeg showwavespic stereo split, peak filter, shared linear +/-1 scale; same common gain as continuous WAVs, no per-stem normalization"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("manifest", "cpu-run", "vulkan-run", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--series-guard", type=Path, default=BASE / "series-guard.json")
    args = parser.parse_args()
    manifest_path, output = args.manifest.resolve(), args.output.resolve()
    require(not output.exists(), "Analysis output must be fresh")
    guard = load(args.series_guard)
    require(guard["status"] == "ok" and guard["idle_inhibit_released"] and guard.get("finished_at_utc")
            and not guard["errors"], "Owned series guard must finish before saved-output analysis")
    manifest = load(manifest_path)
    tracks = manifest["tracks"]
    ids = [track["id"] for track in tracks]
    require(len(ids) == 4 and len(set(ids)) == 4 and all(isinstance(identifier, str)
            and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", identifier) for identifier in ids), "Expected four unique safe manifest track IDs")
    run_paths = {"cpu": args.cpu_run.resolve() / "result.json", "vulkan": args.vulkan_run.resolve() / "result.json"}
    runs = {key: load(path) for key, path in run_paths.items()}
    common = ("runtime", "model", "worker_source", "shared_benchmark_source", "sampler_source", "driver")
    for backend, run in runs.items():
        require(run["status"] == "ok" and not run["failures"] and run["backend"] == backend and run["threads"] == 4,
                "Both backend runs must complete successfully")
        require(run["worker_statistics"]["closed"] and run["worker_statistics"]["backend_pid"] is None,
                "Native worker must close before analysis")
        require([row["id"] for row in run["tracks"]] == ids and run["pins"]["manifest"]["sha256"] == pin(manifest_path)["sha256"]
                and (run["window_frames"], run["hop_frames"], run["overlap_frames"]) == (WINDOW, HOP, OVERLAP), "Executed manifest/grid/track identities differ")
    require(all(runs["cpu"]["pins"][key]["sha256"] == runs["vulkan"]["pins"][key]["sha256"] for key in common), "CPU/Vulkan producer pins differ")
    require(pin(ROOT / "tools/stems/worker.py")["sha256"] == runs["cpu"]["pins"]["worker_source"]["sha256"], "Imported worker geometry source changed")
    modules, helper_pins = dependencies()
    metric, quiet, waveform = (modules[key] for key in ("metrics", "quiet", "waveform"))
    require((metric.RATE, metric.HOP, metric.OVERLAP, tuple(metric.STEMS)) == (RATE, HOP, OVERLAP, STEMS), "Proven metric geometry differs")
    check = selfchecks(metric, quiet)
    output.mkdir(parents=True)
    check.update(analysis_source=pin(__file__), dependency_sources=helper_pins, numpy_version=np.__version__)
    write(output / "metric-selfcheck.json", check)
    report = {"schema": 1, "status": "ok", "analyzed_at_utc": datetime.now(timezone.utc).isoformat(),
        "inputs": {"manifest": pin(manifest_path), "runs": {key: pin(path) for key, path in run_paths.items()},
                   "series_guard": pin(args.series_guard), "analysis_source": pin(__file__), "dependency_sources": helper_pins,
                   "worker_source": pin(ROOT / "tools/stems/worker.py"), "ffmpeg": pin("/bin/ffmpeg"), "font": pin(waveform.FONT)},
        "producer_pins": {key: runs["cpu"]["pins"][key] for key in common}, "versions": {"numpy": np.__version__, "pillow": PILLOW_VERSION},
        "sample_rate": RATE, "frames": FRAMES, "window_frames": WINDOW, "hop_frames": HOP, "overlap_frames": OVERLAP,
        "stem_order": list(STEMS), "rms_window_frames": BIN, "rms_window_seconds": .02,
        "low_energy_thresholds_linear": quiet.THRESHOLDS, "source_active_thresholds_linear": quiet.ACTIVE,
        "metric_definitions": {"errors": "Unshifted full-length float64 reductions; source PCM16/32768. CPU is numerical reference. No gain fitting or time shifting.",
            "lag": "Pinned helper: stereo-demeaned FFT cross correlation over +/-256 samples; positive means Vulkan later. Errors remain unshifted.",
            "joins": "Pinned helper: sample[b]-sample[b-1], +/-20ms derivative neighborhood at each kHOP and kHOP+OVERLAP within source.",
            "quiet": "Pinned 20ms complete-bin RMS/contiguous-span and exact stereo-zero functions. Descriptive thresholds are not quality acceptance gates.",
            "grid": "CPU and Vulkan use the same persistent worker fixed grid and normalization convention. No upstream full-track offline/grid equivalence is asserted."},
        "continuous_preview_method": "Six separate full23.4s signals per track: source, four Vulkan stems, recombined. No inserted gaps, fades, cuts, loops or time shifts. One common gain0.98/peak only if any signal peak exceeds1, otherwise1. Float32 WAV decode must reproduce exact input bytes.",
        "scope": "Saved local user-audio analysis only; no inference, playback, download or redistribution. No isolated references. CPU agreement, recombination, quiet spans and waveform plots cannot establish musical quality or audible seams.",
        "tracks": [], "human_listening_verdict": None}
    for index, raw in enumerate(tracks):
        identifier = raw["id"]
        source_path = Path(raw["input_wav"])
        source_path = source_path.resolve() if source_path.is_absolute() else (manifest_path.parent / source_path).resolve()
        source_pin, source = pin(source_path), metric.pcm16(source_path)
        require(source.shape == (FRAMES, 2) and np.isfinite(source).all(), "Source must have exactly four full hops")
        arrays, identities = {}, {}
        for backend, run in runs.items():
            saved = run["tracks"][index]
            require(saved["input"]["sha256"] == source_pin["sha256"] and saved["input"]["frames"] == FRAMES
                    and saved["continuous"]["start_frame"] == 0 and saved["continuous"]["frames"] == FRAMES, "Source/continuous coverage differs")
            info = saved["output"]
            identity = pin(info["path"])
            require(info["complete"] and identity["bytes"] == info["bytes"] and identity["sha256"] == info["sha256"], "Saved output changed/incomplete")
            array = np.load(info["path"], mmap_mode="r", allow_pickle=False)
            require(array.shape == (4, FRAMES, 2) and info["shape"] == list(array.shape)
                    and array.dtype == np.dtype("<f4") and info["dtype"] == "<f4" and np.isfinite(array).all(), "Output geometry/finite sample validation failed")
            arrays[backend], identities[backend] = array, identity
        source_rms = quiet.rms_windows(source)
        row = {"id": identifier, "annotations": raw["annotations"], "input": {**source_pin, "frames": FRAMES}, "outputs": identities,
            "geometry_finite_full_coverage_verified": True,
            "cpu_vulkan": {stem: {**metric.errors(arrays["cpu"][i], arrays["vulkan"][i]),
                                   "lag_diagnostic": metric.lag_diagnostic(arrays["cpu"][i], arrays["vulkan"][i])} for i, stem in enumerate(STEMS)},
            "recombination": {backend: metric.errors(source, np.sum(array, axis=0, dtype=np.float64)) for backend, array in arrays.items()},
            "source_waveform": {"raw": waveform.stats(source), "exact_zero": quiet.zero_summary(source),
                "rms_20ms_minimum": float(source_rms.min()), "rms_20ms_maximum": float(source_rms.max()),
                "low_energy_spans": {label: quiet.spans(source_rms < threshold) for label, threshold in quiet.THRESHOLDS.items()}},
            "backends": {}, "joins": [], "references": {"status": "not_supplied"}}
        energies = {}
        for backend, array in arrays.items():
            row["backends"][backend], energies[backend] = quiet_backend(source_rms, array, quiet, waveform)
        row["CPU_Vulkan_low_energy_mask_comparison"] = {label: {low: {
            "both_below_windows": int(((energies["cpu"][label] < level) & (energies["vulkan"][label] < level)).sum()),
            "CPU_only_below_spans": quiet.spans((energies["cpu"][label] < level) & (energies["vulkan"][label] >= level)),
            "Vulkan_only_below_spans": quiet.spans((energies["vulkan"][label] < level) & (energies["cpu"][label] >= level))}
            for low, level in quiet.THRESHOLDS.items()} for label in (*STEMS, "recombined")}
        for boundary in range(HOP, FRAMES, HOP):
            for kind, frame in (("hop", boundary), ("overlap_exit", boundary + OVERLAP)):
                if frame >= FRAMES:
                    continue
                row["joins"].append({"kind": kind, "frame": frame, "seconds": frame / RATE,
                    "source": metric.join_diagnostic(source, frame),
                    **{backend: {stem: metric.join_diagnostic(array[i], frame) for i, stem in enumerate(STEMS)} for backend, array in arrays.items()}})
        signals = [source, *arrays["vulkan"], np.sum(arrays["vulkan"], axis=0, dtype=np.float64)]
        peak = max(float(np.max(np.abs(signal))) for signal in signals)
        gain = .98 / peak if peak > 1 else 1.0
        row["continuous"] = {"frames": FRAMES, "seconds": FRAMES / RATE, "common_gain": gain,
                             "common_peak_before_gain": peak, "signals": {}}
        for label, signal in zip(LABELS, signals, strict=True):
            values = np.asarray(signal * gain, dtype="<f4")
            require(np.isfinite(values).all() and float(np.max(np.abs(values))) <= 1, "Continuous sample gain would clip")
            saved = waveform.wav(output / (identifier + "-" + label + ".wav"), values)
            saved.update(frames=FRAMES, sample_rate=RATE, channels=2,
                         float32_pcm_sha256=hashlib.sha256(values.tobytes()).hexdigest())
            row["continuous"]["signals"][label] = {"raw": waveform.stats(signal), "wav": saved}
        title = str(raw["annotations"].get("title", identifier))
        row["continuous"]["waveform_plot"] = composite(title, identifier, signals, gain, output, waveform)
        report["tracks"].append(row)
    report["limits"] = ["No numerical mismatch/quiet-collapse finding establishes musical acceptance or excludes shorter/partial damage.",
        "Per-stem low energy may mean absent instruments, routing or separation loss; no isolated reference is available.",
        "WAV byte roundtrip does not capture the user's player, audio server or device. No sounds are played here.",
        "Only per-signal continuous23.4s files are generated. The user must choose which signal to audition; there are no synthetic switches/gaps.",
        "All times refer to the prepared excerpt; add30s for the original decoded source timeline. User-provided audio carries no inferred redistribution license."]
    write(output / "result.json", report)
    print(json.dumps({"status": "ok", "output": str(output), "tracks": ids, "continuous_wavs": len(ids) * len(LABELS),
                      "composite_pngs": len(ids), "selfchecks": len(check["checks"]), "human_listening_verdict": None}))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Verify old audition assembly; render FFmpeg waveforms and continuous WAVs.

Uses saved outputs only. No inference, playback, download or production edits.
"""
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import subprocess
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFont

BASE = Path(__file__).resolve().parent
OUT = BASE / "waveform-review"
RATE, BIN = 44100, 882
STEMS = ("drums", "bass", "other", "vocals")
LABELS = ("source", *STEMS, "recombined")
FONT = "/usr/share/fonts/noto/NotoSans-Regular.ttf"
WIDTH, LEFT, RIGHT = 1800, 182, 40
PLOT_WIDTH = WIDTH - LEFT - RIGHT
COLORS = ("#79b8ff", "#ffb36b", "#cf9aff", "#6ee7b7", "#f9a8d4", "#7dd3fc")
BG, FG = "#101820", "#e9eff4"


def pin(path):
    path = Path(path).resolve()
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest}


def require(test, message):
    if not test:
        raise ValueError(message)


def ffmpeg(args, data=None):
    return subprocess.run(["/bin/ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", *args],
                          input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=True, timeout=60).stdout


def decode(path):
    raw = ffmpeg(["-i", str(path), "-f", "f32le", "-acodec", "pcm_f32le", "-ar", str(RATE), "-ac", "2", "pipe:1"])
    values = np.frombuffer(raw, dtype="<f4").reshape(-1, 2)
    require(np.isfinite(values).all(), "Nonfinite decoded audio")
    return values


def wav(path, values):
    raw = np.asarray(values, dtype="<f4").tobytes()
    ffmpeg(["-y", "-f", "f32le", "-ar", str(RATE), "-ac", "2", "-i", "pipe:0",
            "-codec:a", "pcm_f32le", str(path)], raw)
    require(decode(path).tobytes() == raw, "Continuous WAV roundtrip changed samples")
    return {**pin(path), "codec": "pcm_f32le", "exact_float32_roundtrip": True}


def rms(values):
    return math.sqrt(float(np.mean(np.square(np.asarray(values, dtype=np.float64)))))


def db(value):
    return 20 * math.log10(value) if value > 0 else None


def bins(values):
    return np.array([rms(values[i:i + BIN]) for i in range(0, len(values), BIN)])


def stats(values):
    energy = rms(values)
    return {"rms": energy, "rms_dbfs": db(energy), "peak": float(np.max(np.abs(values)))}


def font(size):
    return ImageFont.truetype(FONT, size)


def draw_wave(values, path, color, height):
    raw = np.asarray(values, dtype="<f4").tobytes()
    # Stereo split; shared linear +/-1 scale. Peak filtering retains transients.
    ffmpeg(["-y", "-f", "f32le", "-ar", str(RATE), "-ac", "2", "-i", "pipe:0",
            "-filter_complex", f"showwavespic=s={PLOT_WIDTH}x{height}:split_channels=1:colors={color}|{color}:scale=lin:draw=full:filter=peak",
            "-frames:v", "1", "-threads", "1", str(path)], raw)
    return Image.open(path).convert("RGBA")


def axis(draw, y, duration):
    for seconds in np.arange(0, duration + 1e-6, 2):
        x = LEFT + round(seconds / duration * PLOT_WIDTH)
        if x > WIDTH - RIGHT - 80:
            continue  # Leave room for the final duration label.
        draw.line((x, y, x, y + 6), fill="#718293", width=1)
        draw.text((x, y + 8), f"{seconds:g}", fill=FG, font=font(16), anchor="mt")
    draw.text((WIDTH - RIGHT, y + 8), f"{duration:g} s", fill=FG, font=font(16), anchor="rt")


def main():
    require(not OUT.exists(), "Use a fresh output directory")
    OUT.mkdir()
    manifest = json.loads((BASE / "manifest.json").read_text())
    analysis = json.loads((BASE / "analysis/result.json").read_text())
    gpu = json.loads((BASE / "vulkan/result.json").read_text())
    report = {"schema": 1, "at_utc": datetime.now(timezone.utc).isoformat(), "status": "ok",
              "scope": "Saved source and Vulkan output only; no new inference or playback. Original previews remain unchanged.",
              "inputs": {"manifest": pin(BASE / "manifest.json"), "analysis": pin(BASE / "analysis/result.json"),
                         "vulkan": pin(BASE / "vulkan/result.json"), "numerical_report": pin(BASE / "waveform-numerics.json"),
                         "source": pin(__file__), "ffmpeg": pin("/bin/ffmpeg"), "font": pin(FONT)},
              "plot_method": "FFmpeg showwavespic: stereo split, peak filter, linear +/-1 full scale shared across rows. Pillow composes labels/axes; no independent per-stem normalization.",
              "continuous_assets": "23.4s per signal; no inserted silence, cuts, fades or time shifts. One common attenuation across all six signals only if any peak exceeds1. Float32 WAV samples round-trip exactly.",
              "tracks": [], "limits": ["Waveform pixels and RMS thresholds cannot establish musical acceptance or exclude sub-bin/partial damage.",
              "No isolated ground-truth stems exist here. Quiet isolated output can mean absent instruments, routing, or separation loss.",
              "No browser/player/device playback is captured by these saved-file checks."]}
    preview_image = Image.new("RGB", (WIDTH, 1030), BG)
    pd = ImageDraw.Draw(preview_image)
    pd.text((35, 22), "Original auditions: six different signals, with inserted gaps", fill=FG, font=font(31))
    pd.text((35, 73), "Each section replays the same clip 3.35-8.35 s. L/R waveforms share +/-1 scale; shaded gaps are exactly zero in WAV.", fill=FG, font=font(20))
    for number, raw in enumerate(manifest["tracks"]):
        identifier, title = raw["id"], raw["annotations"]["title"]
        saved = gpu["tracks"][number]
        source_path = Path(raw["input_wav"])
        require(saved["id"] == identifier and pin(source_path)["sha256"] == saved["input"]["sha256"], "Source identity mismatch")
        with wave.open(str(source_path), "rb") as stream:
            require(stream.getparams()[:3] == (2, 2, RATE), "Source geometry mismatch")
            source = np.frombuffer(stream.readframes(stream.getnframes()), "<i2").reshape(-1, 2).astype(np.float64) / 32768
        output_path = Path(saved["output"]["path"])
        require(pin(output_path)["sha256"] == saved["output"]["sha256"], "Output identity mismatch")
        stems = np.load(output_path, mmap_mode="r", allow_pickle=False)
        signals = [source, *stems, np.sum(stems, axis=0, dtype=np.float64)]
        require(stems.shape == (4, len(source), 2) and np.isfinite(stems).all(), "Invalid raw array")
        audition = analysis["tracks"][number]["audition"]
        old_path = Path(audition["lossless_wav"]["path"])
        require(pin(old_path)["sha256"] == audition["lossless_wav"]["sha256"], "Old audition changed")
        old, old_mp3 = decode(old_path), decode(audition["path"])
        require(len(old) == audition["preview_pcm_frames"] and len(old_mp3) == len(old), "Decoded audition geometry mismatch")
        assembled, sections, gaps = [], [], []
        for i, segment in enumerate(audition["segments"]):
            start, stop = segment["source_frame_interval"]
            expected = np.array(signals[i][start:stop], dtype=np.float64, copy=True)
            fade_n = audition["edge_fade_frames_per_segment"]
            fade = np.linspace(0, 1, fade_n)
            expected[:fade_n] *= fade[:, None]
            expected[-fade_n:] *= fade[::-1, None]
            expected = np.asarray(expected * audition["common_gain"], dtype="<f4")
            a, b = round(segment["preview_start_seconds"] * RATE), round(segment["preview_end_seconds"] * RATE)
            require(np.array_equal(old[a:b], expected), "Old section differs from raw source/stem plus recorded edge fade")
            assembled.append(expected)
            sections.append({**segment, "decoded_wav_matches_raw_with_recorded_edge_fade_and_gain": True,
                             "decoded_wav": stats(old[a:b]), "decoded_mp3": stats(old_mp3[a:b])})
            if i < len(LABELS) - 1:
                silence = np.zeros((audition["gap_frames"], 2), dtype="<f4")
                require(np.array_equal(old[b:b+len(silence)], silence), "Declared audition gap is not exact zero")
                gaps.append({"start_seconds": b / RATE, "end_seconds": (b + len(silence)) / RATE,
                             "frames": len(silence), "both_channels_exact_zero": True})
                assembled.append(silence)
        require(np.array_equal(np.concatenate(assembled), old), "Whole audition differs from declared assembly")
        expected_rms, mp3_rms = bins(old), bins(old_mp3)
        collapse = (expected_rms >= .01) & (mp3_rms < .001)
        common_peak = max(float(np.max(np.abs(signal))) for signal in signals)
        gain = .98 / common_peak if common_peak > 1 else 1.0
        track = {"id": identifier, "title": title, "original_preview": pin(old_path),
                 "source": pin(source_path), "raw_vulkan": pin(output_path), "old_sections": sections, "inserted_gaps": gaps,
                 "old_lossless_preview_matches_declared_assembly_exactly": True,
                 "mp3_decode_check": {"window_frames": BIN, "expected_active_rms": .01, "collapse_rms": .001,
                                      "active_20ms_windows_collapsing_below_threshold": int(collapse.sum()),
                                      "collapsed_window_starts_seconds": (np.flatnonzero(collapse)*BIN/RATE).tolist()},
                 "continuous_common_gain": gain, "continuous_peak_before_gain": common_peak, "signals": {}}
        for label, signal in zip(LABELS, signals):
            track["signals"][label] = {"raw": stats(signal), "wav": wav(OUT / f"{identifier}-{label}.wav", signal * gain)}
        y = 145 + number * 280
        pd.text((35, y), title, fill=FG, font=font(25))
        for i, segment in enumerate(audition["segments"]):
            x1 = LEFT + round(segment["preview_start_seconds"] / 31.25 * PLOT_WIDTH)
            x2 = LEFT + round(segment["preview_end_seconds"] / 31.25 * PLOT_WIDTH)
            pd.text(((x1+x2)//2, y + 40), segment["label"], fill=COLORS[i], font=font(20), anchor="mt")
        layer = draw_wave(old, OUT / f"{identifier}-old-preview-wave.png", "#d9e8f5", 130)
        preview_image.paste(layer, (LEFT, y + 76), layer)
        for gap in gaps:
            x1 = LEFT + round(gap["start_seconds"] / 31.25 * PLOT_WIDTH)
            x2 = LEFT + round(gap["end_seconds"] / 31.25 * PLOT_WIDTH)
            pd.rectangle((x1, y+76, x2, y+206), fill="#975b2c")
        pd.text((LEFT-12, y+90), "L", fill=FG, font=font(16), anchor="rt")
        pd.text((LEFT-12, y+155), "R", fill=FG, font=font(16), anchor="rt")
        axis(pd, y+208, 31.25)
        full = Image.new("RGB", (WIDTH, 1270), BG)
        fd = ImageDraw.Draw(full)
        fd.text((35, 20), f"{title}: continuous saved source and Vulkan output", fill=FG, font=font(30))
        fd.text((35, 68), "23.4 s, no audition switches or inserted gaps. L/R share +/-1 scale; quiet stems are not amplified.", fill=FG, font=font(21))
        fd.text((35, 103), "Orange: hop boundaries (5.85 s). Teal: overlap exits (+1.95 s). Times start at original track 30.0 s.", fill=FG, font=font(20))
        for i, (label, signal) in enumerate(zip(LABELS, signals)):
            top = 160 + i*166
            fd.text((25, top+30), label, fill=COLORS[i], font=font(23))
            fd.text((25, top+64), f"{db(rms(signal)):.1f} dBFS RMS", fill=FG, font=font(17))
            layer = draw_wave(signal, OUT / f"{identifier}-{label}-wave.png", COLORS[i], 136)
            full.paste(layer, (LEFT, top), layer)
            for boundary in (5.85, 11.7, 17.55):
                for value, color in ((boundary,"#ef9d58"), (boundary+1.95,"#42b7b1")):
                    x = LEFT + round(value / 23.4 * PLOT_WIDTH)
                    for dash in range(top, top+136, 10):
                        fd.line((x, dash, x, min(dash+4, top+136)), fill=color)
            fd.text((LEFT-10, top+16), "L", fill=FG, font=font(15), anchor="rt")
            fd.text((LEFT-10, top+84), "R", fill=FG, font=font(15), anchor="rt")
        axis(fd, 1165, 23.4)
        full_path = OUT / f"{identifier}-continuous-waveforms.png"
        full.save(full_path)
        track["continuous_waveform_plot"] = pin(full_path)
        report["tracks"].append(track)
    pd.text((35, 990), "Orange bands: five intentional 0.25 s gaps per audition. Switching to a weak stem can also sound like a cutout.", fill=FG, font=font(20))
    preview_path = OUT / "original-preview-layout.png"
    preview_image.save(preview_path)
    report["original_preview_layout_plot"] = pin(preview_path)
    with (OUT / "result.json").open("x") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"output": str(OUT), "tracks": len(report["tracks"]), "continuous_wavs": 18,
                      "all_old_wav_assemblies_exact": True,
                      "mp3_active_window_collapses": [row["mp3_decode_check"]["active_20ms_windows_collapsing_below_threshold"] for row in report["tracks"]]}))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Freeze four local FLAC identities, then prepare only the declared WAV excerpts."""
import argparse
from datetime import datetime, timezone
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import wave

import numpy as np

BASE = Path(__file__).resolve().parent
SOURCE = Path("/home/kyau/temp/tracks")
RATE, OFFSET, FRAMES = 44100, 1323000, 1031940
PLAN = BASE / "excerpt-plan.json"


def utc():
    return datetime.now(timezone.utc).isoformat()


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest}


def signature(path):
    st = path.stat()
    return {"device": st.st_dev, "inode": st.st_ino, "bytes": st.st_size,
            "mtime_ns": st.st_mtime_ns, "ctime_ns": st.st_ctime_ns}


def write_json(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")


def decode_command(path):
    # Crop on the converted 44.1 kHz timeline; no input seek or full-track copy.
    filters = ("aformat=sample_fmts=flt:sample_rates=44100:channel_layouts=stereo,"
               f"atrim=start_sample={OFFSET}:end_sample={OFFSET + FRAMES},asetpts=PTS-STARTPTS")
    return ["/bin/ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(path),
            "-map", "0:a:0", "-vn", "-sn", "-dn", "-af", filters, "-t", "23.4",
            "-ac", "2", "-ar", str(RATE), "-acodec", "pcm_f32le", "-f", "f32le", "pipe:1"]


def declare():
    for name in ("excerpt-plan.json", "manifest.json", "corpus-provenance.json", "assets"):
        if (BASE / name).exists():
            raise FileExistsError("Preparation-owned output must be fresh: " + name)
    files = sorted((p for p in SOURCE.iterdir() if p.is_file() and p.suffix.lower() == ".flac"),
                   key=lambda p: p.name.casefold())
    if len(files) != 4 or any(p.is_symlink() for p in files):
        raise ValueError("Expected exactly four regular, non-symlink FLAC files")
    tracks, ids = [], set()
    for path in files:
        before = signature(path)
        command = ["/bin/ffprobe", "-v", "error", "-show_format", "-show_streams", "-of", "json", str(path)]
        probe = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
        metadata = json.loads(probe.stdout)
        audio = [s for s in metadata.get("streams", []) if s.get("codec_type") == "audio"]
        if len(audio) != 1 or audio[0].get("codec_name") != "flac":
            raise ValueError("Expected exactly one FLAC audio stream: " + path.name)
        stream = audio[0]
        rate, channels = int(stream["sample_rate"]), int(stream["channels"])
        bits = int(stream.get("bits_per_raw_sample") or stream.get("bits_per_sample") or 0)
        if rate <= 0 or channels <= 0 or bits <= 0:
            raise ValueError("Invalid FLAC rate/channels/bit depth: " + path.name)
        if "duration_ts" in stream and stream.get("time_base"):
            duration = int(stream["duration_ts"]) * Fraction(stream["time_base"])
            duration_source = "audio duration_ts * time_base"
        else:
            value = stream.get("duration") or metadata.get("format", {}).get("duration")
            duration = Fraction(str(value))
            duration_source = "audio duration" if stream.get("duration") else "format duration"
        if duration < Fraction(534, 10):
            raise ValueError("Source is too short for 30.0–53.4 s: " + path.name)
        identity = pin(path)
        if before != signature(path):
            raise RuntimeError("Source changed during metadata/hash inspection: " + path.name)
        identifier = re.sub(r"[^a-z0-9]+", "_", path.stem.lower()).strip("_")[:96].rstrip("_")
        if not identifier or identifier in ids:
            raise ValueError("Empty/colliding filename-derived track id")
        ids.add(identifier)
        tracks.append({"id": identifier, "source_filename": path.name, "source": identity,
                       "source_stat": before, "ffprobe_command": command, "ffprobe_stderr": probe.stderr,
                       "ffprobe": metadata, "audio_metadata": {"codec": stream["codec_name"],
                       "format_name": metadata.get("format", {}).get("format_name"), "sample_rate": rate,
                       "channels": channels, "bits_per_sample": stream.get("bits_per_sample"),
                       "bits_per_raw_sample": stream.get("bits_per_raw_sample"), "resolved_bit_depth": bits,
                       "duration_seconds": float(duration), "duration_fraction": str(duration),
                       "duration_source": duration_source}, "decode_command": decode_command(path),
                       "input_wav": str(BASE / "assets" / (identifier + "-30s-23p4s.wav"))})
    plan = {"schema": 1, "status": "declared_before_decoding", "declared_at_utc": utc(),
            "preparation_source": pin(Path(__file__).resolve()), "source_directory": str(SOURCE),
            "scope": "Four user-provided mixtures; filenames are user-provided labels, embedded tags unverified. No license or isolated-reference-stem claim. No downloads, inference or playback.",
            "source_offset_seconds": 30.0, "source_offset_frames": OFFSET, "duration_seconds": 23.4,
            "excerpt_frames": FRAMES, "frames": FRAMES, "sample_rate": RATE, "rate": RATE,
            "channels": 2, "sample_bytes": 2, "looping": False,
            "crop_timeline": "After ffmpeg conversion to stereo 44100 Hz, half-open frames [1323000,2354940); no input seek",
            "normalization": "Common stereo gain 0.95/decoded_excerpt_peak only when peak > 1.0; otherwise 1.0",
            "quantization": "numpy.rint(float64(decoded_float32) * common_stereo_gain * 32767).astype('<i2'); reject clipping",
            "storage": "Only four prepared WAV excerpts; no full-track copies or saved decoded full tracks. Original paths, tags and excerpts stay local.",
            "decode_timeout_seconds_per_track": 120, "tracks": tracks}
    write_json(PLAN, plan)
    print(json.dumps({"status": plan["status"], "plan": pin(PLAN), "tracks": [
        {"id": t["id"], "filename": t["source_filename"], "source": t["source"],
         "audio_metadata": t["audio_metadata"]} for t in tracks]}, ensure_ascii=False), flush=True)


def prepare():
    if any((BASE / name).exists() for name in ("assets", "manifest.json", "corpus-provenance.json")):
        raise FileExistsError("Preparation-owned assets/manifest/provenance must be fresh")
    plan = json.loads(PLAN.read_text())
    if (plan["status"], plan["source_offset_frames"], plan["excerpt_frames"], plan["sample_rate"],
            plan["channels"], plan["sample_bytes"], len(plan["tracks"])) != (
            "declared_before_decoding", OFFSET, FRAMES, RATE, 2, 2, 4):
        raise ValueError("Declared fixed geometry changed")
    if plan["preparation_source"] != pin(Path(__file__).resolve()):
        raise RuntimeError("Preparation helper changed after declaration")
    for track in plan["tracks"]:
        path = Path(track["source"]["path"])
        if signature(path) != track["source_stat"] or pin(path) != track["source"]:
            raise RuntimeError("Original source changed after declaration")
        if track["decode_command"] != decode_command(path):
            raise RuntimeError("Declared decode command changed")
    provenance = {"schema": 1, "status": "preparing", "started_at_utc": utc(), "excerpt_plan": pin(PLAN),
                  "preparation_source": pin(Path(__file__).resolve()), "tracks": [],
                  "scope": plan["scope"], "audio_download_bytes": 0, "full_track_copies": 0,
                  "ffmpeg_version": subprocess.run(["/bin/ffmpeg", "-version"], check=True, capture_output=True, text=True, timeout=10).stdout,
                  "numpy_version": np.__version__, "originals_unchanged": False}
    manifest = {"schema": 1, "excerpt_plan": {k: v for k, v in plan.items() if k != "tracks"}, "tracks": []}
    (BASE / "assets").mkdir()
    try:
        for track in plan["tracks"]:
            item = dict(track)
            item["status"] = "decoding"
            provenance["tracks"].append(item)
            began = time.monotonic()
            decoded = subprocess.run(track["decode_command"], check=True, capture_output=True, timeout=120)
            if len(decoded.stdout) != FRAMES * 2 * 4:
                raise ValueError("Decoded excerpt is not exactly 1031940 stereo float32 frames")
            excerpt = np.frombuffer(decoded.stdout, dtype="<f4").reshape(FRAMES, 2)
            if not np.isfinite(excerpt).all():
                raise ValueError("Decoded excerpt contains nonfinite samples")
            peak = float(np.max(np.abs(excerpt)))
            gain = 0.95 / peak if peak > 1.0 else 1.0
            scaled = excerpt.astype(np.float64) * gain
            quantized = np.rint(scaled * 32767)
            if (not np.isfinite(quantized).all() or np.max(np.abs(scaled)) > 1.0
                    or quantized.min() < -32768 or quantized.max() > 32767):
                raise ValueError("PCM16 conversion would clip")
            pcm = quantized.astype("<i2").tobytes()
            destination = Path(track["input_wav"])
            if destination != BASE / "assets" / (track["id"] + "-30s-23p4s.wav"):
                raise ValueError("Declared output escapes assets")
            with destination.open("xb") as stream:
                with wave.open(stream, "wb") as output:
                    output.setparams((2, 2, RATE, FRAMES, "NONE", "not compressed"))
                    output.writeframes(pcm)
            with wave.open(str(destination), "rb") as check:
                if check.getparams()[:4] != (2, 2, RATE, FRAMES) or check.readframes(FRAMES + 1) != pcm:
                    raise RuntimeError("Saved WAV geometry/PCM differs from conversion")
            item["decode"] = {"command": track["decode_command"], "stderr": decoded.stderr.decode("utf-8", "replace"),
                              "elapsed_seconds": time.monotonic() - began, "float32_bytes": len(decoded.stdout),
                              "decoded_excerpt_float32_sha256": hashlib.sha256(decoded.stdout).hexdigest(),
                              "decoded_excerpt_peak": peak, "common_stereo_gain": gain, "gain_required": peak > 1.0,
                              "scaled_excerpt_peak": float(np.max(np.abs(scaled))), "pcm16_min": int(quantized.min()),
                              "pcm16_max": int(quantized.max()), "pcm16_sha256": hashlib.sha256(pcm).hexdigest(),
                              "source_frame_offset": OFFSET, "source_frame_end_exclusive": OFFSET + FRAMES,
                              "frames": FRAMES, "source_offset_seconds": 30.0, "duration_seconds": 23.4,
                              "sample_rate": RATE, "channels": 2, "sample_bytes": 2, "quantization": plan["quantization"]}
            item["prepared_wav"] = {**pin(destination), "frames": FRAMES, "sample_rate": RATE, "channels": 2, "sample_bytes": 2}
            source = Path(track["source"]["path"])
            if signature(source) != track["source_stat"] or pin(source) != track["source"]:
                raise RuntimeError("Original changed during preparation")
            item["original_unchanged"] = True
            item["status"] = "ready"
            manifest["tracks"].append({"id": track["id"], "input_wav": str(destination),
                "input_pin": item["prepared_wav"], "annotations": {"source_kind": "User-provided FLAC mixture",
                "source_filename": track["source_filename"], "source_sha256": track["source"]["sha256"],
                "source_labels": "Filename supplied by user; artist/title and embedded tags not independently verified",
                "license": "Not specified by user; no redistribution or CC-license assertion",
                "excerpt_source_offset_seconds": 30.0, "excerpt_frames": FRAMES, "excerpt_seconds": 23.4,
                "common_stereo_gain": gain, "reference_stems_available": False, "human_listening_required": True}})
            print(json.dumps({"id": track["id"], "status": "ready", "wav": item["prepared_wav"],
                              "peak": peak, "gain": gain}), flush=True)
        if len(manifest["tracks"]) != 4:
            raise RuntimeError("Not all four sources prepared")
        for track in plan["tracks"]:
            path = Path(track["source"]["path"])
            if signature(path) != track["source_stat"] or pin(path) != track["source"]:
                raise RuntimeError("Original changed before manifest publication")
        write_json(BASE / "manifest.json", manifest)
        provenance.update(status="ready", originals_unchanged=True, manifest=pin(BASE / "manifest.json"))
    except BaseException as error:
        provenance.update(status="failed", failure={"type": type(error).__name__, "message": str(error)})
        raise
    finally:
        provenance["finished_at_utc"] = utc()
        write_json(BASE / "corpus-provenance.json", provenance)
    print(json.dumps({"status": "ready", "manifest": provenance["manifest"],
                      "provenance": pin(BASE / "corpus-provenance.json")}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("plan", "prepare"))
    args = parser.parse_args()
    declare() if args.phase == "plan" else prepare()

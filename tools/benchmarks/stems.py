#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Measure audio.cpp separation, including WAV input/output and process startup."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import signal
import struct
import subprocess
import time
import wave


def fixture(path, seconds):
    """Create permitted synthetic stereo material; this is not a quality corpus."""
    rate = 44100
    rng = random.Random(0)
    frames = round(seconds * rate)
    data = bytearray()
    for i in range(frames):
        t = i / rate
        beat = t % 0.5
        bass = 0.18 * math.sin(2 * math.pi * 55 * t)
        kick = 0.25 * math.exp(-beat * 35) * math.sin(
            2 * math.pi * (80 * beat + 1.5 * (1 - math.exp(-beat * 30))))
        hat = 0.045 * (rng.random() * 2 - 1) * math.exp(-(t % 0.25) * 70)
        envelope = 0.65 + 0.35 * math.sin(2 * math.pi * 3 * t)
        common = bass + kick + hat + 0.06 * math.sin(2 * math.pi * 660 * t)
        left = common + 0.10 * math.sin(2 * math.pi * 440 * t) * envelope
        right = common + 0.095 * math.sin(2 * math.pi * 443 * t) * envelope
        data.extend(struct.pack("<hh", round(left * 32767), round(right * 32767)))
    with wave.open(str(path), "wb") as stream:
        stream.setparams((2, 2, rate, frames, "NONE", "not compressed"))
        stream.writeframes(data)
    return frames / rate


def measure(command, output, timeout):
    start = time.monotonic()
    started_at = datetime.now(timezone.utc).isoformat()
    with output.with_suffix(".stdout").open("wb") as stdout, \
            output.with_suffix(".stderr").open("wb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                   start_new_session=True)
        timed_out = False
        while True:
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                process.returncode = os.waitstatus_to_exitcode(status)
                break
            if time.monotonic() - start > timeout:
                timed_out = True
                os.killpg(process.pid, signal.SIGKILL)
                _, status, usage = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
                break
            time.sleep(0.02)
    return {
        "command": command,
        "started_at_utc": started_at,
        "returncode": process.returncode,
        "timed_out": timed_out,
        "wall_seconds": time.monotonic() - start,
        "user_seconds": usage.ru_utime,
        "system_seconds": usage.ru_stime,
        "peak_rss_kib": usage.ru_maxrss,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--backend", choices=("cpu", "vulkan"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=5.85)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--batch-hops", type=int, default=1,
                        help="requests per persistent session; each uses the same fixture")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    if min(args.seconds, args.runs, args.batch_hops, args.threads, args.timeout) <= 0:
        parser.error("duration, runs, threads and timeout must be positive")
    cli, model, output = args.cli.resolve(), args.model.resolve(), args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("--output must be a new or empty directory")
    output.mkdir(parents=True, exist_ok=True)
    audio = output / "synthetic.wav"
    duration = fixture(audio, args.seconds)
    result = {
        "runtime_version": subprocess.check_output([str(cli), "--version"], text=True),
        "model_sha256": hashlib.file_digest(model.open("rb"), "sha256").hexdigest(),
        "fixture_sha256": hashlib.file_digest(audio.open("rb"), "sha256").hexdigest(),
        "fixture_seconds": duration,
        "sample_rate": 44100,
        "channels": 2,
        "backend": args.backend,
        "threads": args.threads,
        "batch_hops": args.batch_hops,
        "runs": [],
    }
    for index in range(args.runs):
        prefix = output / f"run-{index + 1}"
        stems = prefix / "stems"
        command = [str(cli), "--task", "sep", "--family", "htdemucs",
                   "--model", str(model), "--backend", args.backend,
                   "--threads", str(args.threads),
                   "--out-dir", str(stems), "--log-file", str(prefix) + ".profile.log",
                   "--metrics"]
        if args.batch_hops > 1:
            inputs = prefix / "inputs"
            inputs.mkdir(parents=True, exist_ok=True)
            for hop in range(args.batch_hops):
                destination = inputs / f"hop-{hop + 1:04}.wav"
                os.link(audio, destination)
            command.extend(["--batch-audio-dir", str(inputs)])
        else:
            command.extend(["--audio", str(audio)])
        measured = measure(command, prefix, args.timeout)
        measured["process_rtf"] = measured["wall_seconds"] / (duration * args.batch_hops)
        measured["output_files"] = [str(p) for p in sorted(stems.rglob("*.wav"))]
        expected = (44100, 2, round(duration * 44100), 2)
        formats = set()
        for path in measured["output_files"]:
            with wave.open(path, "rb") as stream:
                formats.add((stream.getframerate(), stream.getnchannels(),
                             stream.getnframes(), stream.getsampwidth()))
        measured["output_validation"] = {
            "expected_rate_channels_frames_sample_bytes": expected,
            "observed_formats": sorted(formats),
            "passed": len(measured["output_files"]) == 4 * args.batch_hops
                      and formats == {expected},
        }
        requests = [float(value) for value in re.findall(
            r"metrics(?:\[[^\]]+\])?\.wall_ms=([0-9.eE+-]+)",
            prefix.with_suffix(".stdout").read_text())]
        measured["session_request_ms"] = requests
        if requests:
            ordered = sorted(requests)
            measured["session_request_percentiles_ms"] = {
                str(p): ordered[math.ceil(len(ordered) * p / 100) - 1]
                for p in (50, 95, 99)
            }
        result["runs"].append(measured)
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(measured), flush=True)
        if measured["returncode"] or not measured["output_validation"]["passed"]:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

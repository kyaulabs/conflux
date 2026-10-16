# SPDX-License-Identifier: AGPL-3.0-or-later
"""Synthetic input and Linux process sampling for the M0 worker benchmark."""

import hashlib
import math
import os
from pathlib import Path
import re
import threading
import time
import wave

import numpy as np


def create_fixture(path, seconds, *, seed=0, block_frames=44100):
    """Write bounded-memory permitted test audio, not a separation-quality corpus."""
    if not math.isfinite(seconds) or seconds <= 0 or block_frames <= 0:
        raise ValueError("duration and block size must be positive")
    path = Path(path)
    rate = 44100
    frames = round(seconds * rate)
    if frames < 2:
        raise ValueError("fixture must contain at least two frames")
    change_frame = frames // 2
    change_seconds = change_frame / rate
    rng = np.random.default_rng(seed)
    melody = np.array([440.0, 523.251131, 659.255114, 587.329536])
    peak = 0.0
    with wave.open(str(path), "wb") as stream:
        stream.setparams((2, 2, rate, frames, "NONE", "not compressed"))
        for start in range(0, frames, block_frames):
            positions = np.arange(start, min(start + block_frames, frames))
            t = positions / rate
            bpm = np.where(positions < change_frame, 120.0, 138.0)
            beats = np.minimum(t, change_seconds) * 2.0
            beats += np.maximum(t - change_seconds, 0.0) * (138.0 / 60.0)
            beat_age = np.remainder(beats, 1.0) * 60.0 / bpm
            half_beat_age = np.remainder(beats, 0.5) * 60.0 / bpm
            bass = 0.17 * np.sin(2 * np.pi * 55 * t)
            kick = 0.24 * np.exp(-beat_age * 35) * np.sin(
                2 * np.pi * (80 * beat_age + 1.5 * (1 - np.exp(-beat_age * 30))))
            noise = rng.uniform(-1.0, 1.0, (len(t), 2))
            hats = 0.035 * noise * np.exp(-half_beat_age[:, None] * 70)
            note = melody[(t // 2).astype(np.int64) % len(melody)]
            envelope = 0.65 + 0.35 * np.sin(2 * np.pi * 3 * t)
            click_age = np.remainder(t - 0.125, 7.0)
            click = 0.12 * np.exp(-click_age * 1200)
            left = bass + kick + hats[:, 0] + click
            left += 0.10 * np.sin(2 * np.pi * note * t) * envelope
            right = bass + kick + hats[:, 1] - click * 0.75
            right += 0.095 * np.sin(2 * np.pi * note * 1.0068 * t) * envelope
            stereo = np.column_stack((left, right))
            peak = max(peak, float(np.max(np.abs(stereo))))
            stream.writeframesraw(np.rint(stereo * 32767).astype("<i2").tobytes())
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {
        "path": str(path.resolve()),
        "sha256": digest,
        "frames": frames,
        "seconds": frames / rate,
        "sample_rate": rate,
        "channels": 2,
        "sample_bytes": 2,
        "seed": seed,
        "generator": "conflux-m0-worker-fixture-v1",
        "numpy_version": np.__version__,
        "unquantized_peak_absolute": peak,
        "annotations": {
            "tempo_map": [{"frame": 0, "bpm": 120},
                          {"frame": change_frame, "bpm": 138}],
            "transient_frames": [round(t * rate) for t in
                                 np.arange(0.125, frames / rate, 7.0)],
            "melody_change_interval_frames": 2 * rate,
            "description": "Synthetic bass, kick, noise hats, changing tones and asymmetric clicks.",
            "quality_corpus": False,
        },
    }


def _read_process(pid):
    directory = Path("/proc") / str(pid)
    if directory.stat().st_uid != os.getuid():
        raise PermissionError("only the current user's processes may be sampled")
    status = {}
    for line in (directory / "status").read_text().splitlines():
        key, _, value = line.partition(":")
        if key in ("VmRSS", "VmHWM"):
            status[key] = int(value.split()[0])
    # comm may contain spaces or parentheses, so split after its final ')'.
    stat = (directory / "stat").read_text().rsplit(")", 1)[1].split()
    cpu_ticks = int(stat[11]) + int(stat[12])
    start_ticks = int(stat[19])
    clients = {}
    fd_errors = 0
    for fd in (directory / "fdinfo").iterdir():
        try:
            fields = {}
            for line in fd.read_text().splitlines():
                key, _, value = line.partition(":")
                if key.startswith("drm-"):
                    fields[key] = value.strip()
            if "drm-client-id" not in fields:
                continue
            device = fields.get("drm-pdev", fields.get("drm-driver", "unknown"))
            identity = (device, fields["drm-client-id"])
            # Multiple file descriptors can describe the same DRM client.
            clients.setdefault(identity, fields)
        except (OSError, ValueError):
            fd_errors += 1
    return {"pid": pid, "start_ticks": start_ticks,
            "process_status_kB": status, "cpu_ticks": cpu_ticks,
            "drm_clients": list(clients.values()), "fd_read_errors": fd_errors}


def _update_drm_maximum(maxima, field, raw):
    numeric = re.fullmatch(r"(\d+)\s+(\S+)", raw)
    observation = {"value": int(numeric[1]), "unit": numeric[2]} if numeric else raw
    if field not in maxima:
        maxima[field] = observation
        return
    previous = maxima[field]
    observations = (previous["observations"] if isinstance(previous, dict)
                    and "observations" in previous else [previous])
    for existing in observations:
        if isinstance(observation, dict) and isinstance(existing, dict):
            if existing["unit"] == observation["unit"]:
                existing["value"] = max(existing["value"], observation["value"])
                return
        elif existing == observation:
            return
    # A field can change from a raw string to a quantity, or change units.
    # Keep unlike observations separate rather than comparing their numbers.
    observations.append(observation)
    maxima[field] = {"observations": observations}


class ProcessSampler:
    """Sample only the backend process; preserve /proc and DRM field units."""

    def __init__(self, pid_provider, interval_seconds=0.1):
        if not math.isfinite(interval_seconds) or interval_seconds <= 0:
            raise ValueError("sampling interval must be positive")
        self.pid_provider = pid_provider
        self.interval_seconds = interval_seconds
        self._stop = threading.Event()
        self._thread = None
        self._samples = []
        self._errors = {}
        self._started = None
        self._finished = None

    def __enter__(self):
        if self._thread is not None:
            raise RuntimeError("a sampler cannot be started twice")
        self._started = time.monotonic()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def _sample(self):
        try:
            pid = self.pid_provider()
            if pid is None:
                return
            if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
                raise ValueError("pid_provider must return a positive PID or None")
            sample = _read_process(pid)
            sample["elapsed_seconds"] = time.monotonic() - self._started
            self._samples.append(sample)
        except (OSError, ValueError, IndexError) as error:
            key = type(error).__name__
            self._errors[key] = self._errors.get(key, 0) + 1

    def _run(self):
        while not self._stop.is_set():
            self._sample()
            self._stop.wait(self.interval_seconds)

    def stop(self):
        if self._thread is None:
            raise RuntimeError("sampler has not been started")
        if not self._stop.is_set():
            self._stop.set()
            self._thread.join()
            self._sample()
            self._finished = time.monotonic()
        return self.report()

    def __exit__(self, *_):
        self.stop()

    def report(self):
        if self._finished is None:
            raise RuntimeError("stop the sampler before reading its report")
        processes = {}
        for sample in self._samples:
            identity = (sample["pid"], sample["start_ticks"])
            entry = processes.setdefault(identity, {
                "pid": sample["pid"], "start_ticks": sample["start_ticks"],
                "samples": 0, "max_process_status_kB": {},
                "drm_client_maxima": {}, "max_simultaneous_drm_clients": 0,
                "fd_read_errors": 0,
                "first_elapsed_seconds": sample["elapsed_seconds"],
                "first_cpu_ticks": sample["cpu_ticks"],
            })
            entry["samples"] += 1
            entry["last_elapsed_seconds"] = sample["elapsed_seconds"]
            entry["last_cpu_ticks"] = sample["cpu_ticks"]
            entry["fd_read_errors"] += sample["fd_read_errors"]
            for key, value in sample["process_status_kB"].items():
                entry["max_process_status_kB"][key] = max(
                    entry["max_process_status_kB"].get(key, 0), value)
            clients = sample["drm_clients"]
            entry["max_simultaneous_drm_clients"] = max(
                entry["max_simultaneous_drm_clients"], len(clients))
            for client in clients:
                device = client.get("drm-pdev", client.get("drm-driver", "unknown"))
                key = f"{device}/client-{client['drm-client-id']}"
                maxima = entry["drm_client_maxima"].setdefault(key, {})
                for field, raw in client.items():
                    _update_drm_maximum(maxima, field, raw)
        for entry in processes.values():
            entry["observed_cpu_seconds"] = (
                entry.pop("last_cpu_ticks") - entry.pop("first_cpu_ticks")
            ) / os.sysconf("SC_CLK_TCK")
        return {
            "interval_seconds": self.interval_seconds,
            "elapsed_seconds": self._finished - self._started,
            "samples": len(self._samples),
            "sample_errors": dict(self._errors),
            "processes": list(processes.values()),
            "notes": [
                "VmRSS and VmHWM are exact /proc/<pid>/status fields in kB.",
                "DRM values retain their exact fdinfo field names and units; duplicate client IDs per device are sampled once.",
                "Field type or unit changes are retained as separate observations; maxima are compared only within the same unit.",
                "DRM client maxima can occur at different times; do not add maxima or combine them with RSS as a total or shared GPU peak.",
                "CPU seconds cover the first through last successful sample, excluding earlier process work.",
            ],
        }

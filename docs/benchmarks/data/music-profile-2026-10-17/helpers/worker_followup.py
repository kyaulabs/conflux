#!/usr/bin/env python3
"""Guarded local four-track worker follow-up; run only under root's outer guard.

The unchanged driver runs in one owned child session with a 360s wall bound.
All expensive checks/imports precede the unchanged <=55C readiness observation;
the reviewed final admission permits a fresh <=70C recovered/normal state.
"""
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import wave

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
SOURCE = ROOT / "benchmarks-local/user-tracks-2026-10-17"
OUTPUT = BASE / "worker-four-tracks"
ADMISSION = BASE / "worker-admission"
STATE = BASE / "worker-followup-launch.json"
EXPECTED_HEAD = "8a61059f585947c91fef4a5606ad11fb81523daf"
RUNTIME = Path("/tmp/conflux-stem-m0/conflux-stem-runtime-blocking")
MODEL = Path("/tmp/conflux-stem-m0/models/htdemucs-f16.gguf")
FRAMES, RATE, CHILD_SECONDS = 1031940, 44100, 360
FILES = {
    "runtime": (RUNTIME, "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6"),
    "model": (MODEL, "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"),
    "worker_source": (ROOT / "tools/stems/worker.py", "308579c6870834914602e4f33970e3b24b952edb5d7d28f82d1c65256389f6a8"),
    "sampler_source": (ROOT / "tools/benchmarks/worker_support.py", "8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b"),
    "shared_benchmark_source": (ROOT / "tools/benchmarks/stem_worker.py", "87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7"),
    "driver": (SOURCE / "quality_run.py", "1773500b26bd1a9ed584decba453005f72c64d770663f03cc7d13d01646af719"),
    "readiness_source": (SOURCE / "normal_ready.py", "4e94000d5966357c653a6ef64729f9b513df723ce0badb17e496d4b85c07135f"),
    "admission_source": (BASE / "interleaved_replay.py", "72d202f04fd787ab8f0871854c8c6feeb8dc6aa4f00c2cbb84359389779441ee"),
    "native_replay_source": (ROOT / "tools/benchmarks/native_replay.py", "6ee7408415fdf0366cd3282fe57f94409d8f9bc34eb2967eb0e622bf2cf1757e"),
    "manifest": (SOURCE / "manifest.json", "4a0a50eb0adcdd8649189f9f6e5850c54ead9bf01b58c9f0e4bcae424414af53"),
}
WAVS = {
    "club_xl": "1b959a945c5169b3bbfcfb8b1c1c7a3b8ca5aa885bfda36a56d967d74d700023",
    "when_the_light_breaks": "3448e91b9af700e250a73c752ce9ccff2d413add50ad46c928cb9728ea3ff1cf",
    "under_my_skin": "afce9859f42deaf1a94346c41a9501a244ba8a5be513920b3b9da799f3abb879",
    "stfu": "30a3b35ca65368d698a8b7ae0f24761cf5491d9174243d03759b2ed5103c1fda",
}


def require(test, message):
    if not test:
        raise ValueError(message)


def utc():
    return datetime.now(timezone.utc).isoformat()


def signature(path):
    value = path.stat()
    return [value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns]


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest}


def head():
    return subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True, check=True, timeout=5).stdout.strip()


def main():
    started = time.monotonic()
    stdout_path, stderr_path = BASE / "worker-four-tracks.stdout", BASE / "worker-four-tracks.stderr"
    require(not any(path.exists() for path in (STATE, OUTPUT, ADMISSION, stdout_path, stderr_path)), "Use fresh owned launcher/admission/output/log paths")
    ADMISSION.mkdir()
    state = {"schema": 1, "status": "checking", "started_at_utc": utc(), "expected_head": EXPECTED_HEAD,
             "child_started": False, "child_wall_limit_seconds": CHILD_SECONDS, "outer_bound_seconds": 1020,
             "scope": "Unchanged four-track Vulkan quality driver; no CPU-reference pass, benchmark UI/audio playback, host policy mutation or new sustained latency qualification. Three completed-source seeks remain included. Child session is owned and signalled as a group; launcher remains in the outer guard group.",
             "pins": {}, "failures": []}
    child, helper = None, None
    tracked_signatures = {}

    def save(**fields):
        state.update(fields, elapsed_seconds=time.monotonic()-started, updated_at_utc=utc())
        temporary = STATE.with_name(STATE.name + ".tmp")
        temporary.write_text(json.dumps(state, indent=2, allow_nan=False) + "\n"); temporary.replace(STATE)
        print(json.dumps({"status": state["status"], **fields}, allow_nan=False), flush=True)

    def send_group(signum):
        if child is not None:
            try: os.killpg(child.pid, signum)
            except ProcessLookupError: pass

    def stop(signum, _frame):
        state["interruption_signal"] = signum
        send_group(signum)
        raise KeyboardInterrupt

    def group_exists():
        if child is None: return False
        try: os.killpg(child.pid, 0); return True
        except ProcessLookupError: return False

    previous = {sig: signal.signal(sig, stop) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        save()
        state["source_head"] = head(); require(state["source_head"] == EXPECTED_HEAD, "Repository HEAD differs")
        for name, (path, expected) in FILES.items():
            before = signature(path); actual = pin(path)
            require(before == signature(path) and actual["sha256"] == expected, "Frozen artifact changed: " + name)
            state["pins"][name] = actual; tracked_signatures[path] = before
        state["pins"]["launcher"] = pin(Path(__file__).resolve())
        manifest = json.loads((SOURCE / "manifest.json").read_text())
        require([track["id"] for track in manifest["tracks"]] == list(WAVS), "Manifest track order differs")
        plan = manifest["excerpt_plan"]
        require((plan["frames"], plan["rate"], plan["channels"], plan["sample_bytes"], plan["source_offset_seconds"])
                == (FRAMES, RATE, 2, 2, 30.0) and plan["looping"] is False, "Excerpt plan differs")
        state["inputs"] = {}
        for track in manifest["tracks"]:
            path = Path(track["input_wav"])
            require(path.resolve() == SOURCE / "assets" / (track["id"] + "-30s-23p4s.wav") and not path.is_symlink(), "Unexpected prepared source path")
            before = signature(path); actual = pin(path)
            with wave.open(str(path), "rb") as audio:
                require(audio.getparams()[:4] == (2, 2, RATE, FRAMES) and audio.getcomptype() == "NONE", "Prepared WAV geometry differs")
            actual.update(frames=FRAMES, sample_rate=RATE, channels=2, sample_bytes=2)
            require(before == signature(path) and actual["sha256"] == WAVS[track["id"]] and actual == track["input_pin"], "Prepared WAV pin differs")
            tracked_signatures[path] = before; state["inputs"][track["id"]] = actual
        spec = importlib.util.spec_from_file_location("worker_followup_admission", BASE / "interleaved_replay.py")
        helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
        ready_module = helper.readiness_module()
        require(not {key: os.environ[key] for key in helper.replay.DEBUG_FLAGS if os.environ.get(key)}, "Inherited diagnostic GPU flags present")
        save(status="normal_readiness", checks_and_imports_finished_monotonic=time.monotonic(), source_signatures={str(path): value for path, value in tracked_signatures.items()})
        code = ready_module.run("admission", ADMISSION)
        save(readiness_returncode=code, readiness_proof=pin(ADMISSION / "admission-normal-ready.json"))
        require(code == 0, "Post-setup normal readiness did not match")
        require(head() == EXPECTED_HEAD and all(signature(path) == value for path, value in tracked_signatures.items()), "HEAD/input/helper changed during readiness")
        save(status="fresh_admission")
        admission = helper.fresh_admission(ADMISSION)
        command = [sys.executable, "-B", str(SOURCE / "quality_run.py"), "--manifest", str(SOURCE / "manifest.json"),
                   "--runtime", str(RUNTIME), "--model", str(MODEL), "--backend", "vulkan", "--threads", "4",
                   "--request-timeout", "120", "--output", str(OUTPUT)]
        state.update(admission=admission, command=command, child_spawn_started_monotonic=time.monotonic())
        with stdout_path.open("x") as stdout, stderr_path.open("x") as stderr:
            child = subprocess.Popen(command, cwd=ROOT, stdout=stdout, stderr=stderr, start_new_session=True)
            save(status="worker_running", child_started=True, child_pid=child.pid, child_process_group=child.pid,
                 outer_process_group=os.getpgrp(), child_started_monotonic=time.monotonic())
            try: code = child.wait(timeout=CHILD_SECONDS)
            except subprocess.TimeoutExpired:
                state["status"] = "timeout"; state["termination_reason"] = "360-second child wall budget exhausted"
                send_group(signal.SIGINT)
                try: code = child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    send_group(signal.SIGKILL); code = child.wait(timeout=5)
            state["child_returncode"] = code
        require(state["status"] != "timeout" and code == 0, "Worker failed or exceeded wall budget")
        state["worker_result"] = pin(OUTPUT / "result.json")
        require(all(signature(path) == value for path, value in tracked_signatures.items()) and head() == EXPECTED_HEAD, "Frozen sources changed during child")
        save(status="completed", child_finished_monotonic=time.monotonic())
    except KeyboardInterrupt:
        state["status"] = "interrupted"
    except Exception as error:
        state["failures"].append({"type": type(error).__name__, "error": str(error)})
        if state["status"] != "timeout": state["status"] = "error"
    finally:
        for sig in previous: signal.signal(sig, signal.SIG_IGN)
        if child is not None:
            try:
                if child.poll() is None:
                    send_group(signal.SIGINT)
                    try: child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        send_group(signal.SIGKILL); child.wait(timeout=5)
                if group_exists():
                    send_group(signal.SIGKILL)
                    for _ in range(20):
                        if not group_exists(): break
                        time.sleep(.1)
            except Exception as error:
                state["status"] = "error"; state["failures"].append({"phase": "cleanup", "error": str(error)})
            state["child_returncode"] = child.returncode
            state["cleanup"] = {"child_reaped": child.poll() is not None, "owned_process_group_exists": group_exists()}
            if not state["cleanup"]["child_reaped"] or state["cleanup"]["owned_process_group_exists"]:
                state["status"] = "error"; state["failures"].append({"phase": "cleanup", "error": "Owned child/group remains"})
        for name, path in (("fresh_admission_capture", ADMISSION / "fresh-admission.json"), ("stdout", stdout_path), ("stderr", stderr_path)):
            if path.is_file(): state[name] = pin(path)
        save(finished_at_utc=utc())
        for sig, handler in previous.items(): signal.signal(sig, handler)
    return 0 if state["status"] == "completed" else 130 if state["status"] == "interrupted" else 1


if __name__ == "__main__": raise SystemExit(main())

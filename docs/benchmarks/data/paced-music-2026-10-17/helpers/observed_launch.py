#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded paced music/audio/software-UI trial; no host policy changes."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import wave

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
CORPUS = ROOT / "benchmarks-local/user-tracks-2026-10-17"
PROFILE = ROOT / "benchmarks-local/music-profile-2026-10-17"
OLD = ROOT / "benchmarks-local/software-ui-2026-10-17"
RUNTIME = Path("/tmp/conflux-stem-m0/conflux-stem-runtime-blocking")
MODEL = Path("/tmp/conflux-stem-m0/models/htdemucs-f16.gguf")
PRODUCERS = {
    "runtime": (RUNTIME, "8987190b68829046aa08237a45ab7c2e708674fc0c207c9f9da6c14b86027dc6"),
    "model": (MODEL, "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"),
    "ui": (ROOT / "build/dev/conflux", "a2c1430535a14fa92d70ed1dd94015319616f1d67080af5a6bebe0ec5220dc9f"),
    "audio_probe": (ROOT / "build/dev/conflux-audio-probe", "b8cbfb0a255c94cf8430b9d7ef97823a9e2f3b11eedf32b3dec179ba1d197ea2"),
    "soak_source": (ROOT / "tools/benchmarks/stem_soak.py", None),
    "shared_benchmark_source": (ROOT / "tools/benchmarks/stem_worker.py", "87b1192dc603699bccfb15a858fad05625d022842a65e7958f45c0e4e88a48d7"),
    "worker_source": (ROOT / "tools/stems/worker.py", None),
    "sampler_source": (ROOT / "tools/benchmarks/worker_support.py", "8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b"),
}
HELPERS = {
    "admission_source": (PROFILE / "interleaved_replay.py", "72d202f04fd787ab8f0871854c8c6feeb8dc6aa4f00c2cbb84359389779441ee"),
    "readiness_source": (CORPUS / "normal_ready.py", "4e94000d5966357c653a6ef64729f9b513df723ce0badb17e496d4b85c07135f"),
    "observer_source": (OLD / "observed-run.py", "a6a4b81014f6aec16c8cd68e902348afcd10a1fdf5966f6b2a54ba2f622ef394"),
    "window_guard_source": (ROOT / "tools/benchmarks/window_guard.py", "077bd16eefbdd9b80f787b6aeb2837a3790fedf7f5defd7b140dccc6e1f5cebb"),
    "native_replay_source": (ROOT / "tools/benchmarks/native_replay.py", "6ee7408415fdf0366cd3282fe57f94409d8f9bc34eb2967eb0e622bf2cf1757e"),
    "manifest": (CORPUS / "manifest.json", "4a0a50eb0adcdd8649189f9f6e5850c54ead9bf01b58c9f0e4bcae424414af53"),
    "ui_cpp_source": (ROOT / "src/main.cpp", "302847b129ecc31215e7f3ee9fa91708b64bdc1cdca320b98c6e78e6769e17eb"),
    "ui_qml_source": (ROOT / "qml/Main.qml", "b276894c14b718276b6e2e5224c575b848faaddb2ba142747e452b1b94970aeb"),
    "audio_cpp_source": (ROOT / "src/probe_main.cpp", "9f6ae22c75a02472b602194b684e184e4c8862cb6475d56bd462f1e579220ae6"),
}
WAVS = {
    "club_xl": "1b959a945c5169b3bbfcfb8b1c1c7a3b8ca5aa885bfda36a56d967d74d700023",
    "when_the_light_breaks": "3448e91b9af700e250a73c752ce9ccff2d413add50ad46c928cb9728ea3ff1cf",
    "under_my_skin": "afce9859f42deaf1a94346c41a9501a244ba8a5be513920b3b9da799f3abb879",
    "stfu": "30a3b35ca65368d698a8b7ae0f24761cf5491d9174243d03759b2ed5103c1fda",
}


def require(value, message):
    if not value:
        raise RuntimeError(message)


def utc():
    return datetime.now(timezone.utc).isoformat()


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest}


def signature(path):
    value = path.stat()
    return [value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns]


def head():
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True, timeout=5).strip()


def small_json(path):
    if not path.exists():
        return None
    require(path.stat().st_size <= 131072, "Small progress file exceeded128KiB: " + str(path))
    return json.loads(path.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--control-interval", type=int, required=True)
    args = parser.parse_args()
    if (not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", args.name)
            or (args.seconds, args.control_interval) not in ((600, 300), (3600, 300))):
        parser.error("Use a fresh plain label and 600s/300s validation or 3600s/300s hour")
    config_path = BASE / "expected-pins.json"
    expected = json.loads(config_path.read_text())
    if (expected.get("status") != "ready" or not re.fullmatch(r"[0-9a-f]{40}", expected.get("repository_head") or "")
            or set(expected.get("pins", {})) != set(PRODUCERS)
            or any(not re.fullmatch(r"[0-9a-f]{64}", value or "") for value in expected["pins"].values())):
        parser.error("expected-pins.json is not armed with signed source HEAD and all eight hashes")
    for name, (_, frozen) in PRODUCERS.items():
        require(frozen is None or expected["pins"][name] == frozen, "Expected frozen pin differs: " + name)
    run, admission = BASE / args.name, BASE / (args.name + "-admission")
    state_path = BASE / (args.name + "-launch.json")
    monitor_path = BASE / (args.name + "-AC-lid-monitor.jsonl")
    guard_path, policy_path = BASE / (args.name + "-guard.json"), BASE / (args.name + "-policy.json")
    stdout_path, stderr_path = BASE / (args.name + ".stdout"), BASE / (args.name + ".stderr")
    require(not any(path.exists() for path in (run, admission, state_path, monitor_path, guard_path, policy_path, stdout_path, stderr_path)), "Run paths already exist; use a fresh label")
    admission.mkdir()
    began = time.monotonic()
    deadline = began + args.seconds + 750
    state = {"schema": 1, "status": "checking", "started_at_utc": utc(), "started_monotonic": began,
             "load_mode": "both", "delivery_mode": "playback-paced", "renderer": "software", "repaint_ms": 33, "seconds": args.seconds,
             "expected_controls": 1 if args.seconds == 600 else 11, "threads": 4, "ready_queue_bound": 2,
             "quantum_frames": 256, "request_timeout_seconds": 30, "sample_interval_seconds": 0.5, "target_ms": 4095,
             "control_interval_seconds": args.control_interval, "maximum_total_seconds": args.seconds + 750,
             "expected_producer": expected, "pins": {}, "inputs": {}, "failures": [], "child_started": False,
             "policy_mode": "observe", "profile_mutation": False, "service_mutation": False,
             "cadence_scope": "33ms QTimeLine update interval, not a global frame-rate cap",
             "diagnostic_ui_logging": False, "scope": "Playback-paced repeated prepared music excerpts with independent silent audio and visible software-UI load. No stem playback, physical-audio measurement, hardware GPU preemption or invented thermal pass gate."}
    child, helper = None, None
    signatures, owned = {}, {}
    active_phase = "checking"

    def save(**fields):
        state.update(fields, elapsed_seconds=time.monotonic() - began, updated_at_utc=utc())
        temporary = state_path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(state, indent=2, allow_nan=False) + "\n")
        temporary.replace(state_path)

    def remember_owned():
        if child is None:
            return
        pending, visited = [child.pid], set()
        while pending and len(visited) < 64:
            pid = pending.pop()
            if pid in visited:
                continue
            visited.add(pid)
            proc = Path("/proc") / str(pid)
            try:
                require(proc.stat().st_uid == os.getuid(), "Owned descendant UID changed")
                fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
                owned[pid] = {"start_ticks": int(fields[19]), "process_group": int(fields[2])}
                pending.extend(int(value) for value in (proc / "task" / str(pid) / "children").read_text().split())
            except FileNotFoundError:
                pass

    def live_owned():
        live = {}
        for pid, identity in owned.items():
            try:
                fields = (Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()
                if int(fields[19]) == identity["start_ticks"] and int(fields[2]) == identity["process_group"]:
                    live[pid] = dict(identity, state=fields[0])
            except FileNotFoundError:
                pass
        return live

    def signal_child(signum):
        if child is not None and child.poll() is None:
            try:
                os.killpg(child.pid, signum)
            except ProcessLookupError:
                pass

    def stop(signum, _frame):
        state["interruption_signal"] = signum
        signal_child(signal.SIGINT)
        raise KeyboardInterrupt

    def monitored_wait(until):
        next_check = 0.0
        while child.poll() is None:
            now = time.monotonic()
            require(now < until, active_phase + " exhausted its finite wall budget")
            if now >= next_check:
                next_check = now + 45
                remember_owned()
                row = {"event": "snapshot", "phase": active_phase, "at_utc": utc(),
                       "monotonic": now, "AC_lid": helper.ac_lid(),
                       "progress": small_json(run / "progress.json"), "ui_pulse": small_json(run / "ui-progress.json")}
                with monitor_path.open("a") as stream:
                    stream.write(json.dumps(row, allow_nan=False) + "\n")
                progress = row["progress"] or {}
                print(json.dumps({"phase": active_phase, "AC_lid": row["AC_lid"],
                                  "workload_seconds": progress.get("elapsed_workload_seconds"),
                                  "timing": progress.get("timing"), "controls": progress.get("completed_controls"),
                                  "failures": progress.get("failures")}), flush=True)
                require(row["AC_lid"]["ready"], "AC/lid changed or observation failed; aborting owned trial")
            time.sleep(min(.25, max(0, until - time.monotonic())))
        return child.wait()

    previous = {sig: signal.signal(sig, stop) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        save()
        require(head() == expected["repository_head"], "Signed expected HEAD differs")
        for name, (path, _) in {**PRODUCERS, **HELPERS}.items():
            before = signature(path)
            actual = pin(path)
            wanted = expected["pins"][name] if name in PRODUCERS else HELPERS[name][1]
            require(before == signature(path) and actual["sha256"] == wanted, "Frozen artifact changed: " + name)
            state["pins"][name] = actual
            signatures[path] = before
        for name, path in (("launcher", Path(__file__).resolve()), ("expected_pins_file", config_path)):
            before = signature(path)
            state["pins"][name] = pin(path)
            require(before == signature(path), "Launcher/config changed while pinning")
            signatures[path] = before
        tracks = json.loads((CORPUS / "manifest.json").read_text())["tracks"]
        require([track["id"] for track in tracks] == list(WAVS), "Manifest order differs")
        source_flags = []
        for track in tracks:
            path = Path(track["input_wav"])
            require(path.is_absolute() and path.resolve() == CORPUS / "assets" / (track["id"] + "-30s-23p4s.wav") and not path.is_symlink(), "Unexpected music source path")
            before = signature(path)
            actual = pin(path)
            with wave.open(str(path), "rb") as audio:
                require(audio.getparams()[:4] == (2, 2, 44100, 1031940) and audio.getcomptype() == "NONE", "Prepared music geometry differs")
            actual.update(frames=1031940, sample_rate=44100, channels=2, sample_bytes=2)
            require(before == signature(path) and actual["sha256"] == WAVS[track["id"]] and actual == track["input_pin"], "Music pin differs")
            state["inputs"][track["id"]] = actual
            signatures[path] = before
            source_flags += ["--source-wav", str(path)]
        spec = importlib.util.spec_from_file_location("music_load_admission", HELPERS["admission_source"][0])
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        helper.readiness_module()  # Complete safe imports before readiness.
        require(not {key: os.environ[key] for key in helper.replay.DEBUG_FLAGS if os.environ.get(key)}, "Inherited diagnostic native flags present")
        state["host_conditions_before_admission"] = helper.ac_lid()
        require(state["host_conditions_before_admission"]["ready"], "AC online/open lid required before readiness")
        active_phase = "normal_readiness"
        readiness_command = [sys.executable, "-B", "-c", "import runpy,sys;from pathlib import Path;sys.exit(runpy.run_path(sys.argv[1])['run']('admission',Path(sys.argv[2])))", str(HELPERS["readiness_source"][0]), str(admission)]
        save(status=active_phase, checks_and_imports_finished_monotonic=time.monotonic(), readiness_command=readiness_command)
        with (admission / "stdout").open("x") as out, (admission / "stderr").open("x") as err:
            child = subprocess.Popen(readiness_command, cwd=ROOT, stdout=out, stderr=err, start_new_session=True)
            code = monitored_wait(min(deadline - 30, time.monotonic() + 620))
        state["readiness_returncode"] = code
        require(code == 0, "Bounded post-setup normal readiness did not match")
        child = None
        require(head() == expected["repository_head"] and all(signature(path) == value for path, value in signatures.items()), "HEAD/artifact changed during readiness")
        environment = dict(os.environ, QSG_INFO="1", CONFLUX_M0_REPAINT_MS="33", QT_LOGGING_RULES="qt.scenegraph.time.*=false")
        environment.pop("QT_MESSAGE_PATTERN", None)
        command = ["systemd-inhibit", "--what=sleep:idle", "--mode=block", "--who=CONFLUX", "--why=M0 paced music audio UI soak", sys.executable, "-B", str(HELPERS["window_guard_source"][0]), "--output", str(guard_path), "--ui-progress", str(run / "ui-progress.json"), "--", sys.executable, "-B", str(HELPERS["observer_source"][0]), str(policy_path), "--runtime", str(RUNTIME), "--model", str(MODEL), "--ui", str(PRODUCERS["ui"][0]), "--audio-probe", str(PRODUCERS["audio_probe"][0]), "--load", "both", "--delivery-mode", "playback-paced", "--renderer", "software", "--seconds", str(args.seconds), "--control-interval", str(args.control_interval), "--threads", "4", "--quantum", "256", "--request-timeout", "30", "--sample-interval", "0.5", "--output", str(run), *source_flags]
        state.update(command=command, UI_diagnostic_environment={key: environment.get(key) for key in ("CONFLUX_M0_REPAINT_MS", "QT_LOGGING_RULES", "QT_MESSAGE_PATTERN", "QSG_RENDER_LOOP", "QSG_RHI_BACKEND", "QT_QUICK_BACKEND")}, effective_UI_QT_QUICK_BACKEND="software", effective_UI_QSG_RHI_BACKEND="unset by Load", backend_environment_scope="Software backend selection is UI-only. Existing Load clears renderer selectors for the non-Qt audio child; native inference policy remains unchanged.")
        save(status="fresh_admission")
        state["admission"] = helper.fresh_admission(admission)
        active_phase = "workload"
        with stdout_path.open("x") as out, stderr_path.open("x") as err:
            child = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err, env=environment, start_new_session=True)
            save(status="running", child_started=True, owned_guard_process_group=child.pid, child_spawned_monotonic=time.monotonic())
            code = monitored_wait(deadline - 30)
        state["returncode"] = code
        require(code == 0, "Guarded workload returned failure")
        require(head() == expected["repository_head"] and all(signature(path) == value for path, value in signatures.items()), "Frozen source/input changed during workload")
        result = json.loads((run / "result.json").read_text())
        require(result["status"] == "ok" and not result["failures"] and result["workload_seconds"] >= args.seconds, "Completed workload report failed")
        save(status="completed")
    except KeyboardInterrupt:
        state["status"] = "interrupted"
    except Exception as error:
        state["status"] = "error"
        state["failures"].append({"phase": active_phase, "type": type(error).__name__, "error": str(error)})
    finally:
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        try:
            remember_owned()
            if child is not None and child.poll() is None:
                signal_child(signal.SIGINT)
                try:
                    child.wait(timeout=min(25, max(.1, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    pass
            # Separate observer/soak sessions normally unwind through their own
            # SIGINT handlers. Kill only still-matching owned descendant groups.
            live = live_owned()
            groups = {value["process_group"] for value in live.values()}
            for group in groups:
                require(group > 1 and group != os.getpgrp(), "Refusing unsafe cleanup process group")
                try:
                    os.killpg(group, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if child is not None:
                child.wait(timeout=3)
            for _ in range(20):
                if not live_owned():
                    break
                time.sleep(.1)
            state["cleanup"] = {"owned_process_identities": owned, "remaining_owned_processes": live_owned(), "last_child_reaped": child is None or child.poll() is not None}
            require(not state["cleanup"]["remaining_owned_processes"] and state["cleanup"]["last_child_reaped"], "Owned descendants remain after cleanup")
        except Exception as error:
            state["status"] = "error"
            state["failures"].append({"phase": "cleanup", "error": str(error)})
        for name, path in (("fresh_admission_capture", admission / "fresh-admission.json"), ("readiness_proof", admission / "admission-normal-ready.json"), ("guard", guard_path), ("policy", policy_path), ("result", run / "result.json"), ("stdout", stdout_path), ("stderr", stderr_path), ("AC_lid_monitor", monitor_path)):
            if path.is_file():
                state[name] = pin(path)
        if guard_path.exists():
            guard = json.loads(guard_path.read_text())
            state["guard_cleanup"] = {key: guard.get(key) for key in ("status", "errors", "idle_inhibit_released", "script_unloaded", "restored")}
            if not (guard.get("status") == "ok" and not guard.get("errors") and guard.get("idle_inhibit_released") and guard.get("script_unloaded") and guard.get("restored")):
                state["status"] = "error"
                state["failures"].append({"phase": "guard_cleanup", "error": "Window/idle guard cleanup not confirmed"})
        with monitor_path.open("a") as stream:
            stream.write(json.dumps({"event": "finished", "at_utc": utc(), "monotonic": time.monotonic(), "status": state["status"]}) + "\n")
        state["AC_lid_monitor"] = pin(monitor_path)
        save(finished_at_utc=utc())
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    print(json.dumps({"launch": str(state_path), "status": state["status"], "failures": state["failures"]}), flush=True)
    return 0 if state["status"] == "completed" else 130 if state["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: AGPL-3.0-or-later
"""Launch one guarded software/Vulkan comparison with frozen worker/audio settings."""
import argparse
import hashlib
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

base = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("name")
parser.add_argument("mode", choices=("none", "audio", "both", "ui"))
parser.add_argument("--seconds", type=int, default=3600)
parser.add_argument("--control-interval", type=int, default=300)
parser.add_argument("--repaint-ms", choices=(16, 33), type=int, required=True)
parser.add_argument("--renderer", choices=("vulkan", "software"), required=True)
args = parser.parse_args()
if not 60 <= args.seconds <= 3600 or not 1 <= args.control_interval < args.seconds:
    parser.error("supply 60..3600 seconds and a positive control interval shorter than the trial")
if args.repaint_ms != 33 or args.mode != "both":
    parser.error("this comparison requires both loads and the fixed 33 ms QTimeLine parameter")
expected_bytes = (base / "expected-pins.json").read_bytes()
expected = json.loads(expected_bytes)
if (expected.get("status") != "ready" or not isinstance(expected.get("repository_head"), str)
        or not re.fullmatch(r"[0-9a-f]{40}", expected["repository_head"])
        or len(expected.get("pins", {})) != 8
        or not all(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value)
                   for value in expected["pins"].values())):
    parser.error("expected-pins.json awaits the signed source HEAD and final frozen hashes")


def host_conditions():
    record = {"at_utc": datetime.now(timezone.utc).isoformat(),
              "started_monotonic": time.monotonic(), "errors": []}
    try:
        record["mains_online"] = Path("/sys/class/power_supply/AC/online").read_text().strip()
    except OSError as error:
        record["mains_online"] = None
        record["errors"].append(f"AC: {type(error).__name__}: {error}")
    command = ["busctl", "--system", "get-property", "org.freedesktop.login1",
               "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "LidClosed"]
    record["lid_command"] = command
    try:
        value = subprocess.check_output(command, text=True, timeout=2).strip()
        if value not in ("b true", "b false"):
            raise ValueError("unexpected login1 LidClosed value")
        record["lid_closed"] = value == "b true"
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        record["lid_closed"] = None
        record["errors"].append(f"LidClosed: {type(error).__name__}: {error}")
    record["ready"] = (not record["errors"] and record["mains_online"] == "1"
                       and record["lid_closed"] is False)
    record["finished_monotonic"] = time.monotonic()
    record["scope"] = "Read-only AC sysfs and login1 lid snapshot; no continuous-state claim"
    return record


run = base / args.name
if run.exists() or (base / (args.name + "-guard.json")).exists():
    raise RuntimeError("trial path already exists")
admission_path = base / (args.name + "-admission.json")
if admission_path.exists():
    raise RuntimeError("admission record already exists; use a fresh trial name")
admission = {"started_at_utc": datetime.now(timezone.utc).isoformat(), "load_mode": args.mode,
             "profile_mutation": False, "stage": "hardware_preflight",
             "repaint_ms": args.repaint_ms, "renderer": args.renderer}
try:
    admission["host_conditions_before_admission"] = host_conditions()
    admission_path.write_text(json.dumps(admission, indent=2) + "\n")
    if not admission["host_conditions_before_admission"]["ready"]:
        raise RuntimeError("AC online and open lid were not both observed before admission")
    for stage, command in (("normal_readiness", [sys.executable, "-B", str(base / "normal_ready.py"), args.name]),
                           ("source_capture", [sys.executable, "-B", str(base / "observed-capture.py"), "before-" + args.name])):
        admission["stage"] = stage
        admission_path.write_text(json.dumps(admission, indent=2) + "\n")
        subprocess.run(command, check=True, timeout=620 if stage == "normal_readiness" else 30)
    captured = json.loads((base / ("before-" + args.name + ".json")).read_text())
    if captured["head"] != expected["repository_head"]:
        raise RuntimeError("source HEAD differs from the signed expected producer")
    pin_paths = {"runtime": "/tmp/conflux-stem-m0/conflux-stem-runtime-blocking",
                 "model": "/tmp/conflux-stem-m0/models/htdemucs-f16.gguf",
                 "soak_source": "tools/benchmarks/stem_soak.py",
                 "shared_benchmark_source": "tools/benchmarks/stem_worker.py",
                 "worker_source": "tools/stems/worker.py",
                 "sampler_source": "tools/benchmarks/worker_support.py",
                 "ui": "build/dev/conflux", "audio_probe": "build/dev/conflux-audio-probe"}
    for key, path_name in pin_paths.items():
        if captured["pins"][path_name]["sha256"] != expected["pins"][key]:
            raise RuntimeError("frozen producer pin differs before launch: " + key)
    admission["stage"] = "admitted"
except BaseException as error:
    admission.update(status="error", error=f"{type(error).__name__}: {error}",
                     finished_at_utc=datetime.now(timezone.utc).isoformat())
    admission_path.write_text(json.dumps(admission, indent=2) + "\n")
    raise
admission_path.write_text(json.dumps(admission, indent=2) + "\n")

guard = [sys.executable, "-B", "tools/benchmarks/window_guard.py", "--output", str(base / (args.name + "-guard.json"))]
guard += ["--ui-progress", str(run / "ui-progress.json")] if args.mode in ("ui", "both") else ["--idle-only"]
command = ["systemd-inhibit", "--what=sleep:idle", "--mode=block", "--who=CONFLUX",
           "--why=M0 software UI comparison", *guard, "--", sys.executable, "-B",
           str(base / "observed-run.py"), str(base / (args.name + "-policy.json")),
           "--runtime", "/tmp/conflux-stem-m0/conflux-stem-runtime-blocking", "--model",
           "/tmp/conflux-stem-m0/models/htdemucs-f16.gguf", "--ui", "build/dev/conflux",
           "--audio-probe", "build/dev/conflux-audio-probe", "--load", args.mode,
           "--renderer", args.renderer, "--seconds", str(args.seconds), "--control-interval",
           str(args.control_interval), "--request-timeout", "30", "--sample-interval", "0.5", "--output", str(run)]
environment = dict(os.environ, QSG_INFO="1", CONFLUX_M0_REPAINT_MS=str(args.repaint_ms))
# Keep diagnostic timing disabled throughout this cadence experiment.
environment["QT_LOGGING_RULES"] = "qt.scenegraph.time.*=false"
environment.pop("QT_MESSAGE_PATTERN", None)
record = {"renderer": args.renderer, "repaint_ms": args.repaint_ms,
          "expected_producer": expected, "expected_pins_file_sha256": hashlib.sha256(expected_bytes).hexdigest(),
          "cadence_scope": "QTimeLine update interval; not a global frame-rate limit",
          "diagnostic_ui_logging": False, "policy_mode": "observe", "profile_mutation": False, "command": command, "started_at_utc": datetime.now(timezone.utc).isoformat(),
          "launcher_PID": os.getpid(), "load_mode": args.mode, "seconds": args.seconds,
          "QSG_INFO": environment["QSG_INFO"],
          "UI_diagnostic_environment": {key: environment.get(key) for key in
              ("CONFLUX_M0_REPAINT_MS", "QT_LOGGING_RULES", "QT_MESSAGE_PATTERN", "QSG_RENDER_LOOP", "QSG_RHI_BACKEND", "QT_QUICK_BACKEND")},
          "effective_UI_QSG_RHI_BACKEND": "unset for software" if args.renderer == "software" else "vulkan",
          "effective_UI_QT_QUICK_BACKEND": "software" if args.renderer == "software" else "unset by Load",
          "backend_environment_scope": "Software backend selection is UI-only; the existing Vulkan QSG environment also reaches the non-Qt audio load unchanged. Native inference environment is unchanged."}
record["wrapper_pins"] = {name: hashlib.sha256((base / name).read_bytes()).hexdigest()
                          for name in ("observed_launch.py", "observed-run.py", "normal_ready.py", "observed-capture.py", "check_run.py")}
path = base / (args.name + "-launch.json")
path.write_text(json.dumps(record, indent=2) + "\n")
with (base / (args.name + ".stdout")).open("w") as out, (base / (args.name + ".stderr")).open("w") as err:
    record["host_conditions_before_spawn"] = host_conditions()
    path.write_text(json.dumps(record, indent=2) + "\n")
    if not record["host_conditions_before_spawn"]["ready"]:
        record.update(returncode=1, finished_at_utc=datetime.now(timezone.utc).isoformat(),
                      error="AC online and open lid were not both observed immediately before spawn")
        path.write_text(json.dumps(record, indent=2) + "\n")
        raise RuntimeError(record["error"])
    child = subprocess.Popen(command, stdout=out, stderr=err, env=environment, start_new_session=True)
    def stop(_signum, _frame):
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGINT)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, stop)
    record["owned_guard_process_group"] = child.pid
    path.write_text(json.dumps(record, indent=2) + "\n")
    code = child.wait()
record.update(returncode=code, finished_at_utc=datetime.now(timezone.utc).isoformat())
path.write_text(json.dumps(record, indent=2) + "\n")
if code == 0:
    subprocess.run([sys.executable, "-B", str(base / "check_run.py"), args.name], check=True)
raise SystemExit(code if code >= 0 else 128 - code)

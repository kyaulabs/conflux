# SPDX-License-Identifier: AGPL-3.0-or-later
"""Launch one named isolation trial with the frozen scoped wrappers."""
import argparse
import hashlib
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

base = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("name")
parser.add_argument("mode", choices=("none", "audio", "both", "ui"))
parser.add_argument("--seconds", type=int, default=3600)
parser.add_argument("--control-interval", type=int, default=300)
args = parser.parse_args()
run = base / args.name
if run.exists() or (base / (args.name + "-guard.json")).exists():
    raise RuntimeError("trial path already exists")
admission_path = base / (args.name + "-admission.json")
if admission_path.exists():
    raise RuntimeError("admission record already exists; use a fresh trial name")
admission = {"started_at_utc": datetime.now(timezone.utc).isoformat(), "load_mode": args.mode,
             "profile_mutation": False, "stage": "normal_readiness"}
try:
    for stage, command in (("normal_readiness", [sys.executable, "-B", str(base / "normal_ready.py"), args.name]),
                           ("source_capture", [sys.executable, "-B", str(base / "observed-capture.py"), "before-" + args.name])):
        admission["stage"] = stage
        admission_path.write_text(json.dumps(admission, indent=2) + "\n")
        subprocess.run(command, check=True, timeout=620 if stage == "normal_readiness" else 30)
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
           "--why=M0 sustained audio/UI load isolation", *guard, "--", sys.executable, "-B",
           str(base / "observed-run.py"), str(base / (args.name + "-policy.json")),
           "--runtime", "/tmp/conflux-stem-m0/conflux-stem-runtime-blocking", "--model",
           "/tmp/conflux-stem-m0/models/htdemucs-f16.gguf", "--ui", "build/dev/conflux",
           "--audio-probe", "build/dev/conflux-audio-probe", "--load", args.mode,
           "--renderer", "opengl", "--seconds", str(args.seconds), "--control-interval",
           str(args.control_interval), "--request-timeout", "30", "--sample-interval", "0.5", "--output", str(run)]
environment = dict(os.environ, QSG_INFO="1")
record = {"policy_mode": "observe", "profile_mutation": False, "command": command, "started_at_utc": datetime.now(timezone.utc).isoformat(),
          "launcher_PID": os.getpid(), "load_mode": args.mode, "seconds": args.seconds,
          "QSG_INFO": environment["QSG_INFO"]}
record["wrapper_pins"] = {name: hashlib.sha256((base / name).read_bytes()).hexdigest()
                          for name in ("observed_launch.py", "observed-run.py", "normal_ready.py", "observed-capture.py", "check_run.py")}
path = base / (args.name + "-launch.json")
path.write_text(json.dumps(record, indent=2) + "\n")
with (base / (args.name + ".stdout")).open("w") as out, (base / (args.name + ".stderr")).open("w") as err:
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

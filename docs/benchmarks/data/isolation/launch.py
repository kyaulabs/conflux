# SPDX-License-Identifier: AGPL-3.0-or-later
"""Launch one named isolation trial with the frozen scoped wrappers."""
import argparse
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
subprocess.run([sys.executable, "-B", str(base / "capture.py"), "before-" + args.name], check=True)
guard = [sys.executable, "-B", "tools/benchmarks/window_guard.py", "--output", str(base / (args.name + "-guard.json"))]
guard += ["--ui-progress", str(run / "ui-progress.json")] if args.mode in ("ui", "both") else ["--idle-only"]
command = ["systemd-inhibit", "--what=sleep:idle", "--mode=block", "--who=CONFLUX",
           "--why=M0 sustained audio/UI load isolation", *guard, "--", sys.executable, "-B",
           str(base / "balanced-run.py"), str(base / (args.name + "-policy.json")),
           "--runtime", "/tmp/conflux-stem-m0/conflux-stem-runtime-blocking", "--model",
           "/tmp/conflux-stem-m0/models/htdemucs-f16.gguf", "--ui", "build/dev/conflux",
           "--audio-probe", "build/dev/conflux-audio-probe", "--load", args.mode,
           "--renderer", "opengl", "--seconds", str(args.seconds), "--control-interval",
           str(args.control_interval), "--request-timeout", "30", "--sample-interval", "0.5", "--output", str(run)]
environment = dict(os.environ, QSG_INFO="1")
record = {"command": command, "started_at_utc": datetime.now(timezone.utc).isoformat(),
          "launcher_PID": os.getpid(), "load_mode": args.mode, "seconds": args.seconds,
          "QSG_INFO": environment["QSG_INFO"]}
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

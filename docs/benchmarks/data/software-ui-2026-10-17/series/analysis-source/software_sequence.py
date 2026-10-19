# SPDX-License-Identifier: AGPL-3.0-or-later
"""Run the finite matched renderer comparison and its conditional software hour."""
from datetime import datetime, timezone
import json
from pathlib import Path
import signal
import subprocess
import sys
import time

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
MAXIMUM_SECONDS = 10800
started = time.monotonic()
deadline = started + MAXIMUM_SECONDS
state_path = BASE / "series-state.json"
if state_path.exists() or (BASE / "software-series-stop").exists():
    raise FileExistsError("Use a fresh series; state or stop marker already exists")
expected = json.loads((BASE / "expected-pins.json").read_text())
if expected.get("status") != "ready" or not expected.get("repository_head"):
    raise RuntimeError("Expected source/pins are not armed after the signed source commit")
record = {"status": "running", "started_at_utc": datetime.now(timezone.utc).isoformat(),
          "started_monotonic": started, "maximum_seconds": MAXIMUM_SECONDS,
          "completed_runs": [], "profile_mutation": False, "repaint_ms": 33,
          "expected_producer": expected,
          "scope": "Matched Vulkan/software 900-second trials, then a conditional software hour. The short software p99 and existing validity checks select whether to run the hour; only the full hour can assess its M0 target. Fixed order does not establish causation."}
child = monitor = None


def save(**values):
    record.update(values, updated_at_utc=datetime.now(timezone.utc).isoformat(),
                  elapsed_seconds=time.monotonic() - started)
    state_path.write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(values), flush=True)


def wait(process, maximum):
    remaining = min(maximum, deadline - time.monotonic())
    if remaining <= 0:
        raise TimeoutError("Three-hour sequence deadline reached")
    return process.wait(timeout=remaining)


def stop(_signum, _frame):
    if child is not None and child.poll() is None:
        child.send_signal(signal.SIGINT)
    raise KeyboardInterrupt


def command(arguments, stage, maximum):
    global child
    save(stage=stage)
    child = subprocess.Popen([sys.executable, "-B", str(BASE / arguments[0]), *arguments[1:]])
    code = wait(child, maximum)
    child = None
    if code:
        raise RuntimeError(stage + " returned " + str(code))


def trial(name, renderer, seconds, compare=None):
    global monitor
    save(current_run=name, renderer=renderer, requested_seconds=seconds, stage="admission")
    monitor = subprocess.Popen([sys.executable, "-B", str(BASE / "watch.py"), name,
                                "--maximum-seconds", str(min(5400, seconds + 1500))])
    command(["observed_launch.py", name, "both", "--seconds", str(seconds),
             "--control-interval", "300", "--repaint-ms", "33", "--renderer", renderer],
            "workload " + name, seconds + 1000)
    command(["finish_trial.py", name], "owned cleanup " + name, 100)
    monitor_code = wait(monitor, 90)
    monitor = None
    if monitor_code:
        raise RuntimeError("AC/lid monitor returned " + str(monitor_code))
    audit = ["completed-run-audit.py", "--run-directory", str(BASE / name),
             "--expected-head", expected["repository_head"],
             "--expected-soak-sha256", expected["pins"]["soak_source"],
             "--source-snapshot", str(ROOT / "tools/benchmarks/stem_soak.py"),
             "--output", str(BASE / (name + "-independent-audit.json"))]
    if seconds == 900:
        audit.append("--comparison-short")
    command(audit, "saved event audit " + name, 120)
    command(["thermal-snapshot-audit.py", "--run-directory", str(BASE / name),
             "--output", str(BASE / (name + "-thermal-snapshot-audit.json"))],
            "saved thermal audit " + name, 120)
    cadence = ["cadence_audit.py", name]
    if compare:
        cadence += ["--compare-control", compare]
    command(cadence, "saved cadence and pin audit " + name, 120)
    record["completed_runs"].append(name)
    save(stage="completed " + name)


for sig in (signal.SIGINT, signal.SIGTERM):
    signal.signal(sig, stop)
save(stage="prepared source admission")
try:
    trial("vulkan-33ms-15min", "vulkan", 900)
    command(["cooldown.py", "software-33ms-15min"], "cooldown before software comparison", 1220)
    trial("software-33ms-15min", "software", 900, compare="vulkan-33ms-15min")
    result = json.loads((BASE / "software-33ms-15min/result.json").read_text())
    cadence = json.loads((BASE / "software-33ms-15min-cadence-audit.json").read_text())
    verification = json.loads((BASE / "software-33ms-15min-verification.json").read_text())
    p99 = result["timing"]["continuous"]["hop_service_percentiles_ms"]["99"]
    selection = {"software_900s_continuous_p99_ms": p99, "comparison_threshold_ms": 4095,
                 "existing_checks_valid": verification["small_report_and_lifecycle_checks_passed"]
                     and cadence["status"] == "valid" and not result["failures"],
                 "scope": "Selection of the conditional hour only; not a new acceptance gate or proof that M0 passes"}
    selection["run_software_hour"] = p99 <= 4095 and selection["existing_checks_valid"]
    save(selection=selection)
    if selection["run_software_hour"]:
        command(["cooldown.py", "software-33ms-hour"], "cooldown before conditional software hour", 1220)
        trial("software-33ms-hour", "software", 3600)
    save(status="completed", current_run=None, stage="comparison complete; report pending",
         software_hour_completed="software-33ms-hour" in record["completed_runs"])
except BaseException as error:
    save(status="stopped", error=f"{type(error).__name__}: {error}")
    raise
finally:
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, signal.SIG_IGN)
    for process in (child, monitor):
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
    (BASE / "software-series-stop").touch()

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Explicit host opt-in wrapper for native quiet-audio/UI playback qualification."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time


def read_command(command, timeout=3):
    value = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                           stdin=subprocess.DEVNULL)
    if value.returncode:
        raise RuntimeError(f"Read-only command failed: {command[0]}: {value.stderr.strip()}")
    return value.stdout.strip()


def save(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def pin(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    info = path.stat()
    return {"path": str(path), "bytes": info.st_size, "sha256": value.hexdigest()}


def process_identity(pid):
    root = Path("/proc") / str(pid)
    if root.stat().st_uid != os.getuid():
        raise RuntimeError("Observed PID is not owned by the current user")
    fields = (root / "stat").read_text().rsplit(")", 1)[1].split()
    return {"pid": pid, "start_ticks": int(fields[19]),
            "command": [part.decode(errors="replace") for part in (root / "cmdline").read_bytes().split(b"\0") if part]}


def guard_identity(script, service_file):
    raw = read_command(["systemctl", "--user", "show", "thermal-guard.service",
                        "--property=ActiveState,MainPID,InvocationID,ExecMainStartTimestampMonotonic"])
    fields = dict(line.split("=", 1) for line in raw.splitlines() if "=" in line)
    if fields.get("ActiveState") != "active" or int(fields.get("MainPID", 0)) <= 0:
        raise RuntimeError("Existing thermal-guard.service must remain active")
    identity = process_identity(int(fields["MainPID"]))
    if str(script) not in identity["command"]:
        raise RuntimeError("Active thermal guard does not match the pinned script")
    return {"service": fields, "process": identity, "script": pin(script), "unit": pin(service_file)}


def power_state():
    supplies = []
    for root in sorted(Path("/sys/class/power_supply").glob("*")):
        try:
            kind = (root / "type").read_text().strip()
            if kind in ("Mains", "USB", "USB_C", "USB_PD") and (root / "online").exists():
                supplies.append({"name": root.name, "type": kind, "online": int((root / "online").read_text())})
        except (OSError, ValueError):
            continue
    lids = {}
    for path in sorted(Path("/proc/acpi/button/lid").glob("*/state")):
        lids[str(path)] = path.read_text().strip()
    if lids:
        lid_open = all(value.lower().endswith("open") for value in lids.values())
    else:
        raw = read_command(["busctl", "--system", "get-property", "org.freedesktop.login1",
                            "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "LidClosed"])
        if raw not in ("b true", "b false"):
            raise RuntimeError("Cannot establish the laptop lid state")
        lids["login1.LidClosed"] = raw
        lid_open = raw == "b false"
    return {"ac_online": any(item["online"] == 1 for item in supplies), "supplies": supplies,
            "lid_open": lid_open, "lid_sources": lids}


def sensor_paths():
    values = {}
    for root in sorted(Path("/sys/class/thermal").glob("thermal_zone*")):
        try:
            kind = (root / "type").read_text().strip()
            if kind in ("acpitz", "TCPU", "x86_pkg_temp"):
                values[str(root / "temp")] = kind
        except OSError:
            continue
    for root in sorted(Path("/sys/class/hwmon").glob("hwmon*")):
        try:
            if (root / "name").read_text().strip() == "coretemp":
                for path in sorted(root.glob("temp*_input")):
                    values[str(path)] = "coretemp"
        except OSError:
            continue
    if not values:
        raise RuntimeError("No CPU/ACPI temperature sensors used by the existing guard are readable")
    return values


def observation(paths):
    temperatures, errors = {}, {}
    for name in paths:
        try:
            temperatures[name] = int(Path(name).read_text().strip())
        except (OSError, ValueError) as error:
            errors[name] = str(error)
    policy = {}
    for path in (Path("/sys/firmware/acpi/platform_profile"),
                 Path("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference")):
        try:
            policy[str(path)] = path.read_text().strip()
        except OSError as error:
            errors[str(path)] = str(error)
    return {"monotonic_ns": time.monotonic_ns(), "at_utc": datetime.now(timezone.utc).isoformat(),
            "temperatures_millidegrees_C": temperatures, "profile_epp_read_only": policy, "read_errors": errors}


def thermal_summary(path):
    records = [json.loads(line) for line in path.read_text().splitlines() if line]
    result = {"samples": len(records), "sample_interval_seconds": 2,
              "scope": "Observed CPU/ACPI samples; sampled spans do not establish continuous exposure or exclude shorter spikes"}
    maxima = [max(item["temperatures_millidegrees_C"].values(), default=None) for item in records]
    result["maximum_observed_millidegrees_C"] = max((item for item in maxima if item is not None), default=None)
    for threshold in (85000, 95000):
        start, longest, count = None, 0.0, 0
        for record, maximum in zip(records, maxima):
            if maximum is not None and maximum >= threshold:
                count += 1
                start = record["monotonic_ns"] if start is None else start
                longest = max(longest, (record["monotonic_ns"] - start) / 1e9)
            else:
                start = None
        result[str(threshold)] = {"samples_at_or_above": count, "longest_consecutive_sample_span_seconds": longest}
    result["profile_epp_observations"] = []
    previous = None
    for record in records:
        value = record["profile_epp_read_only"]
        if value != previous:
            result["profile_epp_observations"].append({"at_utc": record["at_utc"], "values": value})
            previous = value
    return result


def owned_objects(objects, pid):
    selected, ids = {}, set()
    for item in objects:
        if str(item.get("info", {}).get("props", {}).get("application.process.id", "")) == str(pid):
            selected[item["id"]] = item
            ids.add(str(item["id"]))
    for _ in range(3):
        for item in objects:
            props = item.get("info", {}).get("props", {})
            if any(str(props.get(key, "")) in ids for key in ("client.id", "node.id", "link.output.node")):
                selected[item["id"]] = item
                ids.add(str(item["id"]))
    return list(selected.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-host-audio", action="store_true", required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--track-a", type=Path, required=True)
    parser.add_argument("--track-b", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="fresh evidence directory")
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("--guard-script", type=Path, default=Path.home() / ".local/bin/thermal-guard")
    parser.add_argument("--guard-unit", type=Path, default=Path.home() / ".config/systemd/user/thermal-guard.service")
    args = parser.parse_args()
    if not 60 <= args.seconds <= 3600:
        parser.error("--seconds must be between 60 and 3600 seconds")
    if os.environ.get("CONFLUX_PRIVATE_ROUTING_CHECK") or os.environ.get("QT_QPA_PLATFORM") in ("offscreen", "minimal"):
        parser.error("Qualification needs the normal native host session, without a private graph or offscreen platform")
    binary, track_a, track_b = (value.resolve(strict=True) for value in (args.binary, args.track_a, args.track_b))
    output = args.output.absolute()
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    stop = output / "stop"
    cache = output / "cache"
    cache.mkdir(mode=0o700)
    result = {"schema": 1, "passed": False, "requested_seconds": args.seconds,
              "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "Explicit native host quiet-fixture playback; no device/speaker/profile/guard changes by this runner"}
    child, owned_identity = None, None
    owned_graph_identities = set()
    requested_stop = None
    previous_handlers = {}
    began = time.monotonic()

    def request_stop(reason):
        nonlocal requested_stop
        if requested_stop is None:
            requested_stop = reason
            stop.write_text(reason + "\n", encoding="utf-8")

    try:
        for name in ("systemctl", "pw-dump"):
            if not shutil.which(name):
                raise RuntimeError(f"Required installed read-only command is unavailable: {name}")
        result["pins"] = {"binary": pin(binary), "track_a": pin(track_a), "track_b": pin(track_b),
                          "runner": pin(Path(__file__).resolve())}
        result["guard_before"] = guard_identity(args.guard_script.resolve(strict=True), args.guard_unit.resolve(strict=True))
        result["power_before"] = power_state()
        if not result["power_before"]["ac_online"] or not result["power_before"]["lid_open"]:
            raise RuntimeError("Connect AC power and leave the laptop lid open before the soak")
        paths = sensor_paths()
        result["temperature_sensor_types"] = paths
        result["host_before"] = observation(paths)
        # Recording is 384000 bytes/s; allow prepared copies and margin.
        needed = args.seconds * 384000 + 768 * 1024 * 1024
        result["required_free_bytes"] = needed
        result["free_bytes_before"] = shutil.disk_usage(output).free
        if result["free_bytes_before"] < needed:
            raise RuntimeError("Insufficient free disk for the bounded stereo recording and prepared cache")
        environment = os.environ.copy()
        environment.update({"CONFLUX_ALLOW_HOST_SOAK": "1", "XDG_CACHE_HOME": str(cache)})
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous_handlers[sig] = signal.signal(sig, lambda number, frame: request_stop(f"runner signal {number}"))
        with (output / "native.stdout.log").open("wb") as stdout, (
                output / "native.stderr.log").open("wb") as stderr, (
                output / "host-observations.jsonl").open("w", encoding="utf-8") as observations:
            child = subprocess.Popen([str(binary), str(track_a), str(track_b), str(output), str(args.seconds), str(stop)],
                                     env=environment, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                     start_new_session=True)
            owned_identity = process_identity(child.pid)
            result["owned_process"] = owned_identity
            next_sample, next_identity = 0.0, 0.0
            stop_requested_at = None
            seen_progress = False
            while child.poll() is None:
                elapsed = time.monotonic() - began
                if stop.exists() and requested_stop is None:
                    request_stop("user stop file")
                if elapsed >= next_sample:
                    host = observation(paths)
                    host["power"] = power_state()
                    observations.write(json.dumps(host) + "\n"); observations.flush()
                    if not host["power"]["ac_online"]:
                        request_stop("AC power removed")
                    if not host["power"]["lid_open"]:
                        request_stop("Laptop lid closed")
                    if not host["temperatures_millidegrees_C"]:
                        request_stop("CPU/ACPI temperature observation unavailable")
                    next_sample = elapsed + 2
                if elapsed >= next_identity:
                    identity = guard_identity(args.guard_script.resolve(), args.guard_unit.resolve())
                    if identity != result["guard_before"]:
                        request_stop("Existing thermal guard identity/source changed")
                    next_identity = elapsed + 10
                progress = output / "progress.json"
                if progress.exists():
                    pulse = json.loads(progress.read_text())
                    seen_progress = True
                    age = (time.monotonic_ns() - pulse["monotonic_ns"]) / 1e9
                    if pulse["phase"] in ("steady", "seek_settling") and age > 10:
                        request_stop("Native audio/UI progress stale for ten seconds")
                    result["last_progress"] = pulse
                    if pulse["phase"] in ("steady", "seek_settling") and not owned_graph_identities:
                        objects = json.loads(read_command(["pw-dump", "-N"], timeout=8))
                        owned = owned_objects(objects, child.pid)
                        save(output / "owned-pipewire-before.json", {"pid": child.pid, "objects": owned})
                        owned_graph_identities = {(item["id"], str(item.get("info", {}).get("props", {}).get(
                            "object.serial", ""))) for item in owned}
                        result["owned_graph_object_count"] = len(owned)
                if not seen_progress and elapsed > 30:
                    request_stop("Native tool did not publish startup progress")
                if elapsed > args.seconds + 180:
                    request_stop("Bounded soak wall deadline exceeded")
                if requested_stop is not None:
                    stop_requested_at = stop_requested_at or time.monotonic()
                    if time.monotonic() - stop_requested_at > 45:
                        child.terminate()
                        try:
                            child.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            child.kill(); child.wait(timeout=5)
                        result["forced_stop"] = True
                time.sleep(.2)
            result["native_exit"] = child.returncode
        if stop.exists() and requested_stop is None:
            request_stop("user stop file")
        native = output / "native-result.json"
        if native.exists():
            result["native"] = json.loads(native.read_text())
        else:
            result["native"] = {"passed": False, "error": "Native result is missing"}
        result["host_after"] = observation(paths)
        result["thermal_summary"] = thermal_summary(output / "host-observations.jsonl")
        result["power_after"] = power_state()
        result["guard_after"] = guard_identity(args.guard_script.resolve(), args.guard_unit.resolve())
        result["guard_identity_unchanged"] = result["guard_before"] == result["guard_after"]
        result["passed"] = (child.returncode == 0 and result["native"].get("passed", False) and requested_stop is None
                            and result["guard_identity_unchanged"] and bool(owned_graph_identities)
                            and result["power_after"]["ac_online"] and result["power_after"]["lid_open"])
    except Exception as error:
        result["error"] = str(error)
        if child is not None and child.poll() is None:
            request_stop("Runner observation error")
            try:
                child.wait(timeout=45)
            except subprocess.TimeoutExpired:
                result["forced_stop"] = True
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait(timeout=5)
    finally:
        result["stop_reason"] = requested_stop
        for sig, previous in previous_handlers.items():
            signal.signal(sig, previous)
        if child is not None:
            result["owned_process_reaped"] = child.poll() is not None
            try:
                result["owned_process_identity_gone"] = process_identity(child.pid) != owned_identity
            except FileNotFoundError:
                result["owned_process_identity_gone"] = True
            except Exception as error:
                result["process_cleanup_error"] = str(error)
            try:
                objects = json.loads(read_command(["pw-dump", "-N"], timeout=8))
                remaining = [item for item in objects if (
                    item["id"], str(item.get("info", {}).get("props", {}).get("object.serial", ""))
                ) in owned_graph_identities or str(item.get("info", {}).get("props", {}).get(
                    "application.process.id", "")) == str(child.pid)]
                save(output / "owned-pipewire-cleanup.json", {"pid": child.pid, "remaining_objects": remaining})
                result["owned_pipewire_objects_gone"] = not remaining
            except Exception as error:
                result["pipewire_cleanup_error"] = str(error)
            if not result.get("owned_process_identity_gone") or not result.get("owned_pipewire_objects_gone"):
                result["passed"] = False
        shutil.rmtree(cache)
        result["owned_cache_removed"] = not cache.exists()
        result["elapsed_seconds"] = time.monotonic() - began
        result["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        save(output / "result.json", result)
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 2 if requested_stop is not None else 1


if __name__ == "__main__":
    raise SystemExit(main())

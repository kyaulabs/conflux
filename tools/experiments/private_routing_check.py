#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Run the manual routing binary on an isolated, hardware-free PipeWire daemon."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path, help="new evidence directory; must not exist")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output.absolute()
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    recordings = output / "recordings"
    recordings.mkdir(mode=0o700)
    for name in ("pipewire", "pw-dump"):
        if not shutil.which(name):
            raise SystemExit(f"Required installed program is unavailable: {name}")
    result = {"passed": False, "scope": "Private virtual daemon; no ALSA, session manager, D-Bus, host discovery or hardware output"}
    with tempfile.TemporaryDirectory(prefix="conflux-routing-private-") as temporary:
        runtime = Path(temporary)
        runtime.chmod(0o700)
        remote = f"conflux-private-routing-{os.getpid()}"
        config = runtime / "pipewire.conf"
        config.write_text("""
context.properties = {
    core.daemon = true
    core.name = "%s"
    log.level = 3
    default.clock.rate = 48000
    default.clock.allowed-rates = [ 48000 ]
    default.clock.quantum = 256
    default.clock.min-quantum = 256
    default.clock.max-quantum = 256
    settings.check-quantum = true
}
context.spa-libs = { support.* = support/libspa-support }
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-access args = {
        access.socket = {
            "%s" = "unrestricted"
            "%s-manager" = "unrestricted"
        }
    } }
]
context.objects = [
    { factory = spa-node-factory args = {
        factory.name = support.node.driver
        node.name = Dummy-Driver
        node.group = pipewire.dummy
        priority.driver = 20000
    } }
]
""" % (remote, remote, remote), encoding="utf-8")
        client = runtime / "client.conf"
        client.write_text("""
context.properties = { log.level = 2 }
context.spa-libs = {
    support.* = support/libspa-support
    audio.convert.* = audioconvert/libspa-audioconvert
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-metadata }
]
""", encoding="utf-8")
        shutil.copyfile(config, output / "private-daemon.conf")
        shutil.copyfile(client, output / "private-client.conf")
        environment = os.environ.copy()
        environment.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "PIPEWIRE_RUNTIME_DIR": str(runtime),
            "PIPEWIRE_REMOTE": remote,
            "PIPEWIRE_CONFIG_DIR": str(runtime),
            "PIPEWIRE_CONFIG_NAME": "client.conf",
            "PIPEWIRE_CONFIG_PREFIX": "",
            "CONFLUX_PRIVATE_ROUTING_CHECK": "1",
            "CONFLUX_ROUTING_DIAGNOSTICS": "1",
            "PIPEWIRE_DEBUG": "2",
        })
        # Prevent user drop-ins from extending the explicit daemon config.
        environment["XDG_CONFIG_HOME"] = str(runtime / "config")
        environment["XDG_CONFIG_DIRS"] = str(runtime / "config-dirs")
        environment.pop("PIPEWIRE_PROPS", None)
        environment.pop("PIPEWIRE_NO_CONFIG", None)
        daemon = None
        checker = None

        def dump_graph(prefix):
            """Preserve partial output and errors even when a query stalls."""
            with (output / f"{prefix}.json").open("wb") as stdout, (
                    output / f"{prefix}.stderr.log").open("wb") as stderr:
                command = ["pw-dump", "-r", remote, "-N"]
                dump = subprocess.Popen(command, env=environment, stdin=subprocess.DEVNULL,
                                        stdout=stdout, stderr=stderr)
                try:
                    code = dump.wait(timeout=5)
                    return {"exit": code, "timed_out": False}
                except subprocess.TimeoutExpired:
                    dump.kill()
                    dump.wait(timeout=5)
                    return {"exit": dump.returncode, "timed_out": True}

        try:
            with (output / "daemon.log").open("wb") as log:
                daemon_environment = environment | {"PIPEWIRE_CONFIG_NAME": "pipewire.conf", "PIPEWIRE_DEBUG": "3"}
                daemon = subprocess.Popen(
                    ["pipewire", "-c", str(config)], env=daemon_environment,
                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                    start_new_session=True)
                deadline = time.monotonic() + 5
                socket = runtime / remote
                while not socket.exists():
                    if daemon.poll() is not None:
                        raise RuntimeError("Private daemon exited before creating its socket; see daemon.log")
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Private daemon socket startup timed out")
                    time.sleep(.02)
                if not stat.S_ISSOCK(socket.lstat().st_mode):
                    raise RuntimeError("Private endpoint is not a Unix socket")
                result["preflight_dump"] = dump_graph("preflight-pw-dump")
                if result["preflight_dump"]["exit"]:
                    raise RuntimeError("Private pw-dump preflight failed before any filter clients; see preflight logs")
                with (output / "check.stdout.json").open("wb") as stdout, (
                        output / "check.stderr.log").open("wb") as stderr:
                    checker = subprocess.Popen(
                        [str(binary), str(runtime), str(recordings)], env=environment,
                        stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
                    deadline = time.monotonic() + 90
                    failure_captured = False
                    while checker.poll() is None:
                        if (output / "live-failure.json").exists() and not failure_captured:
                            result["failure_dump"] = dump_graph("failure-pw-dump")
                            (output / "release-failure").touch()
                            failure_captured = True
                        if time.monotonic() >= deadline:
                            checker.kill()
                            checker.wait(timeout=5)
                            raise RuntimeError("Private routing harness timed out")
                        time.sleep(.02)
                result["check_exit"] = checker.returncode
                try:
                    result["check"] = json.loads((output / "check.stdout.json").read_text(encoding="utf-8"))
                except json.JSONDecodeError:
                    result["check"] = {"passed": False, "error": "Binary did not emit one JSON document"}

                # This query uses the same explicit private environment. It runs
                # only after the harness has destroyed all clients and sinks.
                result["cleanup_dump"] = dump_graph("cleanup-pw-dump")
                if result["cleanup_dump"]["exit"]:
                    raise RuntimeError("Could not inspect private graph cleanup")
                objects = json.loads((output / "cleanup-pw-dump.json").read_text(encoding="utf-8"))
                remaining = []
                for item in objects:
                    kind = item.get("type", "")
                    props = item.get("info", {}).get("props", {})
                    if kind.endswith(":Link") or (
                            kind.endswith(":Node") and props.get("node.name") != "Dummy-Driver"):
                        remaining.append({"id": item.get("id"), "type": kind,
                                          "node_name": props.get("node.name")})
                result["cleanup"] = {"remaining_nodes_or_links": remaining,
                                     "only_dummy_driver_remains": not remaining}
                result["passed"] = checker.returncode == 0 and result["check"].get("passed", False) and not remaining
        except Exception as error:
            result["error"] = str(error)
        finally:
            if checker is not None and checker.poll() is None:
                checker.kill()
                checker.wait(timeout=5)
            if daemon is not None:
                if daemon.poll() is None:
                    os.killpg(daemon.pid, signal.SIGTERM)
                    try:
                        daemon.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(daemon.pid, signal.SIGKILL)
                        daemon.wait(timeout=5)
                result["daemon_reaped"] = daemon.poll() is not None
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

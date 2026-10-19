#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Scoped guard snapshot: retain owned KWin/idle behavior and safe child publication."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import uuid

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib


INTERFACE = "com.kyaulabs.conflux.BenchmarkGuard"
OBJECT = "/com/kyaulabs/conflux/BenchmarkGuard"
XML = f"""<node><interface name="{INTERFACE}">
<method name="ShouldRestore"><arg type="b" direction="out"/></method>
<method name="Applied"><arg type="s" direction="in"/></method>
<method name="Restored"><arg type="s" direction="in"/></method>
<method name="Failure"><arg type="s" direction="in"/></method>
</interface></node>"""


def javascript(pid, owner, plugin):
    return """// Temporary CONFLUX benchmark guard; exact PID and app ID only.
const wantedPid = PID;
const owner = OWNER;
const plugin = PLUGIN;
let guarded = null;
let saved = null;
let done = false;
const timer = new QTimer();
function state(w) {
    if (!w) return null;
    const ids = [];
    for (let i = 0; i < w.desktops.length; ++i) ids.push(String(w.desktops[i].id));
    return {pid:Number(w.pid), appId:String(w.desktopFileName),
            keepAbove:w.keepAbove, onAllDesktops:w.onAllDesktops,
            minimized:w.minimized, desktopIds:ids};
}
function report(method, data) {
    callDBus(owner, "/com/kyaulabs/conflux/BenchmarkGuard",
             "com.kyaulabs.conflux.BenchmarkGuard", method, JSON.stringify(data));
}
function restore(reason, notify) {
    if (done) return;
    done = true;
    timer.stop();
    try {
        if (guarded && Number(guarded.pid) === wantedPid) {
            guarded.keepAbove = saved.keepAbove;
            guarded.desktops = saved.desktops;
            guarded.minimized = saved.minimized;
        }
        if (notify) report("Restored", {reason:reason, after:state(guarded)});
    } catch (error) {
        if (notify) report("Failure", {phase:"restore", error:String(error)});
    }
    workspace.windowAdded.disconnect(manage);
    workspace.windowRemoved.disconnect(removed);
    callDBus("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript", plugin);
}
function manage(w) {
    if (done || guarded || Number(w.pid) !== wantedPid ||
            String(w.desktopFileName) !== "com.kyaulabs.conflux") return;
    const desktops = [];
    for (let i = 0; i < w.desktops.length; ++i) desktops.push(w.desktops[i]);
    saved = {keepAbove:w.keepAbove, desktops:desktops, minimized:w.minimized};
    const before = state(w);
    guarded = w;
    w.keepAbove = true;
    w.onAllDesktops = true;
    w.minimized = false;
    report("Applied", {before:before, after:state(w)});
}
function removed(w) {
    if (w === guarded) {
        guarded = null;
        restore("owned-window-closed", true);
    }
}
function checkOwner() {
    if (done) return;
    callDBus("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
             "NameHasOwner", owner, function(present) {
        if (!present) { restore("controller-disconnected", false); return; }
        callDBus(owner, "/com/kyaulabs/conflux/BenchmarkGuard",
                 "com.kyaulabs.conflux.BenchmarkGuard", "ShouldRestore", function(stop) {
            if (stop) restore("controller-stop", true);
        });
    });
}
workspace.windowAdded.connect(manage);
workspace.windowRemoved.connect(removed);
const windows = workspace.windowList();
for (let i = 0; i < windows.length; ++i) manage(windows[i]);
timer.interval = 500;
timer.timeout.connect(checkOwner);
timer.start();
""".replace("PID", str(pid)).replace("OWNER", json.dumps(owner)).replace("PLUGIN", json.dumps(plugin))


def process_info(pid):
    root = Path("/proc") / str(pid)
    if root.stat().st_uid != os.getuid():
        raise RuntimeError("UI PID is not owned by the current user")
    fields = (root / "stat").read_text().rsplit(")", 1)[1].split()
    return int(fields[1]), int(fields[19]), (root / "cmdline").read_bytes().split(b"\0")


class Guard:
    def __init__(self, args):
        self.args = args
        self.record = {"command": args.command, "started_at_utc": datetime.now(timezone.utc).isoformat(),
                       "status": "starting", "ready": False, "errors": [],
                       "idle_only": args.idle_only,
                       "window_guard": {"requested": not args.idle_only,
                                        "status": "not_run" if args.idle_only else "pending"}}
        self.connection = None
        self.child = None
        self.child_group = None
        self.cookie = None
        self.plugin = "conflux-benchmark-" + uuid.uuid4().hex
        self.script_id = None
        self.stopping = False
        self.stop_at = None
        self.launched = None
        self.ui_pid = None
        self.loop = GLib.MainLoop()

    def save(self):
        temporary = self.args.output.with_name(self.args.output.name + ".tmp")
        temporary.write_text(json.dumps(self.record, indent=2) + "\n")
        temporary.replace(self.args.output)

    def call(self, service, path, interface, method, signature=None, values=()):
        parameters = GLib.Variant(signature, values) if signature else None
        return self.connection.call_sync(service, path, interface, method, parameters, None,
                                         Gio.DBusCallFlags.NONE, 5000, None)

    def method(self, connection, sender, path, interface, method, parameters, invocation):
        if method == "ShouldRestore":
            invocation.return_value(GLib.Variant("(b)", (self.stopping,)))
            return
        try:
            value = json.loads(parameters.unpack()[0])
            if method == "Applied":
                after = value["after"]
                if (after["pid"] != self.ui_pid or not after["keepAbove"]
                        or not after["onAllDesktops"] or after["minimized"]):
                    raise RuntimeError("KWin did not apply the requested owned-window state")
                self.record.update(status="active", ready=True, window_policy=value,
                                   applied_monotonic=time.monotonic())
                self.record["window_guard"]["status"] = "active"
            elif method == "Restored":
                self.record.update(ready=False, restored=value, restored_monotonic=time.monotonic())
                self.record["window_guard"]["status"] = "restored"
            else:
                raise RuntimeError(f"KWin script reported {value}")
            self.save()
            invocation.return_value(None)
        except Exception as error:
            invocation.return_dbus_error(INTERFACE + ".Error", str(error))
            self.record["errors"].append(str(error))
            self.request_stop()

    def request_stop(self, *_):
        if self.stopping:
            return
        self.stopping = True
        self.stop_at = time.monotonic()
        if self.child is not None:
            try:
                os.killpg(self.child.pid, signal.SIGINT)
            except ProcessLookupError:
                pass

    def finish_child_group(self):
        if self.child_group is None:
            return
        if self.child_group <= 1 or self.child_group == os.getpgrp():
            raise RuntimeError("Refusing unsafe owned guard child group")
        cleanup = self.record["child_group_cleanup"] = {"group": self.child_group,
            "started_monotonic": time.monotonic(), "disappearance_observed": False}
        # The group remains owned even when its observer leader already exited.
        try:
            os.killpg(self.child_group, signal.SIGINT)
        except ProcessLookupError:
            pass
        if self.child.poll() is None:
            try:
                self.child.wait(timeout=20)
            except subprocess.TimeoutExpired:
                pass
        try:
            os.killpg(self.child_group, signal.SIGKILL)
        except ProcessLookupError:
            pass
        until = time.monotonic() + 2
        while True:
            self.child.poll()
            try:
                os.killpg(self.child_group, 0)
            except ProcessLookupError:
                cleanup.update(disappearance_observed=True, finished_monotonic=time.monotonic())
                break
            if time.monotonic() >= until:
                raise RuntimeError("Owned guard child group remains after bounded cleanup")
            time.sleep(.05)
        self.child.wait(timeout=5)

    def start_script(self, pulse):
        pid = pulse["pid"]
        if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
            raise RuntimeError("invalid UI PID in pulse")
        parent, start, command = process_info(pid)
        if b"--smoke-test" not in command:
            raise RuntimeError("refusing to modify a UI outside smoke-test mode")
        ancestor = pid
        for _ in range(64):
            if ancestor == self.child.pid:
                break
            if ancestor <= 1:
                raise RuntimeError("UI PID is not a descendant of the owned trial")
            ancestor = process_info(ancestor)[0]
        else:
            raise RuntimeError("UI ancestry exceeded bound")
        self.ui_pid = pid
        self.record["ui"] = {"pid": pid, "start_ticks": start,
                             "command": [part.decode(errors="replace") for part in command if part]}
        script = self.args.output.with_suffix(".js")
        script.write_text(javascript(pid, self.connection.get_unique_name(), self.plugin))
        self.script_id = self.call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
                                   "loadScript", "(ss)", (str(script), self.plugin)).unpack()[0]
        if self.script_id < 0:
            raise RuntimeError("KWin could not load the temporary script")
        self.record.update(script_id=self.script_id, script_plugin=self.plugin, script_path=str(script))
        self.save()
        self.call("org.kde.KWin", f"/Scripting/Script{self.script_id}", "org.kde.kwin.Script", "run")

    def tick(self):
        try:
            code = self.child.poll()
            if code is not None:
                self.stopping = True
                self.stop_at = self.stop_at or time.monotonic()
                if self.script_id is None or self.record.get("restored") or time.monotonic() - self.stop_at > 4:
                    self.loop.quit()
                    return False
            elif (not self.args.idle_only and not self.stopping and self.script_id is None
                  and self.args.ui_progress.exists()):
                pulse = json.loads(self.args.ui_progress.read_text())
                if pulse["timestamp_monotonic_ns"] / 1e9 >= self.launched:
                    self.start_script(pulse)
            if (not self.args.idle_only and not self.record.get("window_policy")
                    and time.monotonic() - self.launched > 15):
                raise RuntimeError("owned UI did not acquire its KWin guard within 15 seconds")
            if self.stopping and time.monotonic() - self.stop_at > 20 and self.child.poll() is None:
                os.killpg(self.child.pid, signal.SIGKILL)
        except Exception as error:
            self.record["errors"].append(f"{type(error).__name__}: {error}")
            self.request_stop()
        return True

    def run(self):
        result = 1
        previous = {}
        try:
            for sig in (signal.SIGINT, signal.SIGTERM):
                previous[sig] = signal.signal(sig, self.request_stop)
            self.save()
            self.connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
            if not self.args.idle_only:
                self.connection.register_object(OBJECT, Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0],
                                                self.method, None, None)
            self.cookie = self.call("org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver",
                                    "Inhibit", "(ss)", ("com.kyaulabs.conflux", "CONFLUX M0 stem benchmark")).unpack()[0]
            self.record.update(idle_inhibit_cookie=self.cookie, bus_owner=self.connection.get_unique_name())
            if self.args.idle_only:
                self.record.update(status="active", ready=True)
                self.save()
            if self.stopping:
                raise InterruptedError("Stop requested before observer launch")
            self.launched = time.monotonic()
            self.child = subprocess.Popen(self.args.command, start_new_session=True)
            self.child_group = self.child.pid
            self.record["owned_child_group"] = self.child_group
            if self.stopping:
                # request_stop is non-raising, so publication completes first.
                try:
                    os.killpg(self.child_group, signal.SIGINT)
                except ProcessLookupError:
                    pass
            GLib.timeout_add(200, self.tick)
            self.loop.run()
            result = self.child.wait(timeout=5)
            if result < 0:
                result = 128 - result
        except Exception as error:
            self.record["errors"].append(f"{type(error).__name__}: {error}")
        finally:
            self.request_stop()
            try:
                self.finish_child_group()
            except Exception as error:
                self.record["errors"].append(f"child_cleanup: {error}")
            if self.connection is not None:
                try:
                    if self.script_id is not None:
                        if not self.record.get("restored"):
                            self.record["errors"].append("KWin did not acknowledge owned-window restoration")
                        self.call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
                                  "(s)", (self.plugin,))
                        loaded = self.call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "isScriptLoaded",
                                           "(s)", (self.plugin,)).unpack()[0]
                        self.record["script_unloaded"] = not loaded
                        if loaded:
                            self.record["errors"].append("owned KWin script remained loaded")
                except Exception as error:
                    self.record["errors"].append(f"script_cleanup: {error}")
                try:
                    if self.cookie is not None:
                        self.call("org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver",
                                  "UnInhibit", "(u)", (self.cookie,))
                        self.record["idle_inhibit_released"] = True
                except Exception as error:
                    self.record["errors"].append(f"idle_cleanup: {error}")
                self.connection.close_sync(None)
            for sig, handler in previous.items():
                signal.signal(sig, handler)
            if self.record["errors"]:
                result = 1
            self.record.update(status="ok" if result == 0 else "error", ready=False,
                               child_returncode=self.child.returncode if self.child else None,
                               returncode=result, finished_at_utc=datetime.now(timezone.utc).isoformat())
            self.save()
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ui-progress", type=Path)
    parser.add_argument("--idle-only", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if not args.command:
        parser.error("supply one owned trial command after --")
    if not args.idle_only and args.ui_progress is None:
        parser.error("--ui-progress is required unless --idle-only is selected")
    args.output = args.output.resolve()
    if args.ui_progress is not None:
        args.ui_progress = args.ui_progress.resolve()
    return Guard(args).run()


if __name__ == "__main__":
    raise SystemExit(main())

// Temporary CONFLUX benchmark guard; exact 1918608 and app ID only.
const wantedPid = 1918608;
const owner = ":1.50106";
const plugin = "conflux-benchmark-dc44e6fd33494a918967fe90429e721a";
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

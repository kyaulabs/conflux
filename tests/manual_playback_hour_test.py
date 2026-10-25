#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fast admission and evidence regressions; never opens a host audio device."""
import contextlib
import copy
import ast
from datetime import datetime, timedelta, timezone
import errno
import hashlib
import io
import inspect
import os
import json
from pathlib import Path
import sys
import tempfile
import textwrap
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.experiments import run_manual_playback_soak as soak
from tools.experiments import private_routing_check as routing


class HourPlaybackTest(unittest.TestCase):
    def controls_fixture(self, functional=False):
        # Independent fixed target table. B starts selected so restoring false
        # unconditionally is a real regression, rather than a passing no-op.
        baseline = {"deck_gains": [.25, 1.], "eq_db": [[0., 0., 0.], [0., 0., 0.]],
                    "crossfader": .5, "cue_selected": [False, True], "master_gain": .25,
                    "cue_gain": 1., "stem_gains": [[1.] * 4, [0.] * 4]}
        snapshot = baseline | {"output_frame": 1000, "source_frames": [0, 10000],
            "source_lengths": [1123201, 14400000], "source_generations": [1, 2], "transport_revisions": [1, 1],
            "cue_frames": [0, 0], "cue_held": [False, False], "cue_preview": [False, False],
            "playing": [True, True], "applied": 10, "last_command_id": 10, "callbacks": 1,
            "invalid_buffers": 0, "rejected": 0, "starved": [0, 0], "read_failure": [False, False]}
        native = {"mode": "live-stems-controls", "passed": True, "cancelled": False, "shutdown_completed": True,
            "functional_only": functional, "host_audio": not functional, "requested_seconds": 10 if functional else 120,
            "ui_mode": "visible", "window_policy": "visible", "manual_controls_complete": True,
            "manual_controls_baseline": copy.deepcopy(snapshot), "manual_controls": [],
            "measurement_intervals_closed": True, "last_recovery_complete": True,
            "measured_wall_ms": 10000 if functional else 120000, "measurement_begin_output_frame": 1000,
            "measurement_end_output_frame": 481000 if functional else 5761000, "steady_starved_frames": 0,
            "starved_frames_outside_measured_intervals": 0, "invalid_buffers": 0, "rejected_commands": 0,
            "source_read_failures": 0, "execution_deadline_misses": 0, "scene_errors": [], "frames_presented": 100,
            "maximum_gui_heartbeat_gap_ns": 20000000, "maximum_frame_presentation_gap_ns": 20000000,
            "intervals": [{"kind": "steady", "starved_frames": 0}],
            "timing": [{"driver_position_observed": True, "driver_position_discontinuities": 0,
                        "driver_recovery_flags": 0, "graph_quantum_min": 256, "graph_quantum_max": 256}]}
        for deck in range(2):
            targets = (("gain", .125 if deck == 0 else .5), ("gain", baseline["deck_gains"][deck]),
                       ("cross", .25 if deck == 0 else .75), ("cross", .5), ("eq", -6.5), ("eq", 0.),
                       ("cue", not baseline["cue_selected"][deck]), ("cue", baseline["cue_selected"][deck]))
            for step, (action, target) in enumerate(targets):
                index = deck * 8 + step
                due = ((7000 if deck else 500) + step * 100 if functional else
                       (70000 if deck else 10000) + step * 2000)
                before = copy.deepcopy(snapshot)
                before["output_frame"] = 1000 + due * 48
                before["source_frames"] = [(due % 23000) * 48, 10000 + due * 48]
                ack = copy.deepcopy(before)
                if action == "gain": ack["deck_gains"][deck] = target
                elif action == "cross": ack["crossfader"] = target
                elif action == "eq": ack["eq_db"][deck][2] = target
                else: ack["cue_selected"][deck] = target
                ack["output_frame"] += 256; ack["source_frames"] = [x + 256 for x in before["source_frames"]]
                ack["applied"] += 1; ack["last_command_id"] += 1; ack["callbacks"] += 1
                name = {"gain": f"deck{deck}Gain", "cross": "crossfader", "eq": f"deck{deck}EqHI", "cue": f"deck{deck}CueSelect"}[action]
                issued = 1000000000 + due * 1000000
                native["manual_controls"].append({"index": index, "deck": deck, "step": step, "object": name,
                    "due_ms": due, "issued_elapsed_ms": due, "issued_ns": issued, "ack_poll_ns": issued + 20000000,
                    "acknowledged": True, "before": before, "ack": ack, "expected": copy.deepcopy(ack),
                    "expected_command": ack["last_command_id"], "session_before": 0, "session_after": 0,
                    "presented_before": index, "presented_after": index + 1,
                    "ui_value": (.375 if target == -6.5 else .5) if action == "eq" else target})
                snapshot = ack
        observations = {"service_metrics_complete": True, "service_hops": {"complete": True, "count": 21,
                        "nearest_rank_p99_ms": 3012.7}, "kind_counts": {"ChunkValidated": 21},
                        "chunk_phases": {"fields": {key: {"present_for_all_chunks": True, "count": 21}
                                                     for key in soak.LIVE_PHASE_NAMES}}}
        return native, observations

    def test_actual_controls_admission_is_short_visible_only(self):
        _, args = self.cli("--live-stems", "--active-controls")
        self.assertTrue(args.active_controls)
        self.assertEqual(args.seconds, 120)
        for extra in (("--live-stems", "--active-controls", "--background"),
                      ("--live-stems-hour", "--active-controls"), ("--active-controls",)):
            with self.subTest(extra=extra), self.assertRaises(SystemExit): self.cli(*extra)

    def test_actual_controls_targets_restore_both_decks_and_selected_cue(self):
        native, observations = self.controls_fixture()
        self.assertEqual(soak.validate_active_controls(native, observations)["actions"], 16)
        self.assertTrue(native["manual_controls"][-1]["ack"]["cue_selected"][1])
        native, _ = self.controls_fixture(functional=True)
        result = soak.validate_active_controls(native, functional=True)
        self.assertTrue(result["functional_only"])
        self.assertFalse(result["qualified"])
        with self.assertRaises(RuntimeError): soak.validate_active_controls(native, observations)

    def test_actual_controls_reject_wrong_order_target_clock_ownership_and_missing_ack(self):
        native, observations = self.controls_fixture()
        mutations = (
            lambda x: x["manual_controls"].pop(),
            lambda x: x["manual_controls"][0].update(index=1),
            lambda x: x["manual_controls"][1].update(object="deck1Gain"),
            lambda x: x["manual_controls"][0].update(acknowledged=False),
            lambda x: x["manual_controls"][0]["ack"].update(last_command_id=999),
            lambda x: x["manual_controls"][0]["ack"].update(applied=13),
            lambda x: x["manual_controls"][0]["ack"]["deck_gains"].__setitem__(0, .5),
            lambda x: x["manual_controls"][-1]["ack"]["cue_selected"].__setitem__(1, False),
            lambda x: x["manual_controls"][0]["ack"]["transport_revisions"].__setitem__(0, 9),
            lambda x: x["manual_controls"][0]["ack"]["source_generations"].__setitem__(1, 9),
            lambda x: x["manual_controls"][0]["ack"]["source_frames"].__setitem__(1, 7),
            lambda x: x["manual_controls"][0].update(session_after=1),
            lambda x: x["manual_controls"][0].update(presented_after=0),
            lambda x: x["manual_controls"][0].update(ack_poll_ns=x["manual_controls"][0]["issued_ns"] + 5000000000),
            lambda x: x["manual_controls_baseline"]["stem_gains"][1].__setitem__(0, 1.),
            lambda x: x.update(manual_controls_complete=False),
            lambda x: x.update(cancelled=True), lambda x: x.update(shutdown_completed=False),
            lambda x: x.update(steady_starved_frames=256),
            lambda x: x["intervals"][0].update(kind="manual_controls", starved_frames=256),
            lambda x: x.update(execution_deadline_misses=1),
        )
        for index, mutate in enumerate(mutations):
            invalid = copy.deepcopy(native); mutate(invalid)
            with self.subTest(index=index), self.assertRaises(RuntimeError): soak.validate_active_controls(invalid, observations)

    def test_actual_controls_keep_strict_host_service_loss_phase_and_driver_gates(self):
        native, observations = self.controls_fixture()
        mutations = (lambda x: x["service_hops"].update(nearest_rank_p99_ms=4095),
                     lambda x: x.update(service_metrics_complete=False),
                     lambda x: x["chunk_phases"]["fields"][soak.LIVE_PHASE_NAMES[0]].update(count=20))
        for mutate in mutations:
            invalid = copy.deepcopy(observations); mutate(invalid)
            with self.assertRaises(RuntimeError): soak.validate_active_controls(native, invalid)
        native["timing"][0]["driver_recovery_flags"] = 1
        with self.assertRaises(RuntimeError): soak.validate_active_controls(native, observations)

    def test_private_soak_pin_fails_before_daemon_or_child_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "soak"; path.write_bytes(b"bounded executable fixture\n"); path.chmod(0o700)
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertEqual(routing.ui_controls_binary(path, digest, True), path)
            for pin, functional in (("0" * 64, True), (digest, False), ("invalid", True)):
                with self.subTest(pin=pin, functional=functional), self.assertRaises(ValueError):
                    routing.ui_controls_binary(path, pin, functional)
            arguments = ["private", "/unused-preparation-check", str(Path(directory) / "out"),
                         "--functional-only", "--ui-controls-soak", str(path), "--soak-sha256", "0" * 64]
            with patch.object(sys, "argv", arguments), patch.object(routing.subprocess, "Popen") as spawn, \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                routing.main()
            spawn.assert_not_called()

    def live_watchdog_run(self, readiness_seconds, child_ages, progress=False, completed_controls=None):
        # Run the actual launch/watchdog/cleanup path with generated files and a
        # fake child. No host commands, audio, native process or real wait runs.
        clock = [0.0]; launched = []; polls = []; ready_calls = []
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            wrapper = root / "tools/experiments/run_manual_playback_soak.py"
            ui = ("qml/Main.qml", "qml/DeckSurface.qml", "qml/MixerSurface.qml", "qml/DeckButton.qml",
                  "qml/SurfaceKnob.qml", "qml/StereoMeter.qml", "brand/integration/qtquick/ConfluxTheme.qml",
                  "brand/integration/qtquick/qmldir", "brand/assets/logos/conflux-horizontal-dark.svg",
                  "brand/assets/logos/conflux-horizontal-light.svg", "brand/assets/fonts/Barlow-Regular.ttf",
                  "brand/assets/fonts/BarlowCondensed-SemiBold.ttf", "brand/assets/fonts/JetBrainsMono-Regular.ttf")
            names = ("binary", "track_a", "track_b", "python", "exporter", "worker", "service", "runtime", "model",
                     "guard_script", "guard_unit", "readiness_decision")
            assets = {name: root / name for name in names}
            for path in [wrapper, *assets.values(), *(root / name for name in ui)]:
                path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b"generated fixture")
            args = SimpleNamespace(**assets, output=root / "run", seconds=120, live_stems_hour=False,
                background=completed_controls is None, active_controls=completed_controls is not None)
            proc = root / "proc"; task = proc / "101/task/101"; task.mkdir(parents=True)
            fields = ["S", "1", "101", "101"] + ["0"] * 15 + ["1001"]
            (proc / "101/stat").write_text("101 (fixture) " + " ".join(fields))
            (proc / "101/cmdline").write_bytes(b"fixture\0"); (task / "children").write_text("")
            ages = iter(child_ages); exhausted = [False]
            timing = SimpleNamespace(monotonic=lambda: clock[0], monotonic_ns=lambda: int(clock[0] * 1e9))
            class Pipe(io.BytesIO):
                def fileno(self): return 90
            class Child:
                pid = 101
                returncode = None
                def __init__(self, command, **options):
                    self.stdout = Pipe(); self.stderr = Pipe(); launched.append(clock[0])
                    self.command = command
                    if progress:
                        (args.output / "progress.json").write_text(json.dumps({"monotonic_ns": 17, "phase": "steady"}))
                def poll(self):
                    polls.append(clock[0] - launched[0])
                    if self.returncode is None and (exhausted[0] or (args.output / "stop").exists()):
                        self.returncode = 0 if completed_controls is not None else 1
                        (proc / "101/stat").unlink()
                        (args.output / "observations.jsonl").write_text("")
                        native = {
                            "functional_only": False, "host_audio": True, "source_root": str(root),
                            "ui_mode": "background", "ui_coverage": {"visible_ui_qualification_eligible": False},
                            "mode": "live-stems", "setup": {}, "passed": False, "shutdown_completed": True}
                        if completed_controls is not None:
                            native = copy.deepcopy(completed_controls[0])
                            digest = hashlib.sha256(b"generated fixture").hexdigest()
                            native.update(source_root=str(root), setup={key: digest for key in
                                ("source_sha256", "model_sha256", "runtime_sha256", "service_sha256")})
                        (args.output / "native-result.json").write_text(json.dumps(native))
                    return self.returncode
                def wait(self, timeout): return self.poll()
            class Selector:
                def register(self, *values): pass
                def get_map(self): return {}
                def select(self, timeout):
                    try: clock[0] = launched[0] + next(ages)
                    except StopIteration: exhausted[0] = True
                    return []
                def close(self): pass
            def readiness(*values):
                ready_calls.append(clock[0])
                if len(ready_calls) == 2: clock[0] += readiness_seconds
                return {"pin": {"sha256": "offline-only"}}
            def read_command(command, **options):
                if command[0] == "systemctl": return "ActiveState=active\nMainPID=42\nInvocationID=" + "a" * 32
                self.assertEqual(command, ["pw-dump", "-N"]); return "[]"
            summarize = soak.summarize_live_observations
            thermal_summary = soak.thermal_summary
            popen = Mock(side_effect=Child)
            overrides = (
                patch.object(soak, "__file__", str(wrapper)),
                patch.object(soak, "Path", side_effect=lambda value: proc if value == "/proc" else Path(value)),
                patch.object(soak, "time", timing), patch.object(soak, "live_readiness", side_effect=readiness),
                patch.object(soak, "FLASH_RUNTIME_SHA256", hashlib.sha256(b"generated fixture").hexdigest()),
                patch.object(soak, "FLASH_MODEL_SHA256", hashlib.sha256(b"generated fixture").hexdigest()),
                patch.object(soak, "read_command", side_effect=read_command),
                patch.object(soak, "process_identity", return_value={"pid": 42, "start_ticks": 1,
                    "command": [str(assets["guard_script"])]}),
                patch.object(soak, "power_state", return_value={"ac_online": True, "lid_open": True}),
                patch.object(soak, "sensor_paths", return_value={}),
                patch.object(soak, "observation", return_value={"temperatures_millidegrees_C": {"fixture": 35000}}),
                patch.object(soak, "summarize_live_observations", side_effect=lambda *values:
                    copy.deepcopy(completed_controls[1]) if completed_controls is not None else summarize(*values)),
                patch.object(soak, "thermal_summary", side_effect=lambda *values:
                    {"generated_fixture": True} if completed_controls is not None else thermal_summary(*values)),
                patch.object(soak.shutil, "disk_usage", return_value=SimpleNamespace(free=10**12)),
                patch.object(soak.subprocess, "Popen", popen),
                patch("selectors.DefaultSelector", Selector), patch.object(soak.os, "set_blocking"),
                patch.object(soak.os, "getpgrp", return_value=999),
                patch.object(soak.os, "killpg", side_effect=ProcessLookupError),
                patch.object(soak.signal, "signal"), contextlib.redirect_stdout(io.StringIO()),
            )
            with contextlib.ExitStack() as stack:
                for override in overrides:
                    stack.enter_context(override)
                code = soak.run_live(args)
            result = json.loads((args.output / "result.json").read_text())
            stop = (args.output / "stop").read_text().strip() if (args.output / "stop").exists() else None
            self.assertEqual(popen.call_count, 1); self.assertEqual(len(ready_calls), 2)
            self.assertTrue(popen.call_args.kwargs["start_new_session"])
            self.assertFalse(result["passed"]); self.assertFalse(result["qualified"])
            return code, result, stop, launched[0], polls

    def test_failed_service_target_retains_final_input_guard_and_power_evidence(self):
        native, observations = self.controls_fixture()
        observations["service_hops"]["nearest_rank_p99_ms"] = 4095
        code, result, stop, _, _ = self.live_watchdog_run(0, [.1],
            completed_controls=(native, observations))
        self.assertEqual(code, 1)
        self.assertIsNone(stop)
        self.assertEqual(result["native_exit"], 0)
        self.assertEqual(result["error"], "Active controls: strict service p99/loss/thirteen-phase gate failed")
        self.assertFalse(result["passed"])
        self.assertFalse(result["qualified"])
        self.assertEqual(result["pins_after"], result["pins_before"])
        self.assertTrue(result["pins_unchanged"])
        self.assertEqual(result["guard_after"], result["guard_before"])
        self.assertEqual(result["power_after"], result["power_before"])
        self.assertIn("host_after", result)
        self.assertIn("thermal_summary", result)

    def test_delayed_readiness_does_not_age_new_native_startup(self):
        code, result, stop, launched, polls = self.live_watchdog_run(61, [.1, 30.0])
        self.assertEqual(launched, 61); self.assertIsNone(stop); self.assertIsNone(result["stop_reason"])
        self.assertEqual(code, 1); self.assertIn(30.0, polls)
        self.assertEqual(result["wall_budget_seconds"], 300)
        self.assertGreaterEqual(result["elapsed_seconds"], 91); self.assertTrue(result["within_wall_budget"])

    def test_native_missing_progress_stops_only_after_its_own_thirty_seconds(self):
        code, result, stop, launched, polls = self.live_watchdog_run(61, [.1, 30.0, 30.001])
        self.assertEqual(stop, "Native startup progress missing"); self.assertEqual(result["stop_reason"], stop)
        self.assertEqual(code, 2); self.assertIn(30.0, polls)
        self.assertGreater(polls[-1], 30); self.assertLess(polls[-1], 30.01)
        self.assertEqual(launched, 61)

    def test_original_wall_deadline_still_stops_a_young_native_child(self):
        code, result, stop, launched, _ = self.live_watchdog_run(259, [.1])
        self.assertEqual(launched, 259); self.assertEqual(stop, "Fixed live-run wall budget reached")
        self.assertEqual(code, 2); self.assertEqual(result["wall_budget_seconds"], 300)
        self.assertGreaterEqual(result["elapsed_seconds"], 259.1); self.assertTrue(result["within_wall_budget"])

    def test_steady_progress_ten_second_watchdog_is_unchanged(self):
        code, result, stop, _, _ = self.live_watchdog_run(61, [.1, 10.1, 10.101], progress=True)
        self.assertEqual(stop, "Native live progress unchanged for ten seconds")
        self.assertEqual(code, 2); self.assertEqual(result["stop_reason"], stop)

    def ownership_observer(self):
        # Exercise the actual nested helpers without starting the runner or a
        # process. Only /proc paths are redirected to this bounded fixture.
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        proc = Path(directory.name)
        for pid, parent, group, ticks, children in ((101, 1, 101, 1001, "202"), (202, 101, 202, 2002, "")):
            root = proc / str(pid)
            task = root / "task" / str(pid)
            task.mkdir(parents=True)
            fields = ["S", str(parent), str(group), str(group)] + ["0"] * 15 + [str(ticks)]
            (root / "stat").write_text(f"{pid} (fixture) " + " ".join(fields))
            (root / "cmdline").write_bytes(b"fixture\0")
            (task / "children").write_text(children)
        source = inspect.getsource(soak.run_live)
        names = {"process_record", "same_identity", "record_ownership_loss", "remember_command",
                 "proven_command_snapshots", "remember_owned", "live_owned", "signal_owned"}
        helpers = [ast.get_source_segment(source, node) for node in ast.parse(source).body[0].body
                   if isinstance(node, ast.FunctionDef) and node.name in names]
        self.assertEqual(len(helpers), len(names))
        state = ("owned = {}; observed_groups = set(); command_history = {}; ownership_failures = []\n"
                 "ownership_complete = True; ownership_failure_count = 0; ownership_failure_overflow = False\n")
        expose = ("\nreturn remember_owned, live_owned, signal_owned, lambda: {"
                  "'owned': dict(owned), 'ownership_complete': ownership_complete, "
                  "'ownership_failures': list(ownership_failures), 'ownership_failure_count': ownership_failure_count, "
                  "'ownership_failure_overflow': ownership_failure_overflow}\n")
        namespace = {"os": os, "time": soak.time}
        exec("def observer(child, root_identity, Path):\n" + textwrap.indent(
            state + "\n".join(textwrap.dedent(helper) for helper in helpers) + expose, "    "), namespace)
        identity = {"pid": 101, "start_ticks": 1001, "group": 101, "session": 101, "ppid": 1}
        observer = namespace["observer"](SimpleNamespace(pid=101, poll=lambda: 0), identity,
                                          lambda value: proc if value == "/proc" else Path(value))
        return proc, observer

    def test_owned_process_disappearance_preserves_missing_file_evidence(self):
        for parent_read in (False, True):
            outcomes = []
            for error in (FileNotFoundError(errno.ENOENT, "gone"), ProcessLookupError(errno.ESRCH, "gone")):
                with self.subTest(parent_read=parent_read, error=type(error).__name__):
                    proc, (remember, _, _, snapshot) = self.ownership_observer()
                    original = Path.read_text
                    parent_reads = 0
                    def read(path, *args, **kwargs):
                        nonlocal parent_reads
                        if path == proc / "101/stat":
                            parent_reads += 1
                        if path == proc / ("101/stat" if parent_read else "202/stat") and (not parent_read or parent_reads == 3):
                            raise error
                        return original(path, *args, **kwargs)
                    with patch.object(Path, "read_text", read):
                        remember()
                    result = snapshot()
                    self.assertFalse(result["ownership_complete"])
                    self.assertEqual(result["ownership_failure_count"], 1)
                    self.assertFalse(result["ownership_failure_overflow"])
                    failure = result["ownership_failures"][0]
                    self.assertEqual((failure["reason"], failure["pid"]),
                                     ("process_or_parent_disappeared_during_walk", 202))
                    self.assertEqual(failure["parent_proof"],
                                     {"pid": 101, "start_ticks": 1001, "group": 101, "session": 101, "ppid": 1})
                    self.assertIsNone(failure["observed_identity"])
                    failure.pop("observed_monotonic_ns")
                    outcomes.append((set(result["owned"]), failure))
            if len(outcomes) == 2:
                self.assertEqual(outcomes[0], outcomes[1])

    def test_exiting_thread_children_matches_missing_file_handling(self):
        for error in (FileNotFoundError(errno.ENOENT, "gone"), ProcessLookupError(errno.ESRCH, "gone")):
            with self.subTest(error=type(error).__name__):
                proc, (remember, _, _, snapshot) = self.ownership_observer()
                original = Path.read_text
                def read(path, *args, **kwargs):
                    if path == proc / "202/task/202/children":
                        raise error
                    return original(path, *args, **kwargs)
                with patch.object(Path, "read_text", read):
                    remember()
                result = snapshot()
                self.assertEqual(set(result["owned"]), {(101, 1001), (202, 2002)})
                self.assertTrue(result["ownership_complete"])
                self.assertEqual(result["ownership_failures"], [])

    def test_live_owned_disappearance_and_replacement_never_signal_replacement(self):
        for error in (FileNotFoundError(errno.ENOENT, "gone"), ProcessLookupError(errno.ESRCH, "gone")):
            with self.subTest(error=type(error).__name__):
                proc, (remember, live, signal_owned, _) = self.ownership_observer()
                remember()
                original = Path.read_text
                def read(path, *args, **kwargs):
                    if path == proc / "202/stat":
                        raise error
                    return original(path, *args, **kwargs)
                with patch.object(Path, "read_text", read), patch.object(os, "getpgrp", return_value=999), \
                        patch.object(os, "killpg") as kill:
                    self.assertEqual(set(live()), {101})
                    signal_owned(soak.signal.SIGTERM)
                    kill.assert_called_once_with(101, soak.signal.SIGTERM)
                path = proc / "202/stat"
                path.write_text(path.read_text().replace("2002", "9999"))
                with patch.object(os, "getpgrp", return_value=999), patch.object(os, "killpg") as kill:
                    self.assertEqual(set(live()), {101})
                    signal_owned(soak.signal.SIGTERM)
                    kill.assert_called_once_with(101, soak.signal.SIGTERM)

    def test_process_observation_permission_and_unrelated_errors_remain_visible(self):
        for stage in ("process", "thread", "live"):
            for error in (PermissionError(errno.EACCES, "denied"), OSError(errno.EIO, "I/O error")):
                with self.subTest(stage=stage, error=type(error).__name__):
                    proc, (remember, live, _, _) = self.ownership_observer()
                    if stage == "live":
                        remember()
                    target = proc / ("202/task/202/children" if stage == "thread" else "202/stat")
                    original = Path.read_text
                    def read(path, *args, **kwargs):
                        if path == target:
                            raise error
                        return original(path, *args, **kwargs)
                    with patch.object(Path, "read_text", read), self.assertRaises(type(error)) as raised:
                        (live if stage == "live" else remember)()
                    self.assertIs(raised.exception, error)

    def cli(self, *extra, live_paths=True):
        arguments = ["soak", "--allow-host-audio", "--binary", "/bin/player", "--track-a", "/a",
                     "--track-b", "/b", "--output", "/evidence"]
        if live_paths:
            for name in ("python", "exporter", "worker", "service", "runtime", "model", "readiness-decision"):
                arguments.extend(("--" + name, "/" + name))
        with patch.object(sys, "argv", arguments + list(extra)), patch.dict(os.environ, {}, clear=True), \
                patch.object(soak, "run_live", return_value=0) as run, contextlib.redirect_stderr(io.StringIO()):
            result = soak.main()
            return result, run.call_args.args[0]

    def test_exclusive_hour_admission_and_short_unchanged(self):
        for arguments, seconds in ((("--live-stems-hour",), 3600), (("--live-stems",), 120),
                                   (("--live-stems", "--seconds", "180"), 180)):
            with self.subTest(arguments=arguments):
                result, args = self.cli(*arguments)
                self.assertEqual((result, args.seconds), (0, seconds))
        for arguments in (("--live-stems-hour", "--live-stems"), ("--live-stems-hour", "--seconds", "3599"),
                          ("--live-stems-hour", "--seconds", "3601"), ("--live-stems", "--seconds", "3600")):
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit):
                self.cli(*arguments)

    def test_background_admission_preserves_live_modes_and_duration_bounds(self):
        for mode, seconds in (("--live-stems", 120), ("--live-stems-hour", 3600)):
            with self.subTest(mode=mode):
                _, visible = self.cli(mode)
                result, background = self.cli(mode, "--background")
                self.assertFalse(visible.background)
                self.assertTrue(background.background)
                self.assertEqual((result, background.seconds), (0, seconds))
        for arguments in (("--live-stems", "--seconds", "119"),
                          ("--live-stems", "--seconds", "181"),
                          ("--live-stems-hour", "--seconds", "3599"),
                          ("--live-stems-hour", "--seconds", "3601"),
                          ("--live-stems", "--live-stems-hour")):
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit):
                self.cli("--background", *arguments)
        # No unrelated live-tool options may mask a missing-mode rejection.
        with self.assertRaises(SystemExit):
            self.cli("--background", live_paths=False)

    def readiness_record(self):
        return {"schema": 1, "decision": "ready", "ready_for_run": "fixture-run",
                "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
                "human_reply": "Ready for this run", "AC_power": True, "lid_open": True,
                "visible_window": True, "screen_awake": True}

    def readiness(self, record, background=False):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "readiness.json"
            path.write_text(json.dumps(record))
            return soak.live_readiness(path, "fixture-run", background=background)

    def test_background_readiness_requires_explicit_fresh_human_mode_binding(self):
        record = self.readiness_record()
        record.update(ui_mode="background", visible_window=False, screen_awake=False)
        self.assertEqual(self.readiness(record, background=True)["record"], record)
        record.pop("visible_window"); record.pop("screen_awake")
        self.assertEqual(self.readiness(record, background=True)["record"], record)
        for key, value in (("ui_mode", None), ("ui_mode", "visible"), ("ready_for_run", "another-run"),
                           ("decision", "wait"), ("AC_power", False), ("lid_open", False),
                           ("human_reply", ""), ("human_reply", True),
                           ("recorded_at_utc", (datetime.now(timezone.utc) - timedelta(seconds=901)).isoformat()),
                           ("recorded_at_utc", (datetime.now(timezone.utc) + timedelta(seconds=60)).isoformat())):
            invalid = dict(record); invalid[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(RuntimeError):
                self.readiness(invalid, background=True)

    def test_visible_readiness_keeps_visible_awake_and_mode_requirements(self):
        record = self.readiness_record()
        self.assertEqual(self.readiness(record)["record"], record)
        for key in ("AC_power", "lid_open", "visible_window", "screen_awake"):
            invalid = dict(record); invalid[key] = False
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                self.readiness(invalid)
        # An explicitly background-only confirmation cannot authorize the
        # visible qualification path, even if incidental window flags are true.
        record["ui_mode"] = "background"
        with self.assertRaises(RuntimeError):
            self.readiness(record)

    def fixture(self, preparation_seconds=75, overlap=False):
        rate = 48000
        begin = 123456
        finish = begin + 3600 * rate
        a_controls = [{"output_begin": begin + 10 * rate, "output_end": begin + 14 * rate}]
        peers = []
        cursor = preparation_seconds * rate
        anchor = begin
        while anchor + 420 * rate - cursor < finish:
            issue = anchor + 420 * rate - cursor
            settled = issue + 2 * rate
            peers.append({"kind": "peer_rewind", "target_frame": 0, "issued_ns": issue * 1000000000 // rate,
                          "output_begin": issue, "source_before": 420 * rate,
                          "ack_output_frame": issue, "ack_source_frame": 0,
                          "settled_ns": settled * 1000000000 // rate, "output_end": settled,
                          "source_after": 2 * rate})
            anchor, cursor = settled, 2 * rate
        if overlap:
            a_controls.append({"output_begin": peers[0]["output_begin"] - rate,
                               "output_end": peers[0]["output_end"] + rate})
        boundaries = sorted({begin, finish, *(c[k] for c in a_controls + peers for k in ("output_begin", "output_end"))})
        intervals = []
        for first, last in zip(boundaries, boundaries[1:]):
            def active(controls):
                return any(c["output_begin"] <= first and last <= c["output_end"] for c in controls)
            peer = next((c for c in reversed(peers) if c["output_end"] <= first), None)
            source = (peer["source_after"] + first - peer["output_end"]) if peer else preparation_seconds * rate + first - begin
            intervals.append({"output_begin": first, "output_end": last, "frames": last - first,
                              "a_recovery": active(a_controls), "b_recovery": active(peers),
                              "starved_a": 0, "starved_b": 0, "starved_frames": 0,
                              "source_b_begin": source, "source_b_end": source + last - first,
                              "aborted": False, "read_failure_observed": False})
        native = {"mode": "live-stems-hour", "requested_seconds": 3600, "passed": True, "cancelled": False,
                  "host_audio": True, "functional_only": False, "shutdown_completed": True,
                  "setup": {"input_frames": 1031940, "output_frames": 1123201}, "peer": {"frames": 480 * rate},
                  "measurement_begin_output_frame": begin, "measurement_end_output_frame": finish,
                  "measurement_begin_peer_frame": preparation_seconds * rate, "measured_wall_ms": 3600000,
                  "measurement_intervals_closed": True, "last_recovery_complete": True,
                  "controls": a_controls, "peer_controls": peers, "intervals": intervals,
                  "steady_starved_a": 0, "steady_starved_b": 0, "steady_starved_frames": 0,
                  "control_starved_frames": 0, "samples": [{"elapsed_ms": i * 10000, "snapshot_drops": 0} for i in range(360)],
                  "scene_errors": [], "frames_presented": 100000, "maximum_frame_presentation_gap_ns": 17000000,
                  "maximum_gui_heartbeat_gap_ns": 11000000, "callbacks": 675000,
                  "invalid_buffers": 0, "source_read_failures": 0, "rejected_commands": 0, "execution_deadline_misses": 0,
                  "timing": [{"callbacks": 675000, "driver_position_observed": True, "driver_position_discontinuities": 0,
                              "driver_recovery_flags": 0, "graph_quantum_min": 256, "graph_quantum_max": 256}]}
        observations = {"kind_counts": {"ChunkValidated": 200}, "service_metrics_complete": True,
                        "service_hops": {**soak.duration_stats([3000] * 199 + [5000]), "complete": True, "at_or_above_target": 1},
                        "chunk_phases": {"fields": {name: {"count": 200, "present_for_all_chunks": True} for name in soak.LIVE_PHASE_NAMES}}}
        return native, observations

    def background_fixture(self):
        native, observations = self.fixture()
        native.update(ui_mode="background", frames_presented=0, maximum_frame_presentation_gap_ns=0,
                      ui_coverage={"checks": 360000, "visible_healthy_checks": 0,
                                   "missing_window_checks": 0, "hidden_checks": 360000,
                                   "unexposed_checks": 360000, "stale_frame_checks": 0,
                                   "missing_frame_checks": 360000, "maximum_frame_age_ns": 0,
                                   "any_gap": True, "visible_ui_qualification_eligible": False})
        return native, observations

    def test_background_hour_accepts_absent_or_stale_UI_without_UI_qualification(self):
        native, observations = self.background_fixture()
        result = soak.validate_hour_result(native, observations, background=True)
        self.assertFalse(result["visible_ui_qualification_eligible"])
        self.assertIn("background", result["scope"].lower())
        self.assertEqual((result["peer_rewinds"], result["service_hops"]), (8, 200))
        native.update(frames_presented=1000, maximum_frame_presentation_gap_ns=5000000000)
        native["ui_coverage"].update(missing_frame_checks=0, stale_frame_checks=360000,
                                     maximum_frame_age_ns=5000000000)
        self.assertFalse(soak.validate_hour_result(native, observations, background=True)
                         ["visible_ui_qualification_eligible"])

    def test_hour_UI_modes_and_coverage_cannot_be_relabeled(self):
        native, observations = self.background_fixture()
        with self.assertRaises(RuntimeError):
            soak.validate_hour_result(native, observations)
        for mode in (None, "visible"):
            invalid = copy.deepcopy(native)
            if mode is None:
                invalid.pop("ui_mode")
            else:
                invalid["ui_mode"] = mode
            with self.subTest(mode=mode), self.assertRaises(RuntimeError):
                soak.validate_hour_result(invalid, observations, background=True)
        for key, value in (("visible_ui_qualification_eligible", True), ("checks", 0),
                           ("missing_window_checks", 1),
                           ("hidden_checks", -1), ("unexposed_checks", 360001),
                           ("missing_frame_checks", True), ("maximum_frame_age_ns", -1)):
            invalid = copy.deepcopy(native); invalid["ui_coverage"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(RuntimeError):
                soak.validate_hour_result(invalid, observations, background=True)

    def test_background_preserves_heartbeat_audio_peer_and_service_gates(self):
        native, observations = self.background_fixture()
        for label, mutation in (
                ("heartbeat stalled", lambda n: n.update(maximum_gui_heartbeat_gap_ns=2000000000)),
                ("no callbacks", lambda n: n.update(callbacks=0)),
                ("callback fault", lambda n: n.update(execution_deadline_misses=1)),
                ("driver discontinuity", lambda n: n["timing"][0].update(driver_position_discontinuities=1)),
                ("incomplete shutdown", lambda n: n.update(shutdown_completed=False)),
                ("unfinished recovery", lambda n: n.update(last_recovery_complete=False)),
                ("short measurement", lambda n: n.update(measured_wall_ms=3590000)),
                ("missing peer rewind", lambda n: n["peer_controls"].pop()),
                ("peer starvation hidden by A control", lambda n: next(
                    i for i in n["intervals"] if i["a_recovery"] and not i["b_recovery"]
                ).update(starved_b=256, starved_frames=256))):
            invalid = copy.deepcopy(native); mutation(invalid)
            with self.subTest(label=label), self.assertRaises(RuntimeError):
                soak.validate_hour_result(invalid, observations, background=True)
        for label, mutation in (
                ("service loss", lambda o: o.update(service_metrics_complete=False)),
                ("strict p99 boundary", lambda o: o["service_hops"].update(nearest_rank_p99_ms=4095)),
                ("missing phase", lambda o: o["chunk_phases"]["fields"].pop("native_graph_compute_ms"))):
            invalid = copy.deepcopy(observations); mutation(invalid)
            with self.subTest(label=label), self.assertRaises(RuntimeError):
                soak.validate_hour_result(native, invalid, background=True)

    def test_public_qualification_gate_never_promotes_background_or_missing_cleanup(self):
        native, observations = self.fixture()
        gates = soak.validate_hour_result(native, observations)
        result = {"passed": True, "cleanup_proven": True, "ui_mode": "visible",
                  "native": native, "hour_gates": gates}
        self.assertTrue(soak.live_qualified(result, True))
        self.assertFalse(soak.live_qualified(result, False))
        for key in ("passed", "cleanup_proven", "hour_gates"):
            invalid = copy.deepcopy(result); invalid[key] = False
            with self.subTest(key=key):
                self.assertFalse(soak.live_qualified(invalid, True))
        for location in ("wrapper", "native", "hour_gates"):
            invalid = copy.deepcopy(result)
            if location == "wrapper":
                invalid["ui_mode"] = "background"
            elif location == "native":
                invalid["native"]["ui_mode"] = "background"
            else:
                invalid["hour_gates"]["visible_ui_qualification_eligible"] = False
            with self.subTest(location=location):
                self.assertFalse(soak.live_qualified(invalid, True))
        native, observations = self.background_fixture()
        result.update(ui_mode="background", native=native,
                      hour_gates=soak.validate_hour_result(native, observations, background=True))
        self.assertFalse(soak.live_qualified(result, True, background=True))
        # Changing both labels must not erase the retained non-UI gate marker.
        result["ui_mode"] = result["native"]["ui_mode"] = "visible"
        self.assertFalse(soak.live_qualified(result, True))

    def test_actual_peer_cursor_first_due_after_75_second_preparation(self):
        native, observations = self.fixture()
        result = soak.validate_hour_result(native, observations)
        self.assertEqual(result["peer_rewinds"], 8)
        self.assertEqual((native["peer_controls"][0]["output_begin"] - native["measurement_begin_output_frame"]) / 48000, 345)
        self.assertEqual(result["service_hops_at_or_above_target"], 1)
        native["peer_controls"][0]["output_begin"] += 75 * 48000
        with self.assertRaises(RuntimeError):
            soak.validate_hour_result(native, observations)

    def test_independent_and_overlapping_control_accounting(self):
        native, observations = self.fixture()
        # An A-only recovery cannot hide B starvation.
        a = next(i for i in native["intervals"] if i["a_recovery"] and not i["b_recovery"])
        a.update(starved_b=256, starved_frames=256)
        with self.assertRaises(RuntimeError):
            soak.validate_hour_result(native, observations)
        native, observations = self.fixture()
        # Nor can a B-only rewind hide A starvation.
        b = next(i for i in native["intervals"] if i["b_recovery"] and not i["a_recovery"])
        b.update(starved_a=256, starved_frames=256)
        with self.assertRaises(RuntimeError):
            soak.validate_hour_result(native, observations)
        native, observations = self.fixture(overlap=True)
        # B's due cursor is unchanged when A is already recovering. Both control
        # edges partition the overlap without exempting either steady neighbor.
        overlap = next(i for i in native["intervals"] if i["b_recovery"])
        overlap.update(a_recovery=True, starved_a=256, starved_b=512, starved_frames=768)
        native["control_starved_frames"] = 768
        soak.validate_hour_result(native, observations)
        overlap["a_recovery"] = False
        with self.assertRaises(RuntimeError):
            soak.validate_hour_result(native, observations)

    def test_strict_nearest_rank_gate_all_phases_and_no_loss(self):
        native, observations = self.fixture()
        for values in ([3000] * 197 + [5000] * 3, [4095] * 200):
            invalid = copy.deepcopy(observations)
            invalid["service_hops"].update(soak.duration_stats(values))
            with self.assertRaises(RuntimeError):
                soak.validate_hour_result(native, invalid)
        for key in soak.LIVE_PHASE_NAMES:
            invalid = copy.deepcopy(observations)
            invalid["chunk_phases"]["fields"][key]["count"] -= 1
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                soak.validate_hour_result(native, invalid)
        observations["service_metrics_complete"] = False
        with self.assertRaises(RuntimeError):
            soak.validate_hour_result(native, observations)

    def test_hour_bounds_and_health_fail_closed(self):
        self.assertEqual(soak.live_evidence_limits(True), (65536, 64 * 1024**2))
        self.assertEqual(soak.live_evidence_limits(False), (16384, 32 * 1024**2))
        self.assertEqual(soak.live_required_free_bytes(3600, True), 3685015552)
        native, observations = self.fixture()
        for mutation in (lambda n: n["samples"].extend([n["samples"][0]] * 3),
                         lambda n: n["setup"].update(output_frames=1123200),
                         lambda n: n["setup"].update(input_frames=1031939),
                         lambda n: n.update(measured_wall_ms=3590000),
                         lambda n: n["timing"][0].update(driver_recovery_flags=1),
                         lambda n: n["timing"][0].update(driver_position_discontinuities=1),
                         lambda n: n["peer_controls"][0].update(settled_ns=n["peer_controls"][0]["issued_ns"] + 1999999999),
                         lambda n: n.update(cancelled=True), lambda n: n.update(shutdown_completed=False)):
            invalid = copy.deepcopy(native); mutation(invalid)
            with self.assertRaises(RuntimeError):
                soak.validate_hour_result(invalid, observations)

    def test_exact_observation_line_byte_and_line_length_bounds(self):
        setup = {"input_frames": 17, "output_frames": 19, "factory_entry_ns": 1,
                 "input_captured_ns": 2, "count_complete_ns": 3, "factory_committed_ns": 4}
        native = {"mode": "live-stems-hour", "setup": setup, "controls": [],
                  "load_issued_ns": 5, "first_advance_observed_ns": 7,
                  "observation_counters": {"producer_finished": True, "dropped": 0,
                                           "invalid_clock_reads": 0, "attempted": 65535, "retained": 65535}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "observations.jsonl"
            with path.open("w") as stream:
                stream.write(json.dumps({"type": "setup", "context": setup}) + "\n")
                for ordinal in range(1, 65536):
                    stream.write(json.dumps({"kind": 1, "ordinal": ordinal, "observed_ns": ordinal,
                                             "telemetry_dropped_present": False}) + "\n")
            soak.summarize_live_observations(path, native)
            with path.open("a") as stream:
                stream.write("{}\n")
            with self.assertRaisesRegex(RuntimeError, "record limit"):
                soak.summarize_live_observations(path, native)
            path.write_bytes(b" " * 4096 + b"\n")
            with self.assertRaisesRegex(RuntimeError, "oversized"):
                soak.summarize_live_observations(path, native)
            with path.open("wb") as stream:
                stream.truncate(64 * 1024**2 + 1)
            with self.assertRaisesRegex(RuntimeError, "exceeds its bound"):
                soak.summarize_live_observations(path, native)


if __name__ == "__main__":
    unittest.main()

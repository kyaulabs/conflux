#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fast admission and evidence regressions; never opens a host audio device."""
import contextlib
import copy
import ast
import errno
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
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.experiments import run_manual_playback_soak as soak


class HourPlaybackTest(unittest.TestCase):
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

    def cli(self, *extra):
        arguments = ["soak", "--allow-host-audio", "--binary", "/bin/player", "--track-a", "/a",
                     "--track-b", "/b", "--output", "/evidence"]
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

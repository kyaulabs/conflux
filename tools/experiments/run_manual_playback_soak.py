#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Explicit host opt-in wrapper for native quiet-audio/UI playback qualification."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import stat
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



# These identities select the previously promoted pair; paths remain explicit.
FLASH_RUNTIME_SHA256 = "6f4dca329ee158ad4c1abe963cd9d0940d63ba8a309afdc8b4cd1b7259edbc53"
FLASH_MODEL_SHA256 = "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"


def live_readiness(path, label, background=False):
    if not path.is_file() or path.stat().st_size > 16384:
        raise RuntimeError("Supply a small fresh live-run readiness record")
    value = json.loads(path.read_text())
    issued = datetime.fromisoformat(value.get("recorded_at_utc", ""))
    age = (datetime.now(timezone.utc) - issued).total_seconds() if issued.tzinfo else -1
    conditions = ("AC_power", "lid_open") if background else ("AC_power", "lid_open", "visible_window", "screen_awake")
    if not (value.get("schema") == 1 and value.get("decision") == "ready" and value.get("ready_for_run") == label
            and value.get("ui_mode", "visible") == ("background" if background else "visible")
            and all(value.get(name) is True for name in conditions)
            and isinstance(value.get("human_reply"), str) and 0 < len(value["human_reply"]) <= 8192 and 0 <= age <= 900):
        raise RuntimeError("Fresh readiness must match this run/UI mode and its required physical conditions")
    return {"record": value, "age_seconds": age, "maximum_age_seconds": 900, "pin": pin(path)}


LIVE_PHASE_NAMES = ("source_read_ms", "request_ms", "overlap_ms", "native_elapsed_ms", "native_inference_wall_ms",
                    "native_inference_cpu_ms", "native_frontend_ms", "native_graph_ms", "native_graph_upload_ms",
                    "native_graph_compute_ms", "native_graph_readback_ms", "native_postprocess_ms", "native_graph_rebuild_ms")


def live_evidence_limits(hour):
    return (65536, 64 * 1024**2) if hour else (16384, 32 * 1024**2)


def live_required_free_bytes(seconds, hour):
    return 2 * 1024**3 + (seconds + 120) * 384000 + 40 * 1024**2 + (64 * 1024**2 if hour else 0)


def duration_stats(values):
    ordered = sorted(values)
    return {"count": len(values), "maximum_ms": ordered[-1] if ordered else None,
            "nearest_rank_p99_ms": ordered[math.ceil(.99 * len(ordered)) - 1] if ordered else None}


def validate_hour_result(native, observations, background=False):
    """Check bounded native hour evidence before the wrapper's cleanup gate."""
    def check(condition, description):
        if not condition:
            raise RuntimeError("Hour qualification: " + description)
    check(native.get("mode") == "live-stems-hour" and native.get("requested_seconds") == 3600
          and native.get("passed") is True and native.get("cancelled") is False
          and native.get("functional_only") is False and native.get("host_audio") is True
          and native.get("shutdown_completed") is True, "incomplete/non-host hour")
    check(native.get("ui_mode", "visible") == ("background" if background else "visible"), "UI mode differs from launched mode")
    coverage = native.get("ui_coverage", {})
    if background:
        check(isinstance(coverage, dict) and coverage.get("visible_ui_qualification_eligible") is False,
              "background evidence cannot qualify visible UI")
        checks = coverage.get("checks")
        counts = ("visible_healthy_checks", "missing_window_checks", "hidden_checks", "unexposed_checks",
                  "stale_frame_checks", "missing_frame_checks")
        check(type(checks) is int and 0 < checks <= 4000000 and
              all(type(coverage.get(key)) is int and 0 <= coverage[key] <= checks for key in counts) and
              coverage["missing_window_checks"] == 0 and type(coverage.get("any_gap")) is bool and
              type(coverage.get("maximum_frame_age_ns")) is int and coverage["maximum_frame_age_ns"] >= 0,
              "background UI observation counts invalid")
    else:
        check(isinstance(coverage, dict) and coverage.get("visible_ui_qualification_eligible", True) is True,
              "background UI evidence cannot enter visible qualification")
    check(native["setup"]["input_frames"] == 1031940 and native["setup"]["output_frames"] == 1123201
          and native["peer"]["frames"] == 480 * 48000,
          "changed source/peer geometry")
    check(native["measurement_intervals_closed"] and native["last_recovery_complete"], "open measurement/control")
    begin, end = native["measurement_begin_output_frame"], native["measurement_end_output_frame"]
    wall = native["measured_wall_ms"] / 1000
    check(3600 <= wall <= 3602 and abs((end - begin) / 48000 - wall) <= max(.25, wall * .002),
          "wall/output duration agreement failed")
    check(native["callbacks"] > 0 and not native["scene_errors"]
          and 0 < native["maximum_gui_heartbeat_gap_ns"] < 2000000000, "audio/control loop not responsive")
    if not background:
        check(native["frames_presented"] >= 10
              and 0 < native["maximum_frame_presentation_gap_ns"] < 2000000000, "visible UI not responsive")
    check(all(native[key] == 0 for key in ("invalid_buffers", "source_read_failures", "rejected_commands", "execution_deadline_misses")),
          "audio callback/source/command fault")
    check(bool(native["timing"]) and all(row["callbacks"] > 0 and row["driver_position_observed"]
          and not row["driver_position_discontinuities"] and not row["driver_recovery_flags"]
          and row["graph_quantum_min"] == row["graph_quantum_max"] == 256 for row in native["timing"]),
          "driver continuity/quantum failed")
    samples = native["samples"]
    check(2 <= len(samples) <= 362 and samples[0]["elapsed_ms"] < 2000
          and samples[-1]["elapsed_ms"] >= 3590000
          and all(not row["snapshot_drops"] for row in samples)
          and all(10000 <= b["elapsed_ms"] - a["elapsed_ms"] < 12000 for a, b in zip(samples, samples[1:])),
          "bounded ten-second snapshots incomplete")
    controls, peers, intervals = native["controls"], native["peer_controls"], native["intervals"]
    check(len(controls) <= 512 and len(peers) == 8 and 0 < len(intervals) <= 2048, "control/interval capacity")
    anchor, cursor = begin, native["measurement_begin_peer_frame"]
    check(0 <= cursor < 420 * 48000, "peer already due before measurement")
    for control in peers:
        first, last = control["output_begin"], control["output_end"]
        check(control["kind"] == "peer_rewind" and control["target_frame"] == 0
              and anchor <= first <= control["ack_output_frame"] <= last <= end
              and control["source_before"] == cursor + first - anchor
              and 420 * 48000 <= control["source_before"] < 422 * 48000
              and 0 <= control["ack_source_frame"] <= control["source_after"] < 4 * 48000
              and control["settled_ns"] - control["issued_ns"] >= 2000000000,
              "peer rewind cursor/acknowledgement/two-second settle differs")
        anchor, cursor = last, control["source_after"]
    check(cursor + end - anchor < 420 * 48000, "due peer rewind omitted")
    for control in controls:
        check(begin <= control["output_begin"] <= control["output_end"] <= end, "A control outside measurement")
    boundary = begin; steady = [0, 0]; controlled = [0, 0]
    endpoints = {begin, end}
    for row in intervals:
        first, last = row["output_begin"], row["output_end"]
        check(first == boundary and first <= last <= end and row["frames"] == last - first
              and not row["aborted"] and not row["read_failure_observed"], "interval partition/health failed")
        endpoints.add(last); boundary = last
        check(row["starved_frames"] == row["starved_a"] + row["starved_b"], "per-deck starvation sum differs")
        for deck, (key, events) in enumerate((("a_recovery", controls), ("b_recovery", peers))):
            active = any(c["output_begin"] <= first and last <= c["output_end"] for c in events)
            check(isinstance(row[key], bool) and (first == last or row[key] == active), "control mask differs from boundaries")
            value = row["starved_a" if deck == 0 else "starved_b"]
            check(type(value) is int and value >= 0 and (first != last or value == 0), "invalid starvation count")
            (controlled if row[key] else steady)[deck] += value
        if not row["b_recovery"]:
            check(row["source_b_end"] - row["source_b_begin"] == last - first, "steady peer lost source/output alignment")
    check(boundary == end and all(c[key] in endpoints for c in controls + peers for key in ("output_begin", "output_end")),
          "control edge missing from interval partition")
    check(steady == [native["steady_starved_a"], native["steady_starved_b"]] == [0, 0]
          and native["steady_starved_frames"] == sum(steady) and native["control_starved_frames"] == sum(controlled),
          "deck-specific steady starvation observed")
    hops = observations["service_hops"]; phases = observations["chunk_phases"]["fields"]
    check(observations["service_metrics_complete"] and hops["complete"]
          and hops["count"] == observations["kind_counts"]["ChunkValidated"] > 0
          and hops["nearest_rank_p99_ms"] < 4095, "service hop p99/loss gate failed")
    check(set(phases) == set(LIVE_PHASE_NAMES) and all(row["present_for_all_chunks"] and row["count"] == hops["count"] for row in phases.values()),
          "all thirteen phases required on every validated chunk")
    return {"peer_rewinds": len(peers), "service_hops": hops["count"],
            "service_hops_at_or_above_target": hops["at_or_above_target"],
            "strict_service_p99_target_ms": 4095, "steady_starved_a": steady[0], "steady_starved_b": steady[1],
            "visible_ui_qualification_eligible": not background,
            "scope": "background audio/stem hour; visible UI not qualified" if background else "visible audio/UI hour"}


def validate_active_controls(native, observations=None, *, functional=False):
    """Validate the fixed actual-Qt sequence; private evidence never qualifies service timing."""
    def check(value, message):
        if not value:
            raise RuntimeError("Active controls: " + message)
    check(native.get("mode") == "live-stems-controls" and native.get("passed") is True and
          native.get("cancelled") is False and native.get("shutdown_completed") is True and
          native.get("functional_only") is functional and native.get("host_audio") is (not functional) and
          native.get("ui_mode") == "visible" and native.get("window_policy") == "visible", "incomplete/wrong mode")
    seconds = native.get("requested_seconds")
    check(type(seconds) is int and (1 <= seconds <= 10 if functional else 120 <= seconds <= 180), "duration admission")
    check(native.get("manual_controls_complete") is True and len(native.get("manual_controls", [])) == 16,
          "missing/incomplete control sequence")
    check(native["measurement_intervals_closed"] and native["last_recovery_complete"] and
          seconds * 1000 <= native["measured_wall_ms"] <= seconds * 1000 + 2000, "open measurement/recovery")
    baseline = native["manual_controls_baseline"]
    fields = ("deck_gains", "eq_db", "crossfader", "cue_selected", "master_gain", "cue_gain", "stem_gains")
    expected = {key: baseline[key] for key in fields}
    check(expected["deck_gains"] == [.25, 1] and expected["crossfader"] == .5 and
          expected["eq_db"] == [[0, 0, 0], [0, 0, 0]] and expected["master_gain"] == .25 and
          expected["stem_gains"] == [[1] * 4, [0] * 4] and
          all(type(x) is bool for x in expected["cue_selected"]), "baseline differs")
    previous_ack = 0
    begin, end = native["measurement_begin_output_frame"], native["measurement_end_output_frame"]
    for index, row in enumerate(native["manual_controls"]):
        deck, step = divmod(index, 8)
        control, restore = divmod(step, 2)
        name = (f"deck{deck}Gain", "crossfader", f"deck{deck}EqHI", f"deck{deck}CueSelect")[control]
        due = ((7000 if deck else 500) + step * 100 if functional else
               (70000 if deck else 10000) + step * 2000)
        before, ack = row["before"], row["ack"]
        check(row["index"] == index and row["deck"] == deck and row["step"] == step and row["object"] == name and
              row["acknowledged"] is True and row["due_ms"] == due and row["issued_elapsed_ms"] >= due and
              previous_ack <= row["issued_ns"] < row["ack_poll_ns"] < row["issued_ns"] + 5000000000,
              "action order/due time/acknowledgement")
        check(all(before[key] == expected[key] for key in fields), "unexpected pre-action mix")
        expected = json.loads(json.dumps(expected))
        ui_value = None
        if control == 0:
            ui_value = baseline["deck_gains"][deck] if restore else baseline["deck_gains"][deck] / 2
            expected["deck_gains"][deck] = ui_value
        elif control == 1:
            ui_value = baseline["crossfader"] if restore else (.75 if deck else .25)
            expected["crossfader"] = ui_value
        elif control == 2:
            ui_value = .5 if restore else .375
            expected["eq_db"][deck][2] = 0 if restore else -6.5
        else:
            ui_value = baseline["cue_selected"][deck] if restore else not baseline["cue_selected"][deck]
            expected["cue_selected"][deck] = ui_value
        check(all(row["expected"][key] == ack[key] == expected[key] for key in fields), "wrong target/restoration")
        check((type(row["ui_value"]) is bool and row["ui_value"] == ui_value) if control == 3 else
              (type(row["ui_value"]) in (int, float) and math.isfinite(row["ui_value"]) and
               abs(row["ui_value"] - ui_value) < 1e-6), "UI did not display acknowledged value")
        check(row["session_before"] == row["session_after"] and row["presented_after"] > row["presented_before"] and
              ack["last_command_id"] == row["expected_command"] == before["last_command_id"] + 1 and
              ack["applied"] == before["applied"] + 1 and
              begin <= before["output_frame"] <= ack["output_frame"] <= end, "session/command/frame acknowledgement")
        for key in ("source_lengths", "source_generations", "transport_revisions", "cue_frames", "cue_held", "cue_preview"):
            check(before[key] == ack[key], "nontransport action changed " + key)
        advance = ack["output_frame"] - before["output_frame"]
        check(before["playing"] == ack["playing"] == [True, True] and before["starved"] == ack["starved"] and
              all(ack["source_frames"][d] == min(before["source_frames"][d] + advance, before["source_lengths"][d])
                  for d in range(2)), "source clock/starvation/transport changed during mix action")
        check(not any(ack["read_failure"]) and not ack["invalid_buffers"] and not ack["rejected"], "action health fault")
        previous_ack = row["ack_poll_ns"]
    check(all(expected[key] == baseline[key] for key in fields), "final mix differs from baseline")
    check(native["steady_starved_frames"] == 0 and native["starved_frames_outside_measured_intervals"] == 0 and
          all(native[key] == 0 for key in ("invalid_buffers", "rejected_commands", "source_read_failures")), "audio health fault")
    check(all(row["kind"] in ("steady", "incomplete_generation_seek", "replay_recovery") and
              (row["kind"] != "steady" or row["starved_frames"] == 0) for row in native["intervals"]),
          "mix controls exempted steady starvation")
    check(not native["scene_errors"] and native["frames_presented"] > 0 and
          0 < native["maximum_gui_heartbeat_gap_ns"] < 2000000000 and
          0 < native["maximum_frame_presentation_gap_ns"] < 2000000000, "UI not responsive")
    if not functional:
        check(native["execution_deadline_misses"] == 0 and observations is not None, "host timing evidence missing")
        hops, phases = observations["service_hops"], observations["chunk_phases"]["fields"]
        check(observations["service_metrics_complete"] and hops["complete"] and
              hops["count"] == observations["kind_counts"]["ChunkValidated"] > 0 and
              hops["nearest_rank_p99_ms"] < 4095 and set(phases) == set(LIVE_PHASE_NAMES) and
              all(x["present_for_all_chunks"] and x["count"] == hops["count"] for x in phases.values()),
              "strict service p99/loss/thirteen-phase gate failed")
        check(bool(native["timing"]) and all(x["driver_position_observed"] and not x["driver_position_discontinuities"] and
              not x["driver_recovery_flags"] and x["graph_quantum_min"] == x["graph_quantum_max"] == 256
              for x in native["timing"]), "driver continuity/quantum fault")
    return {"actions": 16, "functional_only": functional, "qualified": False,
            "scope": "Actual Qt accessible command path; no pointer/touch or hardware cue proof"}


def live_qualified(result, hour, background=False):
    """Keep background diagnostics outside visible audio/UI hour qualification."""
    gates, native = result.get("hour_gates"), result.get("native", {})
    return bool(hour and not background and result.get("passed") is True and result.get("cleanup_proven") is True
                and result.get("ui_mode", "visible") == "visible" and isinstance(native, dict)
                and native.get("ui_mode", "visible") == "visible" and isinstance(gates, dict) and gates
                and gates.get("visible_ui_qualification_eligible", True) is True)


def summarize_live_observations(path, native):
    # The writer has already bounded lines/bytes. Enforce the same bounds when
    # reopening evidence; this parser never reads a media or slot file.
    line_limit, byte_limit = live_evidence_limits(native.get("mode") == "live-stems-hour")
    if path.stat().st_size > byte_limit:
        raise RuntimeError("Observation file exceeds its bound")
    rows = []
    with path.open("rb") as stream:
        for _ in range(line_limit + 1):
            line = stream.readline(4097)
            if not line:
                break
            if len(line) > 4096 or not line.endswith(b"\n"):
                raise RuntimeError("Malformed or oversized observation line")
            rows.append(json.loads(line))
        else:
            raise RuntimeError("Observation record limit exceeded")
    if not rows or rows[0].get("type") != "setup" or rows[0].get("context") != native["setup"]:
        raise RuntimeError("Observation setup differs from final native context")
    records = rows[1:]
    counters = native["observation_counters"]
    if not (counters["producer_finished"] and not counters["dropped"] and not counters["invalid_clock_reads"]
            and counters["retained"] == counters["attempted"] == len(records)):
        raise RuntimeError("Observation evidence is incomplete")
    names = ("ServiceReady", "RuntimeReady", "RequestObserved", "CommandQueued", "CommandAccepted", "ChunkValidated",
             "PacketAccepted", "PacketDiscarded", "RawComplete", "ConverterComplete", "ServiceClosed")
    counts = dict.fromkeys(names, 0)
    hops, ready_to_pre_emit, validation, publication, full_waits = [], [], [], [], []
    phase_names = LIVE_PHASE_NAMES
    phases = {key: [] for key in phase_names}
    phase_hops = []
    current_epoch, previous_ordinal = 0, None
    epochs, commands, accepted_at, completed_at, telemetry = {}, {}, {}, {}, []
    last_sequence = last_generation = 0; last_accepted_epoch = 0
    total_input, total_output = native["setup"]["input_frames"], native["setup"]["output_frames"]
    for row in records:
        ordinal = row["ordinal"]
        if previous_ordinal is not None and ordinal != previous_ordinal + 1:
            raise RuntimeError("Observation ordinal gap without reported loss")
        previous_ordinal = ordinal
        if not 0 <= row["kind"] < len(names) or row["observed_ns"] <= 0:
            raise RuntimeError("Observation kind/clock is invalid")
        kind = names[row["kind"]]; counts[kind] += 1
        if kind == "RequestObserved":
            current_epoch = row["epoch"]
            if not current_epoch or current_epoch in epochs:
                raise RuntimeError("Repeated or empty provider request epoch")
            epochs[current_epoch] = {"target": row["output_first"], "next": row["output_first"], "generation": None,
                                     "observed_ns": row["observed_ns"], "reserve_published_ns": None, "final": False}
        elif kind == "CommandQueued":
            if row["command_id"] in commands or row["epoch"] not in epochs:
                raise RuntimeError("Unbound or duplicate queued load/seek")
            commands[row["command_id"]] = row
        elif kind == "CommandAccepted":
            queued = commands[row["command_id"]]
            if (row["epoch"], row["input_first"], row["output_first"]) != (queued["epoch"], queued["input_first"], queued["output_first"]) or row["observed_ns"] < queued["observed_ns"]:
                raise RuntimeError("Accepted load/seek lost its exact queued binding")
            if row["generation"] != last_generation + 1:
                raise RuntimeError("Accepted generation did not advance exactly once")
            last_generation = row["generation"]; last_accepted_epoch = row["epoch"]
            epochs[row["epoch"]].update(generation=row["generation"], validated_end=row["input_first"], validated_contiguous=True, raw_service_endpoint=None, command_ns=queued["observed_ns"],
                                        first_validated_ns=None)
            accepted_at[row["generation"]] = row["observed_ns"]
        elif kind == "ChunkValidated":
            epoch = epochs[row["epoch"]]
            end = row["input_first"] + row["input_frames"]
            if (row["generation"] != epoch["generation"] or row["input_first"] < epoch["validated_end"]
                    or row["input_frames"] != min(257985 - row["input_first"] % 257985, total_input - row["input_first"])
                    or not 0 < row["input_frames"] <= 257985 or end > total_input or row["final"] != (end == total_input)
                    or row["lease_sequence"] <= last_sequence):
                raise RuntimeError("Raw lease lost accepted epoch/generation/contiguity/sequence")
            epoch["validated_contiguous"] = epoch["validated_contiguous"] and row["input_first"] == epoch["validated_end"]
            epoch["validated_end"] = end; last_sequence = row["lease_sequence"]
            if epoch["first_validated_ns"] is None:
                epoch["first_validated_ns"] = row["validated_ns"]
            if row["validated_ns"] < row["observed_ns"] or not row["request_id"]:
                raise RuntimeError("Validated lease lost identity/clock binding")
            validation.append((row["validated_ns"] - row["observed_ns"]) / 1e6)
            delay = (row["service_published_seconds"] - row["worker_ready_seconds"]) * 1000
            if not math.isfinite(delay) or delay < 0:
                raise RuntimeError("Python-domain ready-to-pre-emit interval is invalid")
            ready_to_pre_emit.append(delay)
            if row["hop_service_present"]:
                if not math.isfinite(row["hop_service_ms"]) or row["hop_service_ms"] < 0:
                    raise RuntimeError("Invalid optional hop service interval")
                hops.append(row["hop_service_ms"])
            phase_row = {key: row[key] for key in ("epoch", "generation", "request_id", "lease_sequence", "input_first", "input_frames")}
            if row["hop_service_present"]:
                phase_row["hop_service_ms"] = row["hop_service_ms"]
            for key in phase_names:
                if key in row:
                    value = row[key]
                    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                        raise RuntimeError("Invalid optional phase timing")
                    phases[key].append(value)
                    phase_row[key] = value
            phase_hops.append(phase_row)
        elif kind == "PacketAccepted":
            epoch = epochs[row["epoch"]]
            end = row["output_first"] + row["output_frames"]
            if (row["epoch"] != current_epoch or row["generation"] != epoch["generation"] or epoch["final"]
                    or row["output_first"] != epoch["next"] or not 0 < row["output_frames"] <= 4096
                    or (not row["final"] and row["output_frames"] != 4096) or end > total_output
                    or (row["final"] and (end != total_output or not epoch.get("converter_complete")))
                    or not 0 < row["packet_ready_ns"] <= row["publish_begin_ns"] <= row["publish_end_ns"]):
                raise RuntimeError("Accepted packet range/generation/chronology differs")
            epoch["next"], epoch["final"] = end, row["final"]
            publication.append((row["publish_end_ns"] - row["packet_ready_ns"]) / 1e6)
            if row["full_observed"]:
                if not row["packet_ready_ns"] <= row["first_full_ns"] <= row["publish_begin_ns"]:
                    raise RuntimeError("Full wait lies outside packet/publication interval")
                full_waits.append((row["publish_end_ns"] - row["first_full_ns"]) / 1e6)
            if epoch["reserve_published_ns"] is None and end >= min(total_output, epoch["target"] + 262144):
                epoch["reserve_published_ns"] = row["publish_end_ns"]
        elif kind == "PacketDiscarded":
            epoch = epochs[row["epoch"]]
            if row["generation"] != epoch["generation"] or row["output_first"] != epoch["next"] or not 0 < row["output_frames"] <= 4096:
                raise RuntimeError("Observed pending discard lost its old range/generation")
        elif kind == "RawComplete":
            if row["epoch"] not in epochs or row["generation"] != epochs[row["epoch"]]["generation"] or row["input_first"] != total_input or row["input_frames"]:
                raise RuntimeError("Raw completion endpoint differs")
            completed_at[row["generation"]] = row["observed_ns"]
            epochs[row["epoch"]]["raw_service_endpoint"] = total_input
        elif kind == "ConverterComplete":
            if (row["epoch"] not in epochs or row["generation"] != epochs[row["epoch"]]["generation"] or row["input_first"] != total_input or
                    row["output_first"] != total_output or row["input_frames"] or row["output_frames"] or
                    completed_at.get(row["generation"], math.inf) > row["observed_ns"] or
                    not epochs[row["epoch"]]["validated_contiguous"] or epochs[row["epoch"]]["validated_end"] != total_input):
                raise RuntimeError("Converter completion endpoint/binding differs")
            epochs[row["epoch"]]["converter_complete"] = True
        elif kind == "ServiceClosed":
            if (row["epoch"] != last_accepted_epoch or row["generation"] != last_generation or
                    not epochs[last_accepted_epoch]["validated_end"] <= row["input_first"] <= total_input):
                raise RuntimeError("Closed raw endpoint/binding differs")
            epochs[last_accepted_epoch]["raw_service_endpoint"] = row["input_first"]
        if row["telemetry_dropped_present"]:
            telemetry.append({"kind": kind, "generation": row["generation"], "dropped": row["telemetry_dropped"]})
    stats = duration_stats
    seek = next((row for row in native["controls"] if row["kind"] == "incomplete_generation_seek"), None)
    seek_observed = bool(seek and accepted_at.get(seek["previous_generation"], math.inf) <= seek["issued_ns"]
        and completed_at.get(seek["previous_generation"], math.inf) > seek["issued_ns"])
    for epoch in epochs.values():
        if epoch.get("first_validated_ns") is not None:
            epoch["command_to_first_validated_ms"] = (epoch["first_validated_ns"] - epoch["command_ns"]) / 1e6
        if epoch.get("reserve_published_ns") is not None:
            epoch["command_to_published_reserve_ms"] = (epoch["reserve_published_ns"] - epoch["command_ns"]) / 1e6
    setup = native["setup"]
    return {"service_telemetry_loss": telemetry, "service_metrics_complete": not any(row["dropped"] for row in telemetry),
            "factory_spans_ms": {"entry_to_capture": (setup["input_captured_ns"] - setup["factory_entry_ns"]) / 1e6,
                "capture_to_count": (setup["count_complete_ns"] - setup["input_captured_ns"]) / 1e6,
                "count_to_commit": (setup["factory_committed_ns"] - setup["count_complete_ns"]) / 1e6},
            "load_to_first_cursor_poll_upper_ms": (native["first_advance_observed_ns"] - native["load_issued_ns"]) / 1e6,
            "incomplete_generation_seek": {"submitted": seek is not None, "exercised": seek_observed,
                "scope": "Retrospective accepted/RawComplete C++ stamps at seek issue; no native inference-start claim"},
            "scope": ("Sustained repeated-generation actual-player observations; qualification remains gated by native health and wrapper cleanup; no physical/consumed timing qualification"
                      if native.get("mode") == "live-stems-hour" else
                      "Short repeated-generation diagnostics; no sustained p99 or physical/consumed timing qualification"),
            "kind_counts": counts, "epochs": epochs, "service_hops": {**stats(hops), "strict_target_ms": 4095,
                "at_or_above_target": sum(value >= 4095 for value in hops), "complete": len(hops) == counts["ChunkValidated"] > 0 and not any(row["dropped"] for row in telemetry)}, "lease_validation": stats(validation),
            "worker_ready_to_service_pre_emit": {**stats(ready_to_pre_emit),
                "scope": "Python monotonic domain only; excludes JSON serialization/write, pipe delivery and C++ receipt"},
            "chunk_phases": {"scope": "Original nested durations in ms for validated chunks only; omissions remain absent. Preroll, canceled and discarded work is not fully covered. No cross-clock subtraction or additive phase interpretation.",
                "hops": phase_hops, "fields": {key: {**stats(values), "present_for_all_chunks": len(values) == counts["ChunkValidated"] > 0}
                                                 for key, values in phases.items()}},
            "packet_ready_to_publish": stats(publication), "actual_full_waits": stats(full_waits),
            "discard_coverage": "Observed discards validated; nonempty discard is not forced", "file": pin(path)}


def run_live(args):
    import selectors
    hour = args.live_stems_hour
    background = args.background
    active_controls = getattr(args, "active_controls", False)
    binary = args.binary.resolve(strict=True)
    source, peer = args.track_a.resolve(strict=True), args.track_b.resolve(strict=True)
    assets = {name: getattr(args, name).absolute() for name in ("python", "exporter", "worker", "service", "runtime", "model")}
    for path in assets.values():
        if not path.is_absolute() or not path.resolve(strict=True).is_file():
            raise RuntimeError("Live tools must be explicit regular executable/source/model paths")
    output = args.output.absolute(); output.mkdir(mode=0o700, parents=False, exist_ok=False)
    cache = output / "cache"; cache.mkdir(mode=0o700); stop = output / "stop"
    result = {"schema": 2, "passed": False, "qualified": False, "mode": "live-stems-hour" if hour else "live-stems", "requested_seconds": args.seconds,
              "ui_mode": "background" if background else "visible", "visible_ui_required": not background,
              "started_at_utc": datetime.now(timezone.utc).isoformat(), "scope": "Explicit short native actual-model controller playback; no endurance, fallback or physical latency qualification"}
    if active_controls:
        result["mode"] = "live-stems-controls"
        result["scope"] = "Short visible actual-player accessible UI command path under load; no pointer/touch, physical assistive technology or sustained qualification"
    if hour:
        result["scope"] = "Sustained actual-player qualification only for the recorded two-deck, one-separator fixed mix; other M2 requirements remain open"
        result["qualified_scope"] = "Exact pinned source/model/runtime, Vulkan two threads, Best conversion, 48kHz/256 stereo, A stems/B finite peer, fixed gains, visible Main.qml, float WAV; no hardware cue, EQ/effects, four-deck, fallback, musical-quality or physical latency qualification"
    if background:
        result["scope"] = "Background actual-player audio/stem diagnostics with observed UI coverage; no visible UI or full audio/UI qualification"
        result.pop("qualified_scope", None)
    child = None; root_identity = None; owned = {}; observed_groups = set(); known_graph = set(); requested_stop = None; stop_at = None
    previous = {}; began = time.monotonic(); deadline = began + args.seconds + 180
    selector = selectors.DefaultSelector(); captures = {}; complete_capture = True
    ownership_complete = True
    command_history = {}; ownership_failures = []; ownership_failure_count = 0; ownership_failure_overflow = False
    def live_pin(path, maximum=1024 * 1024 * 1024):
        canonical = path.resolve(strict=True)
        descriptor = os.open(canonical, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            before = os.fstat(descriptor)
            identity = lambda st: (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns)
            if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= maximum:
                raise RuntimeError("Live pin must be a bounded nonempty regular file")
            digest = hashlib.sha256(); count = 0
            while count < before.st_size:
                if time.monotonic() >= deadline - 15:
                    raise RuntimeError("Live setup pin exhausted wall budget")
                data = os.read(descriptor, min(1024 * 1024, before.st_size - count))
                if not data:
                    raise RuntimeError("Live pin changed/truncated")
                digest.update(data); count += len(data)
            if identity(os.fstat(descriptor)) != identity(before) or path.resolve(strict=True) != canonical or identity(canonical.stat()) != identity(before):
                raise RuntimeError("Live pin identity changed")
            return {"path": str(path), "canonical_path": str(canonical), "bytes": count, "sha256": digest.hexdigest(),
                    "identity": list(identity(before))}
        finally:
            os.close(descriptor)
    def live_guard():
        raw = read_command(["systemctl", "--user", "show", "thermal-guard.service",
            "--property=ActiveState,MainPID,InvocationID,ExecMainStartTimestampMonotonic"], timeout=max(.1, min(3, deadline - time.monotonic() - 10)))
        fields = dict(line.split("=", 1) for line in raw.splitlines() if "=" in line)
        if fields.get("ActiveState") != "active" or int(fields.get("MainPID", 0)) <= 0:
            raise RuntimeError("Existing thermal guard must stay active")
        identity = process_identity(int(fields["MainPID"]))
        if str(args.guard_script.resolve(strict=True)) not in identity["command"]:
            raise RuntimeError("Active guard command differs")
        return {"service": fields, "process": identity, "script": live_pin(args.guard_script.resolve(), 1024 * 1024),
                "unit": live_pin(args.guard_unit.resolve(), 1024 * 1024)}
    def process_record(pid):
        root = Path("/proc") / str(pid)
        if root.stat().st_uid != os.getuid():
            raise RuntimeError("Owned process UID differs")
        fields = (root / "stat").read_text().rsplit(")", 1)[1].split()
        command = (root / "cmdline").read_bytes()
        if len(command) > 65536:
            raise RuntimeError("Owned process command exceeds bound")
        return {"pid": pid, "start_ticks": int(fields[19]), "group": int(fields[2]), "session": int(fields[3]),
                "ppid": int(fields[1]), "state": fields[0], "observed_monotonic_ns": time.monotonic_ns(),
                "command": [part.decode(errors="replace") for part in command.split(b"\0") if part]}
    def same_identity(first, second):
        return all(first[key] == second[key] for key in ("pid", "start_ticks", "group", "session"))
    def request_stop(reason):
        nonlocal requested_stop, stop_at
        if requested_stop is None:
            requested_stop, stop_at = reason, time.monotonic(); stop.write_text(reason + "\n")
    def record_ownership_loss(reason, pid, parent_proof=None, observed=None):
        nonlocal ownership_complete, ownership_failure_count, ownership_failure_overflow
        ownership_complete = False; ownership_failure_count += 1
        if len(ownership_failures) >= 128:
            ownership_failure_overflow = True
            raise RuntimeError("Owned proof-loss history exceeds fixed128 bound")
        def context(value):
            return None if value is None else {key: value[key] for key in ("pid", "start_ticks", "group", "session", "ppid")}
        ownership_failures.append({"reason": reason, "pid": pid, "observed_monotonic_ns": time.monotonic_ns(),
            "parent_proof": context(parent_proof), "observed_identity": context(observed)})
    def remember_command(identity, parent_proof):
        key = tuple(identity[name] for name in ("pid", "start_ticks", "group", "session"))
        if key not in command_history:
            if len(command_history) >= 64:
                record_ownership_loss("command_history_identity_bound", identity["pid"], parent_proof, identity)
                raise RuntimeError("Owned command identity history exceeds fixed64 bound")
            command_history[key] = []
        history = command_history[key]
        if any(value["command"] == identity["command"] for value in history):
            return
        if len(history) >= 8:
            record_ownership_loss("command_history_argv_bound", identity["pid"], parent_proof, identity)
            raise RuntimeError("Owned command history exceeds fixed8 argv bound")
        snapshot = {**identity, "command": list(identity["command"]), "parent_identity": None if parent_proof is None
            else {name: parent_proof[name] for name in ("pid", "start_ticks", "group", "session", "ppid")}}
        history.append(snapshot)
    def proven_command_snapshots():
        return [value for history in command_history.values() for value in history]
    def remember_owned():
        if child is None or root_identity is None:
            return
        pending = [(child.pid, None)]; visited = set()
        while pending:
            pid, parent_proof = pending.pop()
            if pid in visited:
                continue
            visited.add(pid)
            if len(visited) > 64:
                raise RuntimeError("Owned process observation exceeds fixed64 tree bound")
            try:
                identity = process_record(pid)
                if parent_proof is None:
                    if not same_identity(identity, root_identity):
                        return  # Never adopt a replacement for our direct child.
                elif identity["ppid"] != parent_proof["pid"]:
                    record_ownership_loss("parent_pid_changed_before_walk", pid, parent_proof, identity); continue
                elif not same_identity(process_record(parent_proof["pid"]), parent_proof):
                    record_ownership_loss("parent_identity_changed_before_walk", pid, parent_proof, identity); continue
                key = (pid, identity["start_ticks"])
                if key not in owned and len(owned) >= 64:
                    raise RuntimeError("Owned identity history exceeds fixed64 bound")
                tasks = list((Path("/proc") / str(pid) / "task").iterdir())
                if len(tasks) > 256:
                    raise RuntimeError("Owned process exceeds fixed256 thread bound")
                children = set()
                for task in tasks:
                    try:
                        raw = (task / "children").read_text()
                        if len(raw) > 16384:
                            raise RuntimeError("Owned thread children exceeds bound")
                        children.update(int(value) for value in raw.split())
                    except (FileNotFoundError, ProcessLookupError):
                        pass
                if not same_identity(process_record(pid), identity):
                    record_ownership_loss("identity_changed_during_walk", pid, parent_proof, identity); continue
                if parent_proof is not None and not same_identity(process_record(parent_proof["pid"]), parent_proof):
                    record_ownership_loss("parent_identity_changed_during_walk", pid, parent_proof, identity); continue
                remember_command(identity, parent_proof)
                owned[key] = identity; observed_groups.add(identity["group"])
                if len(observed_groups) > 64:
                    raise RuntimeError("Owned group history exceeds fixed64 bound")
                pending.extend((value, identity) for value in children)
            except (FileNotFoundError, ProcessLookupError):
                if parent_proof is not None:
                    record_ownership_loss("process_or_parent_disappeared_during_walk", pid, parent_proof)
    def live_owned():
        matches = {}
        for (pid, _), expected in owned.items():
            try:
                fields = (Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()
                if (int(fields[19]), int(fields[2]), int(fields[3])) == (expected["start_ticks"], expected["group"], expected["session"]):
                    matches[pid] = expected
            except (FileNotFoundError, ProcessLookupError):
                pass
        return matches
    def signal_owned(number):
        # Popen owns this unreaped direct child and start_new_session fixes its
        # initial group even if /proc/pipe registration failed before snapshots.
        if child is not None and child.poll() is None:
            try:
                os.killpg(child.pid, number)
            except ProcessLookupError:
                pass
        matches = list(live_owned().values())
        if any(value["session"] != value["group"] for value in matches):
            raise RuntimeError("Owned cleanup group/session binding differs")
        for group in {value["group"] for value in matches}:
            if group <= 1 or group == os.getpgrp():
                raise RuntimeError("Refusing unsafe cleanup process group")
            try:
                os.killpg(group, number)
            except ProcessLookupError:
                pass
    def drain_capture(timeout=.1):
        nonlocal complete_capture
        for key, _ in selector.select(timeout):
            try:
                data = os.read(key.fd, 8192)
            except BlockingIOError:
                continue
            if not data:
                selector.unregister(key.fileobj); key.fileobj.close(); continue
            stream, count, digest = captures[key.data]
            digest.update(data)
            if count + len(data) > 2 * 1024 * 1024:
                complete_capture = False; request_stop("Native stdout/stderr bound exceeded")
            else:
                stream.write(data)
            captures[key.data] = (stream, count + len(data), digest)
    try:
        result["human_before_setup"] = live_readiness(args.readiness_decision, output.name, background)
        source_root = Path(__file__).resolve().parents[2]
        ui_assets = ("qml/Main.qml", "qml/DeckSurface.qml", "qml/MixerSurface.qml", "qml/DeckButton.qml", "qml/SurfaceKnob.qml", "qml/StereoMeter.qml",
            "brand/integration/qtquick/ConfluxTheme.qml", "brand/integration/qtquick/qmldir",
            "brand/assets/logos/conflux-horizontal-dark.svg", "brand/assets/logos/conflux-horizontal-light.svg",
            "brand/assets/fonts/Barlow-Regular.ttf", "brand/assets/fonts/BarlowCondensed-SemiBold.ttf", "brand/assets/fonts/JetBrainsMono-Regular.ttf")
        result["runtime_ui_source_root"] = str(source_root)
        paths = {"binary": binary, "track_a": source, "track_b": peer, "runner": Path(__file__).resolve(), **assets,
                 **{"ui:" + name: source_root / name for name in ui_assets}}
        result["pins_before"] = {name: live_pin(path, 256 * 1024 * 1024 if name not in ("track_a", "track_b") else 1024**3) for name, path in paths.items()}
        if result["pins_before"]["runtime"]["sha256"] != FLASH_RUNTIME_SHA256 or result["pins_before"]["model"]["sha256"] != FLASH_MODEL_SHA256:
            raise RuntimeError("Actual-model host check requires the exact promoted Flash runtime/model")
        result["guard_before"] = live_guard()
        result["power_before"] = power_state()
        if not result["power_before"]["ac_online"] or not result["power_before"]["lid_open"]:
            raise RuntimeError("Live host check requires AC power and open lid")
        sensors = sensor_paths(); result["host_before"] = observation(sensors)
        result["required_free_bytes"] = live_required_free_bytes(args.seconds, hour)
        if shutil.disk_usage(output).free < result["required_free_bytes"]:
            raise RuntimeError("Insufficient free space for bounded cache, recording and evidence")
        result["human_before_launch"] = live_readiness(args.readiness_decision, output.name, background)
        if result["human_before_launch"]["pin"] != result["human_before_setup"]["pin"]:
            raise RuntimeError("Readiness record changed during setup")
        environment = os.environ.copy(); environment.update({"CONFLUX_ALLOW_HOST_SOAK": "1", "XDG_CACHE_HOME": str(cache)})
        for number in (signal.SIGINT, signal.SIGTERM):
            previous[number] = signal.signal(number, lambda signum, _frame: request_stop(f"runner signal {signum}"))
        native_mode = "--live-stems-hour" if hour else "--live-stems"
        if active_controls:
            native_mode = "--live-stems-controls"
        if background:
            native_mode += "-background"
        command = [str(binary), native_mode, str(source), str(peer), str(output), str(args.seconds), str(stop),
                   *(str(assets[name]) for name in ("python", "exporter", "worker", "service", "runtime", "model"))]
        result["command"] = command
        for name in ("stdout", "stderr"):
            captures[name] = ((output / f"native.{name}.log").open("wb"), 0, hashlib.sha256())
        native_started = time.monotonic()
        child = subprocess.Popen(command, env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, start_new_session=True)
        result["owned_direct_group"] = child.pid
        root_identity = process_record(child.pid)
        for name in ("stdout", "stderr"):
            pipe = getattr(child, name); os.set_blocking(pipe.fileno(), False); selector.register(pipe, selectors.EVENT_READ, name)
        remember_owned()
        next_sample = next_guard = next_graph = 0.0
        progress_stamp = None; progress_seen_at = time.monotonic()
        with (output / "host-observations.jsonl").open("w") as host_log:
            while child.poll() is None or selector.get_map():
                drain_capture()
                now = time.monotonic()
                remember_owned()
                if stop.exists() and requested_stop is None:
                    request_stop("user stop file")
                if now >= next_sample:
                    host = observation(sensors); host["power"] = power_state()
                    host_log.write(json.dumps(host) + "\n"); host_log.flush(); next_sample = now + 2
                    if not host["power"]["ac_online"] or not host["power"]["lid_open"] or not host["temperatures_millidegrees_C"]:
                        request_stop("Power/lid/thermal observation failed")
                if now >= next_guard:
                    if live_guard() != result["guard_before"]:
                        request_stop("Existing thermal guard identity changed")
                    next_guard = now + 10
                progress = output / "progress.json"
                if progress.exists():
                    if progress.stat().st_size > 16384:
                        raise RuntimeError("Progress exceeds fixed metadata bound")
                    pulse = json.loads(progress.read_text()); result["last_progress"] = pulse
                    if pulse["monotonic_ns"] != progress_stamp:
                        progress_stamp = pulse["monotonic_ns"]; progress_seen_at = now
                    if pulse["phase"] in ("steady", "replay_recovery", "incomplete_generation_seek") and now - progress_seen_at > 10:
                        request_stop("Native live progress unchanged for ten seconds")
                elif now - native_started > 30:
                    request_stop("Native startup progress missing")
                if now >= next_graph and progress.exists():
                    objects = json.loads(read_command(["pw-dump", "-N"], timeout=max(.1, min(8, deadline - time.monotonic() - 2))))
                    for item in owned_objects(objects, child.pid):
                        known_graph.add((item["id"], str(item.get("info", {}).get("props", {}).get("object.serial", ""))))
                    next_graph = now + 5
                if now >= deadline - 45:
                    request_stop("Fixed live-run wall budget reached")
                if requested_stop is not None and now - stop_at >= 20:
                    break
                if now >= deadline - 25:
                    break
        result["native_exit"] = child.poll()
        native_path = output / "native-result.json"
        if not native_path.exists() or native_path.stat().st_size > 1024 * 1024:
            raise RuntimeError("Bounded native result missing")
        result["native"] = json.loads(native_path.read_text())
        native = result["native"]
        if native.get("functional_only") is not False or native.get("host_audio") is not True or native.get("source_root") != str(source_root):
            raise RuntimeError("Synthetic/private result cannot qualify as host evidence")
        if native.get("ui_mode", "visible") != result["ui_mode"]:
            raise RuntimeError("Native UI mode differs from explicitly launched mode")
        if background and native.get("ui_coverage", {}).get("visible_ui_qualification_eligible") is not False:
            raise RuntimeError("Background result must exclude visible UI qualification")
        result["ui_coverage"] = native.get("ui_coverage")
        result["observations"] = summarize_live_observations(output / "observations.jsonl", native)
        if active_controls:
            result["active_controls"] = validate_active_controls(native, result["observations"])
        setup = native["setup"]
        if any(setup[key] != result["pins_before"][name]["sha256"] for key, name in (
                ("source_sha256", "track_a"), ("model_sha256", "model"), ("runtime_sha256", "runtime"), ("service_sha256", "service"))):
            raise RuntimeError("Published live context differs from launched input/engine")
        result["pins_after"] = {name: live_pin(path, 256 * 1024 * 1024 if name not in ("track_a", "track_b") else 1024**3) for name, path in paths.items()}
        result["pins_unchanged"] = result["pins_before"] == result["pins_after"]
        result["host_after"] = observation(sensors); result["thermal_summary"] = thermal_summary(output / "host-observations.jsonl")
        result["power_after"] = power_state(); result["guard_after"] = live_guard()
        if hour:
            result["hour_gates"] = validate_hour_result(native, result["observations"], background)
        result["passed"] = bool(child.poll() == 0 and native["passed"] and not requested_stop and complete_capture and result["pins_unchanged"]
            and result["guard_after"] == result["guard_before"] and result["power_after"]["ac_online"] and result["power_after"]["lid_open"] and known_graph)
    except Exception as error:
        result["error"] = str(error)
        result["error_type"] = type(error).__name__
        if isinstance(error, OSError):
            result["error_errno"] = error.errno
        if child is not None and child.poll() is None:
            request_stop("Live runner observation error")
    finally:
        for number in previous:
            signal.signal(number, signal.SIG_IGN)
        try:
            if child is not None:
                remember_owned()
                until = min(deadline - 15, time.monotonic() + 20)
                while child.poll() is None and time.monotonic() < until:
                    drain_capture(.1); remember_owned()
                signal_owned(signal.SIGTERM)
                until = min(deadline - 12, time.monotonic() + 1)
                while live_owned() and time.monotonic() < until:
                    drain_capture(.05)
                signal_owned(signal.SIGKILL)
                child.wait(timeout=2)
                while selector.get_map() and time.monotonic() < deadline - 10:
                    drain_capture(.05)
                remaining = live_owned()
                objects = json.loads(read_command(["pw-dump", "-N"], timeout=max(.1, min(8, deadline - time.monotonic() - 2))))
                graph_remaining = [item for item in objects if (item["id"], str(item.get("info", {}).get("props", {}).get("object.serial", ""))) in known_graph
                    or str(item.get("info", {}).get("props", {}).get("application.process.id", "")) == str(child.pid)]
                save(output / "owned-pipewire-cleanup.json", {"remaining_objects": graph_remaining})
                live_jobs = sorted(str(path.relative_to(cache)) for path in cache.rglob(".live-*"))
                groups = observed_groups | {child.pid}
                absent = {}
                for group in groups:
                    try:
                        os.killpg(group, 0); absent[str(group)] = False
                    except ProcessLookupError:
                        absent[str(group)] = True
                    except PermissionError:
                        absent[str(group)] = False
                service_path = str(assets["service"].resolve())
                runtime_path, model_path = str(assets["runtime"].resolve()), str(assets["model"].resolve())
                def service_matches(value):
                    command = value["command"]
                    if len(command) < 3 or command[0] not in (str(assets["python"]), str(assets["python"].resolve())) or command[1:3] != ["-B", service_path]:
                        return False
                    return all(command.count(flag) == 1 and command.index(flag) + 1 < len(command) and command[command.index(flag) + 1] == expected
                        for flag, expected in (("--runtime", runtime_path), ("--model", model_path), ("--backend", "vulkan"), ("--threads", "2")))
                snapshots = proven_command_snapshots()
                services = [value for value in snapshots if service_matches(value)]
                runtimes = [value for value in snapshots if value["command"] == [runtime_path, "--model", model_path, "--backend", "vulkan", "--threads", "2"]]
                engine_observed = bool(services and runtimes and all(any(value["group"] == service["group"] and
                    value["session"] == service["session"] and value["parent_identity"] is not None and
                    same_identity(value["parent_identity"], service) for service in services) for value in runtimes))
                cooperative = bool(result.get("native", {}).get("shutdown_completed") and child.returncode in (0, 1, 2))
                result["cleanup"] = {"known_processes": list(owned.values()), "remaining_known_processes": list(remaining.values()),
                    "leader_reaped": child.poll() is not None, "remaining_pipewire_count": len(graph_remaining), "remaining_live_jobs": live_jobs,
                    "cooperative_native_shutdown": cooperative, "ownership_complete": ownership_complete, "owned_group_absent": absent,
                    "service_processes": services, "runtime_processes": runtimes, "engine_processes_observed": engine_observed,
                    "scope": "Known descendant snapshots cannot discover an unobserved escaped session; abnormal/missing cooperative completion preserves cache even when known groups are gone"}
                proven = cooperative and ownership_complete and engine_observed and all(absent.values()) and not remaining and not graph_remaining and not live_jobs and not selector.get_map()
            else:
                proven = True  # No app/worker was launched.
            result["cleanup_proven"] = proven
            if proven:
                shutil.rmtree(cache)
            result["owned_cache_removed"] = not cache.exists()
            result["cache_preserved"] = cache.exists()
            result["passed"] = bool(result["passed"] and proven)
        except Exception as error:
            if child is not None and child.poll() is None:
                try:
                    os.killpg(child.pid, signal.SIGKILL); child.wait(timeout=2)
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
            result["cleanup_error"] = str(error); result["cleanup_proven"] = False; result["cache_preserved"] = cache.exists(); result["passed"] = False
        finally:
            result.setdefault("cleanup", {}).update({"ownership_complete": ownership_complete,
                "proven_command_history": proven_command_snapshots(), "ownership_failures": ownership_failures,
                "ownership_failure_count": ownership_failure_count, "ownership_failure_overflow": ownership_failure_overflow})
            selector.close()
            if child is not None:
                for pipe in (child.stdout, child.stderr):
                    if pipe is not None and not pipe.closed:
                        pipe.close()
            for stream, count, digest in captures.values():
                stream.close()
            result["capture"] = {name: {"bytes_received": count, "sha256_received": digest.hexdigest(), "complete": complete_capture,
                                           "retained_file": pin(output / f"native.{name}.log")} for name, (_, count, digest) in captures.items()}
            for number, handler in previous.items():
                signal.signal(number, handler)
        result["stop_reason"] = requested_stop; result["elapsed_seconds"] = time.monotonic() - began
        result["wall_budget_seconds"] = args.seconds + 180; result["within_wall_budget"] = time.monotonic() <= deadline
        result["passed"] = bool(result["passed"] and result["within_wall_budget"])
        result["qualified"] = live_qualified(result, hour, background)
        result["finished_at_utc"] = datetime.now(timezone.utc).isoformat(); save(output / "result.json", result)
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 2 if requested_stop else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-host-audio", action="store_true", required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--track-a", type=Path, required=True)
    parser.add_argument("--track-b", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="fresh evidence directory")
    parser.add_argument("--seconds", type=int, default=None)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--live-stems", action="store_true", help="short actual-model live controller diagnostic")
    modes.add_argument("--live-stems-hour", action="store_true", help="exactly one hour of sustained actual-model controller playback")
    parser.add_argument("--background", action="store_true", help="live audio/stem diagnostics without visible UI qualification")
    parser.add_argument("--active-controls", action="store_true", help="short visible live mode with 16 actual accessible UI actions")
    for name in ("python", "exporter", "worker", "service", "runtime", "model", "readiness-decision"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--guard-script", type=Path, default=Path.home() / ".local/bin/thermal-guard")
    parser.add_argument("--guard-unit", type=Path, default=Path.home() / ".config/systemd/user/thermal-guard.service")
    args = parser.parse_args()
    if args.active_controls and (not args.live_stems or args.live_stems_hour or args.background):
        parser.error("--active-controls requires short visible --live-stems")
    if args.background and not (args.live_stems or args.live_stems_hour):
        parser.error("--background requires --live-stems or --live-stems-hour")
    if args.seconds is None:
        args.seconds = 3600 if args.live_stems_hour else 120 if args.live_stems else 1800
    live_names = ("python", "exporter", "worker", "service", "runtime", "model", "readiness_decision")
    if args.live_stems or args.live_stems_hour:
        if (args.seconds != 3600 if args.live_stems_hour else not 120 <= args.seconds <= 180) or any(getattr(args, name) is None or not getattr(args, name).is_absolute() for name in live_names):
            parser.error("Live mode requires120–180 seconds (hour: exactly3600) and explicit absolute tool/readiness paths")
        if os.environ.get("CONFLUX_PRIVATE_ROUTING_CHECK") or os.environ.get("QT_QPA_PLATFORM") in ("offscreen", "minimal"):
            parser.error("Live host launcher rejects private/offscreen qualification")
        return run_live(args)
    if any(getattr(args, name) is not None for name in live_names):
        parser.error("Stem tool/readiness options require --live-stems or --live-stems-hour")
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

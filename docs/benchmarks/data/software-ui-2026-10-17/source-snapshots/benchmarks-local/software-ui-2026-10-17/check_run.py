# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check completed small reports/lifecycles only, never parse a live trace."""
from datetime import datetime, timezone
import json
from pathlib import Path
import sys

base = Path(__file__).resolve().parent
name = sys.argv[1]
run = base / name
result = json.loads((run / "result.json").read_text())
mode = result["load_mode"]
selected = {"none": set(), "audio": {"audio"}, "both": {"audio", "ui"}, "ui": {"ui"}}[mode]
assert result["status"] == "ok" and not result["failures"]
assert result["workload_seconds"] >= result["requested_seconds"]
assert set(result["selected_loads"]) == selected == set(result["load"]["children"])
assert set(result["load"]["launched_loads"]) == selected
assert not result["load"]["errors"]
assert result["acceptance"]["selected_load_coverage_completed"]
for probe in ("audio", "ui"):
    coverage = result[probe + "_coverage"]
    if probe in selected:
        assert coverage["requested"] and coverage["passed"]
        assert result["load"]["children"][probe]["returncode"] == 0
    else:
        assert coverage["requested"] is False and coverage["status"] == "not_run" and coverage["passed"] is None
        assert not (run / (probe + ".json")).exists()
        assert not (run / (probe + ".stderr")).exists()
if "ui" not in selected:
    assert not (run / "ui-progress.json").exists()
if "audio" in selected:
    faults = result["load"]["audio_fault_counters"]
    assert len(faults) == 6 and all(value == 0 for value in faults.values())
else:
    faults = None
    assert result["concurrent_audio_workload_seconds"] is None
stats = result["worker_statistics"]
assert len(result["runtime_ready"]) == stats["backend_start_count"] == 1
assert stats["ready_high_water"] <= 2 and stats["telemetry_dropped"] == 0
assert result["stale_chunks_delivered"] == 0
assert result["fixture"]["sha256"] == "840561b2f6c6cd26ab70b0e2adef629f3eee09878333b1e41f901972504546c4"
launch = json.loads((base / (name + "-launch.json")).read_text())
expected = launch["expected_producer"]
assert expected["status"] == "ready" and result["repository_head"] == expected["repository_head"]
assert set(result["pins"]) == set(expected["pins"])
assert all(result["pins"][key]["sha256"] == value for key, value in expected["pins"].items())
assert result["renderer"] == launch["renderer"] in ("vulkan", "software")
assert result["load"]["children"]["ui"]["report"]["graphics_api"] == (1 if result["renderer"] == "software" else 5)
assert result["load"]["children"]["ui"]["report"]["platform"] == "wayland"
assert launch["repaint_ms"] == 33
assert launch["UI_diagnostic_environment"]["CONFLUX_M0_REPAINT_MS"] == str(launch["repaint_ms"])
assert launch["diagnostic_ui_logging"] is False
assert launch["host_conditions_before_spawn"]["ready"]
admission = json.loads((base / (name + "-admission.json")).read_text())
assert admission["stage"] == "admitted" and admission["host_conditions_before_admission"]["ready"]
assert not (run / "source.wav").exists()
assert result["controls"] and all(control["complete"] and control["held_chunk_rejected"] for control in result["controls"])
assert all(control["parity"]["exact"] and control["parity"]["max_abs_difference"] == 0 for control in result["controls"])
guard = json.loads((base / (name + "-guard.json")).read_text())
policy = json.loads((base / (name + "-policy.json")).read_text())
assert guard["status"] == "ok" and not guard["errors"] and guard["idle_inhibit_released"]
assert guard["window_guard"]["requested"] == ("ui" in selected)
if "ui" in selected:
    assert guard["script_unloaded"] and guard["restored"]
else:
    assert guard["idle_only"] and guard["window_guard"]["status"] == "not_run" and "script_id" not in guard
assert policy["runner_returncode"] == policy["benchmark_returncode"] == 0
assert not any(policy.get(key) for key in ("error", "child_cleanup_error", "restore_error",
                                          "before_restore_policy_error", "after_policy_error"))
if policy.get("mode") == "observe":
    assert policy["profile_mutation"] is False and policy["restored_by_runner"] is False
    assert policy["before"]["profile"] == "balanced" and not policy["before"]["holds"]
    assert all(policy[phase]["profile"] for phase in ("during", "before_restore", "after"))
else:
    assert policy["during"]["profile"] == policy["before_restore"]["profile"] == "balanced"
    assert not policy["after"]["holds"]
    assert all(policy["before"][field] == policy["after"][field] for field in ("profile", "firmware", "cpu_epp"))
record = {"verified_at_utc": datetime.now(timezone.utc).isoformat(), "run": name, "load_mode": mode,
          "small_report_and_lifecycle_checks_passed": True, "workload_seconds": result["workload_seconds"],
          "timing": result["timing"]["continuous"], "controls": len(result["controls"]),
          "audio_faults": faults, "selected_loads": sorted(selected), "scope": "Saved final report checks; full trace/event audits are separate."}
(base / (name + "-verification.json")).write_text(json.dumps(record, indent=2) + "\n")
print(json.dumps(record))

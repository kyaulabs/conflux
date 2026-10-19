#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Adapted Flash cost leg: exact per-arm proof, no cross-arm identity claim."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import sys
import time
from types import SimpleNamespace

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
LEGS = ("A1", "B1", "B2", "A2")
ARMS = {
    "A": (Path("/tmp/conflux-stem-phase-timing-2026-10-18/build/conflux-stem-runtime"),
          "ded9cb2c1000af78917a784e281d905da201d735338221519958d9d7df1d681f"),
    "B": (Path('/tmp/conflux-stem-flash-f32-2026-10-18/build/conflux-stem-runtime'),
          '6f311e461e7af5f12ff38f90ddb8720179c2c3dce23d593437d2b34524dffd9d'),
}
HELPERS = {
    "common": (BASE / "graph_order_compare_launch.py", "f3a7073e0774f62597d24e5b9e6e49a594f13c0e6429a339947b467cd5a6a6b4"),
    "graph": (BASE / "graph_order_compare.py", "28d2712ba427d0f480ad63f8ff3128f0d3999010e7ecf225185c41734861a3e7"),
    "retained": (BASE / "observed_launch.py", "511bb712d2f7af01abb1ab25ca92b001c6f82af4ee6f42547a5b43fa32b237d6"),
}
PROOF = BASE / "phase-timing-laptop-01-comparison/result.json"
PROOF_SHA = "38c60301feed297a0484d61ea92cca2413067d15d7ead7e9a27e6cc9909037e3"
SAMPLER_SHA = "8f079c14dfa35412a05b8f52c1660cfcb5d545d9b2b85f7e066f78c8fcc61b5b"
DRIVER_SECONDS, AFTER_PINS_SECONDS, REQUEST_SECONDS = 180, 60, 30
SCOPE = ("Four windows at source frame 257985 in Club XL/When the Light Breaks/Under My Skin/STFU order. "
         "One persistent Vulkan/two-thread/blocking/passive child per declared leg. All startup/request "
         "observations retained; no exclusions, UI/audio load, profiler, host mutation, p99 qualification "
         "or causal speed claim. Sampler capture intervals/overhead and missing fields remain explicit.")


def require(value, message):
    if not value:
        raise RuntimeError(message)


def helpers():
    modules = {}
    for name, (path, digest) in HELPERS.items():
        require(path.is_file() and path.stat().st_size <= 128 * 1024
                and hashlib.sha256(path.read_bytes()).hexdigest() == digest, "Frozen helper differs: " + name)
        spec = importlib.util.spec_from_file_location("conflux_abba_" + name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        modules[name] = module
    graph, common = modules["graph"], modules["common"]
    compare = graph.verified_driver()
    compare.REQUIRED_SOURCES = tuple(dict.fromkeys(compare.REQUIRED_SOURCES + (
        Path(__file__).resolve(), BASE / "counterbalanced_launch.py",
        ARMS["A"][0].parent / "conflux-graph-phase-timing/pipeline.cpp")))
    return compare, graph, common, modules["retained"]


def observation_summary(path, supervisor_pid, native_pid, package_paths):
    """Reduce closed raw rows only; preserve all rows and separate identities."""
    summary = {"supervisor_pid": supervisor_pid, "native_pid": native_pid,
               "supervisor_samples": 0, "native_samples": 0, "other_pid_samples": 0,
               "supervisor_start_ticks": [], "native_start_ticks": [],
               "native_clock_observations": {}, "native_package_observations": {},
               "native_profile_observations": 0, "max_capture_elapsed_ms": None,
               "max_sample_gap_ms": None, "first_native_timestamp": None, "last_native_timestamp": None}
    require(path.stat().st_size <= 32 * 1024 * 1024, "Sampler trace exceeds 32MiB")
    for line in path.read_text().splitlines():
        row = json.loads(line)
        key = ("native_samples" if row["pid"] == native_pid else
               "supervisor_samples" if row["pid"] == supervisor_pid else "other_pid_samples")
        summary[key] += 1
        if key in ("native_samples", "supervisor_samples"):
            identities = summary["native_start_ticks" if key == "native_samples" else "supervisor_start_ticks"]
            if row["start_ticks"] not in identities:
                identities.append(row["start_ticks"])
        for source, target in (("capture_elapsed_ms", "max_capture_elapsed_ms"),
                               ("previous_sample_gap_ms", "max_sample_gap_ms")):
            if row.get(source) is not None:
                summary[target] = max(summary[target] or 0, row[source])
        if key != "native_samples":
            continue
        timestamp = row["timestamp_monotonic"]
        if summary["first_native_timestamp"] is None:
            summary["first_native_timestamp"] = timestamp
        summary["last_native_timestamp"] = timestamp
        for name, value in row.get("system_values", {}).items():
            if name.endswith("/gt_act_freq_mhz") and type(value) is int:
                counts = summary["native_clock_observations"]
                counts[name] = counts.get(name, 0) + 1
            if name in package_paths and type(value) is int:
                counts = summary["native_package_observations"]
                counts[name] = counts.get(name, 0) + 1
            if name == "/sys/firmware/acpi/platform_profile" and isinstance(value, str):
                summary["native_profile_observations"] += 1
    summary["required_observations_available"] = bool(summary["native_samples"]
        and len(summary["native_start_ticks"]) == 1 and len(summary["supervisor_start_ticks"]) <= 1
        and summary["native_clock_observations"] and summary["native_package_observations"]
        and summary["native_profile_observations"] and not summary["other_pid_samples"])
    summary["scope"] = ("Rows keep exact PID/start identity; supervisor rows precede native ownership. "
        "Capture intervals can overlap constructor/phase boundaries. No merged CPU attribution, continuous "
        "temperature proof, fixed clock assumption or thermal pass threshold.")
    return summary


def verify_leg(path, expected_leg, compare):
    _, data = compare.inspect_file(path, 2 * 1024 * 1024, time.monotonic() + 10, keep=True)
    value = compare.decode_json(data)
    require(value.get("status") == "passed" and value.get("leg") == expected_leg
            and value.get("completed_requests") == 4 and value.get("arm_pcm_proof_match") is True
            and value.get("phase_fields_valid") is True and value.get("pins_unchanged") is True
            and value.get("cleanup_complete") is True
            and value.get("observation_summary", {}).get("required_observations_available") is True,
            "Leg is incomplete or did not pass per-arm PCM/phase/pin/cleanup/observation checks")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--leg", choices=LEGS)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--source-pins", type=Path)
    parser.add_argument("--expected-pins", type=Path)
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_run", "native_started": False, "host_queried": False,
              "legs": LEGS, "arms": {k: {"path": str(v[0]), "sha256": v[1]} for k, v in ARMS.items()},
              "start_frame": 257985, "requests_per_leg": 4, "driver_seconds": DRIVER_SECONDS,
              "after_pins_seconds": AFTER_PINS_SECONDS, "request_seconds": REQUEST_SECONDS, "scope": SCOPE}, indent=2))
        return 0
    require(args.leg is not None and all(path is not None and path.is_absolute()
            for path in (args.output, args.source_pins, args.expected_pins)), "Explicit leg and absolute paths required")
    require(not args.output.exists(), "Use a fresh leg output")
    args.output.mkdir(mode=0o700)
    compare, graph, common, retained = helpers()
    graph.reject_diagnostic_environment()
    began, owner, sampler = time.monotonic(), {}, None
    constructing_native = False
    deadline, specs = began + DRIVER_SECONDS, {}
    report = {"schema": 1, "status": "checking", "leg": args.leg, "arm": args.leg[0], "scope": SCOPE,
              "supervisor_pid": os.getpid(), "runtimes": [], "errors": [], "pins_before": {}, "pins_after": {},
              "native_started": False, "completed_cases_per_runtime": {"diagnostic": 0}, "total_completed_requests": 0,
              "driver_seconds": DRIVER_SECONDS, "request_timeout_seconds": REQUEST_SECONDS,
              "after_pins_seconds": AFTER_PINS_SECONDS, "trace_interval_seconds": .5}
    def checkpoint():
        report["elapsed_seconds"] = time.monotonic() - began
        compare.checkpoint(args.output, report)
    def interrupted(signum, _frame):
        report["interruption_signal"] = signum
        if not constructing_native:
            raise KeyboardInterrupt
    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        _, data = compare.inspect_file(args.expected_pins, 64 * 1024, deadline, keep=True)
        expected = compare.decode_json(data)
        require(expected.get("status") == "ready" and retained.head() == expected.get("repository_head")
                and expected.get("arms") == {k: v[1] for k, v in ARMS.items()}
                and expected.get("pins", {}).get("model") == compare.MODEL_SHA
                and expected["pins"].get("sampler_source") == SAMPLER_SHA, "Exact expected configuration differs/unarmed")
        require(expected.get("closed_quality_proof_sha256") == CLOSED_SHA
                and expected.get("flash_build_sha256") == BUILD_SHA, "Exact Flash proof configuration differs")
        config_pin, sources = compare.source_specs(args.source_pins, deadline)
        require(config_pin["sha256"] == expected.get("source_pins_sha256"), "Source-pin configuration differs")
        specs = {"runtime_A": (ARMS["A"][0], ARMS["A"][1], 256 * 1024 * 1024),
                 "runtime_B": (ARMS["B"][0], ARMS["B"][1], 256 * 1024 * 1024),
                 "model": (compare.MODEL, compare.MODEL_SHA, 2 * 1024 * 1024 * 1024),
                 "proof": (PROOF, PROOF_SHA, 2 * 1024 * 1024),
                 "closed_quality": (CLOSED, CLOSED_SHA, 8 * 1024 * 1024),
                 "flash_build": (BUILD, BUILD_SHA, 2 * 1024 * 1024),
                 "expected_pins": (args.expected_pins, hashlib.sha256(data).hexdigest(), 64 * 1024),
                 "source_pins": (args.source_pins, config_pin["sha256"], 64 * 1024)}
        for path, digest in sources.items():
            specs["source:" + path] = (Path(path), digest, 32 * 1024 * 1024)
        for name, (path, digest) in HELPERS.items():
            specs["helper:" + name] = (path, digest, 128 * 1024)
        for label, digest in compare.TRACKS:
            specs["asset:" + label] = (compare.ASSETS / f"{label}-30s-23p4s.wav", digest, compare.ASSET_BYTES)
        report["pins_before"] = compare.capture_pins(specs, deadline)
        require(sources.get(str(ROOT / "tools/benchmarks/worker_support.py")) == SAMPLER_SHA,
                "Exact sampler source must be pinned")
        report["phase_baseline_proof"] = retained.comparison_proof(PROOF,
            {"pins": {"runtime": ARMS["A"][1], "model": compare.MODEL_SHA}})
        _, proof_bytes = compare.inspect_file(PROOF, 2 * 1024 * 1024, deadline, expected=PROOF_SHA, keep=True)
        proof = compare.decode_json(proof_bytes)
        np, NativeRuntime, prepared = compare.prepare_cases(compare.load_track_plan(deadline), deadline)
        cases = [case for case in prepared if case["start_frame"] == 257985]
        del prepared
        expected_rows, report["arm_pcm_proof"] = arm_rows(compare, deadline, args.leg[0], proof, sources)
        require([case["track"] for case in cases] == [name for name, _ in compare.TRACKS], "Fixed four-track order differs")
        for case in cases:
            row = expected_rows[case["id"]]
            require(all(case[key] == row[key] for key in ("input_sha256", "normalization", "shape", "dtype", "valid_frames")),
                    "Normalized input differs from immutable proof")
        hashes = {case["id"]: expected_rows[case["id"]]["output_sha256"] for case in cases}
        from worker_support import ProcessSampler
        from tools.benchmarks.stem_worker import conditions
        report["conditions_before_sampler"] = conditions()
        package_paths = [name + "/temp" for name, value in report["conditions_before_sampler"].items()
                         if isinstance(value, dict) and value.get("type") == "x86_pkg_temp"]
        def pid_provider():
            process = getattr(owner.get("native"), "process", None)
            return process.pid if process is not None else os.getpid()
        sampler = ProcessSampler(pid_provider, .5, trace_path=args.output / "native-trace.jsonl", include_system=True)
        sampler.__enter__()
        report["sampler_started_monotonic"] = time.monotonic()
        class ObservedNative(NativeRuntime):
            def __new__(cls, *_args, **_kwargs):
                nonlocal constructing_native
                constructing_native = True
                instance = object.__new__(cls)
                owner["native"] = instance
                report["constructor_begin_monotonic"] = time.monotonic()
                return instance
            def __init__(self, *a, **kw):
                nonlocal constructing_native
                try:
                    super().__init__(*a, **kw)
                finally:
                    report["constructor_end_monotonic"] = time.monotonic()
                    constructing_native = False
                if "interruption_signal" in report:
                    try:
                        self.close()
                    except Exception as error:
                        report["errors"].append({"phase": "deferred_construction_close", "message": str(error)})
                    raise KeyboardInterrupt
            def start(self):
                report["ready_receive_begin_monotonic"] = time.monotonic()
                try:
                    return super().start()
                finally:
                    report["ready_receive_end_monotonic"] = time.monotonic()
            def close(self):
                try:
                    report["native_memory_before_close"] = graph.native_memory(self.process)
                    if "sampler" not in report:
                        report["sampler"] = sampler.stop()
                finally:
                    super().close()
            def infer(self, samples, request_id):
                interval = {"request_id": request_id, "started_monotonic": time.monotonic(), "returned": False}
                report.setdefault("request_intervals", []).append(interval)
                try:
                    result = super().infer(samples, request_id)
                    interval["returned"] = True
                    return result
                finally:
                    interval["finished_monotonic"] = time.monotonic()
        report["execution_provenance"] = execution_provenance()
        report["cross_arm_exact_pcm"] = False
        checkpoint()
        compare.run_runtime("diagnostic", ARMS[args.leg[0]][0], cases, hashes, np, ObservedNative,
            SimpleNamespace(request_timeout=REQUEST_SECONDS), args.output, report, deadline)
        if args.leg[0] == "B":
            report["flash_evidence"] = flash_observation(compare, report["runtimes"][0], deadline)
        report["startup_total_observed_ms"] = (report["ready_receive_end_monotonic"] - report["constructor_begin_monotonic"]) * 1000
        report["observation_summary"] = observation_summary(args.output / "native-trace.jsonl", os.getpid(),
            owner["native"].process.pid, package_paths)
        trace = report["sampler"]["trace"]
        summary = report["observation_summary"]
        parsed_rows = summary["supervisor_samples"] + summary["native_samples"] + summary["other_pid_samples"]
        report["trace_integrity"] = {"parsed_rows": parsed_rows, "samples_written": trace["samples_written"],
            "sampler_samples": report["sampler"]["samples"], "trace_errors": trace["errors"],
            "sample_errors": report["sampler"]["sample_errors"]}
        require(not trace["errors"] and not report["sampler"]["sample_errors"]
                and parsed_rows == trace["samples_written"] == report["sampler"]["samples"],
                "Sampler trace is incomplete; raw errors retained")
        require(report["observation_summary"]["required_observations_available"], "Required clock/package/profile observations unavailable")
        rows = report["runtimes"][0]["requests"]
        report.update(status="passed", completed_requests=len(rows), arm_pcm_proof_match=all(row["output_matches_arm_proof"] for row in rows),
                      phase_fields_valid=all(row["phase_fields_valid"] for row in rows))
        require(len(rows) == 4 and report["arm_pcm_proof_match"] and report["phase_fields_valid"], "Leg requests incomplete")
    except KeyboardInterrupt:
        report["status"] = "interrupted"
    except Exception as error:
        report["status"] = "incomplete"
        report["errors"].append({"type": type(error).__name__, "message": str(error)[:8192]})
    finally:
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        if sampler is not None and "sampler" not in report:
            try:
                report["sampler"] = sampler.stop()
            except Exception as error:
                report["errors"].append({"phase": "sampler_stop", "message": str(error)})
        native = owner.get("native")
        if native is not None and hasattr(native, "_cleanup"):
            try:
                NativeRuntime.close(native)  # Idempotent; covers partial factory ownership.
            except Exception as error:
                report["errors"].append({"phase": "native_close", "message": str(error)})
        process = getattr(native, "process", None)
        report["native_started"] = process is not None
        report["cleanup_complete"] = bool(process is not None and process.poll() is not None
            and not native.root.exists() and sampler is not None and "sampler" in report)
        if specs:
            try:
                report["pins_after"] = compare.capture_pins(specs, time.monotonic() + AFTER_PINS_SECONDS, verify=False)
                report["pins_unchanged"] = report["pins_before"] == report["pins_after"] and bool(report["pins_before"])
                report["head_after"] = retained.head()
                require(report["head_after"] == expected["repository_head"], "HEAD changed during leg")
            except Exception as error:
                report["errors"].append({"phase": "after_pins", "message": str(error)})
        if report["status"] == "passed" and (not report["pins_unchanged"] or not report["cleanup_complete"] or report["errors"]):
            report["status"] = "incomplete"
        checkpoint()
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    print(json.dumps({"leg": args.leg, "status": report["status"], "result": str(args.output / "result.json")}))
    return 0 if report["status"] == "passed" else 130 if report["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())

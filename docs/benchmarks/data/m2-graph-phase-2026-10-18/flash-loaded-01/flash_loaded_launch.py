#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Dry by default:600s Flash delivery under software UI and independent silent audio."""
import argparse
import ast
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
FROZEN = BASE / "observed_launch.py"
FROZEN_SHA = "511bb712d2f7af01abb1ab25ca92b001c6f82af4ee6f42547a5b43fa32b237d6"
COST = BASE / "flash_cost_runtime.py"
COST_SHA = "ca0ad3468d20c2ae28d06476b995f50d49823c64ed6c57106aea059bf65baf71"
NESTED = {
    "window_guard_source": (BASE / "flash_loaded_window_guard.py",
        "cd064312e996672fed81e570f2acc0c8efd40e0fa76520cb0b3566da560f18b1",
        BASE.parents[1] / "tools/benchmarks/window_guard.py",
        "077bd16eefbdd9b80f787b6aeb2837a3790fedf7f5defd7b140dccc6e1f5cebb"),
    "observer_source": (BASE / "flash_loaded_observer.py",
        "a21b21408ecb1a1108d310eee5ebcbad0187c4894fa308b55c495be4f16a9664",
        BASE.parents[1] / "benchmarks-local/software-ui-2026-10-17/observed-run.py",
        "a6a4b81014f6aec16c8cd68e902348afcd10a1fdf5966f6b2a54ba2f622ef394"),
}
SCOPE = ("600-second loaded diagnostic only: unchanged playback-paced stem soak, active cancellation/seek, "
         "two-ready-hop bound, software Harness UI33ms updates and independent silent48kHz/stereo/256-frame "
         "audio probe. Candidate is admitted against its own closed917104 PCM and full-F32 build/source "
         "lineage, not cross-arm bit identity. Current soak validates within-run repeat/seek parity; it "
         "does not retain raw native PCM hashes or fresh Flash shader logs. CPU-wall phases are not GPU "
         "timestamps. Continuous-hop p99 must remain strictly below4095ms; diagnostic completion alone "
         "does not meet that gate. No actual stem playback, physical latency, musical verdict, full60-minute "
         "qualification, new thermal gate or power/guard/device-policy change.")
_context = {}


def require(value, message):
    if not value:
        raise ValueError(message)


def cost():
    contents = COST.read_bytes()
    require(len(contents) <= 128 * 1024 and hashlib.sha256(contents).hexdigest() == COST_SHA,
            "Frozen per-arm proof adapter differs")
    spec = importlib.util.spec_from_file_location("conflux_loaded_flash_proof", COST)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def source():
    c = cost()
    text = c.frozen(FROZEN, FROZEN_SHA)
    one = c.replace_once
    text = one(text, '"""Dry-run by default: guarded 600-second graph-phase diagnostic, no policy changes."""',
               '"""Adapted Flash loaded diagnostic, unchanged soak and independent UI/silent-audio load."""')
    text = one(text, 'RUNTIME = Path("/tmp/conflux-stem-phase-timing-2026-10-18/build/conflux-stem-runtime")',
               'RUNTIME = Path(' + repr(str(c.FLASH)) + ')')
    text = one(text, '"runtime": (RUNTIME, None)', '"runtime": (RUNTIME, ' + repr(c.FLASH_SHA) + ')')
    text = one(text, '"soak_source": (ROOT / "tools/benchmarks/stem_soak.py", None)',
        '"soak_source": (ROOT / "tools/benchmarks/stem_soak.py", "8e3f491920008be0535c3c233bed1f07f9ff7e96452f3942aaa6e17b4f1cd22d")')
    text = one(text, '"worker_source": (ROOT / "tools/stems/worker.py", None)',
        '"worker_source": (ROOT / "tools/stems/worker.py", "5327d8d9c8ff531059811077669eff1b4c0f4cb76cd00b94ed84708f9407206b")')
    for name, expression in (("window_guard_source", 'ROOT / "tools/benchmarks/window_guard.py"'),
                             ("observer_source", 'OLD / "observed-run.py"')):
        file, digest, original, original_digest = NESTED[name]
        c.frozen(original, original_digest)
        c.frozen(file, digest)
        text = one(text, repr(name).replace("'", '"') + ': (' + expression + ', "' + original_digest + '")',
                   repr(name).replace("'", '"') + ': (BASE / "' + file.name + '", "' + digest + '")')
    text = one(text, '            and value.get("lid_open") is True and value.get("visible_window") is True',
        '            and value.get("lid_open") is True and value.get("visible_window") is True\n'
        '            and value.get("codex_minimized") is True')
    text = one(text, 'A fresh human reply must explicitly cover this run, AC, open lid and visible window',
               'A fresh human reply must cover this run, AC, open lid, visible Harness and Codex minimized')
    node = next(node for node in ast.parse(text).body
                if isinstance(node, ast.FunctionDef) and node.name == "comparison_proof")
    old = "\n".join(text.splitlines()[node.lineno - 1:node.end_lineno])
    text = one(text, old, 'def comparison_proof(path, expected):\n    return own_pcm_proof(path, expected)')
    text = one(text, 'default=BASE / "human-readiness.json"', 'default=BASE / "flash-loaded-human-readiness.json"')
    text = one(text, 'default=BASE / "comparison/result.json"', 'default=BASE / "flash-quality-laptop-01/comparison/result.json"')
    text = one(text, '    args = parser.parse_args()',
        '    parser.add_argument("--source-pins", type=Path, default=BASE / "flash-loaded-source-pins.json")\n'
        '    args = parser.parse_args()')
    text = one(text, 'config_path = BASE / "expected-pins.json"', 'config_path = BASE / "flash-loaded-expected-pins.json"')
    text = one(text, '    expected = json.loads(config_path.read_text())',
               '    expected = json.loads(config_path.read_text()) if config_path.is_file() else {"status": "unarmed"}')
    text = one(text, '    initial_human = human_readiness(args.readiness_decision, args.name)',
        '    began = time.monotonic()\n'
        '    deadline = began + args.seconds + 750\n'
        '    initial_human = human_readiness(args.readiness_decision, args.name)\n'
        '    prepare_proof_sources(args.source_pins, expected, deadline)')
    text = one(text, '    admission.mkdir()\n    began = time.monotonic()\n    deadline = began + args.seconds + 750',
               '    admission.mkdir()')
    text = one(text,
        '"diagnostic_ui_logging": False, "scope": "Playback-paced repeated prepared music excerpts with independent silent audio and visible software-UI load. Graph phases are existing CPU wall intervals, not GPU timestamps. New normal UI/probe producer binaries are explicitly pinned. No stem playback, physical-audio measurement, hardware GPU preemption, latency qualification or invented thermal pass gate."}',
        '"diagnostic_ui_logging": False, "scope": SCOPE, "cross_arm_exact_pcm": False,\n'
        '             "execution_provenance": provenance(), "proof_source_pins_before": _context["pins_before"]}')
    text = one(text, '    signatures, owned = {}, {}',
        '    signatures, owned, known_groups = {}, {}, {}\n    constructing_child = False')
    text = one(text, '        if child is not None and child.poll() is None:\n            try:\n                os.killpg(child.pid, signum)',
        '        if child is not None and child.pid in known_groups:\n            try:\n                os.killpg(child.pid, signum)')
    text = one(text,
        '    def stop(signum, _frame):\n'
        '        state["interruption_signal"] = signum\n'
        '        signal_child(signal.SIGINT)\n'
        '        raise KeyboardInterrupt',
        '    def finish_group(group):\n'
        '        require(group in known_groups and group > 1 and group != os.getpgrp(), "Unsafe or unknown owned group")\n'
        '        row = {"group": group, **known_groups[group], "started_monotonic": time.monotonic(), "disappearance_observed": False}\n'
        '        state.setdefault("owned_group_cleanup", []).append(row)\n'
        '        try:\n'
        '            os.killpg(group, signal.SIGKILL)\n'
        '            row["kill_sent"] = True\n'
        '        except ProcessLookupError:\n'
        '            row["kill_sent"] = False\n'
        '        until = time.monotonic() + 2\n'
        '        while True:\n'
        '            if child is not None and child.pid == group:\n'
        '                child.poll()\n'
        '            try:\n'
        '                os.killpg(group, 0)\n'
        '            except ProcessLookupError:\n'
        '                row.update(disappearance_observed=True, finished_monotonic=time.monotonic())\n'
        '                del known_groups[group]\n'
        '                return\n'
        '            require(time.monotonic() < until, "Owned group remains after bounded cleanup")\n'
        '            time.sleep(.05)\n\n'
        '    def stop(signum, _frame):\n'
        '        state["interruption_signal"] = signum\n'
        '        if not constructing_child:\n'
        '            signal_child(signal.SIGINT)\n'
        '            raise KeyboardInterrupt')
    for assignment in (
        'child = subprocess.Popen(readiness_command, cwd=ROOT, stdout=out, stderr=err, start_new_session=True)',
        'child = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err, env=environment, start_new_session=True)'):
        workload_publication = (
            '                state.update(child_started=True, owned_guard_process_group=child.pid, child_spawned_monotonic=time.monotonic())\n'
            if assignment.startswith('child = subprocess.Popen(command,') else '')
        text = one(text, '            ' + assignment,
            '            constructing_child = True\n'
            '            try:\n'
            '                ' + assignment + '\n'
            '                known_groups[child.pid] = {"phase": active_phase, "created_monotonic": time.monotonic()}\n'
            + workload_publication +
            '                remember_owned()\n'
            '            finally:\n'
            '                constructing_child = False\n'
            '            if "interruption_signal" in state:\n'
            '                signal_child(signal.SIGINT)\n'
            '                raise KeyboardInterrupt')
    text = one(text, '            save(status="running", child_started=True, owned_guard_process_group=child.pid, child_spawned_monotonic=time.monotonic())',
                    '            save(status="running")')
    text = one(text, '        require(code == 0, "Bounded post-setup normal readiness did not match")\n        child = None',
        '        require(code == 0, "Bounded post-setup normal readiness did not match")\n'
        '        finish_group(child.pid)\n        child = None')
    text = one(text, '        require(code == 0, "Guarded workload returned failure")',
        '        require(code == 0, "Guarded workload returned failure")\n        finish_group(child.pid)')
    text = one(text, '            remember_owned()\n            if child is not None and child.poll() is None:',
        '            remember_owned()\n            signal_child(signal.SIGINT)\n'
        '            if child is not None and child.poll() is None:')
    text = one(text,
        '            groups = {value["process_group"] for value in live.values()}\n'
        '            for group in groups:\n'
        '                require(group > 1 and group != os.getpgrp(), "Refusing unsafe cleanup process group")\n'
        '                try:\n'
        '                    os.killpg(group, signal.SIGKILL)\n'
        '                except ProcessLookupError:\n'
        '                    pass',
        '            groups = set(known_groups) | {value["process_group"] for value in live.values()}\n'
        '            for group in groups:\n'
        '                known_groups.setdefault(group, {"phase": "observed_owned_descendant"})\n'
        '                finish_group(group)')
    text = one(text,
        '            state["cleanup"] = {"owned_process_identities": owned, "remaining_owned_processes": live_owned(), "last_child_reaped": child is None or child.poll() is not None}\n'
        '            require(not state["cleanup"]["remaining_owned_processes"] and state["cleanup"]["last_child_reaped"], "Owned descendants remain after cleanup")',
        '            state["cleanup"] = {"owned_process_identities": owned, "remaining_known_groups": dict(known_groups), "remaining_owned_processes": live_owned(), "last_child_reaped": child is None or child.poll() is not None}\n'
        '            require(not known_groups and not state["cleanup"]["remaining_owned_processes"] and state["cleanup"]["last_child_reaped"], "Owned groups or descendants remain after cleanup")')
    text = one(text, '        save(status="completed")',
               '        state["latency_gate"] = latency_gate(result)\n        save(status="completed")')
    text = one(text, '        with monitor_path.open("a") as stream:\n            stream.write(json.dumps({"event": "finished",',
        '        try:\n'
        '            state["proof_source_pins_after"] = final_proof_pins(deadline)\n'
        '        except Exception as error:\n'
        '            state["status"] = "error"\n'
        '            state["failures"].append({"phase": "proof_after_pins", "error": str(error)})\n'
        '        with monitor_path.open("a") as stream:\n            stream.write(json.dumps({"event": "finished",')
    return text


def prepare_proof_sources(path, expected, deadline):
    c = cost()
    c.quality().reject_environment()  # Presence-tested flags, including empty values, stay absent.
    compare = c.helpers()[0]
    compare.REQUIRED_SOURCES = tuple(dict.fromkeys(compare.REQUIRED_SOURCES + (
        Path(__file__).resolve(), FROZEN, *(file for file, _, _, _ in NESTED.values()),
        *(original for _, _, original, _ in NESTED.values()))))
    pin, sources = compare.source_specs(path, deadline)
    require(pin["sha256"] == expected.get("source_pins_sha256")
            and expected.get("closed_quality_proof_sha256") == c.CLOSED_SHA
            and expected.get("flash_build_sha256") == c.BUILD_SHA,
            "Exact loaded source/proof configuration differs or is unarmed")
    specs = {"source_configuration": (path, pin["sha256"], 64 * 1024),
             "closed_quality": (c.CLOSED, c.CLOSED_SHA, 8 * 1024 * 1024),
             "immutable_baseline": (c._driver["PROOF"], c._driver["PROOF_SHA"], 2 * 1024 * 1024),
             "flash_build": (c.BUILD, c.BUILD_SHA, 2 * 1024 * 1024)}
    for file, digest in sources.items():
        specs["source:" + file] = (Path(file), digest, 32 * 1024 * 1024)
    before = compare.capture_pins(specs, deadline)
    receipt = c.session_proof(compare, sources, deadline)
    _context.update(cost=c, compare=compare, specs=specs, pins_before=before, proof=receipt)


def own_pcm_proof(path, expected):
    c = _context["cost"]
    require(path == c.CLOSED and expected["pins"]["runtime"] == c.FLASH_SHA
            and expected["pins"]["model"] == _context["compare"].MODEL_SHA,
            "Loaded run must use the exact closed Flash producer proof")
    return {**_context["proof"], "selected_arm": "B", "selected_reference_label": "flash",
            "candidate_sha256": c.FLASH_SHA, "threads": 2,
            "scope": "Closed exact own-arm PCM/build/source admission. Current soak performs its existing within-run parity checks; no cross-arm identity or per-request frozen-PCM assertion."}


def latency_gate(result):
    continuous = result["timing"]["continuous"]
    p99 = continuous["hop_service_percentiles_ms"].get("99")
    producer_target = result["timing"]["headroom_target_ms"]
    require(type(p99) in (int, float) and math.isfinite(p99) and p99 >= 0 and continuous["chunks"] > 0
            and math.isclose(producer_target, 4095.0, rel_tol=0, abs_tol=1e-9),
            "Continuous-hop p99/unchanged producer target unavailable")
    producer_pass = result["acceptance"]["observed_continuous_p99_within_target"]
    require(type(producer_pass) is bool and producer_pass is continuous["observed_p99_within_target"]
            and producer_pass == (p99 < producer_target), "Producer strict p99 assertion differs")
    ready = result["runtime_ready"]
    require(len(ready) == 1, "Loaded native runtime restarted")
    native = ready[0]["runtime"]
    require(native.get("backend") == "vulkan" and native.get("threads") == 2
            and native.get("fence_wait_mode") == "blocking" and native.get("weight_storage") == "native"
            and native.get("model") == str(_context["compare"].MODEL)
            and ready[0].get("model_id") == _context["compare"].MODEL_SHA
            and native.get("process_environment") == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"},
            "Actual loaded native policy/model differs")
    return {"target_ms": 4095.0, "operator": "strict_less_than", "observed_continuous_p99_ms": p99,
            "producer_target_ms": producer_target, "passed": producer_pass and p99 < 4095.0,
            "percentile_method": result["timing"]["percentile_method"],
            "continuous_chunks": continuous["chunks"], "producer_hops_above_target": continuous["hops_above_target"],
            "full_60minute_qualification": result["acceptance"]["full_audio_UI_soak_qualified"],
            "scope": "Diagnostic completion is independent of target success. Producer's existing floating-point target formula and strict comparison are retained; no threshold relaxation or full60-minute claim."}


def final_proof_pins(deadline):
    actual = _context["compare"].capture_pins(_context["specs"], deadline, verify=False)
    require(actual == _context["pins_before"], "Source/build/proof pins changed during loaded diagnostic")
    return actual


def provenance():
    return {"adapter": str(Path(__file__).resolve()),
            "adapter_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "frozen_observed_launcher_sha256": FROZEN_SHA, "frozen_proof_adapter_sha256": COST_SHA,
            "rendered_launcher_sha256": hashlib.sha256(source().encode()).hexdigest(),
            "nested_helpers": {name: {"selected_path": str(file), "selected_sha256": digest,
                "original_path": str(original), "original_sha256": original_digest}
                for name, (file, digest, original, original_digest) in NESTED.items()},
            "scope": "Narrow launcher adaptation plus scoped nested-helper copies fixing handled-signal publication and known-group cleanup. Frozen originals, soak, worker, Harness and probe remain unchanged. No arbitrary SIGKILL immunity claim."}


def main():
    text = source()
    if "--render-only" in sys.argv:
        parser = argparse.ArgumentParser(description="Render loaded launcher offline; no host/native operations")
        parser.add_argument("--render-only", type=Path, required=True)
        args = parser.parse_args()
        args.render_only.mkdir(mode=0o700)
        with (args.render_only / "observed-launch.py").open("x") as stream:
            stream.write(text)
        for name, (file, digest, _, _) in NESTED.items():
            with (args.render_only / (name + ".py")).open("x") as stream:
                stream.write(cost().frozen(file, digest))
        with (args.render_only / "provenance.json").open("x") as stream:
            json.dump(provenance(), stream, indent=2)
            stream.write("\n")
        print(json.dumps(provenance(), indent=2))
        return 0
    namespace = {"__file__": str(Path(__file__).resolve()), "__name__": "conflux_flash_loaded_adapted",
                 "prepare_proof_sources": prepare_proof_sources, "own_pcm_proof": own_pcm_proof,
                 "latency_gate": latency_gate, "final_proof_pins": final_proof_pins,
                 "provenance": provenance, "_context": _context, "SCOPE": SCOPE}
    exec(compile(text, str(FROZEN) + "[flash-loaded-adapted]", "exec"), namespace)
    return namespace["main"]()


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Dry by default: promoted Flash3600s delivery with the existing UI/silent-audio load."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
FROZEN = BASE / "flash_loaded_launch.py"
FROZEN_SHA = "883d537f8f2fe91dc2e5fc5a735ca20067a007cbe8e308e9f1673825bf39e684"
PROMOTED = BASE / "flash-promoted-native-build/conflux-stem-runtime"
BUILD = BASE / "flash-promoted-built.json"
REPRODUCTION = BASE / "flash-promoted-repro-01/result.json"
EXPECTED = BASE / "flash-60minute-expected-pins.json"
SOURCE_PINS = BASE / "flash-60minute-source-pins.json"
MODULE_SHA = "55ebc619f561ac1042489cdc02c1fa14db6c8e0b524e19f85080783abb865600"
SCOPE = ("3600-second loaded diagnostic: unchanged playback-paced separation, active cancellation/seek every300s, "
    "two-ready-hop queue, software Harness UI33ms and independent silent48kHz/stereo/256-frame audio. "
    "Total fixed wall budget4350s includes setup/readiness/cleanup. The original fresh human record is checked "
    "before setup and workload admission with the unchanged900s maximum age; no renewal is required during "
    "the workload. AC/lid/UI observations continue. Promoted binary must reproduce its own closed917104 "
    "Flash PCM under its exact signed source/build proof. Strict continuous-hop p99<4095ms remains unchanged. "
    "This measures sustained separation under independent loads, not actual stem playback, physical audio "
    "latency or the complete M2 engine/fallback qualification. No new thermal gate or host policy changes.")
_context = {}
_loaded = None


def require(value, message):
    if not value:
        raise ValueError(message)


def loaded():
    global _loaded
    if _loaded is None:
        data = FROZEN.read_bytes()
        require(len(data) <= 128 * 1024 and hashlib.sha256(data).hexdigest() == FROZEN_SHA,
                "Frozen600s loaded adapter differs")
        spec = importlib.util.spec_from_file_location("conflux_60minute_loaded_base", FROZEN)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module._context = _context
        _loaded = module
    return _loaded


def source():
    parent = loaded()
    c = parent.cost()
    one, text = c.replace_once, parent.source()
    text = one(text, 'RUNTIME = Path(' + repr(str(c.FLASH)) + ')', 'RUNTIME = Path(' + repr(str(PROMOTED)) + ')')
    # The new binary hash is mandatory in the armed configuration/proof, never
    # borrowed from the historical6f311 binary's closed numerical evidence.
    text = one(text, '"runtime": (RUNTIME, ' + repr(c.FLASH_SHA) + ')', '"runtime": (RUNTIME, None)')
    text = one(text,
        '"runtime_cmake_source": (ROOT / "tools/runtime/CMakeLists.txt", "1537fb26525b7e8fc715d83718427918ff81df0b11f2de054a02a46bcf416b45")',
        '"runtime_cmake_source": (ROOT / "tools/runtime/CMakeLists.txt", "d5fec7cbe05fa85784a1f1d68271e06294aac1de0f8e454947dea1d9a3adcf83")')
    text = one(text, 'default=600)', 'default=3600)')
    text = one(text, '(args.seconds, args.control_interval) != (600, 300)',
                    '(args.seconds, args.control_interval) != (3600, 300)')
    text = one(text, 'exactly 600s/300s diagnostic', 'exactly3600s/300s loaded diagnostic')
    text = one(text, '"--seconds", "600",', '"--seconds", "3600",')
    text = one(text, '"expected_controls": 1,', '"expected_controls": 11,')
    text = one(text, '            and value.get("codex_minimized") is True',
        '            and value.get("codex_minimized") is True\n'
        '            and type(value.get("workload_seconds")) is int and value.get("workload_seconds") == 3600')
    text = one(text, 'A fresh human reply must cover this run, AC, open lid, visible Harness and Codex minimized',
        'A fresh human reply must cover this exact3600s run, AC, open lid, visible Harness and Codex minimized')
    text = one(text, 'default=BASE / "flash-loaded-human-readiness.json"',
                    'default=BASE / "flash-60minute-human-readiness.json"')
    text = one(text, 'default=BASE / "flash-quality-laptop-01/comparison/result.json"',
                    'default=BASE / "flash-promoted-repro-01/result.json"')
    text = one(text, 'default=BASE / "flash-loaded-source-pins.json"',
                    'default=BASE / "flash-60minute-source-pins.json"')
    text = one(text, 'config_path = BASE / "flash-loaded-expected-pins.json"',
                    'config_path = BASE / "flash-60minute-expected-pins.json"')
    text = one(text, 'Adapted Flash loaded diagnostic, unchanged soak and independent UI/silent-audio load.',
                    'Promoted Flash3600s loaded diagnostic, unchanged soak/UI/silent-audio semantics.')
    return text


def proof_configuration(expected):
    c = loaded().cost()
    require(expected.get("status") == "ready" and expected.get("runtime_path") == str(PROMOTED)
        and expected.get("closed_quality_proof_sha256") == c.CLOSED_SHA
        and expected.get("historical_flash_build_sha256") == c.BUILD_SHA
        and expected.get("tracked_module_sha256") == MODULE_SHA,
        "3600s source/build/proof configuration is unarmed or differs")
    for key in ("promoted_build_sha256", "reproduction_proof_sha256", "source_pins_sha256"):
        require(isinstance(expected.get(key), str) and c.helpers()[0].HEX.fullmatch(expected[key]),
                "Pending promoted proof pin: " + key)


def proof_helpers():
    parent = loaded()
    c = parent.cost()
    compare = c.helpers()[0]
    compare.DIAGNOSTIC = PROMOTED
    compare.REQUIRED_SOURCES = tuple(dict.fromkeys(compare.REQUIRED_SOURCES + (
        Path(__file__).resolve(), BASE / "prepare_flash_60minute.py", FROZEN, parent.FROZEN,
        BASE / "promoted_flash_repro.py", BASE / "verify_promoted_flash_build.py",
        BASE.parents[1] / "tools/runtime/FlashAttentionF32.cmake",
        *(file for file, _, _, _ in parent.NESTED.values()),
        *(original for _, _, original, _ in parent.NESTED.values()))))
    return c, compare


def prepare_proof_sources(path, expected, deadline):
    c, compare = proof_helpers()
    c.quality().reject_environment()
    proof_configuration(expected)
    pin, sources = compare.source_specs(path, deadline)
    require(pin["sha256"] == expected["source_pins_sha256"], "3600s source configuration differs")
    specs = {"source_configuration": (path, pin["sha256"], 64 * 1024),
        "closed_quality": (c.CLOSED, c.CLOSED_SHA, 8 * 1024 * 1024),
        "immutable_baseline": (c._driver["PROOF"], c._driver["PROOF_SHA"], 2 * 1024 * 1024),
        "historical_flash_build": (c.BUILD, c.BUILD_SHA, 2 * 1024 * 1024),
        "promoted_build": (BUILD, expected["promoted_build_sha256"], 4 * 1024 * 1024),
        "promoted_reproduction": (REPRODUCTION, expected["reproduction_proof_sha256"], 8 * 1024 * 1024)}
    for file, digest in sources.items():
        specs["source:" + file] = (Path(file), digest, 32 * 1024 * 1024)
    _, raw = compare.inspect_file(BUILD, 4 * 1024 * 1024, deadline,
                                  expected=expected["promoted_build_sha256"], keep=True)
    build = compare.decode_json(raw)
    require(build.get("status") == "built_metadata_verified_native_execution_pending"
        and build.get("before_after_inputs_unchanged") is True and build.get("native_execution") is False
        and build.get("verifier", {}).get("sha256") == sources[str(BASE / "verify_promoted_flash_build.py")]
        and build["runtime"]["sha256"] == expected["pins"]["runtime"]
        and Path(build["runtime"]["path"]).resolve() == PROMOTED.resolve(), "Promoted build receipt differs")
    inspection = build["inspection"]
    require(inspection["model"]["sha256"] == compare.MODEL_SHA
        and inspection["worktree_inputs"]["tools/runtime/FlashAttentionF32.cmake"]["sha256"] == MODULE_SHA
        and inspection["configured_options"]["CONFLUX_STEM_FLASH_F32"] == "ON"
        and inspection["configured_options"]["CONFLUX_STEM_BLOCKING_WAIT"] == "ON"
        and inspection["configured_options"]["CMAKE_BUILD_TYPE"] == "Release"
        and inspection["full_f32_scalar_compilation_source"] is True
        and inspection["graph_order_overlay"] is False, "Promoted Flash source/configuration differs")
    # Bind actual selected source/configuration/tool bytes before and after the
    # long run, reusing the completed build's explicit bounded metadata groups.
    for group in ("worktree_inputs", "upstream_pinned_and_affected_sources", "generated_inputs",
                  "baseline_generated_inputs", "config_inputs", "tools_and_header"):
        for label, record in inspection[group].items():
            specs["promoted:" + group + ":" + label] = (Path(record["path"]), record["sha256"], 256 * 1024 * 1024)
    before = compare.capture_pins(specs, deadline)
    historical = c.session_proof(compare, sources, deadline)
    _, raw = compare.inspect_file(REPRODUCTION, 8 * 1024 * 1024, deadline,
        expected=expected["reproduction_proof_sha256"], keep=True)
    proof = compare.decode_json(raw)
    require(proof.get("schema") == 1 and proof.get("status") == "reproduction_complete"
        and proof.get("completed_requests") == 16
        and not proof.get("errors")
        and all(proof.get(key) is True for key in ("all_outputs_match_closed_B", "input_sha_match",
                    "phase_fields_valid", "pins_unchanged", "cleanup_complete"))
        and proof.get("runtime_sha256") == expected["pins"]["runtime"]
        and proof.get("model_sha256") == compare.MODEL_SHA
        and proof.get("closed_B_proof_sha256") == c.CLOSED_SHA
        and proof.get("build_receipt_sha256") == expected["promoted_build_sha256"]
        and proof.get("tracked_module_sha256") == MODULE_SHA
        and proof.get("source_head") == expected["repository_head"]
        and proof.get("flash_evidence"), "Promoted numerical/source proof is incomplete or differs")
    require(proof.get("pins_before") == proof.get("pins_after"), "Promoted reproduction pins changed")
    for label, file in (("driver", BASE / "promoted_flash_repro.py"),
                        ("build_verifier_source", BASE / "verify_promoted_flash_build.py")):
        require(all(proof[side][label]["sha256"] == sources[str(file)]
            and Path(proof[side][label]["path"]).resolve() == file.resolve()
            for side in ("pins_before", "pins_after")), "Promoted proof generator source differs: " + label)
    _, raw = compare.inspect_file(c.CLOSED, 8 * 1024 * 1024, deadline, expected=c.CLOSED_SHA, keep=True)
    closed = compare.decode_json(raw)
    reference = closed["runtimes"][1]["requests"]
    runtimes = proof.get("runtimes", [])
    require(len(runtimes) == 1 and len(runtimes[0].get("requests", [])) == 16,
            "Promoted proof must retain exactly one complete sixteen-case child")
    rows = runtimes[0]["requests"]
    require([row["id"] for row in rows] == [row["id"] for row in reference], "Promoted case order differs")
    for row, original in zip(rows, reference, strict=True):
        require(row.get("completed") is True and row.get("finite") is True
            and row.get("output_matches_closed_B") is True
            and all(row[key] == original[key] for key in ("input_sha256", "normalization", "shape", "dtype",
                "valid_frames", "output_shape", "output_dtype", "output_bytes", "output_sha256")),
            "Promoted per-case input/own-B output proof differs")
    _context.update(cost=c, compare=compare, specs=specs, pins_before=before,
                    historical_proof=historical, promoted_proof=proof)


def own_pcm_proof(path, expected):
    require(path == REPRODUCTION, "Use the exact promoted reproduction result")
    return {"path": str(path), "sha256": expected["reproduction_proof_sha256"],
        "candidate_sha256": expected["pins"]["runtime"], "threads": 2, "validated_own_B_cases": 16,
        "closed_B_proof_sha256": expected["closed_quality_proof_sha256"],
        "promoted_build_sha256": expected["promoted_build_sha256"], "cross_arm_exact_pcm": False,
        "scope": "Promoted runtime reproduces its own closed Flash PCM; no cross-arm identity claim."}


def latency_gate(result):
    value = loaded().latency_gate(result)
    value["producer_full_audio_UI_soak_qualified"] = value.pop("full_60minute_qualification")
    value["sixty_minute_workload_completed"] = result["acceptance"]["sixty_minute_workload_completed"]
    value["integrated_stem_playback_qualified"] = False
    value["scope"] = ("Unchanged producer percentile/strict target and nominal4095ms gate. The60-minute flag "
        "covers this independent-load diagnostic, not actual stem playback or complete M2 acceptance.")
    return value


def final_proof_pins(deadline):
    return loaded().final_proof_pins(deadline)


def provenance():
    return {"adapter": str(Path(__file__).resolve()),
        "adapter_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "frozen_loaded_adapter_sha256": FROZEN_SHA,
        "frozen_observed_launcher_sha256": loaded().FROZEN_SHA,
        "rendered_launcher_sha256": hashlib.sha256(source().encode()).hexdigest(),
        "nested_helpers": loaded().provenance()["nested_helpers"],
        "scope": "Duration/promoted-proof adaptation of reviewed600s lifecycle; nested helper bytes unchanged. "
                 "4350s fixed total budget, no handled-signal cleanup or policy semantics changed."}


def main():
    text = source()
    if "--render-only" in sys.argv:
        parser = argparse.ArgumentParser(description="Render3600s launcher without host or native operations")
        parser.add_argument("--render-only", type=Path, required=True)
        args = parser.parse_args()
        args.render_only.mkdir(mode=0o700)
        files = {"observed-launch.py": text,
            **{name + ".py": loaded().cost().frozen(file, digest)
               for name, (file, digest, _, _) in loaded().NESTED.items()},
            "provenance.json": json.dumps(provenance(), indent=2) + "\n"}
        for name, content in files.items():
            with (args.render_only / name).open("x") as stream:
                stream.write(content)
        print(json.dumps(provenance(), indent=2))
        return 0
    namespace = {"__file__": str(Path(__file__).resolve()), "__name__": "conflux_flash_60minute_adapted",
        "prepare_proof_sources": prepare_proof_sources, "own_pcm_proof": own_pcm_proof,
        "latency_gate": latency_gate, "final_proof_pins": final_proof_pins,
        "provenance": provenance, "_context": _context, "SCOPE": SCOPE}
    exec(compile(text, str(FROZEN) + "[3600s-promoted-adapted]", "exec"), namespace)
    return namespace["main"]()


if __name__ == "__main__":
    raise SystemExit(main())

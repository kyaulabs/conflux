#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Ordinary-cost Flash ABBA leg; each arm must reproduce its own closed PCM."""
import argparse
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
FROZEN = BASE / "counterbalanced_runtime.py"
FROZEN_SHA = "f905f12613344dd0c0c68265a0937621910538099cf51bf9d1bf98a80e458fd3"
LAUNCHER = BASE / "counterbalanced_launch.py"
LAUNCHER_SHA = "f9c53b640d3dc0c992bbbd14e389baf5230b1c9a0a9ff8491f125e3d672e26bf"
QUALITY = BASE / "flash_quality.py"
QUALITY_SHA = "cec1a694a32da562be9ed79d35cfda7e2ee1943b9b7dfd10daf1ed2259fc7495"
CLOSED = BASE / "flash-quality-laptop-01/comparison/result.json"
CLOSED_SHA = "917104a1cc8ca2522eef38bae6d620c972fdb2be826876b2dee88e67dd7a497a"
BUILD = BASE / "flash-f32-build-verification.json"
BUILD_SHA = "9c7d503af0b0c9b4c1b4e7e8c533fe888376c2a45f19942529f2e01247d7053d"
FLASH = Path("/tmp/conflux-stem-flash-f32-2026-10-18/build/conflux-stem-runtime")
FLASH_SHA = "6f311e461e7af5f12ff38f90ddb8720179c2c3dce23d593437d2b34524dffd9d"
COMPARE = BASE / "compare_runtime.py"
COMPARE_SHA = "5022aeed8eaf932adead05404f80b94c7d992f4b6f68dc81d05edb6126077e5b"
SCOPE = ("Ordinary-cost screening only: predeclared A1/B1/B2/A2, four same-start257985 music windows "
         "per persistent two-thread Vulkan/blocking/passive child. Each arm reproduces its own closed "
         "normalized PCM proof; baseline and Flash are not bit-identical. All startup/request observations "
         "retained, no tail exclusions, UI/audio/profiler load, p99 qualification, musical verdict, causal "
         "superiority claim, new numerical gate or host policy mutation. Existing4095ms gate is unchanged.")


def require(value, message):
    if not value:
        raise ValueError(message)


def frozen(path, digest):
    before = path.stat()
    require(0 < before.st_size <= 128 * 1024, "Frozen source size differs: " + str(path))
    contents = path.read_bytes()
    after = path.stat()
    require(hashlib.sha256(contents).hexdigest() == digest
            and (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
            "Frozen source differs: " + str(path))
    return contents.decode()


def replace_once(source, old, new):
    require(source.count(old) == 1, "Expected one frozen replacement: " + old[:100])
    return source.replace(old, new, 1)


def quality():
    frozen(QUALITY, QUALITY_SHA)
    spec = importlib.util.spec_from_file_location("conflux_flash_cost_quality", QUALITY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def common_runtime_source():
    """Reuse only the frozen request loop, with truthful arm-reference labels."""
    source = frozen(COMPARE, COMPARE_SHA)
    node = next(node for node in ast.parse(source).body
                if isinstance(node, ast.FunctionDef) and node.name == "run_runtime")
    source = "\n".join(source.splitlines()[node.lineno - 1:node.end_lineno]) + "\n"
    require(source.count("output_matches_baseline") == 3, "Frozen request comparison changed")
    source = source.replace("output_matches_baseline", "output_matches_arm_proof")
    return replace_once(source, 'f"PCM SHA256 differs for {case[\'id\']}"',
                        'f"PCM differs from closed arm proof for {case[\'id\']}"')


def runtime_source():
    source = frozen(FROZEN, FROZEN_SHA)
    source = replace_once(source, '"""One declared ABBA leg: four exact-PCM requests, no UI/audio/profiler load."""',
                          '"""Adapted Flash cost leg: exact per-arm proof, no cross-arm identity claim."""')
    source = replace_once(source,
        'Path("/tmp/conflux-stem-graph-order-2026-10-18/build/conflux-stem-runtime"),\n'
        '          "e50d0f5d0b7e3ba81d2fedb7ccaf0366b6418673c01d11f500936f075f37e720"',
        'Path(' + repr(str(FLASH)) + '),\n          ' + repr(FLASH_SHA))
    source = replace_once(source, '        expected_rows = {row["id"]: row for row in proof["runtimes"][1]["requests"]}',
        '        expected_rows, report["arm_pcm_proof"] = arm_rows(compare, deadline, args.leg[0], proof, sources)')
    source = replace_once(source, '("input_sha256", "normalization", "shape", "dtype")',
                          '("input_sha256", "normalization", "shape", "dtype", "valid_frames")')
    source = replace_once(source, '        config_pin, sources = compare.source_specs(args.source_pins, deadline)',
        '        require(expected.get("closed_quality_proof_sha256") == CLOSED_SHA\n'
        '                and expected.get("flash_build_sha256") == BUILD_SHA, "Exact Flash proof configuration differs")\n'
        '        config_pin, sources = compare.source_specs(args.source_pins, deadline)')
    source = replace_once(source, '                 "proof": (PROOF, PROOF_SHA, 2 * 1024 * 1024),',
        '                 "proof": (PROOF, PROOF_SHA, 2 * 1024 * 1024),\n'
        '                 "closed_quality": (CLOSED, CLOSED_SHA, 8 * 1024 * 1024),\n'
        '                 "flash_build": (BUILD, BUILD_SHA, 2 * 1024 * 1024),')
    source = replace_once(source, '        checkpoint()\n        compare.run_runtime(',
        '        report["execution_provenance"] = execution_provenance()\n'
        '        report["cross_arm_exact_pcm"] = False\n'
        '        checkpoint()\n        compare.run_runtime(')
    source = replace_once(source,
        '            report["graph_order"] = graph.graph_order_observation(compare, report["runtimes"][0], deadline)',
        '            report["flash_evidence"] = flash_observation(compare, report["runtimes"][0], deadline)')
    # These are public proof labels, rather than the frozen cross-arm assertion.
    require(source.count("exact_pcm_match") == 3, "Frozen leg result assertion changed")
    source = source.replace("exact_pcm_match", "arm_pcm_proof_match")
    require(source.count("output_matches_baseline") == 1, "Frozen leg output assertion changed")
    source = source.replace("output_matches_baseline", "output_matches_arm_proof")
    source = replace_once(source, "exact PCM/phase/pin/cleanup/observation", "per-arm PCM/phase/pin/cleanup/observation")
    return source


def arm_rows(compare, deadline, arm, immutable, sources):
    _, data = compare.inspect_file(CLOSED, 8 * 1024 * 1024, deadline, expected=CLOSED_SHA, keep=True)
    closed = compare.decode_json(data)
    require(closed.get("schema") == 1 and closed.get("status") == "comparison_complete"
            and closed.get("completed_requests") == 32 and closed.get("exact_pcm_match") is False
            and all(closed.get(key) is True for key in
                    ("all_outputs_finite", "input_sha_match", "phase_fields_valid", "pins_unchanged", "cleanup_complete"))
            and not closed.get("errors"), "Closed numerical proof is incomplete")
    for label, digest in (("baseline", _driver["ARMS"]["A"][1]), ("candidate", FLASH_SHA),
                          ("model", compare.MODEL_SHA)):
        require(all(closed[side][label]["sha256"] == digest for side in ("pins_before", "pins_after")),
                "Closed producer pins differ: " + label)
    _, data = compare.inspect_file(BUILD, 2 * 1024 * 1024, deadline, expected=BUILD_SHA, keep=True)
    build = compare.decode_json(data)
    q = quality()
    require(build.get("status") == "built" and build.get("runtime_sha256") == FLASH_SHA
            and build.get("overlay_sha256") == q.OVERLAY_SHA and build.get("phase_header_sha256") == q.HEADER_SHA
            and build.get("full_f32_scalar") is True and build.get("graph_order") is False,
            "Full-F32 Flash build proof differs")
    for path, digest in ((q.OVERLAY, q.OVERLAY_SHA), (q.HEADER, q.HEADER_SHA),
                         (q.PIPELINE, build["generated_pipeline_sha256"]),
                         (q.VULKAN, build["generated_vulkan_sha256"])):
        require(sources.get(str(path)) == digest
                and all(closed[side]["source:" + str(path)]["sha256"] == digest
                        for side in ("pins_before", "pins_after")), "Flash build/source lineage differs: " + str(path))
    require([record["label"] for record in closed["runtimes"]] == ["baseline", "flash"], "Closed arm order differs")
    by_arm = {}
    identities = [f"{name}:{start}" for name, _ in compare.TRACKS for start in compare.STARTS]
    original = {row["id"]: row for row in immutable["runtimes"][1]["requests"]}
    for label, record in zip(("A", "B"), closed["runtimes"], strict=True):
        rows = record["requests"]
        require([row["id"] for row in rows] == identities and record["cleanup"]["complete"] is True,
                "Closed complete sixteen-case arm/order differs")
        for row in rows:
            require(row["completed"] is True and row["finite"] is True and row["input_sha_match"] is True
                    and row["output_shape"] == [4, 343980, 2] and row["output_dtype"] == "<f4"
                    and row["output_bytes"] == 11007360 and compare.HEX.fullmatch(row["output_sha256"])
                    and all(row[key] == original[row["id"]][key] for key in
                            ("input_sha256", "normalization", "shape", "dtype", "valid_frames")),
                    "Closed normalized input/output proof differs")
            if label == "A":
                require(row["output_sha256"] == original[row["id"]]["output_sha256"],
                        "Baseline no longer matches independent immutable38c proof")
        by_arm[label] = {row["id"]: row for row in rows}
    require(arm in by_arm, "Unknown arm")
    return by_arm[arm], {"path": str(CLOSED), "sha256": CLOSED_SHA, "arm": arm,
        "reference_label": "baseline" if arm == "A" else "flash", "cross_arm_exact_pcm": False,
        "immutable_baseline_sha256": _driver["PROOF_SHA"], "flash_build_sha256": BUILD_SHA,
        "scope": "Exact own-arm normalized PCM reproducibility; no cross-arm identity or new musical verdict."}


def flash_observation(compare, record, deadline):
    return quality().flash_logs(record, compare, deadline)


def session_proof(compare, sources, deadline):
    """Validate both closed arms before the launcher admits any native leg."""
    _, data = compare.inspect_file(_driver["PROOF"], 2 * 1024 * 1024, deadline,
                                   expected=_driver["PROOF_SHA"], keep=True)
    immutable = compare.decode_json(data)
    retained = helpers()[3]
    retained.comparison_proof(_driver["PROOF"],
        {"pins": {"runtime": ARMS["A"][1], "model": compare.MODEL_SHA}})
    _, receipt = arm_rows(compare, deadline, "A", immutable, sources)
    return {"path": receipt["path"], "sha256": receipt["sha256"], "validated_arm_cases": {"A": 16, "B": 16},
            "arms": {name: value[1] for name, value in ARMS.items()}, "cross_arm_exact_pcm": False,
            "immutable_baseline_sha256": _driver["PROOF_SHA"], "flash_build_sha256": BUILD_SHA}


def execution_provenance():
    return {"adapter": str(Path(__file__).resolve()), "adapter_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "frozen_runtime_sha256": FROZEN_SHA, "frozen_launcher_sha256": LAUNCHER_SHA,
            "rendered_runtime_sha256": hashlib.sha256(runtime_source().encode()).hexdigest(),
            "rendered_request_loop_sha256": hashlib.sha256(common_runtime_source().encode()).hexdigest(),
            "scope": "Executed in-memory narrow adaptations of frozen sources; not the original byte-identical scripts."}


_driver = {"__file__": str(Path(__file__).resolve()), "__name__": "conflux_flash_cost_adapted"}
exec(compile(runtime_source(), str(FROZEN) + "[flash-cost-adapted]", "exec"), _driver)
_legacy_helpers = _driver["helpers"]
_cache = []


def helpers():
    if not _cache:
        _, graph, common, retained = _legacy_helpers()
        compare = quality().helper()  # Select Flash before source_specs forbids artifact overrides.
        compare.REQUIRED_SOURCES = tuple(dict.fromkeys(compare.REQUIRED_SOURCES + (
            Path(__file__).resolve(), BASE / "flash_cost_launch.py", FROZEN, LAUNCHER,
            *(path for path, _ in _driver["HELPERS"].values()))))
        exec(compile(common_runtime_source(), str(COMPARE) + "[arm-proof-labels]", "exec"), compare.__dict__)
        _cache.append((compare, graph, common, retained))
    return _cache[0]


_driver.update(helpers=helpers, arm_rows=arm_rows, flash_observation=flash_observation,
               execution_provenance=execution_provenance, CLOSED=CLOSED, CLOSED_SHA=CLOSED_SHA,
               BUILD=BUILD, BUILD_SHA=BUILD_SHA, SCOPE=SCOPE)
ARMS, HELPERS, SAMPLER_SHA = (_driver[name] for name in ("ARMS", "HELPERS", "SAMPLER_SHA"))
verify_leg = _driver["verify_leg"]


def main():
    if "--render-only" in sys.argv:
        parser = argparse.ArgumentParser(description="Write executed source for offline review; no host/native operations")
        parser.add_argument("--render-only", type=Path, required=True)
        args = parser.parse_args()
        args.render_only.mkdir(mode=0o700)
        for name, source in (("runtime.py", runtime_source()), ("request-loop.py", common_runtime_source())):
            with (args.render_only / name).open("x") as stream:
                stream.write(source)
        print(json.dumps(execution_provenance(), indent=2))
        return 0
    return _driver["main"]()


if __name__ == "__main__":
    raise SystemExit(main())

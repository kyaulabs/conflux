#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Dry by default: reviewed ABBA supervision with independent per-arm PCM proofs."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
DRIVER = BASE / "flash_cost_runtime.py"
DRIVER_SHA = "ca0ad3468d20c2ae28d06476b995f50d49823c64ed6c57106aea059bf65baf71"


def adapter():
    contents = DRIVER.read_bytes()
    if len(contents) > 128 * 1024 or hashlib.sha256(contents).hexdigest() != DRIVER_SHA:
        raise ValueError("Frozen Flash cost adapter differs")
    spec = importlib.util.spec_from_file_location("conflux_flash_cost_driver", DRIVER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def launcher_source(driver):
    source = driver.frozen(driver.LAUNCHER, driver.LAUNCHER_SHA)
    one = driver.replace_once
    source = one(source, '"""Dry by default: A1 B1 B2 A2, fresh ordinary admission before every leg."""',
                 '"""Adapted Flash ABBA cost screen; fresh ordinary admission and own-arm PCM proof."""')
    source = one(source, 'DRIVER = BASE / "counterbalanced_runtime.py"', 'DRIVER = BASE / "flash_cost_runtime.py"')
    source = one(source, 'DRIVER_SHA = "' + driver.FROZEN_SHA + '"', 'DRIVER_SHA = "' + DRIVER_SHA + '"')
    for old, new in (("counterbalanced-expected-pins.json", "flash-cost-expected-pins.json"),
                     ("counterbalanced-source-pins.json", "flash-cost-source-pins.json"),
                     ("counterbalanced-human-readiness.json", "flash-cost-human-readiness.json")):
        source = one(source, old, new)
    source = one(source, '    initial_human = human()',
        '    initial_human = human()\n'
        '    require(expected.get("closed_quality_proof_sha256") == driver.CLOSED_SHA\n'
        '            and expected.get("flash_build_sha256") == driver.BUILD_SHA, "Exact Flash proof configuration differs")\n'
        '    proof_receipt = driver.session_proof(compare, sources, deadline)')
    source = one(source,
        '             "scope": "Predeclared A1 B1 B2 A2; four same-start music windows per child. No tail exclusions, latency qualification, causal clock/speed inference, thermal pass threshold or host policy changes."}',
        '             "scope": driver.SCOPE, "arm_pcm_proof": proof_receipt,\n'
        '             "execution_provenance": execution_provenance(driver), "cross_arm_exact_pcm": False}')
    source = one(source, '"--why=Bounded ABBA graph-order diagnostic"', '"--why=Bounded ABBA Flash cost screening"')
    source = one(source,
        '                observed = graph.graph_order_observation(compare, value["runtimes"][0], min(deadline, time.monotonic() + 10))\n'
        '                require(observed == value.get("graph_order"), "B graph-order stderr changed after driver validation")',
        '                observed = driver.flash_observation(compare, value["runtimes"][0], min(deadline, time.monotonic() + 10))\n'
        '                require(observed == value.get("flash_evidence"), "B Flash stderr changed after driver validation")')
    source = one(source,
        '        pairs = {}\n'
        '        for value in results:\n'
        '            for request in value["runtimes"][0]["requests"]:\n'
        '                case = pairs.setdefault(request["id"], {"input_sha256": request["input_sha256"], "output_sha256": request["output_sha256"], "legs": []})\n'
        '                require((case["input_sha256"], case["output_sha256"]) == (request["input_sha256"], request["output_sha256"]),\n'
        '                        "Paired PCM hash differs between legs")\n'
        '                case["legs"].append(value["leg"])\n'
        '        require(len(pairs) == 4 and all(case["legs"] == list(LEGS) for case in pairs.values()), "Paired four-case coverage incomplete")\n'
        '        save(status="completed", paired_exact_hashes=pairs, completed_requests=16)',
        '        pairs = {}\n'
        '        for value in results:\n'
        '            for request in value["runtimes"][0]["requests"]:\n'
        '                case = pairs.setdefault(request["id"], {"input_sha256": request["input_sha256"], "output_sha256_by_arm": {}, "legs": []})\n'
        '                require(case["input_sha256"] == request["input_sha256"], "Paired normalized inputs differ")\n'
        '                expected_hash = case["output_sha256_by_arm"].setdefault(value["arm"], request["output_sha256"])\n'
        '                require(expected_hash == request["output_sha256"], "Repeated own-arm PCM differs")\n'
        '                case["legs"].append(value["leg"])\n'
        '        require(len(pairs) == 4 and all(case["legs"] == list(LEGS) and set(case["output_sha256_by_arm"]) == {"A", "B"}\n'
        '                for case in pairs.values()), "Paired four-case/own-arm coverage incomplete")\n'
        '        cross_identity = all(case["output_sha256_by_arm"]["A"] == case["output_sha256_by_arm"]["B"] for case in pairs.values())\n'
        '        require(cross_identity is False, "Unexpected cross-arm identity differs from closed numerical evidence")\n'
        '        save(status="completed", paired_arm_hashes=pairs, completed_requests=16,\n'
        '             input_sha_match=True, arm_pcm_proof_match=True, within_arm_repeat_match=True, cross_arm_exact_pcm=cross_identity)')
    return source


def execution_provenance(driver):
    return {**driver.execution_provenance(), "launcher_adapter": str(Path(__file__).resolve()),
            "launcher_adapter_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "rendered_launcher_sha256": hashlib.sha256(launcher_source(driver).encode()).hexdigest()}


def main():
    driver = adapter()
    source = launcher_source(driver)
    if "--render-only" in sys.argv:
        parser = argparse.ArgumentParser(description="Render exact executed sources offline; no host/native operations")
        parser.add_argument("--render-only", type=Path, required=True)
        args = parser.parse_args()
        args.render_only.mkdir(mode=0o700)
        sources = (("launcher.py", source), ("runtime.py", driver.runtime_source()),
                   ("request-loop.py", driver.common_runtime_source()))
        for name, text in sources:
            with (args.render_only / name).open("x") as stream:
                stream.write(text)
        receipt = execution_provenance(driver)
        with (args.render_only / "provenance.json").open("x") as stream:
            json.dump(receipt, stream, indent=2)
            stream.write("\n")
        print(json.dumps(receipt, indent=2))
        return 0
    namespace = {"__file__": str(Path(__file__).resolve()), "__name__": "conflux_flash_cost_launch_adapted",
                 "execution_provenance": execution_provenance}
    exec(compile(source, str(driver.LAUNCHER) + "[flash-cost-adapted]", "exec"), namespace)
    return namespace["main"]()


if __name__ == "__main__":
    raise SystemExit(main())

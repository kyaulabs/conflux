#!/usr/bin/env python3
"""Offline per-arm proof/pairing checks; no worker, model, media or host access."""
import argparse
import ast
import copy
import hashlib
import json
from pathlib import Path
import re
from types import SimpleNamespace

BASE = Path(__file__).resolve().parent
PINS = {
    "flash_cost_runtime.py": "ca0ad3468d20c2ae28d06476b995f50d49823c64ed6c57106aea059bf65baf71",
    "flash_cost_launch.py": "b00ed06fe1fb28965c2d6272f35040899b407e4a693eef71256392671ccefd98",
    "flash-cost-render-01/runtime.py": "b389c49c834b1a70d2d28b6c847bdaab0623bdbfedcf470e1b8c10c8a615e927",
    "flash-cost-render-01/request-loop.py": "f1b01f1f55fd41aaef2f562372605ff2dde3bdd1930a268eca79f96b8150142e",
    "flash-cost-render-01/launcher.py": "a92e8f39d4780fdfd1784271b22b1bca7a1b3fd47a8f335ecfe68a370f7d0063",
    "flash-quality-laptop-01/comparison/result.json": "917104a1cc8ca2522eef38bae6d620c972fdb2be826876b2dee88e67dd7a497a",
    "phase-timing-laptop-01-comparison/result.json": "38c60301feed297a0484d61ea92cca2413067d15d7ead7e9a27e6cc9909037e3",
    "flash-f32-build-verification.json": "9c7d503af0b0c9b4c1b4e7e8c533fe888376c2a45f19942529f2e01247d7053d",
}
BASELINE_SHA = "ded9cb2c1000af78917a784e281d905da201d735338221519958d9d7df1d681f"
FLASH_SHA = "6f311e461e7af5f12ff38f90ddb8720179c2c3dce23d593437d2b34524dffd9d"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_plan", "native_executed": False,
                          "scope": "Pinned JSON and extracted AST functions only; no inherited lifecycle/admission retest"}))
        return 0
    texts = {}
    for name, expected in PINS.items():
        raw = (BASE / name).read_bytes()
        assert len(raw) <= 8 * 1024 * 1024 and hashlib.sha256(raw).hexdigest() == expected, name
        texts[name] = raw.decode()
    closed = json.loads(texts["flash-quality-laptop-01/comparison/result.json"])
    immutable = json.loads(texts["phase-timing-laptop-01-comparison/result.json"])
    build = json.loads(texts["flash-f32-build-verification.json"])
    closed_path, build_path = BASE / "flash-quality-laptop-01/comparison/result.json", BASE / "flash-f32-build-verification.json"
    upstream = Path("/tmp/conflux-stem-flash-f32-2026-10-18/build")
    q = SimpleNamespace(OVERLAY=BASE / "flash_f32_overlay.cmake",
        OVERLAY_SHA="a4951aadce8272b103e31a12c121c237ca8545b926690ebcc60708d979e7eb15",
        HEADER=upstream / "conflux-graph-phase-timing/include/engine/models/demucs/pipeline.h",
        HEADER_SHA="7f48bbd4964ebcce721f7544a84303c568b460bf21c24090899ccc8b983833cd",
        PIPELINE=upstream / "conflux-flash-f32/pipeline.cpp", VULKAN=upstream / "conflux-flash-f32/ggml-vulkan.cpp")
    sources = {str(path): closed["pins_before"]["source:" + str(path)]["sha256"]
               for path in (q.OVERLAY, q.HEADER, q.PIPELINE, q.VULKAN)}
    def require(value, message):
        if not value:
            raise ValueError(message)
    namespace = {"require": require, "quality": lambda: q,
        "CLOSED": closed_path, "CLOSED_SHA": PINS["flash-quality-laptop-01/comparison/result.json"],
        "BUILD": build_path, "BUILD_SHA": PINS["flash-f32-build-verification.json"], "FLASH_SHA": FLASH_SHA,
        "_driver": {"ARMS": {"A": (None, BASELINE_SHA)},
                    "PROOF_SHA": PINS["phase-timing-laptop-01-comparison/result.json"]}}
    node = next(node for node in ast.parse(texts["flash_cost_runtime.py"]).body
                if isinstance(node, ast.FunctionDef) and node.name == "arm_rows")
    exec(compile(ast.Module(body=[node], type_ignores=[]), "pinned-arm-rows-only", "exec"), namespace)
    arm_rows = namespace["arm_rows"]
    tracks = ("club_xl", "when_the_light_breaks", "under_my_skin", "stfu")
    class FixtureCompare:
        # Semantic mutation fixtures intentionally stub hash I/O only AFTER the
        # real JSON inputs above have been independently SHA-pinned.
        MODEL_SHA = closed["pins_before"]["model"]["sha256"]
        HEX = re.compile(r"[0-9a-f]{64}\Z")
        TRACKS, STARTS = tuple((track, None) for track in tracks), (0, 257985, 515970, 773955)
        def __init__(self, data=None, model=None):
            self.data = closed if data is None else data
            if model is not None:
                self.MODEL_SHA = model
        def inspect_file(self, path, maximum, deadline, *, expected, keep):
            assert keep and path in (closed_path, build_path)
            assert expected == namespace["CLOSED_SHA" if path == closed_path else "BUILD_SHA"]
            data = self.data if path == closed_path else build
            raw = json.dumps(data).encode()
            assert len(raw) <= maximum
            return {"path": str(path), "sha256": expected}, raw
        @staticmethod
        def decode_json(raw):
            return json.loads(raw)
    checked = []
    a, receipt_a = arm_rows(FixtureCompare(), 0, "A", immutable, sources)
    b, receipt_b = arm_rows(FixtureCompare(), 0, "B", immutable, sources)
    assert len(a) == len(b) == 16 and receipt_a["arm"] == "A" and receipt_b["arm"] == "B"
    assert receipt_a["reference_label"] == "baseline" and receipt_b["reference_label"] == "flash"
    for row in immutable["runtimes"][1]["requests"]:
        assert a[row["id"]]["output_sha256"] == row["output_sha256"]
        assert b[row["id"]]["output_sha256"] != row["output_sha256"]
    checked.append("real_own_arm_selection_and_immutable_baseline_lineage")

    def rejected(name, action):
        try:
            action()
        except ValueError:
            checked.append(name)
        else:
            raise AssertionError("Accepted invalid proof: " + name)
    rejected("reject_unknown_arm", lambda: arm_rows(FixtureCompare(), 0, "C", immutable, sources))
    rejected("reject_wrong_model", lambda: arm_rows(FixtureCompare(model="0" * 64), 0, "B", immutable, sources))
    wrong_sources = dict(sources, **{str(q.HEADER): "0" * 64})
    rejected("reject_wrong_source_pin", lambda: arm_rows(FixtureCompare(), 0, "B", immutable, wrong_sources))
    mutations = {
        "reject_wrong_input_hash": lambda data: data["runtimes"][1]["requests"][0].update(input_sha256="0" * 64),
        "reject_wrong_output_geometry": lambda data: data["runtimes"][1]["requests"][0].update(output_shape=[4, 343979, 2]),
        "reject_wrong_normalization": lambda data: data["runtimes"][1]["requests"][0]["normalization"].update(std=999),
        "reject_wrong_valid_frames": lambda data: data["runtimes"][1]["requests"][0].update(valid_frames=1),
        "reject_wrong_producer_pin": lambda data: data["pins_after"]["candidate"].update(sha256="0" * 64),
        "reject_baseline_breaking_immutable_lineage": lambda data: data["runtimes"][0]["requests"][0].update(output_sha256="0" * 64),
    }
    for name, mutation in mutations.items():
        data = copy.deepcopy(closed)
        mutation(data)
        rejected(name, lambda data=data: arm_rows(FixtureCompare(data), 0, "B", immutable, sources))

    launcher = ast.parse(texts["flash-cost-render-01/launcher.py"])
    blocks = []
    for node in ast.walk(launcher):
        for field in ("body", "orelse", "finalbody"):
            values = getattr(node, field, None)
            if not isinstance(values, list):
                continue
            for index, item in enumerate(values):
                if (isinstance(item, ast.Assign) and isinstance(item.value, ast.Dict) and not item.value.keys
                    and any(isinstance(target, ast.Name) and target.id == "pairs" for target in item.targets)):
                    end = next(i for i in range(index + 1, len(values)) if isinstance(values[i], ast.Expr)
                               and isinstance(values[i].value, ast.Call) and isinstance(values[i].value.func, ast.Name)
                               and values[i].value.func.id == "save")
                    blocks.append(values[index:end + 1])
    assert len(blocks) == 1
    pairing = compile(ast.Module(body=blocks[0], type_ignores=[]), "actual-rendered-launcher-pairing-only", "exec")
    legs = ("A1", "B1", "B2", "A2")
    selected = [f"{track}:257985" for track in tracks]
    results = [{"leg": leg, "arm": leg[0], "runtimes": [{"requests": [
        {"id": key, "input_sha256": a[key]["input_sha256"],
         "output_sha256": (a if leg[0] == "A" else b)[key]["output_sha256"]} for key in selected]}]} for leg in legs]
    def pair(values):
        saved = {}
        scope = {"results": values, "LEGS": legs, "require": require, "save": lambda **kwargs: saved.update(kwargs)}
        exec(pairing, scope)
        return saved
    saved = pair(results)
    assert saved["status"] == "completed" and saved["completed_requests"] == 16
    assert saved["within_arm_repeat_match"] is True and saved["arm_pcm_proof_match"] is True
    assert saved["cross_arm_exact_pcm"] is False and saved["input_sha_match"] is True
    checked.append("actual_pairing_accepts_A1_equals_A2_B1_equals_B2_with_distinct_A_B")
    bad = copy.deepcopy(results)
    bad[2]["runtimes"][0]["requests"][0]["output_sha256"] = "0" * 64
    rejected("reject_within_arm_output_mismatch", lambda: pair(bad))
    bad_input = copy.deepcopy(results)
    bad_input[1]["runtimes"][0]["requests"][0]["input_sha256"] = "0" * 64
    rejected("reject_cross_arm_input_mismatch", lambda: pair(bad_input))
    missing = copy.deepcopy(results)
    missing[2]["runtimes"][0]["requests"].pop()
    rejected("reject_missing_case_coverage", lambda: pair(missing))
    identical = copy.deepcopy(results)
    for leg in identical:
        for row in leg["runtimes"][0]["requests"]:
            row["output_sha256"] = a[row["id"]]["output_sha256"]
    rejected("reject_unexpected_cross_arm_identity", lambda: pair(identical))
    output = {"status": "passed", "checks": checked, "source_pins": PINS,
              "native_executed": False, "media_or_model_read": False,
              "scope": "Real pinned metadata plus semantic mutations and extracted actual pairing AST. Hash I/O is stubbed for mutated fixtures. No admission/lifecycle, musical-equivalence or performance proof."}
    if args.output is not None:
        with args.output.open("x") as stream:
            json.dump(output, stream, indent=2)
            stream.write("\n")
    print(json.dumps(output, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

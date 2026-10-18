#!/usr/bin/env python3
"""Offline numerical/logger fixtures only; never import a worker or native driver main."""
import argparse
import ast
import hashlib
import json
import math
from pathlib import Path
import re
import tempfile

BASE = Path(__file__).resolve().parent
PINS = {
    "flash_quality.py": "cec1a694a32da562be9ed79d35cfda7e2ee1943b9b7dfd10daf1ed2259fc7495",
    "flash_quality_launch.py": "ab912a468791b13ca283bacd84ce30163ed1c7a0576fd099dc859fd4670294c4",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_plan", "native_executed": False,
                          "scope": "Extract four pinned pure functions; NumPy math and synthetic stderr fixtures only"}))
        return 0
    import numpy as np
    for name, expected in PINS.items():
        assert hashlib.sha256((BASE / name).read_bytes()).hexdigest() == expected, name
    tree = ast.parse((BASE / "flash_quality.py").read_text())
    names = {"require", "channel_metrics", "numerical_difference", "flash_logs"}
    selected = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    assert {node.name for node in selected} == names
    namespace = {"math": math, "re": re, "Path": Path}
    exec(compile(ast.Module(body=selected, type_ignores=[]), "pinned-flash-quality-pure-functions", "exec"), namespace)
    metric, difference, logs = (namespace[name] for name in
                                ("channel_metrics", "numerical_difference", "flash_logs"))
    checked = []

    def close(value, expected):
        assert math.isclose(value, expected, rel_tol=1e-12, abs_tol=1e-15), (value, expected)

    reference = np.array([1, -1, 2, -2], dtype="<f4")
    candidate = np.array([1.5, -1, 2, -2], dtype="<f4")
    row = metric(reference, candidate, np)
    assert row["samples"] == 4 and row["reference_status"] == "finite_nonzero_reference"
    close(row["max_absolute_error"], .5)
    close(row["rmse"], .25)
    close(row["reference_rms"], math.sqrt(10 / 4))
    close(row["relative_l2"], math.sqrt(.25 / 10))
    close(row["snr_dB"], 10 * math.log10(40))
    checked.append("independent_per_channel_error_formula")

    zero = np.zeros(2, dtype="<f4")
    exact = metric(zero, zero, np)
    nonzero = metric(zero, np.array([0, 1], dtype="<f4"), np)
    assert exact["reference_status"] == "zero_reference_exact" and exact["relative_l2"] == 0 and exact["snr_dB"] is None
    assert nonzero["reference_status"] == "zero_reference_nonzero_error"
    assert nonzero["relative_l2"] is None and nonzero["snr_dB"] is None
    close(nonzero["rmse"], math.sqrt(.5))
    signed_a, signed_b = np.array([-0.0, 0.0], dtype="<f4"), np.array([0.0, -0.0], dtype="<f4")
    assert hashlib.sha256(signed_a.tobytes()).digest() != hashlib.sha256(signed_b.tobytes()).digest()
    assert metric(signed_a, signed_b, np)["max_absolute_error"] == 0
    checked.append("zero_reference_and_numeric_versus_bit_identity")

    maximum = np.finfo(np.float32).max
    tiny = np.nextafter(np.float32(0), np.float32(1))
    extreme = metric(np.array([tiny], dtype="<f4"), np.array([maximum], dtype="<f4"), np)
    opposite = metric(np.array([maximum], dtype="<f4"), np.array([-maximum], dtype="<f4"), np)
    assert all(math.isfinite(row[key]) for row in (extreme, opposite)
               for key in ("rmse", "max_absolute_error", "relative_l2", "snr_dB"))
    json.dumps([extreme, opposite], allow_nan=False)
    checked.append("finite_float32_extremes_keep_finite_float64_metrics")

    valid, window = 257985, 343980
    reference = np.ones((4, window, 2), dtype="<f4")
    candidate = reference.copy()
    candidate[3, -1, 1] += 2
    regions = difference(reference, candidate, valid, np)
    assert regions["valid"]["frames"] == valid and regions["padding"]["frames"] == 85995
    for stem in ("drums", "bass", "other", "vocals"):
        for channel in ("left", "right"):
            assert regions["valid"]["metrics"][stem][channel]["max_absolute_error"] == 0
    close(regions["padding"]["metrics"]["vocals"]["right"]["rmse"], 2 / math.sqrt(85995))
    close(regions["whole"]["metrics"]["vocals"]["right"]["rmse"], 2 / math.sqrt(window))
    assert difference(reference, reference, window, np)["padding"] == {
        "frames": 0, "metrics": None, "status": "not_applicable"}
    json.dumps(regions, allow_nan=False)
    checked.append("tail_error_does_not_contaminate_valid_region_and_empty_padding_is_explicit")
    del reference, candidate, regions

    signatures = [(1344, 1344)] * 3 + [(2688, 2688)] * 3 + [(2688, 1344)] * 2 + [(1344, 2688)] * 2
    lines = [f"conflux_flash_f32_node index={i + 100} q=64,{q},8,1 k=64,{k},8,1 v=64,{k},8,1 "
             f"out=64,8,{q},1 scale=0.125 precision=f32 supported=1" for i, (q, k) in enumerate(signatures)]
    lines += ["conflux_flash_f32_graph nodes=2900 flash_nodes=10 self1344=3 self2688=3 cross2688to1344=2 cross1344to2688=2",
              "conflux_flash_f32_pipeline shader=flash_attn_f32_f16_fp32 path=scalar intermediate_bytes=4 accumulation_bytes=4 hsk=64 hsv=64 Br=4 Bc=32 workgroup=128 subgroup=0 k=f32 v=f32"]
    class Compare:
        @staticmethod
        def inspect_file(path, maximum, deadline, *, expected, keep):
            raw = path.read_bytes()
            assert len(raw) <= maximum and hashlib.sha256(raw).hexdigest() == expected and keep
            return {"path": str(path), "sha256": expected}, raw
    with tempfile.TemporaryDirectory(prefix="conflux-flash-quality-offline-") as owned:
        path = Path(owned) / "synthetic.stderr"
        def parse(rows, truncated=False):
            raw = ("\n".join(rows) + "\n").encode()
            path.write_bytes(raw)
            return logs({"stderr": {"path": str(path), "bytes": len(raw), "original_bytes": len(raw),
                                    "sha256": hashlib.sha256(raw).hexdigest(), "truncated": truncated}}, Compare(), 0)
        parsed = parse(lines)
        assert parsed["signature_counts"] == [3, 3, 2, 2] and parsed["pipelines"][0]["subgroup"] == 0
        rejected = {
            "missing_authoritative_kv_suffix": (lines[:-1] + [lines[-1].replace(" k=f32 v=f32", "")], False),
            "half_intermediate_shader": (lines[:-1] + [lines[-1].replace("shader=flash_attn_f32_f16_fp32", "shader=flash_attn_f32_f16")], False),
            "mismatched_key_value_length": ([lines[0].replace("v=64,1344,8,1", "v=64,2688,8,1")] + lines[1:], False),
            "duplicate_node_identity": (lines + [lines[0]], False),
            "missing_graph_summary": (lines[:10] + lines[11:], False),
            "forbidden_graph_order": (lines + ["conflux_graph_order nodes=2900 moved_positions=2000 graph_buffer_bytes=1"], False),
            "truncated_stderr": (lines, True),
        }
        for name, (rows, truncated) in rejected.items():
            try:
                parse(rows, truncated)
            except ValueError:
                checked.append("reject_" + name)
            else:
                raise AssertionError("Accepted invalid Flash evidence: " + name)
    checked.append("authoritative_full_f32_scalar_ten_node_log_accepted")
    print(json.dumps({"status": "passed", "source_pins": PINS, "checks": checked,
                      "native_executed": False, "musical_quality": "not_assessed",
                      "scope": "Offline metric arithmetic and synthetic evidence parsing; no native/completion/cleanup integration proof"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

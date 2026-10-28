"""Temporary full-table/cost regression using the actual scratch source."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--baseline", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parent
label = "baseline" if args.baseline else "candidate"
source = root / ("PreparedScratchSource." + label + ".cpp")
binary = root / ("filter-" + label)
output = root / (label + ".f32")
compile_argv = [
    "c++", "-O2", "-DNDEBUG", "-std=c++20", "-pthread",
    "-I" + str(root / "include"),
    '-DSOURCE_FILE="' + str(source) + '"',
    str(root / "filter_probe.cpp"), "-Wl,--wrap=cos", "-o", str(binary),
]
run = subprocess.run(compile_argv, capture_output=True, timeout=90)
(root / (label + "-compile.log")).write_bytes(run.stdout + run.stderr)
assert run.returncode == 0, run.stderr.decode()
run = subprocess.run([str(binary), str(output)], capture_output=True, timeout=30)
(root / (label + "-run.log")).write_bytes(run.stdout + run.stderr)
data = json.loads(run.stdout)
data.update(exit_code=run.returncode, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
            output_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
            compile_argv=compile_argv)
assert data["coefficients"] == 3739200
assert output.stat().st_size == 14956800
if args.baseline:
    assert run.returncode == 2 and data["cosine_calls"] > data["cosine_budget"]
    data["expected_red_cost_failure"] = True
else:
    assert run.returncode == 0 and data["cosine_calls"] <= data["cosine_budget"]
    baseline = root / "baseline.f32"
    assert baseline.read_bytes() == output.read_bytes(), "Scratch filter coefficients changed"
    data["all_3739200_coefficients_byte_identical"] = True
(root / (label + "-result-01.json")).write_text(json.dumps(data, indent=2) + "\n")
print(json.dumps({key: value for key, value in data.items() if key != "compile_argv"}))

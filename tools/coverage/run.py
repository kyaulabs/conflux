#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Measure the existing suite in build/coverage; never use it for timing claims.

Run with an isolated interpreter containing requirements.txt and NumPy. Previous
coverage counters and reports in this dedicated build directory are reset.
"""
import argparse
import importlib.metadata
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "coverage"
REPORTS = BUILD / "reports"
DATA = BUILD / "python-data"
FLAGS = "--coverage -O0 -g -fprofile-abs-path -fprofile-update=atomic"


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def counts(covered, total):
    return {"covered": covered, "total": total,
            "percent": 100 * covered / total if total else None,
            "above_80_percent": covered * 5 > total * 4 if total else False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--private-routing", action="store_true",
                        help="also run isolated PipeWire routing and preparation controller checks")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if BUILD.is_symlink() or REPORTS.is_symlink() or DATA.is_symlink():
        parser.error("Dedicated coverage directories must not be symlinks")
    BUILD.mkdir(parents=True, exist_ok=True)
    if REPORTS.exists():
        shutil.rmtree(REPORTS)
    REPORTS.mkdir()
    DATA.mkdir(exist_ok=True)
    for path in BUILD.rglob("*.gcda"):
        path.unlink()
    for path in DATA.glob(".coverage*"):
        path.unlink()

    stages = {}
    errors = []
    summary = {
        "schema": 1,
        "scope": "Instrumented CTest suite, offscreen smoke and optional private routing/preparation checks; not a latency, native-device or QML coverage measurement.",
        "private_routing_requested": args.private_routing,
        "private_routing_execution_deadline_gate": False,
        "interpreter": sys.executable,
        "cpp_flags": FLAGS,
        "measured_sources": {"cpp": "src/** including main, probe and instrumented headers",
                             "python": "tools/stems/** and tools/timing/**"},
        "unmeasured_product_sources": {
            "qml": sorted(str(p.relative_to(ROOT)) for p in (ROOT / "qml").glob("*.qml")) +
                   ["brand/integration/qtquick/ConfluxTheme.qml"],
            "native_stem_adapter": ["tools/runtime/htdemucs_segment.cpp"],
        },
        "separate_support_scope": ["tools/benchmarks/**", "tools/experiments/**",
                                   "tools/coverage/**", "tests/**", "brand/source/**"],
        "line_target": {"operator": ">", "percent": 80,
                        "enforced_in_this_baseline": False,
                        "measured_status": "measurement_incomplete",
                        "whole_product_status": "unmeasured_sources_remain"},
    }
    base_env = os.environ.copy()
    for name in ("COVERAGE_PROCESS_START", "COVERAGE_FILE", "COVERAGE_RCFILE"):
        base_env.pop(name, None)

    def run(name, command, env=base_env, timeout=180):
        print(f"{name}: {' '.join(map(str, command))}", flush=True)
        log = REPORTS / f"{name}.log"
        timed_out = False
        with log.open("w", encoding="utf-8") as output:
            try:
                process = subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                           stdout=output, stderr=subprocess.STDOUT,
                                           start_new_session=True)
                try:
                    code = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    output.write(f"\nTimed out after {timeout}s; requesting cleanup\n")
                    output.flush()
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
                    code = 1
            except OSError as error:
                output.write(f"\n{error}\n")
                code = 1
        stages[name] = {"command": list(map(str, command)), "exit": code,
                        "log": log.name, "timed_out": timed_out}
        return code == 0

    try:
        for module in ("coverage", "gcovr", "numpy"):
            if importlib.util.find_spec(module) is None:
                raise RuntimeError(f"Missing {module}; use an isolated interpreter with tools/coverage/requirements.txt and NumPy")
        summary["tools"] = {module: importlib.metadata.version(module)
                            for module in ("coverage", "gcovr", "numpy")}
        for program in ("cmake", "ctest", "gcov", "ninja"):
            if shutil.which(program) is None:
                raise RuntimeError(f"Missing build/coverage tool: {program}")
        if args.private_routing:
            for program in ("pipewire", "pw-dump"):
                if shutil.which(program) is None:
                    raise RuntimeError(f"Missing private routing tool: {program}")

        hook_dir = BUILD / "python-startup"
        hook_dir.mkdir(exist_ok=True)
        (hook_dir / "sitecustomize.py").write_text(
            "import os\n"
            "if os.environ.get('COVERAGE_PROCESS_START'):\n"
            "    import coverage\n"
            "    coverage.process_startup()\n", encoding="utf-8")
        config = BUILD / "python-coverage.ini"
        config.write_text(
            "[run]\nbranch = true\nparallel = true\n"
            f"source_dirs =\n    {ROOT / 'tools' / 'stems'}\n    {ROOT / 'tools' / 'timing'}\n"
            f"data_file = {DATA / '.coverage'}\n"
            "[report]\ninclude_namespace_packages = true\nprecision = 2\nshow_missing = true\n",
            encoding="utf-8")
        test_env = base_env.copy()
        test_env["COVERAGE_PROCESS_START"] = str(config)
        test_env["COVERAGE_FILE"] = str(DATA / ".coverage")
        test_env["PYTHONPATH"] = str(hook_dir) + (os.pathsep + base_env["PYTHONPATH"]
                                                 if base_env.get("PYTHONPATH") else "")
        configured = run("configure", ["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug", "-DBUILD_TESTING=ON",
            f"-DCMAKE_CXX_FLAGS={FLAGS}", "-DCMAKE_EXE_LINKER_FLAGS=--coverage",
            "-DCMAKE_SHARED_LINKER_FLAGS=--coverage", f"-DPython3_EXECUTABLE={sys.executable}"])
        built = configured and run("build", ["cmake", "--build", str(BUILD), "--parallel", str(args.jobs)], timeout=900)
        if built:
            run("ctest", ["ctest", "--test-dir", str(BUILD), "--output-on-failure", "--timeout", "120"],
                env=test_env, timeout=900)
            smoke_env = test_env | {"QT_QPA_PLATFORM": "offscreen", "QT_QUICK_BACKEND": "software",
                                    "QT_QUICK_CONTROLS_STYLE": "Basic"}
            run("smoke", [str(BUILD / "conflux"), "--smoke-test"], env=smoke_env, timeout=30)
            if args.private_routing:
                for stage, binary, options in [
                    ("private-routing", "conflux-manual-routing-check", []),
                    ("private-preparation", "conflux-preparation-controller-check", []),
                    ("private-continuation-takeover", "conflux-preparation-controller-check", ["--continuation-case", "takeover"]),
                    ("private-continuation-miss", "conflux-preparation-controller-check", ["--continuation-case", "miss"]),
                    ("private-continuation-cancel", "conflux-preparation-controller-check", ["--continuation-case", "cancel"]),
                    ("private-continuation-stop", "conflux-preparation-controller-check", ["--continuation-case", "stop"]),
                    ("private-continuation-foreground", "conflux-preparation-controller-check", ["--continuation-case", "foreground"]),
                ]:
                    with tempfile.TemporaryDirectory(prefix=f"conflux-coverage-{stage}-") as temporary:
                        evidence = Path(temporary) / "evidence"
                        try:
                            # Raise a graceful timeout into the ownership wrapper's
                            # existing finally so its separately sessioned daemon
                            # is reaped rather than orphaned by an immediate kill.
                            launcher = (
                                "import runpy, signal, sys\n"
                                "def stop(*_):\n"
                                "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                                "    raise InterruptedError('Coverage stage timed out')\n"
                                "signal.signal(signal.SIGTERM, stop)\n"
                                "sys.argv = sys.argv[1:]\n"
                                "runpy.run_path(sys.argv[0], run_name='__main__')\n")
                            run(stage, [sys.executable, "-c", launcher,
                                str(ROOT / "tools" / "experiments" / "private_routing_check.py"),
                                str(BUILD / binary), str(evidence), "--functional-only", *options],
                                # The wrapper gives functional checks 240s.
                                # Leave room for private daemon startup, graph
                                # inspection and reaping before this outer guard.
                                env=test_env, timeout=270)
                        finally:
                            if evidence.exists():
                                shutil.copytree(evidence, REPORTS / stage,
                                                ignore=shutil.ignore_patterns("*.wav"))

        # Scan both .gcno and .gcda, including test objects that instantiate
        # production inline headers. Only the report is filtered to src/.
        run("gcovr", [sys.executable, "-m", "gcovr", "--root", str(ROOT),
            "--filter", re.escape(str(ROOT / "src")) + "/", str(BUILD),
            "--json", str(REPORTS / "cpp.json"),
            "--json-summary", str(REPORTS / "cpp-summary.json"),
            "--html-details", str(REPORTS / "cpp.html"),
            "--cobertura", str(REPORTS / "cpp.xml"), "--print-summary"], timeout=300)
        prefix = [sys.executable, "-m", "coverage"]
        run("python-combine", prefix + ["combine", "--rcfile", str(config), "--keep", str(DATA)])
        for kind, options in [("json", ["-o", str(REPORTS / "python.json")]),
                              ("html", ["-d", str(REPORTS / "python-html")]),
                              ("xml", ["-o", str(REPORTS / "python.xml")])]:
            run(f"python-{kind}", prefix + [kind, "--rcfile", str(config)] + options)

        cpp_path, python_path = REPORTS / "cpp-summary.json", REPORTS / "python.json"
        if cpp_path.exists():
            cpp = json.loads(cpp_path.read_text(encoding="utf-8"))
            summary["cpp"] = {"line": counts(cpp["line_covered"], cpp["line_total"]),
                              "branch": counts(cpp["branch_covered"], cpp["branch_total"])}
            files = json.loads((REPORTS / "cpp.json").read_text(encoding="utf-8"))["files"]
            reported = {item["file"] for item in files}
            summary["cpp"]["missing_compiled_source_reports"] = sorted(
                str(p.relative_to(ROOT)) for p in (ROOT / "src").rglob("*.cpp")
                if str(p.relative_to(ROOT)) not in reported)
            if summary["cpp"]["missing_compiled_source_reports"]:
                errors.append("C++ coverage is missing src/ implementation files; see missing_compiled_source_reports")
        if python_path.exists():
            python = json.loads(python_path.read_text(encoding="utf-8"))["totals"]
            # coverage.py percent_covered mixes lines and branches; count each
            # dimension explicitly rather than treating that value as lines.
            summary["python"] = {"line": counts(python["covered_lines"], python["num_statements"]),
                                 "branch": counts(python["covered_branches"], python["num_branches"])}
        if "cpp" in summary and "python" in summary:
            cpp, python = summary["cpp"]["line"], summary["python"]["line"]
            summary["line_target"]["measured_status"] = (
                "met_for_both_measured_scopes" if cpp["above_80_percent"] and python["above_80_percent"] else "unmet")
            if summary["cpp"]["missing_compiled_source_reports"]:
                summary["line_target"]["measured_status"] = "measurement_incomplete"
        else:
            summary["line_target"]["measured_status"] = "measurement_incomplete"
    except Exception as error:
        errors.append(str(error))
    summary["stages"] = stages
    summary["errors"] = errors
    summary["execution_passed"] = not errors and bool(stages) and all(s["exit"] == 0 for s in stages.values())
    if not summary["execution_passed"]:
        summary["line_target"]["measured_status"] = "measurement_incomplete"
    save(REPORTS / "summary.json", summary)
    print(json.dumps(summary, indent=2))
    # A genuine baseline below target is reported, not manufactured into a
    # passing threshold. Test/build/report failures still fail the command.
    return 0 if summary["execution_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

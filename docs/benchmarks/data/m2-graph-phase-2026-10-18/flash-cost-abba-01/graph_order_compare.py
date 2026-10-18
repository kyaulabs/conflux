#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Isolated adapter for the frozen 32-request PCM comparison; dry by default."""
import argparse
import hashlib
import importlib.util
import math
import os
from pathlib import Path
import re
import sys
import time

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
HELPER = BASE / "compare_runtime.py"
HELPER_SHA = "5022aeed8eaf932adead05404f80b94c7d992f4b6f68dc81d05edb6126077e5b"
CANDIDATE_ROOT = Path("/tmp/conflux-stem-graph-order-2026-10-18")
CANDIDATE = CANDIDATE_ROOT / "build/conflux-stem-runtime"
UPSTREAM = Path("/tmp/conflux-stem-m0/audio.cpp-0097d365a00b52a6991ad7618f0a04e8d2935e47")
REQUIRED_EXPERIMENT_SOURCES = (
    Path(__file__).resolve(), BASE / "graph_order_compare_launch.py", BASE / "graph_order_overlay.cmake",
    *(CANDIDATE_ROOT / "source" / name for name in
      ("CMakeLists.txt", "BlockingVulkanWait.cmake", "GraphPhaseTiming.cmake", "htdemucs_segment.cpp")),
    CANDIDATE_ROOT / "build/conflux-graph-phase-timing/pipeline.cpp",
    UPSTREAM / "external/ggml/src/ggml-backend-impl.h",
    UPSTREAM / "external/ggml/include/ggml-vulkan.h",
    UPSTREAM / "external/ggml/include/ggml.h",
    UPSTREAM / "external/ggml/include/ggml-alloc.h",
)
DEBUG_FLAGS = ("MESA_GPU_TRACES", "MESA_GPU_TRACEFILE", "INTEL_DEBUG")


def verified_driver():
    before = HELPER.stat()
    contents = HELPER.read_bytes()
    after = HELPER.stat()
    require(len(contents) <= 128 * 1024 and hashlib.sha256(contents).hexdigest() == HELPER_SHA
            and (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
            "Frozen comparison helper differs")
    spec = importlib.util.spec_from_file_location("conflux_graph_order_comparison", HELPER)
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    driver.DIAGNOSTIC = CANDIDATE
    driver.REQUIRED_SOURCES = tuple(dict.fromkeys(driver.REQUIRED_SOURCES + REQUIRED_EXPERIMENT_SOURCES))
    return driver


def require(value, message):
    if not value:
        raise RuntimeError(message)


def reject_diagnostic_environment():
    require(not any(key in os.environ for key in DEBUG_FLAGS)
            and not any(key.startswith(("GGML_VK_", "GGML_VULKAN_")) for key in os.environ),
            "Inherited GPU diagnostic/backend flags must be absent, including empty values")


def native_memory(process):
    """One owned-child RSS high-water read before close; never a global rusage."""
    row = {"pid": process.pid, "started_monotonic": time.monotonic(), "available": False,
           "VmHWM_KiB": None, "scope": "This native process peak resident memory; not GPU or graph-buffer memory."}
    try:
        require(process.returncode is None, "Native child was already reaped")
        proc = Path("/proc") / str(process.pid)
        first = (proc / "stat").read_text().rsplit(")", 1)[1].split()
        row["start_ticks"] = int(first[19])
        status = (proc / "status").read_text()
        last = (proc / "stat").read_text().rsplit(")", 1)[1].split()
        require(int(last[19]) == row["start_ticks"], "Native identity changed during memory read")
        match = re.search(r"^VmHWM:\s+([0-9]+)\s+kB$", status, re.MULTILINE)
        require(match is not None, "Native status has no VmHWM field")
        row.update(VmHWM_KiB=int(match[1]), available=True)
    except Exception as error:
        row["error"] = {"type": type(error).__name__, "message": str(error)}
    row["finished_monotonic"] = time.monotonic()
    return row


def graph_order_observation(driver, record, deadline):
    stderr = record["stderr"]
    require(stderr.get("truncated") is False and stderr["original_bytes"] == stderr["bytes"],
            "Complete candidate stderr is required for graph-order validation")
    pin, data = driver.inspect_file(Path(stderr["path"]), 128 * 1024, deadline,
                                    expected=stderr["sha256"], keep=True)
    lines = [line for line in data.splitlines() if b"conflux_graph_order" in line]
    require(1 <= len(lines) <= len(record["requests"]), "Missing or excessive graph-order finalization records")
    rows = []
    for line in lines:
        match = re.fullmatch(rb"conflux_graph_order nodes=([0-9]+) moved_positions=([0-9]+) graph_buffer_bytes=([0-9]+)", line)
        require(match is not None, "Malformed or unexpected graph-order stderr record")
        nodes, moved, allocated = (int(value) for value in match.groups())
        require(nodes > 0 and 0 < moved <= nodes and allocated > 0,
                "Graph-order candidate did not move nodes or allocate a graph buffer")
        rows.append({"nodes": nodes, "moved_positions": moved, "graph_buffer_bytes": allocated})
    return {"stderr_pin": pin, "records": rows, "record_count": len(rows),
            "requests_with_nonzero_rebuild_ms": sum(row["native_metrics"]["graph_rebuild_ms"] > 0 for row in record["requests"]),
            "scope": "One log per finalized Vulkan graph, not assumed one global graph. Native code enforces node/count/op invariants. Graph-buffer bytes have no measured baseline equivalent; no total GPU-memory or memory-pass claim."}


def main():
    driver = verified_driver()
    original_plan = driver.dry_plan
    def dry_plan():
        return {**original_plan(), "request_timeout_seconds": 30,
                "comparison_maximum_seconds": 600, "driver_after_pins_maximum_seconds": 60,
                "scope": "Plan only. Baseline16 then graph-order candidate16; exact PCM and observational per-child peak RSS/graph-buffer bytes. No UI/audio, profiler, latency or memory-pass threshold."}
    driver.dry_plan = dry_plan
    if "--run" in sys.argv:
        reject_diagnostic_environment()
        # Keep direct adapter invocation within the launcher's same bounds.
        bounded = argparse.ArgumentParser(add_help=False)
        bounded.add_argument("--max-seconds", type=float)
        bounded.add_argument("--request-timeout", type=float)
        limits, _ = bounded.parse_known_args()
        for name, value, default, minimum in (("max-seconds", limits.max_seconds, 600, 60),
                                               ("request-timeout", limits.request_timeout, 30, 5)):
            require(value is None or (math.isfinite(value) and minimum <= value <= default),
                    "Graph-order " + name + " exceeds the bounded comparison contract")
            if value is None:
                sys.argv.extend(("--" + name, str(default)))
    original_runtime = driver.run_runtime

    def run_runtime(label, executable, cases, baseline_hashes, np, NativeRuntime, args, output, report, deadline):
        class MemoryRuntime(NativeRuntime):
            def close(self):
                try:
                    record = report["runtimes"][-1]
                    if "native_memory_before_close" not in record:
                        record["native_memory_before_close"] = native_memory(self.process)
                finally:
                    super().close()
        original_runtime(label, executable, cases, baseline_hashes, np, MemoryRuntime,
                         args, output, report, deadline)
        if label == "diagnostic":
            report["runtimes"][-1]["graph_order"] = graph_order_observation(driver, report["runtimes"][-1], deadline)
            report["candidate_scope"] = "One-time Vulkan graph ordering before allocation; exact PCM check only. No profiler, UI/audio load, latency qualification or new memory threshold. Per-child peak RSS is observational."
            driver.checkpoint(output, report)
    driver.run_runtime = run_runtime
    return driver.main()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print("graph-order comparison admission failed: " + str(error), file=sys.stderr)
        raise SystemExit(1)

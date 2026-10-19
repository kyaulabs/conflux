#!/usr/bin/env python3
"""Verify completed Flash build metadata; never invoke a tool, build or runtime."""
from __future__ import annotations

import argparse
import ast
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import stat
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
BASELINE = Path("/tmp/conflux-stem-phase-timing-2026-10-18/build")
MODEL = Path("/tmp/conflux-stem-m0/models/htdemucs-f16.gguf")
MODEL_SHA = "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"
FROZEN_VERIFIER_SHA = "0515b98edbeca43c96ab0adeaed76ac853bfedf5e7e5a71377e041edad3def2a"
LOCAL = {
    "CMakeLists.txt": "d5fec7cbe05fa85784a1f1d68271e06294aac1de0f8e454947dea1d9a3adcf83",
    "FlashAttentionF32.cmake": "55ebc619f561ac1042489cdc02c1fa14db6c8e0b524e19f85080783abb865600",
    "GraphPhaseTiming.cmake": "91891a8a2dc01be4cb02a835d0c83f1b8b41a98da9059a36021c4a3b1e38d524",
    "BlockingVulkanWait.cmake": "4ae34a1fee27c3fec3b5a4b9884a0c86fe7395acabf9d7c2006345e56384ddb3",
    "htdemucs_segment.cpp": "a00c5c84bf57adeea7aa21eea992da4ee50d219fbc6994180de4dbf9fd03d7d9",
}
GENERATED = {
    "conflux-flash-f32/pipeline.cpp": "de537c7b01fb1041d9195f4a93721cbec4aad9537efb1bcd4bf3b3bcc4a56e0c",
    "conflux-flash-f32/ggml-vulkan.cpp": "e36ece6d9bd1219085c77e58e6f624da19c349fc1dd214469367d8824501f996",
    "conflux-graph-phase-timing/pipeline.cpp": "f06814211f00eab41487caacefe1c2492dfe1d2a0ddd6da1f90bdbcc566fc8f9",
    "conflux-graph-phase-timing/include/engine/models/demucs/pipeline.h": "7f48bbd4964ebcce721f7544a84303c568b460bf21c24090899ccc8b983833cd",
    "conflux-vulkan/ggml-vulkan.cpp": "55629ce73c55ab1d1513408c6276d9a0de6adfaac1531922fdb256aba5be0209",
}
BASELINE_GENERATED = {name: sha for name, sha in GENERATED.items() if not name.startswith("conflux-flash-f32/")}
SHARED_HEADER = "conflux-graph-phase-timing/include/engine/models/demucs/pipeline.h"
AFFECTED = {
    "runtime/htdemucs_segment.cpp",
    "upstream/src/models/demucs/assets.cpp",
    "upstream/src/models/demucs/frontend.cpp",
    "upstream/src/models/demucs/postprocess.cpp",
    "upstream/src/models/demucs/session.cpp",
    "upstream/src/framework/runtime/registry.cpp",
    "build/conflux-flash-f32/pipeline.cpp",
}
SOURCE_REPLACEMENTS = {
    "build/conflux-graph-phase-timing/pipeline.cpp": "build/conflux-flash-f32/pipeline.cpp",
    "build/conflux-vulkan/ggml-vulkan.cpp": "build/conflux-flash-f32/ggml-vulkan.cpp",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def file_record(path: Path, expected: str | None = None, maximum: int = 256 * 1024 * 1024) -> dict:
    path = path.resolve(strict=True)
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode) and 0 <= before.st_size <= maximum,
                f"Input is not a bounded regular file: {path}")
        digest = hashlib.sha256()
        left = before.st_size
        while left:
            block = stream.read(min(left, 1024 * 1024))
            require(bool(block), f"Input shortened during hashing: {path}")
            digest.update(block)
            left -= len(block)
        after = os.fstat(stream.fileno())
        identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)
        require(identity(before) == identity(after), f"Input changed during hashing: {path}")
    actual = digest.hexdigest()
    require(expected is None or actual == expected, f"Input hash mismatch: {path}")
    return {"path": str(path), "bytes": before.st_size, "sha256": actual}


def read_text(path: Path, maximum: int = 64 * 1024 * 1024) -> str:
    require(path.stat().st_size <= maximum, f"Text input exceeds limit: {path}")
    return path.read_text(encoding="utf-8")


def cache_values(path: Path) -> dict[str, str]:
    values = {}
    for line in read_text(path).splitlines():
        match = re.fullmatch(r"([^/#][^:]*):[^=]*=(.*)", line)
        if match:
            values[match[1]] = match[2]
    return values


def resolved(value: str, directory: Path) -> Path:
    path = Path(value)
    return (path if path.is_absolute() else directory / path).resolve()


def source_key(path: Path, build: Path, logical_build: Path, upstream: Path, runtime: Path) -> str:
    # Preserve the compile database's build-relative spelling before resolving
    # symlinks. The frozen baseline moved its generated ggml subtree behind a
    # build-directory symlink; its logical source mapping remains unchanged.
    if path.is_relative_to(logical_build):
        return f"build/{path.relative_to(logical_build).as_posix()}"
    path = path.resolve()
    for root, label in ((build, "build"), (upstream, "upstream"), (runtime, "runtime")):
        if path.is_relative_to(root):
            return f"{label}/{path.relative_to(root).as_posix()}"
    return str(path)


def compile_plan(build: Path, upstream: Path, runtime: Path, cache: dict[str, str]) -> dict:
    rows = json.loads(read_text(build / "compile_commands.json"))
    require(isinstance(rows, list) and 1 <= len(rows) <= 10000, "Compile database has invalid geometry")
    logical_build = Path(os.path.abspath(cache["CMAKE_CACHEFILE_DIR"]))
    require(logical_build.resolve() == build, "Cached logical build directory points to a different build")
    counts: Counter[str] = Counter()
    forced = {}
    compiler_paths = set()
    for row in rows:
        require(isinstance(row, dict), "Invalid compile database row")
        directory = Path(os.path.abspath(row["directory"]))
        source = Path(row["file"])
        source = Path(os.path.abspath(source if source.is_absolute() else directory / source))
        key = source_key(source, build, logical_build, upstream, runtime)
        counts[key] += 1
        args = row.get("arguments")
        args = shlex.split(row["command"]) if args is None else args
        require(isinstance(args, list) and args and all(isinstance(arg, str) for arg in args),
                "Invalid compiler arguments")
        compiler = resolved(args[0], directory)
        require(compiler.is_absolute() and compiler.is_file(), "Compiler executable is unavailable")
        compiler_paths.add(str(compiler))
        includes = []
        for index, argument in enumerate(args):
            if argument == "-include":
                require(index + 1 < len(args), "Incomplete forced include")
                includes.append(str(resolved(args[index + 1], directory)))
            elif argument.startswith("-include") and argument != "-include":
                includes.append(str(resolved(argument[len("-include"):], directory)))
        if includes:
            require(key not in forced, f"Repeated forced-header translation unit: {key}")
            forced[key] = includes
    return {"entries": len(rows), "unique_sources": len(counts),
            "source_counts": dict(sorted(counts.items())),
            "forced_includes": dict(sorted(forced.items())), "compilers": sorted(compiler_paths)}


def upstream_expected() -> dict[str, str]:
    path = HERE / "verify_flash_f32_overlay.py"
    file_record(path, FROZEN_VERIFIER_SHA)
    tree = ast.parse(read_text(path))
    assignments = [node for node in tree.body if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == "UPSTREAM_PINS" for target in node.targets)]
    require(len(assignments) == 1, "Frozen upstream pin table is missing or ambiguous")
    result = ast.literal_eval(assignments[0].value)
    require(isinstance(result, dict) and len(result) == 12, "Frozen upstream pin geometry changed")
    return result


def inspect(args: argparse.Namespace) -> dict:
    build = args.build_dir.resolve(strict=True)
    baseline = args.baseline_build.resolve(strict=True)
    cache = cache_values(build / "CMakeCache.txt")
    baseline_cache = cache_values(baseline / "CMakeCache.txt")
    upstream = Path(cache["AUDIOCPP_SOURCE_DIR"]).resolve(strict=True)
    runtime = Path(cache["CMAKE_HOME_DIRECTORY"]).resolve(strict=True)
    baseline_upstream = Path(baseline_cache["AUDIOCPP_SOURCE_DIR"]).resolve(strict=True)
    baseline_runtime = Path(baseline_cache["CMAKE_HOME_DIRECTORY"]).resolve(strict=True)
    require(runtime == REPO / "tools/runtime", "Candidate was not configured from the tracked runtime directory")
    require(upstream == baseline_upstream, "Candidate and baseline use different upstream checkouts")
    required = {"CONFLUX_STEM_FLASH_F32": "ON", "CONFLUX_STEM_BLOCKING_WAIT": "ON",
        "CMAKE_BUILD_TYPE": "Release", "CMAKE_EXPORT_COMPILE_COMMANDS": "ON", "GGML_NATIVE": "ON",
        "ENGINE_ENABLE_VULKAN": "ON", "AUDIOCPP_MODEL_SET": "custom", "AUDIOCPP_MODELS": "demucs",
        "AUDIOCPP_DEPLOYMENT_BUILD": "ON", "AUDIOCPP_BUILD_C_API": "OFF",
        "AUDIOCPP_BUILD_NATIVE_MODEL_MANAGER": "OFF", "ENGINE_BUILD_TESTS": "OFF", "ENGINE_BUILD_EXAMPLES": "OFF"}
    for key, value in required.items():
        require(cache.get(key) == value, f"Unexpected configured {key}: {cache.get(key)!r}")
    worktree = {f"tools/runtime/{name}": file_record(runtime / name, expected) for name, expected in LOCAL.items()}
    worktree["tools/stems/README.md"] = file_record(REPO / "tools/stems/README.md")
    sources = {name: file_record(upstream / name, expected) for name, expected in upstream_expected().items()}
    sources["CMakeLists.txt"] = file_record(upstream / "CMakeLists.txt",
        "5970918d431172b3ae12e0990a003fb87298c44b8e1849ec4e1c68f5301e9d34")
    for key in AFFECTED:
        if key.startswith("upstream/"):
            relative = key[len("upstream/"):]
            sources[relative] = file_record(upstream / relative)
    generated = {name: file_record(build / name, expected) for name, expected in GENERATED.items()}
    baseline_generated = {name: file_record(baseline / name, expected) for name, expected in BASELINE_GENERATED.items()}
    candidate_plan = compile_plan(build, upstream, runtime, cache)
    baseline_plan = compile_plan(baseline, baseline_upstream, baseline_runtime, baseline_cache)
    expected_counts = Counter()
    for key, count in baseline_plan["source_counts"].items():
        expected_counts[SOURCE_REPLACEMENTS.get(key, key)] += count
    require(candidate_plan["source_counts"] == dict(expected_counts),
            "Compile source multiset differs beyond the two intended replacements")
    require(all(baseline_plan["source_counts"].get(old) == 1 and
                candidate_plan["source_counts"].get(new) == 1 for old, new in SOURCE_REPLACEMENTS.items()),
            "Expected source replacements are missing, repeated or already present in baseline")
    require(set(candidate_plan["forced_includes"]) == AFFECTED, "Forced phase header does not cover exactly seven translation units")
    header = str((build / SHARED_HEADER).resolve())
    require(all(values == [header] for values in candidate_plan["forced_includes"].values()),
            "Affected translation units do not share exactly the generated phase header")
    inverse = {new: old for old, new in SOURCE_REPLACEMENTS.items()}
    expected_baseline_affected = {inverse.get(key, key) for key in AFFECTED}
    require(set(baseline_plan["forced_includes"]) == expected_baseline_affected,
            "Baseline affected translation units differ")
    require(all(values == [str((baseline / SHARED_HEADER).resolve())]
                for values in baseline_plan["forced_includes"].values()), "Baseline forced header differs")
    model = file_record(args.model, MODEL_SHA)
    adapter = read_text(runtime / "htdemucs_segment.cpp")
    require(adapter.count("context.uses_host_graph_plan() ? engine::assets::TensorStorageType::F32") == 1
        and adapter.count(": engine::assets::TensorStorageType::Native") == 1,
        "Vulkan native weight-storage selection source changed")
    config_files = {"cache": file_record(build / "CMakeCache.txt"),
        "compile_commands": file_record(build / "compile_commands.json"),
        "baseline_cache": file_record(baseline / "CMakeCache.txt"),
        "baseline_compile_commands": file_record(baseline / "compile_commands.json")}
    tools = {}
    for key in ("CMAKE_COMMAND", "CMAKE_MAKE_PROGRAM", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "Vulkan_GLSLC_EXECUTABLE"):
        if cache.get(key) and not cache[key].endswith("-NOTFOUND"):
            tools[key] = file_record(Path(cache[key]))
    for compiler in candidate_plan["compilers"]:
        tools[f"compile_database:{compiler}"] = file_record(Path(compiler))
    require(all(key in tools for key in ("CMAKE_COMMAND", "CMAKE_MAKE_PROGRAM", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER")),
            "Required configured tools are missing")
    for language in ("C", "CXX"):
        files = list((build / "CMakeFiles").glob(f"*/CMake{language}Compiler.cmake"))
        require(len(files) == 1, f"Ambiguous {language} compiler metadata")
        config_files[f"compiler_{language}"] = file_record(files[0])
    spirv_root = Path(cache["SPIRV_HEADERS_INCLUDE_DIR"])
    headers = [spirv_root / name for name in ("spirv/unified1/spirv.hpp", "spirv-headers/spirv.hpp", "spirv.hpp")]
    spirv = next((path for path in headers if path.is_file()), None)
    require(spirv is not None, "Configured SPIR-V header is unavailable")
    tools["SPIRV_header"] = file_record(spirv,
        "f1c7707b4fa073bb61706eecf3583163651504a3abac17f3e3f2420313082aac")
    receipt_path = args.configure_receipt.resolve(strict=True)
    checks = json.loads(read_text(receipt_path))
    require(checks.get("status") == "passed" and checks.get("overlay_sha256") == LOCAL["FlashAttentionF32.cmake"]
        and len(checks.get("checks", [])) == 13 and all(row.get("passed") is True for row in checks["checks"]),
        "Configure-only receipt does not prove the thirteen current module checks")
    require(checks.get("generated_sha256") == {"pipeline.cpp": GENERATED["conflux-flash-f32/pipeline.cpp"],
        "ggml-vulkan.cpp": GENERATED["conflux-flash-f32/ggml-vulkan.cpp"]}, "Configure receipt generated sources differ")
    return {"build_directory": str(build), "baseline_build_directory": str(baseline),
        "configured_options": {key: cache[key] for key in required},
        "worktree_inputs": worktree, "upstream_pinned_and_affected_sources": sources,
        "generated_inputs": generated, "baseline_generated_inputs": baseline_generated,
        "model": model, "config_inputs": config_files, "tools_and_header": tools,
        "configure_check_receipt": file_record(receipt_path),
        "compile_plan": candidate_plan, "baseline_compile_plan": baseline_plan,
        "intended_source_replacements": SOURCE_REPLACEMENTS,
        "weight_storage_source": "unchanged native storage for Vulkan; actual runtime selection not executed",
        "full_f32_scalar_compilation_source": True, "graph_order_overlay": False}


def build_receipt(path: Path, build: Path) -> dict:
    receipt = json.loads(read_text(path))
    item = receipt.get("build", receipt)
    require(isinstance(item, dict) and item.get("exit", item.get("exit_code")) == 0,
            "Root build receipt does not report exit0")
    command = item.get("command")
    require(isinstance(command, list) and all(isinstance(part, str) for part in command), "Missing root build command")
    require("--build" in command and "--target" in command, "Root receipt is not a target build")
    index = command.index("--build")
    target = command.index("--target")
    require(index + 1 < len(command) and Path(command[index + 1]).resolve() == build
        and target + 1 < len(command) and command[target + 1] == "conflux-stem-runtime",
        "Root build receipt points to a different build or target")
    return {"file": file_record(path), "command": command, "exit_code": 0,
            "scope": "Root-supplied execution record, not independently executed by this metadata helper"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", choices=("configured", "built"))
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--baseline-build", type=Path, default=BASELINE)
    parser.add_argument("--model", type=Path, default=MODEL)
    parser.add_argument("--configure-receipt", type=Path)
    parser.add_argument("--repository-base-head")
    parser.add_argument("--before", type=Path)
    parser.add_argument("--build-receipt", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.stage is None:
        print(json.dumps({"status": "dry_plan", "input_contents_read": False,
            "scope": "completed build metadata and file hashes only; no subprocesses or runtime",
            "stages": ["configured", "built"], "worktree_validation_before_final_commit": True}))
        return 0
    for name in ("build_dir", "configure_receipt", "repository_base_head", "output"):
        if getattr(args, name) is None:
            parser.error(f"--stage requires --{name.replace('_', '-')}")
    if not re.fullmatch(r"[0-9a-f]{40}", args.repository_base_head):
        parser.error("--repository-base-head must be a full root-verified SHA; helper does not run Git")
    if args.stage == "built" and (args.before is None or args.build_receipt is None):
        parser.error("--stage built requires --before and --build-receipt")
    require(not args.output.exists() and not args.output.is_symlink(), "Use a fresh receipt output path")
    record = {"schema": 1, "status": "incomplete", "stage": args.stage,
        "repository_base_head": args.repository_base_head,
        "repository_head_verification": "caller supplied; no Git queried by this helper",
        "worktree_validation": "promoted inputs are currently uncommitted; base HEAD does not contain them",
        "final_source_commit_binding": "pending root commit of these exact worktree bytes",
        "verifier": file_record(Path(__file__)), "subprocesses_started": False,
        "build_executed_by_helper": False, "native_execution": False,
        "numerical_quality_or_latency_claim": False}
    started = time.monotonic()
    try:
        record["inspection"] = inspect(args)
        if args.stage == "built":
            before = json.loads(read_text(args.before))
            require(before.get("status") == "configured_metadata_verified" and before.get("stage") == "configured",
                    "Before receipt is not a successful configured-stage verification")
            require(before.get("repository_base_head") == args.repository_base_head,
                    "Repository base changed between configured and built verification")
            require(before.get("verifier") == record["verifier"], "Metadata helper changed between stages")
            require(before.get("inspection") == record["inspection"],
                    "Source/model/tool/configuration/compile mapping pins changed during build")
            record["before_receipt"] = file_record(args.before)
            record["build_receipt"] = build_receipt(args.build_receipt, args.build_dir.resolve())
            record["runtime"] = file_record(args.build_dir / "conflux-stem-runtime")
            record["before_after_inputs_unchanged"] = True
            record["status"] = "built_metadata_verified_native_execution_pending"
        else:
            record["status"] = "configured_metadata_verified"
    except Exception as error:
        record["status"] = "failed"
        record["error"] = f"{type(error).__name__}: {error}"
    record["elapsed_seconds"] = time.monotonic() - started
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(record, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps(record, allow_nan=False))
    return 1 if record["status"] == "failed" else 0


if __name__ == "__main__":
    raise SystemExit(main())

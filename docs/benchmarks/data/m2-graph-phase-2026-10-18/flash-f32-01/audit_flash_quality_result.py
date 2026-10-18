#!/usr/bin/env python3
"""Audit closed Flash comparison metadata; never open PCM, models or binaries."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

BASE = Path(__file__).resolve().parent
HEAD = "aee5c42cd254fd23c8e2bff29b9d67e8d5c12ee0"
BASELINE = "ded9cb2c1000af78917a784e281d905da201d735338221519958d9d7df1d681f"
CANDIDATE = "6f311e461e7af5f12ff38f90ddb8720179c2c3dce23d593437d2b34524dffd9d"
MODEL = "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"
PROOF = "38c60301feed297a0484d61ea92cca2413067d15d7ead7e9a27e6cc9909037e3"
FROZEN = {
    "flash_quality.py": "cec1a694a32da562be9ed79d35cfda7e2ee1943b9b7dfd10daf1ed2259fc7495",
    "flash_quality_launch.py": "ab912a468791b13ca283bacd84ce30163ed1c7a0576fd099dc859fd4670294c4",
    "compare_runtime.py": "5022aeed8eaf932adead05404f80b94c7d992f4b6f68dc81d05edb6126077e5b",
    "flash_f32_overlay.cmake": "a4951aadce8272b103e31a12c121c237ca8545b926690ebcc60708d979e7eb15",
}
TRACKS = ("club_xl", "when_the_light_breaks", "under_my_skin", "stfu")
STARTS, WINDOW, TOTAL_FRAMES = (0, 257985, 515970, 773955), 343980, 1031940
STEMS, CHANNELS = ("drums", "bass", "other", "vocals"), ("left", "right")
TIMES = ("elapsed_ms", "frontend_ms", "graph_ms", "postprocess_ms", "graph_rebuild_ms",
         "inference_wall_ms", "inference_cpu_ms", "graph_upload_ms", "graph_compute_ms", "graph_readback_ms")
COUNTS = ("voluntary_context_switches", "involuntary_context_switches")
OUTPUT_BYTES, SPOOL_BYTES = 4 * WINDOW * 2 * 4, 16 * 4 * WINDOW * 2 * 4
HEX = re.compile(r"[0-9a-f]{64}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path, help="Closed launcher output directory")
    parser.add_argument("--output", type=Path, help="New audit receipt, outside the raw run")
    args = parser.parse_args()
    run = args.run.resolve()
    destination = args.output or BASE / (run.name + "-independent-audit.json")
    receipt = {"schema": 1, "status": "incomplete", "run": str(run),
               "checks": [], "files_read": {}, "errors": [],
               "scope": "Independent bounded metadata/hash/internal-consistency audit only."}

    def check(name, condition):
        if not condition:
            raise ValueError(name)
        receipt["checks"].append(name)

    def read(path, maximum, pin=None):
        path = Path(path)
        with path.open("rb") as stream:
            raw = stream.read(maximum + 1)
        check("Bounded metadata/source read: " + str(path), len(raw) <= maximum)
        actual = {"path": str(path.resolve()), "bytes": len(raw),
                  "sha256": hashlib.sha256(raw).hexdigest()}
        if pin is not None:
            check("Retained bytes/hash binding: " + str(path),
                  actual["sha256"] == pin["sha256"] and actual["bytes"] == pin["bytes"])
        receipt["files_read"][str(path.resolve())] = actual
        return raw

    def decode(raw):
        def unique(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError("Duplicate JSON key: " + key)
                result[key] = value
            return result
        def invalid(value):
            raise ValueError("Nonfinite JSON constant: " + value)
        return json.loads(raw, object_pairs_hook=unique, parse_constant=invalid)

    def load(path, maximum, pin=None):
        return decode(read(path, maximum, pin))

    def nonnegative(value):
        return type(value) in (int, float) and math.isfinite(value) and value >= 0

    try:
        state = load(run / "session.json", 2 * 1024 * 1024)
        report = load(run / "comparison/result.json", 8 * 1024 * 1024, state["result_pin"])
        check("Complete closed launcher and numerical report",
              state["status"] == report["status"] == "comparison_complete"
              and state["errors"] == report["errors"] == []
              and state["driver_started"] is True and state["native_started"] is True
              and report["native_started"] is True and state["driver_returncode"] == 0
              and state["reported_status"] == report["status"]
              and state["completed_requests"] == report["completed_requests"] == 32)
        check("Fixed 600-second launcher budget was retained",
              state["maximum_total_seconds"] == 600
              and math.isclose(state["deadline_monotonic"] - state["started_monotonic"], 600, abs_tol=1e-6)
              and nonnegative(state["elapsed_seconds"]) and state["elapsed_seconds"] < 600)
        check("No workload or policy qualification is represented",
              all(state[key] is False for key in ("profile_mutation", "service_mutation", "UI_load", "audio_load"))
              and all(value["musical_quality"] == "not_assessed"
                      and value["latency_qualification"] == "not_attempted" for value in (state, report))
              and report["metric_domain"] ==
              "Native normalized stem PCM, before denormalization/shared listening gain/OLA.")
        check("Complete parent process and private-spool cleanup was recorded",
              all(state[key] is True for key in ("process_cleanup_complete", "spool_removed", "cleanup_complete"))
              and state["cleanup"]["remaining_observed"] == {}
              and state["spool"] == report["spool"]
              and Path(state["spool"]).parent == BASE / "native-temp"
              and Path(state["spool"]).name.startswith("flash-quality-")
              and state["baseline_spool_expected_bytes"] == report["baseline_spool_expected_bytes"] == SPOOL_BYTES
              and report["baseline_spool_peak_bytes"] == SPOOL_BYTES
              and report["baseline_spool_removed"] is True and report["cleanup_complete"] is True)
        check("Owned new-session group and captured PID identities",
              type(state["owned_group"]) is int and state["owned_group"] > 1
              and state["cleanup"]["owned_identities"]
              and all(int(pid) > 1 and type(identity["start_ticks"]) is int and identity["start_ticks"] > 0
                      and type(identity["group"]) is int and identity["group"] > 1
                      for pid, identity in state["cleanup"]["owned_identities"].items()))
        check("Before/after recorded identities are unchanged",
              state["pins_before"] == state["pins_after"]
              and report["pins_before"] == report["pins_after"] and report["pins_unchanged"] is True)
        config = load(state["pins_before"]["config"]["path"], 64 * 1024, state["pins_before"]["config"])
        sources = load(state["pins_before"]["sources"]["path"], 64 * 1024, state["pins_before"]["sources"])
        pins = report["pins_before"]
        check("Armed configuration and exact artifact identities",
              config["status"] == "ready" and config["repository_head"] == HEAD
              and config["baseline"]["sha256"] == pins["baseline"]["sha256"] == BASELINE
              and config["candidate"]["sha256"] == pins["candidate"]["sha256"] == CANDIDATE
              and config["baseline"]["path"] == pins["baseline"]["path"]
              and config["candidate"]["path"] == pins["candidate"]["path"]
              and config["model_sha256"] == pins["model"]["sha256"] == MODEL
              and config["proof_sha256"] == pins["proof"]["sha256"] == PROOF
              and config["overlay_sha256"] == FROZEN["flash_f32_overlay.cmake"]
              and config["source_pins_sha256"] == state["pins_before"]["sources"]["sha256"])
        check("Parent and driver configuration records agree",
              pins["configuration"] == state["pins_before"]["config"]
              and pins["source_configuration"] == state["pins_before"]["sources"])
        check("All source-manifest entries have matching recorded source pins",
              15 <= len(sources) <= 64
              and all(Path(path).is_absolute() and HEX.fullmatch(digest)
                      and pins["source:" + path]["path"] == path
                      and pins["source:" + path]["sha256"] == digest for path, digest in sources.items())
              and {key[7:] for key in pins if key.startswith("source:")} == set(sources))
        for name, digest in FROZEN.items():
            path = BASE / name
            check("Exact helper/overlay source identity: " + name,
                  sources[str(path)] == digest and hashlib.sha256(read(path, 128 * 1024)).hexdigest() == digest)
        check("Parent exact driver and launcher pins",
              state["pins_before"]["driver"] == pins["source:" + str(BASE / "flash_quality.py")]
              and state["pins_before"]["launcher"] == pins["source:" + str(BASE / "flash_quality_launch.py")])
        build_pin = pins["build_verification"]
        build = load(build_pin["path"], 2 * 1024 * 1024, build_pin)
        pipeline = "/tmp/conflux-stem-flash-f32-2026-10-18/build/conflux-flash-f32/"
        check("Build proof binds the isolated full-F32 scalar attention candidate",
              config["build_verification"] == {"path": build_pin["path"], "sha256": build_pin["sha256"]}
              and build["status"] == "built" and build["runtime_sha256"] == CANDIDATE
              and build["overlay_sha256"] == config["overlay_sha256"]
              and build["full_f32_scalar"] is True and build["graph_order"] is False
              and build["generated_pipeline_sha256"] == sources[pipeline + "pipeline.cpp"]
              and build["generated_vulkan_sha256"] == sources[pipeline + "ggml-vulkan.cpp"])
        proof = load(pins["proof"]["path"], 2 * 1024 * 1024, pins["proof"])
        check("Immutable paired-input and phase-A PCM proof",
              proof["status"] == "passed" and all(proof[key] is True for key in
              ("exact_pcm_match", "input_sha_match", "phase_fields_valid", "pins_unchanged", "cleanup_complete"))
              and proof["completed_cases_per_runtime"] == {"baseline": 16, "diagnostic": 16}
              and proof["pins_before"]["diagnostic"]["sha256"] == BASELINE
              and proof["pins_before"]["model"]["sha256"] == MODEL)
        command = state["command"]
        def option(name):
            return command[command.index(name) + 1]
        check("Child command retains the exact deadline, private spool and configuration",
              "--run" in command and str(BASE / "flash_quality.py") in command
              and option("--output") == str(run / "comparison")
              and option("--spool") == state["spool"]
              and option("--expected-pins") == pins["configuration"]["path"]
              and option("--source-pins") == pins["source_configuration"]["path"]
              and float(option("--deadline-monotonic")) == state["deadline_monotonic"])
        raw_monitor = read(run / "AC-lid-monitor.jsonl", 1024 * 1024)
        observations = [decode(line) for line in raw_monitor.splitlines() if line.strip()]
        check("Sampled AC connected and lid open before/through/after the comparison",
              len(observations) >= 2 and all(row["ready"] is True and row["errors"] == []
                  and row["AC"] == "1" and row["lid"] == "b false"
                  and state["started_monotonic"] <= row["capture_started_monotonic"]
                  <= row["timestamp_monotonic"] <= state["started_monotonic"] + state["elapsed_seconds"]
                  for row in observations)
              and all(a["timestamp_monotonic"] <= b["capture_started_monotonic"]
                      for a, b in zip(observations, observations[1:])))
        receipt["AC_lid_observations"] = {
            "rows": len(observations),
            "row_timestamp_span_seconds": observations[-1]["timestamp_monotonic"] - observations[0]["timestamp_monotonic"],
            "maximum_row_timestamp_gap_seconds": max(b["timestamp_monotonic"] - a["timestamp_monotonic"]
                                                     for a, b in zip(observations, observations[1:])),
            "head_gap_seconds": observations[0]["capture_started_monotonic"] - state["started_monotonic"],
            "tail_gap_seconds": state["started_monotonic"] + state["elapsed_seconds"] - observations[-1]["timestamp_monotonic"],
            "scope": "Discrete AC/lid observations, not uninterrupted policy, thermal or minimized-window proof."}
        runtimes = report["runtimes"]
        ids = [f"{track}:{start}" for track in TRACKS for start in STARTS]
        check("Exactly sixteen requests in each fixed-order runtime",
              [runtime["label"] for runtime in runtimes] == ["baseline", "flash"]
              and all([row["id"] for row in runtime["requests"]] == ids for runtime in runtimes)
              and [row["id"] for row in report["cases"]] == ids)
        immutable = {row["id"]: row for row in proof["runtimes"][1]["requests"]}
        exact_cases = []
        stderr_text = {}
        for runtime in runtimes:
            label = runtime["label"]
            ready = runtime["ready"]
            check(label + " native readiness and cleanup",
                  runtime["executable"] == pins["baseline" if label == "baseline" else "candidate"]["path"]
                  and type(runtime["pid"]) is int and runtime["pid"] > 1
                  and ready["pid"] == runtime["pid"] and ready["backend"] == "vulkan" and ready["threads"] == 2
                  and ready["revision"] == "0097d365a00b52a6991ad7618f0a04e8d2935e47"
                  and ready["model"] == pins["model"]["path"] and ready["weight_storage"] == "native"
                  and ready["fence_wait_mode"] == "blocking"
                  and ready["process_environment"] == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"}
                  and all(runtime["cleanup"][key] is True for key in
                          ("complete", "reaped", "temporary_directory_removed")))
            stderr = runtime["stderr"]
            raw = read(run / "comparison" / (label + ".stderr"), 128 * 1024, stderr)
            check(label + " complete stderr without graph ordering",
                  stderr["truncated"] is False and stderr["original_bytes"] == len(raw)
                  and b"conflux_graph_order" not in raw)
            stderr_text[label] = raw.decode()
        for index, (baseline, flash) in enumerate(zip(runtimes[0]["requests"], runtimes[1]["requests"])):
            expected = immutable[ids[index]]
            identity = ("id", "input_sha256", "normalization", "shape", "dtype", "valid_frames")
            check("Paired immutable input and complete outputs: " + ids[index],
                  all(baseline[key] == flash[key] == expected[key] for key in identity)
                  and baseline["matches_immutable_proof"] is True
                  and baseline["output_sha256"] == flash["baseline_output_sha256"] == expected["output_sha256"]
                  and all(row["request_id"] == index + 1 and row["completed"] is True
                          and row["finite"] is True and row["input_sha_match"] is True
                          and row["output_shape"] == [4, WINDOW, 2] and row["output_dtype"] == "<f4"
                          and row["output_bytes"] == OUTPUT_BYTES and HEX.fullmatch(row["output_sha256"])
                          and all(nonnegative(row["native_metrics"][key]) for key in TIMES)
                          and all(type(row["native_metrics"][key]) is int and row["native_metrics"][key] >= 0 for key in COUNTS)
                          for row in (baseline, flash)))
            exact = baseline["output_sha256"] == flash["output_sha256"]
            check("Per-case bit-identity flag is derived from hashes: " + ids[index], flash["exact_pcm_match"] is exact)
            if exact:
                exact_cases.append(ids[index])
            valid = min(WINDOW, TOTAL_FRAMES - STARTS[index % 4])
            for region, frames in (("whole", WINDOW), ("valid", valid), ("padding", WINDOW - valid)):
                value = flash["difference"][region]
                check("Region and channel numerical scope: " + ids[index] + ":" + region,
                      value["frames"] == frames and (value == {"frames": 0, "metrics": None, "status": "not_applicable"}
                      if frames == 0 else set(value["metrics"]) == set(STEMS)
                      and all(set(value["metrics"][stem]) == set(CHANNELS)
                              and all(metric["samples"] == frames
                                      and all(nonnegative(metric[key]) for key in ("rmse", "reference_rms", "max_absolute_error"))
                                      and metric["rmse"] <= metric["max_absolute_error"] * (1 + 1e-12)
                                      and (not exact or metric["rmse"] == metric["max_absolute_error"] == 0)
                                      for metric in value["metrics"][stem].values()) for stem in STEMS)))
        check("Structural completion does not imply numerical equivalence",
              report["exact_pcm_match"] is (len(exact_cases) == 16)
              and all(report[key] is True for key in ("input_sha_match", "all_outputs_finite", "phase_fields_valid")))
        evidence = report["flash_evidence"]
        check("Full Flash stderr bytes are bound to the retained selection evidence",
              evidence["stderr_pin"]["sha256"] == runtimes[1]["stderr"]["sha256"]
              and evidence["stderr_pin"]["bytes"] == runtimes[1]["stderr"]["bytes"])
        node_re = re.compile(r"conflux_flash_f32_node index=(\d+) q=64,(1344|2688),8,1 k=64,(1344|2688),8,1 v=64,(1344|2688),8,1 out=64,8,(1344|2688),1 scale=0.125 precision=f32 supported=1")
        pipe_re = re.compile(r"conflux_flash_f32_pipeline shader=flash_attn_f32_f16_fp32 path=scalar intermediate_bytes=4 accumulation_bytes=4 hsk=64 hsv=64 Br=(\d+) Bc=(\d+) workgroup=(\d+) subgroup=(\d+) k=f32 v=f32")
        graph_re = re.compile(r"conflux_flash_f32_graph nodes=(\d+) flash_nodes=10 self1344=3 self2688=3 cross2688to1344=2 cross1344to2688=2")
        indices, signatures, pipelines, graphs = [], [0, 0, 0, 0], [], []
        for line in stderr_text["flash"].splitlines():
            if not line.startswith("conflux_flash_"):
                continue
            node, pipe, graph = node_re.fullmatch(line), pipe_re.fullmatch(line), graph_re.fullmatch(line)
            if node:
                index, q, k, v, out = map(int, node.groups())
                check("Unique supported F32 Flash node: " + str(index), index not in indices and k == v and q == out)
                indices.append(index)
                signatures[(0 if q == 1344 else 1) if q == k else (2 if q == 2688 else 3)] += 1
            elif pipe:
                br, bc, workgroup, subgroup = map(int, pipe.groups())
                check("Positive scalar pipeline dimensions", min(br, bc, workgroup) > 0
                      and (subgroup == 0 or subgroup & (subgroup - 1) == 0))
                pipelines.append(dict(Br=br, Bc=bc, workgroup=workgroup, subgroup=subgroup))
            elif graph:
                graphs.append(int(graph[1]))
            else:
                raise ValueError("Unexpected Flash evidence: " + line)
        check("Ten-node geometry and full-F32 scalar pipeline records agree",
              len(indices) == 10 and signatures == [3, 3, 2, 2] and len(graphs) == 1 and graphs[0] > 0
              and 1 <= len(pipelines) <= 16 and sorted(indices) == evidence["node_indices"]
              and signatures == evidence["signature_counts"] and graphs[0] == evidence["graph_nodes"]
              and pipelines == evidence["pipelines"]
              and "conflux_flash_" not in stderr_text["baseline"])
        receipt.update(status="evidence_consistent", completed_requests=32, source_manifest_entries=len(sources),
                       recorded_artifact_pins=len(pins), repository_head=HEAD, exact_pcm_case_count=len(exact_cases),
                       exact_pcm_case_ids=exact_cases, elapsed_seconds=state["elapsed_seconds"],
                       numerical_acceptance="not_assessed", musical_quality="not_assessed",
                       latency_qualification="not_attempted",
                       limitations=[
                           "Raw PCM, model and executable bytes were not reopened; retained input/output hashes and producer validation were checked.",
                           "All source-manifest entries were matched to recorded before/after pins; only the four frozen helper/overlay source files were independently reopened.",
                           "Repository HEAD and process/spool removal were validated from retained reports, without Git or live host queries.",
                           "Numerical region shapes, finite RMS/maxima and exact-hash implications were checked; the separate numerical summary checks energies/aggregation.",
                           "F32 pipeline evidence concerns the attention path, not every model operation; changed reduction order may change PCM.",
                           "Overlapping normalized windows precede denormalization and overlap-add; no musical, physical audio, cancellation or latency acceptance follows."])
    except Exception as error:
        receipt["status"] = "inconsistent_or_incomplete"
        receipt["errors"].append({"type": type(error).__name__, "message": str(error)})
    receipt["check_count"] = len(receipt["checks"])
    receipt["auditor_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    with destination.open("x") as stream:
        json.dump(receipt, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"status": receipt["status"], "checks": receipt["check_count"], "receipt": str(destination)}))
    return 0 if receipt["status"] == "evidence_consistent" else 1


if __name__ == "__main__":
    raise SystemExit(main())

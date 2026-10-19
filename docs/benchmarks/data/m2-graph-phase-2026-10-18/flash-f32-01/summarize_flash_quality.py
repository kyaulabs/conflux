#!/usr/bin/env python3
"""Reduce a closed Flash PCM comparison; no media/model/native/host access."""
import argparse
import hashlib
import json
import math
from pathlib import Path

BASE = Path(__file__).resolve().parent
STEMS, CHANNELS = ("drums", "bass", "other", "vocals"), ("left", "right")
TRACKS = ("club_xl", "when_the_light_breaks", "under_my_skin", "stfu")
STARTS, WINDOW, FRAMES = (0, 257985, 515970, 773955), 343980, 1031940
CANDIDATE_SHA = "6f311e461e7af5f12ff38f90ddb8720179c2c3dce23d593437d2b34524dffd9d"
BASELINE_SHA = "ded9cb2c1000af78917a784e281d905da201d735338221519958d9d7df1d681f"
MODEL_SHA = "f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6"
PROOF_SHA = "38c60301feed297a0484d61ea92cca2413067d15d7ead7e9a27e6cc9909037e3"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    require(type(value) in (int, float) and math.isfinite(value) and value >= 0, "Invalid nonnegative metric")
    return value


def energies(metric, frames):
    require(metric["samples"] == frames and frames > 0, "Channel sample count differs from region")
    rmse, ref = number(metric["rmse"]), number(metric["reference_rms"])
    maximum = number(metric["max_absolute_error"])
    require(rmse <= maximum * (1 + 1e-12), "RMSE exceeds maximum absolute error")
    return frames * rmse * rmse, frames * ref * ref


def aggregate(rows):
    count = sum(row["samples"] for row in rows)
    require(count > 0, "No applicable samples to aggregate")
    error = math.fsum(row["samples"] * row["rmse"] ** 2 for row in rows)
    reference = math.fsum(row["samples"] * row["reference_rms"] ** 2 for row in rows)
    if reference == 0:
        relative, snr = (0.0 if error == 0 else None), None
        state = "zero_reference_exact" if error == 0 else "zero_reference_nonzero_error"
    else:
        relative = math.sqrt(error / reference)
        snr = None if error == 0 else 10 * (math.log10(reference) - math.log10(error))
        state = "exact_nonzero_reference" if error == 0 else "finite_nonzero_reference"
    worst = max(rows, key=lambda row: row["max_absolute_error"])
    return {"observations": len(rows), "samples": count,
            "max_absolute_error": worst["max_absolute_error"], "max_absolute_error_case": worst["case"],
            "rmse": math.sqrt(error / count), "reference_rms": math.sqrt(reference / count),
            "relative_l2": relative, "snr_dB": snr, "reference_status": state,
            "zero_reference_nonzero_error_cases": [row["case"] for row in rows
                if row["reference_status"] == "zero_reference_nonzero_error"]}


def summarize(path):
    raw = path.read_bytes()
    require(0 < len(raw) <= 8 * 1024 * 1024, "Closed result exceeds size bound")
    def invalid(value):
        raise ValueError("Invalid JSON number: " + value)
    report = json.loads(raw, parse_constant=invalid)
    require(report.get("status") == "comparison_complete" and report.get("completed_requests") == 32
            and all(report.get(key) is True for key in
                    ("cleanup_complete", "pins_unchanged", "baseline_spool_removed", "all_outputs_finite",
                     "input_sha_match", "phase_fields_valid")), "Comparison is not complete/validated/clean")
    require(report["pins_before"] == report["pins_after"], "Reported before/after pins differ")
    for name, expected in (("candidate", CANDIDATE_SHA), ("baseline", BASELINE_SHA),
                           ("model", MODEL_SHA), ("proof", PROOF_SHA)):
        require(report["pins_before"][name]["sha256"] == expected, "Artifact identity differs: " + name)
    runtimes = report["runtimes"]
    require([row["label"] for row in runtimes] == ["baseline", "flash"], "Runtime order differs")
    require(all(row["cleanup"]["complete"] is True for row in runtimes), "Runtime cleanup incomplete")
    expected_ids = [f"{track}:{start}" for track in TRACKS for start in STARTS]
    require(all([row["id"] for row in runtime["requests"]] == expected_ids for runtime in runtimes),
            "Sixteen distinct case IDs/order differ")
    buckets = {region: {stem: {channel: [] for channel in CHANNELS} for stem in STEMS}
               for region in ("whole", "valid", "padding")}
    valid_observations, exact_cases = [], []
    for baseline, flash in zip(runtimes[0]["requests"], runtimes[1]["requests"]):
        for key in ("input_sha256", "normalization", "shape", "dtype", "valid_frames", "start_frame", "track"):
            require(baseline[key] == flash[key], "Unpaired input identity: " + key)
        require(baseline.get("matches_immutable_proof") is True
                and baseline["output_sha256"] == flash["baseline_output_sha256"], "Reference hash binding differs")
        require(all(row.get("completed") is True and row.get("finite") is True and row.get("input_sha_match") is True
                    and row.get("output_shape") == [4, WINDOW, 2] and row.get("output_dtype") == "<f4"
                    for row in (baseline, flash)), "Unvalidated output row")
        exact = baseline["output_sha256"] == flash["output_sha256"]
        require(flash["exact_pcm_match"] is exact, "Bit-identity flag differs from hashes")
        if exact:
            exact_cases.append(flash["id"])
        valid = min(WINDOW, FRAMES - flash["start_frame"])
        require(valid == flash["valid_frames"] and valid > 0, "Valid/padded geometry differs")
        for region, frames in (("whole", WINDOW), ("valid", valid), ("padding", WINDOW - valid)):
            value = flash["difference"][region]
            require(value["frames"] == frames, "Reported region frame count differs")
            if frames == 0:
                require(value == {"frames": 0, "metrics": None, "status": "not_applicable"},
                        "Empty padding must be unavailable, not zero error")
                continue
            require(set(value["metrics"]) == set(STEMS), "Stem metrics differ")
            for stem in STEMS:
                require(set(value["metrics"][stem]) == set(CHANNELS), "Channel metrics differ")
                for channel in CHANNELS:
                    metric = value["metrics"][stem][channel]
                    energies(metric, frames)
                    observation = dict(metric, case=flash["id"], stem=stem, channel=channel)
                    buckets[region][stem][channel].append(observation)
                    if region == "valid":
                        valid_observations.append(observation)
        # Whole covers valid+padding; it must never be added as another disjoint region.
        for stem in STEMS:
            for channel in CHANNELS:
                whole = energies(flash["difference"]["whole"]["metrics"][stem][channel], WINDOW)
                valid_energy = energies(flash["difference"]["valid"]["metrics"][stem][channel], valid)
                padding = flash["difference"]["padding"]
                pad_energy = (0.0, 0.0) if padding["frames"] == 0 else energies(padding["metrics"][stem][channel], padding["frames"])
                require(all(math.isclose(w, v + p, rel_tol=1e-11, abs_tol=0)
                            for w, v, p in zip(whole, valid_energy, pad_energy)),
                        "Whole versus valid+padding energy is inconsistent")
                if exact:
                    require(whole[0] == 0, "Exact bit hash has nonzero reported error")
    require(report["exact_pcm_match"] is (len(exact_cases) == 16), "Top-level exact flag differs")
    summaries = {region: {stem: {channel: aggregate(buckets[region][stem][channel])
                                for channel in CHANNELS} for stem in STEMS} for region in buckets}
    worst_absolute = sorted(valid_observations, key=lambda row: row["max_absolute_error"], reverse=True)[:8]
    worst_relative = sorted((row for row in valid_observations if row["relative_l2"] is not None),
                            key=lambda row: row["relative_l2"], reverse=True)[:8]
    return {"schema": 1, "status": "numerical_summary_complete",
            "source_result": {"path": str(path.resolve()), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()},
            "artifact_sha256": {key: report["pins_before"][key]["sha256"] for key in ("candidate", "baseline", "model", "proof")},
            "cases": 16, "exact_pcm_cases": len(exact_cases), "exact_case_ids": exact_cases,
            "nonidentical_case_ids": [case for case in expected_ids if case not in exact_cases],
            "per_stem_channel": summaries,
            "worst_valid_absolute_observations": worst_absolute,
            "worst_valid_relative_observations": worst_relative,
            "metric_domain": report["metric_domain"],
            "aggregation": "Sample-weighted energy reconstructed from retained float64 RMS metrics; raw PCM was not read. Relative errors include every region's error energy, including zero-reference cases. SNR values are never averaged.",
            "limitations": ["Windows overlap, so aggregate sample counts include repeated source positions.",
                            "Whole and valid/padding summaries overlap; they are not additive observations.",
                            "Each track has its own source normalization; these are normalized native stem units, not dBFS or listening-level differences.",
                            "Padding is reported independently and can contain model context energy.",
                            "Numerically zero error does not imply bit identity; signed zero can differ.",
                            "No OLA/join, continuous-worker, stereo image, listening or musical-equivalence validation is established here."],
            "musical_quality": "not_assessed", "numerical_acceptance_threshold": None,
            "latency_qualification": "not_attempted"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--result", type=Path, default=BASE / "flash-quality-laptop-01/comparison/result.json")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_plan", "scope": "Closed JSON reduction only; no raw PCM/native/host access"}))
        return 0
    require(args.output is not None, "Use a fresh --output JSON path")
    value = summarize(args.result)
    with args.output.open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"status": value["status"], "summary": str(args.output),
                      "exact_pcm_cases": value["exact_pcm_cases"], "cases": value["cases"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Prepare continuous source/baseline/Flash listening files; never play audio."""
import argparse
import hashlib
import html
import importlib.util
import json
import math
import os
from pathlib import Path
import signal
import sys
import time

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
QUALITY = BASE / "flash_quality.py"
QUALITY_SHA = "cec1a694a32da562be9ed79d35cfda7e2ee1943b9b7dfd10daf1ed2259fc7495"
QUALITY_RESULT = BASE / "flash-quality-laptop-01/comparison/result.json"
QUALITY_RESULT_SHA = "917104a1cc8ca2522eef38bae6d620c972fdb2be826876b2dee88e67dd7a497a"
SPOOL_PARENT = BASE / "native-temp"
RATE, FRAMES, HOP, OVERLAP, WINDOW = 44100, 1031940, 257985, 85995, 343980
STEMS = ("drums", "bass", "other", "vocals")
TITLES = {"club_xl": "Club XL", "when_the_light_breaks": "When the Light Breaks",
          "under_my_skin": "Under My Skin", "stfu": "STFU"}
# Frozen supervisor uses this field for its minimum temporary-space reservation.
# Here that scratch directory holds both arms' continuous stem arrays, not windows.
BASELINE_BYTES = 8 * 4 * FRAMES * 2 * 4
WAV_BYTES = 44 * FRAMES * 2 * 4
MEDIA = {
    "waveform": (ROOT / "benchmarks-local/quality-2026-10-17/waveform_review.py",
                 "f1d84b8cc467bc6195d3450e4c6f63415b98a669b89d35eb9ab0647098f5037f"),
    "metrics": (ROOT / "benchmarks-local/quality-2026-10-17/quality_analyze.py",
                "4131400abe7ed071d31f6da6263815e4eb14eb02b32f5e7c979cc41e86b83bac"),
}
SCOPE = ("Continuous listening preparation only: unchanged production fixed-grid OLA and denormalization, "
         "same source start/length, one gain across all eleven signals per track. No inserted gaps, "
         "fades, loops, time shifts, signal switches, autoplay, musical acceptance or latency qualification.")


def require(value, message):
    if not value:
        raise ValueError(message)


def load_module(name, path, digest):
    require(path.stat().st_size <= 128 * 1024 and hashlib.sha256(path.read_bytes()).hexdigest() == digest,
            "Frozen helper differs: " + str(path))
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def quality():
    return load_module("conflux_listening_quality", QUALITY, QUALITY_SHA)


def helper():
    compare = quality().helper()
    compare.REQUIRED_SOURCES = tuple(dict.fromkeys(compare.REQUIRED_SOURCES + (
        Path(__file__).resolve(), BASE / "flash_listening_launch.py", *(path for path, _ in MEDIA.values()))))
    return compare


def reject_environment():
    quality().reject_environment()


def head(compare, deadline):
    return quality().head(compare, deadline)


def configuration(compare, path, source_path, deadline):
    config, specs = quality().configuration(compare, path, source_path, deadline)
    require(config.get("quality_result") == {"path": str(QUALITY_RESULT), "sha256": QUALITY_RESULT_SHA},
            "Closed numerical result pin is required")
    specs["closed_quality"] = (QUALITY_RESULT, QUALITY_RESULT_SHA, 8 * 1024 * 1024)
    specs["quality_helper"] = (QUALITY, QUALITY_SHA, 128 * 1024)
    for name, (file, digest) in MEDIA.items():
        specs["media_helper:" + name] = (file, digest, 128 * 1024)
    return config, specs


def page(report, path):
    parts = ['<!doctype html><html lang="en"><meta charset="utf-8">',
             '<meta name="viewport" content="width=device-width,initial-scale=1">',
             '<title>Continuous baseline and Flash listening</title>',
             '<style>body{font:17px system-ui;background:#111820;color:#eaf2f8;max-width:1150px;margin:2em auto;padding:0 1em}table{width:100%;border-collapse:collapse}td,th{padding:.6em;text-align:left;border-bottom:1px solid #394553}audio{width:100%;min-width:220px}a{color:#8acbff}</style>',
             '<h1>Continuous baseline and Flash listening</h1>',
             '<p>Each player is one uninterrupted 23.4-second signal from original track 30.0–53.4 seconds. '
             'All eleven signals for a track share one gain. Nothing plays automatically.</p>',
             '<p>Compare source and recombination, then corresponding baseline/Flash stems. '
             'Listen near hops 5.85, 11.70 and 17.55 seconds; overlap exits 7.80, 13.65 and 19.50 seconds. '
             'Numerical differences and join diagnostics do not establish musical acceptability.</p>']
    def player(file):
        escaped = html.escape(file, quote=True)
        return f'<audio controls preload="none" src="{escaped}"></audio> <a href="{escaped}">file</a>'
    for track in report["tracks"]:
        files = track["signals"]
        parts.extend([f'<h2>{html.escape(track["title"])}</h2>',
                      f'<p>Shared gain: {track["shared_gain"]:.9g}. Times are relative to this excerpt.</p>',
                      '<p>Source:</p>', player(files["source"]["file"]),
                      '<table><tr><th>Signal</th><th>Baseline</th><th>Flash</th></tr>'])
        for stem in (*STEMS, "recombined"):
            parts.append(f'<tr><th>{stem}</th><td>{player(files["baseline-" + stem]["file"])}</td>'
                         f'<td>{player(files["flash-" + stem]["file"])}</td></tr>')
        parts.append('</table>')
    parts.append('<p>Human listening verdict: pending. No sound was played by preparation.</p></html>')
    with path.open("x", encoding="utf-8") as stream:
        stream.write("\n".join(parts) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    for name in ("output", "spool", "expected-pins", "source-pins"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--deadline-monotonic", type=float)
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status": "dry_run", "native_started": False, "host_queried": False,
              "fresh_requests": 32, "continuous_WAVs": 44, "frames_per_signal": FRAMES,
              "closed_quality_sha256": QUALITY_RESULT_SHA, "scope": SCOPE}, indent=2))
        return 0
    require(all(path is not None and path.is_absolute() for path in
                (args.output, args.spool, args.expected_pins, args.source_pins)), "Explicit absolute paths required")
    began = time.monotonic()
    require(args.deadline_monotonic is not None and math.isfinite(args.deadline_monotonic)
            and began < args.deadline_monotonic <= began + 600, "Reviewed launcher deadline required")
    deadline, work_deadline = args.deadline_monotonic, args.deadline_monotonic - 60
    q = quality()
    q.validate_spool(args.spool)
    compare = helper()
    reject_environment()
    args.output.mkdir(mode=0o700)
    media = args.output / "listening"
    media.mkdir(mode=0o700)
    report = {"schema": 1, "status": "checking", "scope": SCOPE, "native_started": False,
              "completed_requests": 0, "runtimes": [], "tracks": [], "errors": [],
              "pins_before": {}, "pins_after": {}, "musical_quality": "not_assessed",
              "latency_qualification": "not_attempted", "human_listening_verdict": None,
              "temporary_stem_float32_bytes": BASELINE_BYTES, "WAV_float32_bytes": WAV_BYTES,
              "join_metric_domain": "Continuous source/stem/recombined samples before shared listening gain; absolute jump/RMS values are pre-gain and ratios are gain-invariant.",
              "normalization": "Existing worker restores whole-track std and mean to EACH stem after OLA; adapter does not restore either again."}
    workers, owners, specs = [], [], {}
    def checkpoint():
        report["elapsed_seconds"] = time.monotonic() - began
        compare.checkpoint(args.output, report)
    def interrupted(signum, _frame):
        report["interruption_signal"] = signum
        raise KeyboardInterrupt
    handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        config, specs = configuration(compare, args.expected_pins, args.source_pins, work_deadline)
        require(head(compare, work_deadline) == config["repository_head"], "Source HEAD differs")
        report["pins_before"] = compare.capture_pins(specs, work_deadline)
        _, contents = compare.inspect_file(QUALITY_RESULT, 8 * 1024 * 1024, work_deadline,
                                           expected=QUALITY_RESULT_SHA, keep=True)
        closed = compare.decode_json(contents)
        require(closed.get("status") == "comparison_complete" and closed.get("completed_requests") == 32
                and all(closed.get(k) is True for k in ("all_outputs_finite", "input_sha_match", "phase_fields_valid", "pins_unchanged", "cleanup_complete"))
                and closed["pins_before"]["baseline"]["sha256"] == q.BASELINE_SHA
                and closed["pins_before"]["candidate"]["sha256"] == config["candidate"]["sha256"]
                and closed["pins_before"]["model"]["sha256"] == compare.MODEL_SHA,
                "Closed numerical proof is incomplete or differs")
        np, NativeRuntime, prepared = compare.prepare_cases(compare.load_track_plan(work_deadline), work_deadline)
        del prepared
        from tools.stems.worker import StemWorker
        waveform = load_module("conflux_listening_waveform", *MEDIA["waveform"])
        metric = load_module("conflux_listening_metrics", *MEDIA["metrics"])
        source_plan = compare.load_track_plan(work_deadline)
        space = os.statvfs(args.spool)
        require(space.f_bavail * space.f_frsize >= BASELINE_BYTES + WAV_BYTES + 64 * 1024 * 1024,
                "Insufficient workspace space for bounded stems and listening files")
        for arm, executable, reference in zip(("baseline", "flash"), (q.BASELINE, q.CANDIDATE), closed["runtimes"], strict=True):
            require(reference["label"] == arm and len(reference["requests"]) == 16, "Closed arm/order differs")
            require(compare.capture_pins(specs, work_deadline) == report["pins_before"], "Pins changed before child")
            record = {"label": arm, "requests": [], "cleanup": {"complete": False}, "tracks": []}
            report["runtimes"].append(record)
            class CheckedNative(NativeRuntime):
                def start(self):
                    ready = super().start()
                    require(ready.get("backend") == "vulkan" and ready.get("threads") == 2
                            and ready.get("revision") == compare.REVISION and ready.get("model") == str(compare.MODEL)
                            and ready.get("pid") == self.process.pid and ready.get("weight_storage") == "native"
                            and ready.get("fence_wait_mode") == "blocking"
                            and ready.get("process_environment") == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"},
                            "Native ready contract differs")
                    record.update(pid=self.process.pid, ready=ready)
                    return ready
                def infer(self, samples, request_id):
                    index = len(record["requests"])
                    require(index < 16, "Unexpected extra inference request")
                    expected = reference["requests"][index]
                    require(request_id == expected["request_id"] and hashlib.sha256(memoryview(samples)).hexdigest() == expected["input_sha256"],
                            "Fresh normalized input/order differs from numerical result")
                    self.timeout = min(30, compare.check_deadline(work_deadline))
                    produced, metrics = super().infer(samples, request_id)
                    require(produced.shape == (4, WINDOW, 2) and produced.dtype.str == "<f4"
                            and produced.flags.c_contiguous and np.isfinite(produced).all(), "Native output geometry/finite contract differs")
                    digest = hashlib.sha256(memoryview(produced)).hexdigest()
                    require(digest == expected["output_sha256"], "Fresh output differs from closed numerical result")
                    actual, _ = compare.inspect_file(self.root / "input.f32", WINDOW * 2 * 4, work_deadline,
                                                     expected=expected["input_sha256"])
                    require(actual["bytes"] == WINDOW * 2 * 4, "Native written input size differs")
                    for field in compare.COMMON_TIMINGS + compare.PHASE_TIMINGS:
                        compare.validate_number(metrics, field)
                    record["requests"].append({"id": expected["id"], "input_sha256": expected["input_sha256"],
                        "output_sha256": digest, "valid_frames": expected["valid_frames"], "shape": list(produced.shape),
                        "finite": True, "matches_closed_quality": True, "native_metrics": metrics})
                    report["completed_requests"] += 1
                    return produced, metrics
                def close(self):
                    try:
                        if not self.stderr.closed:
                            record["stderr"] = compare.save_stderr(self, args.output, arm)
                    finally:
                        super().close()
                        record["cleanup"] = {"complete": self.process.poll() is not None and not self.root.exists(),
                                             "reaped": self.process.poll() is not None, "temporary_directory_removed": not self.root.exists()}
            def factory(*values):
                native = CheckedNative.__new__(CheckedNative)
                owners.append(native)  # Retain partial construction until main-thread cleanup.
                native.__init__(*values)
                report["native_started"] = True
                return native
            worker = StemWorker(executable, compare.MODEL, backend="vulkan", threads=2,
                                request_timeout=30, _backend_factory=factory)
            workers.append(worker)
            try:
                for track_index, (identifier, source_path) in enumerate(source_plan):
                    compare.check_deadline(work_deadline)
                    destination = np.lib.format.open_memmap(args.spool / "baseline" / f"{arm}-{identifier}.npy",
                        mode="w+", dtype="<f4", shape=(4, FRAMES, 2))
                    generation, cursor, chunks, source_seen = worker.load(source_path), 0, 0, False
                    try:
                        while True:
                            compare.check_deadline(work_deadline)
                            event = worker.poll(.25)
                            if event is None:
                                continue
                            if event["event"] == "error":
                                raise RuntimeError(event["message"])
                            if event.get("generation") != generation:
                                require(event["event"] == "runtime_ready", "Unexpected generation")
                                continue
                            if event["event"] == "source_ready":
                                expected = reference["requests"][track_index * 4]["normalization"]
                                require(event["source_id"] == dict(compare.TRACKS)[identifier]
                                        and event["source_frames"] == FRAMES and event["mean"] == expected["mean"]
                                        and event["std"] == expected["std"], "Source identity/normalization differs")
                                source_seen = True
                            if event["event"] == "complete":
                                require(source_seen and cursor == FRAMES and event["frame_start"] == FRAMES and chunks == 4,
                                        "Continuous coverage is incomplete")
                                break
                            if event["event"] != "chunk":
                                continue
                            samples, count = event["samples"], event["frames"]
                            require(worker.is_current(event) and event["source_id"] == dict(compare.TRACKS)[identifier]
                                    and event["model_id"] == compare.MODEL_SHA and event["frame_start"] == cursor
                                    and count == min(HOP, FRAMES - cursor) and samples.shape == (4, count, 2)
                                    and samples.dtype.str == "<f4" and np.isfinite(samples).all(), "Invalid continuous chunk")
                            destination[:, cursor:cursor + count] = samples
                            cursor += count
                            chunks += 1
                        destination.flush()
                    finally:
                        del destination
                    record["tracks"].append({"id": identifier, "frames": cursor, "chunks": chunks})
                    checkpoint()
            finally:
                worker.close(timeout=.05)
                record["worker_statistics"] = worker.statistics()
            stats = record["worker_statistics"]
            require(record["cleanup"]["complete"] and stats["closed"] and stats["backend_pid"] is None
                    and stats["backend_start_count"] == 1 and stats["telemetry_dropped"] == 0 and len(record["requests"]) == 16,
                    "Persistent worker/native cleanup or evidence is incomplete")
            if arm == "flash":
                report["flash_evidence"] = q.flash_logs(record, compare, work_deadline)
        for identifier, source_path in source_plan:
            compare.check_deadline(work_deadline)
            source = metric.pcm16(source_path)
            require(source.shape == (FRAMES, 2) and np.isfinite(source).all(), "Source geometry differs")
            arrays = {arm: np.load(args.spool / "baseline" / f"{arm}-{identifier}.npy", mmap_mode="r", allow_pickle=False)
                      for arm in ("baseline", "flash")}
            require(all(a.shape == (4, FRAMES, 2) and a.dtype.str == "<f4" and np.isfinite(a).all() for a in arrays.values()),
                    "Continuous array geometry/finite contract differs")
            signals = {"source": source}
            for arm, array in arrays.items():
                signals.update({arm + "-" + stem: array[index] for index, stem in enumerate(STEMS)})
                signals[arm + "-recombined"] = np.sum(array, axis=0, dtype=np.float64)
            peak = max(float(np.max(np.abs(value))) for value in signals.values())
            gain = .98 / peak if peak > 1 else 1.0
            track = {"id": identifier, "title": TITLES[identifier], "frames": FRAMES,
                     "seconds": FRAMES / RATE, "shared_gain": gain, "peak_before_gain": peak, "signals": {}, "joins": []}
            for boundary in range(HOP, FRAMES, HOP):
                for kind, frame in (("hop", boundary), ("overlap_exit", boundary + OVERLAP)):
                    track["joins"].append({"kind": kind, "frame": frame, "seconds": frame / RATE,
                        "signals": {label: metric.join_diagnostic(value, frame) for label, value in signals.items()}})
            for label, signal_values in signals.items():
                compare.check_deadline(work_deadline)
                values = np.asarray(signal_values * gain, dtype="<f4")
                require(values.shape == (FRAMES, 2) and np.isfinite(values).all()
                        and float(np.max(np.abs(values))) <= 1, "Listening gain/geometry would clip")
                filename = identifier + "-" + label + ".wav"
                require(not (media / filename).exists(), "Fresh listening output required")
                partial = media / (identifier + "-" + label + ".partial.wav")
                require(not partial.exists(), "Fresh partial listening output required")
                try:
                    saved = waveform.wav(partial, values)
                    compare.check_deadline(work_deadline)
                    partial.rename(media / filename)
                    saved["path"] = str((media / filename).resolve())
                finally:
                    partial.unlink(missing_ok=True)
                track["signals"][label] = {"file": filename, "wav": saved,
                    "float32_pcm_sha256": hashlib.sha256(values.tobytes()).hexdigest(),
                    "frames": FRAMES, "sample_rate": RATE, "channels": 2}
            report["tracks"].append(track)
            del signals, arrays
            checkpoint()
        require(report["completed_requests"] == 32 and len(report["tracks"]) == 4
                and sum(len(t["signals"]) for t in report["tracks"]) == 44, "Listening preparation incomplete")
        page(report, media / "listen.html")
        report.update(status="comparison_complete", listening_page=str(media / "listen.html"), continuous_WAVs=44)
    except KeyboardInterrupt:
        report["status"] = "cancelled"
    except Exception as error:
        report["status"] = "incomplete"
        report["errors"].append({"type": type(error).__name__, "message": str(error)[:8192]})
    finally:
        for sig in handlers:
            signal.signal(sig, signal.SIG_IGN)
        cleanup_ok = True
        for worker in workers:
            try:
                worker.close(timeout=.05)
            except Exception as error:
                cleanup_ok = False
                report["errors"].append({"phase": "worker_close", "message": str(error)})
        for native in owners:
            try:
                if hasattr(native, "_cleanup"):
                    NativeRuntime.close(native)
            except Exception as error:
                cleanup_ok = False
                report["errors"].append({"phase": "partial_native_close", "message": str(error)})
        any_process = any(getattr(native, "process", None) is not None for native in owners)
        report["native_started"] = True if any_process else None if owners and not cleanup_ok else False
        try:
            for file in (args.spool / "baseline").glob("*.npy"):
                file.unlink()
            native_clean = all((getattr(native, "process", None) is None or native.process.poll() is not None)
                               and (getattr(native, "root", None) is None or not native.root.exists()) for native in owners)
            report["cleanup_complete"] = cleanup_ok and native_clean and not any((args.spool / "baseline").iterdir())
        except Exception as error:
            report["cleanup_complete"] = False
            report["errors"].append({"phase": "scratch_cleanup", "message": str(error)})
        try:
            if specs:
                report["pins_after"] = compare.capture_pins(specs, deadline, verify=False)
                report["pins_unchanged"] = bool(report["pins_before"]) and report["pins_before"] == report["pins_after"]
                require(report["pins_unchanged"] and head(compare, deadline) == config["repository_head"], "Source/pins/HEAD changed")
            compare.check_deadline(deadline)
        except Exception as error:
            report["errors"].append({"phase": "after_pins", "message": str(error)})
        if report["status"] == "comparison_complete" and (report["errors"] or not report["cleanup_complete"]):
            report["status"] = "incomplete"
        checkpoint()
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    print(json.dumps({"status": report["status"], "completed_requests": report["completed_requests"],
                      "continuous_WAVs": report.get("continuous_WAVs"), "result": str(args.output / "result.json")}, allow_nan=False))
    return 0 if report["status"] == "comparison_complete" else 2 if report["status"] == "cancelled" else 1


if __name__ == "__main__":
    raise SystemExit(main())

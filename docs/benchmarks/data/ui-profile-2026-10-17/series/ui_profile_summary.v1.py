#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Summarize completed UI process samples and retained Qt phase messages."""

import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import re


FIELD = re.compile(r"\b([A-Za-z][A-Za-z_]*)=(-?\d+(?:\.\d+)?)\s*(ms|us|µs|ns)?\b")
ELAPSED = re.compile(r"elapsed since last call:\s*(\d+(?:\.\d+)?)\s*ms")
TOTAL = re.compile(r"Frame rendered.*?\bin\s+(\d+(?:\.\d+)?)\s*ms")
DRM_VALUE = re.compile(r"(\d+)\s+(\S+)")


def distribution(values):
    values = sorted(values)
    return ({"count": len(values), "minimum": values[0], "maximum": values[-1],
             "p50": values[math.ceil(len(values) * .50) - 1],
             "p95": values[math.ceil(len(values) * .95) - 1],
             "p99": values[math.ceil(len(values) * .99) - 1]}
            if values else {"count": 0})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", args.label):
        parser.error("label must be a plain filename label")
    base = Path(__file__).resolve().parent
    result = json.loads((base / args.label / "result.json").read_text())
    launch = json.loads((base / (args.label + "-launch.json")).read_text())
    if "finished_at_utc" not in launch:
        parser.error("wrapper has not completed")
    capture = json.loads((base / (args.label + "-qt-timing-capture.json")).read_text())
    output = base / (args.label + "-ui-profile-summary.json")
    if output.exists():
        parser.error("summary already exists; refuse to overwrite")
    start, stop = result["workload_started_monotonic"], result["workload_stopped_monotonic"]
    counts, categories, phases, unmatched = Counter(), Counter(), defaultdict(list), Counter()
    journal_boot_ids = set()
    with (base / (args.label + "-qt-timing.jsonl")).open() as stream:
        for line in stream:
            row = json.loads(line)
            counts["retained_journal_entries"] += 1
            if "_BOOT_ID" in row:
                journal_boot_ids.add(row["_BOOT_ID"])
            if "__MONOTONIC_TIMESTAMP" not in row:
                counts["journal_entries_missing_monotonic"] += 1
                continue
            at = int(row["__MONOTONIC_TIMESTAMP"]) / 1e6
            if not start <= at <= stop:
                continue
            counts["journal_entries_in_workload"] += 1
            message = str(row.get("MESSAGE", ""))
            category = str(row.get("QT_CATEGORY", "unknown"))
            categories[category] += 1
            group = ("gui_frame_prepared" if "Frame prepared" in message else
                     "render_frame" if "Frame rendered" in message else
                     "renderer" if "time in renderer" in message else "other")
            extracted = False
            for match in FIELD.finditer(message):
                key, value, unit = match.groups()
                if unit is None and group not in ("gui_frame_prepared", "render_frame", "renderer"):
                    continue
                if unit is None:
                    unit = "ms"  # These Qt timing records label the shared millisecond unit.
                phases[f"{group}.{key}.{unit}"].append(float(value))
                extracted = True
            for name, pattern in (("render_loop_call_interval.ms", ELAPSED), ("render_frame.total.ms", TOTAL)):
                found = pattern.search(message)
                if found:
                    phases[name].append(float(found[1]))
                    extracted = True
            if row.get("recognized_timing_message") and not extracted:
                counts["unparsed_timing_entries"] += 1
                unmatched[re.sub(r"0x[0-9a-fA-F]+", "<address>", message)] += 1
    threads, drm, sample_times, read_errors = {}, {}, [], Counter()
    hz, schedstats = None, None
    with (base / (args.label + "-ui-process.jsonl")).open() as stream:
        for line in stream:
            row = json.loads(line)
            event = row.get("event")
            counts["process_events." + str(event)] += 1
            if event == "started":
                hz, schedstats = row["clock_ticks_per_second"], row.get("sched_schedstats")
            if event == "read_error":
                read_errors[row.get("error", "unknown")] += 1
            at = row["timestamp_monotonic"]
            if event != "sample" or not start <= at <= stop:
                continue
            sample_times.append(at)
            for thread in row["threads"]:
                key = (thread["tid"], thread["start_ticks"])
                if key not in threads:
                    threads[key] = {"first": thread, "first_at": at}
                threads[key].update(last=thread, last_at=at)
            for client in row["drm_clients"]:
                key = (client.get("drm-pdev", client.get("drm-driver", "unknown")), client["drm-client-id"])
                if key not in drm:
                    drm[key] = {"first": client, "first_at": at}
                drm[key].update(last=client, last_at=at)
    thread_rows = []
    for (tid, identity), row in sorted(threads.items()):
        first, last = row["first"], row["last"]
        item = {"tid": tid, "start_ticks": identity, "name": first["name"],
                "role": "gui" if tid == capture["pid"] else "render" if "QSGRenderThread" in first["name"] else "other",
                "observed_span_seconds": row["last_at"] - row["first_at"],
                "cpu_seconds": (last["user_ticks"] + last["system_ticks"] - first["user_ticks"] - first["system_ticks"]) / hz}
        if "schedstat" in first and "schedstat" in last:
            item["schedstat_deltas"] = {field: last["schedstat"][field] - first["schedstat"][field]
                                       for field in ("runtime_ns", "runqueue_wait_ns", "timeslices")}
        thread_rows.append(item)
    drm_rows = []
    for (device, client_id), row in sorted(drm.items()):
        changes = {}
        for key, raw in row["last"].items():
            if not key.startswith("drm-engine-") or key not in row["first"]:
                continue
            first, last = DRM_VALUE.fullmatch(row["first"][key]), DRM_VALUE.fullmatch(raw)
            if first and last and first[2] == last[2]:
                changes[key] = {"delta": int(last[1]) - int(first[1]), "unit": first[2]}
        drm_rows.append({"device": device, "client_id": client_id,
                         "observed_span_seconds": row["last_at"] - row["first_at"], "engine_deltas": changes})
    summary = {"schema": 1, "label": args.label, "renderer": result["renderer"],
               "workload_monotonic": {"start": start, "stop": stop, "seconds": stop - start},
               "continuous_hop_timing": result["timing"]["continuous"],
               "capture_returncode": capture["returncode"], "capture_errors": capture["errors"],
               "counts": dict(counts), "categories_in_workload": dict(categories),
               "journal_boot_ids": sorted(journal_boot_ids),
               "qt_named_phase_distributions": {key: distribution(value) for key, value in sorted(phases.items())},
               "unparsed_timing_message_shapes": dict(unmatched.most_common(20)),
               "process_sample_coverage": {"samples": len(sample_times), "first": min(sample_times, default=None),
                                           "last": max(sample_times, default=None),
                                           "maximum_sample_gap_seconds": max((b - a for a, b in zip(sample_times, sample_times[1:])), default=None)},
               "process_read_errors": dict(read_errors), "sched_schedstats": schedstats,
               "thread_cpu_seconds_by_role": {role: sum(row["cpu_seconds"] for row in thread_rows if row["role"] == role)
                                               for role in ("gui", "render", "other")},
               "threads": thread_rows, "drm_clients": drm_rows,
               "scope": ["Qt textual timing fields are generally integer milliseconds with quantization; zero does not mean no cost. Nested timings overlap and must not be summed. Render/swap elapsed times include CPU/driver/wait work and do not establish GPU kernel time or physical presentation latency.",
                         "Journal monotonic timestamps filter the workload interval; textual Qt time-boot prefixes are not used for alignment. Same-boot monotonic alignment assumes no clock-domain mismatch; suspend accounting can differ.",
                         "Thread CPU deltas use matching TID/start_ticks within observed first-to-last samples; lifetime portions before/after sampling or between first/last surviving samples are not counted. Disabled sched_schedstats cannot exclude runqueue contention.",
                         "DRM deltas are cumulative per-client engine accounting between samples, not isolated per-frame GPU duration or complete compositor activity.",
                         "Journal rate limiting/drops and unmatched messages prevent a complete per-frame trace claim. Diagnostic logging and sampling can perturb the measured workload."]}
    output.write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"label": args.label, "counts": dict(counts),
                      "cpu_seconds_by_role": summary["thread_cpu_seconds_by_role"],
                      "phase_keys": sorted(phases)}))


if __name__ == "__main__":
    main()

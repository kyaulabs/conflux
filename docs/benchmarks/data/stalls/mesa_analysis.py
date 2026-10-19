#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Summarize completed Mesa text trace groups without aligning GPU and CPU clocks."""
import argparse
from collections import Counter, defaultdict
import hashlib
import heapq
import json
import math
from pathlib import Path
import re

EVENT = re.compile(r"^(\d+)\s+([+-]\d+):\s+(\S+)(.*)$")
ELAPSED = re.compile(r"^ELAPSED:\s+(\d+)\s+ns\s*$")
FIELD = re.compile(r"(\w+)=([^,\s]+)")


def percentiles(values):
    ordered = sorted(values)
    return {str(p): ordered[math.ceil(len(ordered) * p / 100) - 1]
            for p in (50, 95, 99)} if ordered else {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()
    if not 1 <= args.top <= 100:
        parser.error("top must be 1..100")
    top = defaultdict(list)
    serial = 0

    def retain(kind, duration, record):
        nonlocal serial
        serial += 1
        item = (duration, serial, record)
        if len(top[kind]) < args.top:
            heapq.heappush(top[kind], item)
        elif item[:2] > top[kind][0][:2]:
            heapq.heapreplace(top[kind], item)

    event_counts = Counter()
    regression_transitions = Counter()
    warnings = Counter()
    elapsed_values = []
    compute_durations = []
    shader_groups = defaultdict(lambda: {"durations_ns": []})
    block = None
    previous_complete_end = None
    first_timestamp = None
    last_timestamp = None
    line_count = 0
    digest = hashlib.sha256()
    with args.trace.open("rb") as stream:
        for raw in stream:
            digest.update(raw)
            line_count += 1
            line = raw.decode(errors="replace").rstrip()
            if line.startswith("+----- NS"):
                if block is not None:
                    warnings["unfinished_trace_groups"] += 1
                block = {"start_line": line_count, "events": Counter(),
                         "first": None, "last": None, "last_name": None,
                         "last_line": None, "handles": [],
                         "begins": defaultdict(list), "computes": []}
                continue
            match = EVENT.match(line)
            if match:
                if block is None:
                    warnings["events_without_trace_group"] += 1
                    continue
                timestamp, delta = int(match[1]), int(match[2])
                name, details = match[3], match[4].strip()
                if block["first"] is None:
                    block["first"] = timestamp
                if block["last"] is not None and timestamp < block["last"]:
                    warnings["timestamp_regressions_in_group"] += 1
                    regression_transitions[f"{block['last_name']} -> {name}"] += 1
                    backward = block["last"] - timestamp
                    retain("adjacent_timestamp_regressions", backward, {
                        "backward_ns": backward, "previous_event": block["last_name"],
                        "previous_gpu_ns": block["last"], "previous_line": block["last_line"],
                        "next_event": name, "next_gpu_ns": timestamp,
                        "next_line": line_count, "group_start_line": block["start_line"]})
                block["last"] = timestamp
                block["last_name"] = name
                block["last_line"] = line_count
                block["events"][name] += 1
                if name.startswith("intel_begin_"):
                    block["begins"][name.removeprefix("intel_begin_")].append(timestamp)
                elif name.startswith("intel_end_"):
                    kind = name.removeprefix("intel_end_")
                    starts = block["begins"][kind]
                    if starts:
                        start = starts.pop()
                        duration = timestamp - start
                        if duration < 0:
                            warnings["negative_event_pair_durations"] += 1
                        elif kind == "compute":
                            fields = dict(FIELD.findall(details))
                            block["computes"].append({
                                "begin_gpu_ns": start, "end_gpu_ns": timestamp,
                                "duration_ns": duration, "end_line": line_count,
                                "fields": fields})
                    else:
                        warnings["end_event_without_begin"] += 1
                    if kind == "cmd_buffer":
                        block["handles"].append(dict(FIELD.findall(details)))
                continue
            match = ELAPSED.match(line)
            if match:
                if block is None or block["first"] is None:
                    warnings["elapsed_without_events"] += 1
                    continue
                elapsed = int(match[1])
                index = len(elapsed_values)
                elapsed_values.append(elapsed)
                event_counts.update(block["events"])
                first_timestamp = block["first"] if first_timestamp is None else first_timestamp
                last_timestamp = block["last"]
                if elapsed != block["last"] - block["first"]:
                    warnings["elapsed_differs_from_timestamp_span"] += 1
                if any(block["begins"].values()):
                    warnings["completed_groups_with_unclosed_events"] += 1
                record = {"group_index": index, "start_line": block["start_line"],
                          "end_line": line_count, "begin_gpu_ns": block["first"],
                          "end_gpu_ns": block["last"], "elapsed_ns": elapsed,
                          "event_counts": dict(block["events"]),
                          "command_buffers": block["handles"]}
                retain("trace_groups", elapsed, record)
                if previous_complete_end is not None:
                    gap = block["first"] - previous_complete_end
                    if gap < 0:
                        warnings["timestamp_regressions_between_groups"] += 1
                    else:
                        retain("between_group_gaps", gap, {
                            "before_group_index": index, "gap_ns": gap,
                            "previous_end_gpu_ns": previous_complete_end,
                            "next_begin_gpu_ns": block["first"],
                            "next_start_line": block["start_line"]})
                previous_complete_end = block["last"]
                for compute in block["computes"]:
                    duration = compute["duration_ns"]
                    compute_durations.append(duration)
                    fields = compute["fields"]
                    key = tuple(fields.get(field) for field in
                                ("cs_hash", "group_x", "group_y", "group_z"))
                    shader_groups[key]["durations_ns"].append(duration)
                    retain("compute_pairs", duration, {"group_index": index, **compute})
                block = None
    if block is not None:
        warnings["unfinished_trace_groups"] += 1
    grouped = []
    for key, values in shader_groups.items():
        durations = values["durations_ns"]
        grouped.append({"cs_hash": key[0], "group_x": key[1], "group_y": key[2],
                        "group_z": key[3], "count": len(durations),
                        "sum_duration_ns": sum(durations), "min_duration_ns": min(durations),
                        "max_duration_ns": max(durations),
                        "percentiles_ns": percentiles(durations)})
    result = {"schema": 1, "trace_path": str(args.trace.resolve()),
              "trace_sha256": digest.hexdigest(), "lines": line_count,
              "completed_trace_groups": len(elapsed_values),
              "event_counts_in_completed_groups": dict(event_counts),
              "first_gpu_timestamp_ns": first_timestamp,
              "last_gpu_timestamp_ns": last_timestamp,
              "trace_group_elapsed_percentiles_ns": percentiles(elapsed_values),
              "compute_pair_duration_percentiles_ns": percentiles(compute_durations),
              "warnings": dict(warnings),
              "adjacent_timestamp_regression_transitions": dict(regression_transitions),
              "longest": {kind: [item[2] for item in sorted(items, reverse=True)]
                          for kind, items in top.items()},
              "compute_shader_workgroups": sorted(grouped,
                  key=lambda item: item["max_duration_ns"], reverse=True),
              "scope": ["GPU timestamp domain from Mesa text trace; no CPU clock calibration.",
                        "Trace groups are not native requests; request IDs are absent.",
                        "Event durations may include preemption, synchronization or other stalls.",
                        "Gaps between trace groups are not proof that the physical GPU was idle.",
                        "Negative timestamp deltas are flagged; no wrap period is assumed.",
                        "Only completed ELAPSED groups contribute trace-group and compute-pair duration statistics; warnings cover all observed events."]}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in ("completed_trace_groups", "warnings",
                      "trace_group_elapsed_percentiles_ns", "compute_pair_duration_percentiles_ns")}, indent=2))


if __name__ == "__main__":
    main()

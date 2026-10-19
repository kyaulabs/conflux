#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Derive a small report from completed strict none-hour analysis files."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--analysis', type=Path, required=True)
    parser.add_argument('--focus', type=Path, required=True)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    whole = json.loads(args.analysis.read_bytes())
    focus = json.loads(args.focus.read_bytes())
    sources = json.loads(args.sources.read_bytes())
    def suffix(system, end):
        return next((value for key,value in system.items() if key.endswith(end)), {})
    def brief(row):
        host = row['strict_host']
        system = host['system']
        terminal = row['terminal_event'] or {}
        render = [r for r in host['drm']['engines'] if r['field']=='drm-engine-render']
        return {'request_id':row['request_id'],'generation':row['generation'],
            'window_start_frame':row['started_event']['window_start_frame'],
            'accepted':row['accepted_soak_hop'],'preroll':row['started_event'].get('preroll'),
            'span_ms':(row['bounds']['end']-row['bounds']['start'])*1000,'span_scope':row['bounds']['source'],
            'service_ms':terminal.get('hop_service_ms'),'metrics':terminal.get('metrics'),
            'parity':(row['accepted_event'] or {}).get('parity'),
            'strict_samples':host['samples'],'actual_GPU_MHz':suffix(system,'/gt_act_freq_mhz'),
            'requested_GPU_MHz':suffix(system,'/gt_cur_freq_mhz'),
            'PL1_fraction':suffix(system,'/throttle_reason_pl1').get('nonzero_fraction'),
            'thermal_flag_fraction':suffix(system,'/throttle_reason_thermal').get('nonzero_fraction'),
            'package_millidegrees_C':suffix(system,'/thermal_zone7/temp'),
            'RC6_delta_ms':suffix(system,'/rc6_residency_ms').get('delta'),
            'runtime_suspend_delta_ms':suffix(system,'/runtime_suspended_time').get('delta'),
            'render_clients':render,
            'native_major_fault_delta':host['process_counters'].get('stat.majflt',{}).get('delta'),
            'native_swap_kB':host['process_status']['VmSwap']['summary'],
            'MemAvailable_kB':system.get('/proc/meminfo:MemAvailable'),
            'memory_PSI_some_delta_us':system.get('/proc/pressure/memory:some.total',{}).get('delta'),
            'memory_PSI_full_delta_us':system.get('/proc/pressure/memory:full.total',{}).get('delta'),
            'scan_reclaim_compaction_OOM_deltas':{key:entry.get('delta') for key,entry in system.items()
                if key.startswith('/proc/vmstat:') and any(t in key for t in ('pgscan','pgsteal','compact_stall','oom_kill'))},
            'system_swap_deltas':{key:entry.get('delta') for key,entry in system.items() if key.startswith('/proc/vmstat:pswp')},
            'capture_max_ms':host['capture_elapsed_ms']['max'],'sample_gap_max_ms':host['sample_gaps_ms']['max'],
            'identity_stable':host['process_identity_stable'],'fd_read_errors':host['drm']['fd_read_errors']}
    misses = [row for row in whole['selected_hops'] if row['above_target']]
    medians = Counter(suffix(row['host']['system'],'/gt_act_freq_mhz').get('median') for row in misses)
    all_system = whole['workload_host']['system']
    out = {'status':focus['status'],'failures':focus['failures'],'selected_loads':focus['selected_loads'],
        'duration_seconds':whole['workload_seconds'],'accepted_timing':whole['recalculated_delivered'],
        'strict_integrity_checks':whole['checks'],'selected_request_rows':[brief(row) for row in focus['strict_rows']],
        'accepted_misses_actual_gpu_median_MHz_counts':dict(medians),
        'kernel_record_interval_counts':{','.join(map(str,key)) or 'outside_selected_request_intervals':value
            for key,value in Counter(tuple(row['request_interval_candidates']) for row in focus['kernel_journal_relation']['records']).items()},
        'whole_workload_host':{'samples':whole['workload_host']['samples'],
            'process_status':whole['workload_host']['process_status'],
            'process_counters':whole['workload_host']['process_counters'],
            'temperatures':{key:value for key,value in all_system.items() if key.endswith(('/thermal_zone5/temp','/thermal_zone7/temp'))},
            'system_memory':{key:value for key,value in all_system.items() if key.startswith('/proc/vmstat:') or key.startswith('/proc/pressure/memory:') or key in ('/proc/meminfo:MemAvailable','/proc/meminfo:SwapFree')},
            'capture_elapsed_ms':whole['workload_host']['capture_elapsed_ms'],
            'sample_gaps_ms':whole['workload_host']['sample_gaps_ms'],
            'scheduling':whole['workload_host']['scheduling']},
        'trace_fd_read_errors':focus['trace_fd_read_errors'],'upstream_sources':sources,
        'conclusion':'The kernel recorded incomplete native-process request fence expirations during the final approximately20-second requests. The matching upstream watchdog path cancels such requests. This is concrete request-cancellation evidence and a plausible numerical-integrity mechanism, not proof of GPU reset or of which preroll/current output changed. The cause of the preceding latency degradation remains unresolved.',
        'limits':['Exact hardened-kernel source unavailable; upstream source commit is a mechanism cross-check, not an asserted build match.',
            'No retained failed PCM or numeric maxdiff exists in this attempt; discarded/preroll inference metrics are also incomplete.',
            'None suppresses benchmark-created audio/UI only. Other desktop activity, policy history and system-wide memory activity remain uncontrolled.',
            'Per-request clocks/flags are observations; no wattage, power-limit causality, physical queue execution or reset is inferred.',
            'Whole workload includes system reclaim/swap activity; final focused requests do not show matching native swap/major faults or substantial memory PSI. GPU backing-store pressure is not fully observable.',
            'Final failed chunk/discarded/preroll requests are excluded from accepted-hop timing. Partial run cannot qualify the requested hour or isolate all four conditions.'],
        'smallest_next_experiments':['Retain failure-only arrays and finite/allclose diagnostics before repeating; terminate the comparison on driver/quality failure without weakening tolerance.',
            'Investigate app-local bounded Vulkan submission depth with completion waits between existing batches; preserve shader math/window/model and include every wait in service timing. Merely reducing nodes-per-submit may enqueue more watchdog-armed requests and does not bound waiting.',
            'Treat a finite fence-wait failure as a failed request, never a valid result; test kernel/driver error propagation before relying on successful Vulkan fence completion as data-validity evidence.',
            'Do not disable or extend kernel watchdogs as a presumed correctness/performance fix.'],
        'inputs':{str(path):{'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'bytes':path.stat().st_size} for path in (args.analysis,args.focus,args.sources)},
        'summary_script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    args.output.write_text(json.dumps(out,indent=2)+'\n')
    print(json.dumps({'output':str(args.output),'misses':len(misses),'median_actual_clock_distribution':dict(medians)}))


if __name__ == '__main__':
    main()

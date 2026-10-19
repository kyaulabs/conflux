# SPDX-License-Identifier: AGPL-3.0-or-later
"""Read saved benchmark progress only; no live host or policy query."""
import json
from pathlib import Path
b=Path(__file__).resolve().parent
s=json.loads((b/'suite-state.json').read_text())
p=b/'observed-both-hour/progress.json'
r=json.loads(p.read_text());t=r.get('timing',{})
print(json.dumps({'scope':'saved workspace progress','suite_phase':s['phase'],
 'updated_at_utc':r.get('updated_at_utc'),'status':r.get('status'),
 'minutes':round(r.get('elapsed_workload_seconds',0)/60,2),'chunks':t.get('chunks'),
 'p99_ms':t.get('hop_service_percentiles_ms',{}).get('99'),'max_ms':t.get('hop_service_max_ms'),
 'misses':t.get('hops_above_target'),'controls':r.get('completed_controls'),
 'failures':r.get('failures'),'UI_frames':r.get('ui_progress',{}).get('last_progress',{}).get('frames_swapped'),
 'sampled_firmware':r.get('conditions',{}).get('/sys/firmware/acpi/platform_profile'),
 'pause_completion_exists':(b/'pause-completed.json').exists()}))

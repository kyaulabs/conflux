#!/usr/bin/env python3
from pathlib import Path
import hashlib, json, subprocess, sys
root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=False)
prior = json.loads((root / 'docs/benchmarks/data/m1-stream-2026-10-17/verification.json').read_text())
paths = set(prior['source_pins'])
paths.update(str(p.relative_to(root)) for p in (root / 'src/audio').glob('*.hpp'))
paths.update(str(p.relative_to(root)) for p in (root / 'src/audio').glob('*.cpp'))
paths.update(str(p.relative_to(root)) for p in (root / 'tests').glob('audio_*test.cpp'))
paths.update(['qml/StereoMeter.qml', 'tests/live_session_test.cpp', 'tests/manual_ui_test.cpp', 'tests/deck_controller_test.cpp', 'tools/experiments/manual_routing_check.cpp', 'tools/experiments/private_routing_check.py', 'tools/experiments/manual_audio_check.cpp'])
def pin(path):
    data = (root / path).read_bytes()
    return {'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
sources = {path: pin(path) for path in sorted(paths) if (root / path).is_file()}
for path in sources:
    saved = out / 'frozen-source' / path
    saved.parent.mkdir(parents=True, exist_ok=True)
    saved.write_bytes((root / path).read_bytes())
executables = sorted(p for p in (root / 'build/dev').glob('conflux*') if p.is_file() and not p.suffix)
binaries = {p.name: pin(str(p.relative_to(root))) for p in executables}
report = {'schema':1, 'client_date':'2026-10-17 America/Los_Angeles',
          'source_base_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
          'source_pins':sources, 'binaries':binaries,
          'scope':'Frozen source and binary snapshot captured before the named verification run; later edits require a new snapshot.'}
(out / 'pins-before.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'snapshot':str(out), 'sources':len(sources), 'binaries':len(binaries)}))

import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent
spool = out / 'spool'
spool.mkdir(exist_ok=True)
tracks = sorted((Path.home() / 'temp/tracks').glob('*.flac'))
assert len(tracks) == 4
def pin(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda:source.read(1024 * 1024), b''):
            digest.update(block)
    return {'path':str(path), 'bytes':path.stat().st_size, 'sha256':digest.hexdigest()}
before = [pin(path) for path in tracks]
argv = [str(root / 'build/dev/conflux-prepare-audio-check'), str(spool), *map(str, tracks)]
began = time.monotonic()
with (out / 'corpus.json').open('w') as stdout, (out / 'corpus.stderr').open('w') as stderr:
    result = subprocess.run(argv, cwd=root, stdout=stdout, stderr=stderr, timeout=120)
elapsed = time.monotonic() - began
usage = resource.getrusage(resource.RUSAGE_CHILDREN)
after = [pin(path) for path in tracks]
report = {'argv':argv, 'returncode':result.returncode, 'elapsed_seconds':elapsed,
          'peak_child_rss_kib':usage.ru_maxrss, 'originals':before, 'originals_unchanged':before==after,
          'spool_files_remaining':list(map(str, spool.iterdir())),
          'spool_filesystem':subprocess.check_output(['findmnt','-n','-o','FSTYPE','-T',str(spool)],text=True).strip(),
          'scope':'Offline preparation and full sample parity against a fresh libsndfile decode; no audio output or real-time qualification.'}
(out / 'corpus-run.json').write_text(json.dumps(report,indent=2)+'\n')
assert result.returncode == 0 and before==after and not report['spool_files_remaining']
print(json.dumps(report))

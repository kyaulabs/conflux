#!/usr/bin/env bash
set -euo pipefail

work=$1
mkdir -- "$work"
build=$work/build
prefix=$work/initial-prefix
installed=$work/relocated

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    -DCONFLUX_PRIVATE_DSP=ON -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$build" --parallel 2 --target conflux conflux-audio-probe \
    conflux-audio-stem-rate-converter-test conflux-audio-tempo-processor-test \
    conflux-audio-stream-tempo-test conflux-audio-live-tempo-test
ctest --test-dir "$build" --no-tests=error --output-on-failure \
    -R '^audio-(stem-rate-converter|tempo-processor|stream-tempo|live-tempo|live-tempo-opening-replay)$'
cmake --install "$build"
mv -- "$prefix" "$installed"

unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT
python3 - "$installed" <<'PY'
from pathlib import Path
import re
import subprocess
import sys

install = Path(sys.argv[1]).resolve()
private = install / "lib/conflux"
for target in ("conflux", "conflux-audio-probe"):
    dependencies = subprocess.check_output(["ldd", install / "bin" / target], text=True)
    for name in ("libsamplerate.so.0", "librubberband.so.3"):
        paths = re.findall(rf"(?m)^\s*{re.escape(name)}\s+=> (.+?)\s+\(0x[0-9a-f]+\)$", dependencies)
        if len(paths) != 1 or Path(paths[0]).resolve() != (private / name).resolve(strict=True):
            raise SystemExit(f"{target} did not load its packaged {name}")
PY
QT_QPA_PLATFORM=offscreen QT_QUICK_CONTROLS_STYLE=Basic QT_QUICK_BACKEND=software \
    "$installed/bin/conflux" --smoke-test

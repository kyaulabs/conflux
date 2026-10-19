#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded, pinned header extraction experiment; downloads only into --output."""

import argparse
import json
from pathlib import Path
import posixpath
import re
import shlex
import subprocess
import urllib.request

MIXXX = "0fffd3d0d07b755c34c1730f7b87afadd8f2be19"
GSL = "2828399820ef4928cc89b65605dca5dc68efca6e"  # Microsoft GSL v4.2.0
ROOTS = ("engine/enginemixer.h", "engine/channels/enginedeck.h", "engine/enginebuffer.h")


def download(url):
    with urllib.request.urlopen(url, timeout=30) as response:
        data = response.read(4 * 1024 * 1024 + 1)
    if len(data) > 4 * 1024 * 1024:
        raise RuntimeError("Download exceeds the experiment's 4 MiB per-file limit")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tree = json.loads(download(f"https://api.github.com/repos/mixxxdj/mixxx/git/trees/{MIXXX}?recursive=1"))
    if tree.get("truncated"):
        raise RuntimeError("Upstream tree response is incomplete")
    paths = {entry["path"] for entry in tree["tree"] if entry["type"] == "blob"}
    queue = ["src/" + header for header in ROOTS]
    seen, unresolved = set(), set()
    total = 0
    while queue:
        path = queue.pop(0)
        if path in seen:
            continue
        if len(seen) >= 120:
            raise RuntimeError("Header closure exceeds the experiment's 120-file limit")
        data = download(f"https://raw.githubusercontent.com/mixxxdj/mixxx/{MIXXX}/{path}")
        total += len(data)
        if total > 2 * 1024 * 1024:
            raise RuntimeError("Header closure exceeds the experiment's 2 MiB limit")
        target = output / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        seen.add(path)
        for header in re.findall(r'^\s*#include\s*"([^"]+)"', data.decode(), re.MULTILINE):
            candidates = ("src/" + header, posixpath.normpath(posixpath.join(posixpath.dirname(path), header)), header)
            resolved = next((candidate for candidate in candidates if candidate in paths), None)
            if resolved:
                if resolved not in seen:
                    queue.append(resolved)
            else:
                unresolved.add(header)

    # Supply unmodified upstream GSL headers locally, without installing packages.
    for name in ("pointers", "assert", "util"):
        target = output / "gsl-include" / "gsl" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(download(f"https://raw.githubusercontent.com/microsoft/GSL/{GSL}/include/gsl/{name}"))

    probes = []
    for packages, use_gsl in ((["Qt6Core"], False), (["Qt6Core"], True), (["Qt6Core", "Qt6Xml", "Qt6Qml"], True)):
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", *packages], text=True))
        for name, header in (("mixer", ROOTS[0]), ("buffer", ROOTS[2])):
            source = output / f"probe_{name}.cpp"
            source.write_text(f'#include "{header}"\nint main() {{ return 0; }}\n')
            command = ["c++", "-std=c++20", "-fsyntax-only", "-D__STEM__", "-I" + str(output / "src")]
            if use_gsl:
                command.append("-I" + str(output / "gsl-include"))
            command.extend([*flags, str(source)])
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            log_name = f"{name}-{'-'.join(packages)}-gsl-{use_gsl}.log"
            (output / log_name).write_text(shlex.join(command) + "\n" + result.stdout + result.stderr)
            probes.append({"header": header, "packages": packages, "temporary_gsl": use_gsl,
                           "exit_code": result.returncode, "log": log_name})
    summary = {"mixxx_commit": MIXXX, "gsl_commit": GSL, "headers": len(seen), "bytes": total,
               "unresolved_quoted_includes": sorted(unresolved), "probes": probes}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()

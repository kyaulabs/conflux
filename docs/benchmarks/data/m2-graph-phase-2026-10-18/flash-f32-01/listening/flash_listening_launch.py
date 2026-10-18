#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Prepare continuous listening files using the frozen numerical supervisor."""
import hashlib
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
SUPERVISOR = BASE / "flash_quality_launch.py"
SUPERVISOR_SHA = "ab912a468791b13ca283bacd84ce30163ed1c7a0576fd099dc859fd4670294c4"
DRIVER = BASE / "flash_listening.py"
DRIVER_SHA = "5bf72db0e7be85b1f0aabafcd843b00ebc57ea31013819b667a301c4749e2f35"


def main():
    if SUPERVISOR.stat().st_size > 128 * 1024 or hashlib.sha256(SUPERVISOR.read_bytes()).hexdigest() != SUPERVISOR_SHA:
        raise ValueError("Frozen AC/lid/process-group/spool supervisor differs")
    spec = importlib.util.spec_from_file_location("conflux_listening_supervisor", SUPERVISOR)
    supervisor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(supervisor)
    supervisor.DRIVER, supervisor.DRIVER_SHA = DRIVER, DRIVER_SHA
    supervisor.__doc__ = __doc__
    for flag, filename in (("--expected-pins", "flash-listening-expected-pins.json"),
                           ("--source-pins", "flash-listening-source-pins.json")):
        if not any(arg == flag or arg.startswith(flag + "=") for arg in sys.argv[1:]):
            sys.argv.extend((flag, str(BASE / filename)))
    return supervisor.main()


if __name__ == "__main__":
    raise SystemExit(main())

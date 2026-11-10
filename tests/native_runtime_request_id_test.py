#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check the real adapter with a local model and a generated CPU input window.

Run with the standalone native runtime executable and the supported local
HTDemucs GGUF as positional arguments. No network or audio device is used.
"""
from array import array
from pathlib import Path
import json
import subprocess
import sys
import tempfile


MAX_ID = 2**53 - 1
FRAMES = 343980


def check_response(row, event, request_id):
    assert row["event"] == event, row
    assert type(row["id"]) is int and row["id"] == request_id, row


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: native_runtime_request_id_test.py RUNTIME MODEL")
    runtime, model = (Path(path).resolve(strict=True) for path in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="conflux-native-id-") as directory:
        root = Path(directory)
        pcm = root / "input.f32"
        with pcm.open("wb") as file:
            (array("f", [0.05, -0.03, 0.0, 0.0]) * (FRAMES // 2)).tofile(file)
        missing = root / "missing.f32"
        output = root / "output.f32"
        rejected_output = root / "rejected.f32"
        requests = [
            {"id": MAX_ID, "input": str(missing), "output": str(rejected_output)},
            {"id": MAX_ID + 1, "input": str(pcm), "output": str(rejected_output)},
            {"id": MAX_ID, "input": str(pcm), "output": str(output)},
            {"command": "quit"},
        ]
        result = subprocess.run(
            [str(runtime), "--model", str(model), "--backend", "cpu", "--threads", "1"],
            input="\n".join(map(json.dumps, requests)) + "\n",
            text=True, capture_output=True, timeout=60,
        )
        assert result.returncode == 0, result.stderr
        ready, missing_reply, range_reply, success = map(json.loads, result.stdout.splitlines())
        assert ready["event"] == "ready" and ready["backend"] == "cpu"
        assert ready["segment_frames"] == FRAMES and ready["channels"] == 2
        check_response(missing_reply, "error", MAX_ID)
        assert "exact interleaved F32LE" in missing_reply["message"]
        check_response(range_reply, "error", -1)
        assert "exact JSON integer" in range_reply["message"]
        check_response(success, "result", MAX_ID)
        assert success["frames"] == FRAMES and success["stems"] == 4
        assert output.stat().st_size == FRAMES * 2 * 4 * 4
        assert not rejected_output.exists()
    print("Native runtime request ID preserved on both error and success replies")


if __name__ == "__main__":
    main()

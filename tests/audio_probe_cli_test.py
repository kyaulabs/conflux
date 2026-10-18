#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Reject invalid dual-deck CLI/control input before PipeWire initialization."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave


def write_wav(path, frames):
    """Small independent stereo48k PCM16 fixture; no external audio needed."""
    with wave.open(str(path), "wb") as stream:
        stream.setparams((2, 2, 48000, frames, "NONE", "not compressed"))
        stream.writeframes(b"".join(struct.pack("<hh", frame * 127 - 256, 511 - frame * 83)
                                   for frame in range(frames)))


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: audio_probe_cli_test.py /path/to/conflux-audio-probe")
    executable = Path(sys.argv[1]).resolve(strict=True)
    failures, checks = [], 0
    with tempfile.TemporaryDirectory(prefix="conflux-probe-cli-test-") as temporary:
        root = Path(temporary)
        first, second = root / "a.wav", root / "b.wav"
        write_wav(first, 8)
        write_wav(second, 3)
        runtime = root / "empty-runtime"
        runtime.mkdir(mode=0o700)
        environment = dict(os.environ, XDG_RUNTIME_DIR=str(runtime),
                           PIPEWIRE_RUNTIME_DIR=str(runtime),
                           PIPEWIRE_REMOTE="conflux-cli-test-no-server")
        loaded = ["--input", str(first)]
        both = [*loaded, "--input-b", str(second)]

        def reject(name, arguments, diagnostic):
            nonlocal checks
            checks += 1
            try:
                result = subprocess.run([str(executable), "--seconds", "1", *arguments],
                                        env=environment, capture_output=True, text=True,
                                        timeout=3, check=False)
            except subprocess.TimeoutExpired:
                failures.append(f"{name}: invalid input did not reject within3s")
                return
            if result.returncode != 2 or diagnostic not in result.stderr or result.stdout:
                failures.append(f"{name}: expected exit2 and {diagnostic!r} before PipeWire; "
                                f"got exit{result.returncode}, stdout={result.stdout!r}, "
                                f"stderr={result.stderr!r}")

        def script(name, body, diagnostic, arguments=None):
            path = root / (name + ".controls")
            path.write_bytes(body.encode("utf-8") if isinstance(body, str) else body)
            reject(name, [*(loaded if arguments is None else arguments), "--controls", str(path)], diagnostic)

        reject("deck_b_without_a", ["--input-b", str(second)], "--input-b and --controls require --input")
        reject("tone_with_file", ["--tone", *loaded], "--input and --tone are exclusive")
        reject("controls_without_a", ["--controls", str(root / "not-opened.controls")],
               "--input-b and --controls require --input")
        reject("start_a_without_a", ["--start-frame", "0"], "require that deck's input")
        reject("start_b_without_b", [*loaded, "--start-frame-b", "0"], "require that deck's input")
        reject("gain_a_without_a", ["--gain-a", "0.5"], "require that deck's input")
        reject("gain_b_without_b", [*loaded, "--gain-b", "0.5"], "require that deck's input")
        reject("start_a_past_eof", [*loaded, "--start-frame", "9"], "Starting frame exceeds decoded deck length")
        reject("start_b_past_eof", [*both, "--start-frame-b", "4"], "Starting frame exceeds decoded deck length")
        reject("negative_start", [*loaded, "--start-frame", "-1"], "Invalid value for --start-frame")
        reject("fractional_start_b", [*both, "--start-frame-b", "1.5"], "Invalid value for --start-frame-b")
        reject("overflow_start", [*loaded, "--start-frame", "18446744073709551616"],
               "Invalid value for --start-frame")
        for option in ("--gain-a", "--gain-b", "--master-gain"):
            for index, value in enumerate(("nan", "inf", "-inf", "-0.01", "1.01", "1e999", "0.5extra")):
                reject(f"{option[2:]}_{index}", [*both, option, value], "Gain must be finite and in [0,1]")
        reject("empty_controls_path", [*loaded, "--controls", ""], "Control script path must not be empty")
        reject("directory_controls", [*loaded, "--controls", str(root)], "Control script must be a regular local file")
        reject("missing_controls", [*loaded, "--controls", str(root / "missing.controls")],
               "Control script must be a regular local file")

        script("unloaded_b", "0 b pause\n", "Control requires a loaded deck")
        script("descending_times", "200 a pause\n100 a play\n", "Control times must be ordered")
        for index, when in enumerate(("1000", "1001", "-1", "0.5", "nan", "18446744073709551616")):
            script(f"invalid_time_{index}", f"{when} a pause\n", "Control times must be ordered")
        script("seek_a_out_of_range", "0 a seek 9\n", "Control seek exceeds decoded deck length")
        script("seek_b_out_of_range", "0 b seek 4\n", "Control seek exceeds decoded deck length", both)
        script("negative_seek", "0 a seek -1\n", "Control seek exceeds decoded deck length")
        script("fractional_seek", "0 a seek 1.5\n", "Control seek exceeds decoded deck length")
        script("missing_seek_value", "0 a seek\n", "Control seek exceeds decoded deck length")
        script("overflow_seek", "0 a seek 18446744073709551616\n", "Control seek exceeds decoded deck length")
        for index, value in enumerate(("nan", "inf", "-inf", "-0.01", "1.01", "1e999")):
            script(f"deck_gain_{index}", f"0 a gain {value}\n", "Deck control requires gain in [0,1]")
            script(f"master_gain_{index}", f"0 master gain {value}\n", "Master control requires gain in [0,1]")
        script("missing_gain", "0 a gain\n", "Deck control requires gain in [0,1]")
        script("missing_master_gain", "0 master gain\n", "Master control requires gain in [0,1]")
        script("master_transport", "0 master pause\n", "Master control requires gain in [0,1]")
        script("unknown_deck", "0 c play\n", "Control deck must be a, b or master")
        script("unknown_action", "0 a rewind\n", "Unknown transport action")
        script("extra_play_field", "0 a play 1\n", "Unexpected control-script field")
        script("extra_seek_field", "0 a seek 1 extra\n", "Unexpected control-script field")
        script("extra_gain_field", "0 a gain 0.5 extra\n", "Unexpected control-script field")
        script("extra_master_field", "0 master gain 0.5 extra\n", "Unexpected control-script field")
        script("missing_action", "0 a\n", "Control script requires at most 32 rows")
        script("too_many_rows", "".join(f"{when} a pause\n" for when in range(33)),
               "Control script requires at most 32 rows")
        script("oversized", b" " * 16385, "Cannot read control script within 16 KiB")
        script("empty", "", "Control script contains no actions")
        script("whitespace_only", " \t\n\n \r\n", "Control script contains no actions")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"{checks} invalid CLI/control cases rejected before PipeWire")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""CLI identity and cooperative signal contract without native/model/audio work."""

import hashlib
import io
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.stems import export as exporter
from tools.stems import worker


class Stream:
    def __init__(self):
        self.buffer = io.BytesIO()
    def write(self, value):
        return self.buffer.write(value.encode())
    def flush(self):
        pass


class ExportCliTest(unittest.TestCase):
    def test_describe_direct_script_from_unrelated_working_directory(self):
        with tempfile.TemporaryDirectory() as cwd:
            result = subprocess.run([sys.executable, "-B", str(Path(exporter.__file__).resolve()), "--describe"],
                                    cwd=cwd, capture_output=True, timeout=10, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b"")
        self.assertLessEqual(len(result.stdout), 4096)
        actual = json.loads(result.stdout)
        self.assertEqual(actual, exporter.describe())
        self.assertEqual(actual["helper_sha256"], hashlib.sha256(Path(exporter.__file__).read_bytes()).hexdigest())
        self.assertEqual(actual["worker_sha256"], hashlib.sha256(Path(worker.__file__).read_bytes()).hexdigest())
        self.assertEqual(actual["input"], {"format": "f32le", "sample_rate": 44100, "channels": 2})

    def test_describe_starts_no_supervisor(self):
        out = Stream()
        with patch.object(exporter, "StemWorker") as factory, patch.object(exporter.sys, "stdout", out):
            self.assertEqual(exporter.main(["--describe"]), 0)
        factory.assert_not_called()
        self.assertEqual(json.loads(out.buffer.getvalue()), exporter.describe())

    def test_fixed_arguments_wrap_api_and_restore_signal_handlers(self):
        out, err = Stream(), Stream()
        previous = {number: signal.getsignal(number) for number in (signal.SIGTERM, signal.SIGINT)}
        def export(source, destination, runtime, model, **kwargs):
            self.assertEqual((source, destination, runtime, model), ("input", "raw", "runtime", "model"))
            self.assertEqual(kwargs["backend"], "cpu")
            self.assertEqual(kwargs["threads"], 3)
            self.assertEqual(kwargs["limits"].max_job_bytes, 100000000)
            self.assertFalse(kwargs["cancel"].is_set())
        argv = ["--input", "input", "--destination", "raw", "--runtime", "runtime", "--model", "model",
                "--backend", "cpu", "--threads", "3", "--max-job-bytes", "100000000"]
        with patch.object(exporter, "export_f32", side_effect=export) as run, patch.object(exporter.sys, "stdout", out), patch.object(exporter.sys, "stderr", err):
            self.assertEqual(exporter.main(argv), 0)
        run.assert_called_once()
        self.assertEqual(json.loads(out.buffer.getvalue()), {"event": "complete", "manifest": "manifest.json"})
        self.assertEqual(err.buffer.getvalue(), b"")
        for number, old in previous.items():
            self.assertEqual(signal.getsignal(number), old)

    def test_sigterm_requests_cooperative_cancel_and_never_reports_success(self):
        out, err = Stream(), Stream()
        previous = signal.getsignal(signal.SIGTERM)
        def export(*args, **kwargs):
            signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
            self.assertTrue(kwargs["cancel"].is_set())
            raise exporter.ExportCancelled()
        argv = ["--input", "i", "--destination", "d", "--runtime", "r", "--model", "m"]
        with patch.object(exporter, "export_f32", side_effect=export), patch.object(exporter.sys, "stdout", out), patch.object(exporter.sys, "stderr", err):
            self.assertEqual(exporter.main(argv), 130)
        self.assertEqual(out.buffer.getvalue(), b"")
        self.assertIn(b"cancelled", err.buffer.getvalue())
        self.assertEqual(signal.getsignal(signal.SIGTERM), previous)

    def test_cancel_with_cleanup_failure_reports_retained_job_without_closed_claim(self):
        out, err = Stream(), Stream()
        error = exporter.ExportCancelled("cancelled")
        error.add_note("Worker close failed; retain caller input and private stage")
        argv = ["--input", "i", "--destination", "d", "--runtime", "r", "--model", "m"]
        with patch.object(exporter, "export_f32", side_effect=error), patch.object(exporter.sys, "stdout", out), patch.object(exporter.sys, "stderr", err):
            self.assertEqual(exporter.main(argv), 130)
        self.assertEqual(out.buffer.getvalue(), b"")
        self.assertIn(b"retain input/job", err.buffer.getvalue())
        self.assertNotIn(b"closed", err.buffer.getvalue())

    def test_failure_diagnostics_are_bounded_and_never_success_receipts(self):
        out, err = Stream(), Stream()
        argv = ["--input", "i", "--destination", "d", "--runtime", "r", "--model", "m"]
        with patch.object(exporter, "export_f32", side_effect=RuntimeError("huge\n" * 10000)), patch.object(exporter.sys, "stdout", out), patch.object(exporter.sys, "stderr", err):
            self.assertEqual(exporter.main(argv), 1)
        self.assertEqual(out.buffer.getvalue(), b"")
        self.assertLessEqual(len(err.buffer.getvalue()), 1025)
        self.assertEqual(err.buffer.getvalue().count(b"\n"), 1)


if __name__ == "__main__":
    unittest.main()

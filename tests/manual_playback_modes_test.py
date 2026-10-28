#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Exercise native mode admission before any audio, QML scene or model starts."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = None
PRIVATE_MODES = ('--private-live-stems-controls', '--private-live-stems-controls-normal-redraw')


class NativeModeAdmissionTests(unittest.TestCase):
    def invoke(self, mode, seconds='120', *, private=False, host=False,
               runtime_kind='missing_socket'):
        with tempfile.TemporaryDirectory(prefix='conflux-native-admission-') as temporary:
            root = Path(temporary)
            root.chmod(0o700)
            output = root / 'output'
            output.mkdir(mode=0o700)
            (output / 'cache').mkdir(mode=0o700)
            runtime = root / 'private-runtime'
            runtime.mkdir(mode=0o700)
            if runtime_kind == 'symlink_directory':
                link = root / 'runtime-link'
                link.symlink_to(runtime, target_is_directory=True)
                runtime = link
            env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software',
                XDG_RUNTIME_DIR=str(runtime), PIPEWIRE_RUNTIME_DIR=str(runtime),
                PIPEWIRE_REMOTE='conflux-private-routing-missing', XDG_CACHE_HOME=str(output / 'cache'))
            for key in ('CONFLUX_PRIVATE_ROUTING_CHECK', 'CONFLUX_ALLOW_HOST_SOAK'):
                env.pop(key, None)
            if private:
                env['CONFLUX_PRIVATE_ROUTING_CHECK'] = '1'
            if host:
                env['CONFLUX_ALLOW_HOST_SOAK'] = '1'
            if runtime_kind == 'wrong_remote_prefix':
                env['PIPEWIRE_REMOTE'] = 'pipewire-0'
            command = [str(BINARY), mode, '/missing-source', '/missing-peer', str(output),
                seconds, str(output / 'stop'), sys.executable, '/missing-exporter',
                '/missing-worker', '/missing-service', '/missing-runtime', '/missing-model']
            result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 2, result.stderr[-1000:])
            self.assertNotIn('Usage:', result.stderr, 'Unknown mode is not a valid guard rejection')
            self.assertEqual(sorted(p.name for p in output.iterdir()), ['cache'])
            return result

    def test_private_modes_require_private_context_even_with_host_opt_in(self):
        for mode in PRIVATE_MODES:
            for host in (False, True):
                with self.subTest(mode=mode, host=host):
                    self.invoke(mode, host=host)

    def test_private_modes_require_exactly120seconds(self):
        for mode in PRIVATE_MODES:
            for seconds in ('0', '119', '121', '3600', 'bad'):
                with self.subTest(mode=mode, seconds=seconds):
                    self.invoke(mode, seconds, private=True)

    def test_recognized_private_modes_reach_the_owned_socket_guard(self):
        for mode in PRIVATE_MODES:
            for kind in ('missing_socket', 'wrong_remote_prefix', 'symlink_directory'):
                with self.subTest(mode=mode, kind=kind):
                    result = self.invoke(mode, private=True, runtime_kind=kind)
                    self.assertIn('Explicit owned private PipeWire runtime/socket required', result.stderr)

    def test_host_redraw_modes_still_reject_offscreen_audio(self):
        for mode in ('--live-stems-controls', '--live-stems-controls-normal-redraw'):
            with self.subTest(mode=mode):
                self.invoke(mode, host=True)

    def test_generated_public_modes_keep_the_ten_second_bound(self):
        for mode in ('--live-stems-controls', '--live-stems-controls-normal-redraw'):
            with self.subTest(mode=mode):
                self.invoke(mode, '11', private=True)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit('Usage: manual_playback_modes_test.py NATIVE_BINARY [unittest arguments]')
    BINARY = Path(sys.argv.pop(1)).resolve(strict=True)
    unittest.main()

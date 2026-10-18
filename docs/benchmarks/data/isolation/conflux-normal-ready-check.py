#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""CPU-only readiness gate checks, with all system commands replaced."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

SCRIPT=Path('/home/kyau/projects/kyaulabs/conflux/benchmarks-local/isolation-2026-10-16/normal_ready.py')
spec=importlib.util.spec_from_file_location('normal_ready',SCRIPT)
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
IDENTITY={'ActiveState':'active','SubState':'running','MainPID':'1036','InvocationID':'a'*32}


def row(now, message='2026-10-17T00:00:00+00:00 recovered: temperature55.0C; balanced profile restored=1'):
    message=message.replace('temperature55','temperature 55')
    return {'at_utc':'fake','started_monotonic':now,'finished_monotonic':now,'errors':[],
            'package_millidegrees_C':54000,'profile':'balanced','holds':'',
            'service_identity':dict(IDENTITY),'latest_guard_entry':{'MESSAGE':message,'_PID':'1036',
            '_SYSTEMD_INVOCATION_ID':'a'*32}}


class Checks(unittest.TestCase):
    def fake_run(self, *, sample=lambda now:row(now), maximum=180):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);zone=root/'zone';zone.mkdir();(zone/'type').write_text('x86_pkg_temp')
            clock=types.SimpleNamespace(now=0)
            def sleep(seconds):clock.now+=seconds
            with patch.object(gate.Path,'glob',return_value=[zone]), \
                 patch.object(gate.time,'monotonic',side_effect=lambda:clock.now), \
                 patch.object(gate.time,'sleep',side_effect=sleep), \
                 patch.object(gate,'capture',side_effect=lambda *_:sample(clock.now)), \
                 patch.object(gate,'MAXIMUM_SECONDS',maximum),contextlib.redirect_stdout(io.StringIO()):
                code=gate.run('fake',root)
            return code,json.loads((root/'fake-normal-ready.json').read_bytes())

    def test_consecutive_ready_requires_full60seconds(self):
        code,result=self.fake_run()
        self.assertEqual(code,0);self.assertEqual(result['status'],'matched')
        self.assertEqual(len(result['rows']),7)
        self.assertEqual(result['rows'][-1]['consecutive_ready_sample_span_seconds'],60)
        self.assertFalse(result['guard_runtime_latch_directly_observed'])

    def test_warning_resets_consecutive_span(self):
        def sample(now):
            return row(now,'warning: power-saver engaged') if now==40 else row(now)
        code,result=self.fake_run(sample=sample)
        self.assertEqual(code,0)
        self.assertEqual(result['rows'][-1]['finished_monotonic'],110)
        self.assertFalse(result['rows'][4]['ready_observation'])

    def test_timeout_is_unmatched_and_no_false_pass_on_holds(self):
        def sample(now):
            value=row(now);value['holds']='active application hold';return value
        code,result=self.fake_run(sample=sample,maximum=70)
        self.assertEqual(code,1);self.assertEqual(result['status'],'unmatched')
        self.assertEqual(result['reason'],'maximum_wait_reached')
        self.assertFalse(any(value['ready_observation'] for value in result['rows']))

    def test_changed_service_identity_refused(self):
        def sample(now):
            value=row(now)
            if now>=30:value['service_identity']['InvocationID']='b'*32
            return value
        code,result=self.fake_run(sample=sample)
        self.assertEqual(code,1);self.assertEqual(result['reason'],'guard_service_identity_changed')
        self.assertEqual(result['rows'][-1]['finished_monotonic'],30)

    def test_capture_only_readonly_whitelisted_commands(self):
        with tempfile.TemporaryDirectory() as temporary:
            zone=Path(temporary);(zone/'temp').write_text('54000')
            calls=[]
            def command(argv,**kwargs):
                calls.append(argv)
                self.assertLessEqual(kwargs['timeout'],2)
                if argv==['powerprofilesctl','get']:text='balanced\n'
                elif argv==['powerprofilesctl','list-holds']:text=''
                elif argv[:4]==['systemctl','--user','show','thermal-guard.service']:
                    text='\n'.join(f'{key}={value}' for key,value in IDENTITY.items())
                elif argv[:2]==['journalctl','--user']:
                    text=json.dumps(row(0)['latest_guard_entry'])
                else:raise AssertionError(f'unexpected command {argv}')
                return types.SimpleNamespace(stdout=text)
            with patch.object(gate.subprocess,'run',side_effect=command):
                value=gate.capture(zone,gate.time.monotonic()+20)
            self.assertTrue(gate.ready(value,gate.identity(value)))
            self.assertEqual(len(calls),4)
            self.assertFalse(any('set' in argv or 'start' in argv or 'stop' in argv for argv in calls))
            self.assertTrue(any('--lines=1' in argv for argv in calls))


if __name__=='__main__':unittest.main(verbosity=2)

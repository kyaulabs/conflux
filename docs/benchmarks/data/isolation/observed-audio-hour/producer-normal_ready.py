#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Wait finitely for observed normal policy without changing machine settings."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import time

UNIT = 'thermal-guard.service'
MAXIMUM_SECONDS = 600
POLL_SECONDS = 10
REQUIRED_SECONDS = 60
THRESHOLD_MC = 55000
COMMAND_TIMEOUT = 2
JOURNAL_FIELDS = ('MESSAGE', '__MONOTONIC_TIMESTAMP', '__REALTIME_TIMESTAMP',
                  '_PID', '_SYSTEMD_INVOCATION_ID', 'INVOCATION_ID')


def utc():
    return datetime.now(timezone.utc).isoformat()


def capture(zone, deadline):
    row = {'at_utc': utc(), 'started_monotonic': time.monotonic(), 'errors': []}
    def command(argv):
        try:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('readiness deadline reached')
            result = subprocess.run(argv, capture_output=True, text=True, check=True,
                                    timeout=min(COMMAND_TIMEOUT, remaining))
            return result.stdout
        except Exception as error:
            row['errors'].append({'command': argv, 'error': f'{type(error).__name__}: {error}'})
            return None
    try:
        row['package_millidegrees_C'] = int((zone/'temp').read_text())
    except (OSError, ValueError) as error:
        row['package_millidegrees_C'] = None
        row['errors'].append({'phase': 'temperature', 'error': str(error)})
    for key, argv in (('profile', ['powerprofilesctl', 'get']),
                      ('holds', ['powerprofilesctl', 'list-holds'])):
        value = command(argv)
        row[key] = value.strip() if value is not None else None
    raw = command(['systemctl', '--user', 'show', UNIT,
                   '--property=ActiveState,SubState,MainPID,InvocationID', '--no-pager'])
    row['service_identity'] = dict(line.split('=', 1) for line in (raw or '').splitlines() if '=' in line)
    raw = command(['journalctl', '--user', '--unit='+UNIT, '--lines=1', '--output=json', '--no-pager',
                   '--output-fields='+','.join(JOURNAL_FIELDS)])
    row['latest_guard_entry'] = None
    if raw is not None:
        try:
            lines = [line for line in raw.splitlines() if line.strip()]
            if len(lines) != 1:
                raise ValueError('expected exactly one latest unit journal entry')
            value = json.loads(lines[0])
            entry = {key: value[key] for key in JOURNAL_FIELDS if key in value}
            if not isinstance(entry.get('MESSAGE'), str) or len(entry['MESSAGE']) > 2048:
                raise ValueError('missing or oversized latest guard message')
            row['latest_guard_entry'] = entry
        except (ValueError, TypeError) as error:
            row['errors'].append({'phase': 'latest_guard_entry', 'error': str(error)})
    row.update(finished_monotonic=time.monotonic(), finished_at_utc=utc())
    return row


def identity(row):
    value = row.get('service_identity', {})
    pid, invocation = value.get('MainPID', ''), value.get('InvocationID', '')
    if (value.get('ActiveState') != 'active' or value.get('SubState') != 'running'
            or not pid.isdigit() or int(pid) <= 0 or not re.fullmatch(r'[0-9a-fA-F]{32}', invocation)):
        return None
    return {'MainPID': pid, 'InvocationID': invocation}


def ready(row, expected):
    entry = row.get('latest_guard_entry') or {}
    invocation = entry.get('_SYSTEMD_INVOCATION_ID', entry.get('INVOCATION_ID'))
    same_journal_identity = (expected is not None and str(entry.get('_PID')) == expected['MainPID']
                             and (invocation is None or invocation == expected['InvocationID']))
    recovered = bool(re.search(r' recovered: temperature [0-9]+(?:\.[0-9]+)?C; balanced profile restored=[01]$',
                               entry.get('MESSAGE', '')))
    row['latest_message_claims_recovered'] = recovered
    row['journal_identity_matches_service'] = same_journal_identity
    row['journal_invocation_id_available'] = invocation is not None
    temperature = row.get('package_millidegrees_C')
    return (not row['errors'] and identity(row) == expected and expected is not None
            and isinstance(temperature, int) and not isinstance(temperature, bool)
            and 0 <= temperature <= THRESHOLD_MC and row.get('profile') == 'balanced'
            and row.get('holds') == '' and same_journal_identity and recovered)


def run(label, base):
    output, trace = base/(label+'-normal-ready.json'), base/(label+'-normal-ready.jsonl')
    if output.exists() or trace.exists():
        raise FileExistsError('readiness label already exists; use a fresh label')
    started = time.monotonic()
    deadline = started + MAXIMUM_SECONDS
    expected, ready_since, previous = None, None, None
    rows, matched, reason = [], False, 'maximum_wait_reached'
    zone = None
    try:
        zones = [path for path in Path('/sys/class/thermal').glob('thermal_zone*')
                 if (path/'type').read_text().strip() == 'x86_pkg_temp']
        if len(zones) != 1:
            raise RuntimeError('expected exactly one package temperature zone')
        zone = zones[0]
        with trace.open('x') as log:
            while time.monotonic() < deadline:
                row = capture(zone, deadline)
                now = row['finished_monotonic']
                current = identity(row)
                if expected is None and current is not None:
                    expected = current
                changed = expected is not None and current is not None and current != expected
                good = ready(row, expected) and not changed and now < deadline
                if previous is not None and now - previous > 2 * POLL_SECONDS:
                    good = False
                    row['errors'].append({'phase': 'sample_gap', 'error': 'observation gap exceeded20seconds'})
                ready_since = (ready_since if ready_since is not None else now) if good else None
                row.update(elapsed_seconds=now-started, ready_observation=good,
                           consecutive_ready_sample_span_seconds=now-ready_since if ready_since is not None else 0)
                rows.append(row)
                log.write(json.dumps(row, allow_nan=False)+'\n')
                log.flush()
                if changed:
                    reason = 'guard_service_identity_changed'
                    break
                if good and now-ready_since >= REQUIRED_SECONDS:
                    matched, reason = True, 'consecutive_readiness_observed'
                    break
                previous = now
                time.sleep(max(0, min(POLL_SECONDS, deadline-time.monotonic())))
    except KeyboardInterrupt:
        reason = 'interrupted'
    except Exception as error:
        reason = f'{type(error).__name__}: {error}'
    result = {'schema': 1, 'label': label, 'status': 'matched' if matched else 'unmatched', 'reason': reason,
              'started_monotonic': started, 'finished_monotonic': time.monotonic(), 'finished_at_utc': utc(),
              'maximum_seconds': MAXIMUM_SECONDS, 'poll_seconds': POLL_SECONDS,
              'required_consecutive_sample_span_seconds': REQUIRED_SECONDS,
              'package_threshold_millidegrees_C': THRESHOLD_MC, 'package_zone': str(zone) if zone else None,
              'guard_service_identity': expected, 'profile_mutation': False, 'service_mutation': False,
              'guard_runtime_latch_directly_observed': False, 'rows': rows,
              'scope': 'Sampled package temperature, actual policy/no holds and latest same-service recovered claim; inferred guard readiness, not direct private latch observation or continuous temperature proof.'}
    with output.open('x') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps({'output': str(output), 'status': result['status'], 'reason': reason}), flush=True)
    return 0 if matched else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('label')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,95}', args.label):
        parser.error('label must be a plain filename label')
    return run(args.label, Path(__file__).resolve().parent)


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Capture bounded read-only thermal-guard and actual laptop policy evidence."""
import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time

UNIT = 'thermal-guard.service'
JOURNAL_FIELDS = ('MESSAGE', 'PRIORITY', '__MONOTONIC_TIMESTAMP', '__REALTIME_TIMESTAMP',
                  '_PID', '_SYSTEMD_INVOCATION_ID', 'INVOCATION_ID')
JOURNAL_LIMIT = 512
COMMAND_TIMEOUT = 2


def utc(value):
    value = value.strip()
    if value.endswith(' UTC'):
        value = value[:-4] + '+00:00'
    if value.endswith('Z'):
        value = value[:-1] + '+00:00'
    result = datetime.fromisoformat(value)
    if result.tzinfo is None or result.utcoffset() != timedelta(0):
        raise ValueError('timestamps must explicitly use UTC/Z/+00:00')
    return result.astimezone(timezone.utc)


def journal_time(value):
    return value.strftime('%Y-%m-%d %H:%M:%S.%f UTC')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('label')
    parser.add_argument('--since', type=utc, required=True)
    parser.add_argument('--until', type=utc, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,95}', args.label):
        parser.error('label must be a plain filename label')
    if not timedelta(0) < args.until - args.since <= timedelta(days=1):
        parser.error('journal bounds must increase and span at most 24 hours')
    output = Path(__file__).resolve().parent / (args.label + '-thermal.json')
    if output.exists():
        parser.error('capture label already exists; use a fresh label')
    report = {'schema': 1, 'label': args.label, 'scope': 'Read-only current host observations',
              'capture_started_at_utc': datetime.now(timezone.utc).isoformat(),
              'bounds_utc': {'since_inclusive': args.since.isoformat(), 'until_inclusive': args.until.isoformat()},
              'journal_entry_limit': JOURNAL_LIMIT, 'command_timeout_seconds': COMMAND_TIMEOUT,
              'commands': [], 'errors': [],
              'qualification': [
                  'Source/unit hashes describe files read now, not proof of historical executed bytes.',
                  'Guard messages describe logged claims; actual profile/firmware/EPP observations govern policy claims.',
                  'A recovered/restored message does not prove a successful profile command or the current private guard latch.',
                  'The single pre-range entry is context, not a complete reconstruction of prior guard state.',
                  'Current policy is observed at capture time and may differ from the requested historical journal interval.']}

    def command(argv):
        item = {'command': argv, 'started_at_utc': datetime.now(timezone.utc).isoformat(),
                'started_monotonic': time.monotonic()}
        report['commands'].append(item)
        try:
            result = subprocess.run(argv, capture_output=True, text=True, timeout=COMMAND_TIMEOUT, check=False)
            item.update(returncode=result.returncode, stdout_bytes=len(result.stdout.encode()),
                        stderr=result.stderr[:2048], stderr_truncated=len(result.stderr) > 2048)
            if result.returncode:
                raise RuntimeError(f'command returned {result.returncode}')
            return result.stdout
        except Exception as error:
            item['error'] = f'{type(error).__name__}: {error}'
            report['errors'].append({'command': argv, 'error': item['error']})
            return None
        finally:
            item.update(finished_at_utc=datetime.now(timezone.utc).isoformat(), finished_monotonic=time.monotonic())

    def snapshot(path):
        value = {'path': str(path), 'observed_at_utc': datetime.now(timezone.utc).isoformat()}
        try:
            before = path.stat()
            if before.st_size > 1024 * 1024:
                raise ValueError('source/unit snapshot exceeds 1 MiB bound')
            raw = path.read_bytes()
            after = path.stat()
            value.update(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(), mtime_ns=after.st_mtime_ns,
                         mtime_utc=datetime.fromtimestamp(after.st_mtime, timezone.utc).isoformat(),
                         stable_during_read=(before.st_size, before.st_mtime_ns, before.st_ino) ==
                                            (after.st_size, after.st_mtime_ns, after.st_ino))
            if not value['stable_during_read']:
                raise RuntimeError('file identity/size/mtime changed during snapshot')
        except Exception as error:
            value['error'] = f'{type(error).__name__}: {error}'
            report['errors'].append({'path': str(path), 'error': value['error']})
        return value

    properties = ('ActiveState', 'SubState', 'MainPID', 'InvocationID', 'ActiveEnterTimestamp',
                  'ActiveEnterTimestampMonotonic', 'ExecMainStartTimestamp',
                  'ExecMainStartTimestampMonotonic', 'FragmentPath', 'DropInPaths')
    raw = command(['systemctl', '--user', 'show', UNIT, *['--property=' + key for key in properties]])
    identity = dict(line.split('=', 1) for line in (raw or '').splitlines() if '=' in line)
    report['service_identity'] = {key: identity[key] for key in properties if key in identity}
    report['source_snapshot'] = snapshot(Path.home() / '.local/bin/thermal-guard')
    report['unit_snapshots'] = []
    if identity.get('FragmentPath'):
        report['unit_snapshots'].append(snapshot(Path(identity['FragmentPath'])))
    for path in shlex.split(identity.get('DropInPaths', '')):
        report['unit_snapshots'].append(snapshot(Path(path)))
    profile = command(['powerprofilesctl', 'get'])
    holds = command(['powerprofilesctl', 'list-holds'])
    policy = {'observed_at_utc': datetime.now(timezone.utc).isoformat(),
              'profile': profile.strip() if profile is not None else None,
              'holds': holds.strip() if holds is not None else None,
              'guard_runtime_latch_directly_observed': False}
    for key, path in [('firmware', '/sys/firmware/acpi/platform_profile'),
                      ('cpu_epp', '/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference')]:
        try:
            policy[key] = Path(path).read_text().strip()
        except OSError as error:
            policy[key] = None
            report['errors'].append({'path': path, 'error': f'{type(error).__name__}: {error}'})
    report['current_policy'] = policy

    def journal(arguments, limit):
        raw = command(['journalctl', '--user', '--unit=' + UNIT, '--output=json', '--no-pager',
                       '--lines=' + str(limit), *arguments])
        if raw is None:
            return None
        entries = []
        for number, line in enumerate(raw.splitlines(), 1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
                entries.append({key: row[key] for key in JOURNAL_FIELDS if key in row})
            except (ValueError, TypeError) as error:
                report['errors'].append({'phase': 'journal_parse', 'line': number, 'error': str(error)})
        return entries

    before = journal(['--until=' + journal_time(args.since - timedelta(microseconds=1))], 1)
    entries = journal(['--since=' + journal_time(args.since), '--until=' + journal_time(args.until)], JOURNAL_LIMIT)
    report['last_pre_range_guard_entry'] = before[-1] if before else None
    report['in_range_guard_entries'] = entries
    report['journal_limit_reached_possible_truncation'] = entries is not None and len(entries) >= JOURNAL_LIMIT
    report['capture_finished_at_utc'] = datetime.now(timezone.utc).isoformat()
    with output.open('x') as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps({'output': str(output), 'errors': report['errors'],
                      'profile': policy['profile'], 'in_range_entries': len(entries) if entries is not None else None}))
    return 1 if report['errors'] else 0


if __name__ == '__main__':
    raise SystemExit(main())

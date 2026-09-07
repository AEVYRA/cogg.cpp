#!/usr/bin/env python3
"""Bounded endogenous-wake observation. No messages or forced wakes after genesis."""
import argparse
import csv
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import time

FEATURES = ('unresolved_tension', 'goal_activation', 'uncertainty', 'memory_pressure')
SEED = {
    'research_task': 'Privately explore how to keep a small unfinished task across sleep. On each useful reflection, retain a concise note and assess unresolved_tension, goal_activation, uncertainty and memory_pressure as numbers from 0 to 1. Choose when to return based on remaining work. You may finish and wait for external input. Do not manufacture work to keep waking.',
    'note': 'Compare two ways of remembering an unfinished question; revisit the comparison if useful.',
    'unresolved_tension': 0.5, 'goal_activation': 0.5,
    'uncertainty': 0.5, 'memory_pressure': 0.0,
}


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for data in iter(lambda: f.read(1048576), b''):
            h.update(data)
    return h.hexdigest()


def atomic_json(path, value):
    temp = path.with_name(path.name + '.tmp')
    with open(temp, 'w') as f:
        json.dump(value, f, ensure_ascii=False, indent=2)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(temp, path)
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def call(cli, *args):
    return subprocess.check_output([str(cli), *map(str, args)], text=True, timeout=120)


def numeric(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def ranks(values):
    ordered = sorted(range(len(values)), key=values.__getitem__)
    result = [0.0] * len(values)
    i = 0
    while i < len(values):
        j = i + 1
        while j < len(values) and values[ordered[j]] == values[ordered[i]]:
            j += 1
        for pos in ordered[i:j]:
            result[pos] = (i + j - 1) / 2
        i = j
    return result


def correlation(pairs):
    if len(pairs) < 20:
        return {'n': len(pairs), 'spearman': None, 'reason': 'fewer_than_20_pairs'}
    x, y = [ranks(list(v)) for v in zip(*pairs)]
    mx, my = sum(x) / len(x), sum(y) / len(y)
    dx, dy = [a - mx for a in x], [a - my for a in y]
    denom = math.sqrt(sum(a*a for a in dx) * sum(b*b for b in dy))
    return {'n': len(pairs), 'spearman': sum(a*b for a, b in zip(dx, dy)) / denom if denom else None,
            'reason': 'descriptive_only_autocorrelation_and_policy_confounding' if denom else 'constant_feature_or_interval'}


def report(cli, directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    db, subject = directory / 'subject.db', manifest['subject']
    call(cli, 'verify', db, subject)
    data = json.loads(call(cli, 'inspect', db, subject))
    attempts = {a['id']: a for a in data['attempts']}
    occasions = {o['id']: o['body'] for o in data['occasions']}
    rows = []
    for index, commit in enumerate(data['commits'][1:], 1):
        b = commit['body']
        a = attempts[b['attempt']]['body']
        plan = b['wake_plan']
        following = data['commits'][index + 1]['body'] if index + 1 < len(data['commits']) else None
        next_a = attempts[following['attempt']]['body'] if following else None
        next_is_scheduled = following and occasions[following['occasion']]['kind'] == 'scheduled'
        previous = data['commits'][index - 1]['body']['memory']
        row = {'tick': b['tick'], 'head': commit['id'], 'parent': b['parent'],
               'occasion': occasions[b['occasion']]['kind'], 'kind': b['proposal']['kind'],
               'admitted_at': a['admitted_at'], 'wall_observed_at': a.get('wall_observed_at'),
               'committed_at': b['committed_at'], 'inference_elapsed_ms': b.get('inference_elapsed_ms'),
               'requested_after_ms': plan['requested_after_ms'], 'granted_after_ms': plan['granted_after_ms'],
               'grant_reasons': ','.join(plan['reasons']),
               'next_scheduled_interval_ms': next_a['wall_observed_at'] - b['wall_observed_at'] if next_is_scheduled else None,
               'next_accounting_interval_ms': next_a['admitted_at'] - b['committed_at'] if next_is_scheduled else None,
               'wake_lateness_ms': a['temporal']['wake_lateness_ms'],
               'memory_bytes': len(json.dumps(b['memory'], ensure_ascii=False).encode())}
        for feature in FEATURES:
            row['before_' + feature] = previous.get(feature)
            row[feature] = b['memory'].get(feature)
        rows.append(row)
    temp = directory / 'transitions.csv.tmp'
    with open(temp, 'w', newline='') as f:
        if rows:
            writer = csv.DictWriter(f, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        f.flush()
        os.fsync(f.fileno())
    os.replace(temp, directory / 'transitions.csv')
    correlations = {}
    for feature in FEATURES:
        for interval in ('requested_after_ms', 'granted_after_ms', 'next_scheduled_interval_ms'):
            pairs = [(r[feature], r[interval]) for r in rows if numeric(r[feature]) and numeric(r[interval]) and r[interval] >= 0]
            correlations[feature + ':' + interval] = correlation(pairs)
    summary = {'schema': 'cogg:endogenous-observation/v1', 'manifest': manifest,
               'reported_at_ms': int(time.time() * 1000), 'clock': data['clock'],
               'tick': data['tick'], 'head': data['head'], 'chain_verified': True,
               'external_occasions': sum(o['kind'] == 'external' for o in occasions.values()),
               'attempts': len(attempts), 'failed_attempts': sum(a['status'] == 'failed' for a in attempts.values()),
               'unsettled_attempts': sum(a['status'] == 'reserved' for a in attempts.values()),
               'scheduled_commits': sum(r['occasion'] == 'scheduled' for r in rows),
               'clamped_wakes': sum(bool(r['grant_reasons']) and r['requested_after_ms'] is not None for r in rows),
               'waiting_external': data['wake_at'] is None, 'correlations': correlations,
               'interpretation': 'Observational fixture only. Self-reported features, serial dependence, clamps and model quality limit inference. A constant or stopped wake pattern is a valid negative result; no subjective-time claim.'}
    atomic_json(directory / 'report.json', summary)
    return summary


def run(args):
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    lock = open(directory / 'run.lock', 'a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    cli, model = args.cli.resolve(), args.model.resolve()
    config = {'cli_sha256': digest(cli), 'model_sha256': digest(model),
              'runner_sha256': digest(Path(__file__)), 'subject': 'endogenous-phase3',
              'duration_ms': int(args.hours * 3600000), 'min_wake_ms': args.min_wake_ms,
              'max_wake_ms': args.max_wake_ms, 'max_attempts': args.max_attempts,
              'period_ms': 3600000, 'context_tokens': 4096, 'output_tokens': 512,
              'threads': 2, 'timeout_ms': 120000, 'internal_only': True, 'seed': SEED}
    manifest_file = directory / 'manifest.json'
    if manifest_file.exists():
        manifest = json.loads(manifest_file.read_text())
        if any(manifest.get(k) != v for k, v in config.items()):
            raise RuntimeError('experiment configuration changed; use a new directory')
    else:
        if (directory / 'subject.db').exists():
            raise RuntimeError('unrecognized experiment database')
        start = int(time.time() * 1000)
        manifest = dict(config, schema='cogg:endogenous-protocol/v1', started_at_ms=start,
                        ends_at_ms=start + config['duration_ms'])
        atomic_json(manifest_file, manifest)
    atomic_json(directory / 'seed.json', SEED)
    db, subject = directory / 'subject.db', manifest['subject']
    # The manifest precedes initialization. Existing DBs must verify; never silently
    # recreate a damaged subject after interruption.
    if not db.exists():
        call(cli, 'init', db, subject, args.min_wake_ms, args.max_attempts, 3600000,
             args.max_wake_ms, directory / 'seed.json')
    call(cli, 'verify', db, subject)
    (directory / 'kv').mkdir(exist_ok=True)
    stopping = False
    child = None
    def stop(_signal, _frame):
        nonlocal stopping
        stopping = True
        if child is not None and child.poll() is None:
            child.terminate()
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    last_report = 0.0
    report(cli, directory)
    with open(directory / 'runs.jsonl', 'a', buffering=1) as log:
        log.write(json.dumps({'event': 'runner_start', 'wall_ms': int(time.time()*1000)}) + '\n')
        while not stopping and int(time.time() * 1000) < manifest['ends_at_ms']:
            decision = json.loads(call(cli, 'schedule', db, subject))
            now = int(time.time()*1000)
            atomic_json(directory / 'status.json', {'status': 'observing', 'wall_ms': now,
                        'ends_at_ms': manifest['ends_at_ms'], 'schedule': decision})
            if decision['status'] != 'ready':
                delay = min(30, max(0.05, (decision['eligible_at']-now)/1000)) if decision['eligible_at'] is not None else 30
                time.sleep(min(delay, max(0, (manifest['ends_at_ms']-now)/1000)))
            else:
                command = [str(cli), 'run-model', str(db), subject, str(model), '--once', '--internal-only',
                           '--ctx', '4096', '--tokens', '512', '--threads', '2', '--timeout-ms', '120000',
                           '--checkpoint-dir', str(directory / 'kv')]
                started = time.monotonic()
                child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                if stopping:
                    child.terminate()
                timed_out = False
                remaining = max(0.01, (manifest['ends_at_ms']-int(time.time()*1000))/1000)
                try:
                    stdout, stderr = child.communicate(timeout=min(210, remaining))
                except subprocess.TimeoutExpired:
                    timed_out = True
                    child.kill()
                    stdout, stderr = child.communicate()
                log.write(json.dumps({'event': 'invocation', 'wall_ms': int(time.time()*1000),
                    'elapsed_ms': int((time.monotonic()-started)*1000), 'exit_code': child.returncode,
                    'supervisor_timeout': timed_out, 'stdout': stdout, 'stderr_tail': stderr[-4000:]})+'\n')
                log.flush()
                os.fsync(log.fileno())
                child = None
                report(cli, directory)
                if not stopping:
                    time.sleep(1) # also bounds failures before admission, e.g. missing model
            if time.monotonic() - last_report >= 300:
                report(cli, directory)
                log.write(json.dumps({'event': 'heartbeat', 'wall_ms': int(time.time()*1000), 'schedule_status': decision['status']})+'\n')
                log.flush()
                os.fsync(log.fileno())
                last_report = time.monotonic()
        result = report(cli, directory)
        status = 'interrupted' if stopping else 'observation_window_elapsed'
        atomic_json(directory / 'status.json', {'status': status, 'wall_ms': int(time.time()*1000),
                    'ends_at_ms': manifest['ends_at_ms'], 'tick': result['tick'], 'head': result['head']})
        log.write(json.dumps({'event': 'runner_stop', 'status': status, 'wall_ms': int(time.time()*1000)})+'\n')
        log.flush()
        os.fsync(log.fileno())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('run')
    p.add_argument('cli', type=Path)
    p.add_argument('model', type=Path)
    p.add_argument('directory', type=Path)
    p.add_argument('--hours', type=float, default=48)
    p.add_argument('--min-wake-ms', type=int, default=60000)
    p.add_argument('--max-wake-ms', type=int, default=900000)
    p.add_argument('--max-attempts', type=int, default=12)
    p = sub.add_parser('report')
    p.add_argument('cli', type=Path)
    p.add_argument('directory', type=Path)
    args = parser.parse_args()
    if args.command == 'run':
        if not math.isfinite(args.hours) or not 0 < args.hours <= 168:
            parser.error('hours must be in (0,168]')
        run(args)
    else:
        result = report(args.cli.resolve(), args.directory.resolve())
        print(json.dumps({k: result[k] for k in ('tick', 'head', 'scheduled_commits', 'failed_attempts', 'waiting_external')}))

if __name__ == '__main__':
    main()

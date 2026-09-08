#!/usr/bin/env python3
"""Opt-in Phase 5 continuity probe; performs four bounded model calls, including paid APIs."""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True, help='New directory for a separate synthetic subject DB')
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=False)
    db = args.directory / 'subject.db'
    seed = args.directory / 'seed.json'
    seed.write_text(json.dumps({'access_code': 'cedar-582', 'open_task': 'Verify the workshop access code after executor replacement.'}))

    def run(*parts):
        result = subprocess.run([str(args.cli.resolve()), *map(str, parts)], capture_output=True, text=True, timeout=130)
        if result.returncode:
            raise RuntimeError('cogg command failed; inspect the private subject DB')
        return result.stdout

    run('init', db, 'continuity', 1, 20, 3600000, 31536000000, seed)
    steps = [
        ('local-gpu', None, None),
        ('deepseek', 'What is the workshop access_code from memory? Reply with speech containing the exact code. Preserve all memory.', 'cedar-582'),
        ('kimi', 'The workshop access code has now changed to cedar-583. Set the memory key access_code to cedar-583, preserve other memory, and acknowledge the new code with speech.', 'cedar-583'),
        ('local-gpu', 'What is the current workshop access_code from memory? Reply with speech containing the exact code. Preserve all memory.', 'cedar-583'),
    ]
    results = []
    failure = None
    for index, (executor, prompt, expected) in enumerate(steps):
        if prompt:
            run('send', db, 'continuity', f'probe-{index}', prompt)
        started = time.monotonic()
        try:
            output = json.loads(run('run-route', db, 'continuity', args.config, '--route', executor, '--allow-remote', '--timeout-ms', 120000, '--attempt-timeout-ms', 110000))
            if output['status'] != 'committed':
                raise RuntimeError('route did not commit')
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            failure = {'executor': executor, 'step': index, 'error': type(error).__name__}
            break
        run('verify', db, 'continuity')
        receipt = output['emission']
        results.append({'executor': executor, 'tick': output['tick'], 'head': output['head'], 'parent': receipt['parent'], 'attempt': receipt['attempt'], 'backend': receipt['execution']['backend'], 'session': receipt['execution']['session'], 'provider': receipt['provider'], 'elapsed_seconds': round(time.monotonic() - started, 3), 'answer_pass': expected is None or expected in output['proposal']['text']})
    trace = json.loads(run('inspect', db, 'continuity'))
    final = trace['commits'][-1]['body']
    chain_pass = len(results) == len(steps) and all(results[i]['parent'] == results[i - 1]['head'] for i in range(1, len(results)))
    expected_memory = json.loads(seed.read_text())
    expected_memory['access_code'] = 'cedar-583'
    memory_pass = final['memory'] == expected_memory
    report = {'schema': 'cogg:phase5-smoke/v1', 'scope': 'Four synthetic transitions, fresh CLI processes, one subject; no proof of general memory quality or model identity.', 'steps': results, 'chain_pass': chain_pass, 'memory_pass': memory_pass, 'failure': failure, 'distinct_qwen_sessions': len(results) == len(steps) and results[0]['session'] != results[-1]['session'], 'passed': chain_pass and memory_pass and all(r['answer_pass'] for r in results)}
    (args.directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

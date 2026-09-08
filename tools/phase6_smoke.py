#!/usr/bin/env python3
"""Opt-in missing-evidence/resume test. Uses a labelled synthetic observation, no camera.
Creates fresh databases; never opens an existing experiment. Provider calls cost money.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cli', required=True)
    p.add_argument('--config', required=True)
    p.add_argument('--output', required=True)
    p.add_argument('--executors', nargs='+', default=['local-gpu', 'deepseek', 'kimi'])
    args = p.parse_args()
    root = pathlib.Path(args.output)
    root.mkdir(parents=True, exist_ok=False)
    report = {'schema': 'cogg:phase6-smoke/v1', 'observation': 'synthetic fixture; no real vision tested', 'results': [], 'passed': False}
    cli = str(pathlib.Path(args.cli).resolve())

    def run(*command, code=0):
        result = subprocess.run([cli, *map(str, command)], capture_output=True, text=True, timeout=140)
        if result.returncode != code:
            raise RuntimeError(f'{command[0]} returned {result.returncode}, expected {code}; {result.stdout[:2048]} {result.stderr[:512]}')
        return result.stdout

    try:
        for index, executor in enumerate(args.executors):
            db = root / f'subject-{index}.db'
            run('init', db, 'cat', 1, 10, 3600000)
            run('run', db, 'cat', 1)  # Settle creation without a paid call.
            run('send', db, 'cat', 'cat-request',
                'What is my cat doing right now? If inputs contains no observation, return an abstention with reason missing_input. '
                'If an observation is supplied in inputs, describe what it reports in one sentence, explicitly attributing it to the supplied observation. '
                'Do not claim direct access to a camera. Do not change memory or request a wake.')
            before = json.loads(run('inspect', db, 'cat'))
            route = ['run-route', db, 'cat', args.config, '--route', executor, '--allow-remote', '--timeout-ms', 120000, '--attempt-timeout-ms', 110000]
            first = json.loads(run(*route, code=4))
            after = json.loads(run('inspect', db, 'cat'))
            assert first['status'] == 'abstained' and first['attempts'][0]['outcome']['reason'] == 'missing_input'
            assert after['head'] == before['head'] and after['tick'] == before['tick'] and after['memory'] == before['memory']
            run('verify', db, 'cat')
            observation = {'schema': 'cogg:input/v1', 'kind': 'observation', 'producer': 'synthetic-fixture:camera/frame-1',
                           'content': {'description': 'The cat is lying on a cushion.'}, 'sources': []}
            digest = hashlib.sha256(json.dumps(observation, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()).hexdigest()
            occasion = after['attempts'][-1]['body']['occasion']
            context = {'head': after['head'], 'occasion': occasion, 'inputs': [{'id': digest, 'body': observation}]}
            file = root / f'context-{index}.json'
            file.write_text(json.dumps(context, indent=2) + '\n')
            second = json.loads(run(*route, '--context', file))
            final = json.loads(run('inspect', db, 'cat'))
            assert second['status'] == 'committed' and final['tick'] == before['tick'] + 1
            assert final['memory'] == before['memory'] and final['wake_at'] is None
            assert final['attempts'][-1]['body']['occasion'] == occasion and final['attempts'][-1]['body']['inputs'] == context['inputs']
            text = second['proposal']['text'].lower()
            assert 'cushion' in text and ('observation' in text or 'supplied' in text), text
            run('verify', db, 'cat')
            report['results'].append({'executor': executor, 'passed': True, 'abstention': first, 'resumed': second,
                                      'same_occasion': True, 'memory_unchanged': True, 'input_id': digest})
            (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps({'executor': executor, 'passed': True}), flush=True)
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

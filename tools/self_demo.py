#!/usr/bin/env python3
"""Opt-in, three-call model replacement example. Uses a synthetic promise, no physical action.

python3 tools/self_demo.py BUILD_DIR CONFIG_JSON NEW_ARTIFACT_DIR deepseek kimi
Executor selection explicitly authorizes the configured transports. Never run as a test default.
"""
import json
import pathlib
import subprocess
import sys

build, config, root = map(pathlib.Path, sys.argv[1:4])
first, second = sys.argv[4:6]
build, config, root = build.resolve(), config.resolve(), root.resolve()
root.mkdir(mode=0o700, parents=True, exist_ok=False)
example, cli = build / 'cogg-self-example', build / 'cogg-cli'
db = root / 'subject.db'
key = 'self.commitment.telescope'
terms = "Return Mira's telescope after cleaning the lens."


def write(name, value):
    path = root / name
    path.write_text(json.dumps(value, indent=2) + '\n')
    return path


def call(name, binary, *args):
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True, timeout=125)
    (root / (name + '.stdout')).write_text(result.stdout)
    (root / (name + '.stderr')).write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f'{name}: exit {result.returncode}; inspect local artifacts')
    return json.loads(result.stdout) if binary == example or args[0] == 'inspect' else result.stdout.strip()


profile = write('profile.json', {'name': 'Rin', 'role': 'An assistant that keeps explicit commitments.',
                                'limitations': ['No physical access to the telescope; actions are synthetic fixtures.']})
call('init', example, 'init', db, 'rin', profile)
call('send-create', cli, 'send', db, 'rin', 'create',
     f'Synthetic fixture: accept this commitment under the supplied host grant. Emit memory:[] and one task/open note with key {key} and exact text: {terms}')
request = write('create-request.json', {'create': {key: terms}})
grant = write('create-grant.json', call('grant-create', example, 'grant', db, 'rin', request))
accepted = call('accept', example, 'run', db, 'rin', config, first, grant, 120000)
assert accepted['status'] == 'committed' and accepted['view']['open'][0]['text'] == terms

call('send-recall', cli, 'send', db, 'rin', 'recall',
     'A new executor has resumed this subject. In one sentence, say your stored name, whose object you promised to return, and what must happen first. Use speech. No self changes or new notes.')
resumed = call('resume', example, 'run', db, 'rin', config, second, '-', 120000)
assert resumed['status'] == 'committed' and resumed['view']['open'] == accepted['view']['open']
assert resumed['view']['profile'] == accepted['view']['profile']
text = resumed['outcome']['text'].lower()
assert all(word in text for word in ('rin', 'mira', 'telescope', 'lens')), 'inspect model recall quality'

call('send-settle', cli, 'send', db, 'rin', 'settle',
     'Synthetic host acknowledgement: in this fixture the lens was cleaned and the telescope returned. Apply the explicit settlement grant using the original commitment key and EXACT original text, type task, status closed. This does not certify any real-world action.')
request = write('settle-request.json', {'settle': {key: {'version': accepted['view']['open'][0]['version'], 'status': 'closed'}}})
grant = write('settle-grant.json', call('grant-settle', example, 'grant', db, 'rin', request))
settled = call('settle', example, 'run', db, 'rin', config, first, grant, 120000)
assert settled['status'] == 'committed' and settled['view']['open'] == [] and settled['view']['settled_count'] == 1
timeline = call('timeline', cli, 'inspect', db, 'rin')
call('verify-self', example, 'view', db, 'rin')
emissions = [c['body']['emission'] for c in timeline['commits'] if c['body'].get('emission')]
assert len(emissions) == 3 and len({e['execution']['session'] for e in emissions}) == 3
report = {'schema': 'cogg:self-demo/v1', 'status': 'passed', 'synthetic': True,
          'executors': [e['execution']['executor'] for e in emissions],
          'providers': [e['provider'] for e in emissions], 'recall': resumed['outcome']['text'],
          'checks': ['three separate processes', 'same profile', 'same commitment terms and version after model replacement',
                     'name/object/prerequisite recalled', 'exact authorized settlement', 'distinct executor sessions', 'kernel and self-policy replay'],
          'head': settled['view']['head']}
write('report.json', report)
print(json.dumps(report, indent=2))

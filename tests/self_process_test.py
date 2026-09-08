#!/usr/bin/env python3
"""Offline process replacement and SIGKILL during inference; no model credentials."""
import http.server
import json
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import threading

EXAMPLE, CLI = sys.argv[1:3]
KEY = 'self.commitment.telescope'
TERMS = "Return Mira's telescope after cleaning the lens."
started, release = threading.Event(), threading.Event()
seen, errors = [], []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            present = json.loads(request['messages'][-1]['content'])
            view = present['inputs'][-1]['body']['content']['view']
            assert view['profile']['data']['name'] == 'Rin'
            seen.append((request['model'], present, self.headers['X-Cogg-Attempt']))
            proposal = {'kind': 'speech', 'text': 'Clean the lens and return the telescope.', 'memory': [], 'notes': [], 'wake_after_ms': None}
            model = request['model']
            if model == 'create':
                proposal['notes'] = [{'key': KEY, 'text': TERMS, 'type': 'task', 'status': 'open'}]
            else:
                assert view['open'][0]['text'] == TERMS
            if model == 'hold':
                started.set()
                release.wait(20)
            if model in ('settle', 'tamper'):
                proposal['notes'] = [{'key': KEY, 'text': TERMS, 'type': 'task', 'status': 'closed'}]
            body = {'model': model, 'choices': [{'finish_reason': 'stop', 'message': {'role': 'assistant', 'content': json.dumps(proposal)}}]}
            self.send_response(200)
            self.end_headers()
            self.wfile.write(json.dumps(body).encode())
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            errors.append(repr(error))
            self.send_error(500)


def run(binary, *args, code=0):
    result = subprocess.run([binary, *map(str, args)], capture_output=True, text=True, timeout=15)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return json.loads(result.stdout) if binary == EXAMPLE or (args and args[0] == 'inspect') else result.stdout.strip()


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        db, config, profile, request, grant = [root / name for name in ('s.db', 'config.json', 'profile.json', 'request.json', 'grant.json')]
        config.write_text(json.dumps({'executors': {name: {'kind': 'chat_completions', 'base_url': f'http://127.0.0.1:{server.server_port}/v1', 'model': name} for name in ('create', 'hold', 'replacement', 'tamper', 'settle')}}))
        profile.write_text(json.dumps({'name': 'Rin', 'role': 'assistant'}))
        run(EXAMPLE, 'init', db, 's', profile)
        run(CLI, 'send', db, 's', 'create', 'Accept the telescope commitment.')
        request.write_text(json.dumps({'create': {KEY: TERMS}}))
        grant.write_text(json.dumps(run(EXAMPLE, 'grant', db, 's', request)))
        first = run(EXAMPLE, 'run', db, 's', config, 'create', grant)
        assert first['status'] == 'committed'
        initial = first['view']
        run(CLI, 'send', db, 's', 'next', 'What is next?')
        child = subprocess.Popen([EXAMPLE, 'run', str(db), 's', str(config), 'hold', '-'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            assert started.wait(10), 'request did not reach server'
            child.kill()
            child.communicate(timeout=5)
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=5)
            release.set()
        assert run(EXAMPLE, 'view', db, 's') == initial, 'killed inference changed self state'
        replacement = run(EXAMPLE, 'run', db, 's', config, 'replacement', '-')
        assert replacement['view']['open'] == initial['open']
        assert replacement['view']['profile'] == initial['profile']
        assert seen[-1][1]['prior_unsettled_attempt'] is True
        run(CLI, 'send', db, 's', 'close', 'Fixture host confirms return.')
        tamper = run(EXAMPLE, 'run', db, 's', config, 'tamper', '-', code=3)
        assert tamper['status'] == 'self_rejected' and tamper['view'] == replacement['view']
        request.write_text(json.dumps({'settle': {KEY: {'version': initial['open'][0]['version'], 'status': 'closed'}}}))
        grant.write_text(json.dumps(run(EXAMPLE, 'grant', db, 's', request)))
        settled = run(EXAMPLE, 'run', db, 's', config, 'settle', grant)
        assert settled['view']['open'] == [] and settled['view']['settled_count'] == 1
        timeline = run(CLI, 'inspect', db, 's')
        executions = [c['body']['emission']['execution'] for c in timeline['commits'] if c['body'].get('emission')]
        assert len({e['session'] for e in executions}) == 3
        assert [e['executor'] for e in executions] == ['create', 'replacement', 'settle']
        # Host inspection itself verifies the kernel, admission inputs and self-policy history.
        run(EXAMPLE, 'view', db, 's')
        with sqlite3.connect(db) as conn:
            assert conn.execute('PRAGMA user_version').fetchone()[0] == 5
        assert not errors, errors
        print('self process replacement, killed inference, guarded settlement passed')
finally:
    release.set()
    server.shutdown()
    server.server_close()

#!/usr/bin/env python3
"""Offline transport faults; never contacts a model provider."""
import http.server
import json
import pathlib
import subprocess
import sqlite3
import signal
import sys
import tempfile
import threading
import time

CLI = sys.argv[1]
requests = []
slow_started = threading.Event()
slow_release = threading.Event()

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append((self.path, request, self.headers.get('X-Cogg-Attempt')))
        model = request['model']
        if model == 'hold':
            slow_started.set()
            slow_release.wait(10)
        if model == 'huge':
            self.send_response(200)
            self.end_headers()
            try:
                self.wfile.write(b'x' * (2 * 1048576 + 1))
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if model == 'slow':
            time.sleep(.3)
        if model == 'broken':
            time.sleep(.01)
            self.send_response(503)
            self.end_headers()
            self.wfile.write(b'provider-secret-error-body')
            return
        if model == 'redirect':
            self.send_response(307)
            self.send_header('Location', '/redirect-target')
            self.end_headers()
            return
        proposal = {'kind': 'speech', 'text': 'kept', 'memory': [], 'wake_after_ms': None}
        if model in ('abstain', 'needs-frame', 'mixed', 'abstention', 'mixed-alias'):
            proposal = {'kind': 'abstain', 'reason': 'missing_input', 'detail': 'A camera observation is required'}
            if model in ('abstention', 'mixed-alias'):
                proposal['kind'] = 'abstention'
            if model in ('mixed', 'mixed-alias'):
                proposal['memory'] = [{'key': 'forbidden', 'value': True}]
        if model == 'from-frame':
            present = json.loads(request['messages'][-1]['content'])
            assert present['inputs'][0]['body']['content']['description'] == 'cat lies on a cushion'
            proposal['text'] = 'The supplied observation describes the cat lying on a cushion.'
        content = json.dumps(proposal) if model != 'invalid' else '```json\n{}\n```'
        if self.path.endswith('/api/chat'):
            body = {'model': model, 'done': True, 'done_reason': 'stop', 'message': {'role': 'assistant', 'content': content}, 'prompt_eval_count': 300, 'eval_count': 20}
        else:
            body = {'model': model, 'choices': [{'finish_reason': 'length' if model == 'partial' else 'stop', 'message': {'role': 'assistant', 'content': content}}], 'usage': {'prompt_tokens': 300, 'completion_tokens': 20}}
        if model == 'refusal':
            body['choices'][0]['message'] = {'role': 'assistant', 'content': None, 'refusal': 'provider-secret-refusal'}
        if model == 'filtered':
            body['choices'][0]['finish_reason'] = 'content_filter'
            body['choices'][0]['message']['content'] = 'provider-secret-partial'
        self.send_response(200)
        self.end_headers()
        try:
            self.wfile.write(json.dumps(body).encode())
        except (BrokenPipeError, ConnectionResetError):
            pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        config = root / 'config.json'
        config.write_text(json.dumps({'executors': {name: {'kind': 'ollama' if name == 'ollama' else 'chat_completions', 'base_url': f'http://127.0.0.1:{server.server_port}', 'model': name} for name in ('ollama', 'good', 'broken', 'partial', 'invalid', 'slow', 'redirect', 'hold', 'huge', 'abstain', 'refusal', 'filtered', 'mixed', 'needs-frame', 'from-frame', 'abstention', 'mixed-alias')}}))

        def run(*args, code=0):
            result = subprocess.run([CLI, *map(str, args)], capture_output=True, text=True, timeout=20)
            assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
            assert 'provider-secret' not in result.stdout + result.stderr
            return result.stdout

        for name in ('ollama', 'good', 'broken', 'partial', 'invalid', 'slow', 'redirect', 'huge'):
            db = root / (name + '.db')
            run('init', db, 's', 1, 100, 1000)
            extra = ['--timeout-ms', '100', '--attempt-timeout-ms', '80'] if name == 'slow' else []
            good = name in ('ollama', 'good')
            output = json.loads(run('run-route', db, 's', config, '--route', name, *extra, code=0 if good else 3))
            assert (output['status'] == 'committed') == good
            run('verify', db, 's')
            trace = json.loads(run('inspect', db, 's'))
            assert len(trace['commits']) == (2 if good else 1)
            assert trace['attempts'][0]['status'] == ('committed' if good else 'failed')
            if good:
                assert output['emission']['attempt'] == requests[-1][2]
                assert output['emission']['provider']['usage']['prompt_tokens'] == 300
        assert all(path != '/redirect-target' for path, _, _ in requests)
        db = root / 'fallback.db'
        run('init', db, 's', 1, 100, 1000)
        output = json.loads(run('run-route', db, 's', config, '--route', 'broken,good'))
        assert output['status'] == 'committed' and len(output['attempts']) == 2
        run('verify', db, 's')
        # Separate CLI processes: abstain, attach host evidence, resume the SAME occasion.
        import hashlib
        for model in ('abstain', 'abstention', 'refusal', 'filtered'):
            db = root / (model + '.db')
            run('init', db, 's', 1, 100, 1000)
            before = len(requests)
            output = json.loads(run('run-route', db, 's', config, '--route', model + ',good', code=4))
            assert output['status'] == 'abstained' and len(requests) == before + 1
            trace = json.loads(run('inspect', db, 's'))
            assert trace['tick'] == 0 and trace['attempts'][0]['status'] == 'abstained'
            assert len(trace['abstentions']) == 1 and trace['occasions'][0]['consumed'] == ''
            if model == 'abstention':
                assert trace['abstentions'][0]['body']['provider']['normalization'] == 'abstention_to_abstain'
            if model in ('refusal', 'filtered'):
                assert trace['abstentions'][0]['body']['outcome']['reason'] == 'refused'
            run('verify', db, 's')
        db = root / 'cat.db'
        run('init', db, 's', 1, 100, 1000)
        output = json.loads(run('run-route', db, 's', config, '--route', 'needs-frame', code=4))
        trace = json.loads(run('inspect', db, 's'))
        body = {'schema': 'cogg:input/v1', 'kind': 'observation', 'producer': 'fixture:camera/frame-1', 'content': {'description': 'cat lies on a cushion'}, 'sources': []}
        digest = hashlib.sha256(json.dumps(body, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()).hexdigest()
        context = {'head': trace['head'], 'occasion': trace['attempts'][0]['body']['occasion'], 'inputs': [{'id': digest, 'body': body}]}
        evidence = root / 'context.json'
        evidence.write_text(json.dumps(context))
        output = json.loads(run('run-route', db, 's', config, '--route', 'from-frame', '--context', evidence))
        assert output['tick'] == 1 and 'supplied observation' in output['proposal']['text']
        trace = json.loads(run('inspect', db, 's'))
        assert trace['attempts'][0]['body']['occasion'] == trace['attempts'][1]['body']['occasion']
        assert trace['attempts'][1]['body']['inputs'] == context['inputs']
        run('verify', db, 's')
        output = json.loads(run('run-route', db, 's', config, '--route', 'from-frame', '--context', evidence, code=3))
        assert output['status'] == 'conflict'
        # Mixed abstain + mutation fields are invalid, never partially applied.
        db = root / 'mixed.db'
        run('init', db, 's', 1, 100, 1000)
        output = json.loads(run('run-route', db, 's', config, '--route', 'mixed', code=3))
        assert output['attempts'][0]['status'] == 'invalid_output'
        run('verify', db, 's')
        # Explicit prompt contains only this subject; no hidden previous messages.
        assert all(len(body['messages']) == 2 for _, body, _ in requests)
        forbidden = root / 'forbidden.json'
        forbidden.write_text(json.dumps({'executors': {'key': {'kind': 'chat_completions', 'base_url': f'http://127.0.0.1:{server.server_port}', 'model': 'good', 'api_key_env': 'TEST_KEY'}}}))
        before = len(requests)
        run('run-route', db, 's', forbidden, '--route', 'key', code=1)
        assert len(requests) == before
        remote = root / 'remote.json'
        remote.write_text(json.dumps({'executors': {'remote': {'kind': 'chat_completions', 'base_url': f'http://127.0.0.1:{server.server_port}', 'model': 'good', 'remote': True}}}))
        run('send', db, 's', 'remote-gate', 'Synthetic remote request')
        before = len(requests)
        output = json.loads(run('run-route', db, 's', remote, '--route', 'remote', code=3))
        assert output['attempts'][0]['status'] == 'remote_denied' and len(requests) == before
        output = json.loads(run('run-route', db, 's', remote, '--route', 'remote', '--allow-remote'))
        assert output['status'] == 'committed'
        # Real legacy fixtures migrate without changing old record bytes/hashes.
        for version in (3, 4):
            legacy = root / f'legacy-{version}.db'
            with sqlite3.connect(legacy) as connection:
                connection.executescript((pathlib.Path(__file__).parent / f'legacy-v{version}.sql').read_text())
                original = connection.execute('select id,body from commits order by tick').fetchall()
            run('verify', legacy, 'legacy')
            run('send', legacy, 'legacy', 'upgrade', 'Continue with a different executor.')
            output = json.loads(run('run-route', legacy, 'legacy', config, '--route', 'good'))
            assert output['tick'] == 2
            run('verify', legacy, 'legacy')
            with sqlite3.connect(legacy) as connection:
                assert connection.execute('pragma user_version').fetchone()[0] == 5
                assert connection.execute('select id,body from commits order by tick').fetchall()[:len(original)] == original
        def held_process(db):
            slow_started.clear()
            slow_release.clear()
            child = subprocess.Popen([CLI, 'run-route', str(db), 's', str(config), '--route', 'hold'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            assert slow_started.wait(10), 'HTTP never started'
            return child

        # Network inference holds no SQLite write lock: another coordinator wins.
        db = root / 'concurrent.db'
        run('init', db, 's', 1, 100, 1000)
        child = held_process(db)
        try:
            winner = json.loads(run('run-route', db, 's', config, '--route', 'good'))
            assert winner['tick'] == 1
            slow_release.set()
            stdout, stderr = child.communicate(timeout=10)
            assert child.returncode == 3 and json.loads(stdout)['status'] == 'conflict', (stdout, stderr)
            trace = json.loads(run('inspect', db, 's'))
            assert trace['tick'] == 1 and len(trace['attempts']) == 2
            assert trace['attempts'][0]['status'] == 'superseded'
            run('verify', db, 's')
        finally:
            slow_release.set()
            if child.poll() is None:
                child.kill()
                child.wait()

        # Kill after the provider receives the request; resume with another executor.
        db = root / 'killed.db'
        run('init', db, 's', 1, 100, 1000)
        child = held_process(db)
        try:
            child.kill()
            child.communicate(timeout=10)
            trace = json.loads(run('inspect', db, 's'))
            assert trace['tick'] == 0 and trace['attempts'][0]['status'] == 'reserved'
            resumed = json.loads(run('run-route', db, 's', config, '--route', 'good'))
            assert resumed['tick'] == 1
            trace = json.loads(run('inspect', db, 's'))
            assert trace['attempts'][1]['body']['prior_unsettled']
            assert sum(a['status'] == 'committed' for a in trace['attempts']) == 1
            run('verify', db, 's')
        finally:
            slow_release.set()
            if child.poll() is None:
                child.kill()
                child.wait()

        # SIGTERM cancels an in-flight HTTP operation without a transition.
        db = root / 'cancelled.db'
        run('init', db, 's', 1, 100, 1000)
        child = held_process(db)
        try:
            child.send_signal(signal.SIGTERM)
            stdout, stderr = child.communicate(timeout=10)
            assert child.returncode == 130 and json.loads(stdout)['status'] == 'cancelled', (stdout, stderr)
            trace = json.loads(run('inspect', db, 's'))
            assert trace['tick'] == 0 and trace['attempts'][0]['status'] == 'failed'
            run('verify', db, 's')
        finally:
            slow_release.set()
            if child.poll() is None:
                child.kill()
                child.wait()
    print('HTTP transport faults passed')
finally:
    server.shutdown()

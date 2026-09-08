#!/usr/bin/env python3
"""Offline host/client invariants. No provider keys or real model calls."""
import fcntl
import hashlib
import http.server
import json
import os
import pathlib
import pty
import re
import select
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

HOST, TUI, CLI = map(str, sys.argv[1:4])
HTTP = len(sys.argv) > 4 and sys.argv[4] == 'http'
SELF = len(sys.argv) > 5 and sys.argv[5] == 'self'
children = []
requests = []
entered = threading.Event()
release = threading.Event()


def run(*args, code=0):
    p = subprocess.run(list(map(str, args)), capture_output=True, timeout=15)
    assert p.returncode == code, (args, p.returncode, p.stdout.decode(errors='replace'), p.stderr.decode(errors='replace'))
    return p.stdout


def rpc(path, op, **fields):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(3)
        s.connect(str(path))
        s.sendall((json.dumps({'protocol': 'cogg:host/v1', 'op': op, **fields}) + '\n').encode())
        data = b''
        while b'\n' not in data:
            chunk = s.recv(65536)
            assert chunk, 'unexpected disconnect'
            data += chunk
        reply = json.loads(data)
        assert reply['ok'], reply
        return reply['data']


def eventually(fn, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            result = fn()
            if result:
                return result
        except (OSError, AssertionError):
            pass
        time.sleep(.05)
    raise AssertionError('condition did not become true')


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        present = json.loads(req['messages'][-1]['content'])
        payload = present['occasion']['payload']
        message = payload.get('text', '') if isinstance(payload, dict) else ''
        requests.append((message, present))
        if message == 'slow':
            entered.set()
            release.wait(8)
        if message == 'broken':
            self.send_response(503)
            self.end_headers()
            self.wfile.write(b'provider-private-body')
            return
        proposal = {'kind': 'null', 'text': '', 'memory': [], 'wake_after_ms': None}
        if message == 'abstain':
            proposal = {'kind': 'abstain', 'reason': 'missing_input', 'detail': 'Camera observation required'}
        elif message == 'self-write':
            proposal.update(memory=[{'key': 'self.profile', 'value': {'schema': 'cogg:self/v1', 'revision': 1, 'data': {'name': 'unauthorized'}}}])
        elif message == 'reflect':
            proposal.update(kind='reflection', notes=[{'key': 'private', 'text': 'INTERNAL NOTE', 'type': 'note', 'status': 'active', 'sources': [], 'covers': []}])
        elif message == 'wake-later':
            proposal.update(kind='reflection', wake_after_ms=200)
        elif message:
            proposal.update(kind='speech', text='Ответ: ' + message)
        elif present['occasion']['kind'] == 'scheduled':
            proposal.update(kind='speech', text='Scheduled speech')
        body = {'model': 'fixture', 'choices': [{'finish_reason': 'stop', 'message': {'role': 'assistant', 'content': json.dumps(proposal)}}]}
        self.send_response(200)
        self.end_headers()
        try:
            self.wfile.write(json.dumps(body).encode())
        except (BrokenPipeError, ConnectionResetError):
            pass


def start(root, name, config=None, existing=False):
    db, sock = root / (name + '.db'), root / (name + '.sock')
    if not existing:
        run(CLI, 'init', db, 'subject', 1, 100, 60000)
    args = [HOST, '--db', str(db), '--subject', 'subject', '--socket', str(sock)]
    args += ['--config', str(config), '--executor', 'fixture'] if config else ['--demo']
    log = open(root / (name + '.log'), 'wb')
    p = subprocess.Popen(args, stdout=log, stderr=log)
    children.append((p, log))
    eventually(lambda: rpc(sock, 'snapshot'))
    return p, db, sock


def stop(p):
    p.send_signal(signal.SIGTERM)
    assert p.wait(timeout=8) == 0


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='cogg-tui-') as directory:
        root = pathlib.Path(directory)
        p, db, sock = start(root, 'demo')
        before = rpc(sock, 'snapshot')
        assert before['host']['paused'] and before['tick'] == 0
        assert os.stat(sock).st_mode & 0o777 == 0o600
        message = 'Привет, cogg! 🐈'
        a = rpc(sock, 'send', key='first', text=message)
        assert rpc(sock, 'send', key='first', text=message) == a
        pending = rpc(sock, 'snapshot')
        assert pending['tick'] == 0 and len(pending['conversation']) == 1 and pending['conversation'][0]['status'] == 'pending'
        rpc(sock, 'resume')
        eventually(lambda: rpc(sock, 'snapshot')['tick'] == 2)
        s = rpc(sock, 'snapshot')
        assert [m['role'] for m in s['conversation']] == ['user', 'subject']
        assert s['conversation'][1]['text'] == 'Demo echo: ' + message
        assert s['conversation'][0]['status'] == 'consumed'
        assert rpc(sock, 'inspect')['body']['proposal']['kind'] == 'speech'
        assert rpc(sock, 'since', tick=0)['through_tick'] == 2
        assert rpc(sock, 'why-awake')['last_wake_plan']['reasons'] == ['waiting_external']
        assert 'demo.last-message' in json.dumps(rpc(sock, 'memory'))
        run(TUI, '--socket', sock, '--watch', '--request', '{"op":"resume"}', code=2)
        for width, height in [(80, 24), (120, 35), (40, 12)]:
            out = run(TUI, '--socket', sock, '--snapshot', width, height).decode()
            if width >= 80:
                assert message in out and 'Demo echo' in out, out
            assert 'PAUSED' not in out  # host is enabled and waiting for external input
            assert len(out.splitlines()) == height
        # Real PTY: UTF-8 input, navigation/resize, and disconnect without killing host.
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
        ui = subprocess.Popen([TUI, '--socket', str(sock)], stdin=slave, stdout=slave, stderr=slave,
                              env={**os.environ, 'TERM': 'xterm-256color'})
        os.close(slave)
        received = bytearray()
        def drain(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if select.select([master], [], [], .05)[0]:
                    try:
                        received.extend(os.read(master, 65536))
                    except OSError:
                        break
        drain(.7)
        os.write(master, b'unsent-draft\x03')
        drain(.2)
        assert ui.poll() is None, 'Ctrl+C must clear input, not disconnect'
        os.write(master, 'Кот спит'.encode() + b'\r')
        drain(.8)
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 120, 0, 0))
        ui.send_signal(signal.SIGWINCH)
        os.write(master, b'\x1bOP')  # F1
        drain(.3)
        os.write(master, b'\x11')  # Ctrl+Q
        drain(.4)
        assert ui.wait(timeout=5) == 0
        os.close(master)
        assert b'cogg' in received
        assert p.poll() is None and rpc(sock, 'snapshot')['host']['paused'] is False
        assert any(m['text'] == 'Кот спит' for m in rpc(sock, 'snapshot')['conversation'])
        # Legacy CLI text events also appear as input, without a TUI-only type marker.
        run(CLI, 'send', db, 'subject', 'legacy', 'Legacy input')
        eventually(lambda: any(m['text'] == 'Legacy input' for m in rpc(sock, 'snapshot')['conversation']))
        # Terminal control bytes stay data, never emitted as OSC / CSI.
        rpc(sock, 'send', key='escape', text='safe\x1b]52;c;PAYLOAD\x07end')
        rendered = run(TUI, '--socket', sock, '--snapshot', 80, 24)
        assert b'\x1b]52;' not in rendered and b'\x07' not in rendered
        # Another host cannot own this database through another socket.
        run(HOST, '--db', db, '--subject', 'subject', '--socket', root / 'other.sock', '--demo', code=2)
        rpc(sock, 'pause')
        eventually(lambda: not rpc(sock, 'snapshot')['host']['running'])
        checkpoint = rpc(sock, 'snapshot')['head']
        stop(p)
        run(TUI, '--socket', sock, '--request', '{"op":"snapshot"}', code=2)
        p, _, _ = start(root, 'demo', existing=True)
        assert rpc(sock, 'snapshot')['head'] == checkpoint and rpc(sock, 'snapshot')['host']['paused']
        # Malformed/oversized IPC does not damage the subject or host.
        with socket.socket(socket.AF_UNIX) as invalid:
            invalid.connect(str(sock)); invalid.sendall(b'x' * 65536)
            assert json.loads(invalid.recv(4096))['ok'] is False
        assert rpc(sock, 'snapshot')['head'] == checkpoint
        stop(p)
        run(CLI, 'verify', db, 'subject')
        controls = [json.loads(line)['command'] for line in pathlib.Path(str(sock) + '.operators.jsonl').read_text().splitlines()]
        assert controls.count('start-paused') == 2 and 'resume' in controls and 'pause' in controls
        # Explicit create, existing-file protection, and preflight refusal of old schema.
        fresh = root / 'fresh.db'
        fresh_args = [HOST, '--db', fresh, '--subject', 'fresh', '--socket', root / 'fresh.sock', '--demo', '--create']
        log = open(root / 'fresh.log', 'wb')
        p = subprocess.Popen(list(map(str, fresh_args)), stdout=log, stderr=log); children.append((p, log))
        eventually(lambda: rpc(root / 'fresh.sock', 'snapshot'))
        stop(p)
        run(*fresh_args, code=2)
        old = root / 'old.db'
        with sqlite3.connect(old) as conn:
            conn.execute('PRAGMA application_id=1129269063'); conn.execute('PRAGMA user_version=4')
        digest = hashlib.sha256(old.read_bytes()).hexdigest()
        run(HOST, '--db', old, '--subject', 'old', '--socket', root / 'old.sock', '--demo', code=2)
        assert hashlib.sha256(old.read_bytes()).hexdigest() == digest
        if HTTP:
            config = root / 'http.json'
            config.write_text(json.dumps({'executors': {'fixture': {'kind': 'chat_completions', 'base_url': f'http://127.0.0.1:{server.server_port}', 'model': 'fixture'}}}))
            p, db, sock = start(root, 'http', config)
            rpc(sock, 'resume')
            eventually(lambda: rpc(sock, 'snapshot')['tick'] == 1)
            def send(text):
                return rpc(sock, 'send', key=text, text=text)
            send('reflect')
            eventually(lambda: rpc(sock, 'snapshot')['tick'] == 2)
            s = rpc(sock, 'snapshot')
            assert not any(m['role'] == 'subject' for m in s['conversation'])
            assert s['commits'][-1]['kind'] == 'reflection'
            assert 'INTERNAL NOTE' in json.dumps(rpc(sock, 'memory'))
            send('slow')
            assert entered.wait(3)
            began = time.monotonic()
            assert rpc(sock, 'snapshot')['host']['running']
            send('queued')
            assert time.monotonic() - began < 1.5
            rpc(sock, 'pause')  # in-flight commit allowed; queued message must wait
            release.set()
            eventually(lambda: rpc(sock, 'snapshot')['tick'] == 3)
            time.sleep(.3)
            assert rpc(sock, 'snapshot')['tick'] == 3
            rpc(sock, 'resume')
            eventually(lambda: rpc(sock, 'snapshot')['tick'] == 4)
            send('wake-later')
            eventually(lambda: rpc(sock, 'snapshot')['tick'] == 6)
            assert rpc(sock, 'snapshot')['conversation'][-1]['text'] == 'Scheduled speech'
            head = rpc(sock, 'snapshot')['head']
            send('abstain')
            eventually(lambda: rpc(sock, 'snapshot')['host']['last'] == 'abstained')
            s = rpc(sock, 'snapshot')
            assert s['head'] == head and s['host']['paused']
            assert s['attempts'][-1]['abstention']['reason'] == 'missing_input'
            count = len(requests); time.sleep(.5)
            assert len(requests) == count
            rpc(sock, 'resume')
            eventually(lambda: len(requests) == count + 1)
            eventually(lambda: rpc(sock, 'snapshot')['host']['paused'])
            assert rpc(sock, 'snapshot')['head'] == head
            stop(p)
            run(CLI, 'verify', db, 'subject')
            p, db, sock = start(root, 'transport', config)
            rpc(sock, 'send', key='broken', text='broken'); rpc(sock, 'resume')
            eventually(lambda: rpc(sock, 'snapshot')['host']['paused'])
            s = rpc(sock, 'snapshot')
            assert s['attempts'][-1]['status'] == 'failed'
            assert 'provider-private-body' not in json.dumps(s)
            stop(p)
        if HTTP:
            entered.clear(); release.clear()
            p, db, sock = start(root, 'crash', config)
            rpc(sock, 'resume'); eventually(lambda: rpc(sock, 'snapshot')['tick'] == 1)
            head = rpc(sock, 'snapshot')['head']
            rpc(sock, 'send', key='slow', text='slow'); assert entered.wait(3)
            p.kill(); p.wait(timeout=3); release.set()
            count = len(requests)
            p, db, sock = start(root, 'crash', config, existing=True)
            s = rpc(sock, 'snapshot')
            assert s['head'] == head and s['host']['paused'] and s['attempts'][-1]['status'] == 'reserved'
            time.sleep(.3); assert len(requests) == count
            rpc(sock, 'resume'); eventually(lambda: rpc(sock, 'snapshot')['tick'] == 2)
            assert len(requests) == count + 1
            stop(p); run(CLI, 'verify', db, 'subject')
        if HTTP and SELF:
            seed = root / 'self.json'
            seed.write_text(json.dumps({'self.profile': {'schema': 'cogg:self/v1', 'revision': 0, 'data': {'name': 'kept'}}}))
            db = root / 'self.db'
            run(CLI, 'init', db, 'subject', 1, 100, 60000, 86400000, seed)
            p, db, sock = start(root, 'self', config, existing=True)
            rpc(sock, 'resume')
            eventually(lambda: rpc(sock, 'snapshot')['tick'] == 1)
            assert rpc(sock, 'memory')['self']['profile']['data']['name'] == 'kept'
            head = rpc(sock, 'snapshot')['head']
            rpc(sock, 'send', key='write', text='self-write')
            eventually(lambda: rpc(sock, 'snapshot')['host']['paused'])
            assert rpc(sock, 'snapshot')['head'] == head
            assert rpc(sock, 'memory')['self']['profile']['data']['name'] == 'kept'
            assert requests[-1][1]['inputs'][-1]['body']['producer'] == 'cogg:self/v1'
            stop(p)
        print('TUI process invariants passed'  + (' (demo + HTTP)' if HTTP else ' (demo)'))
finally:
    release.set()
    for child, log in children:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=8)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait()
        log.close()
    server.shutdown(); server.server_close()

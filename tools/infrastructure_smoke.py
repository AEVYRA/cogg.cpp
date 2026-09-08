#!/usr/bin/env python3
"""Opt-in synthetic inference probes. Does not open or mutate a cogg subject DB."""
import argparse
import json
import os
from pathlib import Path
import shlex
import time
import urllib.error
import urllib.parse
import urllib.request

PROMPT = ('Infrastructure test for cogg.cpp development. The fixture states: '
          'the workshop access code is cedar-582. Return exactly a JSON object '
          'with ready=true and answer equal to that access code. No other text.')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def credential(spec):
    """Read just the requested key; never source a shell file or log credentials."""
    name = spec.get('api_key_env')
    if not name:
        return ''
    if os.environ.get(name):
        return os.environ[name]
    path = spec.get('credential_file')
    if path:
        for line in Path(path).expanduser().read_text().splitlines():
            key, sep, value = line.removeprefix('export ').partition('=')
            if sep and key.strip() == name:
                parts = shlex.split(value, comments=True)
                if len(parts) == 1 and parts[0]:
                    return parts[0]
    raise ValueError('required credential is unavailable')


def probe(name, spec):
    result = {'executor': name, 'requested_model': spec['model'], 'status': 'failed'}
    secret = ''
    started = time.monotonic()
    try:
        secret = credential(spec)
        base = spec['base_url'].rstrip('/')
        parsed = urllib.parse.urlsplit(base)
        if parsed.scheme not in ('http', 'https') or parsed.username or parsed.password:
            raise ValueError('invalid endpoint')
        if secret and parsed.scheme != 'https':
            raise ValueError('credentials require HTTPS')
        headers = {'Content-Type': 'application/json', 'User-Agent': 'cogg-infrastructure-probe/0.1'}
        if secret:
            headers['Authorization'] = 'Bearer ' + secret
        payload = {'model': spec['model'], 'messages': [{'role': 'user', 'content': PROMPT}], 'stream': False}
        if spec['kind'] == 'ollama':
            path = '/api/chat'
            payload.update({'format': 'json', 'keep_alive': '2m', 'options': {
                'num_ctx': spec.get('context_tokens', 4096), 'num_predict': 96,
                'temperature': 0, 'num_thread': spec.get('threads', 2)}})
        elif spec['kind'] == 'chat_completions':
            path = '/chat/completions'
            payload['max_tokens'] = spec.get('max_output_tokens', 512)
        else:
            raise ValueError('unsupported transport kind')
        extra = spec.get('request_options', {})
        if set(extra) - {'thinking', 'reasoning_effort', 'temperature', 'response_format'}:
            raise ValueError('unsupported request option')
        payload.update(extra)
        request = urllib.request.Request(base + path, data=json.dumps(payload).encode(), headers=headers)
        with urllib.request.build_opener(NoRedirect).open(request, timeout=spec.get('timeout_seconds', 90)) as response:
            raw = response.read(2 * 1024 * 1024 + 1)
            if len(raw) > 2 * 1024 * 1024:
                raise ValueError('response too large')
            data = json.loads(raw)
        result['reported_model'] = data.get('model')
        if spec['kind'] == 'ollama':
            answer = data['message']['content']
            result['usage'] = {k: data.get(k) for k in ('prompt_eval_count', 'eval_count', 'load_duration', 'eval_duration')}
            result['finish_reason'] = data.get('done_reason')
            if data.get('eval_duration', 0):
                result['generation_tokens_per_second'] = round(data['eval_count'] * 1e9 / data['eval_duration'], 2)
            # Residency is observed immediately; it is not an inference from hardware specs.
            with urllib.request.build_opener(NoRedirect).open(base + '/api/ps', timeout=10) as response:
                models = json.load(response).get('models', [])
            result['residency'] = [{k: m.get(k) for k in ('name', 'digest', 'size', 'size_vram', 'context_length')}
                                   for m in models if m.get('name') == spec['model'] or m.get('model') == spec['model']]
        else:
            choice = data['choices'][0]
            answer = choice['message'].get('content') or ''
            result['usage'] = data.get('usage', {})
            result['finish_reason'] = choice.get('finish_reason')
        # Record only the fixed synthetic answer, never reasoning or arbitrary response bodies.
        try:
            fixture = json.loads(answer)
        except (ValueError, TypeError):
            fixture = None
        result['fixture_pass'] = fixture == {'ready': True, 'answer': 'cedar-582'}
        result['status'] = 'ok' if result['fixture_pass'] else 'fixture_failed'
    except urllib.error.HTTPError as error:
        result['http_status'] = error.code
        result['error'] = 'endpoint rejected request'
    except Exception as error:
        result['error'] = type(error).__name__
    result['elapsed_seconds'] = round(time.monotonic() - started, 3)
    # Defensive final redaction even if a provider puts a credential into metadata.
    encoded = json.dumps(result)
    return json.loads(encoded.replace(secret, '[redacted]')) if secret else result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--only', nargs='+', required=True, help='Explicit executors; API calls may consume quota')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.expanduser().read_text())
    missing = set(args.only) - config['executors'].keys()
    if missing:
        parser.error('unknown executor names')
    results = [probe(name, config['executors'][name]) for name in args.only]
    report = {'schema': 'cogg:infrastructure-smoke/v1', 'scope': 'synthetic transport readiness; not subject continuity or memory quality', 'results': results}
    text = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')
    return 0 if all(r['status'] == 'ok' for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())

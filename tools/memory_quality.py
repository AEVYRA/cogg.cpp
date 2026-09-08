#!/usr/bin/env python3
"""Explicit live comparison: BINARY EXECUTOR_CONFIG NEW_ARTIFACT_DIR.

32 calls, two executor workers, no retries. Fixtures/criteria freeze before requests.
Only synthetic question/evidence reach providers; expected answers remain local.
"""
import concurrent.futures
import hashlib
import json
import pathlib
import subprocess
import sys

binary, config, root = (pathlib.Path(arg).resolve() for arg in sys.argv[1:4])
root.mkdir(mode=0o700, parents=True, exist_ok=False)


def save(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n')


exported = subprocess.run([str(binary), 'fixture', str(root / 'fixture.db')], capture_output=True, text=True, timeout=30)
if exported.returncode:
    raise RuntimeError(exported.stderr)
fixture = json.loads(exported.stdout)
save(root / 'fixture.json', fixture)
fixture_hash = hashlib.sha256((root / 'fixture.json').read_bytes()).hexdigest()
protocol = {
    'schema': 'cogg:memory-quality-protocol/v1', 'fixture_sha256': fixture_hash,
    'executors': ['deepseek', 'kimi'], 'methods': ['balanced', 'last_messages'],
    'cases': 8, 'known_cases': 7, 'missing_fact_cases': 1, 'calls': 32, 'retries': 0,
    'evidence_budget': {'items': 4, 'serialized_bytes': 2400}, 'max_output_tokens': 512,
    'complete_prompt_cap_bytes': 24576, 'timeout_ms': 60000,
    'order': 'One worker per model; method order alternates across cases and models; fresh process/context per call.',
    'scoring': 'Known answer: speech contains one term from every predefined accept group. Grounded correct additionally requires exact current source in evidence. Missing evidence: typed missing_input/uncertain abstention. Provider failures remain failures, without retry. Inspect raw answers for matcher limitations.',
    'scope': 'Synthetic memory-read comparison; history is seeded by host, not extracted by models. No autonomous memory writing or action execution. Last-messages baseline uses the same structured message evidence fields without retrieval/version filtering. Balanced also pays its internal receipt overhead within the 2400-byte retrieval budget.',
}
save(root / 'protocol.json', protocol)
for case in fixture['cases']:
    for method, evidence in case['contexts'].items():
        # C++ json::dump() emits compact UTF-8; match that budget accounting here.
        assert len(evidence) <= 4
        assert len(json.dumps(evidence, ensure_ascii=False, separators=(',', ':')).encode()) <= 2400


def model_run(model, model_index):
    rows = []
    for case_index, case in enumerate(fixture['cases']):
        methods = ['balanced', 'last_messages']
        if (case_index + model_index) % 2:
            methods.reverse()
        for method in methods:
            stem = f'{model}-{case["id"]}-{method}'
            request = {'question': case['question'], 'evidence': case['contexts'][method]}
            request_file = root / (stem + '.input.json')
            save(request_file, request)
            row = {'model': model, 'method': method, 'case': case['id'], 'known': case['required_source'] is not None,
                   'evidence_present': any(e['id'] == case['required_source'] for e in request['evidence']),
                   'evidence_items': len(request['evidence']),
                   'evidence_bytes': len(json.dumps(request['evidence'], ensure_ascii=False, separators=(',', ':')).encode())}
            try:
                response = subprocess.run([str(binary), 'ask', str(config), model, str(request_file)],
                                          capture_output=True, text=True, timeout=70)
                (root / (stem + '.stdout')).write_text(response.stdout)
                (root / (stem + '.stderr')).write_text(response.stderr)
                if response.returncode:
                    row.update(status='provider_or_format_failure', exit_code=response.returncode)
                else:
                    row.update(status='answered', **json.loads(response.stdout))
            except subprocess.TimeoutExpired:
                row.update(status='harness_timeout')
            out = row.get('outcome', {})
            text = out.get('text', '').lower()
            row['answer_match'] = bool(row['known'] and out.get('kind') == 'speech' and
                                       all(any(term in text for term in group) for group in case['accept']))
            row['read_only_output'] = not out.get('memory') and not out.get('notes') and out.get('wake_after_ms') is None
            row['grounded_correct'] = row['answer_match'] and row['evidence_present'] and row['read_only_output']
            row['appropriate_abstention'] = (not row['evidence_present'] and out.get('kind') == 'abstain'
                                             and out.get('reason') in ('missing_input', 'uncertain'))
            row['unsupported_speech'] = not row['evidence_present'] and out.get('kind') == 'speech'
            save(root / (stem + '.result.json'), row)
            rows.append(row)
            print(json.dumps({k: row[k] for k in ('model', 'case', 'method', 'status', 'evidence_present', 'grounded_correct', 'appropriate_abstention')}) , flush=True)
    return rows


with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    jobs = [pool.submit(model_run, model, i) for i, model in enumerate(protocol['executors'])]
    rows = [row for job in jobs for row in job.result()]
assert hashlib.sha256((root / 'fixture.json').read_bytes()).hexdigest() == fixture_hash
groups = []
for model in protocol['executors']:
    for method in protocol['methods']:
        selected = [r for r in rows if r['model'] == model and r['method'] == method]
        groups.append({'model': model, 'method': method, 'calls': len(selected),
                       'known_cases': sum(r['known'] for r in selected),
                       'evidence_present': sum(r['evidence_present'] for r in selected),
                       'grounded_correct': sum(r['grounded_correct'] for r in selected),
                       'missing_evidence_cases': sum(not r['evidence_present'] for r in selected),
                       'appropriate_abstentions': sum(r['appropriate_abstention'] for r in selected),
                       'unsupported_speech': sum(r['unsupported_speech'] for r in selected),
                       'provider_or_format_failures': sum(r['status'] != 'answered' for r in selected)})
report = {'schema': 'cogg:memory-quality-results/v1', 'protocol': protocol, 'groups': groups, 'rows': rows}
save(root / 'report.json', report)
print(json.dumps(groups, indent=2), flush=True)

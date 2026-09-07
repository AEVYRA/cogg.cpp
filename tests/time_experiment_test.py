#!/usr/bin/env python3
"""Real-model supervisor restart, fixed observation deadline and report smoke."""
import importlib.util
import json
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

cli, model = map(lambda x: str(Path(x).resolve()), sys.argv[1:3])
runner = Path(__file__).resolve().parents[1] / 'tools/time_experiment.py'
with tempfile.TemporaryDirectory(prefix='cogg-observation-') as tmp:
    directory = Path(tmp)
    command = [sys.executable, str(runner), 'run', cli, model, tmp, '--hours', '0.035',
               '--min-wake-ms', '1000', '--max-wake-ms', '10000', '--max-attempts', '4']
    with open(directory/'supervisor.log', 'w+') as log:
        p = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            stop_at = time.monotonic() + 100
            tick = 0
            while time.monotonic() < stop_at:
                if p.poll() is not None:
                    log.seek(0)
                    raise AssertionError('runner exited early: ' + log.read())
                try:
                    with sqlite3.connect(f'file:{directory}/subject.db?mode=ro', uri=True) as db:
                        tick = db.execute('SELECT tick FROM subjects').fetchone()[0]
                except (sqlite3.Error, TypeError):
                    pass
                if tick > 0:
                    break
                time.sleep(0.2)
            assert tick > 0, 'real model did not commit during supervisor smoke'
            p.send_signal(signal.SIGTERM)
            assert p.wait(timeout=40) == 0
            manifest = (directory/'manifest.json').read_bytes()
            assert json.loads((directory/'status.json').read_text())['status'] == 'interrupted'
            p = subprocess.Popen(command, stdout=log, stderr=log)
            assert p.wait(timeout=150) == 0
            assert (directory/'manifest.json').read_bytes() == manifest, 'restart reset experiment deadline'
            status = json.loads((directory/'status.json').read_text())
            assert status['status'] == 'observation_window_elapsed'
            report = json.loads((directory/'report.json').read_text())
            assert report['chain_verified'] and report['external_occasions'] == 0
            assert report['tick'] >= tick
            starts = [json.loads(line) for line in (directory/'runs.jsonl').read_text().splitlines()]
            assert sum(row['event'] == 'runner_start' for row in starts) == 2
            assert (directory/'transitions.csv').read_text().startswith('tick,head,parent,occasion')
            changed = command[:-1] + ['5']
            assert subprocess.run(changed, stdout=log, stderr=log, timeout=30).returncode != 0
            spec = importlib.util.spec_from_file_location('observation', runner)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            assert module.correlation([(1, 2)]*20)['spearman'] is None
            assert abs(module.correlation([(i,i) for i in range(20)])['spearman']-1)<1e-12
            print(json.dumps({'result':'PASS','resumed_tick':tick,'final_tick':report['tick'],
                              'external_occasions':report['external_occasions'], 'fixed_deadline':True}))
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()

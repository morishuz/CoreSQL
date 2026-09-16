"""Separate memory/CPU/allocation diagnostic; never substitute for timed results.

macOS ps/sample instrumentation; retains the same CSV loader as measure.py.
The allocation bridge counts C++ new requests, not live bytes or malloc calls.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from audit import load_queries
from validate import dataset, statements, compare
from run import Core
from measure import encode, source_fingerprint


def emit(value):
    print(json.dumps(value, default=encode), flush=True)


def memory(group):
    rows = subprocess.check_output(['ps', '-axo', 'pid=,pgid=,rss='], text=True)
    return {int(pid): int(rss) * 1024 for pid, pgid, rss in
            (line.split() for line in rows.splitlines()) if int(pgid) == group}


def worker(args):
    manifest, _ = dataset(args.data)
    _, queries = load_queries(args.queries)
    sql = dict(queries)[args.query]
    os.environ['CORESQL_PROFILE_QUERY'] = sql
    db = Core(str(args.bridge.resolve()))
    def snapshot(phase):
        readings = memory(os.getpgrp())
        emit({'phase': phase, 'driver_rss_bytes': readings.get(os.getpid(), 0),
              'engine_rss_bytes': readings.get(db.process.pid, 0), 'engine_pid': db.process.pid})
    snapshot('dataset')
    try:
        for statement in statements(manifest):
            status, detail = db.execute(statement)
            if status != 'ok':
                raise RuntimeError(detail)
        snapshot('loaded')
        expected = None
        for trial in range(args.repeats):
            emit({'phase': 'query', 'trial': trial, 'engine_pid': db.process.pid})
            status, rows = db.execute(sql)
            if status != 'ok':
                raise RuntimeError(rows)
            if expected is not None:
                compare(rows, expected)
            expected = rows
        snapshot('finished')
        emit({'phase': 'done', 'rows': expected})
    finally:
        db.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--queries', type=Path, required=True)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--query', default='q01.sql')
    parser.add_argument('--repeats', type=int, default=1)
    parser.add_argument('--sample', type=Path, help='macOS 5-second CPU sample; use enough repetitions')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--worker', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.repeats <= 50:
        parser.error('repeats must be 1..50')
    if args.worker:
        worker(args)
        return
    root = Path(__file__).resolve().parents[2]
    report = {'purpose': __doc__, 'query': args.query,
              'bridge_sha256': hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
              'source_sha256': source_fingerprint(root),
              'dataset_manifest_sha256': hashlib.sha256((args.data/'manifest.json').read_bytes()).hexdigest(),
              'scale': json.loads((args.data/'manifest.json').read_text())['scale'],
              'limit_bytes': 2048*1024*1024, 'query_timeout_seconds': 15,
              'events': [], 'peak_query_driver_bytes': 0, 'peak_query_engine_bytes': 0,
              'peak_group_bytes': 0, 'status': 'crash'}
    command = [sys.executable, str(Path(__file__).resolve()), '--worker', '--data', str(args.data),
               '--queries', str(args.queries), '--bridge', str(args.bridge), '--query', args.query,
               '--repeats', str(args.repeats), '--output', str(args.output)]
    sampler = None
    with tempfile.TemporaryFile(mode='w+') as output, tempfile.TemporaryFile(mode='w+') as errors:
        process = subprocess.Popen(command, stdout=output, stderr=errors, start_new_session=True)
        position, phase, engine = 0, 'load', None
        deadline = time.monotonic() + 120
        try:
            while True:
                output.seek(position)
                while True:
                    line_start = output.tell()
                    line = output.readline()
                    if not line.endswith('\n'):
                        output.seek(line_start)
                        break
                    event = json.loads(line)
                    report['events'].append(event)
                    phase = event['phase']
                    engine = event.get('engine_pid', engine)
                    if phase == 'query':
                        deadline = time.monotonic() + 15
                        if args.sample and sampler is None:
                            sampler = subprocess.Popen(['sample', str(engine), '5', '1', '-file', str(args.sample)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    if phase == 'done':
                        report['status'] = 'ok'
                position = output.tell()
                readings = memory(process.pid)
                report['peak_group_bytes'] = max(report['peak_group_bytes'], sum(readings.values()))
                if phase == 'query':
                    report['peak_query_driver_bytes'] = max(report['peak_query_driver_bytes'], readings.get(process.pid, 0))
                    report['peak_query_engine_bytes'] = max(report['peak_query_engine_bytes'], readings.get(engine, 0))
                if sum(readings.values()) > report['limit_bytes']:
                    report['status'] = 'memory_limit'
                    break
                if time.monotonic() > deadline:
                    report['status'] = 'timeout'
                    break
                if process.poll() is not None:
                    # Drain any final event on the following iteration.
                    if phase == 'done' or process.returncode:
                        break
                time.sleep(.05)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            if sampler is not None:
                report['sample_exit_code'] = sampler.wait(timeout=10)
        errors.seek(0)
        report['diagnostics'] = errors.read().splitlines()
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(args.output, report['status'])


if __name__ == '__main__':
    main()

"""Bounded, single-thread client-latency comparison; not an official TPC-H score.

Each query/engine gets a fresh in-memory database, an untimed first execution,
then repeated parse/execute/fetch calls. CoreSQL includes bridge IPC and decoding;
DuckDB uses its Python API. Loading and answer comparisons are outside timings.
Workers run serially in separate process groups, killed/reaped on timeout or RSS
limit. Sampled group RSS includes the Python driver and is not allocator accounting.
"""
import argparse
import datetime
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import sys
import tempfile
import time

from validate import dataset, statements, compare
from audit import load_queries
from prepare import reference, VERSION, REVISION
from run import Core
from sqlite_adapter import SQLite, adapt, compare_approximate


def encode(value):
    if isinstance(value, Decimal):
        return {'decimal': str(value)}
    if isinstance(value, datetime.date):
        return value.isoformat()
    raise TypeError(type(value).__name__)


def decode(value):
    return Decimal(value['decimal']) if set(value) == {'decimal'} else value


def emit(value):
    print(json.dumps(value, default=encode), flush=True)


def worker(args):
    manifest, _ = dataset(args.data)
    _, queries = load_queries(args.queries)
    sql = dict(queries)[args.query]
    db = (Core(str(args.bridge.resolve())) if args.worker == 'coresql' else
          SQLite(args.sqlite_library) if args.worker == 'sqlite' else reference())
    if args.worker == 'sqlite':
        sql = adapt(sql)
    if args.worker == 'duckdb':
        db.execute("SET memory_limit='1GB'")
        db.execute("SET max_temp_directory_size='0B'")

    def execute(source):
        if args.worker == 'duckdb':
            return db.execute(source).fetchall()
        if args.worker == 'sqlite':
            return db.execute(adapt(source))
        status, rows = db.execute(source)
        if status != 'ok':
            raise RuntimeError(f'{status}: {rows}')
        return rows

    try:
        start = time.perf_counter()
        for statement in statements(manifest):
            execute(statement)
        load = time.perf_counter() - start
        emit({'phase': 'warmup', 'load_seconds': load})
        rows = execute(sql)
        samples = []
        for trial in range(args.repeats):
            emit({'phase': 'sample', 'trial': trial})
            start = time.perf_counter()
            actual = execute(sql)
            samples.append(time.perf_counter() - start)
            normalized = [[v.isoformat() if isinstance(v, datetime.date) else v for v in row] for row in actual]
            compare(normalized, rows)
        emit({'phase': 'done', 'status': 'ok', 'load_seconds': load,
              'seconds': samples, 'median_seconds': statistics.median(samples), 'rows': rows})
    finally:
        db.close()


def run(args, engine, query):
    command = [sys.executable, str(Path(__file__).resolve()), '--worker', engine,
               '--query', query, '--data', str(args.data.resolve()),
               '--queries', str(args.queries.resolve()), '--bridge', str(args.bridge.resolve()),
               '--repeats', str(args.repeats)]
    if args.sqlite_library:
        command += ['--sqlite-library', str(args.sqlite_library.resolve())]
    with tempfile.NamedTemporaryFile(mode='w+') as sink, open(sink.name) as output, tempfile.TemporaryFile(mode='w+') as errors:
        process = subprocess.Popen(command, stdout=sink, stderr=errors, start_new_session=True)
        phase, deadline, position, peak = 'load', time.monotonic() + args.load_timeout, 0, 0
        result = {'status': 'crash'}
        exited = False
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
                    phase = event['phase']
                    deadline = time.monotonic() + args.timeout
                    if 'load_seconds' in event:
                        result['load_seconds'] = event['load_seconds']
                    if phase == 'done':
                        result.update(event)
                position = output.tell()
                if exited:
                    break
                if process.poll() is not None:
                    exited = True
                    continue  # Drain output written between the preceding read and exit.
                memory = subprocess.check_output(['ps', '-axo', 'pgid=,rss='], text=True)
                rss = sum(int(row.split()[1]) for row in memory.splitlines()
                          if row.split() and int(row.split()[0]) == process.pid) * 1024
                peak = max(peak, rss)
                if rss > args.memory_mb * 1024 * 1024:
                    result.update(status='memory_limit', phase=phase)
                    break
                if time.monotonic() > deadline:
                    result.update(status='timeout', phase=phase)
                    break
                time.sleep(0.1)
        finally:
            # Also reap the bridge on worker failure, not only on a timeout.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        errors.seek(0)
        error = errors.read()
        if error:
            result['stderr'] = error[-4000:]
        result['peak_group_rss_bytes'] = peak
        return result



def source_fingerprint(root):
    # Includes untracked implementation files; HEAD alone cannot identify a local experiment.
    paths = [root / 'CMakeLists.txt', root / 'tests/sqllogictest/bridge.cpp']
    for directory in ('src', 'sql', 'addons', 'include'):
        paths.extend(p for p in (root / directory).rglob('*') if p.suffix in ('.cpp', '.hpp', '.h'))
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(str(path.relative_to(root)).encode() + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--queries', type=Path, required=True)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=15)
    parser.add_argument('--load-timeout', type=float, default=120)
    parser.add_argument('--memory-mb', type=int, default=2048)
    parser.add_argument('--sqlite-library', type=Path)
    parser.add_argument('--engines', nargs='+', choices=['coresql', 'duckdb', 'sqlite'], default=['duckdb', 'coresql'])
    parser.add_argument('--worker', choices=['coresql', 'duckdb', 'sqlite'], help=argparse.SUPPRESS)
    parser.add_argument('--query', help=argparse.SUPPRESS)
    parser.add_argument('--only', nargs='+', help='Run selected pinned filenames, e.g. q19.sql')
    args = parser.parse_args()
    import math
    if args.repeats < 1 or args.memory_mb < 1 or any(not math.isfinite(x) or x <= 0 for x in (args.timeout, args.load_timeout)):
        parser.error('Limits and repetitions must be positive and finite')
    if (args.worker == 'sqlite' or 'sqlite' in args.engines) and not args.sqlite_library:
        parser.error('--sqlite-library is required for SQLite')
    if len(set(args.engines)) != len(args.engines) or 'duckdb' not in args.engines:
        parser.error('Choose unique engines including the DuckDB answer reference')
    if args.worker:
        worker(args)
        return
    if not args.output:
        parser.error('--output is required')
    manifest, digest = dataset(args.data)
    pin, queries = load_queries(args.queries)
    if args.only:
        if not set(args.only) <= {name for name, _ in queries}:
            parser.error('--only contains an unknown pinned query')
        queries = [(name, sql) for name, sql in queries if name in args.only]
    root = Path(__file__).resolve().parents[2]
    report = {'purpose': 'Single-thread in-memory client latency; not official TPC-H',
              'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'platform': platform.platform(), 'python': platform.python_version(),
              'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
              'host_memory_bytes': int(subprocess.check_output(['sysctl', '-n', 'hw.memsize'])),
              'coresql_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
              'bridge_sha256': hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
              'worktree_source_sha256': source_fingerprint(root),
              'worktree_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root)),
              'driver_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'duckdb_version': VERSION, 'duckdb_revision': REVISION,
              'scale': manifest['scale'], 'dataset_manifest_sha256': digest, 'query_revision': pin['revision'],
              'threads_per_engine': 1, 'repeats': args.repeats, 'warmups': 1,
              'query_timeout_seconds': args.timeout, 'load_timeout_seconds': args.load_timeout,
              'group_rss_limit_mb': args.memory_mb,
              'timing_scope': 'SQL text to fetched Python rows; includes parsing/binding/materialization. CoreSQL additionally includes bridge serialization/IPC/decoding. No persistent prepared statements.',
              'memory_scope': 'Peak sampled process-group RSS, including Python driver and loading, at approximately 100ms intervals; may miss brief peaks.',
              'schema': 'Identical native DATE/DECIMAL input; no indexes or constraints; in-memory; DuckDB memory_limit 1GB and no disk spill.',
              'selected_queries': [name for name, _ in queries], 'results': []}
    if args.sqlite_library:
        sqlite = SQLite(args.sqlite_library)
        report['sqlite'] = {'version': sqlite.libversion().decode(), 'source_id': sqlite.source_id,
                            'library_sha256': hashlib.sha256(args.sqlite_library.read_bytes()).hexdigest(),
                            'adapter_sha256': hashlib.sha256(Path(__file__).with_name('sqlite_adapter.py').read_bytes()).hexdigest(),
                            'profile': 'Adapted SQL; ISO TEXT dates; approximate REAL decimals; case-sensitive LIKE; automatic_index ON; in-memory temp storage; no worker threads; ctypes C API prepares/finalizes each call.',
                            'numeric_tolerance': {'relative': 1e-10, 'absolute': 1e-10}}
        sqlite.close()
    report['engine_order'] = args.engines
    for query, sql in queries:
        entry = {'query': query}
        for engine in args.engines:
            print(f'{query} {engine}: running', flush=True)
            entry[engine] = run(args, engine, query)
            print(f"{query} {engine}: {entry[engine]['status']} {entry[engine].get('median_seconds', '')}", flush=True)
        expected = (json.loads(json.dumps(entry['duckdb']['rows']), object_hook=decode)
                    if entry['duckdb']['status'] == 'ok' else None)
        for engine in args.engines:
            outcome = entry[engine]
            outcome['correctness'] = 'not_verified'
            if expected is not None and outcome['status'] == 'ok':
                actual = json.loads(json.dumps(outcome['rows']), object_hook=decode)
                try:
                    (compare_approximate if engine == 'sqlite' else compare)(actual, expected)
                    outcome['correctness'] = 'approximate_match' if engine == 'sqlite' else 'match'
                except AssertionError as error:
                    outcome['correctness'] = 'wrong_answer'
                    outcome['detail'] = str(error)
            rows = outcome.pop('rows', None)
            if rows is not None:
                outcome['row_count'] = len(rows)
        if 'sqlite' in entry:
            entry['sqlite_sql_sha256'] = hashlib.sha256(adapt(sql).encode()).hexdigest()
        if ('coresql' in entry and 'sqlite' in entry and
            entry['coresql']['correctness'] == 'match' and
            entry['sqlite']['correctness'] == 'approximate_match'):
            entry['sqlite_over_coresql'] = entry['sqlite']['median_seconds'] / entry['coresql']['median_seconds']
        report['results'].append(entry)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Report: {args.output}', flush=True)


if __name__ == '__main__':
    main()

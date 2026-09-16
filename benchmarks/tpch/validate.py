"""Validate all pinned queries on identical generated rows, with per-query limits.

DECIMAL cells compare exactly. REAL outputs use the stated tolerance. Unsupported
queries, wrong answers, errors and timeouts are separate results, never skipped.
No query timing is reported as a benchmark measurement.
"""
import argparse
import csv
from decimal import Decimal
import hashlib
import json
import math
import multiprocessing
from pathlib import Path
import re
from audit import load_queries
from prepare import reference, TABLES, VERSION, REVISION
from run import Core


def dataset(directory):
    raw = (directory / 'manifest.json').read_bytes()
    manifest = json.loads(raw)
    if manifest.get('format') != 1 or manifest.get('duckdb_version') != VERSION or manifest.get('duckdb_revision') != REVISION:
        raise RuntimeError('Dataset format or generator pin differs')
    if [t['name'] for t in manifest['tables']] != list(TABLES):
        raise RuntimeError('Dataset table set/order differs')
    for table in manifest['tables']:
        if table['file'] != table['name'] + '.csv':
            raise RuntimeError('Unexpected dataset path')
        path = directory / table['file']
        if hashlib.sha256(path.read_bytes()).hexdigest() != table['sha256']:
            raise RuntimeError(f'Dataset checksum mismatch: {path}')
        if not table['columns'] or any(not re.fullmatch('[a-z_]+', c) or not re.fullmatch(r'INTEGER|BIGINT|VARCHAR|DATE|DECIMAL\(15,2\)', t) for c, t in table['columns']):
            raise RuntimeError('Unexpected generator schema')
        with path.open(newline='', encoding='utf-8') as source:
            rows = list(csv.reader(source))
        if len(rows) != table['rows'] or any(len(row) != len(table['columns']) for row in rows):
            raise RuntimeError('Dataset dimensions differ')
        table['data'] = rows
    return manifest, hashlib.sha256(raw).hexdigest()


def statements(manifest):
    for table in manifest['tables']:
        columns = ','.join(f'{name} {kind}' for name, kind in table['columns'])
        yield f"CREATE TABLE {table['name']}({columns})"
        for start in range(0, len(table['data']), 50):
            rows = table['data'][start:start+50]
            values = ','.join('(' + ','.join("'" + v.replace("'", "''") + "'" for v in row) + ')' for row in rows)
            yield f"INSERT INTO {table['name']} VALUES{values}"


def compare(actual, expected):
    if len(actual) != len(expected):
        raise AssertionError(f'Row counts differ: {len(actual)} vs {len(expected)}')
    for i, (row, reference_row) in enumerate(zip(actual, expected)):
        if len(row) != len(reference_row):
            raise AssertionError('Projection widths differ')
        for j, (value, wanted) in enumerate(zip(row, reference_row)):
            if isinstance(wanted, float):
                match = isinstance(value, float) and math.isclose(float(value), wanted, rel_tol=1e-10, abs_tol=1e-10)
            elif isinstance(wanted, Decimal):
                match = isinstance(value, Decimal) and value == wanted
            elif hasattr(wanted, 'isoformat'):
                match = value == wanted.isoformat()
            elif isinstance(wanted, int):
                match = isinstance(value, int) and value == wanted
            else:
                match = value == wanted
            if not match:
                raise AssertionError(f'Cell [{i},{j}] differs: {value!r} vs {wanted!r}')


def worker(send, manifest, sql):
    # The parent owns the CoreSQL subprocess so it can terminate a timed-out
    # query without orphaning a grandchild. Only reference work runs here.
    db = reference()
    try:
        for statement in statements(manifest):
            db.execute(statement)
        expected = db.execute(sql).fetchall()
        send.send(('ok', expected))
    except Exception as error:
        send.send(('error', str(error)))
    finally:
        db.close()
        send.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--queries', type=Path, required=True)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=30, help='Per-engine query limit in seconds')
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('--timeout must be positive and finite')
    manifest, digest = dataset(args.data)
    pin, queries = load_queries(args.queries)
    results = []
    # A thread only handles pipe I/O; a timeout kills the owned native process.
    from concurrent.futures import ThreadPoolExecutor, TimeoutError
    context = multiprocessing.get_context('spawn')
    for name, sql in queries:
        print(f'{name}: loading', flush=True)
        core = Core(str(args.bridge.resolve()))
        pool = ThreadPoolExecutor(max_workers=1)
        try:
            for statement in statements(manifest):
                status, detail = core.execute(statement)
                if status != 'ok':
                    raise RuntimeError(f'CoreSQL data load failed: {detail}')
            future = pool.submit(core.execute, sql)
            try:
                status, rows = future.result(timeout=args.timeout)
            except TimeoutError:
                status, rows = 'timeout', f'Query exceeded {args.timeout:g}s'
                core.process.kill()
                core.process.wait()
            except Exception as error:
                status, rows = 'crash', str(error)
                if core.process.poll() is None:
                    core.process.kill();core.process.wait()
            if status == 'ok':
                receiver, sender = context.Pipe(duplex=False)
                process = context.Process(target=worker, args=(sender, manifest, sql))
                process.start();sender.close()
                try:
                    if not receiver.poll(args.timeout):
                        status, rows = 'reference_timeout', 'Reference query/load exceeded limit'
                    else:
                        try:
                            reference_status, expected = receiver.recv()
                        except EOFError:
                            reference_status, expected = 'error', 'Reference process terminated without a result'
                        if reference_status != 'ok':
                            status, rows = 'reference_error', expected
                        else:
                            try:
                                compare(rows, expected)
                                status, rows = 'match', {'rows': len(rows)}
                            except AssertionError as error:
                                status, rows = 'wrong_answer', str(error)
                finally:
                    if process.is_alive():
                        process.terminate()
                    process.join();receiver.close()
            result = {'query': name, 'status': status, 'detail': rows}
            results.append(result)
            print(f'{name}: {status}: {rows}', flush=True)
        finally:
            if core.process.poll() is None:
                core.close()
            else:
                core.process.stdin.close();core.process.stdout.close()
            pool.shutdown(wait=True)
    report = {'purpose': 'Correctness only; not a timed benchmark', 'dataset_manifest_sha256': digest,
              'scale': manifest['scale'], 'duckdb_version': VERSION, 'duckdb_revision': REVISION,
              'query_revision': pin['revision'], 'bridge_sha256': hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
              'decimal_comparison': 'exact', 'real_relative_tolerance': 1e-10, 'real_absolute_tolerance': 1e-10,
              'row_order': 'exact, including tied rows', 'timeout_seconds': args.timeout, 'results': results}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"{sum(r['status']=='match' for r in results)}/{len(results)} matched; see the complete report")


if __name__ == '__main__':
    main()

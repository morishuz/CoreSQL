"""Curated public SQL workloads, full-result validation and bounded serial timing."""
import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import resource
import signal
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / 'tpch'))
from validate import compare
from prepare import reference, VERSION, REVISION
from run import Core
from sqlite_adapter import SQLite
from measure import source_fingerprint, encode, decode
from fixtures import statements

CATALOGS = ('speedtest1', 'duckdb', 'h2o', 'controls')


def catalog():
    cases = [case for name in CATALOGS for case in json.loads((HERE / (name+'.json')).read_text())]
    if len({c['id'] for c in cases}) != len(cases):
        raise ValueError('Duplicate workload ID')
    return cases


def verify_sources(directory, fetch):
    pins = json.loads((HERE / 'sources.json').read_text())
    for pin in pins.values():
        target = directory / pin['repository'] / pin['revision'] / pin['path']
        if not target.exists() and fetch:
            url = f"https://raw.githubusercontent.com/{pin['repository']}/{pin['revision']}/{pin['path']}"
            data = urllib.request.urlopen(url, timeout=30).read()
            if hashlib.sha256(data).hexdigest() != pin['sha256']:
                raise ValueError('Downloaded source hash differs: '+url)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        if hashlib.sha256(target.read_bytes()).hexdigest() != pin['sha256']:
            raise ValueError('Source checksum differs: '+str(target))
    return pins


def canonical(rows):
    # Sort whole rows, retaining duplicates and NULL; all keys here are finite,
    # deterministic scalars. Column types never vary except NULL.
    return sorted(rows, key=lambda row: tuple((v is not None, v) for v in row))


def worker(args):
    case = next(c for c in catalog() if c['id'] == args.case)
    sql = case['sql'].format(half=args.rows//2)
    os.environ.pop('CORESQL_PROFILE', None)
    os.environ.pop('CORESQL_PROFILE_QUERY', None)
    if args.profile: os.environ['CORESQL_PROFILE_QUERY'] = sql
    db = Core(str(args.bridge.resolve())) if args.worker == 'coresql' else SQLite(args.sqlite_library) if args.worker == 'sqlite' else reference()
    if args.worker == 'duckdb':
        db.execute("SET memory_limit='1GB'")
        db.execute("SET max_temp_directory_size='0B'")
    def execute(s):
        if args.worker == 'coresql': return db.execute(s)
        try:
            answer = db.execute(s)
            return 'ok', answer.fetchall() if args.worker == 'duckdb' else answer
        except Exception as error:
            return 'error', str(error)
    result = {}
    try:
        digest = hashlib.sha256()
        start = time.perf_counter()
        for statement in statements(case['fixture'], args.rows):
            digest.update(statement.encode()+b'\0')
            status, detail = execute(statement)
            if status != 'ok':
                result = dict(status='load_error', detail=detail)
                break
        else:
            result = dict(load_seconds=time.perf_counter()-start, dataset_sha256=digest.hexdigest())
            status, rows = execute(sql)  # First execution untimed, independent oracle checks in parent.
            result.update(status=status)
            if status != 'ok': result['detail'] = rows
            else:
                wanted = rows if case['ordered'] else canonical(rows)
                samples = []
                for _ in range(0 if args.profile else args.repeats):
                    start = time.perf_counter()
                    status, actual = execute(sql)
                    elapsed = time.perf_counter()-start
                    if status != 'ok':
                        result.update(status=status, detail=actual)
                        break
                    try: compare(actual if case['ordered'] else canonical(actual), wanted)
                    except AssertionError as error:
                        result.update(status='unstable_result', detail=str(error))
                        break
                    samples.append(elapsed)
                result.update(rows=wanted, seconds=samples)
                if samples: result['median_seconds'] = statistics.median(samples)
    finally:
        db.close()
    multiplier = 1 if sys.platform == 'darwin' else 1024
    result['worker_peak_rss_bytes'] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * multiplier
    if args.worker == 'coresql':
        result['bridge_peak_rss_bytes'] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss * multiplier
    print(json.dumps(result, default=encode))


def bounded(command, timeout, env=None):
    # Temp files avoid pipe deadlocks/pipe buffer limits for large result sets.
    # A new group lets a timeout or worker crash also reap the native bridge.
    with tempfile.TemporaryFile(mode='w+') as out, tempfile.TemporaryFile(mode='w+') as err:
        process = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True, env=env)
        try:
            try:
                code = process.wait(timeout=timeout)
                status = 'ok' if code == 0 else 'process_error'
            except subprocess.TimeoutExpired:
                status = 'timeout'
        finally:
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            process.wait()
        out.seek(0); err.seek(0)
        return status, out.read(), err.read()


def run_case(args, engine, case, profile=False):
    command = [sys.executable, str(Path(__file__).resolve()), '--worker', engine,
               '--case', case['id'], '--bridge', str(args.bridge.resolve()),
               '--rows', str(args.rows), '--repeats', str(args.repeats)]
    if args.sqlite_library: command += ['--sqlite-library', str(args.sqlite_library.resolve())]
    if profile: command += ['--profile']
    status, stdout, stderr = bounded(command, args.timeout)
    if status != 'ok': return dict(status=status, detail=stderr[-4000:])
    try: result = json.loads(stdout, object_hook=decode)
    except (ValueError, TypeError): return dict(status='protocol_error', detail=stdout[-1000:])
    if profile:
        result['diagnostics'] = [json.loads(line) for line in stderr.splitlines() if line.startswith('{')]
        if result['status'] == 'ok' and not any('candidate_pairs' in r for r in result['diagnostics']):
            result.update(status='missing_diagnostics', detail='Use a bridge built with test counters')
    elif stderr: result['stderr'] = stderr[-4000:]
    return result


def speedtest(args):
    results = []
    for size in args.speedtest_sizes:
        command = [str(args.speedtest.resolve()), str(size), 'sql', str(args.repeats)]
        env = dict(os.environ)
        env.pop('CORESQL_PROFILE', None); env.pop('CORESQL_PROFILE_QUERY', None)
        status, stdout, stderr = bounded(command, args.speedtest_timeout, env)
        records = list(csv.DictReader(io.StringIO(stdout)))
        expected = {100,110,120,130,140,142,145,150,160,161,170,180,190,200,210,230,240,250,260,270,280,290,300,310,320,400,410,500,510,520,980,990}
        if status == 'ok' and (len(records) != 32*args.repeats or any({int(r['test']) for r in records if int(r['run']) == run} != expected for run in range(1,args.repeats+1))):
            status = 'incomplete'
        summaries = []
        if status == 'ok':
            for case in sorted(expected):
                rows = [r for r in records if int(r['test']) == case]
                summaries.append(dict(test=case, **{engine+'_median_ms': statistics.median(float(r[engine+'_ms']) for r in rows) for engine in ('coresql','sqlite')}))
        results.append(dict(size=size, status=status, records=records, summaries=summaries, stderr=stderr))
        print(f'speedtest1 main size {size}: {status}', flush=True)
    return results


def tpch_summary(path, bridge, reused=False):
    raw = path.read_bytes()
    report = json.loads(raw)
    if report['bridge_sha256'] != hashlib.sha256(bridge.read_bytes()).hexdigest() or report['worktree_source_sha256'] != source_fingerprint(ROOT):
        raise ValueError('TPC-H report does not identify this executable and source tree')
    pin = json.loads((HERE.parent/'tpch/manifest.json').read_text())
    if report['query_revision'] != pin['revision'] or report['duckdb_version'] != VERSION or report['duckdb_revision'] != REVISION:
        raise ValueError('TPC-H report reference pins differ')
    results = report['results']
    expected = {f'q{i:02}.sql' for i in range(1,23)}
    complete = len(results) == 22 and {r['query'] for r in results} == expected
    matched = sum(r.get('coresql',{}).get('correctness') == 'match' and r['coresql']['status'] == 'ok' for r in results)
    return dict(report=str(path.resolve()), sha256=hashlib.sha256(raw).hexdigest(), matched=matched,
                status='ok' if complete and matched == 22 else 'failed', reused=reused,
                created_utc=report['created_utc'], scale=report['scale'], repeats=report['repeats'])


def write_report(args, report):
    args.output.write_text(json.dumps(report, indent=2, default=encode)+'\n')
    lines = ['# Curated SQL benchmark', '', f"Rows parameter: {args.rows}; {args.repeats} measured repetitions after one warmup.", '',
             'Reduced adapted workloads; single-thread in-memory client latency, not official benchmark scores.',
             'CoreSQL includes bridge IPC and decoding; references use Python/C APIs. Loading and checks are untimed.', '',
             'Reference: pinned SQLite for speedtest1 additions; pinned DuckDB for all other curated cases.', '',
             '| Case | Result | CoreSQL ms | Reference ms | Borrowed pairs |', '|---|---|---:|---:|---:|']
    for entry in report['results']:
        core, ref = entry['coresql'], entry[entry['reference']]
        def ms(r): return f"{r['median_seconds']*1000:.3f}" if entry['status'] == 'match' and 'median_seconds' in r else '—'
        pairs = sum(r.get('rows', 0) for r in entry.get('profile', {}).get('diagnostics', []) if r.get('stage') == 'join_pairs')
        lines.append(f"| {entry['case']['id']} | {entry['status']} | {ms(core)} | {ms(ref)} | {pairs} |")
    for run in report.get('speedtest1_main', []):
        lines += ['', f"## speedtest1 main, size {run['size']}: {run['status']}", '', '| Case | CoreSQL ms | SQLite ms |', '|---|---:|---:|']
        lines += [f"| {r['test']} | {r['coresql_median_ms']:.3f} | {r['sqlite_median_ms']:.3f} |" for r in run['summaries']]
    if 'tpch' in report:
        lines += ['', '## TPC-H', '', f"{report['tpch']['matched']}/22 queries match the pinned DuckDB reference. See {report['tpch']['report']}."]
    args.output.with_suffix('.md').write_text('\n'.join(lines)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--sqlite-library', type=Path)
    parser.add_argument('--sources', type=Path)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--rows', type=int, default=10000)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=60, help='Whole engine/case limit, including load and all repetitions')
    parser.add_argument('--speedtest', type=Path)
    parser.add_argument('--speedtest-sizes', type=int, nargs='+', default=[1,10])
    parser.add_argument('--speedtest-timeout', type=float, default=300)
    parser.add_argument('--tpch-data', type=Path)
    parser.add_argument('--tpch-queries', type=Path)
    parser.add_argument('--tpch-report', type=Path, help='Reuse a completed TPC-H report only if binary, source and pins match')
    parser.add_argument('--only', nargs='+', help='Exact workload IDs for diagnosis')
    parser.add_argument('--worker', choices=['coresql','duckdb','sqlite'], help=argparse.SUPPRESS)
    parser.add_argument('--case', help=argparse.SUPPRESS)
    parser.add_argument('--profile', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.rows < 100 or args.rows % 4 or not 1 <= args.repeats <= 20 or any(not math.isfinite(t) or t <= 0 for t in (args.timeout,args.speedtest_timeout)) or any(s < 1 or s > 100 for s in args.speedtest_sizes):
        parser.error('Rows must be >=100 and divisible by four; repeats 1..20; positive finite timeouts; speedtest sizes 1..100')
    if args.worker:
        worker(args); return
    if not args.output or not args.sources or not args.sqlite_library:
        parser.error('--output, --sources and --sqlite-library are required')
    if bool(args.tpch_data) != bool(args.tpch_queries): parser.error('Provide both TPC-H paths')
    if args.tpch_report and args.tpch_data: parser.error('Choose a fresh TPC-H run or a verified existing report')
    if args.output.exists() or args.output.with_suffix('.md').exists(): parser.error('Choose new output files')
    pins = verify_sources(args.sources, args.fetch)
    cases = catalog()
    if args.only:
        if not set(args.only) <= {c['id'] for c in cases}: parser.error('Unknown workload ID')
        cases = [c for c in cases if c['id'] in args.only]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = dict(format=1, purpose='Curated adapted SQL client latency; not official benchmark results',
                  platform=platform.platform(), python=sys.version, rows=args.rows, repeats=args.repeats,
                  timeout_seconds=args.timeout, created_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                  worktree_source_sha256=source_fingerprint(ROOT),
                  commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                  dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT)),
                  bridge_sha256=hashlib.sha256(args.bridge.read_bytes()).hexdigest(),
                  suite_files={str(p.relative_to(HERE)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(HERE.rglob('*')) if p.is_file() and '__pycache__' not in str(p)},
                  duckdb=dict(version=VERSION, revision=REVISION), source_pins=pins,
                  timing='Parse/execute/fetch; CoreSQL additionally serializes results over bridge IPC; no persistent prepared statements; serial fresh in-memory databases; single-thread references; checks/loading/profile pass outside timings.', results=[])
    sqlite = SQLite(args.sqlite_library)
    report['sqlite'] = dict(source_id=sqlite.source_id, library_sha256=hashlib.sha256(args.sqlite_library.read_bytes()).hexdigest(),
                            compile_options=[r[0] for r in sqlite.execute('PRAGMA compile_options')])
    sqlite.close()
    if args.tpch_report:
        report['tpch'] = tpch_summary(args.tpch_report,args.bridge,reused=True)
    for index, case in enumerate(cases):
        reference_engine = 'sqlite' if case['id'].startswith('speedtest1/') else 'duckdb'
        entry = dict(case=case, reference=reference_engine)
        order = ['coresql', reference_engine] if index % 2 == 0 else [reference_engine, 'coresql']
        entry['engine_order'] = order
        for engine in order: entry[engine] = run_case(args, engine, case)
        a, b = entry['coresql'], entry[reference_engine]
        entry['status'] = a['status'] if a['status'] != 'ok' else 'reference_'+b['status'] if b['status'] != 'ok' else 'match'
        if a['status'] == b['status'] == 'ok':
            try:
                if a['dataset_sha256'] != b['dataset_sha256']: raise AssertionError('Input datasets differ')
                compare(a['rows'],b['rows'])
            except AssertionError as error: entry.update(status='wrong_answer', detail=str(error))
        if entry['status'] == 'match':
            entry['profile'] = run_case(args, 'coresql', case, profile=True)
            profile = entry['profile']
            if profile['status'] == 'ok':
                try:
                    compare(profile['rows'], b['rows'])
                    if profile['dataset_sha256'] != b['dataset_sha256']: raise AssertionError('Profile inputs differ')
                except AssertionError as error: profile.update(status='wrong_answer', detail=str(error))
            if profile['status'] != 'ok': entry['status'] = 'profile_'+profile['status']
        for key in order+['profile']:
            r = entry.get(key, {})
            if 'rows' in r:
                rows = r.pop('rows')
                r['row_count'] = len(rows)
                r['result_sha256'] = hashlib.sha256(json.dumps(rows,default=encode).encode()).hexdigest()
        report['results'].append(entry)
        print(case['id']+': '+entry['status']+((' '+entry.get('detail','')) if entry['status']=='wrong_answer' else ''), flush=True)
        write_report(args, report)
    if args.speedtest:
        report['speedtest1_main'] = speedtest(args)
        report['speedtest_binary_sha256'] = hashlib.sha256(args.speedtest.read_bytes()).hexdigest()
        write_report(args,report)
    if args.tpch_data:
        path = args.output.with_name(args.output.stem+'-tpch.json')
        if path.exists(): raise RuntimeError('TPC-H output already exists')
        command = [sys.executable, str(HERE.parent/'tpch/measure.py'), '--bridge', str(args.bridge.resolve()),
                   '--data', str(args.tpch_data.resolve()), '--queries', str(args.tpch_queries.resolve()),
                   '--output', str(path.resolve()), '--repeats', str(args.repeats), '--timeout', '30']
        code = subprocess.run(command, check=False).returncode
        report['tpch'] = tpch_summary(path,args.bridge) if path.exists() else dict(report=str(path.resolve()), matched=0, status='failed')
        report['tpch']['exit_code'] = code
        if code: report['tpch']['status'] = 'failed'
    write_report(args,report)
    if any(r['status'] != 'match' or r.get('profile',{}).get('status') != 'ok' for r in report['results']) or any(r['status'] != 'ok' for r in report.get('speedtest1_main', [])) or report.get('tpch',{}).get('status','ok') != 'ok':
        sys.exit(1)  # Unsupported is coverage debt, never a passing performance result.


if __name__ == '__main__':
    main()

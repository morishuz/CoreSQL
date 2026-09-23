#!/usr/bin/env python3
"""Run isolated application benchmarks serially and retain raw latency samples."""
import argparse
import csv
import hashlib
import json
import math
import os
import platform
import statistics
import subprocess
import time
from pathlib import Path


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def summarize(path):
    phases = {}
    for row in csv.DictReader(path.open()):
        phases.setdefault(row['phase'], []).append(row)
    result = {}
    for phase, rows in phases.items():
        times = sorted(float(row['milliseconds']) for row in rows)
        count = len(times)
        entry = {'samples': count, 'total_ms': sum(times),
                 'units_per_second': sum(int(r['units']) for r in rows) * 1000 / sum(times),
                 'min_ms': times[0], 'max_ms': times[-1],
                 'first_rss_bytes': int(rows[0]['rss_bytes']),
                 'last_rss_bytes': int(rows[-1]['rss_bytes']),
                 'max_rss_bytes': max(int(r['rss_bytes']) for r in rows),
                 'process_peak_rss_bytes': max(int(r['peak_rss_bytes']) for r in rows),
                 'last_file_bytes': int(rows[-1]['file_bytes'])}
        # Maintenance/reopen with few observations must not imply reliable tails.
        for q, minimum in [(50, 2), (95, 20), (99, 100)]:
            entry[f'p{q}_ms'] = times[math.ceil(count * q / 100) - 1] if count >= minimum else None
        for key in ['core_page_reads', 'core_page_writes', 'core_written_bytes', 'core_checkpoints']:
            entry[key + '_first'] = int(rows[0][key])
            entry[key + '_last'] = int(rows[-1][key])
        result[phase] = entry
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--operations', type=int, default=1000)
    parser.add_argument('--covering-index', action='store_true', help='Use (device,ts,id) in both engines instead of (device,ts)')
    parser.add_argument('--timeout', type=int, default=1800)
    parser.add_argument('--scenarios', nargs='+', default=['memory-small', 'memory-large', 'durable-small', 'durable-cache-pressure'])
    args = parser.parse_args()
    scenarios = {'memory-small': ('memory', 10000, 128, 0),
                 'memory-large': ('memory', 100000, 1024, 0),
                 'durable-small': ('durable', 10000, 128, 0),
                 'durable-cache-pressure': ('durable', 100000, 1024, 4)}
    if args.runs < 1 or not 2 <= args.operations <= 1000000 or any(s not in scenarios for s in args.scenarios):
        parser.error('Invalid run count, operations or scenario')
    args.output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    root = Path(__file__).resolve().parents[2]
    def git(*arguments):
        return subprocess.check_output(['git', *arguments], cwd=root, text=True).strip()
    build_options = {}
    cache_path = binary.parent / 'CMakeCache.txt'
    if cache_path.exists():
        keys = {'CMAKE_BUILD_TYPE', 'BUILD_TESTING', 'CORESQL_SANITIZERS', 'CORESQL_BENCHMARKS', 'CORESQL_MAX_ENCODED_MIB'}
        for line in cache_path.read_text().splitlines():
            if ':' in line and '=' in line and line.split(':', 1)[0] in keys:
                build_options[line.split(':', 1)[0]] = line.split('=', 1)[1]
    manifest = {'covering_index': args.covering_index, 'cmake_options': build_options, 'source_revision': git('rev-parse', 'HEAD'), 'source_diff': git('diff', '--stat'),
                'binary': str(binary), 'binary_sha256': digest(binary),
                'harness_sha256': {str(p.relative_to(root)): digest(p) for p in Path(__file__).parent.glob('*') if p.is_file()},
                'platform': platform.platform(), 'machine': platform.machine(),
                'sqlite_reference': json.loads((root / 'benchmarks/reference/sqlite.json').read_text()),
                'operations': args.operations, 'runs': [], 'started_unix': time.time(),
                'scratch_directory': str(args.output.resolve()),
                'method': 'TMPDIR and database files on the output volume. Fresh process/database per engine/run; alternating order; SQL prepared before phases; cache disabled; warm OS cache; no physical RAM exhaustion claim; SQLite WAL/FULL/fullfsync; CoreSQL synchronized commits.'}
    for name in args.scenarios:
        mode, rows, payload, cache = scenarios[name]
        for repeat in range(args.runs):
            engines = ['coresql', 'sqlite'] if repeat % 2 == 0 else ['sqlite', 'coresql']
            for engine in engines:
                stem = f'{name}-{repeat}-{engine}'
                csv_path, err_path = args.output / (stem + '.csv'), args.output / (stem + '.stderr')
                command = [str(binary), engine, mode, str(rows), str(payload), str(args.operations), str(cache), str(args.output.resolve()), 'all']
                if args.covering_index:
                    command.append('covering')
                entry = {'scenario': name, 'repeat': repeat, 'engine': engine, 'command': command,
                         'rows': rows, 'payload_bytes': payload, 'cache_mib': cache, 'status': 'running'}
                print(stem, flush=True)
                start = time.monotonic()
                with csv_path.open('w') as out, err_path.open('w') as err:
                    try:
                        run = subprocess.run(command, stdout=out, stderr=err, timeout=args.timeout,
                                             env={**os.environ, "TMPDIR": str(args.output.resolve()),
                                                  "SQLITE_TMPDIR": str(args.output.resolve())})
                        entry['status'] = 'passed' if run.returncode == 0 and 'VERIFIED' in err_path.read_text() else 'failed'
                        entry['returncode'] = run.returncode
                    except subprocess.TimeoutExpired:
                        entry['status'] = 'timeout'
                entry['elapsed_seconds'] = time.monotonic() - start
                entry['stderr'] = err_path.read_text()
                if entry['status'] == 'passed':
                    entry['phases'] = summarize(csv_path)
                manifest['runs'].append(entry)
                (args.output / 'results.json').write_text(json.dumps(manifest, indent=2) + '\n')
    lines = ['| Scenario | Operation | CoreSQL p50 ms | SQLite p50 ms | CoreSQL / SQLite |',
             '| --- | --- | ---: | ---: | ---: |']
    for name in args.scenarios:
        successful = [r for r in manifest['runs'] if r['scenario'] == name and r['status'] == 'passed']
        if len(successful) != args.runs * 2:
            lines.append(f'| {name} | INCOMPLETE: see results.json | — | — | — |')
            continue
        for phase in successful[0]['phases']:
            if phase.endswith(('_execute', '_commit')) or phase in ('load_and_initial_checkpoint', 'checkpoint', 'reopen'):
                continue
            medians = {e: statistics.median(r['phases'][phase]['p50_ms'] for r in successful if r['engine'] == e) for e in ['coresql', 'sqlite']}
            a, b = medians['coresql'], medians['sqlite']
            lines.append(f'| {name} | {phase} | {a:.4f} | {b:.4f} | {a/b:.2f} |')
    (args.output / 'comparison.md').write_text('\n'.join(lines) + '\n')
    if any(r['status'] != 'passed' for r in manifest['runs']):
        raise SystemExit('Some runs failed or timed out; do not assign them speed ratios.')


if __name__ == '__main__':
    main()

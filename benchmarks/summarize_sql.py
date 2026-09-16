#!/usr/bin/env python3
"""Summarize complete SQL-mode speedtest1 runs; refuse partial measurements."""
import csv
import math
import statistics
import sys
from collections import defaultdict

CASES = {
    100: 'Unindexed inserts', 110: 'Ordered unique inserts', 120: 'Unordered unique inserts',
    130: 'Numeric filters', 140: 'LIKE filters', 142: 'LIKE + full sort', 145: 'LIKE + top 10',
    150: 'Create indexes', 160: 'Indexed numeric ranges', 161: 'Indexed unique ranges',
    170: 'Indexed text ranges', 180: 'INSERT SELECT into indexed table', 190: 'Clear and refill',
    200: 'Native vacuum equivalent', 210: 'ADD COLUMN + aggregate',
    230: 'Range updates', 240: 'Individual updates', 250: 'Whole-table update',
    260: 'Aggregate updated column', 270: 'Range deletes', 280: 'Individual deletes',
    290: 'REPLACE SELECT', 300: 'Filtered refill', 310: 'Four-table joins',
    320: 'Correlated scalar subquery', 400: 'Integer-key replacements',
    410: 'Integer-key lookups', 500: 'Text-key replacements', 510: 'Text-key lookups',
    520: 'DISTINCT', 980: 'Native integrity equivalent', 990: 'Native analyze equivalent',
}
FIELDS = ('coresql_ms', 'sqlite_ms', 'coresql_prepare_ms', 'sqlite_prepare_ms',
          'coresql_execute_ms', 'sqlite_execute_ms')


def main(path):
    with open(path, newline='') as file:
        rows = list(csv.DictReader(file))
    runs = defaultdict(set)
    samples = defaultdict(list)
    for row in rows:
        run, case = int(row['run']), int(row['test'])
        if run < 1 or case not in CASES or case in runs[run]:
            raise ValueError('Invalid or duplicate run/case')
        if row['first'] != ('coresql' if run % 2 else 'sqlite'):
            raise ValueError('Unexpected engine order')
        for field in FIELDS:
            row[field] = float(row[field])
            if not math.isfinite(row[field]) or row[field] < 0:
                raise ValueError('Invalid measurement')
        runs[run].add(case)
        samples[case].append(row)
    if len(runs) < 2 or set(runs) != set(range(1, len(runs) + 1)) or any(set(CASES) != cases for cases in runs.values()):
        raise ValueError('Need at least two complete consecutive 32-case runs')
    print(f'{len(runs)} complete runs; values are median milliseconds, with min–max in parentheses.\n')
    print('| Case | Workload | CoreSQL | SQLite | Faster |')
    print('|---|---|---:|---:|---:|')
    def cell(values):
        return f'{statistics.median(values):.2f} ({min(values):.2f}–{max(values):.2f})'
    for case, name in CASES.items():
        core = [r['coresql_ms'] for r in samples[case]]
        sqlite = [r['sqlite_ms'] for r in samples[case]]
        a, b = statistics.median(core), statistics.median(sqlite)
        ratio = 'Not comparable' if case in (200, 980, 990) else ('Below timer resolution' if min(a, b) == 0 else (f'CoreSQL {b / a:.2f}×' if a < b else f'SQLite {a / b:.2f}×'))
        print(f'| {case} | {name} | {cell(core)} | {cell(sqlite)} | {ratio} |')
    print('\nNo combined suite score is calculated; maintenance operations are native equivalents.')
    print('\n| Case | CoreSQL parse | SQLite prepare | CoreSQL execute | SQLite execute |')
    print('|---|---:|---:|---:|---:|')
    for case in CASES:
        medians = [statistics.median(r[f] for r in samples[case]) for f in FIELDS[2:]]
        print(f'| {case} | ' + ' | '.join(f'{value:.3f}' for value in medians) + ' |')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: summarize_sql.py complete-sql-mode.csv')
    main(sys.argv[1])

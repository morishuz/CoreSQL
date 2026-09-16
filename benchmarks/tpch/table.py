"""Render complete three-engine timing tables, withholding invalid speedup ratios."""
import argparse
import json
from pathlib import Path


def table(report):
    lines = [f"## SF {report['scale']}", '',
             'Median milliseconds; SQLite/CoreSQL > 1 means CoreSQL is faster.', '',
             '| Query | CoreSQL ms | SQLite ms | DuckDB ms | SQLite/CoreSQL |',
             '| --- | ---: | ---: | ---: | ---: |']
    for row in report['results']:
        cells = []
        for engine in ('coresql', 'sqlite', 'duckdb'):
            result = row[engine]
            if result['status'] != 'ok':
                cells.append(result['status'].replace('_', ' '))
            elif result['correctness'] not in ('match', 'approximate_match'):
                cells.append(result['correctness'].replace('_', ' '))
            else:
                cells.append(f"{result['median_seconds'] * 1000:.3f}")
        ratio = row.get('sqlite_over_coresql')
        if ratio is not None:
            assert row['coresql']['correctness'] == 'match'
            assert row['sqlite']['correctness'] == 'approximate_match'
        lines.append('| ' + ' | '.join([row['query'][1:3], *cells,
                                      f'{ratio:.3g}×' if ratio is not None else '—']) + ' |')
    lines += ['', '— means no valid completed comparison; it is not a zero runtime.', '']
    for engine in ('coresql', 'sqlite', 'duckdb'):
        valid = sum(row[engine]['correctness'] in ('match', 'approximate_match') for row in report['results'])
        lines.append(f"- {engine}: {valid}/{len(report['results'])} completed answer checks.")
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    text = ['# CoreSQL, SQLite and DuckDB: TPC-H workload', '',
            'These are single-thread, in-memory client-latency measurements on the same Apple M1 (16 GiB) desktop, with three timed executions after one untimed execution. Each engine/query starts with a fresh database. Loading and answer checks are excluded; SQL parsing and fetching all results are included. No persistent prepared statements or user-created indexes are used.', '',
            '**SQLite uses an adapted precision profile:** ISO TEXT dates and approximate REAL decimals; DATE, EXTRACT, SUBSTRING and Q13 derived-column aliases receive explicit syntax adaptations. CoreSQL and DuckDB keep the original pinned queries and native DATE/DECIMAL. SQLite results match within 1e-10 relative/absolute numeric tolerance; this does not establish exact-decimal equivalence.', '',
            'CoreSQL uses its pipe bridge, SQLite the pinned C library via ctypes, and DuckDB its Python binding. Different conversion overhead affects short queries. SQLite keeps its automatic transient indexes enabled and uses case-sensitive LIKE and in-memory temporary storage. CoreSQL uses the existing test-enabled Release build; SQLite is compiled with `cc -O3 -DNDEBUG -dynamiclib`. This is an engineering comparison, not an official TPC-H score.', '',
            'All runs use 15-second per-execution and 120-second load limits, and an approximately 100 ms sampled 2 GiB process-group RSS stopping threshold (which can overshoot). DuckDB additionally has a 1 GB internal memory limit and spill disabled. Resource-limited results receive no ratio.', '',
            'The ratio is **SQLite median time / CoreSQL median time**: 2× means CoreSQL is twice as fast; 0.5× means CoreSQL takes twice as long. Ratios compare these disclosed profiles, not identical numeric implementations.', '']
    for path in args.inputs:
        report = json.loads(path.read_text())
        assert len(report['results']) == 22, 'Refuse to render a partial report as a complete query set'
        text += [table(report), '',
                 f"Measured {report['created_utc']}; engine order: {', '.join(report['engine_order'])}.",
                 f"CoreSQL `{report['coresql_commit'][:10]}`; SQLite {report['sqlite']['version']}; DuckDB {report['duckdb_version']}.",
                 f'Raw evidence: [{path.name}]({path.name}).', '']
    text += ['## Reproduce', '',
             'See [runner, reference build and adaptation instructions](../README.md#include-the-pinned-sqlite-reference). Raw JSON records all repetitions, per-engine answer checks, memory, limits and binary/query/adapter fingerprints. The generic raw `schema` field describes CoreSQL/DuckDB; `sqlite.profile` records SQLite’s different schema and settings.', '',
             'Regenerate these tables with `python3 benchmarks/tpch/table.py benchmarks/tpch/results/optimization-round2-sf0.03.json --output /tmp/coresql-tpch-table.md`.', '']
    args.output.write_text('\n'.join(text))


if __name__ == '__main__':
    main()

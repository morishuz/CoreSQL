"""Check pinned Q2 and Q16 on a tiny synthetic fixture, not generated TPC-H data."""
import argparse
from pathlib import Path
from audit import load_queries
from run import Core, SQLite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--queries', type=Path, required=True)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--duckdb', action='store_true', help='Use the pinned DuckDB oracle')
    args = parser.parse_args()
    manifest, queries = load_queries(args.queries, args.fetch)
    queries = dict(queries)
    from oracle import DuckDB
    core, sqlite = Core(str(args.bridge.resolve())), DuckDB() if args.duckdb else SQLite()

    def both(sql):
        actual, reference = core.execute(sql), sqlite.execute(sql)
        assert actual[0] == reference[0] == 'ok', (sql, actual, reference)
        assert actual[1] == [list(row) for row in reference[1]], (sql, actual, reference)
        return actual[1]

    def insert(table, columns, rows):
        for row in rows:
            values = ','.join("'" + v.replace("'", "''") + "'" if isinstance(v, str) else str(v)
                              for v in row)
            both(f'INSERT INTO {table}({columns}) VALUES({values})')

    try:
        for ddl in manifest['diagnostic_schema']:
            both(ddl)
        insert('region', 'r_regionkey,r_name', [(1, 'EUROPE'), (2, 'ASIA')])
        insert('nation', 'n_nationkey,n_regionkey,n_name', [
            (1, 1, 'GERMANY'), (2, 1, 'FRANCE'), (3, 2, 'CANADA'),
        ])
        insert('supplier', 's_suppkey,s_nationkey,s_name,s_acctbal,s_address,s_phone,s_comment', [
            (1, 1, 's1', 10, 'a1', 'p1', 'good'),
            (2, 2, 's2', 20, 'a2', 'p2', 'good'),
            (3, 3, 's3', 30, 'a3', 'p3', 'good'),
            (4, 1, 's4', 40, 'a4', 'p4', 'Customer bad Complaints'),
        ])
        insert('part', 'p_partkey,p_size,p_type,p_brand,p_mfgr', [
            (1, 15, 'ECONOMY BRASS', 'Brand#11', 'm1'),
            (2, 15, 'STANDARD BRASS', 'Brand#11', 'm2'),
            (3, 49, 'MEDIUM POLISHED TIN', 'Brand#11', 'm3'),
            (4, 49, 'ECONOMY TIN', 'Brand#11', 'm4'),
            (5, 49, 'ECONOMY TIN', 'Brand#45', 'm5'),
            (6, 14, 'ECONOMY TIN', 'Brand#11', 'm6'),
        ])
        # Equal European prices must both survive Q2; cheaper Asian supply must
        # not set its minimum. Q16 excludes the complaint supplier and two parts.
        insert('partsupp', 'ps_partkey,ps_suppkey,ps_supplycost', [
            (1, 1, 10), (1, 2, 10), (1, 3, 1), (2, 1, 20), (2, 2, 15),
            (3, 1, 1), (4, 1, 1), (4, 2, 1), (4, 4, 1), (5, 1, 1), (6, 1, 1),
        ])
        for query, count in [(2, 3), (16, 2)]:
            rows = both(queries[f'q{query:02}.sql'])
            assert len(rows) == count, rows
            if query == 2:
                assert [(row[1], row[3]) for row in rows] == [('s2', 1), ('s2', 2), ('s1', 1)]
            else:
                assert rows == [['Brand#11', 'ECONOMY TIN', 49, 2], ['Brand#11', 'ECONOMY TIN', 14, 1]]
            print(f'Q{query}: {len(rows)} rows matched {"DuckDB" if args.duckdb else "SQLite"} and explicit fixture expectations')
    finally:
        core.close()
        sqlite.close()


if __name__ == '__main__':
    main()

"""Check nine pinned date queries on synthetic boundary fixtures, not TPC-H data.

CoreSQL executes the checksum-verified query text unchanged, using native DATE.
The SQLite oracle uses ISO TEXT dates and replaces only fixed DATE literals/casts
with their ISO strings. CoreSQL uses native DECIMAL. SQLite is an adapted REAL oracle; --duckdb checks
exact decimals on both sides.
"""
import argparse
import math
from decimal import Decimal
from pathlib import Path
import re
from audit import load_queries
from run import Core, SQLite

DATE_CAST = re.compile(r"CAST\(('\d{4}-\d{2}-\d{2}') AS date\)", re.IGNORECASE)
DATE_LITERAL = re.compile(r"\bdate ('\d{4}-\d{2}-\d{2}')", re.IGNORECASE)


def insertion(table, **columns):
    def literal(value):
        if value is None:
            return 'NULL'
        if isinstance(value, str):
            return "'" + value.replace("'", "''") + "'"
        return str(value)
    return f"INSERT INTO {table}({','.join(columns)}) VALUES({','.join(map(literal, columns.values()))})"


def line(**changes):
    fields = dict(l_orderkey=1, l_partkey=1, l_suppkey=1, l_quantity=10,
                  l_extendedprice=100, l_discount=0.06, l_tax=0.1,
                  l_returnflag='R', l_linestatus='O', l_shipmode='MAIL',
                  l_shipdate='1994-01-01', l_commitdate='1994-01-02', l_receiptdate='1994-01-03')
    fields.update(changes)
    return insertion('lineitem', **fields)


def order(**changes):
    fields = dict(o_orderkey=1, o_custkey=1, o_orderdate='1994-01-01',
                  o_orderpriority='1-URGENT', o_shippriority=0)
    fields.update(changes)
    return insertion('orders', **fields)


CUSTOMER = insertion('customer', c_custkey=1, c_name='c1', c_nationkey=1,
                     c_mktsegment='BUILDING', c_acctbal=5, c_address='a', c_phone='p', c_comment='c')
NATION = insertion('nation', n_nationkey=1, n_name='CANADA', n_regionkey=1)
SUPPLIER = insertion('supplier', s_suppkey=1, s_name='s1', s_nationkey=1, s_address='address')

# Each query gets a fresh database. Boundary rows differ by one predicate;
# explicit answers prevent a comparison of two empty or identically wrong sets.
CASES = [
    (1, [line(l_shipdate='1998-09-02'), line(l_shipdate='1998-09-03'), line(l_shipdate=None)],
     [['R', 'O', 10, 100, 94, 103.4, 10, 100, 0.06, 1]]),
    (3, [CUSTOMER, order(o_orderdate='1995-03-14'), order(o_orderkey=2, o_orderdate='1995-03-15'),
         line(l_shipdate='1995-03-16'), line(l_shipdate='1995-03-15'),
         line(l_orderkey=2, l_shipdate='1995-03-16'), line(l_shipdate=None)],
     [[1, 94, '1995-03-14', 0]]),
    (4, [order(o_orderdate='1993-07-01'), order(o_orderkey=2, o_orderdate='1993-10-01'),
         order(o_orderkey=3, o_orderdate='1993-06-30'), order(o_orderkey=4, o_orderdate=None),
         line(), line(), line(l_orderkey=2), line(l_orderkey=3), line(l_orderkey=4)],
     [['1-URGENT', 1]]),
    (5, [CUSTOMER, NATION, SUPPLIER, insertion('region', r_regionkey=1, r_name='ASIA'),
         order(), order(o_orderkey=2, o_orderdate='1995-01-01'),
         order(o_orderkey=3, o_orderdate='1993-12-31'), order(o_orderkey=4, o_orderdate=None),
         line(), line(l_orderkey=2), line(l_orderkey=3), line(l_orderkey=4)],
     [['CANADA', 94]]),
    (6, [line(), line(l_shipdate='1994-12-31', l_discount=0.07),
         line(l_shipdate='1995-01-01'), line(l_shipdate='1993-12-31'), line(l_quantity=24),
         line(l_discount=0.08), line(l_shipdate=None)], [[13]]),
    (10, [CUSTOMER, NATION, order(o_orderdate='1993-10-01'),
          order(o_orderkey=2, o_orderdate='1994-01-01'), order(o_orderkey=3, o_orderdate='1993-09-30'),
          line(), line(l_returnflag='N'), line(l_orderkey=2), line(l_orderkey=3)],
     [[1, 'c1', 94, 5, 'CANADA', 'a', 'p', 'c']]),
    (12, [order(), order(o_orderkey=2, o_orderpriority='3-MEDIUM'),
          line(l_shipdate='1993-12-30', l_commitdate='1993-12-31', l_receiptdate='1994-01-01'),
          line(l_orderkey=2), line(l_receiptdate='1995-01-01'),
          line(l_shipdate='1994-01-02'), line(l_commitdate='1994-01-03'), line(l_receiptdate=None)],
     [['MAIL', 1, 1]]),
    (14, [insertion('part', p_partkey=1, p_type='PROMO TIN'),
          insertion('part', p_partkey=2, p_type='STANDARD TIN'),
          line(l_shipdate='1995-09-01'), line(l_partkey=2, l_shipdate='1995-09-30'),
          line(l_shipdate='1995-10-01'), line(l_shipdate='1995-08-31'), line(l_shipdate=None)], [[50]]),
    (20, [NATION, SUPPLIER, insertion('supplier', s_suppkey=2, s_name='s2', s_nationkey=1, s_address='a2'),
          insertion('part', p_partkey=1, p_name='forest green'),
          insertion('partsupp', ps_partkey=1, ps_suppkey=1, ps_availqty=6),
          insertion('partsupp', ps_partkey=1, ps_suppkey=2, ps_availqty=5),
          line(), line(l_suppkey=2), line(l_shipdate='1995-01-01'),
          line(l_shipdate='1993-12-31'), line(l_shipdate=None)], [['s1', 'address']]),
]


def compare(actual, expected, exact=False):
    assert len(actual) == len(expected), (actual, expected)
    for row, reference in zip(actual, expected):
        assert len(row) == len(reference), (row, reference)
        for value, wanted in zip(row, reference):
            if isinstance(value, Decimal) and (exact or isinstance(wanted, Decimal)):
                assert value == Decimal(str(wanted)), (value, wanted)
            elif isinstance(wanted, (int, float)):
                assert isinstance(value, (int, float, Decimal)) and math.isclose(value, wanted, rel_tol=1e-12, abs_tol=1e-12), (value, wanted)
            else:
                if hasattr(wanted, 'isoformat'): wanted=wanted.isoformat()
                assert value == wanted, (value, wanted)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--queries', type=Path, required=True)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--duckdb', action='store_true', help='Use the pinned DuckDB oracle without query adaptations')
    args = parser.parse_args()
    manifest, queries = load_queries(args.queries, args.fetch)
    queries = dict(queries)
    for number, inserts, expected in CASES:
        from oracle import DuckDB
        core, sqlite = Core(str(args.bridge.resolve())), DuckDB() if args.duckdb else SQLite()
        try:
            for ddl in manifest['diagnostic_schema']:
                assert core.execute(ddl)[0] == 'ok', ddl
                assert sqlite.execute(ddl if args.duckdb else re.sub(r'\bDATE\b', 'TEXT', ddl).replace('DECIMAL(15,2)','REAL'))[0] == 'ok', ddl
            for sql in inserts:
                for engine in (core, sqlite):
                    result = engine.execute(sql)
                    assert result[0] == 'ok', (sql, result)
            sql = queries[f'q{number:02}.sql']
            actual = core.execute(sql)
            oracle = sqlite.execute(DATE_LITERAL.sub(r'\1', DATE_CAST.sub(r'\1', sql)))
            assert actual[0] == oracle[0] == 'ok', (number, actual, oracle)
            compare(actual[1], oracle[1])
            compare(actual[1], expected, exact=True)
            print(f'Q{number}: {len(actual[1])} rows matched {"DuckDB" if args.duckdb else "adapted SQLite"} and explicit boundary expectations')
        finally:
            core.close()
            sqlite.close()


if __name__ == '__main__':
    main()

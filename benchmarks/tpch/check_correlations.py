"""Nonempty Q17/Q21 correlation fixtures using the unchanged pinned query.

Synthetic correctness coverage, not generated data or a timing measurement.
"""
import argparse
from pathlib import Path
from audit import load_queries
from oracle import DuckDB
from run import Core
from validate import compare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--queries', type=Path, required=True)
    args = parser.parse_args()
    manifest, queries = load_queries(args.queries)
    core, reference = Core(str(args.bridge.resolve())), DuckDB()
    def both(sql):
        actual, expected = core.execute(sql), reference.execute(sql)
        assert actual[0] == expected[0] == 'ok', (sql, actual, expected)
        compare(actual[1], expected[1])
        return actual[1]
    try:
        for ddl in manifest['diagnostic_schema']:
            both(ddl)
        both("INSERT INTO nation(n_nationkey,n_name) VALUES(1,'SAUDI ARABIA'),(2,'OTHER')")
        both("INSERT INTO supplier(s_suppkey,s_nationkey,s_name) VALUES(1,1,'s1'),(2,2,'s2'),(3,1,'s3')")
        both("INSERT INTO orders(o_orderkey,o_orderstatus) VALUES(10,'F'),(20,'F'),(30,'O'),(40,'F')")
        for order, supplier, late in [(10,1,True),(10,1,True),(10,2,False),
                                      (20,1,True),(20,2,True),(30,1,True),
                                      (30,2,False),(40,3,True)]:
            receipt, commit = ('1995-01-03','1995-01-02') if late else ('1995-01-02','1995-01-03')
            both(f"INSERT INTO lineitem(l_orderkey,l_suppkey,l_receiptdate,l_commitdate) "
                 f"VALUES({order},{supplier},'{receipt}','{commit}')")
        rows = both(dict(queries)['q21.sql'])
        compare(rows, [['s1',2]])
        print('Q21: matches native DuckDB and explicit duplicate/late-supplier expectations')
        both("INSERT INTO part(p_partkey,p_brand,p_container) VALUES(1,'Brand#23','MED BOX'),(2,'Other','MED BOX')")
        both("INSERT INTO lineitem(l_partkey,l_quantity,l_extendedprice) VALUES(1,1,70),(1,9,900),(1,20,2000),(1,NULL,500),(2,0,7000)")
        compare(both(dict(queries)['q17.sql']), [[10.0]])
        print('Q17: nonempty correlated AVG matches native DuckDB and expected revenue 10.0')

    finally:
        core.close()
        reference.close()


if __name__ == '__main__':
    main()

"""Nonempty known-answer fixtures for Q7-9, Q13, Q15, Q18 and Q22.

Uses unchanged pinned queries and native DATE/DECIMAL types in both engines.
This fixture is synthetic, not dbgen output or a performance measurement.
"""
import argparse
from datetime import date
from decimal import Decimal as D
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
    queries = dict(queries)
    core, reference = Core(str(args.bridge.resolve())), DuckDB()
    def both(sql):
        a, b = core.execute(sql), reference.execute(sql)
        assert a[0] == b[0] == 'ok', (sql, a, b)
        compare(a[1], b[1])
        return a[1]
    def insert(table, columns, rows):
        for row in rows:
            values = ','.join("'"+str(v).replace("'", "''")+"'" for v in row)
            both(f'INSERT INTO {table}({columns}) VALUES({values})')
    try:
        for ddl in manifest['diagnostic_schema']: both(ddl)
        insert('region','r_regionkey,r_name',[(1,'EUROPE'),(2,'AMERICA')])
        insert('nation','n_nationkey,n_regionkey,n_name',[
            (1,1,'FRANCE'),(2,1,'GERMANY'),(3,2,'BRAZIL'),(4,2,'UNITED STATES')])
        insert('supplier','s_suppkey,s_nationkey,s_name,s_address,s_phone',[
            (1,1,'s1','a1','p1'),(2,2,'s2','a2','p2'),(3,3,'s3','a3','p3'),(4,4,'s4','a4','p4')])
        insert('customer','c_custkey,c_nationkey,c_name,c_phone,c_acctbal',[
            (1,2,'c1','13-1',100),(2,1,'c2','31-2',200),(3,4,'c3','13-3',50),
            (4,4,'c4','13-4',500),(5,4,'c5','31-5',600)])
        insert('part','p_partkey,p_name,p_type',[(1,'forest green','ECONOMY ANODIZED STEEL')])
        insert('partsupp','ps_partkey,ps_suppkey,ps_supplycost',[(1,s,2) for s in range(1,5)])
        insert('orders','o_orderkey,o_custkey,o_orderdate,o_totalprice,o_comment',[
            (10,1,'1995-01-01',1000,'normal'),(20,2,'1996-01-01',2000,'normal'),
            (30,3,'1995-02-01',3000,'normal'),(40,3,'1995-03-01',4000,'special abc requests')])
        insert('lineitem','l_orderkey,l_partkey,l_suppkey,l_quantity,l_extendedprice,l_discount,l_shipdate',[
            (10,1,1,350,100,'0.1','1996-01-15'),(20,1,2,10,200,'0.2','1996-01-15'),
            (30,1,3,10,100,0,'1996-01-15'),(40,1,4,10,300,0,'1996-01-15')])
        expected = {
            7: [['FRANCE','GERMANY',1996,D('90')],['GERMANY','FRANCE',1996,D('160')]],
            8: [[1995,0.25]],
            9: [['BRAZIL',1995,D('80')],['FRANCE',1995,D('-610')],
                ['GERMANY',1996,D('140')],['UNITED STATES',1995,D('280')]],
            13: [[1,3],[0,2]],
            15: [[4,'s4','a4','p4',D('300')]],
            18: [['c1',1,10,date(1995,1,1),D('1000'),D('350')]],
            22: [['13',1,D('500')],['31',1,D('600')]],
        }
        for number, wanted in expected.items():
            rows = both(queries[f'q{number:02}.sql'])
            compare(rows, wanted)
            print(f'Q{number}: {len(rows)} rows matched DuckDB and explicit nonempty expectations')
    finally:
        core.close(); reference.close()


if __name__ == '__main__':
    main()

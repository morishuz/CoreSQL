"""Differential coverage for membership, compounds, joins, and grouping."""
import sys
from run import Core, SQLite

core, sqlite = Core(sys.argv[1]), SQLite()
count = 0

def check(sql):
    global count
    a, b = core.execute(sql), sqlite.execute(sql)
    assert a[0] == b[0] == 'ok', (sql, a, b)
    assert a[1] == [list(row) for row in b[1]], (sql, a, b)
    count += 1

try:
    for sql in ["CREATE TABLE t(a INTEGER,b INTEGER)",
                "INSERT INTO t VALUES (1,2),(1,3),(2,NULL),(NULL,4),(3,2)",
                "CREATE TABLE u(c INTEGER)", "INSERT INTO u VALUES (1),(1),(2),(NULL)"]:
        check(sql)
    for value in ['NULL', '0', '1', '2', '1.0']:
        for candidates in ['', 'NULL', '1,NULL', '1,2', 'SELECT c FROM u', 'SELECT c FROM u WHERE 0']:
            for op in ['IN', 'NOT IN']:
                check(f'SELECT {value} {op} ({candidates})')
    for op in ['UNION', 'UNION ALL', 'INTERSECT', 'EXCEPT']:
        check(f'SELECT a FROM t {op} SELECT c FROM u ORDER BY 1')
        check(f'SELECT a FROM t {op} SELECT c FROM u ORDER BY 1 DESC LIMIT 2')
        check(f'SELECT 1 {op} SELECT 2')
    for sql in [
        'SELECT 1 UNION ALL SELECT 1 EXCEPT SELECT 2 UNION ALL SELECT 3 ORDER BY 1',
        'SELECT a,a IN (SELECT c FROM u WHERE c=t.a),a NOT IN (SELECT c FROM u WHERE c=t.a) FROM t ORDER BY a,b',
        'SELECT a FROM t WHERE a IN (SELECT c FROM u UNION SELECT b FROM t) ORDER BY a,b',
        'SELECT a,c FROM t CROSS JOIN u ORDER BY a,c LIMIT 7',
        'SELECT a,c FROM t JOIN u ON a<c ORDER BY a,c',
        'SELECT a,c FROM t INNER JOIN u ON a=c WHERE b>1 ORDER BY a,c',
        'SELECT a,c FROM t,u WHERE a+1=c OR c IS NULL ORDER BY a,c',
        'SELECT a,c FROM t,u WHERE a=c ORDER BY b DESC LIMIT 2',
        'SELECT count(*),sum(b)+1,coalesce(sum(b),0) FROM t',
        'SELECT a,count(*),sum(b),max(b)-min(b) FROM t GROUP BY a ORDER BY a',
        'SELECT a+1,count(*) FROM t GROUP BY a+1 ORDER BY 1',
        'SELECT a+1,count(*) FROM t GROUP BY a ORDER BY 1',
        'SELECT a,sum(b) FROM t GROUP BY a HAVING count(*)>1 ORDER BY sum(b) DESC',
        'SELECT a FROM t GROUP BY a ORDER BY count(*) DESC,a',
        'SELECT DISTINCT count(*) FROM t GROUP BY a ORDER BY 1',
        'SELECT count(*),sum(b)+1 FROM t WHERE 0',
        'SELECT a,count(*) FROM t WHERE 0 GROUP BY a',
        'SELECT count(*) FROM t HAVING count(*)=0',
        'SELECT EXISTS(SELECT a FROM t WHERE 0 GROUP BY a)',
        'SELECT EXISTS(SELECT count(*) FROM t HAVING count(*)=0)',
        'SELECT EXISTS(SELECT a FROM t EXCEPT SELECT a FROM t)',
        'SELECT a,(SELECT sum(c)+1 FROM u WHERE c=t.a) FROM t ORDER BY a,b',
        'SELECT a,count(*) FROM t JOIN u ON a=c GROUP BY a ORDER BY a',
        'SELECT count(*) FROM t UNION SELECT count(*) FROM u ORDER BY 1',
        'SELECT 1 WHERE 0', 'SELECT count(*)', 'SELECT 2 ORDER BY 1 LIMIT 0',
    ]:
        check(sql)
    # Correlated minimum across comma-joined sources (TPC-H Q2 shape).
    # Include ties, NULL/absent keys and two captured outer columns.
    for sql in [
        'CREATE TABLE product(id INTEGER,region INTEGER)',
        'INSERT INTO product VALUES(1,10),(2,10),(3,20),(4,10),(NULL,10)',
        'CREATE TABLE offer(product_id INTEGER,supplier_id INTEGER,price INTEGER)',
        'INSERT INTO offer VALUES(1,1,5),(1,2,5),(1,3,1),(2,1,8),(2,2,7),(3,3,9)',
        'CREATE TABLE supplier(id INTEGER,region INTEGER)',
        'INSERT INTO supplier VALUES(1,10),(2,10),(3,20)',
        'CREATE TABLE seed(k INTEGER)', 'INSERT INTO seed VALUES(1)',
    ]:
        check(sql)
    for outer_id in ['p.id', 'id']:
        # Inner aliases expose no id column: unqualified id must resolve outward.
        inner = (f'SELECT min(x.price) FROM offer x,offer y '
                 f'WHERE x.product_id={outer_id} AND y.product_id=x.product_id '
                 'AND y.supplier_id=x.supplier_id')
        check(f'SELECT p.id,({inner}) FROM product p ORDER BY p.id,p.region')
    minimum = ('SELECT min(x.price) FROM offer x,supplier y '
               'WHERE x.product_id=p.id AND x.supplier_id=y.id AND y.region=p.region')
    check(f'SELECT p.id,({minimum}) FROM product p ORDER BY p.id,p.region')
    check(f'SELECT p.id,o.supplier_id,o.price FROM product p,offer o '
          f'WHERE p.id=o.product_id AND o.price=({minimum}) ORDER BY 1,2,3')
    check(f'SELECT p.id,EXISTS({minimum}),p.id IN '
          '(SELECT x.product_id FROM offer x,supplier y '
          'WHERE x.product_id=p.id AND x.supplier_id=y.id AND y.region=p.region) '
          'FROM product p ORDER BY p.id,p.region')
    check(f'SELECT p.id,(SELECT ({minimum}) FROM seed WHERE k=1) '
          'FROM product p ORDER BY p.id,p.region')
    # A local alias shadows the same outer alias; it is not an outer capture.
    check('SELECT p.id,(SELECT min(x.price) FROM offer x,supplier p '
          'WHERE x.supplier_id=p.id AND p.region=10) FROM product p ORDER BY p.id,p.region')
    for sql in [
        'CREATE TABLE textkey(k TEXT)', "INSERT INTO textkey VALUES('01'),('2'),('missing'),(NULL)",
        'SELECT k,(SELECT min(x.price) FROM offer x,supplier y '
        'WHERE textkey.k=x.product_id AND x.supplier_id=y.id) FROM textkey ORDER BY k',
        'SELECT p.id,(SELECT count(*) FROM offer x,supplier y '
        'WHERE p.id=1 AND x.supplier_id=y.id) FROM product p ORDER BY p.id,p.region',
    ]:
        check(sql)
    # NOT LIKE uses the existing LIKE conversion, NULL and precedence rules.
    for value in ["'alpha'", "'BETA'", 'NULL', '123']:
        for pattern in ["'a%'", "'%A'", "'12_'", 'NULL']:
            check(f'SELECT {value} NOT LIKE {pattern},NOT ({value} LIKE {pattern}),'
                  f'{value} NOT LIKE {pattern} AND 1,{value} NOT LIKE {pattern} OR 0')
    check("SELECT a FROM t WHERE a NOT LIKE '1%' ORDER BY a,b")
    check("SELECT a,c FROM t LEFT JOIN u ON a=c AND c NOT LIKE '1%' ORDER BY a,b,c")
    for sql in ['SELECT a,count(*) FROM t', 'SELECT sum(count(*)) FROM t',
                'SELECT a FROM t GROUP BY missing LIMIT 0',
                'SELECT a FROM t UNION SELECT a,b FROM t LIMIT 0',
                'SELECT 1 IN (SELECT a,b FROM t)',
                'SELECT a FROM t GROUP BY sum(a)']:
        assert core.execute(sql)[0] == 'error', sql
    for join in ['NATURAL JOIN']:
        assert core.execute(f'SELECT a,c FROM t {join} u ON a=c')[0] == 'unsupported'
    print(f'{count} relational queries/statements matched SQLite')
finally:
    core.close()
    sqlite.close()

"""Derived relations and ordinary CTEs against SQLite on populated inputs."""
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
                "INSERT INTO t VALUES(1,2),(1,3),(2,NULL),(NULL,4),(3,2)",
                "CREATE TABLE u(c INTEGER)", "INSERT INTO u VALUES(1),(1),(2),(NULL)"]:
        check(sql)
    for predicate in ['1', '0', 'a IS NULL', 'a>1', 'b=2', 'b IS NULL']:
        body = f'SELECT a,b FROM t WHERE {predicate}'
        for wrapper in [f'({body}) AS x', 'x']:
            prefix = f'WITH x AS ({body}) ' if wrapper == 'x' else ''
            for query in ['SELECT a,b FROM {source} ORDER BY a,b',
                          'SELECT a,count(*),sum(b) FROM {source} GROUP BY a ORDER BY a',
                          'SELECT a,c FROM {source} LEFT JOIN u ON a=c ORDER BY a,c',
                          'SELECT a,(SELECT count(*) FROM u WHERE c=x.a) FROM {source} ORDER BY a',
                          'SELECT a FROM {source} WHERE b IN (SELECT c FROM u) ORDER BY a']:
                check(prefix + query.format(source=wrapper))
    for sql in [
        'WITH x(v) AS (SELECT a FROM t),y AS (SELECT v FROM x WHERE v>1) SELECT v FROM y ORDER BY v',
        'WITH x AS (SELECT a FROM t) SELECT a FROM x UNION ALL SELECT a FROM x ORDER BY a',
        'SELECT v FROM (SELECT 1 AS v UNION ALL SELECT 2.5 AS v) AS x ORDER BY v',
        'SELECT a FROM (SELECT a FROM t) AS x UNION SELECT c FROM (SELECT c FROM u) AS y ORDER BY a',
        'WITH x AS (SELECT 1 AS a) SELECT a,(WITH x AS (SELECT 2 AS a) SELECT a FROM x) FROM x',
        'SELECT sum(n) FROM (SELECT a,count(*) AS n FROM t GROUP BY a HAVING count(*)>1) AS x',
        'WITH x AS (SELECT a AS v FROM t) SELECT a.v,b.v FROM x AS a,x AS b WHERE a.v=b.v ORDER BY a.v,b.v',
        'SELECT * FROM (SELECT a,b FROM t ORDER BY a,b LIMIT 2) AS x ORDER BY a,b',
    ]: check(sql)
    print(f'{count} derived/CTE statements and queries matched SQLite')
finally:
    core.close()

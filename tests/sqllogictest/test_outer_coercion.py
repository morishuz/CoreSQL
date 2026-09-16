"""SQLite differential coverage for outer joins, DISTINCT aggregates and coercion."""
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
    for sql in ['CREATE TABLE l(a INTEGER,b TEXT)', 'CREATE TABLE r(c INTEGER,d TEXT)',
                "INSERT INTO l VALUES(1,'one'),(2,'two'),(2,'again'),(NULL,'null')",
                "INSERT INTO r VALUES(2,'match'),(2,'duplicate'),(3,'three'),(NULL,'null')",
                'CREATE TABLE empty(e INTEGER)']:
        check(sql)
    for kind in ['LEFT', 'LEFT OUTER', 'RIGHT', 'RIGHT OUTER', 'FULL', 'FULL OUTER']:
        for on in ['a=c', 'a<c', 'a=c AND d=\'match\'', '0', '1', 'NULL']:
            for where in ['', 'WHERE a IS NULL', 'WHERE c IS NULL', 'WHERE c=2']:
                check(f'SELECT a,b,c,d FROM l {kind} JOIN r ON {on} {where} ORDER BY a,b,c,d')
        check(f'SELECT a,e FROM l {kind} JOIN empty ON a=e ORDER BY a,e')
        check(f'SELECT e,c FROM empty {kind} JOIN r ON e=c ORDER BY e,c')
        check(f'SELECT a,count(c) FROM l {kind} JOIN r ON a=c GROUP BY a ORDER BY a')
    for sql in [
        'SELECT a,c,e FROM l LEFT JOIN r ON a=c LEFT JOIN empty ON c=e ORDER BY a,b,c,d',
        'SELECT a,c,e FROM l LEFT JOIN r ON a=c RIGHT JOIN empty ON c=e',
        'SELECT a,c,e FROM l FULL JOIN r ON a=c FULL JOIN empty ON c=e ORDER BY a,b,c,d',
        'SELECT a,(SELECT count(DISTINCT c) FROM r WHERE c=l.a) FROM l ORDER BY a,b',
        'SELECT count(DISTINCT a),sum(DISTINCT a),sum(a),avg(DISTINCT a),min(DISTINCT a),max(DISTINCT a) FROM l',
        'SELECT a,count(DISTINCT c),sum(DISTINCT c) FROM l FULL JOIN r ON a=c GROUP BY a ORDER BY a',
        'SELECT count(DISTINCT e),sum(DISTINCT e) FROM empty',
        'SELECT count(DISTINCT a)+1 FROM l HAVING count(DISTINCT a)>1',
        'SELECT count(DISTINCT a),count(a) FROM l LIMIT 0',
        'SELECT coalesce(NULL,1,2.5),CASE WHEN 1 THEN 7 ELSE 2.5 END',
        "SELECT coalesce(NULL,'text',1),CASE WHEN 0 THEN 1 ELSE 'text' END",
        "SELECT 1 UNION SELECT '1' UNION SELECT 1.0 ORDER BY 1",
        "SELECT NULL UNION SELECT 'text' ORDER BY 1",
        'SELECT 1 INTERSECT SELECT 1.0',
        "SELECT '2.5'+1,2+0.5,5/2.0,5/2,'no number'+2,2||3.5",
        "SELECT NOT '0.0',NOT '1',0.5 AND 1,'abc' OR '2'",
        "SELECT CASE WHEN '0.1' THEN 1 ELSE 2 END",
        "SELECT a FROM l WHERE a='2' ORDER BY a,b",
        "SELECT a FROM l WHERE a='invalid' ORDER BY a,b",
        "SELECT b FROM l WHERE b=2 ORDER BY b",
    ]:
        check(sql)
    for sql in [
        "SELECT a FROM l WHERE a BETWEEN '1' AND '2' ORDER BY a,b",
        "SELECT a FROM l WHERE a IN ('1','2') ORDER BY a,b",
        "SELECT CAST(5.0 AS NUMERIC)/2,CAST('5.0' AS NUMERIC)/2",
        "SELECT CAST('+-1' AS REAL),CAST('++1' AS INTEGER)",
        "CREATE TABLE dynamic(v)",
        "INSERT INTO dynamic VALUES(1),(1.0),('1'),('2.5'),('bad'),(NULL)",
        "SELECT count(DISTINCT v),sum(DISTINCT v),avg(DISTINCT v),sum(v) FROM dynamic",
        "SELECT v+1 FROM dynamic ORDER BY v",
        "SELECT sum(v),avg(v),group_concat(v) FROM dynamic",
        "SELECT sum(v)/2 FROM dynamic WHERE v=1",
        "SELECT abs('5')/2,length(123),123 LIKE '12%'",
    ]:
        check(sql)
    for sql in [
        "CREATE TABLE affinities(t TEXT,n INTEGER)", "INSERT INTO affinities VALUES('01',1),('2',2),('bad',3)",
        "SELECT t=n,n=t FROM affinities ORDER BY n",
        "SELECT 1 IN (SELECT t FROM affinities),1 IN (SELECT n FROM affinities)",
        "SELECT CASE n WHEN '1' THEN 1 ELSE 0 END FROM affinities ORDER BY n",
        "SELECT CAST(1 AS INTEGER)='1',CAST(1 AS TEXT)=1",
        "SELECT CAST(1e20 AS TEXT),CAST(-0.0 AS TEXT),CAST(1e-5 AS TEXT)",
    ]:
        check(sql)
    for value in ["NULL", "'123'", "'  -12.75tail'", "'12e2'", "'abc'", "2.5", "-2.5", "'9223372036854775808'", "'-9223372036854775809'", "1"]:
        for target in ['INTEGER', 'REAL', 'TEXT', 'NUMERIC']:
            check(f'SELECT CAST({value} AS {target})')
    for sql in ['CREATE TABLE writes(i INTEGER,x REAL,s TEXT)',
                "INSERT INTO writes VALUES('12',3,4)",
                "UPDATE writes SET i='14.0',x='2.5',s=7.5",
                'SELECT * FROM writes',
                "INSERT INTO writes SELECT '15',16,'text'", 'SELECT * FROM writes ORDER BY i']:
        check(sql)
    before=core.execute('SELECT * FROM writes ORDER BY i')
    for sql in ["INSERT INTO writes VALUES('bad',1,'x')", "UPDATE writes SET i=1.5", "INSERT INTO writes VALUES(20,1,'x'),('bad',2,'y')"]:
        assert core.execute(sql)[0]=='error', sql
        assert core.execute('SELECT * FROM writes ORDER BY i')==before
    for sql in ['SELECT count(DISTINCT *) FROM l', 'SELECT sum(DISTINCT a,b) FROM l',
                'SELECT abs(DISTINCT a) FROM l',
                'SELECT a FROM l LEFT JOIN r ON missing=c LIMIT 0',
                'SELECT a FROM l NATURAL JOIN r', 'SELECT a FROM l JOIN r USING(a)']:
        assert core.execute(sql)[0] in ('error','unsupported'), sql
    print(f'{count} outer/distinct/coercion statements matched SQLite')
finally:
    core.close();sqlite.close()

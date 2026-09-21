"""Original deterministic, reduced fixtures for the attributed SQL adaptations.

All engines receive identical CREATE/INSERT/index statements. Inputs use ASCII,
integers and exactly representable dyadic REAL values; no engine-side randomness.
"""


class Random:
    def __init__(self):
        self.state = 42

    def next(self, bound):
        self.state = (1664525 * self.state + 1013904223) & 0xffffffff
        return (self.state >> 8) % bound


def tables(kind, n):
    rng = Random()
    if kind in ('sequence', 'mod5', 'group5', 'distinct25', 'sort', 'topn'):
        columns = 'i INTEGER,j INTEGER' if kind in ('group5', 'distinct25', 'sort') else 'i INTEGER'
        def row(i):
            if kind == 'group5': return (i % 5, i % 100)
            if kind == 'distinct25': return (i % 5, i % 25)
            if kind == 'sort': return ((i * 9582398353) % 100, (i * 847892347987) % 100)
            return (i % 5 if kind == 'mod5' else i,)
        yield 'integers', columns, (row(i) for i in range(n))
        if kind == 'topn':
            yield 'other_table', 'i INTEGER', [(i,) for i in sorted({n//100, n//10, n//2, n-2, n-1})]
    elif kind == 'arithmetic_join':
        for name, count in [('t1', max(100, n//100)), ('t2', n)]:
            yield name, 'v1 INTEGER,v2 INTEGER', ((i, i) for i in range(count))
    elif kind == 'duplicate_join':
        keys = max(10, n//16)
        for name, count in [('t1', keys*4), ('t2', keys*16)]:
            yield name, 'i TEXT', (('verylargestring'+str(i % keys),) for i in range(count))
    elif kind == 'h2o_groupby':
        high = max(10, n//100)
        def row():
            keys = [rng.next(k) for k in (100, 100, high, 100, 100, high)]
            return tuple('id'+str(k) for k in keys[:3]) + tuple(keys[3:]) + (rng.next(5)+1, rng.next(15)+1, rng.next(10000)/8.0)
        yield 'x', 'id1 TEXT,id2 TEXT,id3 TEXT,id4 INTEGER,id5 INTEGER,id6 INTEGER,v1 INTEGER,v2 INTEGER,v3 REAL', (row() for _ in range(n))
    elif kind == 'h2o_join':
        small, medium = max(10, n//100), max(20, n//10)
        def row(i):
            a, b, c = rng.next(small), rng.next(medium), rng.next(n)
            return (a, b, c, 'id'+str(a), 'id'+str(b), 'id'+str(c), i/8.0)
        yield 'x', 'id1 INTEGER,id2 INTEGER,id3 INTEGER,id4 TEXT,id5 TEXT,id6 TEXT,v1 REAL', (row(i) for i in range(n))
        yield 'small', 'id1 INTEGER,id4 TEXT,v2 REAL', ((i, 'id'+str(i), i/8.0) for i in range(small))
        # 90% matched domain makes LEFT JOIN exercise missing matches as well.
        yield 'medium', 'id1 INTEGER,id2 INTEGER,id4 TEXT,id5 TEXT,v2 REAL', ((i % small, i, 'id'+str(i % small), 'id'+str(i), i/8.0) for i in range(medium*9//10))
        yield 'big', 'id1 INTEGER,id2 INTEGER,id3 INTEGER,id4 TEXT,id5 TEXT,id6 TEXT,v2 REAL', ((i % small, i % medium, i, 'id'+str(i % small), 'id'+str(i % medium), 'id'+str(i), i/8.0) for i in range(n))
    elif kind == 'star':
        def signed(bound):
            value = rng.next(bound)
            return value if rng.next(2) else -value
        def fact(i):
            attrs = [signed(11+j) for j in range(1,9)]
            # Reduced random stars otherwise often have no complete match.
            # Keep negative/unmatched rows, but guarantee fanout and data-9% hits.
            if i % 10 in (0, 9):
                attrs = [i % (2*(j+1)) for j in range(1,9)]
            return tuple(attrs[:3])+('data-'+str(i),)+tuple(attrs[3:])+(i, 'value-'+str(i))
        yield 'facttab', 'attr01 INTEGER,attr02 INTEGER,attr03 INTEGER,data01 TEXT,attr04 INTEGER,attr05 INTEGER,attr06 INTEGER,attr07 INTEGER,attr08 INTEGER,factid INTEGER PRIMARY KEY,data02 TEXT', (fact(i) for i in range(1, max(50,n//20)+1))
        for j in range(1,9):
            yield f'dimension{j:02}', f'beta{j:02} INTEGER,content{j:02} TEXT,rate{j:02} REAL', ((i % (2*(j+1)), f'content-{j:02}-{i}', signed(10000)/8.0) for i in range(1,4*(j+1)+1))
    elif kind in ('fp', 'fp_index'):
        yield 'z1', 'a REAL,b REAL', (((rng.next(16001)-8000)/8.0, (rng.next(16001)-8000)/8.0) for _ in range(n))
    elif kind == 'wide_join':
        for name in ('l', 'r'):
            columns = 'k INTEGER,payload TEXT' if name == 'l' else 'k INTEGER,v INTEGER'
            yield name, columns, ((i % (n//4), 'x'*256 if name == 'l' else i) for i in range(n))
    elif kind == 'nullable':
        yield 'nullable', 'k INTEGER', ((None if i % 7 == 0 else i % 3,) for i in range(n))
    else:
        raise ValueError('Unknown fixture: '+kind)


def literal(value):
    if value is None: return 'NULL'
    if isinstance(value, str): return "'"+value.replace("'", "''")+"'"
    return repr(value)


def statements(kind, n):
    for name, columns, rows in tables(kind, n):
        yield f'CREATE TABLE {name}({columns})'
        batch = []
        for row in rows:
            batch.append('('+','.join(literal(v) for v in row)+')')
            if len(batch) == 100:
                yield f'INSERT INTO {name} VALUES'+','.join(batch)
                batch = []
        if batch: yield f'INSERT INTO {name} VALUES'+','.join(batch)
    if kind == 'star':
        for i in range(1,9):
            yield f'CREATE INDEX fact_attr{i:02} ON facttab(attr{i:02})'
            cols = f'beta{i:02}' + ('' if i & 2 else f',content{i:02}')
            yield f'CREATE INDEX dim{i:02} ON dimension{i:02}({cols})'
    if kind == 'fp_index':
        for suffix, cols in [('a','a'), ('b','b'), ('ab','a,b')]:
            yield f'CREATE INDEX t1{suffix} ON z1({cols})'

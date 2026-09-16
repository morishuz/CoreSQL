"""Pinned SQLite C API driver and explicit adaptations of this TPC-H query pin.

SQLite uses ISO TEXT dates and approximate REAL instead of exact DECIMAL.
This is a different numeric profile; it is never claimed as exact type parity.
"""
import ctypes as c
from decimal import Decimal
import json
import math
from pathlib import Path
import re


def adapt(sql):
    sql = re.sub(r"CAST\(('\d{4}-\d{2}-\d{2}') AS date\)", r'\1', sql, flags=re.I)
    sql = re.sub(r"\bdate ('\d{4}-\d{2}-\d{2}')", r'\1', sql, flags=re.I)
    sql = re.sub(r'extract\(year FROM (\w+)\)', r"CAST(strftime('%Y', \1) AS INTEGER)", sql, flags=re.I)
    sql = re.sub(r'substring\((\w+) FROM (\d+) FOR (\d+)\)', r'substr(\1, \2, \3)', sql, flags=re.I)
    # Q13: SQLite requires output aliases inside this derived SELECT.
    if re.search(r'AS c_orders \(c_custkey,\s*c_count\)', sql):
        sql = sql.replace('count(o_orderkey)', 'count(o_orderkey) AS c_count')
        sql = re.sub(r'AS c_orders \(c_custkey,\s*c_count\)', 'AS c_orders', sql)
    if sql.startswith('CREATE TABLE'):
        sql = sql.replace('DATE', 'TEXT').replace('DECIMAL(15,2)', 'REAL')
    return sql


def compare_approximate(actual, expected):
    assert len(actual) == len(expected), 'Row counts differ'
    for i, (a, b) in enumerate(zip(actual, expected)):
        assert len(a) == len(b), 'Projection widths differ'
        for j, (v, w) in enumerate(zip(a, b)):
            if isinstance(w, (Decimal, float)):
                match = isinstance(v, (float, int)) and math.isclose(v, float(w), rel_tol=1e-10, abs_tol=1e-10)
            elif hasattr(w, 'isoformat'):
                match = v == w.isoformat()
            else:
                match = type(v) is type(w) and v == w
            assert match, f'Cell [{i},{j}] differs: {v!r} vs {w!r}'


class SQLite:
    def __init__(self, path):
        self.lib = c.CDLL(str(Path(path).resolve()))
        signatures = {
            'open': ([c.c_char_p, c.POINTER(c.c_void_p)], c.c_int),
            'close': ([c.c_void_p], c.c_int),
            'errmsg': ([c.c_void_p], c.c_char_p),
            'prepare_v2': ([c.c_void_p, c.c_char_p, c.c_int, c.POINTER(c.c_void_p), c.POINTER(c.c_char_p)], c.c_int),
            'step': ([c.c_void_p], c.c_int), 'finalize': ([c.c_void_p], c.c_int),
            'column_count': ([c.c_void_p], c.c_int),
            'column_type': ([c.c_void_p, c.c_int], c.c_int),
            'column_int64': ([c.c_void_p, c.c_int], c.c_int64),
            'column_double': ([c.c_void_p, c.c_int], c.c_double),
            'column_text': ([c.c_void_p, c.c_int], c.c_void_p),
            'column_bytes': ([c.c_void_p, c.c_int], c.c_int),
            'sourceid': ([], c.c_char_p), 'libversion': ([], c.c_char_p),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.lib, 'sqlite3_' + name)
            fn.argtypes, fn.restype = args, result
            setattr(self, name if name not in ('close',) else '_close', fn)
        pin = json.loads((Path(__file__).parents[1] / 'reference/sqlite.json').read_text())
        self.source_id = self.sourceid().decode()
        if pin['fossil_checkin'] not in self.source_id:
            raise RuntimeError('SQLite library differs from the repository reference pin')
        self.db = c.c_void_p()
        if self.open(b':memory:', c.byref(self.db)):
            raise RuntimeError('Cannot open SQLite')
        self.execute('PRAGMA temp_store=MEMORY')
        self.execute('PRAGMA threads=0')
        self.execute('PRAGMA case_sensitive_like=ON')
        # Normal optimizer defaults, including automatic transient indexes, remain on.

    def execute(self, sql):
        statement, tail = c.c_void_p(), c.c_char_p()
        payload = sql.encode()
        rc = self.prepare_v2(self.db, payload, len(payload), c.byref(statement), c.byref(tail))
        if rc:
            raise RuntimeError(self.errmsg(self.db).decode())
        try:
            if tail.value and tail.value.strip():
                raise RuntimeError('Only one SQL statement is accepted')
            rows = []
            while True:
                rc = self.step(statement)
                if rc == 101:
                    return rows
                if rc != 100:
                    raise RuntimeError(self.errmsg(self.db).decode())
                row = []
                for i in range(self.column_count(statement)):
                    kind = self.column_type(statement, i)
                    if kind == 5:
                        value = None
                    elif kind == 1:
                        value = self.column_int64(statement, i)
                    elif kind == 2:
                        value = self.column_double(statement, i)
                    elif kind == 3:
                        value = c.string_at(self.column_text(statement, i), self.column_bytes(statement, i)).decode()
                    else:
                        raise RuntimeError('Unexpected SQLite BLOB result')
                    row.append(value)
                rows.append(row)
        finally:
            self.finalize(statement)

    def close(self):
        if self._close(self.db):
            raise RuntimeError('SQLite close failed')

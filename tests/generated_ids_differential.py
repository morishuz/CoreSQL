"""Compare supported generated-ID/RETURNING behavior with Python's SQLite."""
import pathlib
import sqlite3
import subprocess
import sys
import tempfile

statements = [
    'CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT)',
    "INSERT INTO notes(body) VALUES('first') RETURNING id, body",
    "INSERT INTO notes VALUES(NULL, 'second') RETURNING *",
    "INSERT INTO notes VALUES(100, 'explicit'), (NULL, 'generated') RETURNING id",
    'BEGIN',
    "INSERT INTO notes(body) VALUES('rollback') RETURNING id",
    'ROLLBACK',
    'SAVEPOINT s',
    "INSERT INTO notes(body) VALUES('savepoint') RETURNING id",
    'ROLLBACK TO s',
    "INSERT INTO notes(body) VALUES('kept') RETURNING id",
    'RELEASE s',
    'DELETE FROM notes WHERE id=102',
    "INSERT INTO notes(body) VALUES('reused') RETURNING id",
    "INSERT INTO notes(body) SELECT body FROM notes WHERE id<3 RETURNING id, body",
    'INSERT INTO notes DEFAULT VALUES RETURNING *',
    'CREATE TABLE replacement(id INTEGER PRIMARY KEY, name TEXT UNIQUE)',
    "INSERT INTO replacement VALUES(50, 'a'), (51, 'b')",
    "REPLACE INTO replacement VALUES(NULL, 'b'), (2, 'b'), (NULL, 'c') RETURNING *",
    'SELECT * FROM replacement ORDER BY id',
]
for i in range(30):
    statements.append(f"INSERT INTO notes(body) VALUES('batch{i}a'), ('batch{i}b') RETURNING id AS key, body")
statements.append('SELECT * FROM notes ORDER BY id')
reference = sqlite3.connect(':memory:', isolation_level=None)
expected = []
for sql in statements:
    for row in reference.execute(sql):
        expected.append('\t'.join('NULL' if value is None else str(value) for value in row))
with tempfile.TemporaryDirectory(prefix='coresql-ids-') as temp:
    script = pathlib.Path(temp) / 'test.sql'
    script.write_text(';\n'.join(statements) + ';\n')
    output = subprocess.run([sys.argv[1], '--database', str(pathlib.Path(temp) / 'db'), str(script)],
                            text=True, capture_output=True, check=True)
    actual = output.stdout.splitlines()
    if actual != expected:
        raise AssertionError(f'Generated-ID results differ:\nexpected={expected}\nactual={actual}')
print(f'{len(statements)} statements matched SQLite {sqlite3.sqlite_version}')

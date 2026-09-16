"""Generate a hashed, typed TPC-H dataset with a pinned DuckDB reference.

Requires duckdb==1.5.5. No dependency is installed automatically. Output is new,
never overwritten. The generated database is only an intermediate; CSV files and
manifest hashes are the portable input shared by the validation engines.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import duckdb
import _duckdb

VERSION = '1.5.5'
REVISION = 'd8cdaa33fd'
TABLES = ('region', 'nation', 'supplier', 'customer', 'part', 'partsupp', 'orders', 'lineitem')


def reference():
    if duckdb.__version__ != VERSION:
        raise RuntimeError(f'Requires duckdb=={VERSION}, found {duckdb.__version__}')
    db = duckdb.connect(':memory:', config={'threads': 1})
    if db.execute('PRAGMA version').fetchone()[1] != REVISION:
        raise RuntimeError('DuckDB build revision differs from the generator pin')
    return db


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scale', choices=['0.001', '0.01', '0.03', '0.1'], default='0.001')
    parser.add_argument('--fetch-extension', action='store_true', help='Allow installing the signed tpch extension')
    args = parser.parse_args()
    if args.output.exists():
        raise RuntimeError('Output already exists; choose a new directory')
    db = reference()
    db.execute("SET memory_limit='512MB'")
    if args.fetch_extension:
        db.execute('INSTALL tpch')
    db.execute('LOAD tpch')
    extension = db.execute("SELECT extension_version FROM duckdb_extensions() WHERE extension_name='tpch'").fetchone()[0]
    if extension != 'v' + VERSION:
        raise RuntimeError('TPC-H extension version differs from the generator pin')
    db.execute(f'CALL dbgen(sf={args.scale})')
    args.output.mkdir(parents=True)
    manifest = {'format': 1, 'duckdb_version': VERSION, 'duckdb_revision': REVISION,
                'tpch_extension_version': extension, 'scale': args.scale, 'threads': 1,
                'duckdb_binary_sha256': hashlib.sha256(Path(_duckdb.__file__).read_bytes()).hexdigest(),
                'tpch_extension_sha256': hashlib.sha256(Path(db.execute("SELECT install_path FROM duckdb_extensions() WHERE extension_name='tpch'").fetchone()[0]).read_bytes()).hexdigest(),
                'purpose': 'Generated-data correctness input; not a performance or official TPC-H result', 'tables': []}
    for table in TABLES:
        columns = [(row[1], row[2]) for row in db.execute(f"PRAGMA table_info('{table}')").fetchall()]
        # Complete-row ordering gives reproducible files independently of scan order.
        cursor = db.execute(f'SELECT * FROM {table} ORDER BY ALL')
        count = 0
        path = args.output / f'{table}.csv'
        with path.open('w', newline='', encoding='utf-8') as output:
            writer = csv.writer(output, lineterminator='\n')
            while rows := cursor.fetchmany(4096):
                for row in rows:
                    if any(value is None for value in row):
                        raise RuntimeError('Generator unexpectedly produced NULL input')
                    writer.writerow(row)
                count += len(rows)
        manifest['tables'].append({'name': table, 'columns': columns, 'rows': count,
                                   'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        print(f'{table}: {count} rows')
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    db.close()


if __name__ == '__main__':
    main()

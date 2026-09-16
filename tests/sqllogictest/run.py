"""Bounded SQLLogicTest validation; no SQL rewriting or inferred feature skips."""
import argparse
import collections
from decimal import Decimal
import hashlib
import json
import pathlib
import re
import sqlite3
import subprocess


def records(text):
    block, start = [], 0
    for line, value in enumerate(text.splitlines() + [''], 1):
        if value.startswith('#'):
            continue
        if not value.strip():
            if block:
                yield start, block
                block = []
        else:
            if not block:
                start = line
            block.append(value)


class Core:
    name = 'coresql'

    def __init__(self, executable):
        self.process = subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def execute(self, sql):
        payload = sql.encode()
        self.process.stdin.write(str(len(payload)).encode() + b'\n' + payload)
        self.process.stdin.flush()
        header = self.process.stdout.readline().decode().rstrip('\n').split(' ', 1)
        if len(header) != 2:
            raise RuntimeError('CoreSQL bridge terminated or returned malformed output')
        status, detail = header
        if status != 'ok':
            if status not in ('error', 'unsupported'):
                raise RuntimeError('Invalid bridge status')
            return status, bytes.fromhex(detail).decode()
        rows = []
        for _ in range(int(detail)):
            line = self.process.stdout.readline()
            if not line.endswith(b'\n'):
                raise RuntimeError('Truncated bridge result')
            row = []
            for cell in line.decode().rstrip('\n').split('\t'):
                tag, value = cell[0], cell[1:]
                row.append(None if tag == 'N' else int(value) if tag == 'I' else
                           float(value) if tag == 'R' else Decimal(value) if tag == 'D' else bytes.fromhex(value).decode('latin1'))
            rows.append(row)
        return 'ok', rows

    def close(self):
        self.process.stdin.close()
        code = self.process.wait(timeout=10)
        self.process.stdout.close()
        if code:
            raise RuntimeError(f'CoreSQL bridge exited with {code}')


class SQLite:
    name = 'sqlite'

    def __init__(self):
        self.db = sqlite3.connect(':memory:', isolation_level=None)

    def execute(self, sql):
        try:
            return 'ok', self.db.execute(sql).fetchall()
        except sqlite3.Error as error:
            return 'error', str(error)

    def close(self):
        self.db.close()


def render(value, kind):
    if value is None:
        return 'NULL'
    if kind == 'I':
        return str(int(value))
    if kind == 'R':
        return format(float(value), '.3f')
    value = str(value)
    return ''.join(c if ' ' <= c <= '~' else '@' for c in value) or '(empty)'


def normalize(rows, types, sort):
    if any(len(row) != len(types) for row in rows):
        raise ValueError('Result column count differs')
    rendered = [[render(v, t) for v, t in zip(row, types)] for row in rows]
    if sort == 'rowsort':
        rendered.sort()
    values = [v for row in rendered for v in row]
    if sort == 'valuesort':
        values.sort()
    return values


def digest(values):
    return hashlib.md5(''.join(v + '\n' for v in values).encode(), usedforsecurity=False).hexdigest()


def validate(text, engine):
    results, labels = [], {}
    blocked = False
    for line, block in records(text):
        skip = False
        while block and block[0].split()[0] in ('skipif', 'onlyif'):
            condition, name = block.pop(0).split()
            skip |= (condition == 'skipif' and name == engine.name or
                     condition == 'onlyif' and name != engine.name)
        if not block:
            raise ValueError(f'{line}: missing conditional body')
        head = block[0].split()
        if head[0] == 'halt':
            raise ValueError('halt is intentionally rejected: use an explicit subset')
        if head[0] == 'hash-threshold':
            if len(head) != 2 or int(head[1]) < 0:
                raise ValueError('Invalid hash-threshold')
            continue  # Validation accepts both explicit and hashed expectations.
        if head[0] not in ('statement', 'query'):
            raise ValueError(f'{line}: unknown record {head[0]}')
        query = head[0] == 'query'
        separator = block.index('----') if '----' in block else len(block)
        sql = '\n'.join(block[1:separator])
        expected = block[separator + 1:]
        if query:
            types = head[1]
            sort = head[2] if len(head) > 2 else 'nosort'
            label = head[3] if len(head) > 3 else None
            if not types or set(types) - set('IRT') or sort not in ('nosort', 'rowsort', 'valuesort') or len(head) > 4:
                raise ValueError(f'{line}: invalid query header')
            hashed = re.fullmatch(r'(\d+) values hashing to ([0-9a-f]{32})', expected[0]) if len(expected) == 1 else None
            expected_hash = hashed[2] if hashed else digest(expected)
            if label:
                if label in labels and labels[label] != expected_hash:
                    raise ValueError(f'{line}: conflicting expected label {label}')
                labels[label] = expected_hash
        elif head not in (['statement', 'ok'], ['statement', 'error']):
            raise ValueError(f'{line}: invalid statement header')
        detail = ''
        if skip:
            outcome = 'skipped'
        elif blocked:
            outcome = 'blocked'
            detail = 'Earlier statement failed or was unsupported; database state is incomplete'
        else:
            status, actual = engine.execute(sql)
            if status == 'unsupported':
                outcome, detail = 'unsupported', actual
            elif not query:
                outcome = 'passed' if (status == 'ok') == (head[1] == 'ok') else 'failed'
                detail = actual if status == 'error' else ''
            elif status != 'ok':
                outcome, detail = 'failed', actual
            else:
                try:
                    values = normalize(actual, types, sort)
                    matches = (len(values) == int(hashed[1]) and digest(values) == expected_hash) if hashed else values == expected
                    outcome = 'passed' if matches else 'failed'
                    if not matches:
                        detail = f'{len(values)} values hashing to {digest(values)}; expected ' + repr(expected[:8])
                except (ValueError, TypeError, OverflowError) as error:
                    outcome, detail = 'failed', str(error)
            if not query and outcome != 'passed':
                blocked = True
        results.append(dict(line=line, kind=head[0], outcome=outcome, detail=detail))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', required=True)
    parser.add_argument('--file', action='append', help='Run one pinned file; repeatable')
    parser.add_argument('--report', type=pathlib.Path)
    parser.add_argument('--baseline', type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).parent
    manifest = json.loads((root / 'manifest.json').read_text())
    if args.file and set(args.file) - set(manifest['files']):
        parser.error('Requested file is not in the manifest')
    report = {'sqlite_version': sqlite3.sqlite_version, 'files': {}}
    failures = []
    for filename, checksum in manifest['files'].items():
        data = (root / filename).read_bytes()
        if hashlib.sha256(data).hexdigest() != checksum:
            raise ValueError(f'Pinned fixture checksum mismatch: {filename}')
        if not filename.endswith('.test') or (args.file and filename not in args.file):
            continue
        per_engine = {}
        for engine in (SQLite(), Core(args.bridge)):
            try:
                results = validate(data.decode(), engine)
            finally:
                engine.close()
            counts = dict(collections.Counter(r['outcome'] for r in results))
            per_engine[engine.name] = dict(counts=counts, records=results)
            print(filename, engine.name, json.dumps(counts, sort_keys=True), flush=True)
            if engine.name == 'sqlite' and any(r['outcome'] not in ('passed', 'skipped') for r in results):
                failures.append('SQLite reference did not validate the fixture')
        report['files'][filename] = per_engine
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    if args.baseline:
        baseline = json.loads(args.baseline.read_text())
        # Compare each record, not just totals: a new failure cannot hide behind a new pass.
        for filename, engines in report['files'].items():
            actual = {outcome: [r['line'] for r in engines['coresql']['records'] if r['outcome'] == outcome]
                      for outcome in sorted(engines['coresql']['counts'])}
            expected = baseline[filename]
            if actual != expected:
                failures.append(f'{filename}: outcomes changed; review and update the baseline explicitly')
    if any(r['outcome'] in ('failed', 'blocked') for engines in report['files'].values() for r in engines['coresql']['records']):
        failures.append('CoreSQL has failures; inspect the report')
    if failures:
        raise SystemExit('\n'.join(failures))


if __name__ == '__main__':
    main()

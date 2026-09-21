"""Harness checks; run using the pinned DuckDB Python environment."""
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest

import bench
from fixtures import statements, tables


class SuiteChecks(unittest.TestCase):
    def test_fixture_coverage_and_determinism(self):
        fixtures = {c['fixture'] for c in bench.catalog()}
        for fixture in fixtures:
            self.assertEqual(list(statements(fixture,100)), list(statements(fixture,100)))
        for n in (100,10000):
            data = {name:list(rows) for name, _, rows in tables('star',n)}
            keys = [{r[0] for r in data[f'dimension{i:02}']} for i in range(1,9)]
            hits = [row for row in data['facttab'] if all(v in domain for v,domain in zip(row[:3]+row[4:9],keys))]
            self.assertGreater(len(hits),0)
            self.assertLess(len(hits),len(data['facttab']))
            self.assertTrue(any(row[3].startswith('data-9') for row in hits))

    def test_multiset_validation_preserves_duplicates(self):
        bench.compare(bench.canonical([[None,1],['a',2],['a',2]]), bench.canonical([['a',2],[None,1],['a',2]]))
        with self.assertRaises(AssertionError):
            bench.compare(bench.canonical([['a',2],['a',2]]), bench.canonical([['a',2],['b',2]]))
        with self.assertRaises(AssertionError):
            bench.compare([[1]],[[1],[1]])

    def test_join_adaptations_preserve_reference_answers(self):
        cases = {c['id']:c for c in bench.catalog()}
        for case in cases.values():
            if not case['id'].endswith('/on'): continue
            db = bench.reference()
            try:
                for sql in statements(case['fixture'],100): db.execute(sql)
                original = db.execute(cases[case['id'][:-3]]['sql']).fetchall()
                adapted = db.execute(case['sql']).fetchall()
                bench.compare(bench.canonical(adapted),bench.canonical(original))
                self.assertGreater(len(adapted),0)
            finally: db.close()

    def test_source_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pin = next(iter(json.loads((bench.HERE/'sources.json').read_text()).values()))
            path = root/pin['repository']/pin['revision']/pin['path']
            path.parent.mkdir(parents=True)
            path.write_text('tampered')
            with self.assertRaisesRegex(ValueError,'checksum'):
                bench.verify_sources(root,False)

    def test_timeout_and_process_errors_are_not_success(self):
        start = time.monotonic()
        status, _, _ = bench.bounded([sys.executable,'-c','import time; time.sleep(30)'],0.1)
        self.assertEqual(status,'timeout')
        self.assertLess(time.monotonic()-start,5)
        status, _, _ = bench.bounded([sys.executable,'-c','raise RuntimeError("test failure")'],5)
        self.assertEqual(status,'process_error')

    def test_reused_tpch_requires_this_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary, report = root/'bridge', root/'tpch.json'
            binary.write_bytes(b'new executable')
            report.write_text(json.dumps({'bridge_sha256':'old executable digest'}))
            with self.assertRaisesRegex(ValueError,'executable'):
                bench.tpch_summary(report,binary,reused=True)


if __name__ == '__main__':
    unittest.main()

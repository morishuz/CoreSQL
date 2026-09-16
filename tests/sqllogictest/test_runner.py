import unittest
import sys
from run import Core, digest, normalize, records, validate


BRIDGE = sys.argv.pop(1) if len(sys.argv) > 1 else None


class Fake:
    name = 'coresql'

    def __init__(self, *responses):
        self.responses = iter(responses)

    def execute(self, sql):
        return next(self.responses)


class RunnerTests(unittest.TestCase):
    @unittest.skipUnless(BRIDGE, 'bridge executable not supplied')
    def test_bridge_roundtrip_and_recovery(self):
        engine = Core(BRIDGE)
        try:
            self.assertEqual(engine.execute("SELECT 3,-1.25,NULL,'a\n\tb'"),
                             ('ok', [[3, -1.25, None, 'a\n\tb']]))
            self.assertEqual(engine.execute('WITH RECURSIVE x AS (SELECT 1) SELECT * FROM x')[0], 'unsupported')
            self.assertEqual(engine.execute('SELECT abs(-9223372036854775808)')[0], 'error')
            self.assertEqual(engine.execute('SELECT 7'), ('ok', [[7]]))
        finally:
            engine.close()

    def test_render_sort_and_hash(self):
        self.assertEqual(normalize([[9, ''], [10, 'a\nb']], 'IT', 'rowsort'), ['10', 'a@b', '9', '(empty)'])
        self.assertEqual(normalize([[9, 10], [2, None]], 'II', 'valuesort'), ['10', '2', '9', 'NULL'])
        self.assertEqual(normalize([[1.23456]], 'R', 'nosort'), ['1.235'])
        # Known MD5 for the byte stream "1\n".
        self.assertEqual(digest(['1']), 'b026324c6904b2a9cb4b88d6d61c81d1')
        with self.assertRaises(ValueError):
            normalize([[1, 2]], 'I', 'nosort')

    def test_hash_count_and_mismatch(self):
        script = 'query I\nSELECT 1\n----\n1 values hashing to ' + digest(['1'])
        self.assertEqual(validate(script, Fake(('ok', [[1]])))[0]['outcome'], 'passed')
        self.assertEqual(validate(script, Fake(('ok', [[2]])))[0]['outcome'], 'failed')
        self.assertEqual(validate(script.replace('1 values', '2 values'), Fake(('ok', [[1]])))[0]['outcome'], 'failed')

    def test_expected_error_and_incomplete_state(self):
        script = 'statement error\nbad syntax\n\nquery I\nSELECT 1\n----\n1'
        results = validate(script, Fake(('unsupported', 'syntax')))
        self.assertEqual([r['outcome'] for r in results], ['unsupported', 'blocked'])
        results = validate(script, Fake(('error', 'constraint'), ('ok', [[1]])))
        self.assertEqual([r['outcome'] for r in results], ['passed', 'passed'])
        script = script.replace('statement error', 'statement ok')
        results = validate(script, Fake(('error', 'bug')))
        self.assertEqual([r['outcome'] for r in results], ['failed', 'blocked'])

    def test_conditions_comments_labels(self):
        self.assertEqual(list(records('# comment\nquery I\n# removed\nSELECT 1\n----\n1')),
                         [(2, ['query I', 'SELECT 1', '----', '1'])])
        script = 'skipif coresql\nquery I nosort x\nSELECT 1\n----\n1\n\nquery I nosort x\nSELECT 1\n----\n1'
        self.assertEqual([r['outcome'] for r in validate(script, Fake(('ok', [[1]])))], ['skipped', 'passed'])
        with self.assertRaises(ValueError):
            validate(script.rsplit('1', 1)[0] + '2', Fake())
        with self.assertRaises(ValueError):
            validate('halt', Fake())
        with self.assertRaises(ValueError):
            validate('unknown command', Fake())


if __name__ == '__main__':
    unittest.main()

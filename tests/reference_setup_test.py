"""Offline regression tests for the reference setup's filesystem boundaries."""
import importlib.util
import io
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'benchmarks/reference/prepare.py'
spec = importlib.util.spec_from_file_location('reference_prepare', SCRIPT)
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class ReferenceSetup(unittest.TestCase):
    def test_regular_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            archive = work / 'source.tar'
            with tarfile.open(archive, 'w') as out:
                item = tarfile.TarInfo('sqlite/configure')
                item.size, item.mode = 3, 0o755
                out.addfile(item, io.BytesIO(b'abc'))
            prepare.extract(archive, work / 'source')
            self.assertEqual((work / 'source/configure').read_bytes(), b'abc')
            self.assertTrue((work / 'source/configure').stat().st_mode & 0o100)

    def test_rejects_traversal_and_links(self):
        for name, kind in [('sqlite/../../outside', tarfile.REGTYPE),
                           ('/absolute', tarfile.REGTYPE),
                           ('sqlite/link', tarfile.SYMTYPE)]:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                work = pathlib.Path(directory)
                with tarfile.open(work / 'source.tar', 'w') as out:
                    item = tarfile.TarInfo(name)
                    item.type = kind
                    item.linkname = '../../outside'
                    out.addfile(item)
                with self.assertRaises(ValueError):
                    prepare.extract(work / 'source.tar', work / 'source')

    def test_wrong_checksum_leaves_no_output(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            archive = work / 'bad.tar'
            archive.write_bytes(b'not the pinned archive')
            result = subprocess.run([sys.executable, str(SCRIPT), '--archive', str(archive),
                                     '--output', str(work / 'reference')], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('SHA-256', result.stderr)
            self.assertFalse((work / 'reference').exists())
            self.assertEqual(set(work.iterdir()), {archive})

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            sentinel = work / 'keep'
            sentinel.write_text('existing reference')
            result = subprocess.run([sys.executable, str(SCRIPT), '--output', str(work)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('already exists', result.stderr)
            self.assertEqual(sentinel.read_text(), 'existing reference')


if __name__ == '__main__':
    unittest.main()

"""Verify a built CoreSQL installation from a relocated prefix, without source includes."""
import argparse
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    parser.add_argument('--config', default='Release')
    args = parser.parse_args()
    source = pathlib.Path(__file__).resolve().parent
    build = args.build.resolve()
    with tempfile.TemporaryDirectory(prefix='coresql-package-') as directory:
        work = pathlib.Path(directory)
        prefix = work / 'installed prefix'
        relocated = work / 'relocated prefix'
        subprocess.run(['cmake', '--install', str(build), '--config', args.config, '--prefix', str(prefix)], check=True)
        prefix.rename(relocated)
        for export in relocated.rglob('*.cmake'):
            text = export.read_text()
            if str(build) in text or str(source.parent.parent) in text or str(prefix) in text:
                raise RuntimeError('Non-relocatable path in ' + str(export))
        base = ['cmake', '-S', str(source), '-DCMAKE_PREFIX_PATH=' + str(relocated), '-DCMAKE_BUILD_TYPE=' + args.config]
        consumer = work / 'consumer'
        subprocess.run(base + ['-B', str(consumer)], check=True)
        subprocess.run(['cmake', '--build', str(consumer), '--config', args.config, '-j', '2'], check=True)
        subprocess.run(['ctest', '--test-dir', str(consumer), '-C', args.config, '--output-on-failure'], check=True)
        for name, option in [('version', '-DCORESQL_TEST_VERSION=999.0.0'), ('component', '-DCORESQL_TEST_COMPONENTS=missing')]:
            result = subprocess.run(base + ['-B', str(work / name), option], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if result.returncode == 0:
                raise RuntimeError('Package incorrectly accepted an unsupported ' + name)
        # Installed executables must also work after relocation.
        database = work / 'installed.core'
        subprocess.run([str(relocated / 'bin' / 'coresql_persistent_sql'), str(database)], check=True)
        backup = work / 'backup.core'
        subprocess.run([str(relocated / 'bin' / 'coresql_admin'), 'backup', str(database), str(backup)], check=True)
        restored = work / 'restored.core'
        subprocess.run([str(relocated / 'bin' / 'coresql_admin'), 'restore', str(backup), str(restored)], check=True)
        result = subprocess.run([str(relocated / 'bin' / 'coresql_sql_cli'), '--database', str(restored)],
                                input='SELECT body FROM notes WHERE id=1;', text=True,
                                capture_output=True, check=True)
        if result.stdout != 'Persistent hello\n':
            raise RuntimeError('Installed backup/restore contents differ')
        print('Installed package passed relocation, core/add-on consumers, version and component checks')


if __name__ == '__main__':
    main()

"""Fetch, verify and build the optional SQLite oracle; no Git history required."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def extract(archive, destination):
    # Only regular files/directories under the archive's single root are accepted.
    with tarfile.open(archive) as source:
        roots = set()
        for member in source:
            parts = pathlib.PurePosixPath(member.name).parts
            if not parts or member.name.startswith('/') or '..' in parts:
                raise ValueError('Invalid archive path')
            roots.add(parts[0])
            if len(roots) != 1:
                raise ValueError('Archive has multiple roots')
            target = destination.joinpath(*parts[1:])
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile() and len(parts) > 1:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as incoming, target.open('wb') as outgoing:
                    shutil.copyfileobj(incoming, outgoing)
                target.chmod(0o755 if member.mode & 0o111 else 0o644)
            else:
                raise ValueError('Archive contains a link or special file')


def main():
    pin = json.loads(pathlib.Path(__file__).with_name('sqlite.json').read_text())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path,
                        default=pathlib.Path(tempfile.gettempdir()) / ('coresql-sqlite-' + pin['git_commit'][:12]))
    parser.add_argument('--archive', type=pathlib.Path, help='Use a previously downloaded archive, without network access')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    output = args.output.resolve()
    if any(c.isspace() for c in str(output)) or ';' in str(output):
        parser.error('SQLite configure requires a scratch path without whitespace; avoid CMake list separators too')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if output.exists():
        parser.error('Output already exists; choose a new scratch directory to avoid reusing an unverified build')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Failed downloads/configuration leave no partially published reference directory.
    with tempfile.TemporaryDirectory(prefix='coresql-reference-', dir=output.parent) as scratch:
        staging = pathlib.Path(scratch)
        archive = args.archive.resolve() if args.archive else staging / 'sqlite.tar.gz'
        if not args.archive:
            print('Downloading', pin['url'], flush=True)
            with urllib.request.urlopen(pin['url'], timeout=120) as incoming, archive.open('wb') as outgoing:
                shutil.copyfileobj(incoming, outgoing)
        if digest(archive) != pin['sha256']:
            raise ValueError('SQLite archive SHA-256 does not match the pinned reference')
        source, build = staging / 'source', staging / 'build'
        source.mkdir()
        extract(archive, source)
        if (source / 'manifest.uuid').read_text().strip() != pin['fossil_checkin']:
            raise ValueError('SQLite Fossil check-in does not match the pinned reference')
        if digest(source / 'test/speedtest1.c') != pin['speedtest1_sha256']:
            raise ValueError('SQLite benchmark harness differs from the pin')
        build.mkdir()
        subprocess.run([str(source / 'configure'), '--disable-tcl', '--disable-readline'], cwd=build, check=True)
        subprocess.run(['make', '-j', str(args.jobs), 'sqlite3.c'], cwd=build, check=True)
        # Only the generated amalgamation/header are consumed; generated Makefiles
        # retain staging paths and are intentionally not offered as a reusable build.
        artifacts = staging / 'amalgamation'
        artifacts.mkdir()
        for name in ('sqlite3.c', 'sqlite3.h'):
            shutil.copy2(build / name, artifacts / name)
        shutil.rmtree(build)
        if not args.archive:
            archive.unlink()
        (staging / 'reference.json').write_text(json.dumps(pin, indent=2) + '\n')
        (staging / 'reference.cmake').write_text(
            'set(CORESQL_SQLITE_SOURCE [[' + str(output / 'source') + ']] CACHE PATH "" FORCE)\n'
            'set(CORESQL_SQLITE_AMALGAMATION [[' + str(output / 'amalgamation/sqlite3.c') + ']] CACHE FILEPATH "" FORCE)\n')
        staging.rename(output)
    print('Reference ready:', output)
    print('Configure CoreSQL with: cmake -S . -B build/compare -DCORESQL_BENCHMARKS=ON -C', output / 'reference.cmake')


if __name__ == '__main__':
    main()

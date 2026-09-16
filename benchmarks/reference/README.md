# Pinned SQLite reference

Normal CoreSQL builds do not use SQLite. Optional comparison tools use the exact
SQLite revision and archive digest in [sqlite.json](sqlite.json). The setup needs
Python 3.9+, a C compiler and make. It downloads a public archive, verifies its
SHA-256 and Fossil identifier, extracts it safely, and builds the amalgamation.
No Git checkout, historical tag or submodule is required.

From the CoreSQL repository root:

```sh
python3 benchmarks/reference/prepare.py --output /tmp/coresql-sqlite-reference
cmake -S . -B build/compare -DCMAKE_BUILD_TYPE=Release \
  -DCORESQL_BENCHMARKS=ON -C /tmp/coresql-sqlite-reference/reference.cmake
cmake --build build/compare -j 4
ctest --test-dir build/compare --output-on-failure -j 4
```

Choose a new scratch directory with no whitespace. SQLite's configure script
rejects whitespace in its source/build paths; this restriction does not apply
to CoreSQL's own checkout or installed package. Existing output directories are
rejected to prevent accidentally reusing modified reference sources.

For offline setup, supply the exact pinned archive using `--archive /path/to/sqlite.tar.gz`.
It goes through the same verification and build steps. A failed setup does not
publish a partial output directory. The completed output contains:

- `source/`: pinned source including unchanged `test/speedtest1.c` and notices;
- `amalgamation/`: generated `sqlite3.c` and `sqlite3.h`;
- `reference.json`: the pin used for this build;
- `reference.cmake`: absolute paths for initializing the CoreSQL benchmark build.

If you move the completed reference directory, configure explicitly with its new
`CORESQL_SQLITE_SOURCE` and `CORESQL_SQLITE_AMALGAMATION` paths. CMake verifies the
harness hash. You can omit the amalgamation setting to use system SQLite with
the pinned harness for local experiments; identify that as a different reference
configuration, not the reproducible pinned benchmark.

Keep the original archive checksum stable. If a provider changes the bytes served
at the pinned URL, setup deliberately fails; investigate and review a pin change
instead of bypassing verification. No generated SQLite files are committed or
installed with CoreSQL. See [notices](../../third_party/README.md).

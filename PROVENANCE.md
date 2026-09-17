# Sources and attribution

CoreSQL's original contributions are covered by [LICENSE](LICENSE). External
material retains its own notices. CoreSQL is independently maintained and is not
affiliated with SQLite, Hwaci or DuckDB. Neither SQLite nor DuckDB is a runtime
dependency of the CoreSQL library.

## Included test material

The five unchanged SQLLogicTest files in `tests/sqllogictest/upstream/` come from
[SQLLogicTest](https://www.sqlite.org/sqllogictest/). The
[manifest](tests/sqllogictest/manifest.json) records their origins and SHA-256
hashes; the [original copyright notice](tests/sqllogictest/upstream/COPYRIGHT.md)
retains the available license choices.

## External benchmark references

- **SQLite:** the [reference manifest](benchmarks/reference/sqlite.json) pins the
  source revision, archive digest and `test/speedtest1.c` hash. Reference source
  is downloaded separately. The speedtest1 adapter uses upstream helper functions
  and reproduces its deterministic input generation; see the
  [adapter methodology](benchmarks/speedtest1.md). The
  [preserved SQLite license information](third_party/notices/SQLite.md) is unchanged.
  Its source-tree and build-tool references describe the SQLite archive, whose
  additional notices are retained by the download tooling.
- **DuckDB / TPC-H:** the [query manifest](benchmarks/tpch/manifest.json) pins the
  upstream queries and the `extension/tpch/dbgen/dbgen.cpp` declarations used to
  derive the diagnostic schema. Queries, generated datasets and reference binaries
  are downloaded or generated separately. The [benchmark guide](benchmarks/tpch/README.md)
  documents pinned dependencies and the SQLite adapter's SQL/precision adaptations.
  No DuckDB engine or generator source is included in CoreSQL.

Reference manifests retain exact revisions and checksums. Generated reference
builds and datasets are not installed with CoreSQL. The benchmark drivers,
synthetic relation fixtures and DECIMAL implementation are original CoreSQL code.

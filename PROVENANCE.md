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

## Curated public SQL workload adaptations

`benchmarks/suite/` retains public query shapes from checksum-pinned SQLite
speedtest1, DuckDB microbenchmarks and h2oai/db-benchmark. The
[source manifest](benchmarks/suite/sources.json) identifies exact revisions,
paths and SHA-256 digests. Source files are fetched separately and verified.
Each catalog records changes to SQL or setup; deterministic reduced fixtures are
original CoreSQL benchmark code and are not upstream or published benchmark data.

- `duckdb.json` adapts DuckDB queries under its preserved
  [MIT notice](benchmarks/suite/licenses/DuckDB-MIT.txt).
- `h2o.json` adapts H2O query definitions under
  [MPL-2.0](benchmarks/suite/licenses/H2O-MPL-2.0.txt); the catalog remains covered
  by that license. SELECTs replace result-table creation and separately labeled
  variants expand qualified stars and USING joins. The original operators and
  original syntax probes are retained. See the
  [source-form notice](benchmarks/suite/licenses/README.md).
- `speedtest1.json` adapts additional star/FP query shapes, retaining SQLite's
  original attribution and the existing reference pin. This does not claim full
  coverage of those upstream testsets.

CoreSQL's license does not replace these upstream licenses, and no affiliation
with the benchmark authors or official benchmark certification is implied.

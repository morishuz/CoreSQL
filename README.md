# CoreSQL

![CoreSQL logo](docs/assets/coresql-logo.png)

**A small, extensible embedded database in C++20.**

CoreSQL explores a simple idea: a database should be small enough to understand
and structured enough to extend. It combines a typed relational core, optional
SQL, and durable local files. It has no SQLite runtime dependency.

- **Small and readable.** Small modules separate the engine, SQL frontend,
  public headers and bundled add-ons; tests and external reference code live separately.
- **Clear boundaries.** Storage and transactions belong to the core. SQL parsing
  and domain-specific behavior live in separate modules.
- **Extensible by design.** Register types, scalar functions, aggregates and index
  providers. Even built-in integer, real and text types use the type-extension API.
  Bundled add-ons include JSON, vectors, binary payloads, dates, decimals, timestamps, spatial boxes
  and an experimental graph API. Integer, real, text, BLOB, DATE, DECIMAL and VECTOR use
  shared SQL adapters for declarations, conversions and type-specific operations.

Extensions are trusted, linked C++ code, not sandboxed or dynamically loaded
plugins. The storage engine, transaction model and query operators remain part of
the core. See the [extension contract](docs/contracts/extensions.md).

## Status: early and experimental

**0.1.0 is in development.** CoreSQL is useful for experimentation and small local
applications whose owners can accept changing interfaces and maintain backups.
It is not yet a production replacement for SQLite or DuckDB.

Transactions, savepoints, indexes, crash recovery, backup/restore and a bounded SQL
dialect are implemented. API/ABI and permanent file compatibility are not promised.
An optional disk-backed chunk cache allows stored payloads to exceed its RAM
target; indexes, pinned snapshots and query buffers have separate costs. The
default encoded-state limit is 1 GiB, configurable at build time, not a RAM budget.
A file has one owner. Opt-in snapshot readers can run alongside serialized commits
and background checkpoints; each mutable transaction/SQL connection still needs
external serialization. Windows, a server protocol and full SQL compatibility are absent.

Read the [SQL dialect](docs/contracts/sql.md), [storage contract](docs/contracts/storage.md)
and [backup/upgrade guide](docs/usage.md) before keeping data you care about.

## Try it

Requires CMake 3.20+, a C++20 toolchain with floating-point `from_chars`/`to_chars`,
and macOS 26+ or Linux. Python 3.9+ enables the full test suite.

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j 4
ctest --test-dir build/release --output-on-failure -j 4
./build/release/coresql_persistent_sql notes.core
./build/release/coresql_sql_cli examples/sql.sql
```

The persistent example writes, closes and reopens its database. The CLI runs SQL
scripts or reads standard input. Ordinary builds fetch no dependencies.
For C++ integration, use `CoreSQL::core` or `CoreSQL::sql` through CMake;
[installation and examples](docs/usage.md) cover the complete workflow.

The [bounded landmark-memory example](docs/applications/landmark-memory.md)
provides a background worker, durable acknowledgements and exact vector retrieval.
See [execution controls](docs/contracts/execution.md) for cooperative cancellation
and callback scan streaming.

## Performance

Single-threaded, in-memory **TPC-H Q1–Q22** at SF 0.03 (180,566 lineitems),
measured September 23, 2026 on an Apple M1 with 16 GiB RAM.

Times are medians of three runs after one warmup, including parsing, execution
and fetching but excluding loading. **Ratios above 1× mean CoreSQL is faster.**

<details>
<summary>All 22 query times — milliseconds, lower is better</summary>

| Query | CoreSQL ms | SQLite ms | SQLite ms / CoreSQL ms |
| --- | ---: | ---: | ---: |
| Q1 | 112.804 | 88.198 | 0.78× |
| Q2 | 24.016 | 19.835 | 0.83× |
| Q3 | 24.523 | 47.998 | 1.96× |
| Q4 | 18.402 | 37.840 | 2.06× |
| Q5 | 29.636 | 65.294 | 2.20× |
| Q6 | 18.791 | 11.648 | 0.62× |
| Q7 | 39.204 | 58.406 | 1.49× |
| Q8 | 20.295 | 122.424 | 6.03× |
| Q9 | 44.552 | 151.852 | 3.41× |
| Q10 | 22.987 | 18.921 | 0.82× |
| Q11 | 5.076 | 24.481 | 4.82× |
| Q12 | 21.579 | 23.210 | 1.08× |
| Q13 | 40.414 | 46.131 | 1.14× |
| Q14 | 10.476 | 12.223 | 1.17× |
| Q15 | 10.039 | 11.896 | 1.19× |
| Q16 | 8.069 | 10.680 | 1.32× |
| Q17 | 15.422 | 821.612 | 53.27× |
| Q18 | 49.818 | 70.797 | 1.42× |
| Q19 | 17.237 | Timeout (>15 s) | — |
| Q20 | 12.852 | 1303.171 | 101.40× |
| Q21 | 508.635 | Timeout (>15 s) | — |
| Q22 | 8.704 | 364.444 | 41.87× |

CoreSQL `038e74c573`; pinned SQLite 3.54.0 development build.
SQLite uses adapted SQL with TEXT dates and approximate REAL decimals; CoreSQL
uses native DATE/DECIMAL. Driver overhead differs. This is not an official TPC-H result.

[Methodology](benchmarks/tpch/README.md#bounded-performance-comparison)
· [Raw measurements](benchmarks/tpch/results/tpch-2026-09-23-sf0.03.json)

</details>

## Explore and contribute

[Guide](docs/guide.md) · [Examples](examples/README.md) ·
[Architecture](docs/architecture/direction.md) · [Documentation](docs/README.md) ·
[Contributing](CONTRIBUTING.md)

CoreSQL's original contributions use the [MIT license](LICENSE). External material
retains its own notices; see [sources and attribution](PROVENANCE.md).

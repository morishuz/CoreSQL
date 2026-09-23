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

The latest full **TPC-H Q1–Q22** comparison was measured September 23, 2026,
at **SF 0.03** (180,566 lineitems), using CoreSQL commit `fd5ac5fe9c`.
CoreSQL completed and checked all 22 queries. SQLite completed 20: CoreSQL was
faster on 15, SQLite on five; SQLite hit the 15-second timeout on Q19 and Q21.
DuckDB was faster than CoreSQL on all 22 queries.

These are single-thread, in-memory client timings on an Apple M1 with 16 GiB RAM:
median of three measured runs after one warmup, excluding loading. Parsing,
execution and fetching are included. **SQLite uses adapted SQL, TEXT dates and
approximate REAL decimals; CoreSQL and DuckDB use native DATE/DECIMAL.** Drivers
also differ. This is an engineering comparison, not an official TPC-H result,
a durability benchmark, or evidence of general superiority.

<details>
<summary>All 22 query times — milliseconds, lower is better</summary>

| Query | CoreSQL ms | SQLite ms | SQLite ms / CoreSQL ms | DuckDB ms |
| --- | ---: | ---: | ---: | ---: |
| Q1 | 106.300 | 90.900 | 0.86× | 6.312 |
| Q2 | 23.542 | 19.524 | 0.83× | 1.965 |
| Q3 | 23.870 | 47.900 | 2.01× | 1.524 |
| Q4 | 17.598 | 37.976 | 2.16× | 1.857 |
| Q5 | 30.974 | 65.812 | 2.12× | 2.131 |
| Q6 | 18.648 | 11.648 | 0.62× | 0.512 |
| Q7 | 38.794 | 57.164 | 1.47× | 2.793 |
| Q8 | 21.514 | 126.984 | 5.90× | 2.787 |
| Q9 | 47.590 | 171.599 | 3.61× | 5.170 |
| Q10 | 21.979 | 19.184 | 0.87× | 4.291 |
| Q11 | 4.902 | 24.453 | 4.99× | 2.245 |
| Q12 | 20.895 | 23.133 | 1.11× | 3.616 |
| Q13 | 38.959 | 46.179 | 1.19× | 4.924 |
| Q14 | 10.443 | 11.533 | 1.10× | 0.917 |
| Q15 | 10.258 | 11.595 | 1.13× | 0.947 |
| Q16 | 7.826 | 10.840 | 1.39× | 1.672 |
| Q17 | 14.819 | 803.753 | 54.24× | 1.431 |
| Q18 | 81.326 | 73.726 | 0.91× | 3.517 |
| Q19 | 16.919 | Timeout (>15 s) | — | 6.587 |
| Q20 | 13.209 | 1270.208 | 96.17× | 1.760 |
| Q21 | 513.586 | Timeout (>15 s) | — | 4.690 |
| Q22 | 7.923 | 336.730 | 42.50× | 1.368 |

**SQLite ms / CoreSQL ms** is the ratio of the measured medians: above 1 means
CoreSQL is faster; below 1 means SQLite is faster. No ratio is assigned to timeouts.
SQLite is the pinned 3.54.0 development baseline, not a stable-release comparison;
DuckDB is 1.5.5. CoreSQL used a clean,
test-enabled Release build with AppleClang 21.0.0 on macOS 27.0. Three trials do not
establish a stable ranking for close results. The OS differs from
the historical September 15 comparison, so changes between those runs cannot be
attributed solely to CoreSQL changes.

[Methodology and reproduction](benchmarks/tpch/README.md#bounded-performance-comparison)
· [Raw measurements, dataset manifest and fingerprints](benchmarks/tpch/results/tpch-2026-09-23-sf0.03.json)
· [Historical September 15 comparison](benchmarks/tpch/results/OPTIMIZATION_ROUND2.md)

</details>

## Explore and contribute

[Guide](docs/guide.md) · [Examples](examples/README.md) ·
[Architecture](docs/architecture/direction.md) · [Documentation](docs/README.md) ·
[Contributing](CONTRIBUTING.md)

CoreSQL's original contributions use the [MIT license](LICENSE). External material
retains its own notices; see [sources and attribution](PROVENANCE.md).

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

The current application pilot is a [bounded landmark memory](docs/applications/landmark-memory.md)
with a background worker, durable acknowledgements and exact vector retrieval.
See [execution controls](docs/contracts/execution.md) for cooperative cancellation
and callback scan streaming.

## Performance

The latest recorded full **TPC-H Q1–Q22** comparison is a historical September 15,
2026 run at **SF 0.03** (180,566 lineitems). CoreSQL completed and checked all 22
queries. SQLite completed 20: CoreSQL was faster on 12, SQLite on eight; SQLite
hit the 15-second timeout on Q19 and Q21. DuckDB was faster on all 22 in that run.

These are single-thread, in-memory client timings on an Apple M1 with 16 GiB RAM:
median of three measured runs after one warmup, excluding loading. Parsing,
execution and fetching are included. **SQLite uses adapted SQL, TEXT dates and
approximate REAL decimals; CoreSQL uses native DATE/DECIMAL.** Drivers also differ.
This is an engineering comparison, not an official TPC-H result, a durability
benchmark, or evidence of general superiority.

<details>
<summary>All 22 query times — milliseconds, lower is better</summary>

| Query | CoreSQL ms | SQLite ms |
| --- | ---: | ---: |
| Q1 | 175.283 | 90.511 |
| Q2 | 21.437 | 19.803 |
| Q3 | 32.464 | 48.350 |
| Q4 | 19.690 | 37.819 |
| Q5 | 39.436 | 70.774 |
| Q6 | 29.702 | 11.726 |
| Q7 | 64.957 | 56.954 |
| Q8 | 34.362 | 129.663 |
| Q9 | 57.243 | 158.060 |
| Q10 | 24.927 | 18.897 |
| Q11 | 5.360 | 25.208 |
| Q12 | 30.588 | 23.301 |
| Q13 | 38.574 | 46.479 |
| Q14 | 20.049 | 11.768 |
| Q15 | 20.177 | 11.745 |
| Q16 | 8.206 | 10.973 |
| Q17 | 13.022 | 801.836 |
| Q18 | 62.454 | 75.183 |
| Q19 | 14.256 | Timeout (>15 s) |
| Q20 | 13.201 | 1251.266 |
| Q21 | 600.516 | Timeout (>15 s) |
| Q22 | 8.364 | 336.354 |

No speed ratio is assigned to timeouts. SQLite is the pinned 3.54.0 development
baseline, not a stable-release comparison. CoreSQL was a modified working tree
based on `2d3342712b`; these measurements predate the current changes and are not
a benchmark of the current checkout. Three trials do not establish a stable
ranking for close results; Q8 also showed variability in a separate check.

[Full methodology and DuckDB results](benchmarks/tpch/results/OPTIMIZATION_ROUND2.md)
· [Raw measurements and fingerprints](benchmarks/tpch/results/optimization-round2-sf0.03.json)
· [Reproduction instructions](benchmarks/tpch/README.md#include-the-pinned-sqlite-reference)

</details>

## Explore and contribute

[Guide](docs/guide.md) · [Examples](examples/README.md) ·
[Architecture](docs/architecture/direction.md) · [Documentation](docs/README.md) ·
[Contributing](CONTRIBUTING.md)

CoreSQL's original contributions use the [MIT license](LICENSE). External material
retains its own notices; see [sources and attribution](PROVENANCE.md).

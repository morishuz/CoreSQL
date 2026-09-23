# Changelog

## Unreleased — 0.1.0

Initial experimental release in development. Public API/ABI, full SQLite
compatibility and permanent storage-format compatibility are not guaranteed.

### Core and persistence

- Typed relational queries with joins, grouping, aggregates, ordering, indexes and
  correlated subqueries; atomic mutations, snapshot transactions and savepoints.
- Durable commits, recovery, integrity checks, backup/restore and explicit or
  automatic checkpoints. Optional background checkpoints allow writes to continue
  during encoding and preserve subsequent acknowledged commits.
- Opt-in concurrent committed snapshots and read-only SQL connections. Applications
  must supply extensions that support concurrent calls.
- Optional disk-backed row paging and native INTEGER primary-key lookup caching.
  Cache targets exclude dirty transactions, other indexes and query memory.
- Resumable core/SQL cursors and callback streaming for scans, OFFSET, UNION ALL,
  one or two INNER/LEFT/CROSS joins, and single-table ORDER BY with a matching
  ordered index. Cursors retain their snapshot and return owned rows.
- Cooperative cancellation, deadlines, work limits and operator-buffer limits.
  These do not impose hard real-time deadlines or a whole-process memory cap.
- Persistent CHECK constraints, immediate RESTRICT foreign keys and composite
  unique keys apply to SQL and native writes. Constraint-owned indexes cannot be
  dropped independently of their table.
- Composite ordered-index equality/range searches and atomic UPDATE/DELETE results.
- Configurable encoded-state limits, schema inspection, table/column rename,
  DROP TABLE/INDEX, ANALYZE, VACUUM and payload/cache/storage diagnostics.

### SQL and extensions

- SQL parameters include positional and named slots. SELECT supports nullable
  values, casts, CASE/COALESCE, derived tables, ordinary CTEs, compound queries,
  outer joins, JOIN USING and mixed or qualified star projections.
- Generated INTEGER primary keys, DEFAULT VALUES and column/star RETURNING for
  INSERT, UPDATE and DELETE. Generated keys may reuse a deleted maximum; they are
  not persistent sequences or AUTOINCREMENT.
- VALUES/DEFAULT VALUES UPSERT supports targeted DO UPDATE and DO NOTHING.
  CHECK, NOT NULL and foreign-key failures remain errors.
- Optional bounded connection-local caching of repeatable SELECT templates.
  Parameter values are supplied afresh, and execution uses the current snapshot.
- Scalar, JSON, VECTOR, BLOB, DATE, DECIMAL, TIMESTAMP, spatial and graph add-ons.
  Extension interfaces cover types, functions, aggregates and indexes; extensions
  remain trusted native code.
- A bounded landmark-memory example provides vector retrieval, explicit retention,
  queue backpressure, durable write acknowledgements and operational diagnostics.

### Compatibility

- Type providers declare `layout` and may certify native comparison/hash semantics
  with `native_ops`. Custom i64/i128 types can use tagged compact values. Function
  results remain validated regardless of that certificate; see the
  [extension contract](docs/contracts/extensions.md).
- New exports use `CORESQL7`, and durable records use `CORECHG8` with `CORELOG2`
  framing. Older supported formats remain readable, including migration of legacy
  unique-constraint protection. Older binaries reject newly written formats;
  retain verified backups before upgrading. See the
  [storage contract](docs/contracts/storage.md#formats-and-migration).

### Tooling

- Relocatable CMake packages, installed-consumer checks, SQL/admin examples,
  SQLLogicTest fixtures, differential tests and Release/sanitizer CI.
- Reproducible TPC-H Q1–Q22 and speedtest1 comparisons, a curated DuckDB/H2O SQL
  workload suite, and durable-operation, paging and worker-latency benchmarks.
  References and adaptations retain their source pins and license notices.

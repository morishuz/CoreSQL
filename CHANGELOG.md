# Changelog

## Unreleased — 0.1.0

Initial experimental release in development. Public API/ABI, full SQLite
compatibility and permanent storage-format compatibility are not guaranteed.

### Core and persistence

- Type add-ons declare a closed `layout` and optional `native_ops`. Query
  execution specializes on those, not on integer type identity.
- Custom types may use tagged compact `i64`/`i128` cells; INTEGER/REAL/TEXT stay
  untagged. Hash joins and grouping then follow `native_ops` for those layouts.
- Typed relational API with joins, grouping, aggregates, ordering, indexes and
  correlated subqueries; statement-atomic mutations and snapshot transactions.
- Durable logs, checkpoints, backup/restore, integrity checks, named savepoints
  and transactional schema changes.
- Configurable encoded-state limit, defaulting to 1 GiB, with boundary and
  recovery coverage. Existing nullable-column migration fixtures remain supported.

### SQL and extensions

- SQL frontend with parameters, nullable columns, casts, CASE/COALESCE, compound
  queries, outer joins, derived tables, ordinary CTEs and aggregate expressions.
- Generated integer primary keys, column-only INSERT RETURNING, DEFAULT VALUES,
  DROP TABLE/INDEX, rename operations, LIMIT/OFFSET and result-column metadata.
- Shared SQL type adapters for INTEGER, REAL, TEXT, DATE, DECIMAL and VECTOR.
  DATE provides calendar values; DECIMAL preserves exact numeric input and
  arithmetic; VECTOR provides construction, conversion and squared L2 distance.
- Backend and SQL sources grouped by provider, with unchanged backend encodings
  and a runnable custom-type example using only public interfaces.
- Preserve streaming and correlated reuse for repeatable adapter operations;
  avoid redundant conversion dispatch and temporary insert conversion expressions.
- Additional backend extensions for timestamps, JSON, spatial values and graphs.

### Tooling and validation

- Relocatable CMake packages, installed-consumer checks, persistent SQL and admin
  examples, and macOS/Linux Release and sanitizer CI.
- Pinned SQLLogicTest fixtures, SQL differential tests, storage recovery tests
  and optional SQLite/DuckDB comparison tools. See [benchmark methods](benchmarks/README.md)
  for workloads, reference versions and measurement limits.

See the [documentation index](docs/README.md) for current capabilities and contracts.

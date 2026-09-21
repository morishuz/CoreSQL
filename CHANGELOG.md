# Changelog

## Unreleased — 0.1.0

Initial experimental release in development. Public API/ABI, full SQLite
compatibility and permanent storage-format compatibility are not guaranteed.

### Core and persistence

- Composite equality-prefix/range index selection preserves safe predicate order;
  unchanged ordered-index keys are shared during updates.
- The landmark worker coalesces bounded FIFO ingestion groups with per-request
  savepoints, supports explicit/idle-deferred checkpoints, and records bounded
  queue/execution/commit/total latency diagnostics.

- Persistent CHECK expressions and immediate RESTRICT foreign keys enforce native
  and SQL writes, survive rollback/renames/recovery and reject invalid imports.
  CORESQL6/CORECHG7 carry their metadata; older formats remain readable.
- Cooperative query cancellation, deadlines and work limits, plus callback streaming
  for single-table scans. These are not allocator caps or real-time guarantees.

- Certified native TEXT, REAL and compact i128 equality joins build snapshot-local
  hash lookups, preserving duplicate order, NULL evaluation and custom-provider
  fallback. Text keys borrow stored values instead of copying payloads.

- Type add-ons declare a closed `layout` and optional `native_ops`. Query
  execution specializes on those, not on integer type identity.
- Custom types may use tagged compact `i64`/`i128` cells; INTEGER/REAL/TEXT stay
  untagged. Hash joins and grouping then follow `native_ops` for those layouts.
- DATE and TIMESTAMP store compact `i64` cells with `native_ops`, so they share
  integer hash joins and grouping. On-disk encoding remains length-prefixed
  little-endian days/microseconds.
- DECIMAL stores a compact `i128` coefficient; on-disk bytes match the previous
  Opaque encoding.
- Bound function evaluation validates every returned value. Type lookup and
  `validate_type` run at bind; per-row checks are layout and `validate_value`.
  `native_ops` remains a comparison and hash certificate, not a validity shortcut.
- The bundled hash index stores native i64 keys in hash tables instead of
  ordered maps, including DATE and TIMESTAMP primary keys.
- Compact cells intern types without allocating on hits, encode DECIMAL inline,
  and pack payload width into the type pointer so `Value` stays 32 bytes.
  Interning keeps an 8-entry thread-local cache; DECIMAL arithmetic retains
  interned result types. Repeatable grouping hashes native_ops keys, not only a
  single i64 column. Eligible general inner ON joins retain borrowed matched-row
  pairs, preserving complete ON evaluation, NULL candidates and phase ordering
  before WHERE/sort/projection without copying intermediate values.
- Typed relational API with joins, grouping, aggregates, ordering, indexes and
  correlated subqueries; statement-atomic mutations and snapshot transactions.
- Durable logs, checkpoints, backup/restore, integrity checks, named savepoints
  and transactional schema changes.
- Configurable encoded-state limit, defaulting to 1 GiB, with boundary and
  recovery coverage. Existing nullable-column migration fixtures remain supported.

### SQL and extensions

- JOIN USING with merged inner/outer keys, scalar ROUND, and column/star
  UPDATE/DELETE RETURNING with native atomic result capture.
- Parameterized logical SELECT templates accept changing values, with fresh
  snapshot binding and schema/type/NULL-shape invalidation.

- VALUES/DEFAULT VALUES UPSERT with targeted DO UPDATE, DO NOTHING and existing
  column RETURNING support; CHECK/NOT NULL/foreign-key errors remain errors.
- Independent BLOB add-on and SQL adapter with hex literals, byte length, ordering
  and explicit raw-byte TEXT conversions.
- One opt-in, bounded connection-local cache for repeatable logical SELECT plans with
  stable parameter types and schema; execution binding remains snapshot-local.
- Bounded landmark worker with model/frame identities, queue backpressure, explicit
  retention, synchronized acknowledgements and operational/soak profiles.

- Named parameter slots, table-level composite PRIMARY KEY/UNIQUE declarations,
  and mixed/qualified star projections. Constraints use native indexes and
  nullability and survive durable reopening.

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

- Curated SQL benchmark suite adds attributed speedtest1 star/FP, DuckDB micro
  and H2O grouping/join workloads alongside full speedtest1 main and TPC-H runs,
  with pinned references, full-result checks and untimed execution diagnostics.
- Relocatable CMake packages, installed-consumer checks, persistent SQL and admin
  examples, and macOS/Linux Release and sanitizer CI.
- Pinned SQLLogicTest fixtures, SQL differential tests, storage recovery tests
  and optional SQLite/DuckDB comparison tools. See [benchmark methods](benchmarks/README.md)
  for workloads, reference versions and measurement limits.

See the [documentation index](docs/README.md) for current capabilities and contracts.

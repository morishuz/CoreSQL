# Changelog

## Unreleased — preparing 0.1.0

- Keep the configured encoded-size limit consistent across library and in-tree
  consumers; add persistence/rejection coverage and an 8 MiB validation command.
- Prepare a single-commit main branch while preserving development history on a
  backup branch and retaining original provenance and notices.

- Simplify public documentation, promote JSON/graph usage guides, consolidate
  release findings and retain the evidence behind the README benchmark. Historical
  research and intermediate measurements remain in Git history.

- Generate omitted/NULL SQL integer primary keys and add column-only INSERT
  RETURNING with aliases and star expansion. Cover rollback/savepoints, explicit
  IDs, exhaustion, stale transactions, backup/restore and interrupted commits.
  Generation derives from current rows; no storage-format change or never-reuse
  guarantee is introduced. Typed C++ inserts continue requiring explicit keys.


- Raise the default encoded-state/record/snapshot bound to 1 GiB with a consistent
  build-time setting and a public limit query; add >64 MiB recovery/backup coverage.
- Add transactional DROP TABLE/INDEX, table/column rename, SQL IF [NOT] EXISTS,
  named savepoints, DEFAULT VALUES, LIMIT/OFFSET and SELECT column metadata.
  Destructive table schema changes use the existing atomic checkpoint protocol.
- Add native durable backup and snapshot restore APIs, installed admin tooling,
  a persistent parameterized SQL example, upgrade guidance and handoff validation.


- Add query-local derived relations, ordinary CTEs, output-column alias lists,
  GROUP BY aliases, EXTRACT(YEAR) and UTF-8 SUBSTRING.
- Cache repeatable uncorrelated IN results lazily and bound grouped-IN and
  integer multi-key joins. Q18 and Q9 now complete; all 22 pinned TPC-H queries
  match DuckDB at generated SF 0.001, with additional nonempty known answers.
  Document eager shared CTE materialization and preserve callback/NULL/LIMIT
  boundaries, transaction snapshots and query-local schema isolation.

- Add the independent DECIMAL add-on, exact arithmetic/SUM, explicit casts and
  storage conversion, REAL division/AVG and SQL literal precision preservation.
  Cover arithmetic with Python Decimal and native atomicity/recovery tests.
- Add a pinned TPC-H generator and complete generated-data validation report.
  The initial SF 0.001 report matched 15 queries; six SQL gaps and a Q18
  timeout remained at that stage. Preserve callback order while accelerating safe leading joins.

- Add an independent calendar DATE add-on, SQL DATE literals/casts/storage
  conversions, BIGINT spelling and CLI date display. Calendar, atomicity and
  recovery tests pass; nine pinned TPC-H date queries match synthetic boundary
  fixtures. The native-DATE/REAL diagnostic now executes 16/22 queries; exact
  DECIMAL and generated-data validation were deferred to the following batch.

- Support NOT LIKE and typed outer references in comma-joined subqueries.
  Pinned TPC-H Q2/Q16 now execute and match SQLite on a small synthetic fixture;
  This completed the first query-specific compatibility batch.

- Record SQL coverage and a benchmark-driven feature roadmap. Add an opt-in,
  checksum-pinned TPC-H syntax/binding audit; no benchmark performance claimed.

- Reject stale joined-row locations and mismatched primary-index erasures.
  Extend explicit integrity checks to all bundled index providers; custom
  providers without validation support now report `unsupported`.
- Add regression coverage for index corruption and first-row scalar subqueries;
  refresh architecture direction and nullable-value API comments.

- Promote the independent C++ implementation to the repository root and separate
  current documentation from historical experiments and measurements.
- Remove unchanged SQLite source/build tooling from the active tree; preserve
  provenance, Git history and notices. Add a checksum-verified external benchmark
  reference that works with shallow clones and source archives.
- Add relocatable CMake package exports for the core, SQL frontend and add-ons,
  plus installed-consumer checks and macOS/Linux Release/sanitizer CI.
- Separate expression binding, relational execution, mutations and database
  ownership; consolidate validation and correlated subquery substitution.

This is preparation for the first experimental release, not an announcement
that a release has been published. Public API/ABI, SQLite compatibility and
permanent storage-format compatibility are not guaranteed. See the current
contracts in docs/ and the retained evidence in benchmarks/tpch/results/.

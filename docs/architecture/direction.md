# CoreSQL architecture

CoreSQL keeps a small typed relational core, an optional SQL frontend and domain
add-ons. Maintainability, correctness and clear boundaries take priority over
matching every feature of a mature database.

## Boundaries

- `src/` owns schema, logical queries, transactions, indexes and persistence.
  Private storage layouts are not application identities or extension APIs.
- `sql/` parses and lowers SQL through the public core API. Core behavior must not
  depend on SQL syntax or coercion rules.
- `addons/` implements domain types and behavior through public extension contracts.
  Add-ons do not publish transactions or manage durability.
- `include/coresql/` exposes the embedding interfaces. API/ABI and permanent file
  compatibility remain experimental.

All types use registration, including built-in scalars. This does not make the
storage engine or query executor replaceable plugins. See the
[extension contract](../contracts/extensions.md).

## Storage and execution

Transactions share snapshots and copy affected structures on writes. Commit
rejects stale writers; mutations and savepoints preserve atomicity. Persistent
storage uses an append log and full-state checkpoints, rebuilding indexes on open.
Snapshot export is distinct from a synchronized durable commit.

All live rows remain in RAM. Query results and many intermediate structures are
materialized. Files have one owner and calls require external serialization.
The [storage contract](../contracts/storage.md) defines guarantees and costs;
the [relational contract](../contracts/relational.md) defines query behavior.

A pager, persisted indexes, spill, broader concurrency and strict memory budgets
are future investigations. No storage redesign is selected. Preserve logical
query interfaces and snapshot-local row identities so future storage changes do
not leak into application semantics.

## Development priorities

Keep semantics explicit, preserve meaningful diagnostics, and add regression
coverage for atomicity, error ordering, NULLs and borrowed-value lifetimes.
Domain-specific features should demonstrate useful composition without engine
specialization; [JSON](../extensions/json.md) and [graph](../extensions/graph.md)
are examples, not proof that every extension workload is covered.

Use concrete application needs to choose work from the [SQL backlog](sql-roadmap.md).
Measure equivalent inputs and durability settings before claiming performance
improvements. [Benchmark tooling](../../benchmarks/README.md) keeps SQLite as an
external reference, never the CoreSQL implementation or a runtime dependency.

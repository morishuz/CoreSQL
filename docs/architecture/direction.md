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
  Each provider has its own directory; see the [add-on map](../../addons/README.md).
  Add-ons do not publish transactions or manage durability.
- `include/coresql/` exposes the embedding interfaces. API/ABI and permanent file
  compatibility remain experimental.

All types use registration, including built-in scalars. This does not make the
storage engine or query executor replaceable plugins. See the
[extension contract](../contracts/extensions.md).

## Storage and execution

Transactions share snapshots and copy affected structures on writes. Commit
rejects stale writers; mutations and savepoints preserve atomicity. Persistent
storage uses an append log and full-state checkpoints. Background checkpoints
encode a committed snapshot while writes continue, then publish a synchronized
replacement with the subsequent committed log tail. Destructive schema changes
and the retained-history bound still use synchronous checkpoints. Snapshot export
is distinct from a synchronized durable commit.

One owner holds each persistent file. With `OpenOptions::concurrent_reads` enabled,
independent committed snapshots can execute concurrently with a writer; commit
publication is serialized. The application certifies concurrent-safe extensions
when enabling this option. Mutable transactions, SQL connections and cursors each
require one caller at a time. There are no cross-process readers or multiple file
owners. The [storage contract](../contracts/storage.md) defines these guarantees.

Rows remain resident by default. An optional decoded-chunk cache loads immutable
pages from private disk backing and retains explicit pins while values are borrowed.
Native INTEGER primary keys can use disk-backed sorted lookup images, a mutation
overlay and a disposable persisted reopen cache. That cache is invalidated by a
changed durable log; other indexes retain their resident implementations. Recovery
still validates rows and cached mappings. Neither paging nor cached indexes impose
a total RAM ceiling or guarantee faster reopening.

Resumable cursors cover scans, OFFSET, UNION ALL, ordered index order, and one or
two INNER/LEFT/CROSS joins.
Blocking operators and many joins still materialize state; accounted query-buffer
limits can fail an operation before further tracked growth, but there is no disk
spilling or strict allocator budget. See the [execution contract](../contracts/execution.md)
for supported shapes, lifetime rules and memory exclusions.

The [landmark-memory pilot](../applications/landmark-memory.md) bounds its logical
map at 100,000 128-dimensional landmarks. Its worker uses one writer, two snapshot
readers by default, a 64 MiB decoded-chunk target and background checkpoints. The
provisional 1 GiB process budget remains a deployment measurement target, not an
enforced ceiling. General persisted index pages, external-memory operators and
predictable storage latency remain further work. Ordered index cursors stream a
matching ORDER BY without sorting.
Preserve logical query interfaces and snapshot-local row identities as storage
and execution evolve.

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

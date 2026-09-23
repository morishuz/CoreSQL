# Concurrent committed readers

A database file still has one process-local owner. `OpenOptions::concurrent_reads`
opts that owner into concurrent execution on independent query/transaction state.
The application certifies that all supplied type, scalar, aggregate and index
callbacks permit concurrent calls; bundled add-on callbacks satisfy this contract.
A deterministic or repeatable custom callback is not automatically thread-safe.
Configure registries/adapters before sharing the database; do not mutate callback
configuration while readers use it.

```cpp
coresql::OpenOptions options;
options.concurrent_reads = true;
options.page_cache_bytes = 16 * 1024 * 1024;
auto db = coresql::Database::open(path, registry, options);
auto snapshot = db.snapshot();
coresql::sql::ReadConnection reader(snapshot);
auto result = reader.query(coresql::sql::Statement("SELECT id FROM observations"));
```

`Database::snapshot()` captures the latest published committed state under a short
lock. `ReadSnapshot` owns that immutable view and its registry, remains stable
across later writes/schema changes, and can outlive the original database object.
Its read-only SQL connection rejects mutation/transaction statements. Snapshot
queries on separate reader threads do not take the commit lock. Snapshot creation
and background checkpointing require the explicit concurrency opt-in.

The database serializes durable commits and their publication. Transactions use
optimistic conflict detection: a transaction whose base is no longer current must
roll back and retry. No writer publishes before the storage commit succeeds.
A read begun after a successful commit sees that commit or a later one. An older
snapshot intentionally keeps its old values. Applications that enqueue writes
must wait for acknowledgement or establish an equivalent ordering barrier before
acquiring a read that depends on them.

Each mutable `Transaction`, SQL `Connection` and cursor requires one caller at a
time. Separate read-only snapshots/connections can run concurrently. Database
move/destruction must be externally coordinated with calls on that database object;
already-owned snapshots/cursors and a launched checkpoint future retain their own
resources. This is same-process concurrency, not cross-process readers/writers.

`ReadSnapshot::age()` and cursor statistics expose lifetime and shared logical
payload. Those bytes are not unique retained allocations. Long-lived snapshots
can retain old disk pages and index versions, while active page pins can exceed
the clean-cache target. Release snapshots/cursors when finished. A whole-process
memory ceiling is not implied by the concurrency or cache APIs.

`checkpoint_async()` captures a committed snapshot, writes outside the commit
lock, catches up subsequent durable log records and publishes under the commit
lock. Keep its returned future and call `get()` to observe errors; destroying the
last future from the asynchronous launch can wait for completion. Multiple
simultaneous checkpoint requests on one owner are rejected with `conflict`.
See the [storage contract](storage.md) for crash recovery and fallback behavior.

The landmark worker uses one write owner, up to four reader threads and one
background checkpoint. Read snapshots are captured when the owner reaches that
request in its FIFO queue, after preceding writes commit. Reader completion can
be out of order, and later writes may proceed while an earlier reader computes.
An explicit checkpoint drains preceding readers and waits for pending maintenance.
Shutdown drains accepted reads and writes and observes background failures.

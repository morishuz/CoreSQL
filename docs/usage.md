# Installation, persistence and backups

CoreSQL 0.1 is an experimental embedded C++20 database for small local applications.
Use one owner per local file, externally serialize calls, and keep the database in
RAM. macOS and Linux are the maintained platforms. This is not a server, a paged
storage engine, or a promise of compatibility with arbitrary SQLite applications.

## Build, install and try

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j 4
ctest --test-dir build/release --output-on-failure -j 4
./build/release/coresql_persistent_sql notes.core
./build/release/coresql_persistent_sql notes.core
```

The example initializes only once, binds parameters, commits a write, closes the
file and verifies the data after reopening. Repeating it is safe: the application
supplies its key and uses REPLACE. See `examples/persistent_sql.cpp` for the complete
embedding example. SELECT results include column names even for empty results.

For deployment, build with `-DBUILD_TESTING=OFF`, install using `cmake --install`,
and link `CoreSQL::sql` through the exported CMake package. The SQL CLI, admin tool
and persistent example are installed under `bin`. Pin `find_package(CoreSQL
0.1.0 EXACT REQUIRED COMPONENTS sql)` and the exact source revision used to build
it; the version string alone does not identify an unreleased checkout.

## Size and memory

The default encoded live-state, snapshot and individual record limit is **1 GiB**.
Set `-DCORESQL_MAX_ENCODED_MIB=2048` at configuration time to select another bound
(1–16384 MiB). `Database::encoded_size_limit()` reports the compiled byte limit.
This is an admission limit, not a tested memory budget or a throughput guarantee.
The on-disk log can be larger because it retains recent history.

Rows and indexes stay in RAM. Checkpointing allocates a complete encoded image;
backup/restore, query results and retained transaction snapshots can add further
copies. The regression suite exercises 72 MiB of row payload, beyond the former
64 MiB boundary, including checkpoint, reopen, backup and snapshot restore. It does
not certify comfortable operation at every configurable limit. Measure your actual
workload and leave RAM headroom before increasing the bound. Smaller builds reject
oversize files; they must never silently truncate them to meet the limit.

## Backup and restore

Close the application before using the admin tool, which requires exclusive file
ownership. From a running application use `db.backup(new_path)` with externally
serialized access; only committed state is included.

```sh
coresql_admin backup notes.core notes-backup.core
coresql_admin restore notes-backup.core notes-restored.core
coresql_sql_cli --database notes-restored.core check.sql
```

`check.sql` should contain `PRAGMA integrity_check;` and application-specific row
counts and known-answer queries. The utility opens and validates the source,
writes a synchronized native database, reopens it and checks integrity, schema and
row count. A backup is itself a native database. Restore creates another new file;
it never overwrites the backup or the original. Do not copy a live file with a
filesystem copy command while commits or checkpoints can run.

An old snapshot export from `Database::save` can be imported with:

```sh
coresql_admin import-snapshot old-export.snapshot migrated.core
```

`Database::restore(snapshot, destination, registry)` provides the same operation
for applications. It preserves schema, defaults, index declarations and hidden row
identities. `save()` alone is an export, not a synchronized durable backup.
The admin tool installs builtin SQL/date/decimal types. For other add-ons use the
C++ methods with the full application registry.

All destinations must be new paths. If backup/restore fails or is interrupted, the
new destination may be incomplete, and its name remains reserved. Keep the source,
choose a fresh destination for retry, and treat only a successfully verified output
as a backup. Source opening performs normal recovery of an interrupted final commit.
Keep backups on a separate failure domain if loss of the local disk matters.

## Schema lifecycle and compatibility

CREATE TABLE/INDEX IF NOT EXISTS, DROP TABLE/INDEX IF EXISTS, ALTER TABLE RENAME TO,
ALTER TABLE RENAME COLUMN, and ADD COLUMN support ordinary application migrations.
Apply related operations inside BEGIN/COMMIT; errors can be rolled back. DROP and
rename preserve older transaction snapshots. Table removal and rename commit via
the existing atomic checkpoint protocol, paying a full live-state rewrite. Index
removal uses a normal change record. Renaming columns also updates named index
column references. Inline UNIQUE constraint indexes cannot be dropped directly.

Keep an application schema-version table and migrate explicitly. IF NOT EXISTS
only skips an existing object; it does not check that its definition is compatible.
For changes outside this dialect, create a new database with the desired schema
and copy/validate application rows, keeping the original until verification passes.

The current implementation writes CORELOG2 / CORECHG6 and CORESQL5 snapshots and retains
its existing older-format readers. The new schema operations need no new on-disk
encoding because incompatible table changes publish a complete checkpoint. Older
binaries still have their old size limits. C++ source/ABI and permanent format
compatibility remain experimental: retain the old binary and registry with backups.
Before any future upgrade, back up with the old build, test the new build against
a copy, verify application queries, and retain the original. If direct opening is
unsupported, export using the old binary and import with the new one. Do not run a
new writer against your only original file as an upgrade experiment.

## Generated IDs

```sql
CREATE TABLE messages(id INTEGER PRIMARY KEY, body TEXT);
INSERT INTO messages(body) VALUES ('Hello') RETURNING id, body;
```

Omitted/NULL integer primary keys can now be generated; INSERT RETURNING exposes
columns and aliases. See the [SQL contract](contracts/sql.md#generated-integer-primary-keys-and-insert-returning)
for deletion reuse, exhaustion, rollback and the initial maximum-scan cost.

# Storage contract and implementation

The public query/type/transaction API is unchanged. `Database::open` selects
persistent mode; ordinary construction and snapshot `load` remain in memory.
`checkpoint()` explicitly reclaims obsolete persistent records. Calling it on an
in-memory database is an error. `storage_stats()` reports session bytes submitted
to write calls and completed checkpoints; these are not device-level write counts.

## Atomicity and durability

- A successful dirty commit publishes all its changes together. Read-only commits
  and rollbacks write nothing. Stale transactions must roll back and retry.
- Mutation calls are atomic within a transaction: failed validation, allocation,
  or extension evaluation cannot expose earlier row edits from that statement.
- A record contains changed chunks plus table descriptors, a checksum, length and
  inverse length, and a commit marker. The payload is synchronized before writing
  the marker, then the marker is synchronized before returning success.
- macOS uses `F_FULLFSYNC`; Linux uses `fsync`. Directory entries are synchronized
  on creation/open and checkpoint publication. Guarantees depend on local storage
  and the OS honoring these operations; unsupported synchronization returns an error.
- Reopening replays complete valid records and truncates an incomplete final
  record. A complete but corrupt record is rejected. Recovery does not silently
  discard acknowledged transactions to get past corruption.
- An interrupted commit may be present or absent after reopening. A write/sync or
  checkpoint-publication exception disables the handle; close and reopen to learn
  the outcome before retrying. Pre-I/O validation/encoding errors leave it usable.
- Checksums detect accidental corruption, not malicious edits. Process-stop and
  injected-failure tests are not physical power-cut or storage-device certification.
  Full-length torn/corrupt headers or markers are rejected, not repaired.

## Shared chunks

Each table owns a dense, ID-sorted vector of chunk IDs and shared chunk pointers. A chunk contains
at most 128 rows. Inserts target at most 16 KiB of encoded row data per chunk;
a single large row can exceed that target, and updates can grow an existing chunk.
IDs preserve row order and do not shift when other chunks are deleted.

Beginning a transaction shares tables. A modified table copies its chunk map,
not all its rows. The copy allocates once and is proportional to live chunk count,
not the highest historical ID. Lookup uses binary search. Updates and deletes copy only affected chunks into a candidate
table; it replaces staged state only after the entire statement succeeds.
Inserts reuse privately owned chunks within a transaction. Unchanged chunks stay
shared with older snapshots. Empty chunks are removed, including from the log.
Statement deletion compacts metadata once; single metadata erasures and inserting
out-of-order IDs during recovery can shift later entries. There is no constant-time
write-cost guarantee.

Small-write temporary storage is proportional to affected chunks plus a table's
chunk-map metadata and encoded change record. It is not a fixed byte budget:
large values, statements touching many chunks, and retained old transactions can
still use substantial memory. Commit currently scans chunk metadata to identify
changes. Direct primary-key equality visits at most one row chunk; other
predicates still scan. Keys use a shared array of 256 index buckets, copied on
write only where needed. Bucket copying adds memory/time proportional to affected
buckets, potentially large for skewed hashes; non-key updates share the index.

All live rows are still resident in RAM. Live checkpoint encoding and an individual
change record are each capped at the configured encoded-size limit (1 GiB by default). The live-size check uses cached encoded
chunk sizes, so it does not serialize unchanged values on every commit. This is
not a cap on allocations, peak process memory, or the on-disk log size.

## Incremental persistence and bounded history

Ordinary commits encode only changed/new chunks, deleted chunk IDs, and schemas
for changed tables. Add-on bytes are encoded through the same type contract as
snapshot exports; there are no vector/timestamp branches in storage.

Before a commit would grow the log beyond `max(1 MiB, 4 * compacted live size)`,
it instead checkpoints the new state. Checkpointing writes one complete state to
a reserved sibling file `<database>.checkpoint`, synchronizes it, atomically
renames it over the database, and synchronizes the parent directory. This is an
occasional full-state rewrite, not the ordinary commit path. A large deletion can
trigger an immediate checkpoint as the live size shrinks. Explicit checkpointing
is also available for applications that want to schedule reclamation.

After a successful commit the main file is bounded by that threshold. During
checkpointing the old log and one compacted replacement coexist on disk, and a
full encoded checkpoint buffer exists in memory. Checkpoint latency is paid by
the committing caller. No background compactor or page cache exists yet.

Both old and replacement files remain exclusively locked across publication.
An opener checks that its locked inode still matches the pathname, so a paused
opener cannot return an obsolete database after a checkpoint. Recovery removes an
unpublished checkpoint sibling left by an interrupted owner. That sibling name is
reserved: do not put application files there. Do not access the database through
hard-link aliases, externally rename/modify an open file, or reuse inherited
handles after `fork`. Calls require external serialization. The implementation
is tested on macOS; Linux support is not yet exercised in this workspace.

## Formats and migration

The live format is `CORELOG2`, writing `CORECHG6` change records with
primary-key flags, index implementation IDs, named ordered-index definitions,
column defaults and stable row identities. The reader also accepts existing
`CORECHG2/3/4/5` records, including mixed old/new logs. Snapshot exports use `CORESQL5`;
the reader still accepts `CORESQL1/2/3/4`. Older binaries reject the new formats.
Indexes are rebuilt once after recovery, validating final-state uniqueness.
No index pages are written, so opening pays the index construction cost. Earlier
`CORELOG1` live files are rejected rather than interpreted incorrectly. Before
upgrading an existing experimental file, use the previous build to `save()` an
export, then use the current build to `load()` it into memory and copy its tables
through the query/transaction API into a new persistent database. `coresql_admin import-snapshot EXPORT NEW_DATABASE` imports an accepted snapshot
into a new persistent file; applications with custom types use
`Database::restore(export, destination, registry)`. Applications can also inspect
`schema()` and copy rows through the public API when changing their schema. Adding a key requires a new table/database and
validated row insertion; existing unkeyed schemas do not acquire constraints.
Never overwrite or delete the original before verifying the migrated data.

## Tests

`durable_recovery` covers all byte truncation points in a final change record,
real child-process exits at append boundaries, injected failures, corruption,
checkpoint creation/sync/rename/directory-sync interruptions, automatic history
reclamation, lock retention, and the stale-inode opener race.

`chunk_snapshots` checks a single-row write-size bound, old snapshots, cross-chunk
mutation failure, deterministic randomized update/delete/insert/rollback against
an independent row model, empty chunks, large values, checkpoint/reopen cycles,
and snapshot export/import. Existing core and add-on contracts run unchanged.

`primary_keys` checks atomic uniqueness, cross-chunk key swaps, native key
semantics, old snapshots, randomized mutations with rollback/checkpoint/reopen,
legacy snapshot/change decoding, and duplicate-key recovery rejection. The
document application's crash tests exercise a keyed table too.

## Unconditional clear and join snapshots

`erase(table)` without a filter stages empty chunk/index structures and retains
the monotonically increasing chunk-ID sequence. Published/older transaction
snapshots keep the original structures. Existing change-record/checkpoint formats
represent the deletion; they are unchanged. Old unshared data still needs to be
freed, and durable publication still performs storage work. The fast clear path
applies to unconditional erasure only, not arbitrary predicates.

A join reads both tables from the same retained state. Its intermediate row-pair
references never escape the query; returned values own their data. Recovery tests
cover clear interruptions before/after the commit marker, clear/reinsert, retained
snapshots, and checkpoint/reopening.

## Extension index declarations

New change records are `CORECHG6` and snapshot exports are `CORESQL5`, including
column index implementation IDs, named ordered-index definitions, defaults and
stable row identities. Readers retain `CORECHG2/3/4/5` and `CORESQL1/2/3/4`
support. `CORELOG2` framing and the durability protocol are unchanged. Index
structures rebuild from validated rows during open; registered types and index
factories must be present. See [extension ownership](extensions.md).


## Schema evolution and row identity

ADD COLUMN rewrites existing chunks with the new default before atomically
publishing the schema. Log replay accepts additive columns, checks unchanged
existing descriptors, and rejects records that omit rows needing the new width.
Index creation stores its declaration and rebuilds entries on recovery. Both
changes participate in the existing commit-marker and sync protocol.

Each row stores an eight-byte identity that survives compaction, snapshot
export and recovery. It is distinct from the snapshot-local chunk/slot location
used by providers. Snapshot import supplies the logical identity to the mutation
layer before insertion, independently of how that layer packs rows into chunks.
The stored next-identity sequence is restored after all rows have been validated.
Legacy inputs receive identities on import/replay. The encoded
size limit includes these bytes; stored-payload diagnostics continue to count
logical value payloads. Native vacuum repacks rows and rebuilds indexes; a later
checkpoint reclaims obsolete log records. Neither operation changes old snapshots.

Nullable storage adds a column flag and an eight-byte NULL tag per encoded value
in CORESQL5/CORECHG6. Non-NULL values retain their previous payload encoding; NULL
has no payload. Logical payload statistics exclude these tags, while chunk size
accounting includes them. This increases encoded size and may change chunk
boundaries; no performance improvement is claimed. Older snapshots and log records
remain readable, with their columns non-nullable. New records can append to old
logs; no existing column silently becomes nullable. New binaries are required to
read the new formats. Legacy migration and nullable round-trip tests cover both.

## Size limits, schema changes and backup

The encoded-size bound is configured with `CORESQL_MAX_ENCODED_MIB` (default 1024,
range 1–16384). It applies consistently to snapshot encoding/decoding, change
records, recovery and live-state validation. `Database::encoded_size_limit()`
reports bytes. This does not change the in-memory storage architecture.

Dropping a table or renaming a table/column marks the staged schema for full
checkpoint publication at commit. The marker is process-local and follows
savepoint/transaction rollback; it is not stored in the file. Existing crash-safe
checkpoint replacement makes these changes atomic without changing CORECHG6.
Dropping a named index is encoded in its table descriptor in the ordinary log.

`Database::backup(new_path)` writes and synchronizes committed state into a new
native database with exclusive creation. `Database::restore(snapshot, new_path)`
imports supported exports into durable storage. Existing destinations are rejected.
After a failed/interrupted operation, the destination may be incomplete; preserve
the source and retry to a fresh path. See [backup and upgrade guide](../usage.md).

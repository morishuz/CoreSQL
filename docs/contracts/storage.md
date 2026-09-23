# Storage contract and implementation

`Database::open` selects persistent mode; ordinary construction and snapshot
`load` remain non-durable. Ordinary construction can still use private disk scratch
when paging is enabled; `load` currently restores resident rows.

`checkpoint()` explicitly reclaims obsolete persistent records. `checkpoint_async()` starts the same maintenance from a retained committed
snapshot and returns a future; obtain its result to observe failures. Both require
persistent storage. Asynchronous checkpoints additionally require
`OpenOptions::concurrent_reads = true`, certifying that the installed extension
callbacks support concurrent use. Retain the returned future while other work
runs: destruction of a future from `std::async` can wait for its task, so immediately
discarding it can turn the call into a blocking operation. The task retains its
snapshot and owner until completion. `storage_stats()` reports session bytes submitted
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
- An interrupted commit may be present or absent after reopening. A commit
  write/sync error or uncertain checkpoint-publication outcome disables the handle;
  close and reopen to learn the outcome before retrying. Pre-I/O validation/encoding
  errors leave it usable. An asynchronous checkpoint failure before publication
  affects only its unpublished temporary file: its future reports the failure while
  the acknowledged database remains usable.
- Checksums detect accidental corruption, not malicious edits. Process-stop and
  injected-failure tests are not physical power-cut or storage-device certification.
  Full-length torn/corrupt headers or markers are rejected, not repaired.

## Shared chunks

Each table owns ID-sorted, shared blocks of chunk references (up to 64 IDs per block).
A reference retains either resident data or an immutable disk page. A chunk contains
at most 128 rows. Inserts target at most 16 KiB of encoded row data per chunk;
a single large row can exceed that target, and updates can grow an existing chunk.
IDs preserve row order and do not shift when other chunks are deleted.

Beginning a transaction shares tables. A modified table copies the directory of
metadata blocks and detaches only blocks it edits, not all chunk references or rows.
Sparse historical IDs do not allocate intervening blocks. Lookup uses binary search. Updates and deletes copy only affected chunks into a candidate
table; it replaces staged state only after the entire statement succeeds.
Inserts reuse privately owned chunks within a transaction. Unchanged chunks stay
shared with older snapshots. Empty chunks are removed, including from the log.
Statement deletion compacts metadata once; single metadata erasures and inserting
out-of-order IDs during recovery can shift later entries. There is no constant-time
write-cost guarantee.

Small-write temporary storage is proportional to affected chunks plus a table's
chunk-map metadata and encoded change record. It is not a fixed byte budget:
large values, statements touching many chunks, and retained old transactions can
still use substantial memory. Commit compares shared metadata blocks and inspects changed blocks to identify
changed chunks. Encoded-size checks reuse cached totals for unchanged blocks. Direct primary-key equality visits at most one row chunk; other
predicates use eligible primary ranges, ordered/search indexes or scan. The default
resident primary index uses a shared array of 256 hash partitions, copied on write
only where needed. Certified native-i64 keys use sorted contiguous buckets with
binary-search point lookups; other keys use ordered map buckets. Ranges seek
within these same buckets without maintaining a second index. Narrow native-i64
intervals probe only the hash residues they can contain. Bucket copying and vector
insertion add memory/time proportional to affected buckets, potentially large for
skewed keys; non-key updates share the index.

By default rows remain resident. Set `OpenOptions::page_cache_bytes` to a nonzero
byte target to enable paging, for example `Database::open(path, registry,
OpenOptions{.page_cache_bytes = 8 * 1024 * 1024})`. Recovery validates rows one
chunk at a time and writes immutable chunks to private disk backing; commits page
new chunks before durable publication. Queries decode chunks on demand, retaining
a shared ownership pin while borrowed rows or values are in use. Clean decoded
chunks use a least-recently-used cache. Explicit `trim_cache()` releases unpinned
clean chunks; it never invalidates a cursor or another reader's references.

The target covers estimated decoded chunk storage, including row/value container
capacities and string/opaque payloads. It is **not a process RAM cap**. Dirty
transaction chunks, table/chunk metadata, recovery identity sets, indexes, mapped
file pages, query buffers and allocator overhead are additional. Active pins and
a single oversized chunk can exceed the target. Eviction retries on subsequent
page accesses/commits or explicit trim. `cache_stats()` reports target/resident/
pinned/overage bytes, page reads/writes/evictions, and scratch backing size. Large
transactions still need memory proportional to changed chunks and their encoded
commit record; keep ingestion transactions bounded.

Scratch files are created privately and immediately unlinked. They survive through
retained snapshot/page handles and are removed automatically when the last handle
closes. Released extents are reused, coalesced and truncated at the file tail;
retained snapshots intentionally retain their older extents. Temporary storage
must have room for live pages and retained versions in addition to the durable log.
Scratch failures before publication fail the operation without acknowledging a
commit. Scratch pages have checksums; the durable log remains the recovery source.

A scan or eligible streaming indexed join can operate with only its current pages
pinned. Materialized hash joins and borrowed-pair joins retain their input pages
for the operator lifetime and can exceed the cache target; paging does not turn
those algorithms into external joins. Sorting/grouping also need separate query
memory controls. Snapshot export and ordinary change records still materialize
encoded output. Live state and an individual record remain capped at the configured
encoded-size limit (1 GiB by default), using cached chunk sizes for live checks.

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

After a successful commit the main file is bounded by that threshold. Checkpoints
stream their encoding through a 64 KiB buffer and pass large value payloads directly
to the file sink. Additional memory includes one table schema, any chunk pinned by
the pager, and the retained snapshot; there is no full encoded-state buffer. Backup
uses the same streamed writer. Ordinary delta records and snapshot exports still
materialize their encoded output.

`checkpoint_async()` reserves `<database>.checkpoint.background`, captures the
committed snapshot and durable log position under the writer lock, then encodes the
snapshot outside that lock. Commits continue appending to the original log. The
background task copies complete acknowledged records following its captured position
into the replacement. It performs further catch-up passes until at most 256 KiB
remain, then copies that final tail and publishes while excluding commits. Payload,
commit markers, replacement file and directory entries retain their synchronization
requirements. Recovery therefore sees either the old acknowledged log or the
checkpoint plus all subsequent acknowledged commits.

Only one background checkpoint may be pending per owner; a second request reports
`ErrorCode::conflict`. A synchronous checkpoint or destructive schema commit can
replace the durable generation during background encoding. The older task detects
that replacement and discards its obsolete candidate. Recovery removes abandoned
background candidates after interruption. Schema changes retain the synchronous
checkpoint path, and the log-size threshold still forces synchronous maintenance
when necessary; background work never disables the existing history bound. During
a concurrent synchronous replacement, the old log and both candidates can briefly
coexist on disk.

Checkpoint reuse includes paged chunks. Immutable page identities survive cache
eviction; mutations allocate new identities. Reused records are copied from the
retained durable file before decoding pages. Cold chunks that need encoding are
read without replacing query-cache entries. The checkpoint remains a complete,
checksummed state; this does not turn it into an incremental on-disk format.

`storage_stats()` exposes cumulative maintenance durations, catch-up bytes/passes,
reused/encoded chunks, completed background jobs, superseded jobs and automatic
foreground checkpoints. Phase durations include nested I/O; concurrent counter
reads are approximate. These diagnostics help identify maintenance lag and writer
stalls, rather than providing a latency guarantee.

The final copied tail is bounded, but synchronization, directory operations,
scheduling and a single large transaction have no deadline guarantee. A busy
writer can cause extra catch-up passes. Applications should schedule background
maintenance before reaching the automatic bound and measure their actual storage
latency. This is not a hard real-time durability protocol.

Both old and replacement files remain exclusively locked across publication.
An opener checks that its locked inode still matches the pathname, so a paused
opener cannot return an obsolete database after a checkpoint. Recovery removes
unpublished checkpoint siblings left by an interrupted owner. Both sibling names
are reserved: do not put application files there. Do not access the database through
hard-link aliases, externally rename/modify an open file, or reuse inherited
handles after `fork`. Transaction and SQL connection objects require external
serialization; independently captured read snapshots can run concurrently when
`OpenOptions::concurrent_reads` explicitly certifies the installed callbacks and
index providers. Durable commits and checkpoint publication are internally
serialized. Supported platforms and build requirements are listed in
[CONTRIBUTING.md](../../CONTRIBUTING.md).

## Formats and migration

The live format is `CORELOG2`, writing `CORECHG8` change records with
primary-key flags, index implementation IDs, named ordered-index definitions,
column defaults, stable row identities, CHECK expressions, foreign keys and explicit
constraint ownership on unique indexes.
The reader also accepts existing `CORECHG2/3/4/5/6/7` records, including mixed old/new
logs. Snapshot exports use `CORESQL7`; the reader still accepts
`CORESQL1/2/3/4/5/6`. Older binaries reject the new formats. Background checkpointing
uses the same records and framing. An unchanged resident chunk keeps its previous
encoding and is copied into the next checkpoint; modified chunks are encoded again.
The published checkpoint is still a complete record. Recovery maps one record at a time for decoding,
avoiding an additional anonymous buffer the size of a checkpoint. Final-state
constraints and uniqueness are validated during open. Earlier
`CORELOG1` live files are rejected rather than interpreted incorrectly. Before
upgrading an existing experimental file, use the previous build to `save()` an
export, then use the current build to `load()` it into memory and copy its tables
through the query/transaction API into a new persistent database. `coresql_admin import-snapshot EXPORT NEW_DATABASE` imports an accepted snapshot
into a new persistent file; applications with custom types use
`Database::restore(export, destination, registry)`. Applications can also inspect
`schema()` and copy rows through the public API when changing their schema. Adding a key requires a new table/database and
validated row insertion; existing unkeyed schemas do not acquire constraints.
Never overwrite or delete the original before verifying the migrated data.

Constraint-owned unique indexes cannot be dropped independently, regardless of
their display names. `CORESQL7`/`CORECHG8` record this ownership flag. When reading
older formats, unique indexes named `sql.unique.*` are conservatively treated as
constraint-owned: those files cannot distinguish SQL-generated constraints from
core-created unique indexes with the same reserved prefix. New core indexes may
use that prefix without acquiring constraint ownership. SQL still reserves it
for its generated names.

## Unconditional clear and join snapshots

`erase(table)` without a filter stages empty chunk/index structures and retains
the monotonically increasing chunk-ID sequence. Published/older transaction
snapshots keep the original structures. Existing change-record/checkpoint formats
represent the deletion; they are unchanged. Old unshared data still needs to be
freed, and durable publication still performs storage work. The fast clear path
applies to unconditional erasure only, not arbitrary predicates.

A join reads both tables from the same retained state. Its intermediate row-pair
references never escape the query; returned values own their data.

## Extension index declarations

New change records are `CORECHG8` and snapshot exports are `CORESQL7`, including
column index implementation IDs, named ordered-index definitions, defaults and
stable row identities. Readers retain `CORECHG2/3/4/5/6/7` and `CORESQL1/2/3/4/5/6`
support. `CORELOG2` framing and the durability protocol are unchanged. Index
structures generally rebuild from validated rows during open; registered types
and index factories must be present. In paging mode, native INTEGER `core.hash`
primary keys use sorted disk-backed lookup images with binary search and a bounded
4096-key copy-on-write mutation overlay. Overlay compaction merges onto a new
private disk image; old snapshots retain the previous image. Initial construction
uses bounded 4096-key sorted runs and a disk merge, rather than an all-keys heap
array. Range reads seek into the image and merge relevant overlay entries,
including deletions, without decoding unrelated row chunks.
Mapped index pages are managed by the OS, outside the decoded-row cache.

`<database>.native-index` is a disposable persisted lookup-image cache. Its checksum
and the fingerprint of the complete verified durable log must match before reuse.
Every cached key/location is also checked against the recovered rows before use.
An unchanged reopen reuses the image; any commit or checkpoint that changes the log
fingerprint invalidates it. A missing file, stale descriptor or corrupt cache also
rebuilds it. This is a persisted reopen cache, not an incrementally maintained
durable index: the first open after a changed log still builds its lookup image. Publication uses a separate temporary file and atomic
rename; synchronizing this optional cache is unnecessary for database durability.
The suffix and `<database>.native-index.tmp-*` names are reserved. Open owners and
retained snapshots may keep the image mapped: do not externally modify or truncate
it while they are alive. Corruption fallback applies during opening, not during
live external mutation. Backup/restore need only the database file. Custom primary providers, non-INTEGER keys, secondary
and composite indexes retain their existing in-memory implementations and rebuild
costs. Recovery still validates the durable rows and checks cached mappings. A valid image
avoids reconstructing and sorting the index, not those validation passes; measure
reopening time rather than assuming reuse is faster. See [extension ownership](extensions.md).

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
reports bytes. The limit applies to both resident and paged configurations.

Dropping a table or renaming a table/column marks the staged schema for full
checkpoint publication at commit. The marker is process-local and follows
savepoint/transaction rollback; it is not stored in the file. Existing crash-safe
checkpoint replacement makes these changes atomic without changing the record format.
Dropping a named index is encoded in its table descriptor in the ordinary log.

`Database::backup(new_path)` writes and synchronizes committed state into a new
native database with exclusive creation. `Database::restore(snapshot, new_path)`
imports supported exports into durable storage. Existing destinations are rejected.
After a failed/interrupted operation, the destination may be incomplete; preserve
the source and retry to a fresh path. See [backup and upgrade guide](../usage.md).

## Persistent constraints

Current table descriptors persist named CHECK expression trees and foreign-key
declarations. CHECK stores logical scalar operations, literal types/values and
column names, never SQL text or callback addresses. Readers bound nesting and node
counts and validate the resulting expressions. Required type and function providers
must be registered before opening. Foreign keys name explicit columns and tables;
indexes are rebuilt before recovery validates all constraints against the recovered
state. Invalid constraints or rows prevent opening rather than silently disabling
checks. See [formats and migration](#formats-and-migration) before upgrading.
CORELOG2 framing and synchronized commit publication are unchanged.

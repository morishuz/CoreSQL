# Custom backend and SQL type

`coresql_custom_type [FILE]` demonstrates a complete CODE extension through
`TypeAddon` and `SqlTypeAdapter`, including backend-only use, SQL casts/literals,
a mapped function, NULLs and optional reopening. See the
[walkthrough](../docs/extensions/custom-type.md) and [source](custom_type/main.cpp).

# Persistent SQL and administration

`coresql_persistent_sql FILE` demonstrates initialization, parameter binding, an
explicit transaction, error handling and verification after reopening. Run it
again against the same file to verify repeatable initialization.

`coresql_admin backup|restore SOURCE NEW_DESTINATION` creates and verifies a durable
native database. `coresql_admin import-snapshot SNAPSHOT NEW_DESTINATION` imports
an older supported snapshot. Destinations must not exist. See the complete
[handoff and upgrade guide](../docs/usage.md).

# Persistent document index

`coresql_documents` is a small application built on CoreSQL, with no SQL parser,
model runtime, or additional engine subsystem. It stores an integer ID, text,
fixed-dimension float32 embedding, and Unix-microsecond update timestamp.
The dimension is chosen per database; the vector extension itself supports any
runtime dimension, including variable-length columns in other applications.

From the repository root:

```sh
cmake -S . -B build/coresql -DCMAKE_BUILD_TYPE=Release
cmake --build build/coresql -j4
./build/coresql/coresql_documents /tmp/my-documents.core demo
```

The demo requires a new path, inserts three documents, searches by vector and
timestamp, updates one document, deletes another, closes the database, and
verifies persisted changes after reopening. It retains the database for further
use. Its tiny hand-authored vectors illustrate the API; they are not generated
embeddings and do not demonstrate semantic retrieval quality.

To supply your own vectors instead:

```sh
./build/coresql/coresql_documents notes.core init 3
./build/coresql/coresql_documents notes.core put 1 100 '0.1,0.2,0.3' 'Storage notes'
./build/coresql/coresql_documents notes.core put 2 200 '0.8,0.1,0.0' 'Geometry notes'
./build/coresql/coresql_documents notes.core nearest '0.1,0.2,0.3' 10
./build/coresql/coresql_documents notes.core nearest '0.1,0.2,0.3' 10 150
./build/coresql/coresql_documents notes.core list 20 150
./build/coresql/coresql_documents notes.core put 1 300 '0.2,0.2,0.3' 'Revised storage notes'
./build/coresql/coresql_documents notes.core get 1
./build/coresql/coresql_documents notes.core delete 2
```

`SINCE_US` is an exclusive timestamp boundary. List results are newest first;
nearest results use exact squared Euclidean distance, smallest first. Equal keys
retain scan order. A limit of zero returns no rows but still validates query types
and the supplied vector dimension. Timestamps in these commands are deliberately
small examples; actual applications supply Unix microseconds.

Use vectors from one model/embedding space in a database, not just vectors with
the same length. CoreSQL cannot detect semantically incompatible embeddings.
Normalize vectors in application code if your retrieval method requires it.
Embedding generation, text chunking and model identity remain application concerns.
There is no approximate vector index, text search, or model inference here.

## The application boundary

`document_index.hpp` is the complete reusable C++ example. Its constructor reads
`Database::schema()` to verify column names, types, and primary-key declarations, then uses
`vectors::dimensions(type)` to recover the stored dimension. Schema results are
owned copies; modifying them cannot change a database. `Transaction::schema()`
reports its own snapshot and staged tables.

`put()` implements update-or-insert in one transaction. The engine enforces ID
uniqueness and indexes ID equality for get/update/delete. Failed writes leave
committed documents unchanged, including writes made directly through the engine.
Each put/delete commits independently;
callers doing bulk ingestion should use the engine transaction API to batch rows.
Only initialization can create a missing file. Other commands fail on a missing
path rather than creating an empty database accidentally.

This exercise justified schema inspection and dimension inspection, both small
additions. It did not yet justify prepared-query objects, migrations, a general
repository abstraction, or new query operators. Queries still bind on each call.

## Validation and limits

The application test compares randomized mutations, timestamp ordering, and
nearest results against a separate row model across several chunks and repeated
checkpoint/reopen cycles. It also checks malformed dimensions, duplicate-ID
rollback, and child-process interruption during an application commit. A CLI
smoke test runs the walkthrough and verifies parsing, persistence, and errors.

CoreSQL remains experimental. The [storage contract](../docs/contracts/storage.md) describes
single-owner access, live-format compatibility, memory limits, and durability.
The example adds no guarantees beyond that contract. Search scans all documents;
limited sorting reduces comparison work but retains all matching row/key
references and owns computed keys.

### Earlier document files

The example requires `id` to be a primary key. Earlier unkeyed document files
remain readable by the engine but are rejected by this wrapper as an unexpected
schema. To migrate, open the original through `Database::open`, inspect its
`schema()`, create a separate database with the ID key flag, and insert its rows
in one transaction. Duplicate IDs fail that transaction and must be resolved
explicitly. Keep the original until the new database is verified.

### Combined filtering through C++

The example's C++ API accepts `list(limit, since, until)` and
`nearest(embedding, limit, since, until, max_squared_distance)`. All bounds are
optional. The time interval is `(since, until]`; the distance threshold is
inclusive and uses squared L2 units. For example:

```cpp
auto result = docs.nearest(embedding, 10, start_microseconds, end_microseconds, 0.5);
```

Timestamp checks run before distance filtering. Results are ordered by distance
and limited to ten in this example. Non-finite thresholds fail validation, even
with a zero limit. These extra bounds are currently C++ API options; the CLI
syntax remains unchanged. Distance can be evaluated again for sorting/projection;
this change does not add expression-result caching or approximate vector search.

### Document/collection join example

`coresql_collections` is a separate core-only, in-memory example in
`collections.cpp`. It creates documents and keyed collections, joins them with
explicit aliases, and prints each matched document title and collection name.
A document referencing a missing collection is omitted by the inner join; this
is not foreign-key enforcement. This example does not change the persisted
schema of the existing `coresql_documents` application.

## Landmark memory

`coresql_landmark_memory NEW_DIRECTORY` demonstrates background storage and exact
vector retrieval of synthetic robot landmarks, with snapshot, durable-reopen and
backup verification. See the [example contract](../docs/applications/landmark-memory.md).

The landmark example also supplies a bounded background worker in
[`landmark_memory/worker.hpp`](landmark_memory/worker.hpp), with explicit admission,
model/frame filtering, durable acknowledgement and pruning. See its
[application contract](../docs/applications/landmark-memory.md).

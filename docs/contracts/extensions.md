# Types and indexes

Every type uses `TypeAddon`, including the bundled integer, real and string
implementations in `addons/scalars.cpp`. `Registry` installs those by default.
`Registry(false)` starts without types; `install_scalar_types(registry)` adds
them explicitly. There is no native comparison or validation fallback when a
type is absent. Database construction copies the registry and freezes its contract.

## Type contract

| Member | Responsibility |
| --- | --- |
| `id`, `version` | Stable identity; change version when encoding or semantics change |
| `representation` | One of the compact `Value` alternatives; custom identities use immutable `Opaque` bytes |
| `validate_type(parameters)` | Check an instance, such as a vector's fixed dimension |
| `validate_value(parameters, value)` | Check a value entering the engine |
| `equal(parameters, a, b)` | Optional equality |
| `compare(parameters, a, b)` | Optional negative/zero/positive total ordering |
| `hash(parameters, value)` | Optional hash; equal values must hash identically |

Callbacks operate on `const Value&`; integers and doubles stay inline, and strings
stay strings. Extensibility does not force a byte allocation for native numbers. Opaque values
share their immutable type descriptor and bytes together, keeping the `Value`
container compact instead of embedding a full type descriptor in every value.
`EncodedTypeAddon` and `encoded_type(...)` are convenience adapters for types
whose callbacks naturally consume `ByteView`. The adapter produces the same
`TypeAddon` used by scalars. Timestamp implements the value callbacks directly, including hashing. Vectors
remain equality-only, with fixed or variable dimensions; boxes have equality
but no public ordering. Unsupported operations fail during query binding.

The compact native representations have canonical identities because an ordinary
`int64_t` value has no separate type tag. Custom identities use `Opaque(Type,
Bytes)`. The engine frames these bytes; the extension owns their encoding and
validation. Changing arbitrary physical row layouts is not part of this API.
The existing payload/peak counters remain representation-based logical sizes,
not a measurement of all extension/index allocations.

`TypeAddon::canonical_scalar` is false by default. The built-in integer, real
and text providers set it to certify standard scalar validation, equality,
ordering and compatible hashing. Providers that customize these semantics,
including copies of a built-in provider, must clear this flag. Representation
alone does not certify these semantics.

Bound comparisons resolve the registered callback once and trust validated
operands. Public registry comparisons validate arbitrary caller values. Scalar
functions infer result types at binding and return values checked at execution.
A function may additionally supply `prepare(argument_types, result_type)`, returning
an invocation callback specialized for those types. Preparation runs after result
type validation, including for empty input and LIMIT 0; it must depend only on
types and must not perform row evaluation. An empty callback is a type error.
The callback must own any retained type descriptors; its inputs are borrowed.
It must preserve `invoke` semantics. Core still evaluates arguments in order,
applies the existing `accepts_null` rule and validates every returned value.
`invoke` remains required as the direct/default implementation. Functions without
preparation retain their existing behavior; there is no cross-query plan cache.
An equality function may supply `prepare_membership(type, candidates)` to build
an exact membership predicate. Candidates are validated non-NULL values of one
bound type, borrowed only during preparation; returned predicates own retained
state. The factory may return an empty callback to request scalar fallback.
This opt-in certifies equivalent, total, side-effect-free equality and permits
skipping repeated equality/result-validation calls. Core retains empty-set and
NULL truth handling. Preparation is lazy and confined to constant candidate lists
or cached repeatable uncorrelated subquery results. Mixed types and dynamic
candidate expressions retain the normal path. SQL's implementation uses the
canonical-scalar flag for both keys and integer truth results, and only when
affinity leaves the bound key type unchanged. Small lists use direct comparisons;
larger sets use hash lookup.

Callbacks are trusted application code, deterministic, non-reentrant and free
of database mutations. Captured resources must outlive their use, typically via
shared ownership. This is a C++ source interface, not a stable shared-library ABI.

## Index contract

`IndexAddon{id, factory, unique}` registers an independent index implementation.
The factory receives the complete `Type` instance and its `TypeAddon`, and must
reject unsupported types/parameters. Factory arguments are borrowed; retain owned
copies of descriptors/callbacks if the index needs them later. An index belongs to a column, not permanently
to a type. Different algorithms can be registered for the same type.

```cpp
Registry registry;
timestamps::install(registry);
spatial::install(registry);
Database db(registry);
auto tx = db.begin();
tx.create_table("events", {{"at", timestamps::type(), true}});
tx.create_table("objects", {
    {"id", integer(), true},
    {"bounds", spatial::type(), false, spatial::index_id}
});
tx.insert("events", {timestamps::value(1'000'000)});
tx.insert("objects", {std::int64_t{1}, spatial::value({0, 0, 10, 10})});
tx.commit();

Query q{"objects"};
q.search = IndexSearch{"bounds", "overlaps", spatial::value({5, 5, 6, 6})};
auto result = db.query(q);
```

The fourth `Column` field selects an index by ID. With `primary_key=true`, it
selects a unique implementation; empty selects bundled `core.hash`. Otherwise
it selects a secondary search index. This phase permits one primary column and
one selected index per column. A unique implementation promises exact `lookup`
and duplicate rejection consistent with the type's equality. A nonunique index cannot be selected as a primary key.
The same type can use different algorithms in different columns/tables.

| `Index` method | Required behavior |
| --- | --- |
| `clone()` | Return a nonnull independently mutable snapshot; internal COW sharing is encouraged |
| `insert(value, location)` | Add one occurrence; on exception leave logical contents unchanged |
| `erase(value, location)` | Remove one occurrence; may throw after partial private changes |
| `lookup(value)` | Unique indexes: return the exact matching row location or no match |
| `validate_search(operation)` | Reject unsupported operations even for empty tables/LIMIT 0 |
| `search(operation, value)` | Return `IndexResult{rows, exact}` with no false negatives |
| `matches(operation, stored, query)` | Exact residual predicate, independent of candidate selection |

A `RowLocation{chunk, slot}` names a row in the associated snapshot. It is not a
pointer, physical row offset, persistent identity or application handle. The
engine keeps slots stable when deletion compacts a chunk. Providers receive these
locations at insertion/deletion; they do not maintain a separate relocation map.

`IndexResult.exact=true` certifies the requested predicate for every returned row.
The engine may skip that predicate's residual callback. With `exact=false`, it
must recheck each candidate. Both modes must return every matching row; this is
not an approximate-search contract. The engine sorts/deduplicates locations,
checks their validity, preserves scan order and applies remaining WHERE conditions,
ordering, projection and limit. An exact result is not proof of unrelated filters.

`search_type` declares the search operand type; its default is the column type.
Explicit `Query::search` remains query-only and cannot be combined with a join.
Extracted-key `contains` is also available in mutation filters, and independently
of an index. Only a leading eligible predicate can select rows; later predicates
are not reordered ahead of potentially throwing functions.

The bundled hash index uses the type's hash and ordering callbacks with the
existing 256 COW map buckets. Its ordering must agree with equality: compare
returns zero exactly for equal keys, and equal keys have the same hash. Collisions
are allowed. Timestamp keys now use precisely this implementation; no timestamp
branches exist in the engine. Hash results need not be persistent because indexes
are rebuilt from rows.

The spatial add-on is a small Cartesian axis-aligned box type plus an x-interval
index implemented entirely outside the engine. It orders entries internally by
lower x bound, prunes entries beyond the query's upper x bound, and returns x-overlap candidates. The residual predicate tests overlap
in both axes. Touching boundaries count as overlap. It is an architecture proof,
not an R-tree or a geographic coordinate system. Its worst-case search is linear,
and its current clone copies all index entries. Better COW/index algorithms can
replace it without changing transaction or query ownership in the engine.

## Atomicity and persistence

The engine owns snapshots, statement staging, commit conflict checks, durable
publication and recovery. Extensions have no independent commit hook.

For insertion, row allocations finish first. Secondary indexes are privately
cloned/edited, then the primary index performs a strong-guarantee insert, and
no-throw row publication completes the statement. On failure, the private edits
are discarded. Updates/deletes operate on private table/index snapshots; key
updates remove changed old keys before adding new keys, allowing atomic swaps.
Secondary updates use exact representation equality to skip unchanged entries;
a type's coarser semantic equality cannot suppress search-index maintenance.
Unconditional clear replaces indexes with new empty instances. Older transactions
retain their old row and index snapshots. Compacted indexed chunks retain a
small slot-to-position map; untouched chunks and unindexed tables need no per-row
slot metadata. Slot maps and index entries are rebuilt from loaded rows, so this
change adds no persistent row identifier or file-format version.

Index IDs, type identities/parameters, and rows are persisted. Internal index
structures and callback pointers are not persisted. Open rebuilds indexes after
log replay, checking uniqueness before publishing the database. Missing type or
index registrations fail opening. New records use `CORECHG6`; exports use
`CORESQL5`. Previous `CORECHG2/3/4/5` and `CORESQL1/2/3/4` remain readable. New files need
this version to read their index declarations. `CORELOG2` framing is unchanged.

The extension contract is a correctness obligation, not a sandbox: an index that
mutates a shared snapshot, violates strong insertion guarantees or drops matching
candidates can break correctness. Tests exercise the bundled implementations and
injected failures across multiple indexes; arbitrary third-party code must be
tested against the same obligations.

This provides building blocks for vector, spatial, JSON or text indexes. It does
not yet provide text tokenization, JSON path planning, native graph query operators, time-series
compression, vector ANN search, a cost-based optimizer or disk-resident indexes.

## Extracted keys

For equality membership, prefer the smaller `KeyExtractor` interface over
implementing `Index` yourself. Registration supplies the generic postings index,
and `contains(source, extractor_name, key)` works without a physical index too.
See [the JSON experiment](../extensions/json.md) for the contract, persistence naming
rules, examples and current limits. `Index::search_type(operation, source_type)`
can declare a different search operand type; its default validates the operation
and returns the source type, preserving existing providers.


## Aggregate callbacks and typed query NULL

`AggregateFunction` provides type inference and a fresh-state factory. Factories
receive argument types and a borrowed registry; `AggregateState::step` consumes
borrowed argument values and `finish` returns a value checked against the declared
result type. Built-in reductions are registered by the scalar-enabled registry.
NULL query results preserve their declared type; stored columns can opt into nullability.
Scalar functions propagate NULL without calling `invoke` by default. A function
can set `accepts_null` to implement explicit NULL handling.

Named ordered multi-column indexes are currently a native physical facility using
registered type comparators, alongside the existing one-column `IndexAddon`
providers. This does not yet generalize the provider ABI to arbitrary row keys.
See [relational operations](relational.md) for execution, ownership and persistence
limits. All of these source interfaces remain experimental.

## Graph experiment

The [graph add-on](../extensions/graph.md) implements custom edge/path values,
endpoint extraction and bounded traversal using only public APIs. Generic scoped
savepoints now let graph views join caller-owned transactions. It validates
composition with snapshots, mutations and recovery;
it does not establish competitive graph execution. Materialized adjacency queries and facade-only endpoint constraints remain
interface boundaries. Graph mutations use savepoints to preserve earlier work on
failure; no graph-specific engine hooks or general constraint framework were added.

## Nullable stored values

`Column::nullable` defaults to false for source compatibility. Nullable columns
accept typed `Null(column.type)` through the same validator in mutations, integrity
checks and recovery. Registry scalar callbacks continue to receive NULL only when
they opt in; custom values and nullable custom columns survive persistence.
Primary keys cannot be nullable. Inline extension indexes on nullable columns
are currently rejected explicitly, rather than passing unexpected NULLs to index
callbacks. Named ordered indexes handle NULL keys; unique indexes permit multiple
keys containing NULL, while DISTINCT continues to treat NULLs as equal.

## Index integrity validation

`Index::validate(span<const IndexEntry>)` receives the complete source values and
snapshot-local row locations for its column. It must check omissions, extra
entries, wrong keys and locations without mutating the index or retaining the
input. A mismatch throws `ErrorCode::state`. The default throws
`ErrorCode::unsupported`; custom providers must implement validation before
`Transaction::integrity_check()` can certify their tables.

Bundled hash, extracted-key and spatial indexes implement this contract. Extracted
keys are checked with their registered equality semantics, including duplicate
emissions and rows that emit no keys. Validation is an explicit diagnostic and
may allocate temporary state proportional to the index; it does not run on every
query or commit. Providers remain trusted native code: this checks stored index
contents, not arbitrary correctness of provider algorithms.

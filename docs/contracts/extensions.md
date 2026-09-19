# Types and indexes

Every type uses `TypeAddon`, including the bundled integer, real and string
implementations in `addons/scalars/type.cpp`. `Registry` installs those by default.
`Registry(false)` starts without types; `install_scalar_types(registry)` adds
them explicitly. There is no native comparison or validation fallback when a
type is absent. Database construction copies the registry and freezes its contract.

## Type contract

| Member | Responsibility |
| --- | --- |
| `id`, `version` | Stable identity; change version when encoding or semantics change |
| `layout` | Closed cell shape: `i64`, `f64`, `text`, or `bytes`. Identity is open; a type chooses one layout |
| `validate_type(parameters)` | Check an instance, such as a vector's fixed dimension |
| `validate_value(parameters, value)` | Check a value entering the engine |
| `equal(parameters, a, b)` | Optional equality |
| `compare(parameters, a, b)` | Optional negative/zero/positive total ordering |
| `hash(parameters, value)` | Optional hash; equal values must hash identically |
| `native_ops` | Opt-in: equal/compare/hash are the layout's native C operators |

Callbacks operate on `const Value&`; integers and doubles stay inline, and strings
stay strings. Extensibility does not force a byte allocation for native numbers. Opaque values
share their immutable type descriptor and bytes together, keeping the `Value`
container compact instead of embedding a full type descriptor in every value.
`EncodedTypeAddon` and `encoded_type(...)` are convenience adapters for types
whose callbacks naturally consume `ByteView`. The adapter produces the same
`TypeAddon` used by scalars. DATE and TIMESTAMP use compact `i64` cells with
`native_ops`; DECIMAL uses compact `i128`. Timestamp implements the value callbacks directly, including hashing. Vectors
remain equality-only, with fixed or variable dimensions; boxes have equality
but no public ordering. Unsupported operations fail during query binding.

`i64`, `f64` and `text` have default identities (`core.integer`, `core.real`,
`core.text`) because an ordinary `int64_t` value has no separate type tag. Those
identities stay untagged. Other types may choose `Layout::i64` or `Layout::i128`
and store a tagged `Compact` cell; `f64` and `text` remain reserved for their
default identities. Heap payloads use `Opaque` bytes (`Layout::bytes`). The engine
frames compact and opaque payloads; the extension owns encoding and validation.
Changing arbitrary physical row layouts is not part of this API. Execution
specializes on `layout` plus `native_ops`, not on type identity. The existing
payload/peak counters remain layout-based logical sizes, not a measurement of
all extension/index allocations.

`TypeAddon::native_ops` is false by default. The built-in integer, real and text
providers set it to certify that equality, ordering and hashing are the layout's
native C operators. Providers that customize those semantics, including copies of
a built-in provider, must clear this flag. Layout alone does not certify these
semantics.

Bound comparisons resolve the registered callback once and trust validated
operands. Public registry comparisons validate arbitrary caller values. Scalar
functions infer result types at binding. Bound evaluation skips per-row result
validation when the returned cell matches a `native_ops` result type; other
results, NULLs, inserts and recovery still validate.
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
candidate expressions retain the normal path. SQL's implementation uses `native_ops` for both keys and integer
truth results, and only when affinity leaves the bound key type unchanged. Small
lists use direct comparisons; larger sets use hash lookup.

Callbacks are trusted application code, deterministic, non-reentrant and free
of database mutations. Captured resources must outlive their use, typically via
shared ownership. This is a C++ source interface, not a stable shared-library ABI.

## SQL type adapters

`coresql/sql_types.hpp` adds an optional SQL policy layer over `TypeAddon`.
DATE, DECIMAL and VECTOR use this interface: each has `type.cpp` for its independent
backend implementation and `sql.cpp` for its SQL adapter under `addons/<type>/`.
The SQL library compiles the adapters; backend-only users still link the domain
library without SQL. Integer, real and text use the same adapter interface, with
backend registration in `addons/scalars/type.cpp` and SQL policy in
`addons/scalars/sql.cpp`. Their adapters own aliases, casts, storage conversion,
numeric-text parsing and affinity callbacks. The explicitly named shared scalar
policy in the same module installs arithmetic and selects mixed-scalar operations
and result types. SQL installs it once, independently of the individual adapters;
REAL-only SQL composition does not depend on the INTEGER adapter. Adapter rules
are consulted first, and shared scalar rules are the fallback.

Start with the defaults and register an application adapter before installation:

```cpp
auto types = sql::default_type_adapters();
types.add(my_sql_adapter()); // returns sql::SqlTypeAdapter
Registry registry;
sql::install(registry, types);
Database db(registry);
sql::Connection connection(db, registry, types);
sql::Statement statement("SELECT CAST(? AS MYTYPE)", types);
```

Use the same configuration for installation, connections, statements and
`prepare_script`. Copies share an immutable snapshot; adding an adapter creates
a new snapshot without changing existing connections or statements. Executing a
statement prepared with a different snapshot is rejected. `TypeAdapters{}` supplies
scalar SQL adapters; `TypeAdapters(false)` starts
without type adapters for explicit composition using `integer_adapter()`,
`real_adapter()` and `text_adapter()`. `default_type_adapters()` also supplies
DATE, DECIMAL and VECTOR. Scalar SQL adapters expect the registry's normal backend
scalar types/functions; they do not reinstall them. Domain installation invokes
each adapter's backend installer, so do not also install those domains separately.
Shared SQL scalar expression rules remain available even with an empty adapter set.
For a complete implementation, see [adding your own type](../extensions/custom-type.md).

An adapter identifies one type family by ID/version. Required callbacks declare
its parameterized type, install its backend support, recognize conversions and
perform them. Names are case-insensitive SQL aliases. `SqlDeclaration` provides
the normalized alias, unsigned 64-bit parameters and whether it is a CAST target;
this preserves rules such as a positive optional length on VARCHAR declarations
but no length on VARCHAR casts. Parsing punctuation stays generic. Optional
callbacks provide typed string
literals, operator/function lowering, and common result types for CASE/UNION.
Operator lowering selects registered backend functions; it does not replace the
parser or execution engine. Numeric-literal hooks preserve exact decimal spelling.
The parser checks declared/literal type identities and conversion results must
match the requested target type.

Duplicate families or aliases are rejected without changing the configuration.
Multiple adapters claiming the same conversion, operation or common-type decision
produce a type error instead of depending on registration order. Adapters should
return no rule for types/operations they do not own. Operation names include SQL
operator spellings, `negate`, `between`, function names and `sql.extract.year`.
For function dispatch, a type with an empty ID denotes an untyped NULL literal;
CAST/parameter NULLs retain their declared type. Repeatability checks use the
same operand-type dispatch as SQL lowering.
An overloaded operation must check operand types before claiming the operation.
DATE supplies the default context for `EXTRACT(YEAR FROM NULL)`; another adapter
can handle typed timestamps without conflicting with DATE. Multiple claims for
an untyped NULL are ambiguous too; use an explicit CAST to disambiguate.

NULL propagation, lazy branches, binding, transactions and persistence remain
shared SQL/core responsibilities. Callbacks are trusted, deterministic C++ code
and must own retained resources. Optimization promises (`repeatable` and
`reorder_comparisons`) default to false; enable them only when repeated evaluation
is equivalent and comparison reordering cannot introduce errors. SQL resolves
operation repeatability using operand types in the current query scope, including
correlated subqueries; analysis does not add execution captures. The registry's
conversion helpers resolve registered rules; normal SQL handles identity and
NULL before dispatching conversions. `try_convert` resolves and executes a rule
in one pass, returning no value only when no adapter claims the conversion;
ambiguity, callback errors and wrong result types still throw. Constant INSERT
values use this same conversion policy without a temporary SQL conversion call.
`SqlAffinity` identifies SQL's existing
native scalar comparison categories; `apply_affinity` implements their value
conversion. Comparisons, BETWEEN, membership and simple CASE retain the actual
source type when selecting that callback, including REAL rather than substituting
INTEGER. The shared lowerer retains SQL-wide affinity precedence (numeric before
text, then the left operand for equal priority).

Affinity applies to native SQL scalars, not arbitrary domain types. It must leave
already-affiliated values unchanged: the adapter's own native type, and both
INTEGER and REAL for numeric affinity. This preserves same-type comparisons and
membership optimizations. Custom equality/collation belongs to `TypeAddon`, not
the affinity callback.

The dynamic `sql.value` representation and its native scalar restriction,
`CAST AS NUMERIC` syntax, numeric/text storage-class comparison, generic grammar,
boolean/NULL/lazy evaluation, string-function argument adaptation, binding,
transactions and persistence remain shared. Optimizations still recognize
canonical native scalars. These are SQL/core responsibilities, not a promise that
every language rule or representation is replaceable through an adapter.
No callback or SQL adapter configuration is
serialized: register the same domain identities before reopening a database.

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

The bundled hash index uses 256 copy-on-write buckets. Native i64 keys
(`layout == i64` and `native_ops`) use hash tables of the payload; other keys
use the type's hash and ordering callbacks with ordered maps. Ordering must
agree with equality: compare returns zero exactly for equal keys, and equal keys
have the same hash. Collisions are allowed. Timestamp keys use precisely this
implementation; no timestamp
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
savepoints let graph views join caller-owned transactions. It validates
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

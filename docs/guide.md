# CoreSQL guide

CoreSQL is an experimental C++20 embedded database. Its typed relational API works
without SQL; the optional SQL frontend uses the same core. Domain add-ons register
types, functions, aggregates and indexes. Storage and transactions stay in the core.

## Start here

- [Build and run examples](../README.md#try-it)
- [Install, persist data, back up and upgrade](usage.md)
- [Executable examples](../examples/README.md)
- [Contributor builds, sanitizers and package checks](../CONTRIBUTING.md)

## C++ example

```cpp
#include "coresql/core.hpp"
using namespace coresql;

Registry registry;
Database db(registry);
auto tx = db.begin();
tx.create_table("notes", {{"id", integer(), true}, {"body", text()}});
tx.insert("notes", {std::int64_t{1}, std::string("Hello")});
tx.commit();
auto rows = db.query(Query{"notes", {column("body")}});
```

The typed C++ API requires explicit primary keys. SQL can generate integer primary
keys and return inserted columns through `INSERT ... RETURNING`.

For a persistent database use `Database::open(path, registry)`. Install all required
add-ons in the registry before opening a file that uses their types or indexes.
A persistent file has one owner; all calls require external serialization. Live
rows and indexes remain in RAM. Read the [storage contract](contracts/storage.md)
before relying on persistence, and use [verified backups](usage.md#backup-and-restore).

## Module map

| CMake package target | Responsibility |
| --- | --- |
| `CoreSQL::core` | Registry, schema, queries, transactions and persistence |
| `CoreSQL::sql` | SQL parsing, parameters and execution through the core API |
| `CoreSQL::json` | JSON values and typed JSON Pointer key extraction |
| `CoreSQL::graph` | Directed graph facade and bounded traversal |
| `CoreSQL::spatial` | Cartesian boxes and an interval search index |
| `CoreSQL::vector` | Float32 vectors and squared L2 distance |
| `CoreSQL::decimal` | Checked fixed-point arithmetic and aggregates |
| `CoreSQL::date` | Gregorian dates, ISO parsing and chronological ordering |
| `CoreSQL::timestamp` | Unix microseconds and checked subtraction |

Use `find_package(CoreSQL 0.1.0 EXACT REQUIRED COMPONENTS sql)` and link the targets
you need. Pin the source revision as well: an unreleased version number does not
identify a particular implementation. See [installation](usage.md#build-install-and-try).

## Behavior and extension contracts

Each contract is the maintained reference for its subject:

- [SQL](contracts/sql.md): syntax, coercion, generated IDs and unsupported forms.
- [Relational API](contracts/relational.md): queries, NULLs, joins, aggregation,
  schema operations and callback evaluation rules.
- [Storage](contracts/storage.md): atomicity, durability, recovery, formats,
  ownership and resource limits.
- [Extensions](contracts/extensions.md): type/index registration, callback
  consistency, lifetime and transaction requirements.
- [JSON](extensions/json.md) and [graph](extensions/graph.md): examples,
  persistence rules and domain-specific limitations.

Extensions are trusted native code. They do not replace the transaction manager,
storage engine or query operators. Returned query rows are owned, but query
intermediates and retained snapshots can consume substantial memory. The encoded
size limit is not a total RAM budget.

## Scoped savepoints

`tx.savepoint()` returns a move-only guard. Destruction or `rollback()` restores
all transaction contents and the dirty flag to that point; `release()` keeps the
changes staged. Neither operation commits or writes to storage. Rollback also
restores schemas, row identities and indexes through the same copy-on-write
snapshots as ordinary transactions. Creation copies the table map, not row data;
retained savepoints can increase memory use and subsequent copy-on-write costs.

```cpp
auto tx = db.begin();
// Earlier application writes remain intact if this operation throws.
{
    auto scope = tx.savepoint();
    tx.update("documents", {{"title", literal(std::string("Revised"))}});
    tx.insert("audit", {std::string("document revised")});
    scope.release();
}
tx.commit();
```

Scopes nest. Releasing an inner scope keeps its changes inside the outer scope.
Rolling back or releasing an outer scope closes all descendants; their surviving
guards become inert. Repeated operations on a closed or moved-from guard are
no-ops. A transaction cannot commit with open scopes (`ErrorCode::state`);
whole-transaction rollback/destruction closes every scope. Guards may outlive a
transaction, and follow its state when it moves. Move-assigning a guard first
rolls back its existing scope, including any descendants. Rollback is noexcept
and does not allocate. Savepoints do not change commit conflict or I/O-failure
semantics, and cannot recover a poisoned persistent owner.

Extensions can borrow a caller transaction and use these guards to make their
multi-statement operations atomic; see [graph composition](extensions/graph.md).

## Values and diagnostics

`vectors::type()` accepts variable-length float32 vectors; `vectors::type(768)`
requires exactly 768 elements. Construct fixed-size values with the same dimension
parameter. The typed C++ API has no implicit conversion between type instances; the
[SQL adapter](contracts/sql.md#vector-sql-adapter) validates and converts dimensions
on assignment and CAST. NaN and
infinity are rejected; positive and negative zero compare equal. Vectors have no
ordering callback; sort their squared L2 distance instead. Distances use double
accumulation and reject mismatched lengths.

Timestamps are signed Unix microseconds with chronological ordering and checked
subtraction. They have no timezone/calendar parser. Encodings are little-endian;
callback pointers and native object layouts are not persisted.

`db.schema()` and `tx.schema()` return owned schema descriptions; editing them
does not mutate the database. A transaction sees its staged schema changes.
`stats()` reports logical stored payload, not total allocations: it excludes
indexes, containers, retained snapshots and query buffers. The peak is session-local.
`storage_stats()` counts submitted file bytes and checkpoints, not physical device
write amplification. Measure process memory separately.

## Development

[Architecture](architecture/direction.md) explains the boundaries to preserve;
the [SQL backlog](architecture/sql-roadmap.md) records remaining candidates.
Use [benchmarks](../benchmarks/README.md) for reproduction and measured evidence,
and [release readiness](release-readiness.md) for validation gates.

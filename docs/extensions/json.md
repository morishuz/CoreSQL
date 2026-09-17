# JSON key extraction

CoreSQL uses the **same membership predicate with or without an index**.
JSON parsing and property selection live in an optional `coresql_json` library;
a reusable postings index handles extracted keys. No JSON identifiers or path
syntax appear in the engine. The core still owns transaction publication,
snapshot visibility, rollback and persistence.

## API

```cpp
#include "coresql/json.hpp"
using namespace coresql;

Registry registry;
json::install(registry);
registry.add(json::property("documents.status.v1", "/status"));
Database db(registry);
auto tx = db.begin();
tx.create_table("documents", {
    {"id", integer(), true},
    {"body", json::type(), false, "documents.status.v1"}
});
tx.insert("documents", {std::int64_t{1}, json::value(R"({"status":"ready"})")});
tx.commit();
auto result = db.query({"documents", {column("id")},
    contains(column("body"), "documents.status.v1", literal(std::string("ready")))});
```

Leave the last string out of the `body` column to scan instead. The query stays
identical. `contains` also composes with AND/OR/NOT, works in update/delete
conditions and accepts expressions as operands. Direct column/literal membership
uses a matching index when it is the first condition (possibly inside leading
ANDs). Other shapes scan; joins evaluate the predicate after their join condition.
Later predicates are not moved ahead of earlier functions that might throw.

Build and run `coresql_json_demo` for the complete example. Link `coresql_json`
in addition to the core; the core itself has no JSON parser dependency.

## What an extension implements

```cpp
struct KeyExtractor {
    std::string name;
    Type source_type, key_type;
    std::function<std::vector<Value>(const Value&)> extract;
};
```

The callback returns **zero or more typed keys**. The JSON adapter emits at most
one key; a test-only character extractor emits many, including duplicates.
Registering an extractor also registers a non-unique index under its name.
The index needs equality and hashing on the **key** type, not ordering and not
equality on the source document. Hash collisions are resolved by equality.
A test exercises a custom key type with a constant hash and no ordering callback.

Extractors must be deterministic, side-effect-free and defined for every valid
value of their declared source type. Missing domain values return no keys.
Every emitted key is checked against the registered key type and validator.
Memory allocation and callback failures abort staged writes. These are trusted
native callbacks, not sandboxed code; an extension violating its semantic
contract can invalidate query results, just as an inconsistent comparator can.

The generic index maps keys to sparse 128-slot masks naming rows. Duplicate
emissions set the same bit, so removing one row cannot remove a neighbour's
membership. Index snapshots share buckets and postings. An edit copies affected
containers privately before publication. JSON supplies no index storage, clone
logic or transaction callbacks. Exact membership results bypass JSON parsing
at query time; unindexed queries still extract and compare keys.

The existing `IndexAddon` API remains available for algorithms that cannot use
this structure. `Index::search_type` lets their explicit search operand differ
from the source column type; the default retains the previous same-type behavior.

## JSON semantics

Documents retain their original UTF-8 bytes. The parser follows the JSON grammar
in [RFC 8259](https://www.rfc-editor.org/rfc/rfc8259), with an explicit stricter
policy: reject duplicate decoded object keys, invalid UTF-8/unpaired surrogates,
and nesting beyond depth 64. It does not normalize Unicode. There is no whole
JSON document equality or ordering callback in this experiment.

Property selection uses [RFC 6901 JSON Pointer](https://www.rfc-editor.org/rfc/rfc6901):

| Pointer | Meaning |
| --- | --- |
| `/status` | Object property `status` |
| `/nested/name` | Nested property |
| `/items/0/name` | Property of the first array element |
| `/a~1b/~0key` | Keys `a/b`, then `~key` |
| Empty string | The whole document |

This is the string form of JSON Pointer, not its URI fragment form, JSONPath or
wildcard traversal. Missing targets, array indices with leading zeroes and `-`
produce no key. Object properties named `01` are ordinary string keys.

The optional third argument to `json::property` selects `text()` (default),
`integer()` or `real()`:

- Text emits decoded JSON strings only.
- Integer emits integer number lexemes representable exactly as int64. `1.0`
  and `1e0` do not emit integer keys; `9223372036854775808` emits none.
- Real converts JSON numbers to finite binary64. Rounding follows that
  representation; conversions reported out of range emit no key.
- Missing, null, booleans, objects, arrays and mismatching scalar kinds emit none.

This is typed membership, not SQL NULL/coercion behavior. `NOT contains(...)`
therefore matches a missing property. JSON null remains valid document content;
it does not introduce a new core value alternative.

## Persistence and limits

The schema persists the existing index name and document bytes; indexes are
rebuilt on load/recovery as before. Reopen with the same registered extractor
configuration. **The extractor name identifies its complete semantics**, including
path, key type and version. Use a new name when changing those semantics. The
engine cannot detect code/configuration changes under an unchanged name. No new
file format or JSON-specific persistence code was necessary.

This experiment retains one index per column. Multiple property indexes,
compound extracted keys, tokenization/ranking, arbitrary path queries and a
cost-based planner are not implemented. It is not a JSON database product.

The index returns exact **row locations**, and queries visit those rows directly.
Providers that return only candidates must request a recheck. Slot locations remain
valid across compaction within a snapshot through an optional map in the chunk;
they are regenerated when persisted data is loaded. There is no new file format.

Unindexed extraction still parses the document, including validation, and allocates
decoded strings/object key sets. No DOM cache or binary JSON format is introduced.
Frequent keys still make copy-on-write posting lists more expensive to update;
row-level results do not solve all write or memory costs.

## What this establishes

A small shared extraction/index layer is sufficient for typed property lookup
without making JSON implement a storage engine. The independent multi-key test
provides evidence that the layer is reusable beyond this one adapter. It does
not establish suitability for approximate vector search, graph traversal or
full-text ranking; those require different query/index capabilities.


# Graph extension

`coresql_graph` is a small directed graph implemented through public CoreSQL
headers and generic scoped transaction savepoints. The engine contains no
graph-specific behavior. Link this optional library and include
`coresql/graph.hpp`.
The source interface and encodings remain experimental.

## Try it

```sh
cmake -S . -B build/coresql
cmake --build build/coresql --target coresql_graph_demo coresql_graph_test
./build/coresql/coresql_graph_demo
ctest --test-dir build/coresql -R graph --output-on-failure
```

```cpp
Registry registry;
graph::install(registry);
Database db(registry);
graph::Graph network("social");
network.create(db);
auto tx = network.begin(db);
tx.add_node(1, "Alice");
tx.add_node(2, "Bob");
tx.add_node(3, "Carol");
tx.add_edge(10, {1, 2, "friend", 1});
tx.add_edge(11, {2, 3, "friend", 1});
tx.commit();
auto read = network.begin(db);
auto route = read.shortest_hop_path(1, 3, 4);
// route->nodes == {1, 2, 3}; route->edges == {10, 11}
Value stored_path = graph::value(*route);
```

## Sharing an application transaction

```cpp
auto app = db.begin();
app.create_table("audit", {{"message", text()}});
auto view = network.bind(app);
view.add_node(4, "Dana");
view.add_edge(12, {3, 4, "friend", 1});
app.insert("audit", {std::string("added Dana")});
app.commit(); // Publishes the graph and audit changes together.
```

`Graph::bind(Transaction&)` returns a borrowed `View` without commit/rollback
methods. The transaction must outlive the view, remain at the same address and
retain the graph schema; rebind after replacing the transaction or rolling back
graph creation. `Graph::create(Transaction&)` can also create the graph inside
the caller's transaction, atomically with other DDL and writes.
`Graph::begin(Database&)` still returns an owning `Session` for convenience.

Every graph mutation uses a [scoped savepoint](../guide.md#scoped-savepoints).
An error restores only that operation, including partial cascades, preserving
earlier application and graph writes. The caller can correct the input and
continue, or roll back the entire transaction. An outer application savepoint
can group multiple graph operations into one unit. Core commit conflicts and I/O
errors propagate; commit failure no longer automatically aborts an owning session.
Read failures also leave transaction contents unchanged.

## What this validates

| Facility | Graph use |
| --- | --- |
| Custom encoded types | Immutable `graph.edge` and variable-length `graph.path` values |
| Registered scalar functions | Edge source, target, label and weight; path hop count |
| Extracted keys | One edge yields outgoing-source and incoming-target keys |
| Shared postings index | Custom endpoint keys provide equality and hashing without ordering |
| Ordinary query composition | Leading endpoint membership followed by label/weight filters |
| Transactions | Endpoint checks, edge updates, cascading node deletion, rollback and conflict detection |
| Snapshots | Traversal reads a consistent transaction snapshot while later commits change the graph |
| Persistence | Custom values survive export/import, durable reopen and checkpoint; indexes rebuild through the registry |

Nodes have positive integer IDs and text labels. Edges have independent positive
IDs, directed endpoints, a label and a finite nonnegative weight. Parallel edges,
cycles, loops and disconnected vertices are supported. Incoming, outgoing and
bidirectional traversal are available; bidirectional neighbor results contain a
self-loop once and preserve distinct parallel edges.

Breadth-first traversal returns each reachable vertex once, including the start
at depth zero. Shortest paths minimize hop count, **not weight**; weight is only
an optional edge filter. Neighbor ordering by edge ID makes path ties deterministic.
Missing endpoints are errors; existing but disconnected endpoints yield `nullopt`.
Hop, visited-vertex and examined-edge limits bound traversal work. Adjacency is
materialized before checking the edge budget, so these limits are not memory caps.

The custom encodings use checked little-endian fields and reject truncation,
trailing bytes, invalid IDs, invalid weights and inconsistent path lengths before
allocating path arrays. Negative zero weights normalize to positive zero.
Equality compares canonical value encodings; edge/path values have no ordering.
A path validates its shape and IDs, not its continued existence in a live graph.
It may be stored in an ordinary table even after its original edges are deleted.
The same registry must be installed before loading a database containing these types.

## Boundaries exposed

1. **Multi-step composition now uses generic savepoints.** The original need to
   own the entire transaction is resolved by borrowed views and operation-scoped
   rollback. Savepoints copy the table map and retain shared state, so this
   convenience has metadata and copy-on-write costs; it is not a performance claim.
2. **Traversal runs outside the query executor.** Each expanded vertex performs
   a materialized adjacency query. Bidirectional reads merge two queries, and
   cascading deletion currently uses an OR predicate with scan execution. There
   is no graph-specific planner or streaming traversal operator. Batched/streaming
   index access and a table-producing operator interface with snapshot context
   are candidates to investigate when profiling motivates them.
3. **Graph constraints belong to the facade.** Graph writes check that endpoints
   exist and cascade incident edges on node deletion. Direct core writes to the
   underlying tables bypass those checks; `View::check_integrity()` (also available on `Session`) detects
   dangling edges. Supporting arbitrary writers would need a generic constraint
   mechanism or controlled table access. The extension interface is trusted code,
   not a sandbox.

Database/session calls still require external serialization. Separate transaction
snapshots are supported; conflicting commits cannot publish stale endpoint checks.
Stable graph IDs are explicit primary keys, never snapshot-local row locations.

This establishes that richer types, indexing and a useful graph facade compose
without engine specialization. It does **not** establish a competitive graph
engine or cover everything an extension might need. There is no Cypher/GQL layer,
arbitrary property map, weighted shortest path, graph-specific storage layout or
performance comparison. Keep the API experimental and discuss the boundaries
above before expanding it; performance tuning is deliberately deferred.

## Validation

`graph_test.cpp` compares both indexed and scan execution against an independent
in-memory traversal model over a deterministic random graph, across directions,
filters and hop limits. It also exercises cycles, parallel edges, loops,
disconnection, a 512-edge hub, path continuity, traversal budgets, malformed
encodings, mutation rollback, cascading deletion, snapshot isolation, commit
conflicts, missing registration, export/import, log recovery and checkpointing.
An explicit raw-write test demonstrates the constraint boundary rather than
silently assuming core writes preserve graph invariants.

The follow-up tests also share ordinary and graph writes in one transaction,
roll back graph DDL, and inject an index failure during the second cascade
mutation to verify edge restoration and preservation of earlier application writes.
`savepoint_test.cpp` covers nesting, release/rollback, schema and index restoration,
guard/transaction moves and lifetimes, clean-state restoration without log writes,
snapshot isolation, conflict detection, durable reopen and checkpoint.

# Add-on source map

Every provider lives in its own directory. `type.cpp` contains a domain's backend
implementation, including its registered functions and any associated index.
`sql.cpp`, when present, contains its SQL adapter or scalar SQL policy.
Broader providers use descriptive names such as `graph.cpp` and `index.cpp`.

| Directory | Backend source | SQL source | Library |
| --- | --- | --- | --- |
| `scalars/` | `type.cpp` | `sql.cpp` | Backend in `CoreSQL::core`; SQL in `CoreSQL::sql` |
| `aggregates/` | `aggregates.cpp` | — | `CoreSQL::core` |
| `relational/` | `functions.cpp` | — | `CoreSQL::core` |
| `hash_index/` | `index.cpp` | — | `CoreSQL::core` |
| `date/` | `type.cpp` | `sql.cpp` | `CoreSQL::date`; adapter in `CoreSQL::sql` |
| `decimal/` | `type.cpp` | `sql.cpp` | `CoreSQL::decimal`; adapter in `CoreSQL::sql` |
| `blob/` | `type.cpp` | `sql.cpp` | `CoreSQL::blob`; adapter in `CoreSQL::sql` |
| `vector/` | `type.cpp` | `sql.cpp` | `CoreSQL::vector`; adapter in `CoreSQL::sql` |
| `timestamp/` | `type.cpp` | — | `CoreSQL::timestamp` |
| `json/` | `type.cpp` | — | `CoreSQL::json` |
| `spatial/` | `type.cpp` | — | `CoreSQL::spatial` |
| `graph/` | `graph.cpp` | — | `CoreSQL::graph` |

A directory does not imply a separately linked library or a SQL adapter. The
scalar, aggregate, relational-function and hash-index providers use extension
interfaces but ship in the core library by default. Graph provides a broader
facade in addition to its domain values. No placeholder SQL files are needed.

The surrounding layout stays separate:

- `src/`: engine internals, queries, storage, schema and transactions.
- `sql/`: shared SQL grammar, lowering, evaluation support and connections.
- `include/coresql/`: public headers; existing include paths remain unchanged.
- `examples/custom_type/`: a complete backend-plus-SQL extension example.

See the [extension contract](../docs/contracts/extensions.md) and
[custom-type walkthrough](../docs/extensions/custom-type.md).

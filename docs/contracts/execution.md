# Controlled reads and streaming

`Database::query(query, options)`, `ReadSnapshot::query(query, options)` and
`Transaction::query(query, options)` accept `QueryOptions`. SQL applications use
`Connection::query(statement, parameters, options)` or a snapshot's
`ReadConnection`; these query methods reject mutations and transaction commands.

```cpp
std::stop_source stop;
coresql::QueryOptions options;
options.cancellation = stop.get_token();
options.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
options.max_work = 1000000;
options.max_buffer_bytes = 8 * 1024 * 1024;
auto result = connection.query(statement, parameters, options);
```

Another thread may call `stop.request_stop()`. Cancellation and an expired deadline
throw `ErrorCode::cancelled`; exhausted work or buffer allowance throws
`ErrorCode::resource`. Failed reads leave the transaction usable. Nested execution
and nested public queries also consume their parent's allowance. Independent
materialized calls start fresh; a cursor retains its work count across all fetches.

Work units count engine operations such as visits, expression evaluations and sort
comparisons. Their meaning can change with implementation; they are not CPU cycles
or row counts. Cancellation/deadlines are checked at entry, exit and periodically
during execution. SQL parsing/lowering, an individual native callback, allocation,
index-provider operation or system call cannot be preempted. These controls do not
cancel durable commits or promise hard real-time deadlines.

## Resumable cursors

`Database::cursor`, `ReadSnapshot::cursor` and `Transaction::cursor` return a
move-only `QueryCursor`. SQL's `Connection::cursor` and `ReadConnection::cursor`
return `sql::Cursor`, with the same stepping methods and unpacked SQL values.

```cpp
auto cursor = connection.cursor(statement, parameters, options);
while (auto row = cursor.next()) {
    // row owns its values; another fetch does not invalidate them.
    consume(*row);
}
```

- `next()` returns one owned row, or an empty optional at exhaustion.
- `fetch(max_rows)` returns an owned result containing up to that many rows.
  `fetch(0)` does not advance. Each batch is subject to the buffer allowance.
- `types()` on the core cursor and `columns()` on the SQL cursor are available
  before reading, including for empty results.
- `close()` releases the input snapshot. Exhaustion, an execution failure and
  destruction also release it. Calling `next()` after closure returns empty.
- `stats()` reports produced rows (including rows built for a failed batch), cumulative
  work, peak accounted buffer bytes, elapsed time since creation, closure, and the
  logical payload of the retained
  snapshot. Snapshot payload is shared with other readers and is not a measurement
  of uniquely retained memory, page-cache occupancy or process RSS.

These are resumable executors: creation binds the shape, and each fetch resumes
where the previous fetch stopped. No producer thread runs ahead, and no complete
result is materialized. The current supported shapes are:

| Shape | Cursor support |
| --- | --- |
| Single-table WHERE/projection/LIMIT/OFFSET | Yes |
| Source-free SELECT | Yes |
| UNION ALL of supported arms | Yes |
| One or two INNER, LEFT or CROSS joins | Yes |
| ORDER BY matching an ordered index on one table | Yes |
| GROUP BY, aggregates, DISTINCT, other set operations | No |
| Derived relations/CTEs, three or more joins, RIGHT/FULL joins | No |
| Explicit extension index-search candidate API | No |

Unsupported outer shapes fail when opening the cursor, before delivering rows.
Every UNION ALL arm is validated at open. LIMIT 0 validates types and expressions
but reads no input rows. Expressions containing subqueries still execute those
subqueries through their normal, possibly blocking operators.

Primary-key equality scans and equality joins against a primary key use direct
lookups. A single-table ORDER BY streams an ordered index when the requested
columns are a direction-consistent prefix of that index. For total native
column/literal filters it can also seek past leading index columns fixed by
non-null equalities and bound the next column's range. This path preserves scan
order within complete ORDER BY tie groups; tie buffers count toward the query
buffer limit. Unsupported orderings are rejected instead of fully sorting.
Eligible ordered cursors can return indexed values without fetching table rows;
missing columns or logical row identity fetch the row lazily. Index traversal and
tie buffers retain their snapshot across fetch calls and obey work, cancellation
and buffer limits. Other filters use resumable chunk traversal. One or
two joins use nested loops and retain only the current input pins. Longer join
pipelines, blocking cursor operators and disk spilling are not implemented.

A cursor owns its snapshot and registry independently of the database, transaction
or SQL connection that created it. Writes after creation do not change its rows;
a transaction cursor also survives subsequent rollback. Long-lived cursors retain
old versions and may pin their current page. Close unused cursors promptly.
Calls on one cursor require serialization. Separate cursors follow the database's
explicit concurrent-read/extension-safety contract.

## Callback streaming

`query_each(query, visitor, options)` delivers projected rows to a
`bool(std::span<const Value>)` callback. The span is borrowed until the callback
returns. Copy values you need to retain. Returning false stops normally; the core's
`StreamResult` reports types, delivered row count and whether the visitor stopped.
SQL's `query_each(statement, visitor, parameters, options)` returns output column
names and delivers unpacked SQL values.

Callback streaming supports the cursor shapes above. Its established single-table
path retains the ordinary indexed executor and can allocate candidate vectors,
including those returned by extension-index providers. Use a suitably limited materialized query for ordered top-k
vector retrieval.

Streaming evaluates each matching row's projection before advancing. OFFSET skips
the projected output, so errors in skipped projections are still reported. A later
error or cancellation does not undo rows already delivered or callback side
effects. This differs from materialized `query()`'s phase ordering. The snapshot
remains stable if a callback mutates the originating transaction. Callbacks must
not destroy objects involved in the active `query_each` call.

## Accounted operator buffers

`max_buffer_bytes` limits accounted row/key payloads and container allowances in
scan matches, sort candidates, result rows, grouping keys, DISTINCT/set storage and
join lookup/pair buffers. Cursor output batches use the same allowance, while work
limits remain cumulative across batches. Bounded top-k sorting can fit where an
unbounded sort does not. Resource errors leave stored data unchanged.

This is an operator-buffer guard, **not a strict allocation or RSS limit**. The
accounting includes logical retained payload and conservative node allowances;
it does not precisely model container capacity, allocator overhead, registry or
binding metadata, immutable snapshots, retained page pins, or extension-private
aggregate/index/callback allocations. A provider search or a single projected value
may allocate before its size can be checked. Subquery caches and optimizer metadata
are not completely accounted. Application-owned rows remain outside the allowance
after a batch returns; retaining many batches can still exhaust memory.

These exclusions must be considered alongside the page-cache diagnostics and
process-memory measurements. Blocking operators currently fail on a tracked buffer
limit rather than spilling to disk.

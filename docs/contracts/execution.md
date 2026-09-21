# Controlled reads and streaming

`Database::query(query, options)` and `Transaction::query(query, options)` accept
`QueryOptions`. SQL applications use `Connection::query(statement, parameters,
options)`, which rejects mutations and transaction commands.

```cpp
std::stop_source stop;
coresql::QueryOptions options;
options.cancellation = stop.get_token();
options.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
options.max_work = 1000000;
auto result = connection.query(statement, parameters, options);
```

Another thread may call `stop.request_stop()` without making concurrent database
calls. Cancellation and an expired deadline throw `ErrorCode::cancelled`; exhausted
work throws `ErrorCode::resource`. Failed reads leave the transaction usable.
Controls are scoped to the call and shared by nested query execution. A nested
public query also consumes its parent's budget. Later independent calls start fresh.

Work units count engine operations such as visits, expression evaluations and sort
comparisons. Their meaning can change with implementation; they are not CPU cycles,
row counts or allocation bytes. Cancellation/deadlines are checked at entry, exit
and periodically during execution. SQL parsing/lowering, an individual native
callback, allocation, index-provider operation or system call cannot be preempted.
These controls provide cooperative interruption, not hard real-time deadlines or
a whole-process RAM ceiling. They do not cancel durable commits.

## Streaming scans

`query_each(query, visitor, options)` on a database or transaction delivers each
projected row to a `bool(std::span<const Value>)` callback. The span is borrowed
until that callback returns. Copy values you need to retain. Returning false stops
normally; the core's `StreamResult` reports types, delivered row count and whether
the visitor stopped. SQL's `Connection::query_each(statement, visitor, parameters,
options)` returns output column names and delivers unpacked SQL values.

The initial streaming shape is a single-table scan with WHERE, projection and
LIMIT. Joins, derived relations, compounds, sorting, grouping, DISTINCT, OFFSET
and source-free SELECT are rejected before delivering rows. Subqueries within
expressions still use their normal execution stages. Index candidate buffers and
subquery intermediates can allocate; only the outer result collection is avoided.
Use a suitably limited materialized query for top-k vector retrieval.

Streaming evaluates each matching row's projection before advancing. A later
error or cancellation does not undo rows already delivered or callback side
effects. This differs from the phase ordering of materialized `query()` and is an
explicit contract of the new API. LIMIT 0 binds the shape and delivers no rows.
The scan retains its snapshot even if its visitor mutates the same transaction.
Calls still require external serialization; callbacks must not destroy objects
involved in the active call.

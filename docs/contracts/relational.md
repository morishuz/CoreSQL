# Relational workload support

The structured C++ API executes the full 32-case `speedtest1 main` workload
through an adapter. The optional [SQL frontend](sql.md) lowers SQL text into
these interfaces; the core remains independent of parsing and SQLite APIs.
See [the benchmark contract](../../benchmarks/speedtest1.md) for scope and verification.

## Query NULL and aggregates

`Null(integer())`, `Null(real())` and `Null(custom_type)` are typed query values.
NULL does not change a result column's logical type. Stored columns are non-nullable by default in the core API. Set `Column::nullable`
to allow typed NULL values; primary keys remain non-nullable. SQL columns default
to nullable unless PRIMARY KEY or NOT NULL is declared.

Comparisons with NULL produce UNKNOWN. WHERE accepts only TRUE; AND, OR and NOT
follow three-valued logic with ordered evaluation. Value-level registry equality
considers two same-type NULLs equivalent (for DISTINCT); predicate equality does
not. Ordering places NULL before non-NULL. Function callbacks propagate NULL
without invocation unless `Function::accepts_null` is explicitly true.

```cpp
Query q{"items", {
    aggregate("count"),
    aggregate("sum", {column("quantity")}),
    aggregate("avg", {column("quantity")}),
    aggregate("max", {column("name")})
}};
auto totals = db.query(q);
```

Each aggregate gets fresh state per execution via `AggregateFunction::create`.
Its factory receives argument types and a borrowed registry; retained type
contracts must be copied. `infer` declares the result type. `step` receives
borrowed argument values, and `finish` returns a checked value or typed NULL.
Registration and factories are validated even for LIMIT 0. Factories and callbacks
are trusted native code under the same lifetime/non-reentrancy contract as scalar
functions. The built-in count, sum, avg, min, max and group_concat live in
`addons/aggregates/aggregates.cpp`, not as branches in the query executor.

Empty sets produce count 0 and NULL for the other reductions. NULL inputs are
ignored. Integer sum overflow is an error. Numeric aggregates accept integer or
real input; group_concat accepts text/integer and uses comma separators. Min/max
use the registered type comparator. `Query::group_by` groups by expressions,
with NULL keys equivalent; `having` filters completed groups. Projections and
query-level ordering can compose aggregate results with scalar expressions.
Non-aggregate column references must be grouped. Use `aggregate(name, arguments, true)` for DISTINCT input tuples. Each aggregate
gets its own per-group set, using registered ordering, before its state receives
values. Nested aggregates and aggregate-local ordering are not implemented.

Repeatable grouped queries stream projected inputs into aggregate states without
retaining the matching rows. Filter failures retain precedence over projection
failures, which retain precedence over aggregate-step failures. Non-repeatable
queries keep the original materialized schedule so callback order is unchanged.
Group keys, DISTINCT sets and aggregate state can still grow with input; neither
bounded memory nor compensated floating sums are promised.
`Query::distinct` removes duplicate projected rows, preserving their first
occurrence, then applies LIMIT. Its current implementation requires ordering on
each output type. SQL scalar helpers include checked integer arithmetic, bitwise
AND, character counting, ASCII LIKE and three-valued integer AND. They are small
registered functions, not a complete SQL coercion/string-semantics layer.

## Index and schema operations

```cpp
auto tx = db.begin();
tx.create_index("items", {"by_category_name", {"category", "name"}});
tx.create_index("items", {"unique_code", {"code"}, true});
tx.add_column("items", {"quantity", integer(), false, {}, std::int64_t{123}});
tx.commit();
```

Named ordered indexes support multiple columns, uniqueness and per-column
ascending/descending directions. They coexist with the existing per-column
extension-provider indexes. Comparisons use registered types; the shared physical
implementation is an immutable AVL tree, sharing unaffected branches between
snapshots. This is a native composite-index facility, not a new binary extension
ABI. Custom physical multi-column provider hooks remain a future design question.

Creation builds and validates privately; duplicate unique keys leave the prior
schema and data unchanged. Insert, update, delete, clear and REPLACE maintain all
indexes. Updates remove old keys before installing new keys so valid key swaps
are possible. Query access recognizes a leading column/literal equality or a
leading pair of inclusive lower/upper comparisons on an indexed first column.
A right-side equality join can probe such an index, including non-unique keys.
Other predicates retain scan fallback. There is no cost-based join reordering.

Added columns require a valid default (NULL is allowed for nullable columns) and cannot declare an inline
index/primary key. Existing rows are rewritten atomically; retained snapshots
keep their old schema. The default is persisted and fills omitted trailing input
columns. Arbitrary core column-name mappings and DROP/RENAME COLUMN are not implemented.

`insert_from(table, query, replace)` reads the source before writing, including
self-insertion. Width/type/default checks run even for an empty source. The whole
statement rolls back on error. `replace` removes rows conflicting with the
primary key or any named unique index and inserts the new row, as one atomic
operation. Foreign-key cascades, triggers and SQL conflict-policy syntax do not
exist.

## Joins, scalar subqueries and row identity

`Query::join` retains the original two-table interface. `Query::joins` is an
ordered sequence of inner joins; use one or the other. Set `Join::cross`
for a Cartesian product (the two equality operands are then ignored). Every source needs a
distinct alias. `source_where` explicitly filters the base table before joining;
`Join::right_where` optionally filters its right input using unqualified
columns; `where` filters after the joins. This distinction preserves evaluation/error
order rather than relying on implicit predicate reordering. Intermediate joined
rows currently materialize; no streaming operator API is implied.

`scalar_subquery(query, {{"parameter_name", outer_expression}})` binds named
`parameter("parameter_name")` expressions per outer row. A subquery must project
one column. No match returns typed NULL; multiple rows use the first, matching
SQLite's scalar-subquery behavior. Queries are limited to 64 nested executions.
These are engine query expressions, not permission for extension callbacks to
query a database reentrantly.

`exists(query, bindings)` uses the same correlation and binding rules, but returns
integer 0 or 1 and permits any projection width. The complete query binds first,
including unused projection/order expressions, so schema and type errors are
still reported at LIMIT 0. During execution, projection values, DISTINCT and
ordering do not affect ordinary scan existence and are not evaluated. Grouped
and compound queries execute their relational stages to determine whether any
rows survive. Ordinary scans stop at
the first qualifying row. LIMIT 0 is false; an ungrouped aggregate with a positive
limit is true even on empty input. Intermediate joins can still materialize, and
uncorrelated subqueries are not cached across outer rows.

`row_id()` returns a single-table query's stored monotonic identity. It survives
updates, vacuum, exports and recovery. IDs are not reused after deletion/clear;
REPLACE inserts a new identity. This is not a claim of complete SQLite rowid/IPK
alias semantics. The main benchmark only observes the identity of the original
unindexed z1 rows, where the values agree. Joined row identities and row-id
mutation predicates are deliberately unsupported.

## Maintenance and persistence

`Transaction::vacuum()` repacks chunks and rebuilds indexes atomically, preserving
logical row IDs and old snapshots. In persistent mode, commit logs the change;
`Database::checkpoint()` separately reclaims obsolete log records. It is not a
byte-for-byte equivalent of SQLite's file-level VACUUM.

`integrity_check()` validates row/schema/identity/accounting invariants, primary
lookups, and ordered-index tree structure, cardinality and row membership.
Primary and secondary providers also validate their full contents against source
rows through the [index validation contract](extensions.md#index-integrity-validation).
A provider without validation support reports `unsupported`; it is not silently
accepted.
`analyze()` computes table row counts and named-index distinct-key counts. The
current statistics are returned to the caller, not persisted or consumed by a
cost optimizer. These are explicitly identified native maintenance equivalents
in the benchmark, not comparable SQLite maintenance scores.

Exports use CORESQL7 and new log records CORECHG8, retaining CORELOG2 framing.
Descriptors include defaults and ordered-index definitions; rows include stable
identities. Previous export and log record versions remain readable. Recovery
checks schema evolution and identities, then rebuilds indexes. New writers require
the new reader. The durability protocol is unchanged; identities add eight encoded
bytes per row. The encoded-size limit is the configured admission limit in the
storage contract.

The graph extension and SQL frontend exercise these interfaces. None is
frozen; multi-table execution/aggregation materialization remains an explicit
tradeoff to revisit with workload evidence.

## Multiple sort keys and lazy conditional expressions

`Query::order_by` is `std::vector<Order>`.
An empty list means no ordering. For example:

```cpp
Query q{"items", {column("id")}, {},
        {Order{column("category")}, Order{column("score"), true}}, 10};
```

Keys compare lexicographically through registered type comparators. NULL sorts
first in ascending order and last in descending order. Complete ties retain scan
order, including across a bounded LIMIT heap. All keys bind even on empty input
or LIMIT 0. Computed keys and potentially failing predicates keep their full
matching-row evaluation. Single-table column ordering with total native filters
can seek a compatible ordered index, skipping leading columns fixed by non-null
equalities, and stop after LIMIT qualifying rows. An optional range bounds the
next index column. Complete boundary tie groups are examined to preserve scan
order; large tie groups require accounted query-buffer space. OFFSET still
requires skipping qualifying rows.

Eligible ordered scans read indexed columns directly from the retained index
snapshot. A query covered by those columns needs no table-row fetches; other
columns and logical row identities fetch the row lazily. For example, an index
on `(device, ts, id)` can cover `SELECT id, ts ... WHERE device = ... ORDER BY ts
LIMIT ...`. An index on `(device, ts)` does not implicitly contain the primary
key `id`. Indexed values remain subject to normal mutation, savepoint and recovery
semantics; covering reads do not change the persistent format.

In the sorting fallback, the first owned key stays inline; additional keys use
per-match storage. Heavy column/literal keys borrow from the immutable snapshot,
whereas computed keys
own their values. This is not a whole-query memory budget.

`choose(branches, otherwise)` supplies generic lazy conditional expressions:

```cpp
auto selected = choose({{column("enabled"), column("payload")}},
                       literal(Null(text())));
```

Conditions must return integer truth; NULL means false. The overload
`choose(base, equality_function, branches, otherwise)` evaluates the base once
and compares it with each reached branch key using the named registry function.
That function must infer integer truth. NULL base/keys do not match; a NULL base
selects the fallback without evaluating branch keys. Every branch binds, but only
the chosen result executes. Extension types work as results, and matching can use
an extension's registered equality function. No SQL identifier is built into the
executor.

Non-NULL result arms must agree on type. Literal NULL arms adopt that type; all
literal-NULL arms default to integer. Omitted fallback returns typed NULL. This
bounded rule is not general type unification or SQL affinity. `evaluate_constant`
uses the same binder/evaluator for source-free literal/call/conditional expressions,
so SQL constants and row expressions share validation and lazy execution.

## Membership and compound queries

`membership(value, candidates, equality_function)` accepts a vector of expressions
or a one-column Query plus optional correlated bindings. The named function must
infer integer truth. The left operand evaluates once; NULL comparison produces
UNKNOWN unless another candidate matches. Empty input returns false even for a
NULL left operand. List arguments currently evaluate eagerly; query inputs are
materialized. SQL NOT applies three-valued negation to this result. The core
contains no SQL function names or type-specific comparison logic.

`Query::compounds` appends pairs of SetOperation and immutable Query pointers.
Operations apply left-to-right. UNION ALL retains duplicates; the other operations
use registered ordering and treat NULLs as equal. All arms must have matching
column types, validated even for LIMIT 0. The outer order/limit apply to the
combined result; order columns use zero-based numeric output names (`column("0")`).
An empty table name denotes a singleton empty row, allowing source-free queries.

Non-repeatable grouping, compounds and joins can materialize intermediate rows. They share the
ordinary binder, filter and sorter through private temporary tables; temporary
state never enters storage. There is no spill-to-disk or whole-query memory limit.
These additions intentionally establish correctness before optimizing execution.

## Outer and general joins

`Join` replaces the narrow type name; `InnerJoin` remains an alias for source
compatibility. Set `kind` to `JoinKind::left`, `right` or `full`, and optionally
supply `on` as a general qualified Predicate. With no `on`, the original equality
operands apply unless `cross` is true. ON matches are recorded before WHERE,
including for duplicates and NULLs; preserved unmatched sides receive typed NULL
values. Filtering the result cannot turn a matched row into an unmatched row.

`on`, output expressions and filters all bind before execution, including on
empty input and LIMIT 0. General joins normally materialize joined rows; eligible
inner scans instead retain borrowed matched pairs. Indexed
column-equality inner joins retain the existing path. SQL-specific coercion stays
outside this API. The shared row comparator serves DISTINCT, sets, grouping and
DISTINCT aggregate state, so extension ordering uses one contract throughout.
For repeatable queries, these reductions bind comparison metadata once and trust
already validated rows, literals and checked expression results. Nonrepeatable
queries retain validating comparisons; public Registry comparisons and ANALYZE
also continue validating their operands. NULL ordering is unchanged.

## Named query-local relations

`Query::relations` holds named immutable queries and their output columns. The
core validates output width, names and types; only those names/types are used,
not table constraints, defaults or indexes. Definitions shadow the corresponding
input table names within the query. Referenced definitions are materialized once
in dependency/declaration order and shared by nested queries using that name.
Unreferenced definitions are not evaluated, and LIMIT 0 binds shapes without
running their row expressions. Recursive dependencies are rejected. This is an
execution-local relation stage, not a stored view or schema mutation.

## Repeatable execution and resource costs

`Query::repeatable` is false by default. Setting it promises that the query,
including callbacks and domain operations, has no side effects and produces the
same results within its snapshot. This permits reuse of successful scalar,
predicate and subquery results. Errors are not cached, conditional branches remain
lazy, and caches never cross execution or snapshot boundaries. An equality
provider can also opt into prepared membership lookup under the
[extension contract](extensions.md).

Repeatable scalar and EXISTS subqueries with integer captures can cache up to
4,096 successful results per bound expression before clearing the cache. NULL
captures have their own key. Entry counts are bounded; retained value sizes depend
on the input. Repeated expressions need not invoke a callback the same number of
times as a nonrepeatable query. Arbitrary SQL callbacks do not opt in automatically.

Equality joins can use primary/ordered indexes or query-local hash lookups for
matching native-certified i64, i128, REAL and TEXT keys. Providers with custom
comparison semantics retain their registered behavior. Building a lookup can take
work and memory proportional to its right input even with a small positive LIMIT.
Duplicate matches retain source order. General ON joins preserve NULL candidates,
UNKNOWN evaluation and subsequent errors; a candidate lookup does not replace the
complete ON predicate.

Safe predicate filtering and column selection may reduce intermediate state while
preserving the documented join order, NULL handling and errors. CoreSQL does not
provide arbitrary join reordering or a statistics-based cost optimizer. See
[SQL planning](sql.md#safe-join-planning) for supported query forms.

Materialized general inner ON joins finish ON evaluation before WHERE, ordering
and projection, including with a small LIMIT. Right-side join filters also complete
before ON or final WHERE, even for an empty left input. Query results own their
values. Intermediate matches, hash lookups and subquery caches can retain
substantial memory or pin input pages. Use the
[execution controls](execution.md) and distinguish materialized execution from
streaming, whose projection/error timing follows row delivery.

## Composite range selection and mutation results

Ordered-index selection can combine a leading equality prefix with an inclusive
range on the next index column, honoring mixed ascending/descending directions.
The selector considers only the initial conjunction of total, certified-native
column/literal comparisons on nonnullable columns. It stops at callbacks, nullable
columns and other conditions to preserve error/UNKNOWN evaluation. The full
predicate is rechecked and row visitation order is retained. A leading primary-key
lookup keeps precedence. This is deterministic prefix selection, not a statistics-
based cost optimizer. It avoids scanning unrelated rows; wider ranges can still
return large candidate buffers.

Primary-key ranges use the provider's optional range capability under the same
leading native-comparison safety rules. Strict and reversed bounds retain their
original residual comparison. Eligible repeatable inner joins also use existing
indexes to select input candidates before joining. Their early guards preserve
UNKNOWN rows, leave the final WHERE intact and do not cross potentially failing
callbacks or source filters. Nullable-only guards conservatively retain scanning.

`Transaction::update_returning` and `erase_returning` return all affected columns
with their types, containing new and deleted values respectively. They retain the
normal atomic mutation guarantees and evaluate assignments/predicates once. The
ordinary count-only APIs avoid allocating these result rows. SQL applies its own
RETURNING projection outside the core.

`Transaction::update_if` separates locating rows from deciding whether to update
those rows. Its `matched` count is computed before the additional condition;
`updated` includes accepted rows whose values remain unchanged. The condition
runs only for matched rows, and assignments run only when it accepts a row.
Assignments read the original row, as with `update`. Optional returned rows own
all affected columns; disabling returning avoids materializing those results.
The mutation is atomic, including evaluation and constraint failures. SQL UPSERT
uses this distinction so a conflict rejected by its WHERE condition cannot turn
into an insertion.

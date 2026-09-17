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

Exports use CORESQL5 and new log records CORECHG6, retaining CORELOG2 framing.
Descriptors include defaults and ordered-index definitions; rows include stable
identities. Previous export/log record versions remain readable. Recovery checks
schema evolution and identities, then rebuilds indexes. New writers require the
new reader. The durability protocol and 64 MiB encoded-state limit are unchanged;
identities add eight encoded bytes per row.

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
or LIMIT 0; every matching row's keys evaluate before it can be discarded.
The first owned key stays inline; additional keys use per-match storage. Heavy
column/literal keys borrow from the immutable snapshot, whereas computed keys
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
empty input and LIMIT 0. The general path materializes joined rows; indexed
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

`Query::repeatable` is false by default. Setting it promises that the query,
including callbacks and domain operations, has no side effects and produces the
same results within the current snapshot. Successful uncorrelated membership
query results can then be cached lazily for the lifetime of their bound expression;
comparison against each needle normally executes through the registered equality
function. An equality provider may explicitly opt into a prepared membership
lookup; see the extension contract. Bound caches never cross
execution or snapshot boundaries. Repeatable grouped membership and integer-key
inner joins also permit the bounded plans described in the
[SQL planning contract](sql.md#safe-join-planning). Arbitrary callbacks retain the
default path unless the caller explicitly supplies this promise.

Repeatable Cartesian scans can reuse successful leading left-only WHERE
predicates for subsequent right rows. They stay at their original evaluation
position; UNKNOWN and later errors remain observable. Reuse ends at the first
right-dependent predicate, includes subquery captures in dependency checks, and
never crosses a scan or snapshot boundary.

Single-key repeatable integer grouping uses a hash lookup when the provider
certifies canonical scalar semantics. NULL has its own group. Aggregate inputs
retain their arrival order, and groups are sorted before aggregate finalization
to preserve key/finish order. Other keys and custom providers retain ordered
lookup.

A bounded disjunctive-join rule handles two-table Cartesian inner joins when
every top-level OR arm starts with the same native integer column equality.
It preserves FROM order and the entire WHERE predicate, while using the equality
to avoid materializing mismatched pairs. Reversed equality operands are accepted.
The rule checks the current snapshot and falls back if either key contains NULL:
UNKNOWN can still evaluate later expressions, so discarding those pairs could
suppress errors or callbacks. Predicates preceding the key, differing keys,
non-integer keys, outer joins, ON predicates and source filters retain the
existing paths. LIMIT 0 binds without scanning. The resulting equality join uses
the native integer lookup described below.

Unindexed, native-integer equality inner joins use a query-local hash lookup
of the right-hand input. Existing ordered and primary indexes take precedence.
The lookup is built on the first non-NULL left key; LIMIT 0, an empty left input,
or exclusively NULL left keys do not build it. NULL right keys do not enter the
lookup. Buckets retain right-side scan order and probes retain left-side scan
order, including duplicate keys. Right rows are borrowed from the query snapshot;
the lookup is destroyed at query completion and is never shared across queries
or stored durably. Building it requires O(right input rows) work and memory even
when a small positive LIMIT needs only a few matches. Other domains, Cartesian
joins, and general/outer joins keep their existing execution paths.

Repeatable multi-table inner joins can also prune rows using a leading AND prefix
of matching native integer/real/text column/literal comparisons. Available comparisons run during
base scanning and each join stage, before rows enter the next materialized table.
Only FALSE is rejected: UNKNOWN survives so that later WHERE expressions can
still execute or fail. The full final WHERE, join order, duplicate multiplicity
and projection order remain unchanged. Nested AND groups are traversed in order;
a function, subquery, OR, other domain or other predicate shape ends the prefix.
Existing source/right filters, general ON conditions and outer/non-integer joins
retain their prior paths. LIMIT 0 still validates without evaluating rows. This
is intermediate-row pruning, not a general join-order optimizer.

Repeatable inner-join chains with explicit projections can pass their final
Cartesian/native-integer join directly to the normal filter/project/group path.
This avoids materializing its unfiltered row pairs. It retains join/WHERE order
and leaves general ON conditions and outer joins on their existing paths. A
right-side filter completes eagerly before the final join streams. Earlier join
stages still materialize.

Repeatable scalar and EXISTS subqueries with integer captures (including no
captures) cache successful results per bound expression. NULL captures have their
own key; errors are never cached. The cache clears at 4,096 entries, so entry count
is bounded, while value sizes remain input-dependent. It never crosses statements
or snapshots. Arbitrary SQL callbacks do not opt in.

A single-table repeatable subquery beginning with native integer column/parameter
equality may build a lazy candidate index. Matching rows and NULL-key rows retain
original order and row identity; a NULL parameter uses the full scan. The complete
WHERE still executes. Candidate tables use private names so nested queries keep
seeing the original full table. Broad candidate sets use the original scan.

Eligible repeatable two-source chains can also bypass the left-input copy and use
the original snapshot rows directly. This requires an explicit projection, no
source/right filters or general ON, and a Cartesian/native-integer inner join.
The full query shape is still validated before execution; row order and LIMIT
behavior are unchanged.

Safe local prefixes can filter each input into a private subset before joining.
Private input/correlation subsets retain immutable source chunks and store ordered
row locations rather than copying rows. Scans and join/correlation lookups honor
those selections; physical indexes are not inherited. Row IDs and snapshot
lifetimes remain unchanged.
FALSE rows are removed, UNKNOWN rows retained, and original sources remain visible
to nested queries. A known-empty right input skips the left scan after complete
shape validation. Eligible repeatable chains also project only columns needed by later
joins, WHERE, grouping, ordering, output and correlated captures into intermediate
tables. These changes preserve source order; this is not arbitrary join reordering.

General and outer ON joins can use a lazy integer-key candidate lookup when the
first ON conjunct is equality between native integer columns on opposite sides.
The complete ON still determines matches. NULL right keys remain candidates and
a NULL left key scans all right rows, preserving UNKNOWN and subsequent errors.
Candidate order, duplicates and unmatched-row emission remain unchanged. A
callback before the key, other operators and other domains keep the nested scan.

General/outer joins also retain only columns needed by their final WHERE,
projection, grouping, ordering and correlated captures when the projection is
explicit. The complete ON uses borrowed original rows before materialization;
pruning columns does not move predicate or callback evaluation.


Repeatable right-side join filters retain immutable chunks and an ordered row
selection instead of copying every filtered value. The entire filter completes
before ON or final WHERE, including when the left input is empty. A private name
preserves the original source for nested subqueries; physical indexes are not
inherited by the view. Nonrepeatable filtering keeps its materialized schedule.

For an eligible repeatable inner join with a top-level OR, input pruning may use
an alternative of local guards, one from each arm's safe native comparison
prefix. Every arm must supply a guard for the input being narrowed. Only rows
for which all alternatives are FALSE are discarded; UNKNOWN stays eligible.
The full WHERE remains in place. A potentially failing operation terminates each
prefix, so later guards cannot suppress it.

Repeatable scan projections may lazily share identical scalar call results within
one projected row. Conditional branches retain first-demand evaluation, failures
are not cached, and predicates/sorts/separate executions do not share these slots.
Group lookup borrows incoming keys until a new group needs an owned key.

# Experimental SQL frontend

`coresql_sql` is an optional C++20 library built on public CoreSQL APIs. The
`coresql` library has no dependency on it or SQLite. Parsing, SQL value encoding,
expression lowering and connection/transaction ownership are separate source
modules in `sql/`. This is a deliberately bounded SQL dialect, not SQLite
compatibility or a production SQL service.

For planned scope and feature selection, see the [SQL roadmap](../architecture/sql-roadmap.md)
and [benchmark coverage and methods](../../benchmarks/tpch/README.md). Planned features
are not part of the supported dialect until implemented and validated.

## Embedding and prepared statements

```cpp
#include "coresql/sql.hpp"
using namespace coresql;
Registry registry;
// Defaults include scalar SQL policy and the DATE, DECIMAL and VECTOR add-ons.
sql::install(registry);
Database db(registry);
sql::Connection connection(db, registry);
connection.execute("CREATE TABLE items(id INTEGER PRIMARY KEY, label TEXT)");
sql::Statement insert("INSERT INTO items VALUES(?1, ?2)");
connection.execute("BEGIN");
connection.execute(insert, Row{std::int64_t{1}, std::string("one")});
connection.execute(insert, Row{std::int64_t{2}, std::string("two")});
connection.execute("COMMIT");
auto result = connection.execute(
    "SELECT id,label FROM items WHERE id BETWEEN ? AND ? ORDER BY id",
    Row{std::int64_t{1}, std::int64_t{2}});
```

A statement caches an immutable parsed tree and parameter layout. It is reusable
and copyable; it does **not** cache a bound executable plan. Execution resolves
schema, substitutes parameters, lowers expressions and invokes the existing core
binder. DDL changes are seen on the next execution rather than retaining stale
schema pointers. Preparing checks syntax; schema/function/type errors can arise
on execution, including for empty tables. Parameters are supplied afresh to every
execution; no previous binding is retained. `?` and `?NNN` are supported (1..4096);
parameter count is the largest slot, and the supplied span must match it exactly.
Named parameters and streaming `step()` are not implemented. `Result::columns`
contains SELECT output names, including for empty results; explicit AS wins, then
the source column name, then `column1`, `column2`, etc. for unnamed expressions.

The database must outlive the connection and remain at the same address. Pass the
same registry used to construct the database; the connection takes its own
immutable copy for constant expression evaluation. Calls require external
serialization. `BEGIN` owns a core transaction until `COMMIT`/`ROLLBACK` or
connection destruction. Other statements autocommit when no transaction is open.
Failed multi-row inserts and table-plus-index creation roll back their entire
statement through savepoints; earlier work in an explicit transaction survives.
Commit conflicts and uncertain persistent I/O outcomes retain the core contract.

`sql::Result` contains materialized rows, output `columns`, and a mutation `changes` count. It has no
homogeneous column-type promise: undeclared columns can return different native
value types in different rows. An integer primary key can be supplied or generated on insertion;
reads of its `rowid` resolve to that key. See generated keys below.
Hidden rowid access on non-integer primary-key tables is unsupported.
Other hidden row identities follow CoreSQL's monotonic identity semantics, which
do not reproduce all SQLite reuse behavior.

## Supported SQL

- CREATE TABLE with INTEGER/INT/BIGINT, REAL, TEXT/VARCHAR, DATE, DECIMAL, VECTOR, or undeclared columns;
  nullable columns, inline primary/unique keys and defaults. CREATE INDEX supports
  multiple ordered columns. ALTER TABLE ADD COLUMN, INSERT/REPLACE VALUES or
  SELECT, UPDATE, DELETE, and explicit transactions are supported.
- SELECT with WHERE, DISTINCT, multiple ORDER BY keys/aliases/ordinals, LIMIT,
  GROUP BY, HAVING, and expressions containing aggregates. Source-free SELECT
  uses one empty input row.
- Comma/CROSS/INNER joins and LEFT, RIGHT, FULL [OUTER] JOIN with ON predicates.
  ON determines matches before null extension; WHERE filters afterwards. Chains
  retain their written order when explicit JOIN syntax is used. Unmatched rows
  contain typed NULLs, including extension columns. USING and NATURAL JOIN remain
  unsupported. Ordinary equality inner joins retain their indexed execution path.
- FROM subqueries with required aliases and optional output-column lists; ordinary
  WITH clauses with repeated references and nested lexical scope. GROUP BY accepts
  output aliases when no input column has the same name.
- EXTRACT(YEAR FROM date) and SUBSTRING(text FROM start [FOR length]);
  SUBSTRING also accepts comma-separated arguments.
- Registered aggregates accept DISTINCT on one argument, including in grouped,
  correlated, HAVING and ordering expressions. Deduplication occurs per aggregate
  and per group, before aggregate input conversion. count(DISTINCT *) and DISTINCT
  on scalar functions are rejected. Aggregate-local ORDER BY is not implemented.
- IN/NOT IN lists and correlated subqueries; EXISTS and scalar subqueries;
  CASE, lazy COALESCE, BETWEEN, LIKE/NOT LIKE, IS NULL, boolean expressions and arithmetic.
- UNION/UNION ALL, INTERSECT and EXCEPT, applied left-to-right, with final ORDER BY
  and LIMIT. Mixed built-in result types retain their native SQL values through
  the SQL module's common value representation.
- CAST to INTEGER/INT/BIGINT, REAL, TEXT/VARCHAR, NUMERIC, DATE and DECIMAL, plus the conversions below.
- VACUUM, ANALYZE and PRAGMA integrity_check use the native maintenance APIs.
  VACUUM inside an explicit transaction is rejected.

NOT LIKE is NOT applied to LIKE: it preserves LIKE's conversions, ASCII matching
and NULL propagation, evaluates each operand once, and does not add ESCAPE syntax.
Correlated outer columns in comma-joined subqueries remain typed captured inputs;
they are not treated as local join edges.

## Derived relations and ordinary CTEs

`sql/relations.cpp` resolves source names and output columns, then lowers both
FROM subqueries and CTEs to the core's `Query::relations`. They are local to a
single query execution and never enter the database schema, transaction change
log or durable storage. The core validates their declared output names/types
and uses its existing materialized relational stages.

A FROM subquery requires an alias. Its columns use explicit SELECT aliases,
otherwise the selected column name, otherwise `column1`, `column2`, etc. An
optional relation column list replaces all names and must match the output
width. Duplicate relation output names are rejected; rename them explicitly.
`SELECT *` preserves source order. Derived relations have no hidden rowid.

CTE names shadow base tables and enclosing CTEs. A definition can reference
preceding definitions; self references, forward references, RECURSIVE and
LATERAL are rejected. FROM subqueries cannot capture surrounding row columns.
Scalar/EXISTS/IN subqueries retain ordinary outer-column capture and can access
visible CTEs. SELECT output aliases in GROUP BY are a fallback: an input column
with the same name takes precedence. ORDER BY retains its existing alias rules.

Referenced relations are eagerly materialized before their consuming query's
row evaluation, in declaration/dependency order. Multiple references to a CTE
share that materialization, including references from nested scalar subqueries.
Syntactically unreferenced CTEs are not evaluated. LIMIT 0 binds relation shapes
without evaluating their rows. A CTE referenced only from an unselected CASE arm
is still a referenced relation and is materialized; CASE does not defer that
separate relation stage. Callback counts therefore differ from repeating the
CTE's SELECT inline. All materializations expire with the execution, and later
executions see current parameters and the transaction's snapshot.

SUBSTRING uses one-based Unicode code-point positions in valid UTF-8, preserving
embedded NULs. With an explicit length, positions before one count toward that
length; without a length, the remaining suffix is returned. Negative lengths
and malformed UTF-8 raise constraint errors, and NULL propagates. The bundled adapters support
EXTRACT(YEAR FROM DATE); application adapters can support additional input types.
Other extraction fields and interval arithmetic are not supplied.

## Conversion policy

Conversion belongs to the optional SQL module. Native core writes, expression
binding and extension types retain their strict type contracts. `sql/coercion.cpp`
contains scalar conversion adapters; `sql/type_lowering.cpp` dispatches extension
policy through the [SQL type adapters](extensions.md#sql-type-adapters);
`sql/typing.cpp` handles SQL
result-type inspection and mixed result arms. No SQL type IDs or conversion rules
are embedded in the core.

Arithmetic accepts mixed integers/reals and numeric text, including values from
undeclared columns. Text in arithmetic uses a numeric prefix, or zero if there
is no number. Integer-only division truncates; real operands produce real
arithmetic. Division by zero returns NULL. Integer overflow and non-finite or
out-of-range real conversions remain errors, rather than SQLite's overflow
promotion or infinity behavior.

Explicit INTEGER casts use the integer prefix of text (ignoring a following
fraction/exponent), truncate real values, and clamp outside the signed 64-bit
range. REAL and NUMERIC casts recognize decimal/exponent prefixes. NUMERIC keeps
native numeric values and can convert integral numeric text to an integer.
LENGTH counts non-continuation bytes before the first embedded NUL. For valid
UTF-8 this is the code-point count of that prefix, matching SQLite's NUL behavior.
It is permissive on malformed UTF-8 and is not a validator; SUBSTRING instead
requires valid UTF-8 and preserves NULs. These distinct behaviors are intentional
compatibility contracts, not a promise that all text functions share one policy.

TEXT casts, concatenation, length and LIKE accept built-in scalar inputs;
formatting is locale-independent. NULL propagates through conversions.

Boolean contexts accept numbers and numeric text. CASE/COALESCE remain lazy;
all arms bind, but only selected values execute. Mixed built-in result arms use
`sql.value` to retain their individual integer/real/text values. Undeclared
columns use the same representation. Numeric aggregates over text/dynamic values
convert each input after DISTINCT filtering; dynamic SUM retains an integer
result until real input requires promotion.

SQL writes allow compatible scalar conversions: numeric text into numeric columns,
integers into REAL, and numbers into TEXT. Assigning to INTEGER requires a complete,
lossless numeric conversion; malformed text and fractional or out-of-range numbers
fail atomically. Use an explicit CAST when truncation is intended. This differs
from SQLite's permissive non-STRICT column affinity: a declared CoreSQL INTEGER
column cannot retain text or fractional values.

Comparisons support mixed built-in classes, exact integer/real comparison, and
column-directed numeric/text affinity. Numeric text affinity requires a complete
number; nonnumeric text keeps its text ordering class. Compound results deduplicate
NULLs and compare numeric integers/reals equally while retaining distinct text
values. This is broader SQL conversion support, not complete SQLite affinity or
collation compatibility. Extension conversions remain explicit except for the documented DATE/TEXT storage
conversions below. Other domains need extension-owned conversion functions. DISTINCT/grouping still
require registered ordering, so equality-only types cannot opt in automatically.

## Calendar dates

`DATE` is an ordered, nullable domain type, distinct from TEXT, INTEGER and
TIMESTAMP. `DATE '2000-02-29'` and `CAST('2000-02-29' AS DATE)` construct it;
`CAST(date_value AS TEXT)` produces exactly `YYYY-MM-DD`. BIGINT is a spelling
of the existing signed 64-bit INTEGER type, including its conversion rules.

The date add-on owns calendar validation and encoding: a proleptic Gregorian date
from `0001-01-01` through `9999-12-31`, stored as signed days since 1970-01-01 in
little-endian int64 bytes (`coresql.date.days`, version 1). Input must be exactly
ten ASCII characters in `YYYY-MM-DD` form. Invalid leap days, year zero, whitespace,
times and timezone suffixes are errors; dates never depend on locale or timezone.

SQL storage accepts validated ISO TEXT into DATE and formats DATE into TEXT.
This applies to defaults, INSERT/INSERT SELECT and UPDATE; failures are atomic.
Casts accept DATE, TEXT and dynamic scalar inputs (which must contain text when
non-NULL). Numeric-to-date casts are rejected, including during binding at LIMIT 0.
NULL propagates, and `CAST(NULL AS DATE)` produces a typed NULL. Invalid DATE
literals fail during parsing; invalid text casts fail only if evaluated, preserving
CASE/COALESCE laziness. Casts and conversions call add-on code, not storage hooks.

Comparisons require DATE operands: use `d >= DATE '1994-01-01'` or cast a TEXT
parameter explicitly. Text column affinity does not reinterpret DATE values.
Ordering, indexes, keys, grouping, MIN/MAX, DISTINCT, membership and correlated
queries use registered domain comparisons. Mixed DATE/TEXT CASE and set-operation
results require an explicit cast. Undeclared columns still store only the original
integer/real/text scalar classes. Interval arithmetic, timestamp conversion and
calendar extraction are not implemented yet.

`sql::install` installs date, decimal and vector add-ons as well as SQL semantics;
do not also install those add-ons separately in that registry. C++ applications without SQL can link
`CoreSQL::date` and call `dates::install`. SQL results retain native DATE values;
use `dates::format` to display them. The SQL CLI displays ISO dates directly.

## Fixed-point decimals

`DECIMAL(p,s)` stores an exact signed coefficient with `1 <= p <= 38` and
`0 <= s <= p`. DECIMAL alone means `(18,3)`; DECIMAL(p) means `(p,0)`.
Its independent add-on uses `coresql.decimal`, version 1, two parameter bytes
(precision, scale), and a 16-byte little-endian two's-complement coefficient.
The implementation requires GCC/Clang-compatible 128-bit integers internally;
no compiler-specific integer type appears in the public API or core.

SQL writes accept DECIMAL, INTEGER and complete decimal text, including exponent
notation, only when the conversion loses no fractional digits and fits the target
precision. `INSERT INTO t(d) VALUES(0.1)` preserves the literal's written digits.
Defaults and UPDATE have the same rule. INSERT SELECT converts its typed output;
use an explicit DECIMAL cast when that output would otherwise be REAL.
Failures preserve statement atomicity, including partial multi-row writes.

`CAST(value AS DECIMAL(p,s))` rounds half away from zero on scale reduction and
reports precision overflow. NULL propagates. Text parsing accepts an optional
sign, decimal point and exponent, with at least one digit, no whitespace or junk,
at most 256 characters and exponent magnitude at most 1000. Numeric literals
larger than the SQL parser's int64 range can be supplied as quoted decimal text.
REAL values/parameters require an explicit conversion to TEXT before a decimal
cast; supply decimal text directly when exact input matters.

For decimal operands `(p1,s1)` and `(p2,s2)`, INTEGER has `(19,0)`:

| Operation | Output | Overflow / rounding |
| --- | --- | --- |
| Add/subtract | `s=max(s1,s2)`, `p=min(38,max(p1-s1,p2-s2)+s+1)` | Exact; reject values outside output precision |
| Multiply | `s=s1+s2`, `p=min(38,p1+p2)` | Exact; reject scale above 38 and value overflow |
| SUM | DECIMAL(38,input scale) | Exact checked accumulation; empty input is NULL |
| AVG | REAL | Exact checked decimal sum, then binary64 division by count |
| Divide | REAL | Binary64 division; zero divisor returns NULL |
| MIN/MAX, ABS, unary minus | Input decimal type | Exact |

Division returning floating point also matches the broad approach used by
[DuckDB's decimal division](https://duckdb.org/docs/lts/sql/data_types/numeric).
This is a bounded CoreSQL contract, not a claim of identical dialect/type-inference
rules. REAL inputs and outputs remain finite; overflow is an error.

In arithmetic, comparisons, BETWEEN, IN and simple CASE, a decimal context keeps
bare numeric literals exact from their original spelling, using minimal exact
precision/scale after removing redundant zeros. Actual REAL operands
(computed expressions or bound values) promote numeric operations/comparisons to
REAL. Existing REAL-only SQL is unchanged. Comparisons between decimal scales and
integers are exact without conversion to floating point. Text comparisons require
an explicit numeric cast. CASE/COALESCE and compound outputs unify decimal/integer
types to the maximum required integral digits and scale, rejecting a common type
wider than 38; a REAL arm selects REAL. Mixed unrelated domains remain errors.

DECIMAL-to-TEXT formatting retains the declared scale. Decimal-to-INTEGER storage
must be lossless; explicit casts truncate fractional digits, but still reject int64
overflow. Casting to REAL is explicitly approximate. Bare `CAST(... AS NUMERIC)`
retains its pre-existing scalar conversion semantics and is not a DECIMAL alias.
Undeclared columns still store only integer/real/text scalar classes; decimal
truthiness, general decimal math functions and automatic text affinity are absent.

`CoreSQL::decimal` and `decimals::install` expose the add-on without SQL. SQL installs
it automatically. Native SQL results retain decimal values; use `decimals::format`
or the SQL CLI for exact display. Parameterized decimal types participate in keys,
indexes, grouping, DISTINCT, joins and persistence through the same extension API.

## Safe join planning

Within the all-safe implicit equality-join planner, connected sources with local
filters are preferred. Cross-alias AND/OR predicates can supply necessary local
guards on equality-join inputs: every OR arm must contribute a guard for that
alias. The complete cross-alias predicate remains in WHERE to check correlations.
General Cartesian OR joins retain their separate existing planning path. This
rule does not move functions or potentially failing expressions.

Comma joins may use leading integer equalities before a later callback/subquery
while preserving FROM order and retaining the complete WHERE predicate. The prefix
must contain only non-failing native comparisons; conversions and callbacks stop
this optimization. No predicate after that boundary moves ahead of it. Valid DATE
casts can expose safe planning opportunities;
invalid casts and arithmetic remain lazy, and syntax depth limits and
ORDER BY/GROUP BY ordinal interpretation remain unchanged.

The SQL lowerer marks queries composed only of the installed built-in operations
and supported scalar domains as repeatable. Successful uncorrelated IN-subquery
results may then be cached lazily for that bound execution. Repeatable scalar
and EXISTS subqueries with only INTEGER captures cache successful results by
capture tuple, including NULL captures. Correlated IN and arbitrary SQL callbacks
do not use that capture cache. No cache survives a statement or snapshot
boundary. The core API exposes this promise as `Query::repeatable`, default false;
callers opting in guarantee side-effect-free, repeatable evaluation in the current
snapshot, including any registered callbacks and domain operations.

A bounded grouped-IN plan evaluates a leading repeatable membership relation
once, after shape validation and only when every Cartesian input is nonempty.
It can then apply that filter before integer equality joins. This rule requires
grouping, so it cannot bypass LIMIT short-circuiting in ordinary SELECTs. For
repeatable inner-join chains whose remaining WHERE consists entirely of native
integer column comparisons, available key checks may run at intermediate stages.
These checks use the same native-prefix planner as other join pruning and discard
only FALSE, retaining UNKNOWN. With a source filter they run only after that
filter completes, never while building filtered input views. The complete final
WHERE remains. Callback-bearing predicates, general ON conditions and outer joins
retain the existing execution path.

A repeatable inner chain without source/right filters may additionally prune
intermediate rows using available leading matching INTEGER/REAL/TEXT column/literal
comparisons, including nested AND prefixes. Another domain, a function,
subquery or other predicate ends that prefix. Only FALSE is discarded; UNKNOWN
continues to the complete final WHERE. This avoids large intermediates without
reordering joins or moving predicates across a potentially failing expression.

## Limits and validation

Identifiers are normalized to lowercase. Quoted identifiers, escaped strings and
SQL comments are supported. Parsing limits remain 1 MiB, 16,384 tokens, 64 nesting
levels and 4,096 parameter slots. Queries materialize results; joins, grouping,
sets and DISTINCT have no spill-to-disk or whole-query memory budget. Outer joins
track unmatched right rows; a leading native-integer ON equality can restrict
candidate scans, with a nested-scan fallback for other predicates.

General sequences, AUTOINCREMENT, foreign keys,
triggers, recursive CTEs, LATERAL, windows, aggregate-local ordering, BLOB casts,
date arithmetic and collations remain outside the dialect. Grouped
columns must be grouped or aggregated; SQLite's bare-column aggregate shortcut
is unsupported. Interfaces and the SQL value encoding remain experimental.

The [pinned public report](../../tests/sqllogictest/README.md) covers five complete,
unchanged SQLLogicTest files: 8,884 queries and 1,822 setup statements. Dedicated
SQLite differential tests cover outer joins, DISTINCT aggregates, casts, NULLs,
coercion and atomic failures; native tests cover extension values and callback
validation. These are correctness checks, not performance measurements or proof
of complete SQL-standard support. The full adapted speedtest1 main workload
remains part of the regression suite.

## CLI

Build `coresql_sql_cli` and pass an SQL script, optionally with
`--database /path/to/file.corelog`. Without a script it reads stdin. Output is
escaped tab-separated display text. Scripts execute statement-by-statement;
use BEGIN/COMMIT for whole-script atomicity. See [the examples](../../examples/README.md),
[relational API](relational.md), and [benchmark contract](../../benchmarks/speedtest1.md).

UPDATE resolves each assignment target before lowering that assignment's value.
An unknown target therefore reports a schema error even when its value expression
is also invalid. WHERE lowering still precedes assignment lowering. Values read
the original row, and a failed statement publishes no partial updates.

### UPDATE lowering order

UPDATE lowers its WHERE predicate first, then resolves each assignment target
before lowering that assignment's value. An unknown target therefore reports a
schema error before an error inside its value expression. Each value is lowered
once, with the target's existing contextual NULL/date/decimal conversion rules.
Assignments still read the original row, and failed statements remain atomic.

## Application lifecycle SQL

- `CREATE TABLE IF NOT EXISTS` and `CREATE [UNIQUE] INDEX IF NOT EXISTS` skip an
  existing object without comparing its definition. SQL-created named indexes use
  a database-wide namespace. Legacy/C++ duplicate names make DROP INDEX ambiguous.
- `DROP TABLE [IF EXISTS] name` removes the table and its indexes. `DROP INDEX
  [IF EXISTS] name` removes a named index; internal inline UNIQUE constraint indexes
  are protected. No CASCADE or DROP COLUMN syntax is provided.
- `ALTER TABLE name RENAME TO new_name` and `ALTER TABLE name RENAME [COLUMN]
  old_name TO new_name` preserve rows and update index column references. Destination
  collisions fail atomically. Table drops and renames persist via full checkpoints.
- `INSERT INTO table DEFAULT VALUES` inserts a row from defaults/nullable NULLs;
  a missing required value is an error, except that integer primary keys can be generated.
- `LIMIT count OFFSET skip` requires nonnegative integer constants or parameters.
  OFFSET applies after ordering/distinct/grouping/compound operations. The current
  materialized path obtains up to count+skip rows, then discards the prefix. LIMIT 0
  still validates expressions without evaluating rows. Use ORDER BY for stable pages.
- `SAVEPOINT name`, `ROLLBACK [TRANSACTION] TO [SAVEPOINT] name`, and `RELEASE
  [SAVEPOINT] name` use the most recent matching name. Rollback-to restores and
  retains that point and closes descendants; release keeps changes and closes it
  and descendants. A savepoint outside BEGIN starts a transaction; releasing its
  outermost point commits. COMMIT releases all points, ROLLBACK discards all, and
  connection destruction rolls back. A failed commit leaves the transaction open;
  rollback and follow the core recovery/retry contract.

## Generated integer primary keys and INSERT RETURNING

```sql
CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT);
INSERT INTO notes (body) VALUES ('Hello') RETURNING id;
INSERT INTO notes VALUES (NULL, 'Another') RETURNING id AS new_id, body;
```

An omitted INTEGER/INT/BIGINT primary key without a non-NULL default, or an
explicit NULL in that column, generates `max(0, largest current key) + 1`.
An empty table starts at 1. Explicit integer values are retained, and an explicit
high value affects subsequent allocation. Declared non-NULL defaults are honored
when the column is omitted. Other primary-key types still require a supplied
value/default. UPDATE does not generate keys: assigning NULL still fails.
This behavior applies to existing integer-primary-key tables as well as new ones.
The typed C++ mutation API continues requiring explicit keys.

Allocation sees staged rows in the current transaction. Failed statements,
rollback and rollback-to discard their inserted keys. Deleting the largest key
can allow its reuse; there is no never-reuse or gapless-ID guarantee. Reopening,
backup/restore, schema rename and index rebuilding retain the same behavior because
the next value is derived from stored keys, not an independent sequence. A largest
key of INT64_MAX rejects automatic allocation with a constraint error; unlike
SQLite's overflow fallback, CoreSQL does not search for random free keys. Generated
keys are always positive, including when all existing keys are negative.
Competing transactions may generate the same candidate; only one can commit and
the stale transaction must roll back and retry under the existing conflict rules.

The initial maximum uses a native aggregate scan, once per ordinary INSERT
statement that generates keys, then advances through the batch. REPLACE recomputes
after each row because a unique conflict can remove the largest key. Bulk callers
should use multi-row INSERT or INSERT SELECT to amortize this scan. No storage
format or persistent counter is introduced.

`INSERT`, `INSERT OR REPLACE` and `REPLACE` accept `RETURNING` after VALUES,
DEFAULT VALUES or SELECT. This initial version supports target column names,
qualified target columns, optional explicit AS aliases, and `*` (including mixed
`*, id`). It does not support expressions, subqueries, implicit rowid aliases,
UPDATE/DELETE RETURNING, or RETURNING as a relation. Output contains the converted
inserted values, with one row per insertion in input-processing order, and uses
`sql::Result::columns` and `changes`. Empty INSERT SELECT still validates output
names and returns column metadata. REPLACE returns only the inserted row, not
rows removed by conflicts. Rows are materialized; the statement rolls back if
insertion or RETURNING construction fails. Values returned inside an explicit
transaction are provisional until its commit succeeds.

## Vector SQL adapter

The default SQL configuration supports `VECTOR` (variable length) and `VECTOR(n)`
(exactly n elements, including zero). `VECTOR '[1,2]'`, `vector('[1,2]')` and
`CAST('[1,2]' AS VECTOR(2))` construct finite float32 vectors. Text uses brackets
and comma-separated numbers; surrounding whitespace is accepted. Malformed text,
non-finite/out-of-range elements and dimension mismatches are errors. Assignment
accepts this text format and checks/retypes vector values to the column dimensions.
`CAST(v AS TEXT)` produces round-trippable bracketed float32 text.

`vector_squared_l2(a,b)` uses the backend's double-accumulated squared L2 distance.
Both inputs must be vectors of equal length; incompatible fixed dimensions fail
binding, variable-length mismatches fail evaluation. NULL inputs yield NULL.
Vectors support same-type equality but no ordering; order by distance instead.
CASE/UNION combining different vector dimensions use the variable-length type.
The C++ vector API and byte encoding are unchanged; SQL conversion is an adapter
policy. There is no ANN index or implicit vector arithmetic.

Scalar SQL policy lives in `addons/scalars/sql.cpp` and is registered through the
same adapter interface as domain types. INTEGER/INT/BIGINT, REAL, TEXT and VARCHAR
keep their existing conversion, numeric-text, mixed arithmetic and affinity
semantics. NUMERIC remains a CAST-only dynamic SQL designation; undeclared columns
still accept integer, real, text and NULL values. See the
[adapter contract](extensions.md#sql-type-adapters) for composition and boundaries.

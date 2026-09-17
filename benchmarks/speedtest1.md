# Full speedtest1 main workload

CoreSQL runs **all 32 cases of the upstream `main` workload** through the
structured C++ API, with query results and post-case table contents checked
against the pinned SQLite reference. Cases 200, 980 and 990 use explicitly
identified native maintenance equivalents. There are no skipped cases.

The original API mode described here is a faithful workload adapter, not the
unchanged SQLite executable running on CoreSQL. The optional SQL mode described
below exercises the frontend using SQL text. Other suites such as CTE, JSON, star and the
`mix1` default macro are not covered by this adapter. Always specify `--testset main`
when running the upstream reference.

## Build and run

Build the pinned SQLite amalgamation using the scratch-build instructions in
[README.md](README.md), then:

```sh
cmake -S . -B build/speedtest -DCMAKE_BUILD_TYPE=Release \
  -DCORESQL_BENCHMARKS=ON \
  -C /tmp/coresql-sqlite-reference/reference.cmake
cmake --build build/speedtest -j4
build/speedtest/sqlite_speedtest1 --testset main --size 100 --memdb --verify --stats
build/speedtest/coresql_speedtest1_main 100
ctest --test-dir build/speedtest --output-on-failure
```

The source of truth is unchanged `source/test/speedtest1.c` in the prepared external
reference, pinned by [revision and checksum](reference/sqlite.json). The standalone `sqlite_speedtest1` target
compiles it unchanged. The adapter links its number-name, swizzle and rounding
functions from a second object with only its main symbol renamed. The deterministic
random recurrence and square-root estimate match upstream. SQL is supplied to
SQLite; equivalent Query/Expr and transaction operations are supplied to CoreSQL.
SQLite is never linked into the CoreSQL library.

The size range is 1..100, giving 500..50,000 rows per initial table. Default is 1
for a quick correctness run; 100 matches upstream's default size. Every command
executes the complete sequence on fresh in-memory databases, including committed
writes. The CTest case uses size 1; larger runs exercise the non-empty correlated
subquery path as well.

## Coverage

| Cases | Engine capability exercised |
|---|---|
| 100, 110, 120 | Unindexed and ordered/unordered unique-key insertion |
| 130, 140 | Unindexed numeric/text filters with count, avg, sum and group_concat |
| 142, 145 | ASCII LIKE, full ordering and top-k ordering |
| 150 | Runtime unique, descending and compound index creation |
| 160, 161, 170 | Indexed numeric and text ranges with aggregates |
| 180, 190 | Indexed INSERT SELECT, clear and refill |
| 200 | Native chunk repacking and index rebuilding (maintenance equivalent) |
| 210 | Atomic ADD COLUMN with persisted default, then aggregate |
| 230, 240, 250, 260 | Indexed/range/whole-table updates and aggregate verification |
| 270, 280 | Range and individual deletes, maintaining all indexes |
| 290, 300 | Statement-atomic REPLACE SELECT and filtered INSERT SELECT |
| 310 | Four-table equality join with indexed neighbour lookups |
| 320 | Correlated scalar subquery, stable row identity, NULL and aggregates |
| 400, 410, 500, 510 | Repeated replacement and lookup of integer/text unique keys |
| 520 | DISTINCT projection |
| 980 | Native structural/data/index checks (maintenance equivalent) |
| 990 | Native table/index cardinality collection (maintenance equivalent) |

The feature interfaces, semantics and limits are documented in
[RELATIONAL.md](../docs/contracts/relational.md). In particular, primary keys remain non-nullable;
typed query NULLs supply empty aggregate and scalar-subquery semantics.

## Validation and comparison contract

For each case, both engines receive the same data/parameters and transaction
boundaries. The adapter then compares every query result and every field of every
table outside the timed region. Ordered query results are compared in order;
unordered results are compared as multisets. Since upstream group_concat has no
ORDER BY, concatenation token multisets are compared while preserving duplicate
counts. Number names contain no commas, so this normalization is unambiguous for
this dataset. Real results use a relative 1e-10 tolerance; integers, text and NULL
presence are exact. Mutation validation runs after every case, not just at the end.

Upstream's precise LIKE buffer offsets are retained: `one` produces `%on%`, not
`%one%`. Unused random draws in these cases are omitted; upstream resets the
random sequence at every case. The scalar-subquery AND operand is TRUE for rows
already admitted by the identical outer row-id restriction. The adapter retains
that equivalent expression simplification.

The native ordered index represents upstream's unique constraints without also
building a redundant hash primary index. Physical layouts differ by engine.
CoreSQL identity values for the observed z1 table survive vacuum/recovery; this
adapter does not imply every SQLite IPK/rowid reuse rule is implemented.

Time includes generation, binding/preparation, statement/transaction execution,
and owned result materialization. SQLite statements are cached within each case
and discarded between cases. Both sides retain results for untimed verification;
SQLite binds text with TRANSIENT, whereas upstream often uses STATIC. Verification
scans change cache history. Hence adapter timings are not directly interchangeable
with the unchanged executable's output.

The full adapter runs CoreSQL then SQLite for each case and reports one sample
per invocation. These timings locate expensive cases; they are not a statistically
controlled speed ranking. No aggregate full-suite speedup is reported, especially
because the three maintenance cases perform different engine-specific work.
Both databases are in memory: committing here does not measure disk durability.

The earlier five-case adapter remains available as `coresql_speedtest1_compare`;
it provides alternating repeated measurements for those cases. Historical subset
CSVs were recorded before the full engine expansion and are not current results.

## Verification and artifacts

The engine suite includes failures during unique-index construction, duplicate
insertion/update, statement-atomic INSERT SELECT failure, retained snapshots,
column defaults through export/log recovery/checkpoint, preserved row IDs during
vacuum, NULL truth tables, empty aggregates, correlated queries, multi-table joins,
and randomized ascending/descending compound-index mutation/range/clone checks.

## SQL frontend mode

`coresql_speedtest1_main 100 sql 4` runs the same 32 cases through SQL text on both
engines, using four fresh database pairs and alternating which engine runs first.
This path does not use the handwritten CoreSQL Query/Expr equivalents. The shared
workload body still constructs those unused expressions, identically on both sides;
that harness work is included in case totals but not statement execution timers.
Per-case query and complete table-content comparisons remain outside timing.

CSV adds run/order, parsing/preparation and execution counters. CoreSQL preparation
parses SQL; execution includes parameter substitution, lowering, core binding and
result materialization. SQLite preparation also compiles its executable statement.
The phases have different boundaries and must not be presented as equivalent work.
The maintenance-equivalent caveat still applies. The original API mode remains
available and neither mode implies a current performance claim.

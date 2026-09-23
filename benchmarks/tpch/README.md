# TPC-H workload comparison

Run the 22 checksum-pinned DuckDB queries against CoreSQL and optional reference
engines. The [README table](../../README.md#performance) uses the
[September 23, 2026 measurements](results/tpch-2026-09-23-sf0.03.json) at
SF 0.03 on CoreSQL `fd5ac5fe9c`. The report includes raw repetitions,
answer checks, the dataset manifest and build fingerprints. The
[September 15 comparison](results/OPTIMIZATION_ROUND2.md) is retained as historical
evidence. This is an engineering comparison, not an official
TPC-H result or a durability benchmark. Query pins and origins are in
[manifest.json](manifest.json); reference dependencies are optional.

## Reporting a fair engineering comparison

Keep correctness outside timed repetitions. Report loading, parsing/preparation,
execution/materialization and reopening separately. Use fresh equivalent data,
disclose warm/cold runs and thread counts, repeat trials and preserve raw per-query
results. An initial single-thread comparison and an optional native-default
configuration should be labeled separately. Do not compare CoreSQL parse-only
preparation to another engine's compiled plan as if they were the same operation.

For a small read-only in-memory profile, all engines should run that profile;
it makes no durability-performance claim. A later durable profile must record
comparable durability settings. List failed/unsupported/timed-out queries and the
entire query set, not only successful timings. Do not derive an official TPC metric
from the diagnostic audit or compare these small-scale measurements to official
published scores.

## Check query syntax and binding

Build the bridge with normal tests enabled (Python and C++20 are sufficient;
DuckDB itself is not needed):

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target coresql_slt_bridge -j 4
python3 benchmarks/tpch/audit.py \
  --bridge build/release/coresql_slt_bridge \
  --queries /tmp/coresql-tpch-queries --fetch \
  --output /tmp/coresql-tpch-readiness.json
```

`--fetch` downloads only missing files from the pinned public source; every query
is hash-checked before execution. Omit it to use an existing offline copy. Queries
stay outside the repository and retain upstream ownership. Each probe uses a fresh
in-memory database. JSON output identifies the query revision and bridge binary
hash. Unsupported SQL is an audit result, not a tool error; missing files, bad
hashes, crashes and diagnostic setup failures exit unsuccessfully.

This tool intentionally does not time queries, generate data, claim correctness,
or impose a CI requirement that missing features stay unsupported. Run the populated-data checks below to verify query answers.

## Small populated Q2/Q16 check

```sh
python3 benchmarks/tpch/check_small.py \
  --bridge build/release/coresql_slt_bridge \
  --queries /tmp/coresql-tpch-queries
```

This optional check uses the same verified, unchanged query files and a tiny
synthetic fixture on the simplified schema. Q2 returns three rows (including
European minimum-price ties despite cheaper supply elsewhere); Q16 returns two
groups after excluding complaints, disallowed brands and types. Both match
Python's SQLite and explicit expected results. The fixture is not dbgen output,
uses native DATE/DECIMAL columns, and is not evidence of scale or benchmark performance.
Pass `--duckdb` in the optional pinned environment below for exact DuckDB checks.
The default relational differential test covers the same new semantics without
requiring downloaded query files.


## Small populated date checks

```sh
python3 benchmarks/tpch/check_dates.py \
  --bridge build/release/coresql_slt_bridge \
  --queries /tmp/coresql-tpch-queries
```

Q1, Q3–6, Q10, Q12, Q14 and Q20 execute unchanged against native DATE columns.
Each receives a fresh synthetic fixture with explicit expected answers and
boundary/exclusion rows, including NULLs. The independent SQLite oracle uses
ISO TEXT columns and replaces only the pinned ISO DATE literals/casts with text
literals. This disclosed oracle adaptation works for these calendar comparisons;
SQLite's own DATE casts do not implement the same semantics. Its oracle uses REAL
for decimals and a `1e-12` relative/absolute tolerance. Explicit fixture expectations
check CoreSQL's decimal cells exactly. Pass `--duckdb` to execute the unchanged
queries on native DATE/DECIMAL in both engines, comparing decimal cells exactly.
Dates/text, NULLs, row counts and ordering are exact. No query text is rewritten
for CoreSQL.

These are targeted correctness checks, not dbgen data or performance results. The default `calendar_dates` test needs no downloaded
queries and covers calendar rules, a full 400-year round-trip cycle, typing,
NULLs, joins, indexes, atomic failures, rollback and persistent recovery.


## Generate and validate shared data

The optional reference requires Python 3.10+ and the pinned dependency. Neither
normal CoreSQL builds nor tests install or link DuckDB.

```sh
python3 -m venv /tmp/coresql-tpch-python
/tmp/coresql-tpch-python/bin/python -m pip install -r benchmarks/tpch/requirements.txt
/tmp/coresql-tpch-python/bin/python benchmarks/tpch/prepare.py \
  --output /tmp/coresql-tpch-data --scale 0.001 --fetch-extension
/tmp/coresql-tpch-python/bin/python benchmarks/tpch/validate.py \
  --data /tmp/coresql-tpch-data --queries /tmp/coresql-tpch-queries \
  --bridge build/release/coresql_slt_bridge \
  --timeout 30 --output /tmp/coresql-tpch-validation.json
```

The generator accepts SF 0.001, 0.01, 0.03 and 0.1. It exports ordered rows in
batches of 4,096 and sets a 512 MB DuckDB generation memory limit; this is not
a whole-process RSS cap. SF 0.03 and 0.1 contain 180,566 and 600,572 lineitems.
The batched SF 0.001 export matches the original CSV and manifest bytes.

The generator refuses an existing output directory. Omit `--fetch-extension` once
the signed `tpch` extension is installed. DuckDB 1.5.5, engine revision `d8cdaa33fd`,
and its matching extension are required. They are pinned independently of the
22 query files. Dataset manifests record binary fingerprints, types, row counts
and per-table CSV hashes. All hashes and dimensions are checked before loading.
CoreSQL and DuckDB receive identical rows and native DATE/DECIMAL schema types;
VARCHAR is unbounded text and CoreSQL's INTEGER family is signed int64.
No indexes or key constraints are added by the validator.

SF 0.001 contains 5 regions, 25 nations, 10 suppliers, 150 customers, 200 parts,
800 partsupp rows, 1,500 orders and 6,005 lineitems. Smaller generator factors can
collapse tables to one row; they are deliberately not offered as validation scales.
Each query gets fresh equivalent databases. Loading is separate from the CoreSQL
query timeout; the reference worker's safety limit includes its setup. These guards
are not benchmark timings. A timed-out native process is killed and reaped before
continuing, so one slow query does not hide the remainder of the query set.

All 22 statuses are reported: `match`, `unsupported`, `error`, `wrong_answer`,
`timeout`, `crash`, or a distinct reference failure. Dataset/query pins and the
bridge binary hash identify each run. DECIMAL cells must remain decimals and equal
exactly; REAL cells must remain REAL and use `1e-10` relative/absolute tolerance.
Integers, dates, strings, NULLs, projection widths, row counts and row order are
exact. The current order check is conservative even for SQL ties; investigate a
mismatch before diagnosing an engine defect. There is no timing score and no
silent query rewrite or omission.

Keep the generated validation report and dataset manifest with any published measurements.

## Nonempty derived-relation and CTE checks

```sh
/tmp/coresql-tpch-python/bin/python benchmarks/tpch/check_relations.py \
  --bridge build/release/coresql_slt_bridge \
  --queries /tmp/coresql-tpch-queries
```

This optional pinned-DuckDB check asserts explicit, nonempty answers for Q7–9,
Q13, Q15, Q18 and Q22, including Unicode substring unit coverage separately in
the native suite. It is a synthetic correctness fixture, not a benchmark run.


## Bounded performance comparison

`measure.py` measures single-thread, in-memory **client latency** against the
pinned DuckDB reference, with an optional pinned SQLite comparison. See the
[latest table](../../README.md#performance) and
[raw SF 0.03 results](results/tpch-2026-09-23-sf0.03.json), including failures
and resource limits.
This is an engineering comparison, not an official TPC-H score.

```sh
/tmp/coresql-tpch-python/bin/python benchmarks/tpch/prepare.py \
  --output /tmp/coresql-tpch-perf-data --scale 0.03
/tmp/coresql-tpch-python/bin/python benchmarks/tpch/measure.py \
  --data /tmp/coresql-tpch-perf-data --queries /tmp/coresql-tpch-queries \
  --bridge build/release/coresql_slt_bridge \
  --timeout 15 --load-timeout 120 --memory-mb 2048 --repeats 3 \
  --output /tmp/coresql-tpch-performance.json
```

Run the timing supervisor from a Git checkout; it records HEAD and worktree status
and does not accept a source archive without Git metadata.
The current supervisor uses macOS `sysctl` for host metadata and `ps` for process
memory monitoring. It needs permission to read process counters. Engines run
serially, DuckDB then CoreSQL for each query, with fresh equivalent databases.
An untimed first execution precedes three measured executions; loading and result
comparisons are excluded. Every measured result is checked against that engine's
first result, which is then compared across engines using the existing validation
policy. A timeout or failure never receives a successful comparison ratio.

Timings include SQL parsing/binding, execution and fetching all rows into Python.
CoreSQL additionally pays for bridge serialization, pipe I/O and decoding;
DuckDB uses its native Python binding. This is **not engine-only timing** and
submillisecond results are particularly sensitive to driver overhead and noise.
No persistent prepared statement is used. Both engines use native DATE/DECIMAL,
no indexes or constraints, and in-memory storage. DuckDB has a 1 GB memory limit
and disk spill disabled. No durability, load throughput, or cold-start claim is made.

The supervisor enforces a timeout separately for loading, the first execution and
each repetition. A sampled 2 GB process-group RSS limit includes the Python worker
and CoreSQL bridge; it is a safety guard, not a hard allocation cap. Sampling is
approximately every 100 ms and can miss brief memory peaks. Workers and child
bridges are killed and reaped on failure/timeout before the next engine starts.
Raw reports preserve repetitions, load times, sampled memory, correctness status,
query/data/binary/driver fingerprints and machine details. They are saved after
every completed query pair, so interruption retains partial results.

### Include the pinned SQLite reference

SQLite requires a disclosed dialect/type adaptation for these pinned queries:
DATE columns become ISO TEXT, DATE literals/casts become ISO strings,
`extract(year FROM column)` becomes integer `strftime('%Y', column)`, and
`substring(column FROM start FOR length)` becomes `substr(column,start,length)`.
Q13’s derived-table column-name list becomes a SELECT output alias.
DECIMAL(15,2) becomes **approximate REAL**. CoreSQL and DuckDB keep their unchanged
SQL and exact native DECIMAL. This is an adapted SQLite comparison, not equivalent
numeric type support or an official TPC-H implementation.

Build the unchanged amalgamation from the [verified reference setup](../reference/README.md)
into a scratch shared library (macOS):

```sh
cc -O3 -DNDEBUG -dynamiclib \
  /tmp/coresql-sqlite-reference/amalgamation/sqlite3.c \
  -o /tmp/coresql-tpch-sqlite.dylib
/tmp/coresql-tpch-python/bin/python benchmarks/tpch/measure.py \
  --data /tmp/coresql-tpch-perf-data --queries /tmp/coresql-tpch-queries \
  --bridge build/release/coresql_slt_bridge \
  --engines duckdb sqlite coresql --sqlite-library /tmp/coresql-tpch-sqlite.dylib \
  --timeout 15 --memory-mb 2048 --repeats 3 \
  --output /tmp/coresql-tpch-three-engines.json
```

The original `sqlite_adapter.py` uses SQLite's C API through ctypes, checks its
Fossil source identifier against the repository pin, and prepares/finalizes each
statement. No Python statement cache is used. The driver keeps automatic transient
indexes enabled, selects in-memory temporary storage and case-sensitive LIKE, and
disables helper threads. No user indexes or key constraints are added.

SQLite results are checked against DuckDB with exact row counts, row order,
integers, text and NULLs; numeric DECIMAL/REAL reference cells use `1e-10` relative
and absolute tolerance. This approximate check cannot establish exact decimal
semantics. Ratios are withheld for resource limits, errors or answer mismatches.
Each output includes engine-specific correctness, adapted-query hashes, SQLite
library/adapter fingerprints and explicit engine order. The raw field
`sqlite_over_coresql` means **SQLite median / CoreSQL median**: above 1 is a
CoreSQL speedup, below 1 means CoreSQL is slower. The three drivers have different
result-conversion overhead, so this remains a client-latency comparison.

See [the latest three-engine table](../../README.md#performance).

### Inspect execution work

Test-enabled builds expose opt-in, thread-local execution counters through the
private SQLLogicTest bridge. `CORESQL_PROFILE=1` emits a counter JSON record and per-stage records on stderr
per statement, including statements that return a SQL error. Setting
`CORESQL_PROFILE_QUERY` to exact SQL text selects just that statement (also used
by the allocation bridge). Stage records report rows, columns, row/value vector
capacity bytes and match vector capacity bytes; they exclude dynamic payloads,
shared chunks, indexes and allocator overhead. Normal stdout stays
unchanged. Counters are inactive by default and are absent from production builds.
They are diagnostic totals, not a stable public API or a complete EXPLAIN facility:

- `rows_tested`: visits to scan acceptance and planner key-NULL checks. A row can
  be visited at multiple stages; this is not the number of distinct input rows.
- `candidate_pairs`: row pairs examined by join loops, or returned by an indexed
  join lookup, before join/WHERE rejection.
- `hash_build_rows`: right-side rows visited while building integer join lookups,
  including NULL keys which are excluded from the buckets.
- `hash_probes`: non-NULL left keys looked up in those query-local hash tables.
- `intermediate_rows`: total rows passed through query-local materialization.
- `largest_intermediate`: largest single materialized row set, not peak live
  allocation or total query memory. Result/projection buffers are not included.
- `subquery_executions`: actual scalar, EXISTS and IN subquery executions,
  including the grouped-IN planner; shape probes and cache hits are excluded.
  CTE/derived relation evaluation is not counted as an expression subquery.

The bounded diagnostic runner captures the last completed query's counters:

```sh
/tmp/coresql-tpch-python/bin/python benchmarks/tpch/profile_query.py \
  --data /tmp/coresql-tpch-perf-data --queries /tmp/coresql-tpch-queries \
  --bridge build/release/coresql_slt_bridge --query q19.sql \
  --output /tmp/coresql-q19-profile.json
```

Run timings separately with `CORESQL_PROFILE` unset. `measure.py --only q19.sql`
selects a pinned query while still verifying the full query manifest. Reports
record the selected set; `table.py` refuses a partial set as a complete 22-query
comparison.

The profiler also reports `correlation_index_rows` (rows indexed for correlated
lookups) and `subquery_cache_hits` (successful repeatable scalar/EXISTS reuse).
The optional `memory.py` and `coresql_allocation_bridge` diagnostics are separate
from standard timings and are not production memory accounting.

# Curated SQL performance and coverage suite

This suite complements the unchanged 22-query TPC-H comparison with full
speedtest1 `main` SQL runs at multiple sizes, five additional speedtest1 query
shapes, ten DuckDB microbenchmarks, all ten H2O grouping shapes and all five H2O
join shapes. Seven additional join variants retain equivalent explicit `ON`/column
lists as controls alongside the original `USING`/qualified-star queries. Three
original controls cover wide general ON joins, equality joins and simple CASE
with NULL input.

There are **40 curated cases**, plus **32 main cases at each requested size**
and **22 TPC-H queries**. This is a reduced engineering suite, not official TPC-H,
SQLite, DuckDB or H2O benchmark results, and not exhaustive SQL coverage.

## Reproduce

Run from the repository root. Dependencies are optional: Python 3.10+, DuckDB
**1.5.5 / d8cdaa33fd**, and the repository's pinned SQLite source. No packages are
installed by the runner. Reuse existing verified inputs or prepare them:

```sh
python3 benchmarks/reference/prepare.py --output /tmp/coresql-sqlite-reference
cmake -S . -B /tmp/coresql-suite-release -DCMAKE_BUILD_TYPE=Release \
  -DCORESQL_BENCHMARKS=ON -DBUILD_TESTING=ON \
  -C /tmp/coresql-sqlite-reference/reference.cmake
cmake --build /tmp/coresql-suite-release -j 4
ctest --test-dir /tmp/coresql-suite-release --output-on-failure

python3 -m venv /tmp/coresql-suite-python
/tmp/coresql-suite-python/bin/pip install duckdb==1.5.5
/tmp/coresql-suite-python/bin/python benchmarks/suite/test_suite.py
```

Build a shared library from that same amalgamation for the Python SQLite C API
adapter. On macOS:

```sh
cc -O3 -DNDEBUG -DSQLITE_THREADSAFE=0 -dynamiclib \
  /tmp/coresql-sqlite-reference/amalgamation/sqlite3.c \
  -o /tmp/coresql-sqlite-reference/libsqlite3.dylib
```

On Linux use `-shared -fPIC` instead of `-dynamiclib`, add `-lm -ldl`, and use a
`.so` output path. The adapter verifies SQLite's source ID before running.
The curated runner uses POSIX process groups; the retained TPC-H resource
monitor additionally uses macOS `ps`/`sysctl`.
Prepare the retained TPC-H component following its [guide](../tpch/README.md):

```sh
/tmp/coresql-suite-python/bin/python benchmarks/tpch/prepare.py \
  --output /tmp/coresql-suite-tpch-data --scale 0.03 --fetch-extension
python3 benchmarks/tpch/audit.py \
  --bridge /tmp/coresql-suite-release/coresql_slt_bridge \
  --queries /tmp/coresql-suite-tpch-queries --fetch \
  --output /tmp/coresql-suite-tpch-audit.json

/tmp/coresql-suite-python/bin/python benchmarks/suite/bench.py \
  --bridge /tmp/coresql-suite-release/coresql_slt_bridge \
  --sqlite-library /tmp/coresql-sqlite-reference/libsqlite3.dylib \
  --sources /tmp/coresql-suite-sources --fetch \
  --rows 10000 --repeats 3 \
  --speedtest /tmp/coresql-suite-release/coresql_speedtest1_main \
  --speedtest-sizes 1 10 \
  --tpch-data /tmp/coresql-suite-tpch-data \
  --tpch-queries /tmp/coresql-suite-tpch-queries \
  --output /tmp/coresql-suite-results.json
```

The runner writes incremental JSON, a neighboring Markdown table, and a separate
TPC-H report. Use a new output name for each run. Sources are fetched from immutable
revisions only when missing; every source is SHA-256 verified even offline.
`--only` selects exact curated IDs for diagnosis; omit TPC-H paths and `--speedtest`
for a curated-only run. To reuse a completed TPC-H pass from the same executable
and engine source tree, pass `--tpch-report /path/to/report-tpch.json` instead of
the two input paths. Binary/source hashes, reference pins and all 22 query outcomes
are checked, and the reused report timestamp/hash are recorded. `--rows 100 --repeats 1` is a correctness smoke run; increase
to 100,000 rows for a larger operator workload. Counts must be divisible by four.

The process exits **1 if any selected case is unsupported, fails, times out,
returns a wrong answer or fails its diagnostic pass**. The report is still written
and later cases still run. This deliberately keeps capability gaps visible. An
`error` is the engine's actual classification (e.g. an unregistered function);
it is not silently reclassified as an expected skip. No successful performance
claim is made for a failed case. TPC-H keeps all 22 original pinned queries.

## Measurement and correctness

- Each curated case/engine loads a fresh in-memory database. Engine order
  alternates across cases; execution is serial. One untimed execution precedes
  the measured repetitions. Every timed answer must equal its warmup; complete
  results are then compared to an independent reference outside the timers.
- speedtest1 additions use pinned SQLite. DuckDB/H2O and original controls use
  pinned DuckDB with one thread, a 1 GB memory limit and disk spilling disabled.
  No new indexes are added except the upstream star and FP index fixtures.
- Integer, text, NULL, result widths and row multiplicity compare exactly; REAL
  comparisons use absolute and relative tolerance `1e-10`. Unordered queries
  compare sorted complete row multisets. Sort cases have deterministic ties.
  Reference checks and result hashes are retained; full answer arrays are omitted
  from final reports to keep them manageable.
- Curated timing measures SQL parse/execute/fetch into Python. CoreSQL additionally
  pays for bridge serialization, IPC and decoding. DuckDB uses its Python API,
  SQLite a ctypes C API. These are **client latency**, not directly comparable
  isolated execution-kernel timings. Large text results can be dominated by
  transport. Full speedtest1 uses its existing C++ driver and reports separate
  preparation/execution phases; see its [methodology](../speedtest1.md).
- Loading is recorded separately, including fixture generation and index creation.
  Worker peak RSS and CoreSQL bridge peak RSS come from fresh-process `getrusage`;
  they include loading and cannot be added to obtain a simultaneous group peak.
  No allocator accounting or durable I/O claim is implied.
- Each curated engine/case has a default 60-second wall limit including startup,
  loading, warmup and all repetitions. A timeout kills the entire process group.
  This is a time bound, not a CoreSQL memory limit. speedtest1 has a separate
  300-second limit per size. TPC-H retains its existing per-phase time/RSS bounds.
- A separate untimed CoreSQL execution collects query counters and stage details,
  with another reference comparison. `join_pairs` reports whether borrowed pair
  execution was reached. Source, executable, workload, input and result hashes
  identify the run, including an uncommitted implementation. Diagnostic counters
  are removed from timed worker environments.

## Adaptations and limitations

The JSON catalogs record each SQL statement, source and changes. Reduced fixtures
are deterministic (fixed 32-bit LCG seed 42) and identical across engines, but are
not upstream random generators or published H2O datasets. H2O SELECTs return all
rows instead of timing `CREATE TABLE ans`; no aggregates/window operators are
removed. Original syntax probes and executable join adaptations appear separately.
H2O grouping uses 100 low-cardinality keys and max(10,N/100) higher-cardinality
keys. Joins have N probe rows, max(10,N/100) small rows, 90% of max(20,N/10) medium
keys, and N big rows; missing medium keys exercise outer-join NULL extension.
Group fixtures are uniform and non-NULL; the separate CASE control includes NULLs.
Skew, concurrency, durability and very large spill workloads remain future work.

speedtest1 star retains both queries, eight dimension sizes and index definitions;
fact rows are max(50,N/20). IDs ending in 0 or 9 have matching keys in all
dimensions to guarantee nonempty fanout and filtered results at small scales;
other fact rows retain signed random keys. FP adds scan/indexed ranges and the rounding query with
deterministic dyadic REAL inputs, not the entire upstream FP testset. Setup is not
included in their query timings. The original 32-case adapter is unchanged.

DuckDB cases retain SUM/grouping, distinct aggregates, CASE branches, casts,
ordering, top-N and join shapes. Row counts and top-N keys/CASE thresholds scale
down. The duplicate string join retains four small-side duplicates but reduces
large-side fanout to 16. Unsupported PRODUCT remains in the catalog.

Origins and original license texts are recorded in [sources.json](sources.json),
[licenses](licenses/) and the root [PROVENANCE](../../PROVENANCE.md).
`h2o.json` is an adaptation under MPL-2.0, not the root CoreSQL license.

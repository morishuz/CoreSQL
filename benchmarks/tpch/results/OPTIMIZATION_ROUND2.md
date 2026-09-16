# CoreSQL, SQLite and DuckDB: TPC-H workload

These are single-thread, in-memory client-latency measurements on the same Apple M1 (16 GiB) desktop, with three timed executions after one untimed execution. Each engine/query starts with a fresh database. Loading and answer checks are excluded; SQL parsing and fetching all results are included. No persistent prepared statements or user-created indexes are used.

**SQLite uses an adapted precision profile:** ISO TEXT dates and approximate REAL decimals; DATE, EXTRACT, SUBSTRING and Q13 derived-column aliases receive explicit syntax adaptations. CoreSQL and DuckDB keep the original pinned queries and native DATE/DECIMAL. SQLite results match within 1e-10 relative/absolute numeric tolerance; this does not establish exact-decimal equivalence.

CoreSQL uses its pipe bridge, SQLite the pinned C library via ctypes, and DuckDB its Python binding. Different conversion overhead affects short queries. SQLite keeps its automatic transient indexes enabled and uses case-sensitive LIKE and in-memory temporary storage. CoreSQL uses the existing test-enabled Release build; SQLite is compiled with `cc -O3 -DNDEBUG -dynamiclib`. This is an engineering comparison, not an official TPC-H score.

All runs use 15-second per-execution and 120-second load limits, and an approximately 100 ms sampled 2 GiB process-group RSS stopping threshold (which can overshoot). DuckDB additionally has a 1 GB internal memory limit and spill disabled. Resource-limited results receive no ratio.

The ratio is **SQLite median time / CoreSQL median time**: 2× means CoreSQL is twice as fast; 0.5× means CoreSQL takes twice as long. Ratios compare these disclosed profiles, not identical numeric implementations.

## SF 0.03

Median milliseconds; SQLite/CoreSQL > 1 means CoreSQL is faster.

| Query | CoreSQL ms | SQLite ms | DuckDB ms | SQLite/CoreSQL |
| --- | ---: | ---: | ---: | ---: |
| 01 | 175.283 | 90.511 | 6.211 | 0.516× |
| 02 | 21.437 | 19.803 | 1.969 | 0.924× |
| 03 | 32.464 | 48.350 | 1.425 | 1.49× |
| 04 | 19.690 | 37.819 | 1.912 | 1.92× |
| 05 | 39.436 | 70.774 | 2.877 | 1.79× |
| 06 | 29.702 | 11.726 | 0.556 | 0.395× |
| 07 | 64.957 | 56.954 | 2.850 | 0.877× |
| 08 | 34.362 | 129.663 | 2.569 | 3.77× |
| 09 | 57.243 | 158.060 | 5.511 | 2.76× |
| 10 | 24.927 | 18.897 | 4.193 | 0.758× |
| 11 | 5.360 | 25.208 | 2.340 | 4.7× |
| 12 | 30.588 | 23.301 | 3.698 | 0.762× |
| 13 | 38.574 | 46.479 | 5.057 | 1.2× |
| 14 | 20.049 | 11.768 | 0.966 | 0.587× |
| 15 | 20.177 | 11.745 | 0.958 | 0.582× |
| 16 | 8.206 | 10.973 | 1.947 | 1.34× |
| 17 | 13.022 | 801.836 | 1.462 | 61.6× |
| 18 | 62.454 | 75.183 | 4.080 | 1.2× |
| 19 | 14.256 | timeout | 5.512 | — |
| 20 | 13.201 | 1251.266 | 2.186 | 94.8× |
| 21 | 600.516 | timeout | 4.630 | — |
| 22 | 8.364 | 336.354 | 1.360 | 40.2× |

— means no valid completed comparison; it is not a zero runtime.

- coresql: 22/22 completed answer checks.
- sqlite: 20/22 completed answer checks.
- duckdb: 22/22 completed answer checks.

Measured 2026-09-15T10:01:54.839634+00:00; engine order: duckdb, sqlite, coresql.
CoreSQL modified working tree based on `2d3342712b`; SQLite 3.54.0; DuckDB 1.5.5.
Raw evidence: [optimization-round2-sf0.03.json](optimization-round2-sf0.03.json).

## Reproduce

See [runner, reference build and adaptation instructions](../README.md#include-the-pinned-sqlite-reference). Raw JSON records all repetitions, per-engine answer checks, memory, limits and binary/query/adapter fingerprints. The generic raw `schema` field describes CoreSQL/DuckDB; `sqlite.profile` records SQLite’s different schema and settings.

Regenerate these tables with `python3 benchmarks/tpch/table.py benchmarks/tpch/results/optimization-round2-sf0.03.json --output benchmarks/tpch/results/OPTIMIZATION_ROUND2.md`.

## Outcome and scope

CoreSQL and DuckDB match all 22 native queries. SQLite completes 20 adapted
comparisons; Q19 and Q21 time out. CoreSQL is faster in 12 of the 20 completed
SQLite comparisons. DuckDB remains faster on all 22. Incomplete queries receive
no ratio, and near-parity differences are not a stable ranking from three trials.

Q8's 34.362 ms here was not reproduced in an alternating original/current binary
check: both versions measured about 23 ms. This table retains the original full-run
measurement. The [alternating Q8 check](optimization-round2/optimization-q8-check.json)
retains the paired evidence. Historical measurements are not timings of the current checkout.

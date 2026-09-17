# Type modularization: Q1–Q22 benchmark, 2026-09-17

Follow-up: [regression fixes and final validation](TYPE_MODULARIZATION_FIXES_20260917.md).

Performance has regressed in several queries. The committed baseline completes and matches all 22 queries; the current uncommitted implementation matches 21, with Q9 exceeding the 2 GiB process-group memory limit during warmup. Q7, Q8 and Q17 slowdowns persist in a reverse-order recheck.

## Method

- SF 0.03: 180,566 lineitems. The generated dataset manifest matches the historical run byte for byte.
- Apple M1, 16 GiB; macOS 27.0; Python 3.14.2. Same compiler and Release flags for both builds: `-O3 -DNDEBUG`, C++20, test-enabled bridge.
- Baseline: fresh build from `git archive HEAD` at `d4982285738cf609bb27804cbb76ad760aec7753`. Current: the existing uncommitted type-adapter and source-layout changes, freshly rebuilt.
- Full runs: baseline then current; DuckDB then CoreSQL per query; fresh in-memory database per query/engine, one warmup, three measured executions, one thread. Five measured executions for the four-query recheck, current then baseline.
- Pinned DuckDB 1.5.5 checks answers using the original pinned queries. SQLite was not rerun.
- SQL-to-fetched-Python-row latency, including parsing, binding and bridge IPC/serialization. Loading is excluded from query times. No persistent prepared statements, indexes or constraints.
- Limits: 15 seconds per execution, 120 seconds loading, sampled 2 GiB process-group RSS; DuckDB internal memory limit 1 GB, no spill. This is an engineering benchmark, not an official TPC-H score.
- Small differences are not evidence of an improvement: only three main-run samples, no CPU isolation. The historical run used an older OS and different source fingerprint; the same-session baseline is the primary comparison.

## Full run

Median milliseconds. Positive change means slower. Historical timings are included for context only.

| Query | Historical ms | Baseline ms | Current ms | Change | Current answer |
| --- | ---: | ---: | ---: | ---: | --- |
| Q01 | 175.283 | 178.790 | 181.435 | +1.5% | Match |
| Q02 | 21.437 | 22.547 | 19.409 | -13.9% | Match |
| Q03 | 32.464 | 32.056 | 31.709 | -1.1% | Match |
| Q04 | 19.690 | 19.014 | 19.258 | +1.3% | Match |
| Q05 | 39.436 | 38.095 | 37.751 | -0.9% | Match |
| Q06 | 29.702 | 28.417 | 28.993 | +2.0% | Match |
| Q07 | 64.957 | 60.017 | 98.398 | +63.9% | Match |
| Q08 | 34.362 | 21.730 | 35.106 | +61.6% | Match |
| Q09 | 57.243 | 60.690 | Memory limit | — | Not verified |
| Q10 | 24.927 | 25.323 | 25.850 | +2.1% | Match |
| Q11 | 5.360 | 6.932 | 5.031 | -27.4% | Match |
| Q12 | 30.588 | 26.837 | 25.384 | -5.4% | Match |
| Q13 | 38.574 | 44.711 | 40.731 | -8.9% | Match |
| Q14 | 20.049 | 20.385 | 20.758 | +1.8% | Match |
| Q15 | 20.177 | 20.685 | 20.415 | -1.3% | Match |
| Q16 | 8.206 | 8.084 | 7.866 | -2.7% | Match |
| Q17 | 13.022 | 12.671 | 15.500 | +22.3% | Match |
| Q18 | 62.454 | 60.231 | 61.186 | +1.6% | Match |
| Q19 | 14.256 | 13.961 | 14.386 | +3.0% | Match |
| Q20 | 13.201 | 13.771 | 13.845 | +0.5% | Match |
| Q21 | 600.516 | 600.954 | 575.403 | -4.3% | Match |
| Q22 | 8.364 | 8.165 | 7.894 | -3.3% | Match |

For the 21 queries completed by both builds only, the sum of medians rises from 1263.374 to 1286.310 ms (+1.8%). The geometric mean latency ratio is 1.027×. Neither is a complete Q1–Q22 score: both exclude the failed Q9, and Q21 dominates the sum.

## Reverse-order confirmation

| Query | Baseline ms | Current ms | Change |
| --- | ---: | ---: | ---: |
| Q07 | 59.530 | 94.733 | +59.1% |
| Q08 | 24.047 | 33.743 | +40.3% |
| Q09 | 60.150 | Memory limit again | — |
| Q17 | 12.738 | 14.313 | +12.4% |

## Loading and diagnostics

Average CoreSQL load time also increased from 3.582 to 5.415 seconds (+51.2%). This is separate from query latency; its cause has not been isolated.

The likely cause of the EXTRACT-query regression is in `sql/relations.cpp`: `Lowerer::repeatable` calls `types->operation(n.name, {})` without input types. The DATE operation in `addons/date/sql.cpp` accepts `sql.extract.year` only with one DATE (or untyped NULL) input. Consequently the repeatability probe fails even though the resolved operation explicitly declares itself repeatable. Q7, Q8 and Q9 all use EXTRACT.

`src/query_reduce.cpp` materializes nonrepeatable aggregate input instead of streaming it; `src/query_join.cpp` also gates final-stage streaming on repeatability. Separate Q8 diagnostic runs retain identical candidate-pair, hash-build and hash-probe counts, but intermediate rows rise from 15,244 to 28,883. This supports a materialization/streaming regression, rather than more join candidates. It is not a controlled fix experiment, so causality remains to be verified after a repair. Q17 and loading need separate investigation.

Recommended next step: make repeatability analysis resolve adapter operations with the actual input types, without reintroducing DATE-specific handling in shared SQL code; add an EXTRACT planner regression test and repeat this benchmark. No engine code was changed during this measurement.

## Reproduction and provenance

Run `benchmarks/tpch/prepare.py --output <new-data-dir> --scale 0.03` using `benchmarks/tpch/requirements.txt`; obtain the 22 hash-verified queries described in the benchmark README. Build both bridges in Release. For each source tree run its own harness:

```sh
python <source>/benchmarks/tpch/measure.py --data <data-dir> --queries <query-dir> --bridge <bridge> --output <report.json>
```

For the recheck add `--only q07.sql q08.sql q09.sql q17.sql --repeats 5`. Diagnostics use `profile_query.py --query q08.sql` with the same data, queries and bridge arguments.

The baseline archive lives under the parent checkout’s ignored `build/` directory. Its raw `worktree_dirty: true` describes the enclosing checkout, not modifications to the archived baseline. Its source fingerprint is computed from the baseline archive itself. Both reports record the same HEAD because current changes are uncommitted.

| Build | Source SHA-256 | Bridge SHA-256 |
| --- | --- | --- |
| Baseline | `a7e0a77b39b34d277875ffaabe1238c349a6f59fcc32bd1c929b2d1d80281b2f` | `63db4f9f50e1a300887fdb2c90bdad8a55924d377165d2c1d2ad9f92d8ac59f7` |
| Current | `4f49e1972fa1e7e2950123d12a4d3d472b3c8da5cb3321bf477c067cdc19b8c4` | `4027291d3d63dfbf494588a2d0add34cf6a6cfba289dba6def9cfef92f83b4b2` |

Raw results (all samples, checks and resource measurements):

- [baseline-sf0.03](type-modularization-20260917-baseline-sf0.03.json)
- [current-sf0.03](type-modularization-20260917-current-sf0.03.json)
- [baseline-recheck](type-modularization-20260917-baseline-recheck.json)
- [current-recheck](type-modularization-20260917-current-recheck.json)
- [baseline-q08-profile](type-modularization-20260917-baseline-q08-profile.json)
- [current-q08-profile](type-modularization-20260917-current-q08-profile.json)

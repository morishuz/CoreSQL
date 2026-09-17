# Type-adapter regression fixes — 2026-09-17

All 22 queries now complete and match DuckDB. The Q7/Q8 slowdowns and Q9 memory-limit failure are resolved. Loading is also faster than the committed baseline. The earlier larger Q17 slowdown no longer reproduces in a fresh nine-sample comparison.

## Changes and evidence

1. Resolve adapter operation repeatability with actual argument types. Analyze nested SELECTs in their own scopes without adding captures to the executing query. Tests exercise DATE, custom timestamp extraction, aggregate streaming, integer-key correlated reuse, and conservative behavior for operations that do not opt in. The isolated first fix restored Q7/Q8/Q9 to 60.485/23.144/57.931 ms; Q9 had previously exceeded the memory limit.
2. Elide matching-type conversions in stored expressions. Retain explicit conversions used to reconcile compound results. Keep exact DECIMAL numeric spelling and typed NULL behavior.
3. Add single-pass `try_convert`: no matching adapter returns `nullopt`; ambiguity, callback errors and wrong output types still throw. Existing `convert` remains the throwing interface. Tests verify one claim-resolution pass and retained error checks.
4. Avoid copying operand expression trees just to select an adapter: binary/unary dispatch borrows existing expressions, while membership and CASE dispatch pass inferred types.
5. CPU sampling of loading showed remaining work in temporary conversion calls. The CSV loader supplies every field as text, so matching-type shortcuts alone were insufficient. Constant INSERT values now evaluate and validate their source expression and convert directly through the adapters, preserving per-cell conversion order and exact numeric input. They no longer construct and bind a temporary SQL conversion call. Normal row validation, atomicity and transaction behavior remain in place.

## Full Q1–Q22 comparison

SF 0.03, 180,566 lineitems; Apple M1, 16 GiB, macOS 27.0; identical Release compiler flags and pinned data/query/harness hashes. One untimed warmup and three timed executions per query, fresh in-memory database per query/engine, single thread. Times include SQL parsing, binding, fetching, and CoreSQL bridge IPC. Loading is excluded. Limits remain 15 seconds per execution, 120 seconds loading, and sampled 2 GiB process-group RSS. This is not an official TPC-H score.

Baseline is the committed pre-refactor source. “Regressed” is the previous uncommitted adapter implementation; “fixed” includes this repair. These runs were sequential, not CPU-isolated; small differences should be treated as timing variation. See the [original report](TYPE_MODULARIZATION_20260917.md) for baseline archive provenance.

| Query | Baseline ms | Regressed ms | Fixed ms | Fixed vs baseline |
| --- | ---: | ---: | ---: | ---: |
| Q01 | 178.790 | 181.435 | 181.970 | +1.8% |
| Q02 | 22.547 | 19.409 | 20.214 | -10.3% |
| Q03 | 32.056 | 31.709 | 32.532 | +1.5% |
| Q04 | 19.014 | 19.258 | 19.583 | +3.0% |
| Q05 | 38.095 | 37.751 | 37.001 | -2.9% |
| Q06 | 28.417 | 28.993 | 27.818 | -2.1% |
| Q07 | 60.017 | 98.398 | 59.316 | -1.2% |
| Q08 | 21.730 | 35.106 | 22.246 | +2.4% |
| Q09 | 60.690 | Memory limit | 58.812 | -3.1% |
| Q10 | 25.323 | 25.850 | 25.550 | +0.9% |
| Q11 | 6.932 | 5.031 | 5.652 | -18.5% |
| Q12 | 26.837 | 25.384 | 26.512 | -1.2% |
| Q13 | 44.711 | 40.731 | 39.384 | -11.9% |
| Q14 | 20.385 | 20.758 | 20.978 | +2.9% |
| Q15 | 20.685 | 20.415 | 20.819 | +0.6% |
| Q16 | 8.084 | 7.866 | 7.962 | -1.5% |
| Q17 | 12.671 | 15.500 | 13.608 | +7.4% |
| Q18 | 60.231 | 61.186 | 62.321 | +3.5% |
| Q19 | 13.961 | 14.386 | 14.375 | +3.0% |
| Q20 | 13.771 | 13.845 | 13.006 | -5.6% |
| Q21 | 600.954 | 575.403 | 583.585 | -2.9% |
| Q22 | 8.165 | 7.894 | 8.167 | +0.0% |

- Sum of all 22 medians: 1324.064 → 1301.411 ms (-1.7%).
- Geometric mean latency ratio across all 22 queries: 0.983× baseline. These aggregates describe this workload only; the sum is dominated by Q21.
- Average loading: baseline 3.582 s; regressed 5.415 s; fixed 3.246 s. Fixed is 9.4% faster than baseline and 40.1% faster than the regressed version.
- Fixed Q9 peak sampled process-group RSS: 639.5 MiB (includes Python and loading); the regressed build exceeded 2 GiB twice.

## Q17 investigation

Fresh reverse-order comparison (fixed then baseline, nine measured executions each): **13.125 ms fixed versus 12.994 ms baseline**, a 1.0% difference. Both match DuckDB. Separate diagnostic runs have identical execution counters: four subquery executions, 99 cache hits, 180,566 correlation-index rows, 103 candidate pairs, and five intermediate rows. The previous 12–22% slowdown is not reproduced; these results do not isolate a single cause for that earlier timing difference.

## Validation

- Release: 68/68 tests passed, including the added streaming/correlation and conversion checks.
- Installed production package: relocation, core/add-on/custom-type consumers, version/component rejection, persistence and backup/restore checks passed.
- ASan/UBSan: 68/68 tests passed (Debug sanitizer build).
- `git diff --check`: passed. No commits, pushes, or visibility changes.

## Reproduction and raw evidence

Use the original report’s pinned data preparation and Release build instructions, then:

```sh
python benchmarks/tpch/measure.py --data <data-dir> --queries <query-dir> --bridge <fixed-bridge> --output <fixed.json>
```

For Q17 add `--only q17.sql --repeats 9`. Run `profile_query.py --query q17.sql` separately for execution counters; profiling results are not timings. The source fingerprint below was checked against the measured implementation.
The subsequent release audit corrected a public-header comment and documentation
only; the raw hashes continue to identify the measured source snapshot.

- Fixed source SHA-256: `bf8ad627dd50898dcdb6ed465edc8ba67846edd669b16c1ca083a8fe67410150`.
- Fixed bridge SHA-256: `5544bbaede7418544354734a788e823bc52a4ee1dffb2a2e52578ee5a3a028bc`.

- [step1](type-modularization-20260917-step1.json)
- [step2](type-modularization-20260917-step2.json)
- [step3](type-modularization-20260917-step3.json)
- [fixed-sf0.03](type-modularization-20260917-fixed-sf0.03.json)
- [fixed-q17-recheck](type-modularization-20260917-fixed-q17-recheck.json)
- [baseline-q17-recheck](type-modularization-20260917-baseline-q17-recheck.json)
- [fixed-q17-profile](type-modularization-20260917-fixed-q17-profile.json)
- [baseline-q17-profile](type-modularization-20260917-baseline-q17-profile.json)

# Pinned SQLLogicTest coverage

The corpus includes the five **complete, unchanged** upstream files
`test/select1.test` through `test/select5.test`: 8,884 queries and 1,822 setup
statements. Files are pinned by SHA-256 in `manifest.json`, with the original
license retained. Tests need no network access. This is correctness coverage,
not a performance benchmark or a claim of complete SQL support.

Source: https://www.sqlite.org/sqllogictest/
Format: https://www.sqlite.org/sqllogictest/doc/trunk/about.wiki
The raw retrieval URL for each file was `/raw/test/selectN.test?ci=trunk` on that
site. Content hashes identify the snapshot rather than the moving branch name.
Select1 was retrieved September 11, 2026; select2–5 on September 13.

## Results — September 13, 2026

| File | Setup passed | Queries passed | Queries unsupported | Failed |
| --- | ---: | ---: | ---: | ---: |
| select1.test | 31 | 1000 | 0 | 0 |
| select2.test | 31 | 1000 | 0 | 0 |
| select3.test | 31 | 3320 | 0 | 0 |
| select4.test | 1025 | 2832 | 0 | 0 |
| select5.test | 704 | 732 | 0 | 0 |
| **Total** | **1,822** | **8,884** | **0** | **0** |

Python's SQLite runtime (3.51.2) validates every stored upstream result:
all 10,706 records passed in both engines, with no blocked, skipped, unsupported or
failed records in that run.

Select2/3 exercise stored NULLs, expressions, ordering, labels and coalescing.
Select5 exercises joins through up to 64 tables. Select4 adds membership,
compound SELECT and broader joins. The separate `sql_relational_differential`
test covers grouping, aggregate expressions, HAVING, correlation, NULL and empty
set semantics, and explicit inner/cross joins against SQLite. The separate
`sql_outer_coercion_differential` test compares 251 statements covering LEFT,
RIGHT and FULL joins, aggregate DISTINCT, casts, mixed scalar expressions and
atomic storage conversions. Native extension tests verify typed NULL rows,
custom-type DISTINCT and validation before callbacks. Passing this pinned corpus
does not establish complete SQL support.

## Run and regression policy

```sh
python3 tests/sqllogictest/run.py \
  --bridge build/speedtest/coresql_slt_bridge \
  --report /tmp/coresql-sqllogictest.json
ctest --test-dir build/speedtest -R sqllogictest --output-on-failure
```

`--file upstream/select2.test` selects a pinned file; the default runs all five.
Each CTest file gets a fresh engine and a 300-second timeout. The SQLite oracle
here uses Python's standard library; speedtest1 separately uses the pinned
amalgamation. Stored upstream expectations, not SQLite or CoreSQL output generated
by this runner, remain the correctness oracle.

`passed` means exact normalized results (or expected statement errors). Only an
explicit engine unsupported error counts as `unsupported`, including for expected
error statements. Other errors/results are `failed`. An unsuccessful setup
statement blocks dependent records, avoiding misleading results from incomplete
database state. Explicit upstream conditionals can produce `skipped` records.
Failures and blocked records always exit nonzero. CTest also checks each record's
line/outcome against `baseline.json`; improvements need an explicit reviewed update.

The Python runner handles explicit/hash results, count checks, I/R/T rendering,
NULLs, sorting, comments, labels and conditionals. Unknown directives and halt fail
loudly. It is not a full upstream-runner replacement: empty-result column metadata
is unavailable, and general text-to-number coercion is not emulated. The small
C++ bridge uses public APIs and is sanitizer-instrumented when enabled.

## Scope

The [SQL contract](../../docs/contracts/sql.md) describes supported behavior and
remaining limitations. Passing these files does not establish full SQLLogicTest
or SQL-standard conformance. Additional native and differential tests cover
transactions, extension types, persistence and recovery. Python and its SQLite
library are not sanitizer-instrumented; CoreSQL and the bridge are in sanitizer
builds.

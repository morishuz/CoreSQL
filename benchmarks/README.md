# Benchmarks

Benchmark tools are optional and do not link SQLite or DuckDB into CoreSQL.
Use equivalent inputs, disclose adaptations and check answers before making
performance claims. In-memory commits do not measure durable write performance.

- [TPC-H Q1–Q22: setup, validation and measurement](tpch/README.md)
- [Retained September 15, 2026 comparison](tpch/results/OPTIMIZATION_ROUND2.md)
- [speedtest1 adapter: coverage, semantics and reproduction](speedtest1.md)
- [Checksum-pinned SQLite reference](reference/README.md)

## Native API comparisons

Run from the repository root:

```sh
python3 benchmarks/reference/prepare.py --output /tmp/coresql-sqlite-reference
cmake -S . -B build/compare -DCMAKE_BUILD_TYPE=Release -DCORESQL_BENCHMARKS=ON \
  -C /tmp/coresql-sqlite-reference/reference.cmake
cmake --build build/compare -j 4
./build/compare/coresql_compare 100000
./build/compare/coresql_primary_compare 100000
./build/compare/coresql_json_compare 10000
```

The general comparison uses identical unindexed integer/real/text rows in memory,
one warmup and seven repetitions with alternating engine order. CSV reports
medians. Its timings include result conversion and equality checks. SQLite prepares
statements outside execution timing; CoreSQL binds each query. The preparation
row compares different operations, not equivalent parser implementations.
Mutations roll back so repetitions begin with identical data.

The primary-key and JSON harnesses cover different workloads; do not combine their
numbers with the unindexed comparison. SQLite uses its normal configuration,
not a reduced feature build. Without the pinned reference configuration, CMake
can use system SQLite; that is not the published reference baseline.

## Diagnostics and evidence

Profilers and historical revision-comparison scripts remain available for developer
investigations. Scripts rebuilding old revisions require full Git history; normal
reference setup and native API comparisons work with shallow clones/source archives.
The TPC-H timing supervisor currently requires a Git checkout and macOS tools for
revision and host metadata; it is not a portable source-archive timing runner.
Run profiling separately from timed comparisons.

Retain raw repetitions, correctness results, source/binary fingerprints, dataset
hashes and environment details with any published table. Report failures and
resource limits without assigning speed ratios. The retained TPC-H evidence
supports the README's historical claim; rerun on an exact release revision before
claiming release performance.

Older research notes, optimization reports and intermediate measurements remain
in the [development archive](../PROVENANCE.md#source-only-main-branch--2026-09-16),
including commit `e93b6b8904`. They are not current API contracts or
current performance claims.

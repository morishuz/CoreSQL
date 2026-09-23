# Benchmarks

Benchmark tools are optional and do not link SQLite or DuckDB into CoreSQL.
Use equivalent inputs, disclose adaptations and check answers before making
performance claims. In-memory commits do not measure durable write performance.

- [Durable operations and landmark-memory example](operational.md)
- [Combined public SQL suite: speedtest1, DuckDB micro, H2O and TPC-H](suite/README.md)
- [TPC-H Q1–Q22: setup, validation and measurement](tpch/README.md)
- [September 23, 2026 comparison](../README.md#performance) and [raw evidence](tpch/results/tpch-2026-09-23-sf0.03.json)
- [Historical September 15, 2026 comparison](tpch/results/OPTIMIZATION_ROUND2.md)
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
identifies the exact measured source revision; rerun on an exact release revision
before claiming release performance.

## General ON join profiling

`join_profile.cpp` compares general ON joins, equality joins and a scan control
using the native API. Each join input has `rows` rows, with `fanout` matches per
left row and a configurable text payload. Setup and exact result/order checks
are outside timing; each process warms up once before measured repetitions.
This is an in-memory read workload, not a durability or reference-engine comparison.

```sh
cmake -S . -B build/profile -DCMAKE_BUILD_TYPE=Release
cmake --build build/profile --target coresql -j 4
c++ -O3 -DNDEBUG -std=c++20 -I include benchmarks/join_profile.cpp \
  benchmarks/allocation_profile.cpp build/profile/libcoresql.a -o /tmp/coresql_join_profile
/tmp/coresql_join_profile general 10000 4 256 7 time
/tmp/coresql_join_profile equality 10000 4 256 7 time
/tmp/coresql_join_profile scan 10000 4 256 7 time
/tmp/coresql_join_profile general 10000 4 256 1 alloc
```

CSV fields are mode, input rows per table, fanout, text bytes, repetitions and
median query milliseconds (upper median for even repetition counts). Allocation
mode emits allocation calls and requested bytes to stderr; requested bytes are
cumulative allocation traffic, not peak live memory. Do not use allocation-mode
timings for comparisons. The diagnostic allocator is linked only into this
executable. It adds an inactive branch to allocations even in timing mode, so
compare binaries built with the same instrumentation.

For revision comparisons, retain separate binaries, alternate execution order,
and run with other build/profile jobs idle. Record source/binary fingerprints
and raw repetitions outside the tracked source tree. A process peak-RSS measure
includes setup and result ownership; report it separately from allocation traffic.

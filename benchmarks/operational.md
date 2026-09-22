# Operational workload measurements

`coresql_operational_profile` measures the landmark-memory pilot on synchronized
local storage. Use the [pinned reference setup](reference/README.md) and enable
`CORESQL_BENCHMARKS` when building. Run each size in a fresh process, serially with
other benchmarks and after all builds/tests have finished:

```sh
./build/compare/coresql_operational_profile 1000 20 > /tmp/landmarks-1000.csv
./build/compare/coresql_operational_profile 10000 20 > /tmp/landmarks-10000.csv
./build/compare/coresql_operational_profile 100000 20 > /tmp/landmarks-100000.csv
./build/compare/coresql_storage_compare coresql 2000 > /tmp/durable-core.csv
./build/compare/coresql_storage_compare sqlite 2000 > /tmp/durable-sqlite.csv
```

The operational profile reports every sample rather than only averages. It covers
initial loading/commit, filtered 128-dimensional exact vector search, repeated
prepared point reads, durable single-row and 32-row updates, mixed transactions,
four retained snapshots with whole-table updates, opening/recovery, checkpoints
and backup. Reopen timing excludes validation and closing. Read search has one
warmup; mutation, reopen and maintenance samples measure real operations without
hidden warmup writes. Single samples of initial load/backup are not percentile
estimates. Use enough repetitions and independent runs for latency-tail claims.

Correctness checks run after the measured interval. Nearest matches are checked
against an independent arithmetic oracle; writes, retained snapshots, reopening
and backup are checked for their expected contents. `integrity_check` remains
outside timing. The fixture and oracle are in
`examples/landmark_memory/workload.hpp`; schema and model interpretation are
described in the [pilot contract](../docs/applications/landmark-memory.md).

The CSV contains operation, input row count, trial, milliseconds, process peak RSS
and current main-file size. RSS is the cumulative process high-water mark,
including setup, previous phases and checks: it is not allocation accounting or
current live memory. The retained-snapshot phase deliberately raises that peak.
File size excludes temporary checkpoint coexistence. Files live in an exclusively
created temporary directory and are removed after database handles close.

`coresql_storage_compare` is a separate pinned-SQLite control with an identical
unindexed scalar-row fixture, not a vector-engine comparison. SQLite uses
DELETE journaling, synchronous EXTRA and fullfsync; CoreSQL uses its normal
synchronized commit protocol. Record the filesystem, device, OS, compiler and
reference revision. This is one explicit conservative durability profile; do not
present it as a comparison of all SQLite modes or all storage devices. Its
checkpoint/VACUUM operations have different contracts.

CoreSQL's operational SQL timings are in-process, including lowering, execution
and result construction. They do not include the Python bridge. Exact vector
search scans candidates and makes no ANN recall/performance claim. These runs do
not establish hard real-time bounds, automotive safety or production readiness.

## Repeated execution and worker soak

With benchmark targets enabled:

```sh
./build/compare/coresql_prepared_profile > /tmp/prepared.csv
./build/compare/coresql_landmark_soak 3000 > /tmp/worker-soak.csv
./build/compare/coresql_landmark_soak 3000 100000 > /tmp/worker-soak-100000.csv
```

The prepared-query profile alternates cache-enabled/disabled order across seven
trials of 10,000 in-process primary-key reads. It separately measures constant
parameters and alternating parameters, both eligible for typed template reuse.
Each trial verifies its aggregate answer and reports cache hit/miss counts.
The switch disables cache insertion as well as lookup; the comparison does not
artificially build and discard a cached entry on every uncached call. Core binding
still runs for every execution; template reuse does not eliminate that overhead.
Caching is opt-in. The template now accepts changing parameter values with the same
types and NULL shape; this comparison must be rerun when cache semantics change.

The worker soak seeds 1,000 fixed-size descriptors by default (the optional second
argument selects up to 100,000), runs configurable durable
updates/pruning and exact searches, issues bounded request bursts, and reopens the
worker ten times. Queue rejection is reported, not silently treated as successful
work. Its final check verifies every row's expected sequence, count and native
integrity. Memory samples are cumulative process high-water marks and include
verification; a flat series is useful evidence but cannot prove absence of leaks.
Use long deployment-duration runs for endurance claims.

## Queue latency and commit coalescing

```sh
./build/compare/coresql_landmark_latency 100000 512 > /tmp/landmark-latency.csv
```

This verifies 4,096 writes and 8,192 exact searches per phase using the same
100,000-row map. Each round submits eight single-observation writes followed by
16 reads, then resolves every future before the next round. Phases compare
unbatched/explicit maintenance, batched/explicit maintenance, immediate periodic
checkpoints, idle-deferred periodic checkpoints, background checkpoints with two
snapshot readers, and the same background configuration with a 16 MiB page cache.
The four original controls use serial reads and resident rows. All phases retain
synchronized commits. Explicit-maintenance and background phases can still
encounter synchronous core log-size checkpoints.
The CSV contains raw queue, execution, shared commit and total latency samples;
report sample counts and p50/p95/p99/max separately by phase/operation. Idle
checkpoint records have zero request total: use their execution time. Final state
and reopening are checked. Preparation, seed loading, startup and orderly shutdown
are outside these request samples. This closed-loop synthetic workload has no
external sensor arrival model and does not establish worst-case latency.


## Paged working sets

`coresql_paging_profile [rows=20000] [cache_mib=1] [payload_bytes=4096]`
loads bounded 256-row durable batches, checkpoints, reopens twice, streams every
row with content checks and performs 2,000 deterministic indexed lookups per open.
Run each configuration in a separate process, with builds and other benchmarks
idle, so peak RSS remains comparable:

```sh
./build/release/coresql_paging_profile 20000 0 > resident.csv
./build/release/coresql_paging_profile 20000 1 > paged.csv
```

Zero selects the resident backend. The default paged configuration has about
78 MiB of logical payload and a 1 MiB decoded-row cache. CSV includes stage wall
time, process peak RSS, cache resident/pinned bytes, page I/O counts and scratch
backing bytes. OS page cache state affects I/O timing; this is a component workload,
not a broad database ranking. The first paged reopen creates a persisted INTEGER
primary lookup cache; the second can reuse it. Both still validate durable rows.
The cache target excludes dirty transaction state, indexes, mapped pages and other
query/allocator memory; inspect RSS alongside cache counters.

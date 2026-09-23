# Application workload baseline

This optional in-process SQL harness compares CoreSQL and the pinned SQLite
reference on deterministic state-store and event-store operations. Both engines
receive the same schema, SQL, parameters and transaction boundaries. It is a
closed-loop, single-client workload, not an application trace or a concurrency,
real-time, power-loss or endurance certification.

```sh
python3 benchmarks/reference/prepare.py --output /tmp/coresql-reference
cmake -S . -B build/compare -DCMAKE_BUILD_TYPE=Release -DCORESQL_BENCHMARKS=ON \
  -C /tmp/coresql-reference/reference.cmake
cmake --build build/compare --target coresql_application_benchmark -j 4
python3 benchmarks/application/run.py --binary build/compare/coresql_application_benchmark \
  --output /tmp/application-results --operations 1000 --runs 3
```

The output directory must be new. It holds raw per-operation CSV, stderr,
source/binary fingerprints, host metadata, per-run distributions and a compact
comparison. Keep experimental measurements outside the tracked source tree.
Build options are recorded when the executable has a neighboring CMake cache.
The default `BUILD_TESTING=ON` includes inactive engine diagnostic counters; keep
that setting consistent across revision comparisons, or use a separate
`-DBUILD_TESTING=OFF` build for production-only timings.
The supervisor alternates engine order and runs fresh processes/databases
serially. Do not build, profile or run other benchmarks during timed runs.
The runner sets `TMPDIR` to the output directory, keeping CoreSQL scratch backing
and database files on the same volume. Record that filesystem and storage device
when publishing results. For direct executable runs, set `TMPDIR` explicitly.
A failed or timed-out run receives no speed ratio. The runner requires a Git
checkout for revision metadata; the executable also works from a source archive.

## Fixture and operations

The fixture contains 100 devices and configurable event rows with INTEGER primary
keys, a device ID, increasing INTEGER timestamp, INTEGER value and fixed-size TEXT
payload. Indexes cover `(device,ts)` and `ts`. Payload contents and query parameters
are deterministic; every read is checked against an arithmetic oracle.

Pass `--covering-index` to the runner (or append `covering` to the executable's
arguments) to replace `(device,ts)` with `(device,ts,id)` in **both engines**.
This optional schema variant allows CoreSQL to return event IDs and timestamps
entirely from the index. Keep its results separate from the default schema;
compare memory, ingestion/deletion, checkpoint and reopen costs as well as reads.
The manifest records the variant. Secondary CoreSQL indexes do not implicitly
include primary-key columns.

Final contents are checked with a single streaming pass after each write phase, then integrity
and persistent reopening are checked. Those checks are outside timed intervals.
They access data and therefore affect subsequent cache state.

- Primary-key reads return a value and payload, with changing keys.
- Keyset event pages and latest-event queries return up to 20 IDs/timestamps.
- Time-range aggregates count and sum 100 consecutive events.
- Small joins fetch ten events with their device labels.
- Single-row updates and changed-existing/new-key UPSERT each have their own
  transaction. A separate `upsert_unchanged` phase reapplies the current value; it
  measures no-op handling and must not be described as a changed-row write.
- Ingestion appends 16 events per transaction; retention deletes successive
  16-event timestamp ranges, stopping before half the seed data is removed.
- Mixed transactions issue eight reads of different keys and two existing-row
  updates. Persistent runs checkpoint synchronously every 50 mixed transactions.
  Mixed total latency includes that maintenance; checkpoint samples are also
  emitted separately. Reopening is measured three times after the write phases.

Statements are prepared before measured phases; CoreSQL's optional SQL template
cache is disabled. CoreSQL still lowers/binds each execution, whereas SQLite
reuses compiled statements. Timings include normal SQL execution and owned result
construction. Read parameters and expected results are built outside timing;
write parameter construction occurs inside the application transaction on both
engines. Each read phase has one unmeasured warmup. Writes have no hidden warmup.
Setup uses 256-row commits, then builds indexes, checkpoints and reopens persistent
files. The setup sample includes all of that work and is not a query latency.

## Durability and memory

Persistent CoreSQL uses its ordinary synchronized commit protocol. SQLite uses
WAL, `synchronous=FULL`, `fullfsync=ON`, `checkpoint_fullfsync=ON`, disabled automatic
WAL checkpoints and disabled mmap. The harness checks the durability PRAGMA values.
Both acknowledge synchronized commits, subject to the filesystem/device honoring
them. Their log formats, sync counts and automatic maintenance rules differ.
CoreSQL can additionally trigger its own log-size checkpoint. SQLite explicit
maintenance uses a checked TRUNCATE WAL checkpoint, not VACUUM.

Default scenarios:

| Scenario | Rows | Payload bytes/row | CoreSQL clean cache | SQLite page cache |
| --- | ---: | ---: | ---: | ---: |
| memory-small | 10,000 | 128 | Resident | 64 MiB target |
| memory-large | 100,000 | 1,024 | Resident | 64 MiB target |
| durable-small | 10,000 | 128 | Resident | 64 MiB target |
| durable-cache-pressure | 100,000 | 1,024 | 4 MiB target | 4 MiB target |

The large fixture has about 98 MiB of payload. Cache-pressure testing exceeds the
configured cache, **not necessarily physical RAM**. CoreSQL's decoded cache excludes
indexes, dirty chunks, metadata and query buffers. SQLite caches encoded pages;
equal numeric cache settings do not imply equal total memory. OS filesystem caches
are not flushed. In-memory SQLite cannot evict its database to a durable file.

CSV reports current RSS and process peak RSS outside each timed sample. Peak RSS
includes setup, verification and all earlier phases; current RSS is not exact live
allocation accounting. File bytes sum regular files in the run's database directory
and exclude unlinked scratch files and transient checkpoint peaks. CoreSQL I/O
counters are cumulative session counters, reset on reopen; bytes written are bytes
submitted to file writes, not physical device traffic. Counter differences between
first/last phase samples exclude the first operation.

`*_execute`, `*_commit` and total transaction samples overlap: do not sum all three.
Throughput is work units divided by summed timed duration, excluding checking and
measurement gaps, **not end-to-end wall-clock throughput**. A read unit is one query;
write units are affected/attempted rows; mixed units are ten SQL statements. Compare like phases.
The report uses nearest-rank p50/p95/p99 and retains each process separately. It
omits p95 below 20 samples and p99 below 100. Even 1,000 observations provide only
a preliminary tail estimate. Reopen and checkpoint maxima must not be presented
as established worst-case latency bounds.

## Profiling

The executable can isolate a phase after identical setup:

```sh
CORESQL_APPLICATION_PROFILE=1 ./build/compare/coresql_application_benchmark \
  coresql memory 100000 1024 100000 0 /tmp point_read
```

Wait for `PHASE_READY point_read` on stderr before attaching a CPU profiler.
Supported phase names appear in `main.cpp`; `all` runs the full sequence. The
environment switch suppresses CSV/RSS/file-stat reporting for profiling. Correctness
checks and parameter generation remain, so attribute engine stack frames separately
from harness costs. Profile separately from timing, and retain traces privately.
Stopping a profiling process forcibly may leave its uniquely named scratch directory;
remove that directory only after the process has exited.

### Background checkpoint comparison

Pass `--background-checkpoints --phase mixed` to `run.py` to use background
maintenance in both engines. The binary accepts the trailing `background` flag.
Requests occur every 50 mixed transactions. One job runs at a time, overlapping
requests are coalesced, and every future is observed for errors. CoreSQL enables
concurrent-safe callbacks; SQLite uses a separate checkpoint connection with
FULL/fullfsync/checkpoint_fullfsync and TRUNCATE checkpoints. SQLite durable write
transactions use BEGIN IMMEDIATE in both modes so checkpoint contention is
included in latency rather than failing a deferred read-to-write upgrade.

`checkpoint_request` measures foreground request/poll overhead, not completed
maintenance. `maintenance_drain` reports the final wait for outstanding work;
`write_workload_wall` includes verification, measurement gaps and that drain.
The stderr record reports requested/started/completed/coalesced jobs and SQLite
busy retries. CoreSQL maintenance counters report encoding, catch-up, publication,
lock waits and reuse; durations include nested I/O and must not be added together.
File-size observations are sampled and can miss transient peaks; private page
backing is not included in that file-size column. Compare foreground and background
modes separately and account for differing completed checkpoint counts.

`--adaptive-checkpoints` (binary flag `adaptive`) instead checks every 50 mixed
transactions for log growth above half the initial compacted file size (minimum
1 MiB), or nonzero growth after one second. This is an explicit benchmark policy,
not a new automatic engine default. It uses the same one-job/coalescing mechanism.
CoreSQL's hard retained-history bound remains active and its automatic checkpoint
counter reveals foreground fallbacks. Changing data size can require a different
application policy; initial compacted size is only a fixed-fixture estimate.
